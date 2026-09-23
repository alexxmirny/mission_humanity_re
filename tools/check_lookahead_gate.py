#!/usr/bin/env python3
"""check_lookahead_gate -- did the mp:P10 grow gate actually run in this peer's logs?

WHY THIS EXISTS. The gate is a decision inside mh.dll, and NOTHING a run reports says which mh.dll
it ran: `mp_pacing_report`'s `armed` line reads the peer's own seam log and so reports the INI it
armed, which is identical either way. On 2026-09-22 that cost two rig runs -- the source was
restored after a deliberate baseline build and the rebuild was skipped, so two runs labelled as the
fix were the pre-fix binary, and the only reason it surfaced was an adaptive line that grew where
the gate should have refused. This makes that read mechanical.

THE PREDICATE, from the `; [adaptive]` line alone (tools/data/log_formats.json
`net.adaptive_lookahead`, which carries `slack` and, since mp:P10, `owd`):

    a window with  tail95 < AD_LATE_GROW_MULT * sim_step   (the grow arm is entered)
              and  slack - owd > AD_SLACK_MULT * sim_step  (the gate's own condition holds)
              and  owd >= 0                                (the link was measured)
    must NOT have verdict `grow`.

A single such line is proof the gate was ABSENT -- it is not a threshold or a heuristic, it is the
condition the code tests, re-evaluated off the numbers the code printed. The converse is weaker and
is reported rather than asserted: a run with no such window never put the gate in a position to act,
which is the ordinary case on a LAN (slack never exceeds the delay) and is NOT evidence either way.

Run against a peer directory (holding mh_net.log) or any number of them.
"""

import argparse
import os
import re
import sys

# The two multipliers are mh::netstats::AD_LATE_GROW_MULT and AD_SLACK_MULT (udp_stats.h). They are
# duplicated here rather than parsed out of the header because this tool has to be able to judge a
# log captured by an OLDER build than the tree it is run from -- reading today's constants would
# silently re-interpret yesterday's run.
AD_LATE_GROW_MULT = 0.5
AD_SLACK_MULT = 0.5

RE_LINE = re.compile(
    r"lookahead (\d+) -> (\d+) ms (\w+) \(late p50 (-?\d+) tail95 (-?\d+) tail99 (-?\d+) ms, "
    r"n=(\d+), peer=(-?\d+), slack (-?\d+) ms(?:, owd (-?\d+) ms)?"
)


def scan(run_dir, sim_ms):
    """-> (verdict, n_windows_gate_could_act, n_violations, [sample lines])"""
    path = os.path.join(run_dir, "mh_net.log")
    if not os.path.isfile(path):
        return ("NO LOG", 0, 0, [])
    grow_at = AD_LATE_GROW_MULT * sim_ms
    dead_band = AD_SLACK_MULT * sim_ms
    actionable = 0
    violations = []
    saw_owd = False
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if "; [adaptive] " not in line:
                continue
            m = RE_LINE.search(line)
            if not m:
                continue
            verdict = m.group(3)
            tail95 = int(m.group(5))
            slack = int(m.group(9))
            owd = m.group(10)
            if owd is None:
                continue  # pre-P10 log format: the line cannot answer the question
            saw_owd = True
            owd = int(owd)
            if owd < 0:
                continue  # the transport could not measure the link; the gate stands down by design
            if tail95 >= grow_at:
                continue  # not a grow window at all
            if (slack - owd) <= dead_band:
                continue  # the gate's condition did not hold; a grow here is correct
            actionable += 1
            if verdict == "grow":
                violations.append(line.strip())
    if not saw_owd:
        return ("PRE-P10 LOG (no owd column)", 0, 0, [])
    if violations:
        return ("GATE ABSENT", actionable, len(violations), violations[:3])
    if actionable == 0:
        return ("NOT EXERCISED", 0, 0, [])
    return ("GATE PRESENT", actionable, 0, [])


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("dirs", nargs="+", help="peer run directories (each holding mh_net.log)")
    ap.add_argument(
        "--sim-step-ms",
        type=float,
        default=20.0,
        help="the run's sim_step_ms (the thresholds are relative to it); default 20",
    )
    ap.add_argument(
        "--require-exercised",
        action="store_true",
        help="also fail when no window ever put the gate in a position to act -- use it "
        "for a run that is SUPPOSED to exercise it (a slow link), never on a LAN",
    )
    a = ap.parse_args()
    bad = 0
    for d in a.dirs:
        verdict, actionable, nviol, samples = scan(d, a.sim_step_ms)
        print("%-46s %-22s windows=%d violations=%d" % (d, verdict, actionable, nviol))
        for s in samples:
            print("      " + s)
        if verdict == "GATE ABSENT":
            bad += 1
        elif a.require_exercised and verdict != "GATE PRESENT":
            bad += 1
    if bad:
        print("check_lookahead_gate: FAIL (%d dir(s))" % bad)
        return 1
    print("check_lookahead_gate: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
