#!/usr/bin/env python3
"""
tools/check_mv_marshal.py -- mods:M1N field fix (user playtest rc10, 2026-10-16): a unit the wave planner stalled in the
group-marshal state (0x0a, no path) next to a friend must get past it.

The three `m1n_marshal_*` UI rows replay the user's two orders on the corridor save 7-abigan-5 (planet 7, a one-tile corridor
at x=14, unit 12 walking south into it, unit 1 standing idle at (14,106)) through the harness's mv_replay probe, and this checker
reads the verdict the probe and the seams wrote -- `; MV ORDERSUM` lines in mh_match_harness.log and `; M1N ...` lines in
mh_net.log of the session directory. Three arms:

  --case idle     move_nudge_idle=1 (the ship default under improved_movement): unit 12's order must ARRIVE (arrived=1,
                  gave_up=0, stalls=0) and the log must carry a `; M1N swap-marshal` line naming units 12 and 1 -- the corridor
                  has no sidestep, so the idle friend and the mover trade tiles.
  --case headon   move_nudge_idle=0, move_swap_headon=1: unit 12 stalls at the idle friend (retail), then unit 1 is ordered
                  north through it: both are marshal-stuck head-on. BOTH orders must arrive (gave_up=0) and the log must carry
                  `; M1N swap-marshal ... unit 1 at 14,106 / unit 12 at 14,105`.
  --case off      both keys 0 (retail): unit 12 and unit 1 each stall and give up (gave_up=1, stalls>=1); no M1N marshal line.

  --case corner   (mods:M1N follow-up 2, save 7-abigan-5-1, rc11 "more like a corner"): unit 1 is ordered to (7,93) and its only
                  exit is the x=14 corridor where unit 12 stands idle. move_route_through_idle=1: the order must ARRIVE
                  (arrived=1, gave_up=0) and the log must carry a `; M1N route-through` line (the planner opened an idle
                  friend's tile: the unit squeezes diagonally past unit 12, which stays where it is).
  --case corridor the m1n_marshal_idle order (unit 12 south through the one-tile corridor past idle unit 1 at (14,106)) with the
                  DEFAULT keys, so the route planner routes straight through the idle friend: the order must ARRIVE with no
                  stall, the log must carry `; M1N route-through` and a `; M1N swap-trade` line naming units 12 and 1 (they
                  trade tiles, as the marshal site does), and NO `; M1N nudge` line moving unit 1 straight ahead.
  --case cornoff  move_route_through_idle=0 (retail planner): the same order does NOT arrive and the log has no route-through line.

Every arm also needs the probe's `; MV DONE` line (a run that never reached it proves nothing).

  python tools/check_mv_marshal.py --case idle|headon|off <session-run-dir>
  python tools/check_mv_marshal.py --selftest        planted logs; every negative RED
"""

import argparse
import os
import re
import shutil
import sys
import tempfile

ORDERSUM = re.compile(
    r"; MV ORDERSUM ord=(\d+) .*? arrived=(\d+) .*? gave_up=(\d+) .*? stalls=(\d+)"
)
SWAP = re.compile(r"; M1N swap-marshal p\d+ unit (\d+) at (\d+),(\d+) / unit (\d+) at (\d+),(\d+)")
ANY_MARSHAL = re.compile(r"; M1N (?:swap|nudge)-marshal ")
ROUTE = re.compile(r"; M1N route-through p\d+ idle friends opened=(\d+)")
TRADE = re.compile(r"; M1N swap-trade p\d+ unit (\d+) at \d+,\d+ / unit (\d+) at \d+,\d+")
PUSH = re.compile(r"; M1N nudge p\d+ unit (\d+) at \d+,\d+ / unit (\d+) at (\d+),(\d+)")


def read(path):
    try:
        return open(path, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        return []


def check(run_dir, case):
    fails = []
    h = read(os.path.join(run_dir, "mh_match_harness.log"))
    n = read(os.path.join(run_dir, "mh_net.log"))
    if not any("; MV DONE" in ln for ln in h):
        fails.append(
            "clause 0: no `; MV DONE` line in mh_match_harness.log -- the replay probe never finished"
        )
    orders = {}
    for ln in h:
        m = ORDERSUM.search(ln)
        if m:
            orders[int(m.group(1))] = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
    swaps = [m.groups() for m in (SWAP.search(ln) for ln in n) if m]
    marshal_lines = [ln for ln in n if ANY_MARSHAL.search(ln)]
    if case in ("corner", "cornoff"):
        return check_corner(orders, n, fails, case)
    if case == "corridor":
        return check_corridor(orders, n, fails)
    want = {"idle": [0], "headon": [0, 1], "off": [0, 1]}[case]
    for o in want:
        if o not in orders:
            fails.append(
                "clause 1: no `; MV ORDERSUM ord=%d` line (the order was never issued or never summarised)"
                % o
            )
    if case in ("idle", "headon"):
        for o in want:
            if o in orders:
                arrived, gave_up, stalls = orders[o]
                if arrived != 1 or gave_up != 0:
                    fails.append(
                        "clause 2: order %d arrived=%d gave_up=%d -- the unit did not get past its friend"
                        % (o, arrived, gave_up)
                    )
        if case == "idle" and 0 in orders and orders[0][2] != 0:
            fails.append(
                "clause 3: order 0 stalls=%d -- the mover waited instead of trading tiles at once"
                % orders[0][2]
            )
        if case == "idle":
            if not any({int(g[0]), int(g[3])} == {12, 1} for g in swaps):
                fails.append(
                    "clause 4: no `; M1N swap-marshal` line naming units 12 and 1 in mh_net.log -- the fix did not run"
                )
        else:
            if not any(
                int(g[0]) == 1
                and (int(g[1]), int(g[2])) == (14, 106)
                and int(g[3]) == 12
                and (int(g[4]), int(g[5])) == (14, 105)
                for g in swaps
            ):
                fails.append(
                    "clause 4: no `; M1N swap-marshal ... unit 1 at 14,106 / unit 12 at 14,105` (the head-on swap) in mh_net.log"
                )
    else:
        for o in want:
            if o in orders:
                arrived, gave_up, stalls = orders[o]
                if arrived != 0 or gave_up != 1 or stalls < 1:
                    fails.append(
                        "clause 2: order %d arrived=%d gave_up=%d stalls=%d -- retail must stall and give up with the keys at 0"
                        % (o, arrived, gave_up, stalls)
                    )
        if marshal_lines:
            fails.append(
                "clause 3: %d `; M1N *-marshal` line(s) with the keys at 0 -- the splice acted while disabled"
                % len(marshal_lines)
            )
    return fails


def check_corner(orders, n, fails, case):
    routes = [int(m.group(1)) for m in (ROUTE.search(ln) for ln in n) if m]
    if 0 not in orders:
        fails.append(
            "clause 1: no `; MV ORDERSUM ord=0` line (the order was never issued or never summarised)"
        )
        return fails
    arrived, gave_up, stalls = orders[0]
    if case == "corner":
        if arrived != 1 or gave_up != 0:
            fails.append(
                "clause 2: order 0 arrived=%d gave_up=%d -- the unit did not get out of the corner"
                % (arrived, gave_up)
            )
        if not routes:
            fails.append(
                "clause 3: no `; M1N route-through` line in mh_net.log -- the planner never opened a friend's tile"
            )
    else:
        if arrived != 0:
            fails.append(
                "clause 2: order 0 arrived=1 with the key at 0 -- the control no longer reproduces the corner"
            )
        if routes:
            fails.append(
                "clause 3: %d `; M1N route-through` line(s) with the key at 0 -- the splice acted while disabled"
                % len(routes)
            )
    return fails


def check_corridor(orders, n, fails):
    routes = [ln for ln in n if ROUTE.search(ln)]
    trades = [m.groups() for m in (TRADE.search(ln) for ln in n) if m]
    pushes = [m.groups() for m in (PUSH.search(ln) for ln in n) if m]
    if 0 not in orders:
        fails.append(
            "clause 1: no `; MV ORDERSUM ord=0` line (the order was never issued or never summarised)"
        )
        return fails
    arrived, gave_up, stalls = orders[0]
    if arrived != 1 or gave_up != 0 or stalls != 0:
        fails.append(
            "clause 2: order 0 arrived=%d gave_up=%d stalls=%d -- the unit did not get through the corridor cleanly"
            % (arrived, gave_up, stalls)
        )
    if not routes:
        fails.append(
            "clause 3: no `; M1N route-through` line in mh_net.log -- the planner never opened a friend's tile"
        )
    if not any({int(g[0]), int(g[1])} == {12, 1} for g in trades):
        fails.append(
            "clause 4: no `; M1N swap-trade` line naming units 12 and 1 in mh_net.log -- the friend was not traded with"
        )
    if pushes:
        fails.append(
            "clause 5: %d `; M1N nudge` line(s): the idle friend was pushed ahead instead of trading tiles"
            % len(pushes)
        )
    return fails


def selftest():
    sums = {
        "good_idle": [
            "; MV ORDERSUM ord=0 goal=14,111 units=1 arrived=1 near=0 stopped_short=0 gave_up=0 interrupted=0 dead=0 t_first_arrive=672 t_last_settle=671 max_spread=0 stalls=0 (x)"
        ],
        "stall": [
            "; MV ORDERSUM ord=0 goal=14,111 units=1 arrived=0 near=0 stopped_short=0 gave_up=1 interrupted=0 dead=0 t_first_arrive=-1 t_last_settle=756 max_spread=0 stalls=1 (x)"
        ],
    }
    good_h = ["; MV DONE step=832 rel=832"] + sums["good_idle"]
    swap12 = "[22:25:15.059] ; M1N swap-marshal p0 unit 12 at 14,105 / unit 1 at 14,106 (1)"
    swap_head = "[22:28:44.594] ; M1N swap-marshal p0 unit 1 at 14,106 / unit 12 at 14,105 (1)"
    both = [
        "; MV DONE step=932 rel=932",
        "; MV ORDERSUM ord=1 goal=14,100 units=1 arrived=1 near=0 stopped_short=0 gave_up=0 interrupted=0 dead=0 t_first_arrive=217 t_last_settle=216 max_spread=0 stalls=0 (x)",
        "; MV ORDERSUM ord=0 goal=14,111 units=1 arrived=1 near=0 stopped_short=0 gave_up=0 interrupted=0 dead=0 t_first_arrive=772 t_last_settle=771 max_spread=0 stalls=1 (x)",
    ]
    off_h = [
        "; MV DONE step=1001 rel=1001",
        sums["stall"][0],
        sums["stall"][0].replace("ord=0", "ord=1"),
    ]
    corner_ok = [
        "; MV DONE step=900 rel=900",
        "; MV ORDERSUM ord=0 goal=7,93 units=1 arrived=1 near=0 stopped_short=0 gave_up=0 interrupted=0 dead=0 t_first_arrive=700 t_last_settle=699 max_spread=0 stalls=0 (x)",
    ]
    corner_stall = [
        "; MV DONE step=900 rel=900",
        "; MV ORDERSUM ord=0 goal=7,93 units=1 arrived=0 near=0 stopped_short=0 gave_up=1 interrupted=0 dead=0 t_first_arrive=-1 t_last_settle=500 max_spread=0 stalls=2 (x)",
    ]
    corner_net = [
        "; M1N route-through p0 idle friends opened=1 goal=7,93 (1)",
    ]
    corr_net = [
        "; M1N route-through p0 idle friends opened=2 goal=14,111 (1)",
        "; M1N swap-trade p0 unit 12 at 14,105 / unit 1 at 14,106 (1)",
    ]
    corr_push = ["; M1N nudge p0 unit 12 at 14,105 / unit 1 at 14,107 (1)"]
    cases = [
        ("idle ok", "idle", good_h, [swap12], False),
        (
            "idle: stalled (retail)",
            "idle",
            ["; MV DONE step=1 rel=1"] + sums["stall"],
            [swap12],
            True,
        ),
        ("idle: no swap line", "idle", good_h, [], True),
        (
            "idle: wrong units in swap line",
            "idle",
            good_h,
            ["; M1N swap-marshal p0 unit 3 at 1,1 / unit 4 at 1,2 (1)"],
            True,
        ),
        ("idle: probe never finished", "idle", good_h[1:], [swap12], True),
        (
            "idle: arrived but stalled",
            "idle",
            ["; MV DONE step=1 rel=1", sums["good_idle"][0].replace("stalls=0", "stalls=2")],
            [swap12],
            True,
        ),
        ("headon ok", "headon", both, [swap_head], False),
        ("headon: first order only", "headon", both[:1] + both[2:], [swap_head], True),
        ("headon: wrong swap pair", "headon", both, [swap12], True),
        (
            "headon: one gave up",
            "headon",
            [
                both[0],
                both[1],
                both[2].replace("gave_up=0", "gave_up=1").replace("arrived=1", "arrived=0"),
            ],
            [swap_head],
            True,
        ),
        ("off ok", "off", off_h, [], False),
        (
            "off: it arrived (fix leaking)",
            "off",
            [
                "; MV DONE step=1 rel=1",
                sums["good_idle"][0],
                sums["good_idle"][0].replace("ord=0", "ord=1"),
            ],
            [],
            True,
        ),
        ("off: marshal line present", "off", off_h, [swap12], True),
        ("corridor ok", "corridor", good_h, corr_net, False),
        ("corridor: no trade", "corridor", good_h, corr_net[:1], True),
        ("corridor: pushed ahead", "corridor", good_h, corr_net + corr_push, True),
        (
            "corridor: stalled",
            "corridor",
            ["; MV DONE step=1 rel=1"] + sums["stall"],
            corr_net,
            True,
        ),
        ("corridor: no route-through", "corridor", good_h, corr_net[1:], True),
        ("corner ok", "corner", corner_ok, corner_net, False),
        ("corner: gave up", "corner", corner_stall, corner_net, True),
        ("corner: no route-through line", "corner", corner_ok, corner_net[1:], True),
        ("cornoff ok", "cornoff", corner_stall, [], False),
        ("cornoff: it arrived", "cornoff", corner_ok, [], True),
        ("cornoff: splice acted", "cornoff", corner_stall, corner_net, True),
    ]
    bad = 0
    tmp = tempfile.mkdtemp(prefix="chk_mv_marshal_")
    try:
        for name, case, hl, nl, expect_red in cases:
            d = os.path.join(tmp, re.sub(r"\W+", "_", name))
            os.makedirs(d)
            open(os.path.join(d, "mh_match_harness.log"), "w").write("\n".join(hl) + "\n")
            open(os.path.join(d, "mh_net.log"), "w").write("\n".join(nl) + "\n")
            fails = check(d, case)
            ok = bool(fails) == expect_red
            print(
                "  %-40s %s%s"
                % (name, "ok" if ok else "WRONG", "" if not fails else "  [" + fails[0][:60] + "]")
            )
            bad += 0 if ok else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("check_mv_marshal selftest: %d/%d" % (len(cases) - bad, len(cases)))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--case", choices=["idle", "headon", "off", "corner", "cornoff", "corridor"])
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("run_dir", nargs="?")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.case or not a.run_dir:
        ap.error(
            "usage: check_mv_marshal.py --case idle|headon|off|corner|cornoff|corridor <session-run-dir>"
        )
    fails = check(a.run_dir, a.case)
    if fails:
        for f in fails:
            print("FAIL " + f)
        print("check_mv_marshal: FAIL (%d clause(s))" % len(fails))
        return 1
    print("check_mv_marshal: PASS (%s)" % a.case)
    return 0


if __name__ == "__main__":
    sys.exit(main())
