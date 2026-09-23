#!/usr/bin/env python3
"""
tools/check_cancel_task.py -- mp:D28: the building dialog's "cancel task -> Yes" as a replicated order.

Two arms, one checker (the u39_diplomacy / check_u39_echo.py shape):
  * `d28_canceltask` (routing ON, the ship default): the host's mh_net.log must carry the seam's
    `; D28: cancel-task Yes spliced at <site>` banner WITHOUT the `ROUTING OFF` suffix and at least
    one `; D28: cancel-task Yes routed as order (bldg N state 0x.. -> idle 0x....)` line -- the click
    really went through the order pipeline; the pair's state hash must be IDENTICAL on every common
    step (`--expect identical`).
  * `d28_canceltask_local` (`[net] cancel_task_order=0`, the reproduction arm): the banner carries
    `ROUTING OFF`, no `routed as order` line, and the state hash DIVERGES from some step and STAYS
    diverged to the last common step (the other peer's building keeps working; nothing heals it),
    with `buildings` among the diverging regions at the first mismatch (`--expect diverge`,
    region_hash_step=1 in the registry row).

Clauses are named on failure. Absence of the banner is a FAIL in both arms (the seam did not run).

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_cancel_task.py --expect identical|diverge <host-run-dir> <client-run-dir>
  python tools/check_cancel_task.py --selftest        planted logs; every negative RED
"""

import argparse
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mp_analyze  # noqa: E402

SPLICED = "D28: cancel-task Yes spliced at"
ROUTING_OFF = "ROUTING OFF ([net] cancel_task_order=0)"
ROUTED = "cancel-task Yes routed as order"


def net_log_lines(run_dir):
    """mh_net.log lines of the run dir AND of the process dir its session.json names (the seam
    banner is written at DllMain, before any session directory exists)."""
    cands = [os.path.join(run_dir, "mh_net.log")]
    sj = os.path.join(run_dir, "session.json")
    if os.path.isfile(sj):
        try:
            pd = json.load(open(sj, encoding="utf-8")).get("process_dir")
        except Exception:
            pd = None
        if pd:
            parent = os.path.dirname(os.path.abspath(run_dir).rstrip("\\/"))
            cands.append(os.path.join(parent, pd, "mh_net.log"))
    out = []
    for fp in cands:
        if os.path.isfile(fp):
            try:
                out.extend(open(fp, encoding="utf-8", errors="replace").read().splitlines())
            except OSError:
                pass
    return out


def check(host_dir, client_dir, expect, min_overlap=1000):
    fails = []
    lines = net_log_lines(host_dir)
    banner = [ln for ln in lines if SPLICED in ln]
    routed = [ln for ln in lines if ROUTED in ln]
    if not banner:
        fails.append(
            "clause 1: the host's mh_net.log has no `; %s` banner -- the D28 seam did not run"
            % SPLICED
        )
    else:
        off = ROUTING_OFF in banner[-1]
        if expect == "identical" and off:
            fails.append(
                "clause 1: the banner says ROUTING OFF -- this is the reproduction arm, not the ship arm"
            )
        if expect == "diverge" and not off:
            fails.append(
                "clause 1: the banner does not say ROUTING OFF -- the retail local call was NOT the arm under test"
            )
    if expect == "identical" and not routed:
        fails.append(
            "clause 2: no `; D28: %s` line -- the Yes click never reached the order pipeline (dialog not reached, or the walk never clicked Yes)"
            % ROUTED
        )
    if expect == "diverge" and routed:
        fails.append(
            "clause 2: a `routed as order` line on the reproduction arm -- routing was on after all"
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
    regs = fm.get("regions")
    if expect == "identical":
        if n:
            fails.append(
                "clause 4: state hash differs on %d step(s), first at %s (%s) -- the routed cancel did not keep the peers identical"
                % (n, fm.get("step"), ", ".join(regs or []) or "no per-region rows")
            )
    else:
        if n < 1:
            fails.append(
                "clause 4: state hash IDENTICAL on every step -- the local cancel no longer diverges (the premise moved, or the dialog never applied)"
            )
        else:
            sa, sb = a["harness"]["steps"], b["harness"]["steps"]
            common = sorted(set(sa) & set(sb))
            if common and sa[common[-1]]["state"] == sb[common[-1]]["state"]:
                fails.append(
                    "clause 4: the last common step %d is identical again -- the divergence healed, which a local building-state write cannot do"
                    % common[-1]
                )
            if regs is None:
                fails.append(
                    "clause 5: no per-region hashes at the first mismatch (step %s) -- the row needs region_hash_step=1"
                    % fm.get("step")
                )
            elif "buildings" not in regs:
                fails.append(
                    "clause 5: `buildings` not among the diverging regions at step %s: %s"
                    % (fm.get("step"), ", ".join(regs))
                )
    summary = "overlap=%d mismatches=%d first=%s regions=%s routed_lines=%d" % (
        ov,
        n,
        fm.get("step"),
        ",".join(regs or []),
        len(routed),
    )
    return fails, summary


# ---- selftest ------------------------------------------------------------------------------------

REGION_COUNT = len(mp_analyze.REGION_NAMES)
BLDG_IDX = (
    mp_analyze.REGION_NAMES.index("buildings") if "buildings" in mp_analyze.REGION_NAMES else 0
)


def plant(root, name, steps, diverge_from, banner_off, routed, region_idx=None):
    proc = os.path.join(root, name, "logs", "20260922T000000Z_menu_solo")
    sess = os.path.join(root, name, "logs", "20260922T000001Z_deadbeef_0_solo")
    os.makedirs(proc)
    os.makedirs(sess)
    ridx = BLDG_IDX if region_idx is None else region_idx
    with open(os.path.join(proc, "mh_harness.log"), "w", encoding="utf-8") as fh:
        fh.write(
            "; ==== mh replay harness armed: seed_step=0 seed_mode=2 stop_step=0 fixed_step=0 pin_fpu=1 region_hash_step=1 order_mode=0 replay_ai_off=0 suppress_enqueue=0 ====\n"
        )
        for s in range(1, steps + 1):
            dv = diverge_from is not None and s >= diverge_from
            state = "%016X" % (0xA000 + s + (1 if dv else 0))
            fh.write("%d %016X %016X %s\n" % (s, 0x3F80 + s, 0xB000 + s, state))
            regs = [
                "%016X" % (0xC000 + s * 7 + i + (1 if (dv and i == ridx) else 0))
                for i in range(REGION_COUNT)
            ]
            fh.write("R %d %s\n" % (s, " ".join(regs)))
    with open(os.path.join(proc, "mh_net.log"), "w", encoding="utf-8") as fh:
        if banner_off is not None:
            fh.write(
                "; %s 004C7060 -- in a lockstep match the cancel is a replicated building order%s\n"
                % (
                    SPLICED,
                    (" -- " + ROUTING_OFF + ": retail local call, the reproduction arm")
                    if banner_off
                    else "",
                )
            )
        if routed:
            fh.write(
                "; D28: %s (bldg 3 state 0x6D -> idle 0x0067) instead of the local call\n" % ROUTED
            )
    with open(os.path.join(sess, "session.json"), "w", encoding="utf-8") as fh:
        json.dump({"match_id": "deadbeef", "process_dir": "20260922T000000Z_menu_solo"}, fh)
    open(os.path.join(sess, "mh_lockstep.log"), "w").close()
    return sess


def selftest():
    cases = [
        (
            "ship arm: banner on, routed line, identical",
            "identical",
            dict(diverge_from=None, banner_off=False, routed=True),
            True,
        ),
        (
            "ship arm NEG: no routed line (Yes never clicked)",
            "identical",
            dict(diverge_from=None, banner_off=False, routed=False),
            False,
        ),
        (
            "ship arm NEG: still diverges",
            "identical",
            dict(diverge_from=500, banner_off=False, routed=True),
            False,
        ),
        (
            "ship arm NEG: banner says ROUTING OFF",
            "identical",
            dict(diverge_from=None, banner_off=True, routed=False),
            False,
        ),
        (
            "repro arm: banner off, diverges and stays",
            "diverge",
            dict(diverge_from=500, banner_off=True, routed=False),
            True,
        ),
        (
            "repro arm NEG: identical (premise moved)",
            "diverge",
            dict(diverge_from=None, banner_off=True, routed=False),
            False,
        ),
        (
            "repro arm NEG: wrong region",
            "diverge",
            dict(
                diverge_from=500,
                banner_off=True,
                routed=False,
                region_idx=(BLDG_IDX + 1) % REGION_COUNT,
            ),
            False,
        ),
        (
            "repro arm NEG: routing was on",
            "diverge",
            dict(diverge_from=500, banner_off=False, routed=True),
            False,
        ),
        (
            "either arm NEG: no banner at all",
            "identical",
            dict(diverge_from=None, banner_off=None, routed=False),
            False,
        ),
    ]
    bad = 0
    for label, expect, kw, want in cases:
        root = tempfile.mkdtemp(prefix="d28chk_")
        try:
            h = plant(root, "host", 1500, **kw)
            c = plant(root, "client", 1500, diverge_from=None, banner_off=None, routed=False)
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
    print("check_cancel_task selftest: %d/%d" % (len(cases) - bad, len(cases)))
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
            "usage: check_cancel_task.py --expect identical|diverge <host-run-dir> <client-run-dir>"
        )
        return 2
    fails, summary = check(args.dirs[0], args.dirs[1], args.expect)
    if fails:
        print("check_cancel_task: FAIL (%s)" % summary)
        for f in fails:
            print("  " + f)
        return 1
    print("check_cancel_task: PASS -- %s arm (%s)" % (args.expect, summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
