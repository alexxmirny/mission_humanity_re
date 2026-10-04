#!/usr/bin/env python3
"""check_u62_handover.py -- mp:U62 (HM-M4): did the PLANNED HANDOVER of the hub work on a 3-peer run?

Reads the three pulled peer dirs of a `det_arms.py --u62-*` run (<det_dir>/{host,client1,client2}); the
host is the LEAVER (it quits by ESC menu, or its process exits gracefully), the two clients are the
SURVIVORS. Pass conditions (the done_when of mp:U62):

  * the leaver's net log holds `hub-leave -- HANDED OVER` (the handover ran and every client acknowledged);
  * BOTH survivors log `hub-handover` (one became the hub, one re-dialled it) and a `U62 notice: hub A -> B` seam
    line (the in-game notice), naming the SAME new hub;
  * both survivors kept stepping >= MIN_POST steps past the leaver's last step;
  * the survivors are ALL PAIRS IDENTICAL over >= MIN_POST steps past the leaver's last step (mp_analyze);
  * no stall > STALL_MAX_MS of the survivors' game clock around the exit (mh_lockstep.log wall gap between
    clock advances, in the window [exit clock - 1 s, exit clock + 10 s]).

`--negative` inverts the migration expectation (the `[net] hub_migration=0` arm): the leaver must log
`nothing to hand over`, no survivor may log `hub-handover`, and a survivor's net log must show the U55
outcome (`on_gameover ... outcome=8`); the timeline (seconds from the first stall to outcome 8) is printed
and must be inside 30..90 s (U55 measured ~58 s).

  python tools/check_u62_handover.py <det_dir> [--negative] [--exit-step N] [--min-post N]
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_u53_elim as u53  # noqa: E402

MIN_POST = 1000
STALL_MAX_MS = 1000
SURVIVORS = ("client1", "client2")


def _wall(ln):
    m = re.match(r"\[(\d\d:\d\d:\d\d\.\d+)\] ", ln)
    return u53._secs(m.group(1)) if m else None


def stall_window(path, clock_lo_ms, clock_hi_ms):
    """Longest wall-clock gap (ms) between two consecutive rows in which the game clock ADVANCED,
    restricted to rows whose clock is inside [lo, hi]. Returns (gap_ms, rows_considered)."""
    if not os.path.isfile(path):
        raise u53.Refusal("missing %s" % path)
    last_clock, last_wall, worst, n = None, None, 0, 0
    for ln in open(path, errors="replace"):
        f = ln.split()
        if ln.startswith("#") or len(f) < 14:
            continue
        try:
            wall, clock = int(f[0]), int(f[1])
        except ValueError:
            continue
        if clock < clock_lo_ms or clock > clock_hi_ms:
            last_clock, last_wall = clock, wall
            continue
        n += 1
        if last_clock is None or clock != last_clock:
            if last_wall is not None and last_clock is not None:
                worst = max(worst, wall - last_wall)
            last_clock, last_wall = clock, wall
    return worst, n


def check(det_dir, negative=False, exit_step=None, min_post=MIN_POST):
    out, bad = [], []
    logs = {}
    for p in u53.PEERS:
        d = os.path.join(det_dir, p)
        logs[p] = (
            u53._read(os.path.join(d, "mh_net.log")),
            u53._read(os.path.join(d, "mh_harness.log")),
        )
    host_net, host_har = logs["host"]
    hs = u53.harness_steps(host_har)
    if not hs:
        raise u53.Refusal("the host's harness log has no hashed steps")
    leave_step, leave_clock = hs[-1]
    if exit_step is not None:
        leave_step = max(leave_step, exit_step) if not hs else hs[-1][0]
    out.append("leaver (host) last hashed step=%d (gclk %.1fs)" % (leave_step, leave_clock))
    last = {}
    for sv in SURVIVORS:
        st = u53.harness_steps(logs[sv][1])
        last[sv] = st[-1][0] if st else 0
        out.append(
            "%s last hashed step=%d (%d past the leaver)" % (sv, last[sv], last[sv] - leave_step)
        )
    handed = [ln for ln in host_net.splitlines() if "hub-leave -- HANDED OVER" in ln]
    nothing = [ln for ln in host_net.splitlines() if "hub-leave -- nothing to hand over" in ln]
    changes = {}
    for sv in SURVIVORS:
        net = logs[sv][0]
        hov = [ln for ln in net.splitlines() if "hub-handover --" in ln]
        chg = re.findall(r"U62 notice: hub (\d+) -> (\d+)", net)
        changes[sv] = chg[-1] if chg else None
        for ln in hov[:3]:
            out.append("    %s: %s" % (sv, ln.strip()[:200]))
        if negative:
            if hov:
                bad.append("%s logged a hub-handover with migration OFF" % sv)
        elif not hov:
            bad.append("%s never logged a hub-handover" % sv)
    if negative:
        out.append(
            "leaver handover line: %s"
            % ("; ".join(x.strip()[-110:] for x in nothing[:1]) or "MISSING")
        )
        if handed:
            bad.append("the leaver handed over with migration OFF")
        if not nothing:
            out.append(
                "NOTE: no `nothing to hand over` line (the leaver may have died before logging it)"
            )
        found = False
        for sv in SURVIVORS:
            tl = u53.timeline(logs[sv][0], sv)
            out += tl
            for t in tl:
                m = re.search(r"timeline \+\s*([\d.]+)s\s+on_gameover outcome=(\d+)", t)
                if m and m.group(2) == "8":
                    found = True
                    secs = float(m.group(1))
                    if not 30.0 <= secs <= 90.0:
                        bad.append(
                            "%s reached outcome 8 at +%.1fs (expected ~58 s, window 30..90)"
                            % (sv, secs)
                        )
        if not found:
            bad.append("no survivor reached outcome 8 (U55's timeline did not reproduce)")
        return (not bad), out + ["FAIL: " + b for b in bad]

    if not handed:
        bad.append("the leaver never logged `hub-leave -- HANDED OVER`")
    else:
        out.append("leaver: " + handed[0].strip()[-150:])
    hubs = {c[1] for c in changes.values() if c}
    if len(hubs) != 1 or any(c is None for c in changes.values()):
        bad.append("survivors do not agree on one hub change: %r" % (changes,))
    else:
        out.append("both survivors report the hub change to player %s" % next(iter(hubs)))
    for sv in SURVIVORS:
        if last[sv] - leave_step < min_post:
            bad.append(
                "%s stepped only %d past the leaver (< %d)" % (sv, last[sv] - leave_step, min_post)
            )
    a, b = SURVIVORS
    txt = u53.analyze(os.path.join(det_dir, a), os.path.join(det_dir, b), min_post)
    m = re.search(r"combined-hash=(\d+)", txt)
    common = int(m.group(1)) if m else 0
    ident = "ALL PAIRS IDENTICAL" in txt
    out.append(
        "survivors %s vs %s: %s, %d common hashed steps (leaver stopped at %d)"
        % (a, b, "ALL PAIRS IDENTICAL" if ident else "NOT IDENTICAL", common, leave_step)
    )
    if not ident:
        bad.append("survivors are not ALL PAIRS IDENTICAL")
        out += ["    " + ln for ln in txt.strip().splitlines()[-8:]]
    elif common < leave_step + min_post:
        bad.append(
            "only %d common steps (< %d = leaver step + %d)"
            % (common, leave_step + min_post, min_post)
        )
    lo = int(max(0.0, leave_clock - 1.0) * 1000)
    hi = int((leave_clock + 10.0) * 1000)
    for sv in SURVIVORS:
        gap, n = stall_window(os.path.join(det_dir, sv, "mh_lockstep.log"), lo, hi)
        out.append(
            "%s longest game-clock stall in [%.1fs, %.1fs]: %d ms over %d rows"
            % (sv, lo / 1000.0, hi / 1000.0, gap, n)
        )
        if n == 0:
            bad.append("%s: no lockstep rows in the exit window (cannot measure the stall)" % sv)
        elif gap > STALL_MAX_MS:
            bad.append("%s stalled %d ms around the exit (> %d)" % (sv, gap, STALL_MAX_MS))
    return (not bad), out + ["FAIL: " + b for b in bad]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("det_dir")
    ap.add_argument("--negative", action="store_true")
    ap.add_argument("--exit-step", type=int, default=None)
    ap.add_argument("--min-post", type=int, default=MIN_POST)
    a = ap.parse_args()
    try:
        ok, lines = check(a.det_dir, a.negative, a.exit_step, a.min_post)
    except u53.Refusal as e:
        print("REFUSED: %s" % e)
        return 2
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
