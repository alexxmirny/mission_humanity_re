#!/usr/bin/env python3
"""replay_match_segment.py -- mp:SES7b: replay ONE recorded match offline, in a fresh process, and
compare its per-step hashes with the recording's own.

WHAT IT REPLAYS. A match folder from a bug report (or a rig lane) that holds the mp:SES7 match
segment: mh_match_orders.bin + mh_match_clock.bin + mh_match_seed.bin (the replay path's three
inputs on the MATCH's own step axis) and mh_match_harness.log (the recorded per-step hashes, on the
PROCESS axis, with `step_base` on its `; [match] segment OPEN` line). A pre-SES7 report has no
segment; its PROCESS folder (mh_orders.bin / mh_clock.bin / mh_harness_seed.bin / mh_harness.log)
is replayable only when the process's FIRST sim step was this match's first step -- i.e. no earlier
match in that process ran a single step -- because the process seed is dumped at process step 1.
The tool checks that off the session.json files and refuses otherwise, by name.

HOW. A per-tree solo lane (the ui_test `solo=` machinery), the three inputs copied beside mh.exe as
mh_orders.bin / mh_clock.bin / mh_harness_seed.bin, and a generated walk: host a network game on
the recorded MAP (session.json `map`, picked by row in "Available maps"), seat one computer
opponent, Start -- sp_det.txt's walk with the map row added. The harness then runs the replay
contract (the LIB-REF one, tools/fixture_replay.py REPLAY_FLAGS, plus the seed inject):

    seed_step=1 seed_mode=1      the match's step-1 state over every hash region, pre-body
    order_mode=2                 the recorded queue is injected at the top of each step
    replay_suppress_enqueue=1    the replay's own immediate-lane enqueues append nothing
    replay_ai_off=0              the AI runs (its orders are suppressed; the rest of what it does
                                 is sim state the recording does not hold)
    synth_move=0                 no workload: the recording is the only order source
    clock track                  mh_clock.bin pins TOTAL_GAME_TIME per step (the recorded deltas)
    stop_step=N exit_on_stop=1   N = the last recorded step, on the match axis

THE CONFIGURATION FOLLOWS THE RECORDING. The recorder's mh_harness.log says `configuration (1)`
(mh.dll + mh_net.dll + mh_harness.dll, no libmh.dll -- what a player runs) or `(2)` (libmh.dll's
spine present -- the rig). The replay lane gets the same module set: libmh.dll is removed from the
lane for a (1) recording (ui_test refreshes only the satellites a lane already has, so the removal
sticks). `--config 1|2` overrides.

THE VERDICT compares the `state` column (never `combined`: it folds in the wall-clock/pacing regions
state_excluded() drops -- fixture_replay.py's ruling) and the clock column at every step both logs
carry. IDENTICAL, or the FIRST diverging step plus the non-excluded regions that differ there when
both logs carry `R` lines (the rig does; a player's region_hash_step=0 log does not, and then the
first step is all the tool can name). Exit 0 = IDENTICAL over >= --min-steps common steps.

Usage:
    python tools/replay_match_segment.py <match_or_process_dir> [--config auto|1|2] [--steps N]
    python tools/replay_match_segment.py <dir> --compare <replay_run_dir>    # no run, compare only
    python tools/replay_match_segment.py --selftest                          # offline, no rig
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))

SEG = {
    "orders": "mh_match_orders.bin",
    "clock": "mh_match_clock.bin",
    "seed": "mh_match_seed.bin",
    "log": "mh_match_harness.log",
}
PROC = {
    "orders": "mh_orders.bin",
    "clock": "mh_clock.bin",
    "seed": "mh_harness_seed.bin",
    "log": "mh_harness.log",
}
LANE_NAME = {"orders": "mh_orders.bin", "clock": "mh_clock.bin", "seed": "mh_harness_seed.bin"}
ORDERS_MAGIC_LEN = 8  # MHOR v1 header: u32 magic, u32 version
ORDER_REC = 72  # u32 step + the 68-byte llm_strat_order

REPLAY_FLAGS = {
    "seed_step": 1,
    "seed_mode": 1,
    "order_mode": 2,
    "replay_suppress_enqueue": 1,
    "replay_ai_off": 0,
    "synth_move": 0,
    "fixed_step": 0,
    "region_hash_step": 1,
    "exit_on_stop": 1,
    "match_segment": 0,  # the replay's own session must not write a second segment beside it
}

# rows of "Available maps" (mp_host_el2.txt, from a `dump`): widget [1] origin (14,54), row i at
# y = 98 + 14*i, listed in the game's (FindFirstFile, case-folded) order of Maps\*.mpm.
MAP_ROW_X, MAP_ROW_Y0, MAP_ROW_DY = 80, 98, 14

STEP_RE = re.compile(r"^(\d+) ([0-9A-Fa-f]{16}) ([0-9A-Fa-f]{16}) ([0-9A-Fa-f]{16})")
OPEN_RE = re.compile(r"; \[match\] segment OPEN (\S+) at process step (\d+).*?step_base=(\d+)")
CONFIG_RE = re.compile(r"; \[harness\] configuration \((\d)\)")
ARM_RE = re.compile(r"; ==== mh replay harness armed: (.*?) ====")


class Refusal(Exception):
    """A named reason this folder cannot be replayed (not a divergence)."""


# ---- reading a recording ------------------------------------------------------------------------


def _read_text(path):
    with open(path, "r", encoding="latin-1") as fh:
        return fh.read()


def parse_stream(text, base=0):
    """({match_step: (clock, combined, state)}, {match_step: [region hashes]}) from a harness log."""
    steps, regs = {}, {}
    for ln in text.splitlines():
        if ln.startswith("R "):
            p = ln.split()
            if len(p) > 2 and p[1].isdigit():
                regs[int(p[1]) - base] = [x.upper() for x in p[2:]]
            continue
        m = STEP_RE.match(ln)
        if m:
            steps[int(m.group(1)) - base] = tuple(x.upper() for x in m.groups()[1:])
    return steps, regs


def arm_flags(text):
    """The recorder's `; ==== mh replay harness armed: k=v ... ====` banner as a dict (last one)."""
    got = {}
    for m in ARM_RE.finditer(text):
        got = dict(kv.split("=", 1) for kv in m.group(1).split() if "=" in kv)
    return got


def config_of(text):
    m = CONFIG_RE.findall(text)
    return int(m[-1]) if m else None


def _session_json(d):
    try:
        with open(os.path.join(d, "session.json"), encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return None


def sessions_of_process(logs_dir, proc_leaf):
    """[(leaf, session.json)] of the session folders whose process_dir names `proc_leaf`, by time."""
    out = []
    for d in sorted(glob.glob(os.path.join(logs_dir, "*"))):
        j = _session_json(d)
        if j and j.get("process_dir") == proc_leaf:
            out.append((os.path.basename(d), j))
    return out


def resolve(path):
    """The recording in `path` -> dict(kind, files{orders,clock,seed,log}, base, map, config, ...).

    Raises Refusal with the reason when the folder cannot be replayed."""
    path = os.path.abspath(path.rstrip("\\/"))
    if not os.path.isdir(path):
        raise Refusal("no such folder: %s" % path)
    logs_dir = os.path.dirname(path)
    sj = _session_json(path)
    if os.path.isfile(os.path.join(path, SEG["log"])) or os.path.isfile(
        os.path.join(path, SEG["orders"])
    ):
        files = {k: os.path.join(path, v) for k, v in SEG.items()}
        missing = [
            SEG[k] for k in ("orders", "clock", "seed", "log") if not os.path.isfile(files[k])
        ]
        if missing:
            raise Refusal(
                "segment incomplete, missing %s -- the recorder ran without order_mode=1 "
                "(no orders/clock) or with seed_mode!=0 (no seed): nothing to replay from"
                % ", ".join(missing)
            )
        log = _read_text(files["log"])
        m = OPEN_RE.search(log)
        if not m:
            raise Refusal(
                "%s has no `; [match] segment OPEN` line -- step_base unknown" % SEG["log"]
            )
        base = int(m.group(3))
        proc_leaf = (sj or {}).get("process_dir")
        proc_log = os.path.join(logs_dir, proc_leaf, PROC["log"]) if proc_leaf else None
        arm = _read_text(proc_log) if proc_log and os.path.isfile(proc_log) else ""
        return dict(
            kind="segment",
            dir=path,
            snap_dir=path,
            files=files,
            base=base,
            map=(sj or {}).get("map"),
            config=config_of(arm),
            arm=arm_flags(arm),
            ref=log,
        )
    # a session folder without a segment (pre-SES7) -> its process folder
    if sj and sj.get("process_dir") and not os.path.isfile(os.path.join(path, PROC["orders"])):
        return resolve(os.path.join(logs_dir, sj["process_dir"]))
    files = {k: os.path.join(path, v) for k, v in PROC.items()}
    missing = [PROC[k] for k in ("orders", "clock", "seed", "log") if not os.path.isfile(files[k])]
    if missing:
        raise Refusal(
            "no match segment and no process recording here (missing %s)" % ", ".join(missing)
        )
    ref = _read_text(files["log"])
    arm = arm_flags(ref)
    if arm.get("seed_step") != "1" or arm.get("seed_mode") != "0":
        raise Refusal(
            "the process seed was not dumped at process step 1 (armed seed_step=%s seed_mode=%s)"
            % (arm.get("seed_step"), arm.get("seed_mode"))
        )
    played = [
        (leaf, j)
        for leaf, j in sessions_of_process(logs_dir, os.path.basename(path))
        if (j.get("final_clock_ms") or 0) > 0
    ]
    if len(played) != 1:
        raise Refusal(
            "a process recording replays only when exactly ONE match of that process ran sim steps "
            "(its seed is process step 1); found %d: %s -- use that match's mp:SES7 segment"
            % (len(played), ", ".join(leaf for leaf, _ in played) or "none")
        )
    return dict(
        kind="process",
        dir=path,
        snap_dir=os.path.join(logs_dir, played[0][0]),
        files=files,
        base=0,
        map=played[0][1].get("map"),
        config=config_of(ref),
        arm=arm,
        ref=ref,
        session=played[0][0],
    )


def snap_steps(snap_dir):
    """{step: path} of the desync watch's snapshots in a match folder (mh_desync_snap_<step>.bin)."""
    out = {}
    for p in glob.glob(os.path.join(snap_dir or "", "mh_desync_snap_*.bin")):
        m = re.search(r"mh_desync_snap_(\d+)\.bin$", p)
        if m:
            out[int(m.group(1))] = p
    return out


def snap_cadence(steps):
    """(every, max) that makes the replay dump at every recorded snapshot step (their gcd)."""
    import math

    if not steps:
        return 0, 0
    g = 0
    for s in steps:
        g = math.gcd(g, s)
    return g, max(steps) // g


def diff_snaps(a_path, b_path):
    """[region names whose VERDICT streams differ] between two MHSN snapshots of one step."""
    import mp_desync_snap_diff as sd

    a, b = sd.read_snap(a_path), sd.read_snap(b_path)
    if a["step"] != b["step"] or a["fp"] != b["fp"]:
        raise Refusal(
            "snapshot pair %s / %s is not comparable (step or manifest)" % (a_path, b_path)
        )
    names = [r["name"] if isinstance(r, dict) else str(r) for r in sd.load_manifest()]
    return [
        names[i] if i < len(names) else "col%d" % i
        for i, (x, y) in enumerate(zip(a["regions"], b["regions"]))
        if x != y
    ]


def clock_steps(path):
    return os.path.getsize(path) // 8


def order_records(path):
    return max(0, (os.path.getsize(path) - ORDERS_MAGIC_LEN) // ORDER_REC)


# ---- comparing ----------------------------------------------------------------------------------


def excluded_columns():
    """Positional indexes of the state_excluded() regions, plus the column names."""
    import fixture_replay
    import mp_analyze

    names = list(mp_analyze.REGION_NAMES)
    ex = fixture_replay.excluded_names()
    return names, {i for i, n in enumerate(names) if n in ex}


def compare(ref_steps, ref_regs, rep_steps, rep_regs, names=None, excluded=frozenset()):
    """-> dict(common, first, what, regions). `first` is None when every common step agrees."""
    common = sorted(set(ref_steps) & set(rep_steps))
    first, what = None, None
    for s in common:
        a, b = ref_steps[s], rep_steps[s]
        if a[2] != b[2] or a[0] != b[0]:
            first = s
            what = "state" if a[2] != b[2] else "clock"
            break
    regions = []
    if first is not None and first in ref_regs and first in rep_regs:
        for i, (x, y) in enumerate(zip(ref_regs[first], rep_regs[first])):
            if x != y and i not in excluded:
                regions.append(names[i] if names and i < len(names) else "col%d" % i)
    return dict(common=len(common), first=first, what=what, regions=regions)


def verdict_text(rec, res, min_steps):
    if res["common"] < min_steps:
        return (
            False,
            "REFUSED: only %d common step(s) (< %d) -- the replay did not run far enough"
            % (
                res["common"],
                min_steps,
            ),
        )
    if res["first"] is None:
        return True, "IDENTICAL: state + clock agree at all %d common step(s)" % res["common"]
    where = (
        " regions: %s" % ", ".join(res["regions"])
        if res["regions"]
        else " (no R lines on both sides at that step)"
    )
    return False, "DIVERGED at match step %d (%s);%s" % (res["first"], res["what"], where)


# ---- running ------------------------------------------------------------------------------------


def map_row(maps_dir, map_name):
    """Row index of `map_name` in "Available maps" (case-insensitive), or None."""
    if not map_name:
        return 0, None
    files = sorted(
        (f for f in os.listdir(maps_dir) if f.lower().endswith(".mpm")), key=lambda f: f.upper()
    )
    for i, f in enumerate(files):
        if f.lower() == map_name.lower():
            return i, os.path.splitext(map_name)[0].lower()
    return None, None


def walk_script(row, label):
    """sp_det.txt's walk with the map row picked (split click, then the right panel's label)."""
    y = MAP_ROW_Y0 + MAP_ROW_DY * row
    pick = (
        "cursor %d %d\ncursor %d %d\npress %d %d\nrelease %d %d\npresent %s\n"
        % ((MAP_ROW_X, y) * 4 + (label,))
        if label
        else ""
    )
    return (
        "# GENERATED by tools/replay_match_segment.py (mp:SES7b): host alone on the recorded map,\n"
        "# seat one computer, Start; the harness injects the seed and replays the recording.\n"
        "settled value:110\nclickv 110\npresent Player name\nsettled Ok\nclickl Ok\n"
        "settled Create\nclickl Create\npresent Game name\nsettled Ok\nclickl Ok\n"
        "present Available maps\nsettled Ok\n" + pick + "settled Ok\nclickl Ok\n"
        "present Network players\nsettled Start\nclick 70 132\npresent Computer\nenabled Start\n"
        "clickl Start\nabsent Network players\ngamemode 2\n"
        "log REPLAY in-game, the harness owns the run from here\nend\n"
    )


def provision_lane(config):
    import ui_test

    lane = ui_test._solo_lane_dir("mhreplay_c%d" % config)
    lib = os.path.join(lane, "libmh.dll")
    if config == 1 and os.path.isfile(lib):
        os.remove(lib)  # configuration (1): no spine; refresh_satellites will not put it back
    if config == 2 and not os.path.isfile(lib):
        shutil.copy(os.path.join(REPO, "src", "mh_dll", "Release", "libmh.dll"), lib)
    return lane


def run_replay(rec, config, steps, timeout):
    import make_lane

    lane = provision_lane(config)
    for k, dst in LANE_NAME.items():
        shutil.copyfile(rec["files"][k], os.path.join(lane, dst))
    row, label = map_row(os.path.join(lane, "Maps"), rec.get("map"))
    if row is None:
        raise Refusal(
            "map %r is not in this lane's Maps\\ -- cannot load the recorded map" % rec["map"]
        )
    tmpd = os.path.join(REPO, "tmp", "replay_match")
    os.makedirs(tmpd, exist_ok=True)
    script = os.path.join(tmpd, "replay_walk.txt")
    with open(script, "w", encoding="ascii", newline="\n") as fh:
        fh.write(walk_script(row, label))
    flags = dict(REPLAY_FLAGS)
    flags["pin_fpu"] = rec["arm"].get("pin_fpu", "1")
    flags["stop_step"] = steps
    every, nmax = snap_cadence([k for k in snap_steps(rec.get("snap_dir")) if k <= steps])
    if every:
        flags["verdict_snap_every"] = every
        flags["verdict_snap_max"] = nmax
    ident = make_lane.read_identity(lane)
    argv = [
        sys.executable,
        os.path.join(REPO, "tools", "ui_test.py"),
        script,
        "--harness",
        "--steps",
        str(steps),
        "--harness-extra",
        ";".join("%s=%s" % kv for kv in flags.items()),
        "--host-dir",
        lane,
        "--port",
        str(6300 + int(ident.get("lane") or 0)),
        "--timeout",
        str(timeout),
        "--timeout-frames",
        str(max(400000, steps * 4)),
        "--stall-timeout",
        "120",
        "--headless",
    ]
    before = (
        set(os.listdir(os.path.join(lane, "logs")))
        if os.path.isdir(os.path.join(lane, "logs"))
        else set()
    )
    print(
        "  replay lane %s (configuration (%d)), map %r row %d, %d steps"
        % (lane, config, rec.get("map"), row, steps)
    )
    print("  $ " + " ".join(argv[1:]))
    subprocess.run(argv, cwd=REPO)
    after = set(os.listdir(os.path.join(lane, "logs")))
    new = sorted(
        d for d in after - before if os.path.isfile(os.path.join(lane, "logs", d, "mh_harness.log"))
    )
    if not new:
        raise Refusal("the replay left no run folder carrying an mh_harness.log")
    return os.path.join(lane, "logs", new[-1])


def _session_of_run(run):
    """The replay's session folder (its snapshots land wherever MH_RunDir pointed at the time)."""
    logs = os.path.dirname(run)
    for leaf, _ in sessions_of_process(logs, os.path.basename(run)):
        return os.path.join(logs, leaf)
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dir", nargs="?", help="a match (session) folder, or a process folder")
    ap.add_argument("--config", default="auto", choices=("auto", "1", "2"))
    ap.add_argument("--steps", type=int, default=0, help="replay only the first N match steps")
    ap.add_argument("--compare", help="compare against an existing replay run folder; do not run")
    ap.add_argument("--min-steps", type=int, default=50)
    ap.add_argument("--timeout", type=int, default=3600)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not a.dir:
        ap.error("a recording folder is required")
    try:
        rec = resolve(a.dir)
    except Refusal as e:
        print("replay_match_segment: REFUSED -- %s" % e)
        return 2
    ref_steps, ref_regs = parse_stream(rec["ref"], rec["base"])
    ref_steps = {s: v for s, v in ref_steps.items() if s >= 1}
    n_clock = clock_steps(rec["files"]["clock"])
    last = min(max(ref_steps) if ref_steps else 0, n_clock)
    if a.steps:
        last = min(last, a.steps)
    config = (rec["config"] or 1) if a.config == "auto" else int(a.config)
    print(
        "recording: %s (%s), base=%d, %d hashed step(s), %d clock step(s), %d order record(s), "
        "map=%r, recorded in configuration (%s)"
        % (
            rec["dir"],
            rec["kind"],
            rec["base"],
            len(ref_steps),
            n_clock,
            order_records(rec["files"]["orders"]),
            rec.get("map"),
            rec["config"] or "?",
        )
    )
    if last < 1:
        print("replay_match_segment: REFUSED -- no recorded step to replay")
        return 2
    try:
        run = a.compare or run_replay(rec, config, last, a.timeout)
    except Refusal as e:
        print("replay_match_segment: REFUSED -- %s" % e)
        return 2
    rep_steps, rep_regs = parse_stream(_read_text(os.path.join(run, "mh_harness.log")))
    ref_steps = {s: v for s, v in ref_steps.items() if s <= last}
    names, excluded = excluded_columns()
    res = compare(ref_steps, ref_regs, rep_steps, rep_regs, names, excluded)
    ok, text = verdict_text(rec, res, min(a.min_steps, last))
    print("  replay run: %s" % run)
    print("  %s (recorded steps 1..%d, replayed %d)" % (text, last, len(rep_steps)))
    # the recording's desync snapshots (a desynced lockstep match writes them) vs the replay's own
    # dumps at the same match steps: the region names a region-less (player) log cannot give
    rec_snaps, rep_snaps = snap_steps(rec.get("snap_dir")), snap_steps(run)
    rep_snaps.update(snap_steps(_session_of_run(run)))
    ex_names = {names[i] for i in excluded}
    for k in sorted(set(rec_snaps) & set(rep_snaps)):
        d = diff_snaps(rec_snaps[k], rep_snaps[k])
        judged = [r for r in d if r not in ex_names]
        print(
            "  snapshot step %d: %s (+%d state_excluded region(s) differ, not judged)"
            % (
                k,
                ("differs in " + ", ".join(judged)) if judged else "every judged region IDENTICAL",
                len(d) - len(judged),
            )
        )
        if judged:
            print(
                "    bytes: python tools/mp_desync_snap_diff.py %s %s"
                % (rec_snaps[k], rep_snaps[k])
            )
    return 0 if ok else 1


# ---- selftest -----------------------------------------------------------------------------------


def _line(step, clock, state, regs=None):
    s = "%d %s %s %s\n" % (step, clock, "C0FFEE00C0FFEE00", state)
    if regs is not None:
        s += "R %d %s\n" % (step, " ".join(regs))
    return s


def selftest():
    fails = []

    def check(label, cond):
        print("  %-66s %s" % (label, "ok" if cond else "FAIL"))
        if not cond:
            fails.append(label)

    names = ["a", "b", "c"]
    ok_regs = ["1" * 16, "2" * 16, "3" * 16]
    clk = lambda s: "%016X" % (0x3F80000000000000 + s)  # noqa: E731
    st = lambda s: "%016X" % (0xABC0000 + s)  # noqa: E731
    ref = "".join(_line(100 + s, clk(s), st(s), ok_regs) for s in range(1, 61))
    rep = "".join(_line(s, clk(s), st(s), ok_regs) for s in range(1, 61))
    r1, rr1 = parse_stream(ref, 100)
    r2, rr2 = parse_stream(rep)
    res = compare(r1, rr1, r2, rr2, names, {2})
    check("identical streams (rebased by step_base) -> IDENTICAL", verdict_text({}, res, 50)[0])
    bad = list(ok_regs)
    bad[1] = "F" * 16
    rep2 = "".join(
        _line(s, clk(s), st(s) if s < 40 else "DEADBEEF00000000", bad if s >= 40 else ok_regs)
        for s in range(1, 61)
    )
    r3, rr3 = parse_stream(rep2)
    res = compare(r1, rr1, r3, rr3, names, {2})
    check(
        "a state mismatch names the first step and the region",
        res["first"] == 40 and res["regions"] == ["b"] and not verdict_text({}, res, 50)[0],
    )
    ex = list(ok_regs)
    ex[2] = "E" * 16
    rep3 = "".join(_line(s, clk(s), st(s), ex) for s in range(1, 61))
    r4, rr4 = parse_stream(rep3)
    check(
        "an excluded-region-only difference is not a divergence",
        compare(r1, rr1, r4, rr4, names, {2})["first"] is None,
    )
    rep4 = "".join(_line(s, clk(s) if s != 7 else clk(99), st(s)) for s in range(1, 61))
    r5, rr5 = parse_stream(rep4)
    res = compare(r1, rr1, r5, rr5, names, {2})
    check("a clock mismatch is a divergence", res["first"] == 7 and res["what"] == "clock")
    short = "".join(_line(s, clk(s), st(s)) for s in range(1, 11))
    r6, rr6 = parse_stream(short)
    check(
        "too few common steps is REFUSED, not IDENTICAL",
        not verdict_text({}, compare(r1, rr1, r6, rr6), 50)[0],
    )

    # resolve(): a segment, an incomplete segment, and the two process-recording shapes
    tmp = tempfile.mkdtemp(prefix="replay_seg_selftest_")
    try:
        logs = os.path.join(tmp, "logs")
        proc = os.path.join(logs, "20260101T000000Z_menu_solo")
        s1 = os.path.join(logs, "20260101T000100Z_aaaa_0_solo")
        s2 = os.path.join(logs, "20260101T000200Z_bbbb_0_solo")
        for d in (proc, s1, s2):
            os.makedirs(d)

        def put(d, name, data=b""):
            with open(os.path.join(d, name), "wb") as fh:
                fh.write(data if isinstance(data, bytes) else data.encode("latin-1"))

        armed = (
            "; [harness] configuration (1): spine ABSENT, registry=mh.dll\n"
            "; ==== mh replay harness armed: seed_step=1 seed_mode=0 stop_step=0 fixed_step=0 "
            "pin_fpu=1 region_hash_step=0 order_mode=1 replay_ai_off=0 suppress_enqueue=0 ====\n"
        )
        put(proc, "mh_harness.log", armed + ref)
        for n in ("mh_orders.bin", "mh_clock.bin", "mh_harness_seed.bin"):
            put(proc, n, b"\0" * 16)
        put(
            s1,
            "session.json",
            json.dumps(
                {"process_dir": os.path.basename(proc), "map": "x.mpm", "final_clock_ms": 5000}
            ),
        )
        put(
            s2,
            "session.json",
            json.dumps(
                {"process_dir": os.path.basename(proc), "map": "x.mpm", "final_clock_ms": 9000}
            ),
        )
        put(
            s2,
            "mh_match_harness.log",
            "; [match] segment OPEN %s at process step 101: match step k = process step 100+k (step_base=100).\n"
            % os.path.basename(s2)
            + ref,
        )
        for k in ("orders", "clock", "seed"):
            put(s2, SEG[k], b"\0" * 16)
        r = resolve(s2)
        check(
            "a segment resolves with its step_base, map and the process's configuration",
            r["kind"] == "segment"
            and r["base"] == 100
            and r["map"] == "x.mpm"
            and r["config"] == 1,
        )
        os.remove(os.path.join(s2, SEG["seed"]))
        try:
            resolve(s2)
            check("a segment without its seed is REFUSED", False)
        except Refusal:
            check("a segment without its seed is REFUSED", True)
        try:
            resolve(s1)
            check("a pre-SES7 match that is not the process's only played one is REFUSED", False)
        except Refusal as e:
            check(
                "a pre-SES7 match that is not the process's only played one is REFUSED",
                "exactly ONE" in str(e),
            )
        put(
            s2,
            "session.json",
            json.dumps(
                {"process_dir": os.path.basename(proc), "map": "x.mpm", "final_clock_ms": 0}
            ),
        )
        for n in SEG.values():
            p = os.path.join(s2, n)
            if os.path.isfile(p):
                os.remove(p)
        r = resolve(s1)
        check(
            "the process's ONLY played match replays from the process files",
            r["kind"] == "process" and r["base"] == 0 and r["session"] == os.path.basename(s1),
        )
        put(proc, "mh_harness.log", armed.replace("seed_mode=0", "seed_mode=2") + ref)
        try:
            resolve(s1)
            check("a process recording whose seed was never dumped is REFUSED", False)
        except Refusal:
            check("a process recording whose seed was never dumped is REFUSED", True)
        # the walk picks the right row
        maps = os.path.join(tmp, "Maps")
        os.makedirs(maps)
        for f in ("Blue Monday.mpm", "BlueMonday.mpm", "Cold War.mpm", "Last Question.mpm"):
            put(maps, f)
        row, label = map_row(maps, "last question.mpm")
        check(
            "map row: case-folded file order (Last Question = row 3 here)",
            row == 3 and label == "last question",
        )
        check(
            "the walk splits the row click and gates on the map label",
            "press 80 140" in walk_script(row, label)
            and "present last question" in walk_script(row, label),
        )
        check(
            "an unknown map is None (refused by the runner)",
            map_row(maps, "nowhere.mpm")[0] is None,
        )
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("replay_match_segment selftest: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    if "--selftest" in sys.argv[1:] or "--compare" in sys.argv[1:]:
        raise SystemExit(main())
    import hostlock

    raise SystemExit(hostlock.run_rig_tool(main, "replay_match_segment"))
