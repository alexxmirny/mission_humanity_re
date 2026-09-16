#!/usr/bin/env python3
"""gate_timeline.py -- per-lane timeline + utilization for the most recent gate run.

    python tools/gate_timeline.py                 # newest run window (auto-detected)
    python tools/gate_timeline.py --window 1200   # widen the cluster cut-off (seconds)

Reads every surviving game-run directory (workdir/mh_lanes/*/logs/*_solo) and takes each run's
START/END from its own mh_net.log timestamps ([HH:MM:SS.mmm] -- the DLL's log clock), so the
numbers are the game's, not the filesystem's. Prints the sorted timeline, the busy-sum /
wall-clock ratio (effective games-in-flight), a concurrency profile, and per-run fps from
mh_frametime.log -- the number that says whether a lane SPUN or SLEPT (the [pacing] fps_cap
story, 2026-09-10).

The window is the newest CLUSTER of runs: everything whose end lies within --window seconds of
the newest end. A gate run is one cluster; --window only matters when two runs sit close.

LANES ONLY, and that is deliberate: the abc/ab archive copies (tmp/ui_sessions, tmp/ab/logs)
duplicate lane run dirs byte-for-byte, so counting them double-counts busy time -- the first
version of this analysis did exactly that (4341s busy for 2080s of games).
"""

import argparse
import glob
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


def frames(run):
    p = os.path.join(run, "mh_frametime.log")
    try:
        return sum(1 for _ in open(p, "rb")) - 1
    except OSError:
        return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--window", type=int, default=900, help="cluster cut-off seconds")
    args = ap.parse_args()

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
        rows.append((s[0], s[1], lane, frames(d), mt))
    if not rows:
        print("no lane run dirs under %s" % make_lane.LANE_ROOT)
        return 1

    # Window by MTIME (real recency), positions by the log stamps: newest cluster only.
    newest_mt = max(r[4] for r in rows)
    rows = sorted((s, e, lane, fr) for s, e, lane, fr, mt in rows if mt > newest_mt - args.window)
    newest = max(e for _s, e, _l, _f in rows)
    t0 = rows[0][0]
    print(
        "gate timeline: %d game runs; t0 = %02d:%02d:%02d"
        % (len(rows), int(t0) // 3600, int(t0) % 3600 // 60, int(t0) % 60)
    )
    print("  %-26s %6s %6s %6s %8s %6s  %s" % ("lane", "start", "end", "dur", "frames", "fps", ""))
    # CPU-BUSY vs WAIT-BOUND (user, 2026-09-10: "gate_timeline counts network tests as cpu-busy").
    # A lane under the [pacing] fps_cap SLEEPS most of every frame -- its duration is wall
    # occupancy, not CPU -- so summing it with the spinning lanes overstates the machine's load.
    # The classifier is the measured fps itself: the cap lands at ~40-65, everything uncapped runs
    # hundreds-plus, so 70 splits the population with a wide margin on both sides.
    CAP_FPS = 70
    cpu_busy = wait_busy = 0.0
    for s, e, lane, fr in rows:
        d = e - s
        capped = fr / max(d, 1) < CAP_FPS
        if capped:
            wait_busy += d
        else:
            cpu_busy += d
        print(
            "  %-26s %6.0f %6.0f %6.0f %8d %6.0f  %s"
            % (lane, s - t0, e - t0, d, fr, fr / max(d, 1), "wait" if capped else "cpu")
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
    events = sorted([(s, 1) for s, _e, _l, _f in rows] + [(e, -1) for _s, e, _l, _f in rows])
    cur, prev, prof = 0, events[0][0], {}
    for t, dv in events:
        prof[cur] = prof.get(cur, 0) + (t - prev)
        cur += dv
        prev = t
    print(
        "concurrency profile (s at N games): %s"
        % {k: round(v) for k, v in sorted(prof.items()) if v >= 1}
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
