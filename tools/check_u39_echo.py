#!/usr/bin/env python3
"""
tools/check_u39_echo.py -- mp:U39: the NEGATIVE ARM of the diplomacy-echo NOP.

`u39_diplomacy` (tools/test_ui.py) proves the fix: with the 22-byte echo NOPed, a host that ticks
"allied" for the client in the ESC -> Diplomacy dialog leaves mp_analyze at ALL PAIRS IDENTICAL.
`u39_diplomacy_echo` runs the SAME walk with `[net] diplo_echo_nop=0` (retail bytes kept) and this
checker asserts the divergence the fix exists for is STILL THERE -- so the green row cannot be green
for the wrong reason (a walk that never reaches the dialog, a dialog that never applies, a hash that
stopped covering Players[]). Measured 2026-09-21 on the rig before the fix, three runs: the state
hash differs for EXACTLY 4 consecutive steps (the local write -> the 0xf4 commit) and `players`
(HASH_REGIONS[55]) is among the diverging regions at the first differing step; the rest re-converge.

What passes, in order (any miss is a FAIL naming the clause):
  1. both peers carry a harness log and overlap on >= 1000 steps (the walk reached the match);
  2. the host's mh_net.log carries the `; U39: diplomacy relation echo KEPT` line (the arm really
     ran the retail bytes -- a run that NOPed after all would pass clause 3 by accident, never);
  3. the pair's state hash MISMATCHES on at least 1 and at most --max-steps (default 16) steps --
     more than that is a real desync, not the 4-step echo window;
  4. `players` is in the first mismatch's per-region set (region_hash_step=1 in the registry row);
  5. the last common step's state hash is IDENTICAL again (the echo self-heals at the commit).

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_u39_echo.py [--max-steps N] <host-run-dir> <client-run-dir>
  python tools/check_u39_echo.py --selftest        planted harness logs; every negative RED

`<run-dir>` is what test_ui.py's `post_check` machinery (with `post_check_peers`) hands a checker for
every peer: that lane's newest run directory. mp_analyze.discover() resolves the harness log the
way the analyzer itself does (the PROCESS directory's mh_harness.log for a session directory).
"""

import argparse
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mp_analyze  # noqa: E402

KEPT_LINE = "U39: diplomacy relation echo KEPT"


def net_log_lines(run_dir):
    """mh_net.log lines of the run dir AND its process dir (the seam banner is written at DllMain,
    before any session directory exists -- the check_data_timeout.py belt)."""
    out = []
    cands = [os.path.join(run_dir, "mh_net.log")]
    sj = os.path.join(run_dir, "session.json")
    if os.path.isfile(sj):
        try:
            import json

            pd = json.load(open(sj, encoding="utf-8")).get("process_dir")
            if pd:
                cands.append(os.path.join(os.path.dirname(run_dir.rstrip("\\/")), pd, "mh_net.log"))
                cands.append(os.path.join(pd, "mh_net.log"))
        except Exception:
            pass
    # the lane's other process dirs, newest first
    parent = os.path.dirname(run_dir.rstrip("\\/"))
    if os.path.isdir(parent):
        for d in sorted(os.listdir(parent), reverse=True):
            p = os.path.join(parent, d, "mh_net.log")
            if p not in cands:
                cands.append(p)
    for p in cands:
        if os.path.isfile(p):
            try:
                out.extend(open(p, encoding="utf-8", errors="replace").read().splitlines())
            except OSError:
                pass
    return out


def check(host_dir, client_dir, max_steps, min_overlap=1000):
    fails = []
    a = mp_analyze.load_peer(host_dir)
    b = mp_analyze.load_peer(client_dir)
    if "harness" not in a or "harness" not in b:
        return [
            "clause 1: a peer has no harness log (host=%s client=%s)"
            % ("harness" in a, "harness" in b)
        ]
    d = mp_analyze.diff_peers(a["harness"], b["harness"], "host", "client")
    ov = d.get("overlap", 0)
    if ov < min_overlap:
        fails.append(
            "clause 1: overlap %d steps < %d -- the walk did not reach a live match"
            % (ov, min_overlap)
        )
    if not any(KEPT_LINE in ln for ln in net_log_lines(host_dir)):
        fails.append(
            "clause 2: the host's mh_net.log has no `; %s` line -- the retail echo was NOT the arm under test"
            % KEPT_LINE
        )
    n = d.get("mismatch_count", 0)
    if n < 1:
        fails.append(
            "clause 3: state hash IDENTICAL on every step -- the echo no longer diverges (the premise moved, or the dialog never applied)"
        )
    elif n > max_steps:
        fails.append(
            "clause 3: %d mismatching steps > %d -- a real desync, not the echo window"
            % (n, max_steps)
        )
    fm = d.get("first_mismatch") or {}
    regs = fm.get("regions")
    if n >= 1:
        if regs is None:
            fails.append(
                "clause 4: no per-region hashes at the first mismatch (step %s) -- the row needs region_hash_step=1"
                % fm.get("step")
            )
        elif "players" not in regs:
            fails.append(
                "clause 4: `players` not among the diverging regions at step %s: %s"
                % (fm.get("step"), ", ".join(regs))
            )
    sa, sb = a["harness"]["steps"], b["harness"]["steps"]
    common = sorted(set(sa) & set(sb))
    if common and sa[common[-1]]["state"] != sb[common[-1]]["state"]:
        fails.append(
            "clause 5: the last common step %d still differs -- the echo did not re-converge at the commit"
            % common[-1]
        )
    summary = "overlap=%d mismatches=%d first=%s regions=%s" % (
        ov,
        n,
        fm.get("step"),
        ",".join(regs or []),
    )
    return fails, summary


# ---- selftest: planted harness logs -----------------------------------------------------------

REGION_COUNT = len(mp_analyze.REGION_NAMES)
PLAYERS_IDX = (
    mp_analyze.REGION_NAMES.index("players") if "players" in mp_analyze.REGION_NAMES else 55
)


def plant(root, name, steps, diverge, kept=True, players_diverges=True, heal=True):
    """One peer: a process dir with mh_harness.log + mh_net.log and a session dir naming it."""
    proc = os.path.join(root, name, "logs", "20260921T000000Z_menu_solo")
    sess = os.path.join(root, name, "logs", "20260921T000001Z_deadbeef_0_solo")
    os.makedirs(proc)
    os.makedirs(sess)
    with open(os.path.join(proc, "mh_harness.log"), "w", encoding="utf-8") as fh:
        fh.write(
            "; ==== mh replay harness armed: seed_step=0 seed_mode=2 stop_step=0 fixed_step=0 pin_fpu=1 region_hash_step=1 order_mode=0 replay_ai_off=0 suppress_enqueue=0 ====\n"
        )
        for s in range(1, steps + 1):
            dv = s in diverge or (not heal and s >= min(diverge, default=steps + 1))
            state = "%016X" % (0xA000 + s + (1 if dv else 0))
            # <step> <clock> <combined> <state> -- the analyzer's step-line columns
            fh.write("%d %016X %016X %s\n" % (s, 0x3F80 + s, 0xB000 + s, state))
            regs = []
            for i in range(REGION_COUNT):
                v = 0xC000 + s * 7 + i
                if dv and (i == PLAYERS_IDX if players_diverges else i == 0):
                    v += 1
                regs.append("%016X" % v)
            fh.write("R %d %s\n" % (s, " ".join(regs)))
    with open(os.path.join(proc, "mh_net.log"), "w", encoding="utf-8") as fh:
        fh.write(
            "; %s ([net] diplo_echo_nop=0)\n" % KEPT_LINE
            if kept
            else "; U39: diplomacy relation echo NOPed at 004C8070\n"
        )
    import json

    with open(os.path.join(sess, "session.json"), "w", encoding="utf-8") as fh:
        json.dump({"match_id": "deadbeef", "process_dir": "20260921T000000Z_menu_solo"}, fh)
    with open(os.path.join(sess, "mh_lockstep.log"), "w", encoding="utf-8") as fh:
        fh.write("")
    return sess


def selftest():
    cases = [
        ("positive: 4-step players echo, kept, heals", dict(diverge={349, 350, 351, 352}), True),
        ("negative: identical -- no echo", dict(diverge=set()), False),
        ("negative: 40-step desync", dict(diverge=set(range(349, 389))), False),
        (
            "negative: wrong region diverges",
            dict(diverge={349, 350}, players_diverges=False),
            False,
        ),
        ("negative: arm ran the NOP", dict(diverge={349, 350, 351, 352}, kept=False), False),
        ("negative: never re-converges", dict(diverge={349}, heal=False), False),
    ]
    bad = 0
    for label, kw, want in cases:
        root = tempfile.mkdtemp(prefix="u39chk_")
        try:
            h = plant(root, "host", 1500, **kw)
            # the client is the un-diverged reference: same generator, no divergence, and its arm line
            # does not matter (clause 2 reads the host)
            c = plant(root, "client", 1500, diverge=set(), kept=kw.get("kept", True))
            res = check(h, c, 16)
            fails = res[0] if isinstance(res, tuple) else res
            got = not fails
            ok = got == want
            print(
                "  %s  %s%s"
                % ("ok " if ok else "BAD", label, "" if got else "  -> " + "; ".join(fails))
            )
            bad += 0 if ok else 1
        finally:
            shutil.rmtree(root, ignore_errors=True)
    print("check_u39_echo selftest: %d/%d" % (len(cases) - bad, len(cases)))
    return 0 if bad == 0 else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-steps", type=int, default=16)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("dirs", nargs="*")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if len(args.dirs) != 2:
        print("usage: check_u39_echo.py <host-run-dir> <client-run-dir>")
        return 2
    res = check(args.dirs[0], args.dirs[1], args.max_steps)
    fails, summary = (res, "") if not isinstance(res, tuple) else res
    if fails:
        print("check_u39_echo: FAIL (%s)" % summary)
        for f in fails:
            print("  " + f)
        return 1
    print(
        "check_u39_echo: PASS -- the retail echo still diverges `players` for a bounded window and heals (%s)"
        % summary
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
