#!/usr/bin/env python3
"""gate_timeline.py -- per-lane timeline + utilization for the most recent gate run.

    python tools/gate_timeline.py                 # the LAST GATE RUN's window (tmp/gate/last_timings.json)
    python tools/gate_timeline.py --newest        # the newest cluster of runs instead (any runner)
    python tools/gate_timeline.py --newest --window 1200   # widen that cluster cut-off (seconds)

Reads every surviving game-run directory (workdir/mh_lanes/*/logs/*_solo) and takes each run's
START/END from its own mh_net.log timestamps ([HH:MM:SS.mmm] -- the DLL's log clock), so the
numbers are the game's, not the filesystem's. Prints the sorted timeline, the busy-sum /
wall-clock ratio (effective games-in-flight), a concurrency profile, and per-run fps from
mh_frametime.log -- the number that says whether a lane SPUN or SLEPT (the [pacing] fps_cap
story, 2026-09-10).

THE WINDOW is the last gate run's own span: run_gate's record (tmp/gate/last_timings.json) is
written when the gate ends, so [its mtime - _gate_wall, its mtime] is the gate, whatever ran on the
lanes afterwards. (2026-09-25: the old default -- the newest cluster of runs -- showed 4 rows after
a single standalone test_ui row ran post-gate, hiding the whole gate.) --newest keeps the cluster
rule: everything whose end lies within --window seconds of the newest end.

LABELS (gate diet block 4c, 2026-09-24). Each row names its GATE UNIT and SCENARIO: a capture-suite
lane is resolved through the TESTS registry (the lane's own row, or the share_lanes row whose script
the run loaded), a recorded-session arm (ui_play..ui_play3) by the journal its harness log names,
and the single-purpose lanes by their block. The CRITICAL PATH is printed explicitly from
run_gate's own record (tmp/gate/last_timings.json `_units`): the unit that ended last bounds the
gate, and inside the suite the longest share_lanes chain is the floor no pool width can beat.

LANES ONLY, and that is deliberate: the abc/ab archive copies (tmp/ui_sessions, tmp/ab/logs)
duplicate lane run dirs byte-for-byte, so counting them double-counts busy time -- the first
version of this analysis did exactly that (4341s busy for 2080s of games).
"""

import argparse
import glob
import json
import os
import re
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import make_lane  # noqa: E402

TS = re.compile(r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]")


def net_span(run):
    p = os.path.join(run, "mh_net.log")
    if not os.path.isfile(p):
        return None
    first = last = None
    with open(p, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            m = TS.match(ln)
            if m:
                t = (
                    int(m.group(1)) * 3600
                    + int(m.group(2)) * 60
                    + int(m.group(3))
                    + int(m.group(4)) / 1000
                )
                if first is None:
                    first = t
                last = t
    if first is None:
        return None
    if last < first:  # the run crossed midnight
        last += 86400
    return first, last


LOADED_RE = re.compile(r"\[script\] loaded '([^']+)'")
JOURNAL_RE = re.compile(r"journal=[^\r\n]*?([^\\/\r\n]+)\.journal")
# Lane-name prefixes of the single-purpose (non-suite) lanes -> their run_gate unit.
LANE_UNITS = (
    ("ui_sp_det", "spdet"),
    ("ui_tact_", "suite"),  # the suite's pooled tactical-journal tail
    ("ui_determinism", "det_c1"),  # --det-local's pair (det itself runs on the VMs)
    ("inmem", "inmem"),
    ("f1e_parity", "inmem"),  # check_inmem_patch_parity.py --run's own lane
    ("det3_", "u19j_gpfg3"),  # the U19j 3-peer shape's local third peer (the VMs hold the others)
    ("ui_play", "abc"),
)


def _registry():
    """{lane name: [TESTS rows that may run on it]} -- a lane's owner plus every share_lanes row
    that borrows it. Empty (and labels degrade to the lane name) if test_ui will not import."""
    try:
        import test_ui  # noqa: PLC0415 -- heavy; only the labeller needs it
    except Exception:
        return {}
    by_name = {t["name"]: t for t in test_ui.TESTS}
    lanes = {}
    for t in test_ui.TESTS:
        own = list(test_ui.lane_names(t))
        if not own:
            # a sharer: its targets' lanes in a full run, its OWN lanes when run without them
            own = [ln for x in test_ui.share_targets(t) for ln in test_ui.lane_names(by_name[x])]
            own += test_ui.lane_names(dict(t, share_lanes=None))
        for ln in own:
            lanes.setdefault(ln, []).append(t)
    return lanes


def _first_match(path, rx, limit=4000):
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for i, ln in enumerate(fh):
                m = rx.search(ln)
                if m:
                    return m.group(1)
                if i > limit:
                    break
    except OSError:
        pass
    return None


def label(run, lane, lanes):
    """(unit, scenario) for one game run dir."""
    # A session dir's uidrive/harness lines are in its PROCESS (menu) dir: the newest *_menu_* dir
    # of the same logs/ folder at or before this one.
    logs = os.path.dirname(run)
    base = os.path.basename(run)
    menus = sorted(d for d in os.listdir(logs) if "_menu_" in d and d <= base)
    pdir = os.path.join(logs, menus[-1]) if menus else run
    if lane in lanes:
        script = _first_match(os.path.join(pdir, "mh_uidrive.log"), LOADED_RE)
        cands = lanes[lane]
        if script:
            hit = [
                t
                for t in cands
                if script in ([t.get("script"), t.get("host")] + list(t.get("clients") or []))
            ]
            cands = hit or cands
        return "suite", "/".join(t["name"] for t in cands[:2])
    for pre, unit in LANE_UNITS:
        if lane.startswith(pre):
            if unit == "abc":
                j = _first_match(os.path.join(pdir, "mh_harness.log"), JOURNAL_RE)
                return ("abc_" + j.split("-")[0] if j else "abc"), (j or lane)
            return unit, lane
    return "?", lane


def critical_path():
    """Print the gate's critical path from run_gate's own record."""
    rec_p = os.path.join(REPO, "tmp", "gate", "last_timings.json")
    try:
        rec = json.load(open(rec_p, encoding="utf-8"))
    except (OSError, ValueError):
        print("critical path: no %s (run tools/run_gate.py)" % os.path.relpath(rec_p, REPO))
        return
    units = rec.get("_units") or {}
    if not units:
        print("critical path: the record predates unit spans (gate diet block 4)")
        return
    last = max(units.items(), key=lambda kv: kv[1][1])
    print(
        "critical path: gate wall %.0fs, bounded by unit %s (%s, %.0fs..%.0fs = %.0fs)"
        % (
            rec.get("_gate_wall", 0),
            last[0],
            last[1][2],
            last[1][0],
            last[1][1],
            last[1][1] - last[1][0],
        )
    )
    for n, (a, b, v) in sorted(units.items(), key=lambda kv: kv[1][0]):
        print("    %-12s %6.0f -> %6.0f  (%5.0fs) %s" % (n, a, b, b - a, v))
    chains = rec.get("_suite_chains") or []
    if chains:
        top = max(chains, key=lambda c: c.get("secs", 0))
        print(
            "  inside the suite, the longest serial chain (share_lanes group) is %.0fs: %s"
            % (top.get("secs", 0), " -> ".join(top.get("tests", [])))
        )
        sw = (units.get("suite") or [0, 0])[1] - (units.get("suite") or [0, 0])[0]
        print("  every suite chain (secs, share of the suite's %.0fs span):" % sw)
        for c in sorted(chains, key=lambda c: -c.get("secs", 0)):
            print(
                "    %5.0fs %4.0f%%  %s"
                % (
                    c.get("secs", 0),
                    100.0 * c.get("secs", 0) / max(sw, 1),
                    " -> ".join(c.get("tests", [])),
                )
            )
    for r in rec.get("_reds") or []:
        print("  RED (cost): %s" % r)


def gate_window():
    """(start_epoch, end_epoch) of the last gate run, or None without a usable record."""
    rec_p = os.path.join(REPO, "tmp", "gate", "last_timings.json")
    try:
        rec = json.load(open(rec_p, encoding="utf-8"))
        end = os.path.getmtime(rec_p)
    except (OSError, ValueError):
        return None
    wall = rec.get("_gate_wall")
    return (end - wall, end) if wall else None


def unit_utilization(rows, t0):
    """Per gate unit: its span (run_gate's record), the game-seconds its lanes ran inside it, and
    the average games in flight -- the unit's real core use, cpu-busy vs wait-bound (capped)."""
    rec_p = os.path.join(REPO, "tmp", "gate", "last_timings.json")
    try:
        units = json.load(open(rec_p, encoding="utf-8")).get("_units") or {}
    except (OSError, ValueError):
        units = {}
    CAP_FPS = 70
    agg = {}
    for s, e, lane, fr, (unit, scen) in rows:
        d = e - s
        a = agg.setdefault(unit, [0.0, 0.0, 0, None, None])
        a[1 if fr / max(d, 1) < CAP_FPS else 0] += d
        a[2] += 1
        a[3] = s if a[3] is None else min(a[3], s)
        a[4] = e if a[4] is None else max(a[4], e)
    print("per-unit utilization (games in flight = game-seconds / unit span):")
    print(
        "  %-16s %6s %7s %7s %6s %7s %7s"
        % ("unit", "span", "cpu s", "wait s", "games", "cpu/sp", "all/sp")
    )
    for unit, (cpu, wait, n, a, b) in sorted(agg.items(), key=lambda kv: -(kv[1][0] + kv[1][1])):
        sp = units.get(unit)
        span = (sp[1] - sp[0]) if sp else (b - a)
        print(
            "  %-16s %6.0f %7.0f %7.0f %6d %7.2f %7.2f%s"
            % (
                unit,
                span,
                cpu,
                wait,
                n,
                cpu / max(span, 1),
                (cpu + wait) / max(span, 1),
                "" if sp else "  (span from games)",
            )
        )


def frames(run):
    p = os.path.join(run, "mh_frametime.log")
    try:
        return sum(1 for _ in open(p, "rb")) - 1
    except OSError:
        return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--window", type=int, default=900, help="cluster cut-off seconds (--newest)")
    ap.add_argument(
        "--newest", action="store_true", help="newest cluster of runs, not the last gate's window"
    )
    args = ap.parse_args()
    gate = None if args.newest else gate_window()

    rows = []
    for d in glob.glob(os.path.join(make_lane.LANE_ROOT, "*", "logs", "*_solo")):
        s = net_span(d)
        if not s:
            continue
        try:
            mt = os.path.getmtime(os.path.join(d, "mh_net.log"))
        except OSError:
            continue
        # CANNED-LOG FILTER. The det-selftest fixtures (det3c etc.) are COPIED logs: their file
        # mtimes are fresh (the lint unit re-runs --det-selftest every gate) but their in-log
        # stamps are whenever the fixture was recorded -- the first run of this tool put a
        # "1,090,343 fps" det3c row at the top of the timeline off exactly that. A REAL run's
        # last log stamp agrees with its file's mtime clock-of-day to within seconds.
        lt = time.localtime(mt)  # the log stamps are LOCAL clock-of-day; mtime is epoch (UTC)
        mt_day = lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec
        drift = abs((s[1] % 86400) - mt_day)
        if min(drift, 86400 - drift) > 120:
            continue
        lane = os.path.basename(os.path.dirname(os.path.dirname(d)))
        rows.append((s[0], s[1], lane, frames(d), mt, d))
    if not rows:
        print("no lane run dirs under %s" % make_lane.LANE_ROOT)
        return 1

    # Window by MTIME (real recency), positions by the log stamps: the gate's own span, or (with
    # --newest / no gate record) the newest cluster.
    if gate:
        lo, hi = gate[0] - 30, gate[1] + 30
        keep = [r for r in rows if lo <= r[4] <= hi]
        print(
            "window: last gate run, %s .. %s (tmp/gate/last_timings.json; --newest for the latest cluster)"
            % (
                time.strftime("%H:%M:%S", time.localtime(gate[0])),
                time.strftime("%H:%M:%S", time.localtime(gate[1])),
            )
        )
    else:
        newest_mt = max(r[4] for r in rows)
        keep = [r for r in rows if r[4] > newest_mt - args.window]
    if not keep:
        print("no lane run dirs inside the window (the lanes were cleaned since?) -- try --newest")
        critical_path()
        return 1
    lanes = _registry()
    rows = sorted((s, e, lane, fr, label(d, lane, lanes)) for s, e, lane, fr, mt, d in keep)
    newest = max(r[1] for r in rows)
    t0 = rows[0][0]
    print(
        "gate timeline: %d game runs; t0 = %02d:%02d:%02d"
        % (len(rows), int(t0) // 3600, int(t0) % 3600 // 60, int(t0) % 60)
    )
    print(
        "  %-26s %-12s %-30s %6s %6s %6s %8s %6s  %s"
        % ("lane", "unit", "scenario", "start", "end", "dur", "frames", "fps", "")
    )
    # CPU-BUSY vs WAIT-BOUND (user, 2026-09-10: "gate_timeline counts network tests as cpu-busy").
    # A lane under the [pacing] fps_cap SLEEPS most of every frame -- its duration is wall
    # occupancy, not CPU -- so summing it with the spinning lanes overstates the machine's load.
    # The classifier is the measured fps itself: the cap lands at ~40-65, everything uncapped runs
    # hundreds-plus, so 70 splits the population with a wide margin on both sides.
    CAP_FPS = 70
    cpu_busy = wait_busy = 0.0
    for s, e, lane, fr, (unit, scen) in rows:
        d = e - s
        capped = fr / max(d, 1) < CAP_FPS
        if capped:
            wait_busy += d
        else:
            cpu_busy += d
        print(
            "  %-26s %-12s %-30s %6.0f %6.0f %6.0f %8d %6.0f  %s"
            % (
                lane,
                unit,
                scen[:30],
                s - t0,
                e - t0,
                d,
                fr,
                fr / max(d, 1),
                "wait" if capped else "cpu",
            )
        )
    wall = newest - t0
    print(
        "cpu-busy %.0fs + wait-bound %.0fs over %.0fs wall -> avg %.2f CPU-busy games "
        "(%.2f incl. waiters)"
        % (
            cpu_busy,
            wait_busy,
            wall,
            cpu_busy / max(wall, 1),
            (cpu_busy + wait_busy) / max(wall, 1),
        )
    )
    events = sorted([(r[0], 1) for r in rows] + [(r[1], -1) for r in rows])
    cur, prev, prof = 0, events[0][0], {}
    for t, dv in events:
        prof[cur] = prof.get(cur, 0) + (t - prev)
        cur += dv
        prev = t
    print(
        "concurrency profile (s at N games): %s"
        % {k: round(v) for k, v in sorted(prof.items()) if v >= 1}
    )
    unit_utilization(rows, t0)
    critical_path()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
