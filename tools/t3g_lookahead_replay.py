#!/usr/bin/env python3
"""t3g_lookahead_replay -- mp:T3g's alternate-history replay of the adaptive lookahead controller.

T3e settled the DOWN-leg step-SIZE question from a change-only recomputation off the `; [adaptive]`
lines in mh_net.log (which only exist at decision points), but flagged that it could not answer the
fuller question -- "which windows shrink at all" -- because the credit-decay rule also changed, and a
change-only log cannot recover that. T3g's scope is to close that gap with a FULL replay driven by
mh_lockstep.log's own per-row columns (`late_tail95_ms`, `local_h_ms`, `committed_ms`, `srtt0_ms`,
`rttvar0_ms`, `late_n`), which -- unlike the decision-only adaptive lines -- are written on every row
mh_lockstep.log's own row-gate emits (a tracked-column change, or every 500 ms regardless), so they are
close to (not identical to) the controller's own per-window sampling cadence.

This file has TWO independent, hand-ported copies of `mh::netstats::lookahead_decide` (udp_stats.h):
  - `decide_current()` -- the T3c/T3e-era function (warm-up gate, unused-horizon/P10 slack term, the
    proportional shrink + decay-by-one credit, P12's uncapped first grow). Verbatim port as of this
    session's src/mh_dll/mh_net_udp/udp_stats.h.
  - `decide_old()` -- the function AT 9b0d758a (the commit immediately before T3c, `git show
    9b0d758a:src/mh_dll/mh_net_udp/udp_stats.h`): no warm-up, no slack term, flat 4% shrink, credit
    zeroed (not decayed) by any in-band window.
Both are driven off the SAME measured tail95/slack/link stream (one row at a time, in log order), so
the only thing that differs between the two replay lines is the DECISION RULE -- which is the question
T3g asks.

METHOD, and its honest limits:
  1. Replay `decide_current()` over the real run's own mh_lockstep.log rows -> a simulated move count
     and trajectory. VALIDATE it against the run's OWN real la_moves (mp_pacing_report.adaptive_moves,
     i.e. the actual `; [adaptive]` lines in mh_net.log) -- this is the check that the replay's cadence
     and input reconstruction are a good enough stand-in for the real controller's own per-window loop.
  2. Only if that validates (within 10%, per T3g's done_when) does the OLD-rule replay over the SAME
     input stream mean anything: it answers "what would 9b0d758a's controller have done, presented
     with what T3c's controller actually saw" -- properly a FULL alternate history now, because the OLD
     replay accrues its OWN credit counter under the OLD zero-on-band rule, instead of reusing T3c's.

  What is still an approximation, stated once here rather than hedged at every number: (a) slack_ms is
  the INSTANTANEOUS `local_h_ms - committed_ms` at each logged row, not the controller's own per-frame
  MEDIAN over its sampling window (mh_lockstep.log does not carry that window); (b) link_owd_ms is a
  per-row (SRTT + 4*RTTVAR)/2 off that row's own srtt0_ms/rttvar0_ms, not a windowed figure either;
  (c) the replay's decision cadence is mh_lockstep.log's row-gate cadence (on a tracked-column change,
  or every 500 ms), which need not be the exact cadence `adaptive_tick()` itself runs at. All three bias
  toward UNDER- rather than over-counting fine oscillation, since a coarser sample stream smooths spikes
  the real per-frame loop would see. The validation step is what catches whether that matters here.

Usage:
    python tools/t3g_lookahead_replay.py <run-dir> [<run-dir> ...]

A run-dir is what ui_test.py --determinism / net_shim.py leave in tmp/<...>/<peer>/ -- i.e. a folder
holding mh_lockstep.log and mh_net.log side by side (see mp_pacing_report.py's own run-dir contract).
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mp_pacing_report as mpr  # noqa: E402  (reuse adaptive_moves(), the ground truth)

# ---- constants, current (T3c/T3e) era -- src/mh_dll/mh_net_udp/udp_stats.h at the tree's HEAD -----
AD_LATE_GROW_MULT = 0.5
AD_LATE_SHRINK_MULT = 1.5
AD_LATE_GROW_STEP = 1.25
AD_LATE_GROW_MAX = 2.00
AD_LATE_SHRINK = 0.96
AD_LATE_SHRINK_AFTER = 3
AD_LATE_SHRINK_FRAC = 0.5
AD_LATE_SHRINK_MAX = 0.60
AD_FIRST_GROW_MIN_SAMPLES = 32
AD_SLACK_MULT = 0.5
AD_LATE_MIN_SAMPLES = 8
AD_SIM_FLOOR_MULT = 3.0
WARM_DELAY_MS = 1500  # mp:T3c's 1.5 s join-transient gate

# adaptive_tick's own OUTER decision-rate gate (net_lockstep.cpp) -- decide at most once per
# AD_WINDOW_MS, or once per AD_FAST_MS when the window is already reading a clear GROW with a full
# tail behind it. UNCHANGED by T3c (verified: `git show 9b0d758a:...net_lockstep.cpp` has the
# identical AD_WINDOW_MS/AD_FAST_MS/urgent gate) -- so this cadence is shared ground truth for BOTH
# replay lines, not a T3c-vs-9b0d758a variable. Missing this gate is what made the first cut of this
# script replay ~2000-3900 "decisions" against a real la_moves of 13-33: mh_lockstep.log's row-gate
# fires roughly every 20-40 ms during a match (whenever the monotone-pinned STEP_SIZE ticks, which is
# every advancing frame, mp:D30) while the controller itself only re-judges every 0.5-2 s -- most rows
# carry a late_tail95_ms IDENTICAL to several rows before them, and re-deciding on a stale reading
# manufactured runaway shrink chains straight to the floor.
AD_WINDOW_MS = 2000
AD_FAST_MS = 500
AD_FAST_MIN_SAMPLES = 32  # == AD_FIRST_GROW_MIN_SAMPLES (net_lockstep.cpp static_asserts this)

LA_NO_SAMPLES, LA_HOLD, LA_GROW, LA_SHRINK, LA_WARMUP = (
    "no_samples",
    "hold",
    "grow",
    "shrink",
    "warmup",
)


class St:
    """Mutable per-replay state threaded across rows -- the thing a pure decide() cannot own itself."""

    def __init__(self, cur_ms):
        self.cur_ms = cur_ms
        self.clean = 0
        self.first_measure_wall = None
        self.pending_first_warm = False


def decide_current(
    cur_ms,
    sim_ms,
    floor_ms,
    ceil_ms,
    have,
    tail95_ms,
    clean_in,
    warm,
    slack_ms,
    link_owd_ms,
    samples,
    first_warm,
):
    want, clean_out, verdict, first_full = cur_ms, clean_in, LA_NO_SAMPLES, False
    if not warm:
        clean_out, verdict = 0, LA_WARMUP
    elif have and sim_ms > 0.0 and cur_ms > 0.0:
        grow_at, shrink_at = AD_LATE_GROW_MULT * sim_ms, AD_LATE_SHRINK_MULT * sim_ms
        have_owd = link_owd_ms >= 0.0
        idle_ms = slack_ms
        if have_owd:
            idle_ms = max(0.0, slack_ms - link_owd_ms)
        slack = idle_ms > AD_SLACK_MULT * sim_ms
        if tail95_ms < grow_at and not (slack and have_owd):
            w = cur_ms + (grow_at - tail95_ms)
            if w < cur_ms * AD_LATE_GROW_STEP:
                w = cur_ms * AD_LATE_GROW_STEP
            full = first_warm and samples >= AD_FIRST_GROW_MIN_SAMPLES
            first_full = full and w > cur_ms * AD_LATE_GROW_MAX
            if not full and w > cur_ms * AD_LATE_GROW_MAX:
                w = cur_ms * AD_LATE_GROW_MAX
            want, clean_out, verdict = w, 0, LA_GROW
        elif tail95_ms > shrink_at or slack:
            clean_out = clean_in + 1
            if clean_out >= AD_LATE_SHRINK_AFTER:
                w = cur_ms * AD_LATE_SHRINK
                if tail95_ms > shrink_at:
                    give = AD_LATE_SHRINK_FRAC * (tail95_ms - shrink_at)
                    w = min(w, cur_ms - give)
                if slack:
                    give = AD_LATE_SHRINK_FRAC * idle_ms
                    w = min(w, cur_ms - give)
                w = max(w, cur_ms * AD_LATE_SHRINK_MAX)
                want, verdict = w, LA_SHRINK
            else:
                verdict = LA_HOLD
        else:
            clean_out = max(0, clean_in - 1)  # T3c: decay by one, not zero
            verdict = LA_HOLD
    want = max(want, floor_ms)
    want = min(want, ceil_ms)
    return want, clean_out, verdict, first_full


def decide_old(cur_ms, sim_ms, floor_ms, ceil_ms, have, tail95_ms, clean_in):
    """9b0d758a (pre-T3c): no warm-up, no slack term, flat shrink, credit zeroed on any in-band read."""
    want, clean_out, verdict = cur_ms, clean_in, LA_NO_SAMPLES
    if have and sim_ms > 0.0 and cur_ms > 0.0:
        grow_at, shrink_at = AD_LATE_GROW_MULT * sim_ms, AD_LATE_SHRINK_MULT * sim_ms
        if tail95_ms < grow_at:
            w = cur_ms + (grow_at - tail95_ms)
            if w < cur_ms * AD_LATE_GROW_STEP:
                w = cur_ms * AD_LATE_GROW_STEP
            if w > cur_ms * AD_LATE_GROW_MAX:
                w = cur_ms * AD_LATE_GROW_MAX
            want, clean_out, verdict = w, 0, LA_GROW
        elif tail95_ms > shrink_at:
            clean_out = clean_in + 1
            if clean_out >= AD_LATE_SHRINK_AFTER:
                want, verdict = cur_ms * AD_LATE_SHRINK, LA_SHRINK
            else:
                verdict = LA_HOLD
        else:
            clean_out, verdict = 0, LA_HOLD  # pre-T3c: zeroed, not decayed
    want = max(want, floor_ms)
    want = min(want, ceil_ms)
    return want, clean_out, verdict


NA = {"n/a", "n/a\n"}


def fnum(x):
    return None if x in NA else float(x)


def read_rows(run_dir):
    rows, names = mpr.read_lockstep(os.path.join(run_dir, "mh_lockstep.log"))
    return rows


def band_ms(run_dir):
    """adaptive=1 [lo..hi ms], sim_step=NN ms -- off the seam's own armed line, same source
    mp_pacing_report.adaptive_floor_ms reads. Returns (sim_ms, floor_ms, ceil_ms) or None."""
    net = os.path.join(run_dir, "mh_net.log")
    if not os.path.isfile(net):
        return None
    band_re = re.compile(r"adaptive=\d+\s*\[\s*(\d+(?:\.\d+)?)\s*\.\.\s*(\d+(?:\.\d+)?)\s*ms\s*\]")
    step_re = re.compile(r"sim_step=(\d+(?:\.\d+)?)")
    with open(net, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if "time_tick hook armed" not in line:
                continue
            bm, sm = band_re.search(line), step_re.search(line)
            if bm and sm:
                lo, hi, sim = float(bm.group(1)), float(bm.group(2)), float(sm.group(1))
                return sim, max(lo, AD_SIM_FLOOR_MULT * sim), hi
    return None


def replay(run_dir):
    rows = read_rows(run_dir)
    bm = band_ms(run_dir)
    if not rows or not bm:
        return None
    sim_ms, floor_ms, ceil_ms = bm
    cur0 = float(rows[0]["step_ms"])
    cs = St(cur0)  # current-rule replay state
    os_ = St(cur0)  # old-rule replay state
    cur_moves, old_moves = [], []
    t0 = None  # adaptive_tick's own g_ad_t0 -- SHARED gate (measurement-driven, not rule-driven)
    for r in rows:
        late_n = fnum(r.get("late_n"))
        tail95 = fnum(r.get("late_tail95_ms"))
        srtt0 = fnum(r.get("srtt0_ms"))
        rttvar0 = fnum(r.get("rttvar0_ms"))
        local_h = fnum(r.get("local_h_ms"))
        committed = fnum(r.get("committed_ms"))
        wall = fnum(r.get("wall_ms"))
        if wall is None:
            continue
        have = late_n is not None and late_n >= AD_LATE_MIN_SAMPLES and tail95 is not None
        if t0 is None:
            t0 = wall  # g_ad_t0 == 0 -> set to `now` on the first-ever attempted tick
            continue  # ...and that same call returns without deciding (now - t0 == 0 < due)
        urgent = have and late_n >= AD_FAST_MIN_SAMPLES and tail95 < AD_LATE_GROW_MULT * sim_ms
        due = AD_FAST_MS if urgent else AD_WINDOW_MS
        if (wall - t0) < due:
            continue  # adaptive_tick's own early return -- no decision attempt at all this row
        t0 = wall  # the gate opened: both replay lines judge THIS row, whatever they decide

        if have and cs.first_measure_wall is None:
            cs.first_measure_wall = wall
            cs.pending_first_warm = True
        warm = (
            have
            and cs.first_measure_wall is not None
            and (wall - cs.first_measure_wall) >= WARM_DELAY_MS
        )
        slack_ms = max(0.0, (local_h or 0.0) - (committed or 0.0))
        link_owd = -1.0 if srtt0 is None or rttvar0 is None else (srtt0 + 4.0 * rttvar0) / 2.0
        samples = int(late_n) if late_n is not None else 0

        w, clean, verdict, first_full = decide_current(
            cs.cur_ms,
            sim_ms,
            floor_ms,
            ceil_ms,
            have,
            tail95 or 0.0,
            cs.clean,
            warm,
            slack_ms,
            link_owd,
            samples,
            cs.pending_first_warm,
        )
        if verdict not in (LA_NO_SAMPLES, LA_WARMUP):
            cs.pending_first_warm = False
        if verdict in (LA_GROW, LA_SHRINK):
            cur_moves.append((wall, cs.cur_ms, w, verdict))
        cs.cur_ms, cs.clean = w, clean

        w2, clean2, verdict2 = decide_old(
            os_.cur_ms, sim_ms, floor_ms, ceil_ms, have, tail95 or 0.0, os_.clean
        )
        if verdict2 in (LA_GROW, LA_SHRINK):
            old_moves.append((wall, os_.cur_ms, w2, verdict2))
        os_.cur_ms, os_.clean = w2, clean2

    return {
        "rows": len(rows),
        "sim_ms": sim_ms,
        "floor_ms": floor_ms,
        "ceil_ms": ceil_ms,
        "cur_moves": cur_moves,
        "old_moves": old_moves,
        "cur_final": cs.cur_ms,
        "old_final": os_.cur_ms,
    }


def summarize(run_dir):
    real = mpr.adaptive_moves(run_dir)
    real_n = len(real)
    real_shrink = sum(1 for m in real if m["verdict"] == "shrink")
    real_grow = sum(1 for m in real if m["verdict"] == "grow")
    rep = replay(run_dir)
    if rep is None:
        print("%s: no lockstep rows or no adaptive band line -- skipped" % run_dir)
        return
    sim_n = len(rep["cur_moves"])
    sim_shrink = sum(1 for m in rep["cur_moves"] if m[3] == LA_SHRINK)
    sim_grow = sum(1 for m in rep["cur_moves"] if m[3] == LA_GROW)
    old_n = len(rep["old_moves"])
    old_shrink = sum(1 for m in rep["old_moves"] if m[3] == LA_SHRINK)
    old_grow = sum(1 for m in rep["old_moves"] if m[3] == LA_GROW)
    pct = (abs(sim_n - real_n) / real_n * 100.0) if real_n else float("nan")
    print(
        "== %s (rows=%d sim_step=%g floor=%g ceil=%g) =="
        % (run_dir, rep["rows"], rep["sim_ms"], rep["floor_ms"], rep["ceil_ms"])
    )
    print(
        "  real  (mh_net.log ; [adaptive] lines): la_moves=%d  grow=%d shrink=%d"
        % (real_n, real_grow, real_shrink)
    )
    print(
        "  sim   (decide_current over mh_lockstep.log): la_moves=%d  grow=%d shrink=%d  "
        "(%.1f%% off real, final=%.0f ms)" % (sim_n, sim_grow, sim_shrink, pct, rep["cur_final"])
    )
    print(
        "  old   (decide_old, SAME input stream):        la_moves=%d  grow=%d shrink=%d  "
        "(final=%.0f ms)" % (old_n, old_grow, old_shrink, rep["old_final"])
    )
    print("  validated within 10%%: %s" % ("YES" if pct <= 10.0 else "NO"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dirs", nargs="+")
    a = ap.parse_args()
    for d in a.run_dirs:
        summarize(d)


if __name__ == "__main__":
    main()
