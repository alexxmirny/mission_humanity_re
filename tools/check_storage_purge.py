#!/usr/bin/env python3
"""
tools/check_storage_purge.py -- mp:D38 row 5: the storage building panel leaves dead docked units to
the sim in a lockstep match.

On every refresh, llm_strat_ui_storage_bldg_panel (0x00417409) calls
llm_strat_storage_purge_dead_docked for the selected storage -- on the selecting peer alone -- while
the sim purges the same rows on every peer once per sub-tick A (5 game-s). A docked unit that dies
while its storage is selected leaves that peer's `unit_storage` ahead of the others until the sim
catches up (and permanently if llm_strat_hangar_recharge_pulse revives the row elsewhere first). The
mh.dll seam (src/mh_dll/mh/seams/ui_storage_panel_purge.cpp) skips the panel's purge in lockstep.
The dead unit is the harness fixture's (`d38_*` knobs, both peers): a barracks, one docked soldier,
killed at the same step on both peers with the game's storage-change notification.
Two arms, one checker (the check_netcrit_pure.py shape):

  * `d38_storage_purge` (seam ON, ship default; `--expect identical`):
      clause 1  the host's mh_net.log carries `; D38: storage-panel purge sim-owned at ...` (ARMED);
      clause 2  a `; D38: storage-panel purge SKIPPED ... N dead docked unit(s)` line with N > 0 -- the
                panel really refreshed on the dead row and the seam left it to the sim (N = 0
                everywhere means the walk never had the barracks selected at the kill, so IDENTICAL
                would prove nothing);
      clause 7  both peers' harness logs carry `; D38FIX step=S KILLED ...` at the SAME step S;
      clause 3  both peers wrote harness rows and the overlap covers a live match;
      clause 4  the state hash is IDENTICAL on every common step;
      clause 6  the in-band desync watch logged no `*** DESYNC` on either peer.
  * `d38_storage_purge_retail` (`[net] storage_panel_purge_fix=0`, the reproduction arm;
    `--expect diverge`):
      clause 1  the banner says KEPT ([net] storage_panel_purge_fix=0);
      clause 2  no SKIPPED line (the retail call ran);
      clause 7  as above;
      clause 4  the state hash DIVERGES, first within 600 steps after the kill (the divergence heals
                when the sim's own purge runs on the other peer -- a blip, by design of the fixture;
                NOT asserted to persist);
      clause 5  `unit_storage` is among the regions differing at the first step with per-region rows;
      clause 6  the in-band desync watch logged `*** DESYNC ... unit_storage`.

---- USAGE ------------------------------------------------------------------------------------

  python tools/check_storage_purge.py --expect identical|diverge <host-run-dir> <client-run-dir>
  python tools/check_storage_purge.py --selftest        planted logs; every negative RED
"""

import argparse
import glob
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

ARMED = "D38: storage-panel purge sim-owned at"
KEPT = "D38: storage-panel purge KEPT ([net] storage_panel_purge_fix=0)"
BANNER = "D38: storage-panel purge"
SKIPPED = "D38: storage-panel purge SKIPPED"
SKIPPED_RE = re.compile(r"(\d+) dead docked unit")
KILLED_RE = re.compile(r"^; D38FIX step=(\d+) KILLED ")
DESYNC = "*** DESYNC step="
REGION = "unit_storage"
REGION_IN_LINE_RE = re.compile(r"\bunit_storage\b")
KILL_WINDOW = 600  # steps after the kill in which the reproduction arm's first mismatch must fall


def banner_of(lines):
    b = [ln for ln in lines if BANNER in ln and SKIPPED not in ln]
    return b[-1] if b else None


def harness_log_lines(run_dir):
    """The harness log lines of the run (session) dir and of the process dir its session.json names."""
    dirs = [run_dir]
    sj = os.path.join(run_dir, "session.json")
    if os.path.isfile(sj):
        try:
            pd = json.load(open(sj, encoding="utf-8")).get("process_dir")
        except Exception:
            pd = None
        if pd:
            dirs.append(os.path.join(os.path.dirname(os.path.abspath(run_dir).rstrip("\\/")), pd))
    out = []
    for d in dirs:
        for fp in sorted(glob.glob(os.path.join(d, "*harness*.log"))):
            try:
                out.extend(open(fp, encoding="utf-8", errors="replace").read().splitlines())
            except OSError:
                pass
    return out


def kill_steps(run_dir):
    return sorted(
        {int(m.group(1)) for m in (KILLED_RE.match(ln) for ln in harness_log_lines(run_dir)) if m}
    )


def check(host_dir, client_dir, expect, min_overlap=1000):
    fails = []
    host_lines = check_cancel_task.net_log_lines(host_dir)
    client_lines = check_cancel_task.net_log_lines(client_dir)
    banner = banner_of(host_lines)
    skipped = [ln for ln in host_lines if SKIPPED in ln]
    dead = [int(m.group(1)) for m in (SKIPPED_RE.search(ln) for ln in skipped) if m]
    desync = [ln for ln in host_lines + client_lines if DESYNC in ln]
    hk, ck = kill_steps(host_dir), kill_steps(client_dir)

    if banner is None:
        fails.append(
            "clause 1: the host's mh_net.log has no `; %s ...` banner -- the D38 seam did not run"
            % BANNER
        )
    elif expect == "identical" and ARMED not in banner:
        fails.append("clause 1: the banner is not ARMED: %s" % banner.strip())
    elif expect == "diverge" and KEPT not in banner:
        fails.append(
            "clause 1: the banner does not say KEPT ([net] storage_panel_purge_fix=0): %s"
            % banner.strip()
        )
    if expect == "identical" and not any(n > 0 for n in dead):
        fails.append(
            "clause 2: no `; %s ... N dead docked unit(s)` line with N > 0 (%d skip line(s)) -- the panel never "
            "refreshed on the dead row" % (SKIPPED, len(skipped))
        )
    if expect == "diverge" and skipped:
        fails.append(
            "clause 2: a SKIPPED line on the reproduction arm -- the seam was armed after all"
        )
    if len(hk) != 1 or hk != ck:
        fails.append(
            "clause 7: the fixture's KILLED step is not one and the same on both peers (host %s, client %s)"
            % (hk, ck)
        )
    kill = hk[0] if len(hk) == 1 else None

    if expect == "identical" and desync:
        fails.append("clause 6: the in-band desync watch fired: %s" % desync[0].strip())
    if expect == "diverge" and not any(REGION_IN_LINE_RE.search(ln) for ln in desync):
        fails.append(
            "clause 6: no `%s ... unit_storage` line from the in-band watch (%d DESYNC line(s)) -- the symptom did "
            "not reproduce" % (DESYNC, len(desync))
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
                "clause 4: state hash differs on %d step(s), first at %s -- the panel still writes hashed state on one peer"
                % (n, fm.get("step"))
            )
    else:
        first = fm.get("step")
        if n < 1:
            fails.append(
                "clause 4: state hash IDENTICAL on every step -- the retail panel purge no longer diverges"
            )
        elif kill is not None and not (kill < (first or 0) <= kill + KILL_WINDOW):
            fails.append(
                "clause 4: the first mismatch (step %s) is not within %d steps after the kill (step %d) -- another cause"
                % (first, KILL_WINDOW, kill)
            )
        if n >= 1:
            if regs is None:
                fails.append(
                    "clause 5: no differing step carries per-region rows on both peers -- the row needs region_hash_step"
                )
            elif REGION not in regs:
                fails.append(
                    "clause 5: `unit_storage` not among the regions differing at step %s: %s"
                    % (rstep, ", ".join(regs))
                )
    summary = (
        "overlap=%d mismatches=%d first=%s kill=%s region_step=%s regions=%s skip_lines=%d dead_max=%d desync_lines=%d"
        % (
            ov,
            n,
            fm.get("step"),
            kill,
            rstep,
            ",".join(regs or []),
            len(skipped),
            max(dead or [0]),
            len(desync),
        )
    )
    return fails, summary


# ---- selftest ------------------------------------------------------------------------------------

REGION_COUNT = len(mp_analyze.REGION_NAMES)
STORE_IDX = mp_analyze.REGION_NAMES.index(REGION) if REGION in mp_analyze.REGION_NAMES else 0


def plant(
    root, name, steps, diverge_from, banner, dead, desync, kill=1000, region_idx=None, heal_at=None
):
    proc = os.path.join(root, name, "logs", "20260927T000000Z_menu_solo")
    sess = os.path.join(root, name, "logs", "20260927T000001Z_deadbeef_0_solo")
    os.makedirs(proc)
    os.makedirs(sess)
    ridx = STORE_IDX if region_idx is None else region_idx
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
            if kill is not None and s == kill:
                fh.write(
                    "; D38FIX step=%d KILLED docked unit 2 of player 0 in storage 1 (building 2), docked_count=1; "
                    "the sim's purge is due in 4989 ms\n" % s
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
                "; %s 0041748A -- in a lockstep match the panel leaves dead docked units to the sim's sub-tick purge\n"
                % ARMED
            )
        elif banner == "kept":
            fh.write(
                "; %s: the selecting peer purges dead docked units ahead of the sim -- the reproduction arm\n"
                % KEPT
            )
    with open(os.path.join(sess, "mh_net.log"), "w", encoding="utf-8") as fh:
        for n in dead or []:
            fh.write(
                "; %s in lockstep: player 0 storage 1, %d dead docked unit(s) left for the sim's purge\n"
                % (SKIPPED, n)
            )
        if desync:
            fh.write(
                "; [desync] %s%d peer=1 mine=0000000000000001 theirs=0000000000000002 first_region=4 %s (mismatch #1)\n"
                % (DESYNC, diverge_from or 1, desync)
            )
    with open(os.path.join(sess, "session.json"), "w", encoding="utf-8") as fh:
        json.dump({"match_id": "deadbeef", "process_dir": "20260927T000000Z_menu_solo"}, fh)
    open(os.path.join(sess, "mh_lockstep.log"), "w").close()
    return sess


def selftest():
    ship = dict(diverge_from=None, banner="armed", dead=[0, 1], desync=None)
    repro = dict(diverge_from=1050, banner="kept", dead=None, desync="unit_storage", heal_at=1400)
    cases = [
        (
            "ship arm: armed, a dead row left to the sim, identical, watch clean",
            "identical",
            ship,
            None,
            True,
        ),
        (
            "ship arm NEG: skipped only live rows (never refreshed on the dead one)",
            "identical",
            dict(ship, dead=[0]),
            None,
            False,
        ),
        ("ship arm NEG: no skip line", "identical", dict(ship, dead=None), None, False),
        ("ship arm NEG: still diverges", "identical", dict(ship, diverge_from=1050), None, False),
        (
            "ship arm NEG: the in-band watch fired",
            "identical",
            dict(ship, desync="unit_storage"),
            None,
            False,
        ),
        ("ship arm NEG: banner says KEPT", "identical", dict(ship, banner="kept"), None, False),
        ("ship arm NEG: no banner at all", "identical", dict(ship, banner=None), None, False),
        ("ship arm NEG: the fixture never killed", "identical", dict(ship, kill=None), None, False),
        ("ship arm NEG: the peers killed on different steps", "identical", ship, 1001, False),
        (
            "repro arm: kept, diverges in unit_storage after the kill, watch fired",
            "diverge",
            repro,
            None,
            True,
        ),
        (
            "repro arm NEG: identical (premise moved)",
            "diverge",
            dict(repro, diverge_from=None),
            None,
            False,
        ),
        (
            "repro arm NEG: wrong region",
            "diverge",
            dict(repro, region_idx=(STORE_IDX + 1) % REGION_COUNT),
            None,
            False,
        ),
        (
            "repro arm NEG: diverged before the kill",
            "diverge",
            dict(repro, diverge_from=800),
            None,
            False,
        ),
        (
            "repro arm NEG: seam armed (banner + skip line)",
            "diverge",
            dict(repro, banner="armed", dead=[1]),
            None,
            False,
        ),
        (
            "repro arm NEG: the in-band watch never fired",
            "diverge",
            dict(repro, desync=None),
            None,
            False,
        ),
    ]
    bad = 0
    for label, expect, kw, client_kill, want in cases:
        root = tempfile.mkdtemp(prefix="d38chk_")
        try:
            h = plant(root, "host", 1500, **kw)
            ck = kw.get("kill", 1000) if client_kill is None else client_kill
            c = plant(
                root,
                "client",
                1500,
                diverge_from=None,
                banner=None,
                dead=None,
                desync=None,
                kill=ck,
            )
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
    print("check_storage_purge selftest: %d/%d" % (len(cases) - bad, len(cases)))
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
            "usage: check_storage_purge.py --expect identical|diverge <host-run-dir> <client-run-dir>"
        )
        return 2
    fails, summary = check(args.dirs[0], args.dirs[1], args.expect)
    if fails:
        print("check_storage_purge: FAIL (%s)" % summary)
        for f in fails:
            print("  " + f)
        return 1
    print("check_storage_purge: PASS -- %s arm (%s)" % (args.expect, summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
