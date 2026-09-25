#!/usr/bin/env python3
"""check_queue_rollups -- mp:U41b's post_check: two matches over one link, two per-match rollups.

Before U41b the inbound-queue counters reset only at the TRANSPORT boundary (net_reset), so a second
match over a live link inherited the first match's high-water and eviction/refusal counts in its
`net: inbound queue rollup (this match)` line. MH_Net_QueueMatchBoundary (called from
mp_session_close) now logs the finished match's rollup and restarts the counters, logging
`net: match boundary -- inbound queue counters restarted`.

WHAT THIS ASSERTS, per peer (each argument is one peer's run directory -- the PROCESS directory,
`<stamp>_menu_<role>`, whose mh_net.log carries the transport's lines):

  1. >= 2 rollup lines, each IMMEDIATELY followed by the boundary line -- i.e. both came from the
     match boundary, not from the 50 s periodic cadence or a transport reset.
  2. mp:U41d -- EACH of those >= 2 boundary rollups is tied to a DIFFERENT `[session] match_id=`
     (the last one logged before the rollup line). A rematch's two matches always get fresh
     match_ids from unrelated session-setup code, so two boundary events sharing one match_id (or
     missing one) means the reader is not looking at two genuinely distinct matches -- log
     misalignment, a duplicate emission, or the wrong file -- and that is refused regardless of
     what anything else says.
  3. mp:U41d -- THE RESET MARKER. The boundary line now also carries an EPOCH (`lane_queue::epoch_`,
     mh_net_queue_policy.h) that is incremented ONLY inside `reset_counters()` -- nowhere else can
     move it -- plus the evicted/refused counters READ BACK OUT immediately after the reset, which
     `reset_counters()` zeroes. So a PASS on this clause requires, for BOTH of the two boundary
     rollups: the epoch value is present, it is STRICTLY GREATER than the previous boundary's epoch
     (0 the first time this checker has seen this peer), and its post-reset evicted/refused both
     read 0. This is what mp:U41d replaces the old magnitude proof with: whether reset_counters()
     actually ran no longer depends on how deep either match happened to queue (lane E's own rig
     evidence: the boundary/match_id lines print regardless of whether the reset call itself was
     reachable -- a marker that only the reset code path can move is what closes that gap).
  4. BACKWARDS COMPATIBILITY. An OLDER boundary line (pre-epoch, e.g. an archived log from before
     2026-09-24) has no epoch fields at all -- `RE_BOUNDARY` still matches it (the epoch group is
     optional), and such a PEER (graded per peer, not per run) falls back to THE OLD PER-PEER RULE
     instead of clause 3: for at least one lane (total / H / M), the second high-water must be
     strictly lower than the first (an inherited high-water can never go down), and a non-zero
     evicted/refused must not repeat unchanged. That per-peer computation is unchanged from before
     U41d and is what the old --selftest cases (still kept below) continue to exercise.

     mp:U41d DROPS the old RUN-LEVEL leniency ("if ANY peer proves it, the run is proven"): since the
     epoch removes the luck dependency, an epoch-carrying peer is expected to prove itself outright,
     so EVERY peer passed to one invocation must end up `proven` (old-rule peers included, still
     graded by their own magnitude comparison) for the overall verdict to read OK. A pre-epoch peer
     that reads merely "inconclusive" on its own no longer gets rescued by a sibling peer's proof the
     way it could before U41d -- see the multi-peer --selftest cases below.

  For NEW-format peers (epoch present), the depth/high-water magnitude comparison from the old rule
  is still computed and printed, but ONLY as an informational note -- it is no longer part of the
  verdict, because the epoch is a stronger, luck-independent proof of the same fact.

The periodic cadence is 50 s (QUEUE_ROLLUP_MS) and each match in the rematch walks is ~15 s, so a
periodic line is not expected; one that appears is ignored unless it is followed by the boundary.
"""

import argparse
import os
import re
import sys
import tempfile

RE_ROLLUP = re.compile(
    r"net: inbound queue rollup \(this match\): depth (\d+) \(H (\d+) / M (\d+)\), high-water (\d+) / "
    r"\d+ \(H (\d+) / \d+, M (\d+) / \d+\), evicted (\d+) superseded horizon\(s\), REFUSED (\d+)"
)
# mp:U41d -- the epoch/post-reset group is OPTIONAL so an older, pre-epoch boundary line (no such
# group in the text at all) still matches; see the module docstring's backwards-compatibility clause.
RE_BOUNDARY = re.compile(
    r"net: match boundary -- inbound queue counters restarted \(link kept; \d+ frame\(s\) still "
    r"queued carry over(?:; epoch (\d+), post-reset evicted (\d+) / refused (\d+))?\)"
)
# mp:U41d -- the session-setup line stamped once per match, well before its rollup; unrelated to
# the queue code, so its value is not perturbed by whatever the queue counters are doing.
RE_MATCH_ID = re.compile(r"\[session\] match_id=([0-9a-f]+)")


def boundary_rollups(text):
    lines = text.splitlines()
    out = []
    cur_mid = None
    for i, ln in enumerate(lines):
        mid = RE_MATCH_ID.search(ln)
        if mid:
            cur_mid = mid.group(1)
            continue
        m = RE_ROLLUP.search(ln)
        if not m:
            continue
        nxt = lines[i + 1] if i + 1 < len(lines) else ""
        bm = RE_BOUNDARY.search(nxt)
        if not bm:
            continue
        g = [int(x) for x in m.groups()]
        epoch, post_ev, post_ref = bm.groups()  # all three or all None (one optional group)
        out.append(
            {
                "high": g[3],
                "high_h": g[4],
                "high_m": g[5],
                "evicted": g[6],
                "refused": g[7],
                "match_id": cur_mid,
                "epoch": int(epoch) if epoch is not None else None,
                "post_ev": int(post_ev) if post_ev is not None else None,
                "post_ref": int(post_ref) if post_ref is not None else None,
            }
        )
    return out


def read_peer(path):
    f = path if os.path.isfile(path) else os.path.join(path, "mh_net.log")
    with open(f, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def check_peer(label, text):
    fails, notes = [], []
    r = boundary_rollups(text)
    if len(r) < 2:
        fails.append(
            "%s: %d match-boundary rollup(s), need >= 2 -- the second match never ended in this "
            "process, so there is nothing to compare" % (label, len(r))
        )
        return fails, notes, False
    a, b = r[0], r[1]
    notes.append(
        "%s: match 1 high-water %d (H %d / M %d) ev %d ref %d; match 2 high-water %d (H %d / M %d) "
        "ev %d ref %d"
        % (
            label,
            a["high"], a["high_h"], a["high_m"], a["evicted"], a["refused"],
            b["high"], b["high_h"], b["high_m"], b["evicted"], b["refused"],
        )
    )  # fmt: skip
    # mp:U41d -- the non-magnitude discriminator: two boundary rollups must be stamped from two
    # DIFFERENT match_id sessions, or this is not proof of anything (see module docstring clause 2).
    mid_a, mid_b = a["match_id"], b["match_id"]
    if not mid_a or not mid_b:
        fails.append(
            "%s: could not find a [session] match_id= line before one or both match-boundary "
            "rollups -- cannot confirm these came from two distinct matches" % label
        )
    elif mid_a == mid_b:
        fails.append(
            "%s: both match-boundary rollups are stamped with the SAME match_id (%s...) -- these "
            "are not two distinct matches" % (label, mid_a[:8])
        )
    else:
        notes.append(
            "%s: match-boundary rollups tied to two distinct match_id sessions (%s / %s) -- "
            "genuinely two different matches, independent of the high-water numbers below"
            % (label, mid_a, mid_b)
        )

    have_epoch = a["epoch"] is not None and b["epoch"] is not None
    lower = [k for k in ("high", "high_h", "high_m") if b[k] < a[k]]

    if have_epoch:
        # mp:U41d -- THE VERDICT for a peer whose logs carry the reset marker: the epoch, not the
        # magnitude comparison, is what PASS/FAIL turns on.
        if not (b["epoch"] > a["epoch"]):
            fails.append(
                "%s: epoch did not advance across the two boundary rollups (%s -> %s, want strictly "
                "greater) -- reset_counters() did not run between them"
                % (label, a["epoch"], b["epoch"])
            )
        else:
            notes.append(
                "%s: epoch advanced %s -> %s across the two boundary rollups -- reset_counters() "
                "proven to have run, independent of luck or traffic"
                % (label, a["epoch"], b["epoch"])
            )
        for tag, e in (("match 1", a), ("match 2", b)):
            if e["post_ev"] != 0 or e["post_ref"] != 0:
                fails.append(
                    "%s: %s's post-reset evicted/refused reads %s/%s, want 0/0 -- the reset did not "
                    "actually zero the counters" % (label, tag, e["post_ev"], e["post_ref"])
                )
        # The old magnitude comparison is still informative (and still worth a note when it
        # happens to hold), but it is no longer part of the verdict for an epoch-carrying peer.
        if lower:
            notes.append(
                "%s: (informational) match 2 is also strictly lower on %s -- consistent with, but not "
                "needed for, the epoch proof above" % (label, ",".join(lower))
            )
        else:
            notes.append(
                "%s: (informational) match 2's high-water is >= match 1's on every lane -- the epoch "
                "above is what proves the reset here, not this" % label
            )
        proven = not fails
    else:
        # BACKWARDS COMPATIBILITY -- an older, pre-epoch log: fall back to the magnitude rule this
        # checker used before mp:U41d (module docstring clause 4).
        if not lower:
            notes.append(
                "%s: inconclusive on its own -- match 2's high-water is >= match 1's on every lane, "
                "which cannot tell a reset from an inherited value (another peer must prove it)"
                % label
            )
        else:
            notes.append(
                "%s: match 2 is strictly lower on %s -- not inherited" % (label, ",".join(lower))
            )
        for k in ("evicted", "refused"):
            if a[k] and b[k] == a[k]:
                fails.append(
                    "%s: match 2's %s equals match 1's non-zero %d -- inherited" % (label, k, a[k])
                )
        proven = bool(lower)
    return fails, notes, proven


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("runs", nargs="*", help="each peer's run directory (or mh_net.log)")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if not args.runs:
        ap.error("give each peer's run directory")
    fails, notes = [], []
    all_proven = True
    for p in args.runs:
        f, n, proven = check_peer(os.path.basename(os.path.normpath(p)) or p, read_peer(p))
        fails += f
        notes += n
        # mp:U41d -- each peer must prove it on its OWN (epoch is not luck-dependent, so there is no
        # more "one peer proves it for everyone" the way the old magnitude rule needed). A peer
        # without epoch data that is merely "inconclusive" (not failing outright) is the one
        # remaining backwards-compat exception -- see the old-rule branch above.
        all_proven = all_proven and proven
    if not fails and not all_proven:
        fails.append(
            "INCONCLUSIVE -- at least one peer's boundary rollups are pre-epoch and its match 2's "
            "high-water is not strictly below match 1's on any lane, so nothing proves a reset there"
        )
    for n in notes:
        print("  %s" % n)
    for f in fails:
        print("  - %s" % f)
    print("check_queue_rollups: %s" % ("FAIL" if fails else "OK"))
    return 1 if fails else 0


def _roll(h, hh, hm, ev=0, ref=0):
    return (
        "net: inbound queue rollup (this match): depth 0 (H 0 / M 0), high-water %d / 4352 (H %d / "
        "4096, M %d / 256), evicted %d superseded horizon(s), REFUSED %d real input(s)\n"
        % (h, hh, hm, ev, ref)
    )


def _bound_old():
    """Pre-U41d boundary line -- no epoch/post-reset group at all."""
    return (
        "net: match boundary -- inbound queue counters restarted (link kept; 0 frame(s) still "
        "queued carry over)\n"
    )


def _bound_new(epoch, post_ev=0, post_ref=0):
    """mp:U41d boundary line -- carries the epoch + the post-reset zero-proof fields."""
    return (
        "net: match boundary -- inbound queue counters restarted (link kept; 0 frame(s) still "
        "queued carry over; epoch %d, post-reset evicted %d / refused %d)\n"
        % (epoch, post_ev, post_ref)
    )


def _sess(mid):
    return "; [session] match_id=%s\n" % mid


def _match_old(mid, h, hh, hm, ev=0, ref=0):
    """One [session] match_id= line + an OLD-format boundary rollup (no epoch)."""
    return _sess(mid) + _roll(h, hh, hm, ev, ref) + _bound_old()


def _match_new(mid, h, hh, hm, epoch, post_ev=0, post_ref=0, ev=0, ref=0):
    """One [session] match_id= line + a NEW-format (mp:U41d) boundary rollup, carrying the epoch."""
    return _sess(mid) + _roll(h, hh, hm, ev, ref) + _bound_new(epoch, post_ev, post_ref)


def selftest():
    # Single-peer cases: (name, text, want_rc). A `text` that is a LIST instead of a str is a
    # MULTI-PEER case -- one file per list entry, all passed to main() together (mirrors how the
    # rig post-check reads every peer's mh_net.log in one call).
    cases = [
        # ---- OLD-FORMAT (pre-epoch) cases -- backwards compatibility (module docstring clause 4) --
        (
            "old-format: two rollups, second lower, distinct match_id -> OK",
            _match_old("aaaa1111", 20, 20, 4) + _match_old("bbbb2222", 5, 3, 5),
            0,
        ),
        ("old-format: only one match ended -> red", _match_old("aaaa1111", 20, 20, 4), 1),
        (
            "old-format: second equals first everywhere (inherited shape) -> red",
            _match_old("aaaa1111", 20, 20, 4) + _match_old("bbbb2222", 20, 20, 4),
            1,
        ),
        (
            "old-format: rollup not followed by boundary (periodic) does not count",
            _match_old("aaaa1111", 20, 20, 4) + _sess("bbbb2222") + _roll(5, 3, 5),
            1,
        ),  # fmt: skip
        (
            "old-format: non-zero evicted carried over -> red",
            _match_old("aaaa1111", 20, 20, 4, ev=7) + _match_old("bbbb2222", 5, 3, 5, ev=7),
            1,
        ),
        (
            "old-format: same match_id both rollups -> red (not proven distinct matches)",
            _match_old("deadbeef", 20, 20, 4) + _match_old("deadbeef", 5, 3, 5),
            1,
        ),
        (
            "old-format: no [session] match_id= line at all -> red",
            _roll(20, 20, 4) + _bound_old() + _roll(5, 3, 5) + _bound_old(),
            1,
        ),  # fmt: skip
        (
            "old-format: two peers, NEITHER shows a decrease -> red (the pre-U41d flaky shape)",
            [
                _match_old("cccc3333", 3, 3, 2) + _match_old("dddd4444", 3, 3, 2),
                _match_old("eeee5555", 5, 3, 4) + _match_old("ffff6666", 62, 60, 5),
            ],
            1,
        ),
        # ---- NEW-FORMAT (mp:U41d epoch) cases -- the verdict is the epoch, not the magnitude -------
        (
            "epoch advances, post-reset zero, distinct match_id -> OK",
            _match_new("aaaa1111", 20, 20, 4, epoch=1) + _match_new("bbbb2222", 3, 3, 2, epoch=2),
            0,
        ),
        (
            "epoch advances even though match 2 queued DEEPER than match 1 -> still OK (epoch is "
            "the proof, not the magnitude)",
            _match_new("aaaa1111", 3, 3, 2, epoch=1) + _match_new("bbbb2222", 20, 20, 4, epoch=2),
            0,
        ),
        (
            "epoch missing on one of the two boundary lines (mixed old/new) -> red (falls back to "
            "the old magnitude rule, which is inconclusive here)",
            _match_new("aaaa1111", 3, 3, 2, epoch=1) + _match_old("bbbb2222", 3, 3, 2),
            1,
        ),
        (
            "epoch present but STUCK (does not advance) -> red -- the boundary/match_id lines print "
            "regardless (lane E's rig evidence), so distinctness alone is not enough",
            _match_new("aaaa1111", 20, 20, 4, epoch=5) + _match_new("bbbb2222", 3, 3, 2, epoch=5),
            1,
        ),
        (
            "epoch present but went BACKWARDS -> red",
            _match_new("aaaa1111", 20, 20, 4, epoch=7) + _match_new("bbbb2222", 3, 3, 2, epoch=6),
            1,
        ),
        (
            "epoch advances but post-reset evicted/refused did not actually read 0 -> red -- the "
            "reset did not fully run even though something bumped the epoch",
            _match_new("aaaa1111", 20, 20, 4, epoch=1, post_ev=7)
            + _match_new("bbbb2222", 3, 3, 2, epoch=2),
            1,
        ),
        (
            "epoch + zero post-reset fields, but SAME match_id twice -> still red (clause 2 is "
            "independent of clause 3)",
            _match_new("deadbeef", 20, 20, 4, epoch=1) + _match_new("deadbeef", 3, 3, 2, epoch=2),
            1,
        ),
        (
            "inherited high-water (match 2 >= match 1 on every lane) but epoch PROVES the reset -> "
            "OK -- exactly the case the old magnitude-only rule could not resolve",
            _match_new("aaaa1111", 3, 3, 2, epoch=1) + _match_new("bbbb2222", 4, 3, 3, epoch=2),
            0,
        ),
        (
            "two peers, one epoch-proven OK and one old-format inconclusive -> overall red (mp:U41d "
            "drops the old 'one peer is enough' leniency -- since the epoch removes the luck "
            "dependency, EVERY peer is expected to prove itself, not just one of several)",
            [
                _match_new("cccc3333", 3, 3, 2, epoch=1) + _match_new("dddd4444", 4, 3, 2, epoch=2),
                _match_old("eeee5555", 5, 3, 4) + _match_old("ffff6666", 62, 60, 5),
            ],
            1,
        ),
    ]
    bad = 0
    with tempfile.TemporaryDirectory() as td:
        for i, (name, text, want) in enumerate(cases):
            texts = text if isinstance(text, list) else [text]
            files = []
            for j, t in enumerate(texts):
                f = os.path.join(td, "c%d_%d.log" % (i, j))
                with open(f, "w", encoding="utf-8") as fh:
                    fh.write(t)
                files.append(f)
            got = main(files)
            ok = got == want
            bad += 0 if ok else 1
            print("  %-4s %s (rc=%d want %d)" % ("ok" if ok else "FAIL", name, got, want))
    print("check_queue_rollups --selftest: %s" % ("PASS" if not bad else "FAIL"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
