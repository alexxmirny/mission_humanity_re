#!/usr/bin/env python3
"""check_map_transfer -- the post_check for mp:X2's rig scenarios (map_absent / map_conflict).

WHAT IT ASSERTS, AND WHY EACH CLAUSE NEEDS BOTH PEERS' LOGS. X2's acceptance is a statement about
two machines that disagreed about a file and then did not: the HOST's log carries the content claim
it advertised and the transfer it armed, and the CLIENT's carries what it held before, what it
stored, and what its own file hashed to afterwards. A checker handed one of them can only ever see
half the claim, which would read as a pass -- so this runs under `post_check_peers`.

THE FOUR ASSERTIONS, in the order the tracker states them:

  1. THE HOST MADE A CONTENT CLAIM.  `; [map] host claim <name> sha=<hex> size=<n>`. Without it
     nothing downstream means anything: a run where the host silently fell back to "no claim" would
     otherwise satisfy every other line here by doing nothing at all, which is the vacuous-green
     shape this suite keeps refusing.

  2. THE CLIENT DID NOT HAVE IT, AND THEN DID.  `; [map] client want ... sha=<H>` with a
     `; [map] client resolve missing` (so the run really was the download case and not a peer that
     happened to hold the map already), followed by `; [map] client stored <file> (<n> B, sha=<H>)`
     with the SAME hash the host claimed. The stored name must also be the content-addressed form --
     `<stem>.<16 hex>.<ext>` -- because storing under the base name is the one failure that would
     look identical in every other line.

  3. THE CLIENT'S OWN FILE IS BYTE-UNCHANGED.  `; [map] local before ...` and `; [map] local after
     ...` bracket the whole session (the first fires when the advert lands, the second on the host's
     FLAG_START) and must agree -- both hash and size, or both "absent". This is the assertion the
     mechanism exists for and the one a human would forget to make.

  4. THE PEERS PLAYED.  Both logs reach the match. Asserted through the scenario's own launch
     assertions rather than re-derived here; what this file adds is that the HOST's Start was HELD
     first: `; [map] start REFUSED` naming the client is expected in the download scenarios, and its
     absence means the gate never closed, i.e. the Start-gate clause was not exercised.

WHAT THESE SCENARIOS DO NOT PROVE, said here rather than left to be assumed. The client's starting
state is staged by `[net] map_test_pretend=none|other`, which changes what that peer REPORTS holding
and whether its resolver considers the base-name candidate -- it moves no file, because a local
lane's `Maps` is a symlink to the one shared game image and the file-moving first version of the
knob took the map away from the host too. So on a local lane the client's base file IS the host's
content, and a broken open-redirect would still open matching bytes and still pass here. The
redirect's own proof is `net_selftest.exe maptest` arms E and W, where the local file is genuinely
different bytes. What these scenarios prove is the INTEGRATION: the claim, the gate, the transfer,
the content-addressed write and a match that plays.

`--expect-nothing` inverts clause 2 for the third scenario: a joiner that already holds the content
must store NOTHING and the host must arm NO transfer, asserted as the absence of `; [map] send armed`
together with the presence of `; [map] peer ... holds the map`, so the absence is read beside a
positive line rather than on its own.
"""

import argparse
import os
import re
import sys

# The needles are the registered ones (tools/data/log_formats.json, the map.* rows). Kept as module
# constants rather than inline so the registry lint can find them as string literals.
RE_HOST_CLAIM = re.compile(r"; \[map\] host claim (.+?) sha=([0-9a-f]{16}) size=(\d+)")
RE_HOST_NOCLAIM = re.compile(r"; \[map\] host noclaim ")
RE_WANT = re.compile(r"; \[map\] client want (.+?) sha=([0-9a-f]{16}) size=(\d+)")
RE_RESOLVE_MISSING = re.compile(r"; \[map\] client resolve missing")
RE_STORED = re.compile(r"; \[map\] client stored (.+?) \((\d+) B, sha=([0-9a-f]{16})\)")
RE_LOCAL = re.compile(
    r"; \[map\] local (before|after) (.+?) (?:sha=([0-9a-f]{16}) size=(\d+)|absent)"
)
RE_SEND_ARMED = re.compile(r"; \[map\] send armed to peer (\d+) '(.+?)' \((\d+) B")
RE_PEER_HOLDS = re.compile(r"; \[map\] peer (\d+) '(.+?)' holds the map")
RE_PEER_NEEDS = re.compile(r"; \[map\] peer (\d+) '(.+?)' needs the map")
RE_START_REFUSED = re.compile(r"; \[map\] start REFUSED -- waiting for '(.+?)'")
# The stored file is `mh_dl\<stem>.<16 hex>.<ext>`: the NAME is the content-addressed form the
# tracker asks for, and the `mh_dl\` prefix says it landed OUTSIDE the map directory. Both halves
# are asserted, and both were wrong once. A copy beside the player's maps would appear in their
# picker -- and the local rig's lanes share one `Maps` by symlink, so it would change what every
# other scenario defaults to. A subdirectory of `Maps\` is worse: llm_mp_mappicker_populate_list
# scans that directory for SUB-FOLDERS before it scans for `*.mpm`, so the download folder became
# the picker's pre-selected first row and a rig host never reached its lobby.
RE_STORED_FORM = re.compile(r"^mh_dl\\(.*)\.([0-9a-f]{16})(\.[^.]*)?$")

FAILS = []
NOTES = []


def fail(msg):
    FAILS.append(msg)


def note(msg):
    NOTES.append(msg)


def read_one(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def read(path):
    """One peer's whole `mh_net.log` history, as a single text.

    THE ARGUMENT IS A RUN DIRECTORY, and one run directory is not enough. `post_check_peers` hands
    each peer's NEWEST run (SES1's per-session log split), and X2's evidence straddles that split by
    construction: the host's content claim is written while it is still on the menu/lobby process
    log, and the peer report, the transfer and the Start gate are written in the match session's.
    A checker reading only the newest directory sees the gate and no claim, and would report "the
    host made no claim" about a host that plainly did. So every run of the same LANE is read, oldest
    first -- the peer is the process, not the session.

    A plain file path is still accepted, which is what makes the checker runnable by hand on a log.
    """
    if not os.path.isdir(path):
        return read_one(path)
    logs_dir = os.path.dirname(os.path.normpath(path))  # <lane>/logs
    runs = []
    for d in (logs_dir, path):
        if os.path.isdir(d):
            for name in os.listdir(d):
                f = os.path.join(d, name, "mh_net.log")
                if os.path.isfile(f):
                    runs.append(f)
            f = os.path.join(d, "mh_net.log")
            if os.path.isfile(f):
                runs.append(f)
    runs = sorted(set(runs), key=lambda f: os.path.getmtime(f))
    return "\n".join(read_one(f) for f in runs)


def classify(paths):
    """Split the given logs into (host_text, client_texts) by what each one claims to be.

    The harness hands the peers' logs in an order this file should not depend on, and "which peer was
    the host" is written in the logs themselves -- the host is the one that made a content claim (or
    armed a send). Deriving it beats trusting an argument position that a future runner may reorder.
    """
    host, clients = None, []
    for p in paths:
        t = read(p)
        if RE_HOST_CLAIM.search(t) or RE_SEND_ARMED.search(t) or RE_PEER_NEEDS.search(t):
            if host is None:
                host = (p, t)
                continue
        clients.append((p, t))
    return host, clients


def peer_label(path):
    """A name for this peer in the report -- the LANE, not the run directory.

    basename() of a run directory is a timestamp, and of a path with a trailing separator it is the
    empty string, which is how the first green run printed `: stored ...` with nothing in front of
    the colon. The lane name is the thing a reader can act on.
    """
    p = os.path.normpath(path)
    parts = p.split(os.sep)
    if "logs" in parts:
        return parts[parts.index("logs") - 1]
    return os.path.basename(p) or p


def check_client(path, text, claim_hash, expect_nothing):
    name = peer_label(path)
    want = RE_WANT.search(text)
    if not want:
        fail(
            "%s: no `; [map] client want` line -- this peer never learned the host's map claim"
            % name
        )
        return
    if want.group(2) != claim_hash:
        fail(
            "%s: the client wants sha=%s but the host claimed sha=%s"
            % (name, want.group(2), claim_hash)
        )

    stored = RE_STORED.search(text)
    if expect_nothing:
        if stored:
            fail(
                "%s: the client STORED a map it should already have had (%s)"
                % (name, stored.group(1))
            )
        else:
            note("%s: stored nothing, as expected" % name)
    else:
        if not RE_RESOLVE_MISSING.search(text):
            fail(
                "%s: the client never reported `resolve missing` -- it already held the content, so "
                "this run did not exercise a download at all" % name
            )
        if not stored:
            fail("%s: the client never stored the host's map" % name)
        else:
            if stored.group(3) != claim_hash:
                fail(
                    "%s: the stored map hashes to %s, not the advertised %s"
                    % (name, stored.group(3), claim_hash)
                )
            m = RE_STORED_FORM.match(stored.group(1))
            if not m or m.group(2) != claim_hash:
                fail(
                    "%s: the stored file '%s' is not `mh_dl\\<stem>.<16 hex>.<ext>` -- a download "
                    "under the base name would have overwritten the player's map, and one beside "
                    "it would have appeared in their map picker" % (name, stored.group(1))
                )
            else:
                note("%s: stored %s (%s B)" % (name, stored.group(1), stored.group(2)))

    # Clause 3: the player's own file, before and after.
    befores = RE_LOCAL.findall(text)
    before = [b for b in befores if b[0] == "before"]
    after = [b for b in befores if b[0] == "after"]
    if not before:
        fail(
            "%s: no `; [map] local before` sample -- the byte-unchanged claim has no baseline"
            % name
        )
    elif not after:
        fail(
            "%s: no `; [map] local after` sample -- the client never reached the host's Start, so "
            "nothing can be said about its own file afterwards" % name
        )
    else:
        b, a = before[0], after[-1]
        if (b[2], b[3]) != (a[2], a[3]):
            fail(
                "%s: THE PLAYER'S OWN %s CHANGED: before sha=%s size=%s, after sha=%s size=%s"
                % (name, b[1], b[2] or "absent", b[3] or "-", a[2] or "absent", a[3] or "-")
            )
        else:
            note(
                "%s: own file unchanged (%s sha=%s size=%s)"
                % (name, b[1], b[2] or "absent", b[3] or "-")
            )


def main(argv=None):
    del FAILS[:], NOTES[:]
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("logs", nargs="*", help="mh_net.log from each peer (host and client(s))")
    ap.add_argument(
        "--expect-nothing",
        action="store_true",
        help="the joiner already holds the content: assert NO transfer was armed and "
        "nothing was stored (read beside the positive `holds the map` line)",
    )
    ap.add_argument(
        "--selftest",
        action="store_true",
        help="plant a log pair per outcome and assert this reader's verdict on each",
    )
    ap.add_argument(
        "--expect-gate",
        action="store_true",
        help="assert the host's Start was HELD at least once (the refusal names the peer)",
    )
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()

    host, clients = classify(args.logs)
    if host is None:
        fail(
            "none of the given logs is a host log (no `; [map] host claim`, `send armed` or `peer "
            "... needs the map` line in any of them)"
        )
        print("check_map_transfer: FAIL")
        for f in FAILS:
            print("  - %s" % f)
        return 1
    hpath, htext = host

    claim = RE_HOST_CLAIM.search(htext)
    if not claim:
        fail(
            "%s: the host made NO content claim (%s) -- every other assertion here would pass "
            "vacuously"
            % (
                peer_label(hpath),
                "a `host noclaim` line is present"
                if RE_HOST_NOCLAIM.search(htext)
                else "not even a noclaim line",
            )
        )
        print("check_map_transfer: FAIL")
        for f in FAILS:
            print("  - %s" % f)
        return 1
    claim_hash = claim.group(2)
    note("host claimed %s sha=%s size=%s" % (claim.group(1), claim_hash, claim.group(3)))

    armed = RE_SEND_ARMED.search(htext)
    if args.expect_nothing:
        if armed:
            fail(
                "%s: the host ARMED a transfer to a peer that already held the map (%s)"
                % (peer_label(hpath), armed.group(0).strip())
            )
        if not RE_PEER_HOLDS.search(htext):
            fail(
                "%s: the host never logged `peer ... holds the map` -- so 'nothing was transferred' "
                "cannot be told apart from 'no peer ever joined'" % peer_label(hpath)
            )
        else:
            note("host: the peer held the map; no transfer armed")
    else:
        if not armed:
            fail("%s: the host never armed a transfer" % peer_label(hpath))
        else:
            note(
                "host armed a %s B transfer to peer %s '%s'"
                % (armed.group(3), armed.group(1), armed.group(2))
            )

    if args.expect_gate:
        held = RE_START_REFUSED.search(htext)
        if not held:
            fail(
                "%s: the Start gate NEVER CLOSED -- no `; [map] start REFUSED` line, so this run "
                "did not exercise the host refusing to Start" % peer_label(hpath)
            )
        else:
            note("host held Start for '%s'" % held.group(1))

    if not clients:
        fail("no client log was given -- the receiver-side half of every clause is unread")
    for p, t in clients:
        check_client(p, t, claim_hash, args.expect_nothing)

    for n in NOTES:
        print("  %s" % n)
    if FAILS:
        print("check_map_transfer: FAIL")
        for f in FAILS:
            print("  - %s" % f)
        return 1
    print("check_map_transfer: OK")
    return 0


# ---- the reader's own negative cases -------------------------------------------------------------
#
# A POST-CHECK IS AN ORACLE, AND AN ORACLE THAT CANNOT GO RED IS DECORATION. Every clause above
# exists to catch a specific wrong run, so each one is driven here against a planted log pair that
# is wrong in exactly that way -- and against the right pair, which must stay green. The planted
# logs are real files in a temp directory laid out the way a lane is (`<lane>/logs/<run>/mh_net.log`),
# because the directory walk that stitches a peer's menu log to its session log is itself one of the
# things that was wrong once and read as "the host made no claim".

HASH_OK = "d3c5707f54d7ecfb"
HASH_NO = "49475958b6537b6e"

HOST_MENU = "; [map] host claim blue monday.mpm sha=%s size=462065\n" % HASH_OK
HOST_RUN = (
    "; [map] peer 1 'client' needs the map (has=none want=%s)\n"
    "; [map] send armed to peer 1 'client' (462065 B of blue monday.mpm)\n"
    "; [map] start REFUSED -- waiting for 'client' to finish downloading the map\n"
    "; [map] peer 1 'client' holds the map (sha=%s) -- nothing to transfer\n"
    "; [map] start OK -- every joiner reports the map we advertised\n" % (HASH_OK, HASH_OK)
)
CL_MENU = (
    "; [map] client want blue monday.mpm sha=%s size=462065\n"
    "; [map] local before blue monday.mpm sha=%s size=462065\n"
    "; [map] client resolve missing -- waiting for the host's copy over channel C\n"
    % (HASH_OK, HASH_OK)
)
CL_RUN = (
    "; [map] client stored mh_dl\\blue monday.%s.mpm (462065 B, sha=%s) -- the local blue "
    "monday.mpm is untouched\n"
    "; [map] local after blue monday.mpm sha=%s size=462065\n" % (HASH_OK, HASH_OK, HASH_OK)
)


def _plant(root, lane, runs):
    """Lay a peer out the way a lane is: <lane>/logs/<run>/mh_net.log, one file per run."""
    out = None
    for i, text in enumerate(runs):
        d = os.path.join(root, lane, "logs", "run%d" % i)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "mh_net.log"), "w", encoding="utf-8") as fh:
            fh.write(text)
        out = d
    return out  # the NEWEST run, which is what post_check_peers hands over


def selftest():
    import shutil
    import tempfile

    cases = [
        # (name, host runs, client runs, argv flags, expected rc)
        (
            "the honest download run is GREEN",
            [HOST_MENU, HOST_RUN],
            [CL_MENU, CL_RUN],
            ["--expect-gate"],
            0,
        ),
        (
            "a host that made NO CLAIM is red (every other clause would pass vacuously)",
            ["; [map] host noclaim -- the packs answer for this name\n", HOST_RUN],
            [CL_MENU, CL_RUN],
            ["--expect-gate"],
            1,
        ),
        (
            "a client that stored NOTHING is red",
            [HOST_MENU, HOST_RUN],
            [CL_MENU, ""],
            ["--expect-gate"],
            1,
        ),
        (
            "a download stored under the BASE NAME is red -- the one failure that looks like success",
            [HOST_MENU, HOST_RUN],
            [CL_MENU, CL_RUN.replace("mh_dl\\blue monday.%s.mpm" % HASH_OK, "blue monday.mpm")],
            ["--expect-gate"],
            1,
        ),
        (
            "a download stored BESIDE the player's maps is red (the picker would list it)",
            [HOST_MENU, HOST_RUN],
            [CL_MENU, CL_RUN.replace("mh_dl\\blue", "Maps\\blue")],
            ["--expect-gate"],
            1,
        ),
        (
            "THE PLAYER'S OWN FILE CHANGED is red",
            [HOST_MENU, HOST_RUN],
            [
                CL_MENU,
                CL_RUN.replace(
                    "local after blue monday.mpm sha=%s" % HASH_OK,
                    "local after blue monday.mpm sha=%s" % HASH_NO,
                ),
            ],
            ["--expect-gate"],
            1,
        ),
        (
            "a stored map that does not hash to the advert is red",
            [HOST_MENU, HOST_RUN],
            [CL_MENU, CL_RUN.replace("sha=%s)" % HASH_OK, "sha=%s)" % HASH_NO)],
            ["--expect-gate"],
            1,
        ),
        (
            "a run where the gate NEVER CLOSED is red under --expect-gate",
            [
                HOST_MENU,
                HOST_RUN.replace(
                    "; [map] start REFUSED -- waiting for 'client' to finish downloading the map\n",
                    "",
                ),
            ],
            [CL_MENU, CL_RUN],
            ["--expect-gate"],
            1,
        ),
        (
            "--expect-nothing: a peer that held the map and transferred nothing is GREEN",
            [
                HOST_MENU,
                "; [map] peer 1 'client' holds the map (sha=%s) -- nothing to transfer\n" % HASH_OK,
            ],
            [
                "; [map] client want blue monday.mpm sha=%s size=462065\n"
                "; [map] local before blue monday.mpm sha=%s size=462065\n"
                "; [map] client resolve base -- the local file already IS the host's content\n"
                % (HASH_OK, HASH_OK),
                "; [map] local after blue monday.mpm sha=%s size=462065\n" % HASH_OK,
            ],
            ["--expect-nothing"],
            0,
        ),
        (
            "--expect-nothing: a transfer that WAS armed is red",
            [HOST_MENU, HOST_RUN],
            [CL_MENU, CL_RUN],
            ["--expect-nothing"],
            1,
        ),
        (
            "--expect-nothing with no `holds the map` line is red -- 'nothing transferred' and "
            "'nobody joined' must be different verdicts",
            [HOST_MENU, ""],
            [
                "; [map] client want blue monday.mpm sha=%s size=462065\n"
                "; [map] local before blue monday.mpm sha=%s size=462065\n" % (HASH_OK, HASH_OK),
                "; [map] local after blue monday.mpm sha=%s size=462065\n" % HASH_OK,
            ],
            ["--expect-nothing"],
            1,
        ),
    ]

    root = tempfile.mkdtemp(prefix="mh_maptransfer_selftest_")
    bad = 0
    try:
        for i, (name, host_runs, cl_runs, flags, want) in enumerate(cases):
            case = os.path.join(root, "c%d" % i)
            h = _plant(case, "host", host_runs)
            c = _plant(case, "client", cl_runs)
            got = main(flags + [h, c])
            ok = got == want
            if not ok:
                bad += 1
            print("  %-4s %s (rc=%s, wanted %s)" % ("ok" if ok else "FAIL", name, got, want))
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print(
        "check_map_transfer --selftest: %s (%d case(s), %d failure(s))"
        % ("PASS" if bad == 0 else "FAIL", len(cases), bad)
    )
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
