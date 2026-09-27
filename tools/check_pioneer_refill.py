#!/usr/bin/env python3
"""
tools/check_pioneer_refill.py -- mp:D37a: a pioneer RE-landing in a lockstep match refills the
starting stock on NEITHER peer (retail: on every non-owner peer).

llm_strat_bldg_completion_dispatch (0x004795dd)'s mothership arm credits Building.capacity[1..9] +
human_transport when `mother_established == 0 || player != PlayerSide`. PlayerSide is per-peer, so
on a re-landing (after a lift-off) the idle client refilled the host's pioneer and the host did not
(rc4 Last Question match). The fix -- the mh.dll seam seams/sim_pioneer_refill.cpp in configuration
(1), libmh's sim_bldg_completion_dispatch body in configuration (2) -- makes the lockstep re-landing
refill AI players only. Two arms, one checker (the check_build_probe.py shape):

  * ship arm (`--expect identical`; d37a_relanding_c1 / _cargo_c1 / d37a_relanding_c2):
      clause 1  the host's mh_net.log carries the `; D37a: pioneer re-landing refill gated at ...`
                banner (a KEPT / NOT patched banner is a FAIL: this arm is about the fix);
      clause 2  the CLIENT logged `; D37a: pioneer re-landing player P (local L) ... -> no refill
                (retail: refill)` with P != L -- the non-owner branch the bug lives in really ran and
                the fix withheld the refill -- and the HOST logged its own re-landing line;
      clause 3  both peers wrote harness rows and the overlap covers a live match;
      clause 4  the state hash is IDENTICAL on every common step;
      clause 6  the in-band desync watch logged no `*** DESYNC` on either peer.
  * reproduction arm (`--expect diverge`; `[net] pioneer_refill_fix=0`):
      clause 1  the banner says KEPT;
      clause 2  no re-landing line (the retail compare ran);
      clause 4  the state hash DIVERGES and is still diverged at the last common step;
      clause 5  p0_ai_econ (resource_spent) or player_resources is among the regions differing at
                the first step with per-region rows;
      clause 6  the in-band desync watch logged `*** DESYNC`.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_pioneer_refill.py --expect identical|diverge <host-run-dir> <client-run-dir>
  python tools/check_pioneer_refill.py --selftest        planted logs; every negative RED
"""

import argparse
import json
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_build_probe  # noqa: E402  (first_region_diff)
import check_cancel_task  # noqa: E402  (net_log_lines: the session dir + its process dir)
import mp_analyze  # noqa: E402

ARMED = "D37a: pioneer re-landing refill gated at"
KEPT = "D37a: pioneer re-landing refill KEPT"
BANNER = "D37a: pioneer re-landing refill"
LINE_RE = re.compile(
    r"D37a: pioneer re-landing player (\d+) \(local (\d+)\) status 0x([0-9A-Fa-f]+) -> (refill \(AI\)|no refill)"
    r" \(retail: (refill|no refill)\)"
)
DESYNC = "*** DESYNC step="
REGIONS_OK = re.compile(r"^(p0_ai_econ|player_resources)$")


def banner_of(lines):
    b = [ln for ln in lines if BANNER in ln]
    return b[-1] if b else None


def relandings(lines):
    return [m.groups() for m in (LINE_RE.search(ln) for ln in lines) if m]


def check(host_dir, client_dir, expect, min_overlap=1000):
    fails = []
    host_lines = check_cancel_task.net_log_lines(host_dir)
    client_lines = check_cancel_task.net_log_lines(client_dir)
    banner = banner_of(host_lines)
    host_rl = relandings(host_lines)
    client_rl = relandings(client_lines)
    desync = [ln for ln in host_lines + client_lines if DESYNC in ln]

    if banner is None:
        fails.append(
            "clause 1: the host's mh_net.log has no `; %s ...` banner -- the D37a seam did not run"
            % BANNER
        )
    elif expect == "identical" and ARMED not in banner:
        fails.append("clause 1: the banner is not ARMED: %s" % banner.strip())
    elif expect == "diverge" and KEPT not in banner:
        fails.append(
            "clause 1: the banner does not say KEPT -- the retail compare was NOT the arm under test: %s"
            % banner.strip()
        )

    if expect == "identical":
        nonowner = [g for g in client_rl if g[0] != g[1] and g[4] == "refill"]
        if not nonowner:
            fails.append(
                "clause 2: the client logged no non-owner re-landing line with `retail: refill` -- the pioneer never re-landed, or the bug's branch was not exercised (%d client line(s))"
                % len(client_rl)
            )
        elif any(g[3] != "no refill" for g in nonowner):
            fails.append(
                "clause 2: a non-owner re-landing line says the refill ran: %s" % (nonowner,)
            )
        if not host_rl:
            fails.append("clause 2: the host logged no re-landing line of its own")
        elif any(g[3] != "no refill" for g in host_rl):
            fails.append("clause 2: the host refilled its own human pioneer: %s" % (host_rl,))
    elif host_rl or client_rl:
        fails.append(
            "clause 2: re-landing line(s) on the reproduction arm -- the fix was live after all"
        )

    if expect == "identical" and desync:
        fails.append("clause 6: the in-band desync watch fired: %s" % desync[0].strip())
    if expect == "diverge" and not desync:
        fails.append(
            "clause 6: no `%s` line from the in-band watch -- the player-visible symptom did not reproduce"
            % DESYNC
        )

    a = mp_analyze.load_peer(host_dir)
    b = mp_analyze.load_peer(client_dir)
    if "harness" not in a or "harness" not in b:
        fails.append(
            "clause 3: a peer has no harness log (host=%s client=%s)"
            % ("harness" in a, "harness" in b)
        )
        return fails, "no harness"
    d = mp_analyze.diff_peers(a["harness"], b["harness"], "host", "client")
    ov = d.get("overlap", 0)
    if ov < min_overlap:
        fails.append(
            "clause 3: overlap %d steps < %d -- the walk did not reach a live match"
            % (ov, min_overlap)
        )
    n = d.get("mismatch_count", 0)
    fm = d.get("first_mismatch") or {}
    rstep, regs = check_build_probe.first_region_diff(a["harness"], b["harness"])
    if expect == "identical":
        if n:
            fails.append(
                "clause 4: state hash differs on %d step(s), first at %s" % (n, fm.get("step"))
            )
    else:
        if n < 1:
            fails.append(
                "clause 4: state hash IDENTICAL on every step -- the retail refill no longer diverges (or the pioneer never re-landed)"
            )
        else:
            sa, sb = a["harness"]["steps"], b["harness"]["steps"]
            common = sorted(set(sa) & set(sb))
            if common and sa[common[-1]]["state"] == sb[common[-1]]["state"]:
                fails.append(
                    "clause 4: the last common step %d is identical again -- the refill cannot heal"
                    % common[-1]
                )
            if regs is None:
                fails.append(
                    "clause 5: no differing step carries per-region rows on both peers -- the row needs region_hash_step"
                )
            elif not any(REGIONS_OK.match(r) for r in regs):
                fails.append(
                    "clause 5: neither p0_ai_econ nor player_resources among the regions differing at step %s: %s"
                    % (rstep, ", ".join(regs))
                )
    summary = (
        "overlap=%d mismatches=%d first=%s region_step=%s regions=%s host_lines=%d client_lines=%d desync_lines=%d"
        % (
            ov,
            n,
            fm.get("step"),
            rstep,
            ",".join(regs or []),
            len(host_rl),
            len(client_rl),
            len(desync),
        )
    )
    return fails, summary


# ---- selftest ------------------------------------------------------------------------------------

REGION_COUNT = len(mp_analyze.REGION_NAMES)
ECON_IDX = mp_analyze.REGION_NAMES.index("p0_ai_econ")


def plant(root, name, steps, diverge_from, banner, lines, desync, region_idx=None, heal_at=None):
    proc = os.path.join(root, name, "logs", "20260927T000000Z_menu_solo")
    sess = os.path.join(root, name, "logs", "20260927T000001Z_deadbeef_0_solo")
    os.makedirs(proc)
    os.makedirs(sess)
    ridx = ECON_IDX if region_idx is None else region_idx
    with open(os.path.join(proc, "mh_harness.log"), "w", encoding="utf-8") as fh:
        fh.write(
            "; ==== mh replay harness armed: seed_step=0 seed_mode=2 stop_step=0 fixed_step=0 pin_fpu=1 region_hash_step=50 order_mode=0 replay_ai_off=0 suppress_enqueue=0 ====\n"
        )
        for s in range(1, steps + 1):
            dv = diverge_from is not None and s >= diverge_from and (heal_at is None or s < heal_at)
            fh.write(
                "%d %016X %016X %016X\n"
                % (s, 0x3F80 + s, 0xB000 + s, 0xA000 + s + (1 if dv else 0))
            )
            if s % 50 == 0:
                regs = [
                    "%016X" % (0xC000 + s * 7 + i + (1 if (dv and i == ridx) else 0))
                    for i in range(REGION_COUNT)
                ]
                fh.write("R %d %s\n" % (s, " ".join(regs)))
    with open(os.path.join(proc, "mh_net.log"), "w", encoding="utf-8") as fh:
        if banner == "armed":
            fh.write(
                "; %s 004798F1 -- in a lockstep match a re-landing refills AI players only\n"
                % ARMED
            )
        elif banner == "kept":
            fh.write("; %s ([net] pioneer_refill_fix=0): ... -- the reproduction arm\n" % KEPT)
    with open(os.path.join(sess, "mh_net.log"), "w", encoding="utf-8") as fh:
        for ln in lines:
            fh.write("; D37a: pioneer re-landing %s\n" % ln)
        if desync:
            fh.write(
                "; [desync] %s%d peer=1 mine=0000000000000001 theirs=0000000000000002 first_region=8 p0_ai_econ (mismatch #1)\n"
                % (DESYNC, diverge_from or 1)
            )
    with open(os.path.join(sess, "session.json"), "w", encoding="utf-8") as fh:
        json.dump({"match_id": "deadbeef", "process_dir": "20260927T000000Z_menu_solo"}, fh)
    open(os.path.join(sess, "mh_lockstep.log"), "w").close()
    return sess


HOST_LINE = "player 0 (local 0) status 0x7 -> no refill (retail: no refill)"
CLIENT_LINE = "player 0 (local 1) status 0x7 -> no refill (retail: refill)"
CLIENT_BUG = "player 0 (local 1) status 0x7 -> refill (AI) (retail: refill)"


def selftest():
    ship_h = dict(diverge_from=None, banner="armed", lines=[HOST_LINE], desync=False)
    ship_c = dict(diverge_from=None, banner="armed", lines=[CLIENT_LINE], desync=False)
    rep_h = dict(diverge_from=3300, banner="kept", lines=[], desync=True)
    rep_c = dict(diverge_from=None, banner="kept", lines=[], desync=False)
    cases = [
        ("ship arm: armed, both lines, identical, watch clean", "identical", ship_h, ship_c, True),
        (
            "ship arm NEG: never re-landed (no lines)",
            "identical",
            dict(ship_h, lines=[]),
            dict(ship_c, lines=[]),
            False,
        ),
        (
            "ship arm NEG: client refilled",
            "identical",
            ship_h,
            dict(ship_c, lines=[CLIENT_BUG]),
            False,
        ),
        (
            "ship arm NEG: still diverges",
            "identical",
            dict(ship_h, diverge_from=3300),
            ship_c,
            False,
        ),
        ("ship arm NEG: watch fired", "identical", dict(ship_h, desync=True), ship_c, False),
        ("ship arm NEG: banner KEPT", "identical", dict(ship_h, banner="kept"), ship_c, False),
        ("ship arm NEG: no banner", "identical", dict(ship_h, banner=None), ship_c, False),
        (
            "repro arm: kept, diverges in p0_ai_econ and stays, watch fired",
            "diverge",
            rep_h,
            rep_c,
            True,
        ),
        ("repro arm NEG: identical", "diverge", dict(rep_h, diverge_from=None), rep_c, False),
        (
            "repro arm NEG: wrong region",
            "diverge",
            dict(rep_h, region_idx=(ECON_IDX + 1) % REGION_COUNT),
            rep_c,
            False,
        ),
        ("repro arm NEG: healed", "diverge", dict(rep_h, heal_at=3900), rep_c, False),
        (
            "repro arm NEG: fix live (lines)",
            "diverge",
            dict(rep_h, banner="armed", lines=[HOST_LINE]),
            rep_c,
            False,
        ),
        ("repro arm NEG: watch never fired", "diverge", dict(rep_h, desync=False), rep_c, False),
    ]
    bad = 0
    for label, expect, hk, ck, want in cases:
        root = tempfile.mkdtemp(prefix="d37achk_")
        try:
            h = plant(root, "host", 4500, **hk)
            c = plant(root, "client", 4500, **ck)
            fails, _ = check(h, c, expect)
            got = not fails
            ok = got == want
            print(
                "  %s  %s%s"
                % ("ok " if ok else "BAD", label, "" if got else "  -> " + "; ".join(fails))
            )
            bad += 0 if ok else 1
        finally:
            shutil.rmtree(root, ignore_errors=True)
    print("check_pioneer_refill selftest: %d/%d" % (len(cases) - bad, len(cases)))
    return 0 if bad == 0 else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--expect", choices=("identical", "diverge"), default="identical")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("dirs", nargs="*")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if len(args.dirs) != 2:
        print(
            "usage: check_pioneer_refill.py --expect identical|diverge <host-run-dir> <client-run-dir>"
        )
        return 2
    fails, summary = check(args.dirs[0], args.dirs[1], args.expect)
    if fails:
        print("check_pioneer_refill: FAIL (%s)" % summary)
        for f in fails:
            print("  " + f)
        return 1
    print("check_pioneer_refill: PASS -- %s arm (%s)" % (args.expect, summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
