#!/usr/bin/env python3
"""
tools/check_m1h.py -- mods:M1H: the movement preset's fixtures, judged from the logs of ONE solo process.

The four solo rows (tools/uiscripts/registry.yaml) run the same fixtures in four configurations and call this checker once per step:

  m1h_ship  libmh.dll absent (the shipped configuration). Every key rides its RETAIL SPLICE in mh.dll.
  m1h_orig  libmh absent + [config] mode=original (rollback_original.ini). Same carriers as ship.
  m1h_lib   libmh bound (mv_waves=0, so the pathfind rebind is armed). The libmh TWINS carry the keys.
  m1h_off   the three masters 0, libmh absent. The retail outcome: no key reported, no hit counter moves.

Each fixture (`--case`) names the outcome it must show, the hit counters its keys must move (splice counters `; move hits ...` on
ship/orig, twin counters `; move twin hits ...` on lib; the twins of squad merge / merge goal / attack heading / attack range /
projectile facing keep no counter, so on lib those are judged by outcome only) and the carrier text of every reported key.

The fixtures (the m1h_*.txt scripts; the harness probe that drives each is in the row's harness_extra):
  f1 corner      7-abigan-5-1 + m1h_f1.csv   route-through (3e-5): the idle friend in the x=14 corridor is opened, the order arrives
  f2 corridor    7-abigan-5   + m1h_f2.csv   route-through + the walker's swap-trade (nudge): arrives, no stall, no push ahead
  f3 head-on     7-abigan-5   + m1h_f3.csv   move_nudge_idle=0: the head-on swap at the marshal site (mk2), both orders arrive
  f4 kofu        5-kofu-c     + m1h_f4.csv   centroid wrap, corridor, route cost, distinct goals, near-enough, blocked, detour guard
  f5 nd 1+2      5-kofu-c, nd_case=12          nudge an idle blocker; head-on swap in a one-tile corridor
  f6 speed       ayy30-tests  + m1h_f6.csv   group speed (commit + walk): faster members carry the slowest member's scale
  f7 attack      2-almost     + m1h_f7.csv   attack_range_footprint (unit 5 fires from 62,3); d7 = improved_combat=0: attack-heading fix (59,5)
  f8 merge       nortus-1     + m1h_f8.csv   squad merge into an attacker, merge goal, projectile facing (f8x on lib: arf off, completeness only)
  f9 guards      5-kofu-c, nd_case=6, mv_oob_fill=1   the slot-0xff splice: with the guard the walker never writes the landing slot (own slot 255 on the relocated pool, else player+2 slot 55); retail does
  f10 big map    the lobby's 256x256 skirmish, pf probe, route_corridor_mode=2   exact flood (route_flood_exact) + corridor hits on a front that can lap the ring
  f12 wall       5-kofu-c, nd_case=8, nd_len=1  move_route_through_replanning (M1DT fix 2): W is ordered through the gap of a 17-tile wall that holds a friend waiting in the
                                       marshal state 0x0a; the planner opens its tile and W follows it through (6 tiles; retail walks round the wall: 18)
  f13 queue      5-kofu-c, nd_case=9, nd_len=1  move_replan_detour_cap + move_block_reset_moving (M1DT fix 4): eight units queue through that gap with the
                                       block wait cut to 450 ms and the route_through keys off; the block-wait replans are refused instead of going round the wall
  f11 column     5-kofu-c, nd_case=7    move_route_through_moving (3e-6): W is ordered through a column of two WALKING friends in a one-tile corridor; the planner opens
                                       their tiles and W follows them (arrives 3 steps behind the column's front unit; retail goes round the corridor walls: 53)
  d2 d5          dev arms: move_route_through_idle=0 (marshal-site trade, 3e-4); nd_case=3 + move_claim_leader=1
The 2-peer rows (--mp) read both peers' logs: the five nd cases (1, 2, 4, 5, 7) finished on both, equal ND VERDICT lines, equal seam lines,
each peer's carrier text and hit counters for its own configuration (ship, lib, or mixed = host shipped + client libmh).

The six m1n_* rows this replaced: m1n_marshal_idle -> d2 (arrives, stalls=0, swap-marshal naming 12 and 1); m1n_marshal_headon -> f3;
m1n_marshal_off -> f3 on m1h_off (both orders gave up, no `; M1N` line); m1n_route_corner -> f1; m1n_route_corner_off -> f1 on m1h_off
(order did not arrive, no route-through line); m1n_route_corridor -> f2 (arrive, route-through, swap-trade 12/1, no push ahead).

  python tools/check_m1h.py --case f1 --arm ship <session-run-dir>
  python tools/check_m1h.py --mp --arm ship|lib|mixed <host-session-dir> <client-session-dir>
  python tools/check_m1h.py --case f4 --arm lib --tag teq_f4 --vs ship <dir>   also demand a step-identical hash stream against the
                                                       stream an EARLIER step of the same row saved for the ship arm (m1h_twin_eq)
  python tools/check_m1h.py --hash-cmp f4 ship lib     first step at which two arms' saved hash streams differ (offline)
  python tools/check_m1h.py --selftest

Every run also saves the step -> hash stream under tmp/m1h/hash_<tag-or-case>__<arm>.json (tmp/ is not versioned).
"""

import argparse
import json
import os
import re
import shutil
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HASH_DIR = os.path.join(REPO, "tmp", "m1h")

ORDERSUM = re.compile(
    r"; MV ORDERSUM ord=(\d+) goal=(-?\d+),(-?\d+) units=(\d+) arrived=(\d+) near=(\d+) stopped_short=(\d+) gave_up=(\d+) interrupted=(\d+) "
    r"dead=(\d+) t_first_arrive=(-?\d+) t_last_settle=(-?\d+) max_spread=(\d+) stalls=(\d+)"
)
OS_FIELDS = "ord goal_x goal_y units arrived near stopped_short gave_up interrupted dead t_first t_last spread stalls".split()
HITS = re.compile(r"; move hits (.*)$")
TWIN = re.compile(r"; move twin hits (\w+)=(\d+)")
CARRIER = re.compile(r"; move carrier \[net\] (\w+)=(-?\d+): (.*)$")
MVC = re.compile(
    r"^; MV C step=(\d+) rel=(\d+) u=(\d+) xy=(\d+),(\d+) .*? st=0x([0-9a-f]+) .*? scale=(\d+)/(\d+)"
)
OOBFILL = re.compile(r"; MV OOBFILL player (\d+) slot (\d+) ")
OOBW = re.compile(r"; MV OOBW step=(\d+) player=(\d+) slot=(\d+)")
ND_V = re.compile(
    r"; ND VERDICT case=(\d+) k=(\d+) unit=(\d+) xy=(\d+),(\d+) goal=(-?\d+),(-?\d+) arrived_el=(-?\d+) tiles=(\d+) replans=(\d+) wait_steps=(\d+) max_retry=(\d+) state=0x([0-9a-f]+)"
)
ND_T = re.compile(r"; ND TOTAL case=(\d+) steps=(\d+)")
FC_T = re.compile(
    r"; FC TOTAL case=(\d+) steps=(\d+) n=(\d+) tiles=(\d+) straight=(\d+) moved=(\d+) maxtiles=(\d+) maxcheb=(\d+) dup=(\d+) maxratio=(\d+) onwall=(\d+) idle=(\d+) bw=(\d+) bh=(\d+) rigid=(\d+) lastmove=(\d+)"
)
FC_V = re.compile(
    r"; FC VERDICT case=(\d+) k=(\d+) unit=\d+ start=(\d+),(\d+) end=(\d+),(\d+) tiles=(\d+)"
)
DR_T = re.compile(
    r"; DR TOTAL steps=(-?\d+) n=(\d+) docked=(\d+) giveups=(\d+) closes=(\d+) firstdock=(-?\d+) lastdock=(-?\d+) eff=(-?\d+)"
)
DR_FIELDS = "steps n docked giveups closes firstdock lastdock eff".split()
FC_FIELDS = "case steps n tiles straight moved maxtiles maxcheb dup maxratio onwall idle bw bh rigid lastmove".split()
HASHLN = re.compile(r"^(\d+) ([0-9A-F]{16}) ([0-9A-F]{16}) ([0-9A-F]{16})\s*$")

# hit name -> the key whose carrier line must be there (and whose splice/twin it counts)
HIT_KEY = {
    "cent_x": "move_centroid_wrap_fix",
    "cent_y": "move_centroid_wrap_fix",
    "slot_x": "move_centroid_wrap_fix",
    "slot_y": "move_centroid_wrap_fix",
    "detour": "move_detour_slot_guard",
    "flood_q": "move_flood_queue_guard",
    "flood_c": "route_cost_mode",
    "corr": "route_corridor_mode",
    "flood_exact": "route_flood_exact",
    "arr_commit": "move_distinct_goals",
    "arr_retry": "move_near_enough",
    "merge_goal": "move_merge_goal_fix",
    "blocked": "move_block_wait_ms",
    "nudge": "move_nudge_idle|move_swap_headon",
    "mk": "move_nudge_idle|move_swap_headon",
    "mk2": "move_nudge_idle|move_swap_headon",
    "route_through": "move_route_through_idle",
    "route_moving": "move_route_through_moving",
    "route_replanning": "move_route_through_replanning",
    "replan_cap": "move_replan_detour_cap",
    "formation": "move_formation_click",
    "stream_heat": "move_stream_heat",
    "enter_queue": "move_enter_queue",
    "door_hold": "storage_door_hold",
    "speed_commit": "move_group_speed",
    "speed_walk": "move_group_speed",
    "squad_merge": "move_squad_merge_fix",
    "proj_facing": "projectile_facing_fix",
    "arf_issue": "attack_range_footprint",
    "arf_fire": "attack_range_footprint",
    "arf_issue_repos": "attack_range_footprint",
    "arf_issue_alt": "attack_range_footprint",
    "arf_jit_repos": "attack_range_footprint",
    "arf_jit_alt": "attack_range_footprint",
    "arf_jit_walk": "attack_range_footprint",
    "arf_walk": "attack_range_footprint",
    "arf_reaim": "attack_range_footprint",
    "arf_step": "attack_range_footprint",
}
# hits whose twin keeps NO counter (libmh side): judged by outcome on the lib arm
# proj_facing / arf_fire twins count since 2026-10-18 (twin_hit_c.h); arf_issue is arf_issue_repos/_alt there
NO_TWIN_COUNTER = {"merge_goal", "squad_merge", "arf_issue"}
# keys whose carrier line may legitimately say `retail splice` on the lib arm (a site libmh does not displace)
LIB_SPLICE_OK = {"attack_range_footprint", "projectile_facing_fix"}


def read(path):
    try:
        return open(path, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        return []


def facts(run_dir):
    h = read(os.path.join(run_dir, "mh_match_harness.log"))
    n = read(os.path.join(run_dir, "mh_net.log"))
    f = {"h": h, "n": n, "run_dir": run_dir}
    f["done"] = any("; MV DONE" in ln for ln in h)
    f["orders"] = {}
    for ln in h:
        m = ORDERSUM.search(ln)
        if m:
            f["orders"][int(m.group(1))] = dict(zip(OS_FIELDS, map(int, m.groups())))
    f["carrier"] = {}
    for ln in n:
        m = CARRIER.search(ln)
        if m:
            f["carrier"][m.group(1)] = (int(m.group(2)), m.group(3))
    splice = {}
    for ln in n:
        m = HITS.search(ln)
        if m:
            splice = {}
            for tok in m.group(1).split():
                k, _, v = tok.partition("=")
                if v.isdigit():
                    splice[k] = int(v)
    f["conflict"] = [ln for ln in n if "CARRIER-CONFLICT" in ln]
    twin = {}
    for ln in n:
        m = TWIN.search(ln)
        if m:
            twin[m.group(1)] = max(twin.get(m.group(1), 0), int(m.group(2)))
    f["splice"], f["twin"] = splice, twin
    f["m1n"] = [ln for ln in n if "; M1N" in ln]
    f["nd"] = []
    for ln in h:
        m = ND_V.search(ln)
        if m:
            f["nd"].append(
                ("V",) + tuple(int(x, 16) if i == 12 else int(x) for i, x in enumerate(m.groups()))
            )
        m = ND_T.search(ln)
        if m:
            f["nd"].append(("T",) + tuple(int(x) for x in m.groups()))
    f["fc"] = {}
    for ln in h:
        m = FC_T.search(ln)
        if m:
            f["fc"][int(m.group(1))] = dict(zip(FC_FIELDS, map(int, m.groups())))
    f["fcv"] = {}  # case -> [(start x, y, end x, y, tiles)] in k order
    for ln in h:
        m = FC_V.search(ln)
        if m:
            g = [int(x) for x in m.groups()]
            f["fcv"].setdefault(g[0], []).append(tuple(g[2:]))
    f["fcmap"] = (
        0,
        0,
    )  # the map size the probe ran on (the blob may straddle the seam: compare vectors modulo it)
    for ln in h:
        m = re.search(r"; FC SETUP .* map=(\d+)x(\d+) ", ln)
        if m:
            f["fcmap"] = (int(m.group(1)), int(m.group(2)))
    f["fcwall"] = -1  # F20: the wall column of the last FC SETUP
    for ln in h:
        m = re.search(r"; FC SETUP .* wall_x=(-?\d+)", ln)
        if m:
            f["fcwall"] = int(m.group(1))
    f["fcfail"] = [ln for ln in h if "; FC FAIL" in ln]
    f["dr"] = None  # F21: the door-queue probe's totals (mods:M1BE)
    for ln in h:
        m = DR_T.search(ln)
        if m:
            f["dr"] = dict(zip(DR_FIELDS, map(int, m.groups())))
    f["drfail"] = [ln for ln in h if "; DR FAIL" in ln]
    f["oobw"] = [tuple(int(x) for x in m.groups()) for m in (OOBW.search(ln) for ln in h) if m]
    f["mvc"] = []
    for ln in h:
        m = MVC.match(ln)
        if m:
            g = m.groups()
            f["mvc"].append(
                dict(
                    step=int(g[0]),
                    unit=int(g[2]),
                    x=int(g[3]),
                    y=int(g[4]),
                    st=int(g[5], 16),
                    scale=int(g[6]),
                    base=int(g[7]),
                )
            )
    f["oobfill"] = any("MV OOBFILL" in ln for ln in h)
    # U4g: the slot the fill was written to = where the retail slot-0xff index lands (own slot 255 on the relocated pool, else player+2 slot 55)
    f["oobsite"] = next(
        ((int(m.group(1)), int(m.group(2))) for m in (OOBFILL.search(ln) for ln in h) if m), None
    )
    return f


def hits_of(f, arm):
    """The hit counters this arm's carriers move: splice counters (ship/orig/off), twin counters (lib; plus any splice that still runs)."""
    if arm == "lib":
        d = dict(f["splice"])
        for k, v in f["twin"].items():
            d[k] = max(d.get(k, 0), v)
        return d
    return dict(f["splice"])


def save_hashes(run_dir, case, arm):
    out = {}
    for ln in read(os.path.join(run_dir, "mh_match_harness.log")):
        m = HASHLN.match(ln)
        if m:
            out[m.group(1)] = "".join(m.groups()[1:])
    os.makedirs(HASH_DIR, exist_ok=True)
    p = os.path.join(HASH_DIR, "hash_%s__%s.json" % (case, arm))
    try:
        json.dump(out, open(p, "w"))
    except OSError:
        pass
    return len(out)


def hash_cmp(case, a, b):
    def load(arm):
        return json.load(open(os.path.join(HASH_DIR, "hash_%s__%s.json" % (case, arm))))

    x, y = load(a), load(b)
    common = sorted(set(x) & set(y), key=int)
    for s in common:
        if x[s] != y[s]:
            print(
                "%s: %s vs %s first differ at step %s (%d common steps, %s has %d, %s has %d)"
                % (case, a, b, s, len(common), a, len(x), b, len(y))
            )
            return 1
    print(
        "%s: %s vs %s IDENTICAL over %d common steps (%s has %d, %s has %d)"
        % (case, a, b, len(common), a, len(x), b, len(y))
    )
    return 0


# ---- the cases ------------------------------------------------------------------------------------------------------------
def o(f, n):
    return f["orders"].get(n)


def pat_lines_n(f, pat):
    r = re.compile(pat)
    return [ln for ln in f["n"] if r.search(ln)]


def pat_lines(f, pat):
    r = re.compile(pat)
    return [ln for ln in f["m1n"] if r.search(ln)]


def swap_trade_pair(f, a, b):
    for ln in pat_lines(f, r"; M1N swap-trade p\d+ unit (\d+) at \d+,\d+ / unit (\d+) at"):
        m = re.search(r"unit (\d+) at \d+,\d+ / unit (\d+) at", ln)
        if {int(m.group(1)), int(m.group(2))} == {a, b}:
            return True
    return False


def swap_marshal(f, u1, p1, u2, p2):
    for ln in pat_lines(f, r"; M1N swap-marshal"):
        m = re.search(r"unit (\d+) at (\d+),(\d+) / unit (\d+) at (\d+),(\d+)", ln)
        if m and (
            int(m.group(1)),
            (int(m.group(2)), int(m.group(3))),
            int(m.group(4)),
            (int(m.group(5)), int(m.group(6))),
        ) == (u1, p1, u2, p2):
            return True
    return False


def swap_marshal_any(f, a, b):
    for ln in pat_lines(f, r"; M1N swap-marshal"):
        m = re.search(r"unit (\d+) at \d+,\d+ / unit (\d+) at", ln)
        if m and {int(m.group(1)), int(m.group(2))} == {a, b}:
            return True
    return False


def fired_from(f, unit, x, y):
    return any(
        r["unit"] == unit and r["st"] == 0x1C and (r["x"], r["y"]) == (x, y) for r in f["mvc"]
    )


def nd_verdict(f, case, k):
    for r in f["nd"]:
        if r[0] == "V" and r[1] == case and r[2] == k:
            return dict(
                zip(
                    "case k unit x y gx gy arrived tiles replans wait maxretry state".split(), r[1:]
                )
            )
    return None


def nd_total(f, case):
    return any(r[0] == "T" and r[1] == case for r in f["nd"])


# each case: (fixture kind, hits required on the preset arms, outcome fn(f) -> [fails], retail fn(f) -> [fails] for the off arm)
def out_f1(f):
    fails = []
    a = o(f, 0)
    if not a:
        return ["no ORDERSUM ord=0"]
    if a["arrived"] != 1 or a["gave_up"]:
        fails.append(
            "order 0 arrived=%d gave_up=%d: the unit did not get out of the corner"
            % (a["arrived"], a["gave_up"])
        )
    if not pat_lines(f, r"; M1N route-through"):
        fails.append(
            "no `; M1N route-through` line: the planner never opened the idle friend's tile"
        )
    return fails


def off_f1(f):
    a = o(f, 0)
    if not a:
        return ["no ORDERSUM ord=0"]
    return (
        []
        if a["arrived"] == 0
        else [
            "order 0 arrived with every key at retail: the corner control no longer reproduces the dead end"
        ]
    )


def out_f2(f):
    fails = []
    a = o(f, 0)
    if not a:
        return ["no ORDERSUM ord=0"]
    if a["arrived"] != 1 or a["gave_up"] or a["stalls"]:
        fails.append(
            "order 0 arrived=%d gave_up=%d stalls=%d: the mover did not get through the corridor cleanly"
            % (a["arrived"], a["gave_up"], a["stalls"])
        )
    if not pat_lines(f, r"; M1N route-through"):
        fails.append("no `; M1N route-through` line")
    if not swap_trade_pair(f, 12, 1):
        fails.append(
            "no `; M1N swap-trade` line naming units 12 and 1: the friend was not traded with"
        )
    if pat_lines(f, r"; M1N nudge p"):
        fails.append(
            "`; M1N nudge` line: the idle friend was pushed ahead instead of trading tiles"
        )
    return fails


def off_f2(f):
    a = o(f, 0)
    if not a:
        return ["no ORDERSUM ord=0"]
    return (
        []
        if (a["arrived"] == 0 and a["gave_up"] == 1)
        else [
            "order 0 arrived=%d gave_up=%d: retail must stall and give up"
            % (a["arrived"], a["gave_up"])
        ]
    )


def out_f3(f):
    fails = []
    for n in (0, 1):
        a = o(f, n)
        if not a:
            fails.append("no ORDERSUM ord=%d" % n)
        elif a["arrived"] != 1 or a["gave_up"]:
            fails.append("order %d arrived=%d gave_up=%d" % (n, a["arrived"], a["gave_up"]))
    if not swap_marshal(f, 1, (14, 106), 12, (14, 105)):
        fails.append(
            "no `; M1N swap-marshal ... unit 1 at 14,106 / unit 12 at 14,105` (the head-on swap at the marshal site)"
        )
    return fails


def off_f3(f):
    fails = []
    for n in (0, 1):
        a = o(f, n)
        if not a:
            fails.append("no ORDERSUM ord=%d" % n)
        elif a["arrived"] != 0 or a["gave_up"] != 1:
            fails.append(
                "order %d arrived=%d gave_up=%d: retail must stall and give up"
                % (n, a["arrived"], a["gave_up"])
            )
    return fails


def out_f4(f):
    a = o(f, 0)
    if not a:
        return ["no ORDERSUM ord=0"]
    fails = []
    if a["stopped_short"] != 0 or a["gave_up"] != 0 or a["arrived"] + a["near"] < 1:
        fails.append(
            "order 0 (the seam-straddling group move) units=%d arrived=%d near=%d stopped_short=%d gave_up=%d: the group did not get to the goal"
            % (a["units"], a["arrived"], a["near"], a["stopped_short"], a["gave_up"])
        )
    return fails


def off_f4(f):
    a = o(f, 0)
    if not a:
        return ["no ORDERSUM ord=0"]
    return (
        []
        if a["stopped_short"] + a["gave_up"] >= 1
        else [
            "order 0 stopped_short=0 gave_up=0 with every key at retail: the seam-straddling group move no longer fails"
        ]
    )


def out_f5(f):
    fails = []
    for c in (1, 2):
        if not nd_total(f, c):
            fails.append("no `; ND TOTAL case=%d`: the nd probe never finished that case" % c)
    v = nd_verdict(f, 1, 0)
    if v and v["arrived"] < 0:
        fails.append("ND case 1: the mover never arrived")
    for k in (0, 1):
        v = nd_verdict(f, 2, k)
        if not v:
            fails.append("no ND VERDICT case=2 k=%d" % k)
        elif v["tiles"] < 1:
            fails.append("ND case 2 unit k=%d never moved" % k)
    if not pat_lines(f, r"; M1N nudge p"):
        fails.append("no `; M1N nudge` line (case 1: the idle blocker is nudged aside)")
    if not pat_lines(f, r"; M1N swap p|; M1N swap-trade"):
        fails.append("no `; M1N swap` line (case 2: the head-on pair trades tiles in the corridor)")
    return fails


def off_f5(f):
    fails = []
    if pat_lines(f, r"; M1N"):
        fails.append("`; M1N` line(s) with every key at retail")
    for c in (1, 2):
        if not nd_total(f, c):
            fails.append("no `; ND TOTAL case=%d`" % c)
    return fails


# f11: the column fixture (nd_case=7, nd_len=6, nd_delay=3). W = k0 (order goal 40,29), the column = k1 / k2 (goal 43,29). Pinned clock: the
# numbers are deterministic per configuration (ship / lib: key on 291 vs 288 for the column's front unit, key off / retail 341; mode=original: 348 vs
# 345 / 409), so the verdict is W's LAG behind the front unit: key on, W follows the column straight through the corridor (lag 3); key off / retail,
# W's route goes round the corridor walls because the friends' settled tiles are walls (lag 53 / 64).
F11_ON_LAG_MAX = 10
F11_OFF_LAG_MIN = 30


def f11_common(f):
    fails = []
    if not nd_total(f, 7):
        fails.append("no `; ND TOTAL case=7`: the nd probe never finished the column case")
    v, front = nd_verdict(f, 7, 0), nd_verdict(f, 7, 1)
    if not v or not front:
        fails.append("no ND VERDICT case=7 k=0 / k=1 (W / the column's front unit)")
        return fails, None
    if v["arrived"] < 0 or front["arrived"] < 0:
        fails.append(
            "ND case 7: W (arrived=%d) or the column's front unit (arrived=%d) never arrived"
            % (v["arrived"], front["arrived"])
        )
        return fails, None
    return fails, (v, v["arrived"] - front["arrived"])


def out_f11(f):
    fails, r = f11_common(f)
    if r:
        v, lag = r
        if lag > F11_ON_LAG_MAX:
            fails.append(
                "ND case 7: W arrived %d steps after the column's front unit (want <= %d: it should follow the column through the corridor)"
                % (lag, F11_ON_LAG_MAX)
            )
        if v["replans"] or v["wait"]:
            fails.append(
                "ND case 7: W replanned %d times / waited %d steps behind the column"
                % (v["replans"], v["wait"])
            )
    if not pat_lines(f, r"; M1N route-through p0 walking friends opened=\d+.*goal=38,29"):
        fails.append(
            "no `; M1N route-through p0 walking friends opened=N ... goal=38,29` line: W's order never opened the column's tiles"
        )
    return fails


def off_f11(f):
    fails, r = f11_common(f)
    if r and r[1] < F11_OFF_LAG_MIN:
        fails.append(
            "ND case 7: W arrived %d steps after the column's front unit with every key at retail (want >= %d: the walking friends are walls, W goes round)"
            % (r[1], F11_OFF_LAG_MIN)
        )
    return fails


def dump_nd(f, case):
    return "; ".join(
        "k%d arr=%d tiles=%d repl=%d wait=%d" % (r[2], r[8], r[9], r[10], r[11])
        for r in f["nd"]
        if r[0] == "V" and r[1] == case
    )


# f12: nd_case=8 (mods:M1DT fix 2, move_route_through_replanning). A 17-tile wall with a one-tile gap (nd_len=1); friend F (k1) stands IN the gap in state
# 0x0a for 1.5 s (state poked, activity_clock +1.5 s), W (k0) west of the wall is ordered through the gap. Pinned clock, deterministic per configuration:
# key on, W waits for F and follows it through the gap (arrives el=212, 6 tiles; with libmh the same); key off / retail, F is a wall for the planner and
# W walks round the end of the wall (el=543, 18 tiles).
F12_ON_TILES_MAX = 10
F12_OFF_TILES_MIN = 14


def f12_common(f):
    fails = []
    if not nd_total(f, 8):
        fails.append("no `; ND TOTAL case=8`: the nd probe never finished the replanning-wall case")
    v = nd_verdict(f, 8, 0)
    if not v:
        fails.append("no ND VERDICT case=8 k=0 (W)")
        return fails, None
    if v["arrived"] < 0:
        fails.append("ND case 8: W never arrived (%s)" % dump_nd(f, 8))
        return fails, None
    return fails, v


def out_f12(f):
    fails, v = f12_common(f)
    if v and v["tiles"] > F12_ON_TILES_MAX:
        fails.append(
            "ND case 8: W walked %d tiles (want <= %d: it should wait for the re-planning friend in the gap and follow it through, not go round the wall)"
            % (v["tiles"], F12_ON_TILES_MAX)
        )
    if not pat_lines_n(f, r"; M1DT route-through p\d+ replanning friends opened=\d+"):
        fails.append(
            "no `; M1DT route-through ... replanning friends opened=N` line: W's order never opened the friend's tile"
        )
    return fails


def off_f12(f):
    fails, v = f12_common(f)
    if v and v["tiles"] < F12_OFF_TILES_MIN:
        fails.append(
            "ND case 8: W walked only %d tiles with every key at retail (want >= %d: the re-planning friend is a wall, W goes round the wall)"
            % (v["tiles"], F12_OFF_TILES_MIN)
        )
    return fails


# f13: nd_case=9 (mods:M1DT fix 4, move_block_reset_moving + move_replan_detour_cap). Eight units in a blob west of a 17-tile wall with a one-tile gap are
# ordered together through it; the step's net_extra turns move_route_through_moving / _replanning OFF (their walls are what makes a block-wait replan a
# detour) and move_block_wait_ms=450 (9 retries), move_block_reset_moving=1 (off in the preset, forced on here so 4a rides the twin == splice hash) so the followers' patience runs out. Key on: the replans that happen are refused (up to 3 times) and the
# units take the gap (max 8 tiles walked per unit); with move_replan_detour_cap=0 the followers walk 19-22 tiles round the wall's end (measured: 4b is the
# rule that prevents it; 4a alone does not).
F13_ON_TILES_MAX = 12


def out_f13(f):
    fails = []
    if not nd_total(f, 9):
        return ["no `; ND TOTAL case=9`: the nd probe never finished the block-queue case"]
    vs = [r for r in f["nd"] if r[0] == "V" and r[1] == 9]
    if len(vs) < 8:
        fails.append("ND case 9: %d verdict lines (want 8)" % len(vs))
    worst = max((r[9] for r in vs), default=0)
    if worst > F13_ON_TILES_MAX:
        fails.append(
            "ND case 9: a unit walked %d tiles (want <= %d: the block-wait replan went round the wall's end instead of being refused): %s"
            % (worst, F13_ON_TILES_MAX, dump_nd(f, 9))
        )
    v0 = nd_verdict(f, 9, 0)
    if not v0 or v0["arrived"] < 0:
        fails.append("ND case 9: the front unit (k0) never arrived")
    if f["arm"] in ("ship", "orig") and not [ln for ln in f["n"] if "; M1DT replan refused" in ln]:
        fails.append("no `; M1DT replan refused` line: the cap never refused a detour route")
    return fails


def off_f13(f):
    fails = []
    if not nd_total(f, 9):
        fails.append("no `; ND TOTAL case=9`")
    v0 = nd_verdict(f, 9, 0)
    if not v0 or v0["arrived"] < 0:
        fails.append("ND case 9: the front unit (k0) never arrived with every key at retail")
    return fails


# f14 / f15 / f16 / f18 / f19: nd_fc=1 / 2 / 3 / 6 / 7 (mods:M1G 4b, move_formation_click; value 3 = "the formation moves as one", the preset).
# A blob of 16 idle ground units in a 4x4 block is ordered with ONE right-click.
#   f14 in place   the click is the blob's centre tile. Key on: NOBODY moves (the in-place deadband: click less than 1 tile from the centroid);
#                  retail shifts every member by click - truncated centroid and re-centres each wave (measured: see F14_OFF_TILES_MIN).
#   f15 far        the click is 10 tiles east in open ground. Key on: EVERY unit moves exactly (10,0) (one rigid translation, 160 tiles),
#                  no two on one tile.
#   f16 wall       a wall column 6 tiles east of the blob, the click one tile west of it. Key on: nobody ends inside the wall column or on a tile
#                  shared with another unit, everybody stops, nobody walks more than twice its straight distance, >= 12 units share one move vector.
#   f18 below      (spec F17) the click is the tile one below the blob's bottom edge. Key on: EVERY unit moves exactly (0,2) (32 tiles), no two on one
#                  tile, no `; M1N nudge` / `; M1N swap` line (nobody crosses), the last tile change <= F18_LAST_STEP_MAX steps after the order.
#   f19 spread     (spec F18) 16 units scattered over an 11x11 square (the offline model S5), the click at its centre. Key on: the end box is <= 6x6 (assigned 4x4),
#                  every unit within 3 of the click, no two on one tile.
F14_ON_TILES_MAX = 0  # the in-place deadband: nobody moves (was 2 under the keep rule)
F14_OFF_TILES_MIN = 9  # measured with every key at retail: 134 tiles, one unit 18, five units still moving after 1100 steps
F15_RING_MAX = 3
F15_RATIO_PCT_MAX = 115
F16_RATIO_PCT_MAX = 200
F16_CHEB_MAX = 5
F16_RIGID_MIN = 12  # units sharing one move vector (spec section 4: 12 of 16)
F18_LAST_STEP_MAX = 150  # steps from the order to the last tile change
F19_BOX_MAX = 6  # the ASSIGNMENT is a 4x4 block (selftest T4c, spec S5); gathering from all sides, arrivals nudge each other and some stop short: measured 6x5 (5x5 before the cause-C merge)
F19_CHEB_MAX = 3  # measured 3 (2 before the cause-C merge)


def fc_common(f, case, need_idle=True):
    fails = []
    if f["fcfail"]:
        fails.append(f["fcfail"][0].strip())
    t = f["fc"].get(case)
    if not t:
        fails.append("no `; FC TOTAL case=%d`: the fc probe never finished the case" % case)
        return fails, None
    if need_idle and t["idle"] != t["n"]:
        fails.append(
            "FC case %d: %d of %d units still moving at the end"
            % (case, t["n"] - t["idle"], t["n"])
        )
    return fails, t


def fc_vec(f, v):
    """end - start of one FC VERDICT tuple, modulo the map size"""
    w, h = f["fcmap"]
    dx, dy = v[2] - v[0], v[3] - v[1]
    return (dx % w, dy % h) if w and h else (dx, dy)


def dump_fc(t):
    return " ".join("%s=%d" % (k, t[k]) for k in FC_FIELDS[1:])


def out_f14(f):
    fails, t = fc_common(f, 1)
    if t:
        if t["tiles"] > F14_ON_TILES_MAX or t["moved"] > 0:
            fails.append(
                "FC case 1 (in place): %d units moved, %d tiles in total, one unit %d (want nobody: %d tiles): %s"
                % (t["moved"], t["tiles"], t["maxtiles"], F14_ON_TILES_MAX, dump_fc(t))
            )
        if t["dup"]:
            fails.append("FC case 1: %d units share an end tile" % t["dup"])
    if not pat_lines_n(f, r"; M1G FC p\d+ n=\d+ kept=\d+"):
        fails.append("no `; M1G FC ... kept=N` line: the formation never handled the commit")
    return fails


def off_f14(f):
    fails, t = fc_common(f, 1, need_idle=False)
    if t and t["tiles"] < F14_OFF_TILES_MIN:
        fails.append(
            "FC case 1 (in place), every key at retail: the blob moved only %d tiles (want >= %d: retail shifts it and re-centres each wave): %s"
            % (t["tiles"], F14_OFF_TILES_MIN, dump_fc(t))
        )
    return fails


def out_f15(f):
    fails, t = fc_common(f, 2)
    if t:
        bad = [
            v
            for v in f["fcv"].get(2, [])
            if fc_vec(f, v) != (10 % f["fcmap"][0] if f["fcmap"][0] else 10, 0)
        ]
        if bad or len(f["fcv"].get(2, [])) != t["n"]:
            fails.append(
                "FC case 2 (far): %d of %d units did not move exactly (10,0), first %s: %s"
                % (len(bad) or t["n"], t["n"], bad[:1], dump_fc(t))
            )
        if t["maxcheb"] > F15_RING_MAX:
            fails.append(
                "FC case 2 (far): a unit ended %d tiles from the click (want <= %d): %s"
                % (t["maxcheb"], F15_RING_MAX, dump_fc(t))
            )
        if t["dup"]:
            fails.append("FC case 2: %d units share an end tile: %s" % (t["dup"], dump_fc(t)))
        if t["tiles"] * 100 > F15_RATIO_PCT_MAX * max(t["straight"], 1):
            fails.append(
                "FC case 2 (far): walked %d tiles for %d straight (want <= %d%%): %s"
                % (t["tiles"], t["straight"], F15_RATIO_PCT_MAX, dump_fc(t))
            )
    if not pat_lines_n(f, r"; M1G FC p\d+ n=\d+ kept=\d+"):
        fails.append("no `; M1G FC ... kept=N` line: the formation never handled the commit")
    return fails


def off_f15(f):
    return fc_common(f, 2, need_idle=False)[0]


def out_f16(f):
    fails, t = fc_common(f, 3)
    if t:
        if t["onwall"]:
            fails.append(
                "FC case 3 (wall): %d units ended inside the wall column: %s"
                % (t["onwall"], dump_fc(t))
            )
        if t["dup"]:
            fails.append("FC case 3: %d units share an end tile: %s" % (t["dup"], dump_fc(t)))
        if t["maxratio"] > F16_RATIO_PCT_MAX:
            fails.append(
                "FC case 3 (wall): a unit walked %d%% of its straight distance (want <= %d%%): %s"
                % (t["maxratio"], F16_RATIO_PCT_MAX, dump_fc(t))
            )
        if t["maxcheb"] > F16_CHEB_MAX:
            fails.append(
                "FC case 3 (wall): a unit ended %d tiles from the click (want <= %d): %s"
                % (t["maxcheb"], F16_CHEB_MAX, dump_fc(t))
            )
        if t["rigid"] < F16_RIGID_MIN:
            fails.append(
                "FC case 3 (wall): only %d units share one move vector (want >= %d): %s"
                % (t["rigid"], F16_RIGID_MIN, dump_fc(t))
            )
    if not pat_lines_n(f, r"; M1G FC p\d+ n=\d+ kept=\d+"):
        fails.append("no `; M1G FC ... kept=N` line: the formation never handled the commit")
    return fails


def off_f16(f):
    return fc_common(f, 3, need_idle=False)[0]


# f17: nd_fc=5 (mods:M1DT cause C, folded into move_route_through_replanning). A 17-tile wall with a ONE-tile gap 3 tiles east of a 4x4 blob of 16 idle units;
# the blob is ordered through the gap, and nd_delay (100) steps later RE-ORDERED two tiles east / two south while it streams through. The later waves of the
# re-order's marshal call are planned while the earlier waves hop in state 0x0c; with the key on those hoppers are open ground, off they are walls and the
# later waves go round the wall's end (move_formation_click pinned 0 by the row: 4b would hide the effect).
F17_ON_TILES_MAX = 100  # measured with the key on: 88 tiles, worst unit 12 (nd_delay=140)
F17_ON_UNIT_MAX = 16
F17_OFF_UNIT_MIN = 20  # measured with every key at retail / the key off: 124 tiles, one unit 26 (round the wall's end)


def out_f17(f):
    fails, t = fc_common(f, 5)
    if t:
        if t["tiles"] > F17_ON_TILES_MAX or t["maxtiles"] > F17_ON_UNIT_MAX:
            fails.append(
                "FC case 5 (hop wall): the re-ordered group walked %d tiles, worst unit %d (want <= %d total, <= %d each: the 0x0c hoppers of the earlier waves "
                "should be open ground, not walls to the later waves): %s"
                % (t["tiles"], t["maxtiles"], F17_ON_TILES_MAX, F17_ON_UNIT_MAX, dump_fc(t))
            )
        if t["dup"]:
            fails.append("FC case 5: %d units share an end tile: %s" % (t["dup"], dump_fc(t)))
    if not pat_lines_n(f, r"; M1DT route-through p\d+ replanning friends opened=\d+"):
        fails.append(
            "no `; M1DT route-through ... replanning friends opened=N` line: the re-order never opened a friend's tile"
        )
    return fails


def off_f17(f):
    fails, t = fc_common(f, 5, need_idle=False)
    if t and t["maxtiles"] < F17_OFF_UNIT_MIN:
        fails.append(
            "FC case 5 (hop wall), every key at retail: the worst unit walked only %d tiles (want >= %d: the hopping friends are walls, a later wave goes round the wall's end): %s"
            % (t["maxtiles"], F17_OFF_UNIT_MIN, dump_fc(t))
        )
    return fails


def out_f18(f):
    fails, t = fc_common(f, 6)
    if t:
        vs = f["fcv"].get(6, [])
        bad = [v for v in vs if fc_vec(f, v) != (0, 2)]
        if bad or len(vs) != t["n"]:
            fails.append(
                "FC case 6 (below): %d of %d units did not move exactly (0,2), first %s: %s"
                % (len(bad) or t["n"], t["n"], bad[:1], dump_fc(t))
            )
        if t["dup"]:
            fails.append("FC case 6: %d units share an end tile: %s" % (t["dup"], dump_fc(t)))
        if t["lastmove"] > F18_LAST_STEP_MAX:
            fails.append(
                "FC case 6 (below): the last unit moved %d steps after the order (want <= %d): %s"
                % (t["lastmove"], F18_LAST_STEP_MAX, dump_fc(t))
            )
        if pat_lines(
            f, r"; M1N nudge p0 |; M1N swap(-trade|-marshal)? p0 "
        ):  # the group is player 0's; player 1's own traffic is none of it
            fails.append(
                "FC case 6 (below): a `; M1N nudge` / `; M1N swap` line of player 0: somebody crossed the group"
            )
    if not pat_lines_n(f, r"; M1G FC p\d+ n=\d+ kept=\d+"):
        fails.append("no `; M1G FC ... kept=N` line: the formation never handled the commit")
    return fails


def off_f18(f):
    return fc_common(f, 6, need_idle=False)[0]


def out_f19(f):
    fails, t = fc_common(f, 7)
    if t:
        if t["bw"] > F19_BOX_MAX or t["bh"] > F19_BOX_MAX:
            fails.append(
                "FC case 7 (spread): the end box is %dx%d (want <= %dx%d): %s"
                % (t["bw"], t["bh"], F19_BOX_MAX, F19_BOX_MAX, dump_fc(t))
            )
        if t["maxcheb"] > F19_CHEB_MAX:
            fails.append(
                "FC case 7 (spread): a unit ended %d tiles from the click (want <= %d): %s"
                % (t["maxcheb"], F19_CHEB_MAX, dump_fc(t))
            )
        if t["dup"]:
            fails.append("FC case 7: %d units share an end tile: %s" % (t["dup"], dump_fc(t)))
    if not pat_lines_n(f, r"; M1G FC p\d+ n=\d+ kept=\d+"):
        fails.append("no `; M1G FC ... kept=N` line: the formation never handled the commit")
    return fails


def off_f19(f):
    return fc_common(f, 7, need_idle=False)[0]


# f20 thin wall (0.2.1 hotfix, nd_fc=8): a ONE-tile wall column, 9 tiles tall, 3 tiles east of the blob, free ground on both sides; the click is the tile just west of
# its middle. Geometric ring order put tiles on the far side of the wall (one ring away, ~9 steps by path) into the group's target set; key 3 ranks by path distance.
# Key on: nobody ends east of the wall column (within F20_FAR_SPAN tiles of it), nobody walks more than F20_UNIT_TILES_MAX tiles, nobody ends on the wall, no shared tile.
F20_FAR_SPAN = 12
F20_UNIT_TILES_MAX = 8  # measured 6 with the path ranking; the geometric order put two units on the far side (one walked 9)


def f20_far(f):
    """units whose end tile lies on the far (east) side of the wall column"""
    w, _ = f["fcmap"]
    wx = f["fcwall"]
    if not w or wx < 0:
        return None
    return [v for v in f["fcv"].get(8, []) if 1 <= (v[2] - wx) % w <= F20_FAR_SPAN]


def out_f20(f):
    fails, t = fc_common(f, 8)
    if t:
        far = f20_far(f)
        if far is None:
            fails.append("FC case 8 (thin wall): no `wall_x=` in the FC SETUP line")
        elif far:
            fails.append(
                "FC case 8 (thin wall): %d unit(s) ended on the FAR side of the wall (first %s): %s"
                % (len(far), far[:1], dump_fc(t))
            )
        if t["onwall"]:
            fails.append(
                "FC case 8: %d units ended inside the wall column: %s" % (t["onwall"], dump_fc(t))
            )
        if t["dup"]:
            fails.append("FC case 8: %d units share an end tile: %s" % (t["dup"], dump_fc(t)))
        if t["maxtiles"] > F20_UNIT_TILES_MAX:
            fails.append(
                "FC case 8 (thin wall): a unit walked %d tiles (want <= %d: a detour round the wall): %s"
                % (t["maxtiles"], F20_UNIT_TILES_MAX, dump_fc(t))
            )
    if not pat_lines_n(f, r"; M1G FC p\d+ n=\d+ kept=\d+"):
        fails.append("no `; M1G FC ... kept=N` line: the formation never handled the commit")
    return fails


def off_f20(f):
    return fc_common(f, 8, need_idle=False)[0]


# f21 door queue (mods:M1BE, nd_door=2): 16 soldiers ordered into a freshly built barracks whose efficiency is pinned to 0.533 (the report's
# barracks). The probe's `; DR TOTAL` line is the verdict. With move_enter_queue + storage_door_hold on: every soldier docks, nobody gives up,
# and the group is inside in at most F21_STEPS_MAX steps after the order (the door no longer closes between entrants) with no door closing.
# Measured 2026-10-20 (19 slowest-accepted units in a diagonal block, the save's alien barracks poked to efficiency 0.533): retail (m1h_off)
# 2606 steps / 5 closes, improved_movement without the two keys (f21n) 2072 / 2, with them 1757 / 0.
F21_STEPS_MAX = 1900
F21_STEPS_MAX_ORIG = 2400  # [config] mode=original (rollback_original.ini) walks the same block 100 steps later to the first dock: measured 2277, still 0 closes


def dr_dump(t):
    return " ".join("%s=%d" % (k, t[k]) for k in DR_FIELDS)


def dr_common(f):
    fails = []
    for ln in f["drfail"]:
        fails.append(ln.strip())
    t = f["dr"]
    if not t:
        fails.append("no `; DR TOTAL` line: the door-queue probe never finished")
    return fails, t


def out_f21(f):
    limit = F21_STEPS_MAX_ORIG if f.get("arm") == "orig" else F21_STEPS_MAX
    fails, t = dr_common(f)
    if t:
        if t["docked"] != t["n"]:
            fails.append("DR: %d of %d soldiers docked: %s" % (t["docked"], t["n"], dr_dump(t)))
        if t["giveups"]:
            fails.append(
                "DR: %d soldier(s) gave the order up outside: %s" % (t["giveups"], dr_dump(t))
            )
        if t["closes"]:
            fails.append(
                "DR: the door closed %d time(s) while soldiers were still arriving: %s"
                % (t["closes"], dr_dump(t))
            )
        if t["lastdock"] > limit:
            fails.append(
                "DR: the last soldier docked %d steps after the order (want <= %d): %s"
                % (t["lastdock"], limit, dr_dump(t))
            )
        print("  F21 " + dr_dump(t))
    return fails


def off_f21(f):
    fails, t = dr_common(f)
    if t:
        print("  F21 (retail) " + dr_dump(t))
    return fails


def out_f21n(f):
    """the rc22 behaviour (improved_movement on, move_enter_queue = storage_door_hold = 0): informational, the probe must only finish"""
    fails, t = dr_common(f)
    if t:
        print("  F21n (no fix) " + dr_dump(t))
    return fails


# f21s shuttle door (mods:M1SH, nd_door=3): the SAME fixture on the race's shuttle (storage_door_hold covers every storage door). Key on: every
# unit docks before the door closes (closes == 0), nobody gives up, last dock inside F21S_STEPS_MAX. f21sn (key 0) is the red-proof: the shuttle
# door closes between entrants (closes > 0), so f21s' closes == 0 is the key's doing.
F21S_STEPS_MAX = 1400  # measured 2026-10-24: key on 1186 steps / 0 closes, key 0 (f21sn) 1690 / 5 closes (16 slowest accepted infantry, the save's alien shuttle)


def out_f21s(f):
    fails, t = dr_common(f)
    if t:
        if t["docked"] != t["n"]:
            fails.append("DR: %d of %d units docked: %s" % (t["docked"], t["n"], dr_dump(t)))
        if t["giveups"]:
            fails.append(
                "DR: %d unit(s) gave the order up outside: %s" % (t["giveups"], dr_dump(t))
            )
        if t["closes"]:
            fails.append(
                "DR: the shuttle door closed %d time(s) before the last dock: %s"
                % (t["closes"], dr_dump(t))
            )
        if F21S_STEPS_MAX and t["lastdock"] > F21S_STEPS_MAX:
            fails.append(
                "DR: the last unit docked %d steps after the order (want <= %d): %s"
                % (t["lastdock"], F21S_STEPS_MAX, dr_dump(t))
            )
        print("  F21s " + dr_dump(t))
    return fails


def out_f21sn(f):
    """key 0 on the shuttle: the retail rule closes the door between entrants (the red-proof of f21s)"""
    fails, t = dr_common(f)
    if t:
        if t["closes"] <= 0:
            fails.append(
                "DR: key 0 but the shuttle door never closed (the fixture proves nothing): %s"
                % dr_dump(t)
            )
        print("  F21sn (key 0) " + dr_dump(t))
    return fails


def out_f6(f):
    fails = []
    for n in (0, 1):
        if not o(f, n):
            fails.append("no ORDERSUM ord=%d" % n)
    if not f["done"]:
        fails.append("no `; MV DONE`")
    if not any(r["scale"] > r["base"] for r in f["mvc"]):
        fails.append(
            "no MV C line with scale > 1000/1000: the faster members were never slowed to the group's slowest"
        )
    return fails


def off_f6(f):
    return (
        []
        if not any(r["scale"] > r["base"] for r in f["mvc"])
        else ["a unit carries a group-speed scale > 1.0 with every key at retail"]
    )


def out_f7(f):
    fails = []
    if not f["done"]:
        fails.append("no `; MV DONE`")
    if not fired_from(f, 5, 62, 3):
        fails.append(
            "unit 5 never fires (state 0x1c) from 62,3: the footprint aim (attack_range_footprint) did not bring it into range there"
        )
    return fails


def off_f7(f):
    return (
        [] if not fired_from(f, 5, 62, 3) else ["unit 5 fires from 62,3 with every key at retail"]
    )


def out_d7(f):
    fails = []
    if not f["done"]:
        fails.append("no `; MV DONE`")
    if not fired_from(f, 5, 59, 5):
        fails.append(
            "unit 5 never fires (state 0x1c) from 59,5: the attack-heading fix did not unstick it"
        )
    return fails


def out_f8(f):
    fails = []
    a = o(f, 3)
    if not a:
        return ["no ORDERSUM ord=3 (the Shift-click merge)"]
    if a["dead"] != 1:
        fails.append(
            "order 3 dead=%d: the mover was not absorbed into the attacking squad (move_squad_merge_fix)"
            % a["dead"]
        )
    if not f["done"]:
        fails.append("no `; MV DONE`")
    return fails


def out_f8x(f):
    """f8 with attack_range_footprint=0 (the libmh arm, while that twin diverges): the merge scenario itself needs the footprint aim to bring
    the squad into position, so only the replay's completeness is asserted here."""
    fails = []
    if not o(f, 3):
        fails.append("no ORDERSUM ord=3 (the Shift-click merge order was never issued)")
    if not f["done"]:
        fails.append("no `; MV DONE`")
    return fails


def off_f8(f):
    a = o(f, 3)
    if not a:
        return ["no ORDERSUM ord=3"]
    return (
        []
        if a["dead"] == 0
        else ["the merge was accepted with every key at retail (an attacking squad refuses it)"]
    )


def out_f9(f):
    fails = []
    if not nd_total(f, 6):
        fails.append(
            "no `; ND TOTAL case=6`: the corridor-with-a-parked-friend (case 6) case never finished"
        )
    if not f["oobfill"]:
        fails.append("no `; MV OOBFILL` line: the forced precondition was never set")
    site = f["oobsite"]
    if f["oobfill"] and not site:
        fails.append("`; MV OOBFILL` line does not name the landing player/slot")
    bad = [w for w in f["oobw"] if site and (w[1], w[2]) == site]
    if bad:
        fails.append(
            "%d `; MV OOBW` write(s) into the landing slot (player %d slot %d): the slot-0xff splice wrote out of bounds"
            % (len(bad), site[0], site[1])
        )
    return fails


def off_f9(f):
    fails = []
    if not nd_total(f, 6):
        fails.append("no `; ND TOTAL case=6`")
    if not f["oobfill"]:
        fails.append("no `; MV OOBFILL` line")
    site = f["oobsite"]
    if f["oobfill"] and not site:
        fails.append("`; MV OOBFILL` line does not name the landing player/slot")
    elif site and not [w for w in f["oobw"] if (w[1], w[2]) == site]:
        fails.append(
            "no `; MV OOBW` into the landing slot (player %d slot %d) with the guard off: the fixture no longer provokes the slot-0xff write, so the guard's green proves nothing"
            % site
        )
    return fails


def out_d2(f):
    fails = []
    a = o(f, 0)
    if not a:
        return ["no ORDERSUM ord=0"]
    if a["arrived"] != 1 or a["gave_up"] or a["stalls"]:
        fails.append(
            "order 0 arrived=%d gave_up=%d stalls=%d: the mover waited instead of trading tiles at once"
            % (a["arrived"], a["gave_up"], a["stalls"])
        )
    if not swap_marshal_any(f, 12, 1):
        fails.append(
            "no `; M1N swap-marshal` line naming units 12 and 1: the marshal-site trade did not run"
        )
    return fails


def out_d5(f):
    fails = []
    if not nd_total(f, 3):
        fails.append("no `; ND TOTAL case=3`")
    if not pat_lines(f, r"; M1N claim"):
        fails.append("no `; M1N claim` line (move_claim_leader=1)")
    return fails


PF_V = re.compile(
    r"; PF VERDICT k=(\d+) unit=(\d+) alive=(\d+) .*? remain_bfs=(-?\d+) start_bfs=(\d+)"
)


def pf_progress(f):
    """[(alive, remain_bfs, start_bfs)] of the pf probe's verdict lines."""
    return [
        (int(m.group(3)), int(m.group(4)), int(m.group(5)))
        for m in (PF_V.search(ln) for ln in f["h"])
        if m
    ]


def out_f10(f):
    fails = []
    pv = pf_progress(f)
    if not pv:
        fails.append("no `; PF VERDICT` line: the group move on the big map never finished")
    elif sum(1 for a, r, st in pv if a and r < st - 20) < 6:
        fails.append(
            "fewer than 6 of the %d units made >20 tiles of progress along the 256x256 route: %s"
            % (len(pv), pv)
        )
    if not any("; PF ORDER" in ln for ln in f["h"]):
        fails.append("no `; PF ORDER` line: the pf probe never gave its order")
    if not any("mode=2" in ln for ln in f["n"] if "; corridor (mods:M1C)" in ln):
        fails.append("no `; corridor (mods:M1C) mode=2` line: route_corridor_mode=2 never ran")
    return fails


def off_f10(f):
    fails = []
    if not any("; PF VERDICT" in ln for ln in f["h"]):
        fails.append("no `; PF VERDICT` line")
    return fails


CASES = {
    "f1": dict(hits=["route_through"], out=out_f1, off=off_f1),
    "f2": dict(hits=["route_through", "nudge"], out=out_f2, off=off_f2),
    "f3": dict(hits=["mk2"], out=out_f3, off=off_f3),
    "f4": dict(
        hits=[
            "cent_x",
            "corr",
            "flood_c",
            "flood_q",
            "arr_commit",
            "arr_retry",
            "blocked",
            "detour",
        ],
        out=out_f4,
        off=off_f4,
    ),
    "f5": dict(hits=["nudge"], out=out_f5, off=off_f5),
    "f6": dict(hits=["speed_commit", "speed_walk"], out=out_f6, off=off_f6),
    "f11": dict(hits=["route_moving"], out=out_f11, off=off_f11),
    "f12": dict(hits=["route_replanning"], out=out_f12, off=off_f12),
    "f13": dict(hits=["blocked", "replan_cap"], out=out_f13, off=off_f13),
    "f14": dict(hits=["formation"], out=out_f14, off=off_f14),
    "f15": dict(hits=["formation"], out=out_f15, off=off_f15),
    "f16": dict(hits=["formation"], out=out_f16, off=off_f16),
    "f17": dict(hits=["route_replanning"], out=out_f17, off=off_f17),
    "f18": dict(hits=["formation"], out=out_f18, off=off_f18),
    "f19": dict(hits=["formation"], out=out_f19, off=off_f19),
    "f20": dict(hits=["formation"], out=out_f20, off=off_f20),
    "f21": dict(hits=["enter_queue", "door_hold"], out=out_f21, off=off_f21),
    "f21n": dict(hits=[], out=out_f21n, off=None),
    "f21s": dict(hits=["door_hold"], out=out_f21s, off=off_f21),
    "f21sn": dict(hits=[], out=out_f21sn, off=None),
    "f7": dict(hits=["arf_issue", "arf_fire"], out=out_f7, off=off_f7),
    "d7": dict(hits=[], out=out_d7, off=None),
    "f8": dict(hits=["squad_merge", "merge_goal", "proj_facing"], out=out_f8, off=off_f8),
    "f8x": dict(hits=[], out=out_f8x, off=None),
    "f9": dict(hits=["detour"], out=out_f9, off=off_f9),
    "d2": dict(hits=["mk2"], out=out_d2, off=None),
    "d5": dict(hits=["nudge"], out=out_d5, off=None),
    "f10": dict(hits=["flood_exact", "corr"], out=out_f10, off=off_f10),
}


def check(run_dir, case, arm):
    fails = []
    f = facts(run_dir)
    spec = CASES[case]
    if arm == "off":
        if f["carrier"]:
            fails.append(
                "%d `; move carrier [net]` key line(s) with the three masters 0 (retail): %s"
                % (len(f["carrier"]), ", ".join(sorted(f["carrier"])[:4]))
            )
        hot = {k: v for k, v in list(f["splice"].items()) + list(f["twin"].items()) if v}
        if hot:
            fails.append("hit counters moved with every key at retail: %s" % hot)
        if f["m1n"]:
            fails.append(
                "%d `; M1N` seam line(s) with every key at retail -- a splice or twin acted while disabled"
                % len(f["m1n"])
            )
        if spec["off"]:
            fails += ["retail: " + x for x in spec["off"](f)]
        return fails, f
    if not f["carrier"]:
        fails.append("no `; move carrier [net]` line: the preset keys were never reported")
    for key, (_val, text) in f["carrier"].items():
        if "NOT ACTIVE" in text or "MISMATCH" in text:
            fails.append("carrier: %s -- %s" % (key, text))
        elif arm in ("ship", "orig") and not text.startswith("retail splice"):
            fails.append("carrier: %s says `%s`, want `retail splice`" % (key, text.split(" (")[0]))
        elif arm == "lib" and key not in LIB_SPLICE_OK and "libmh twin" not in text:
            fails.append("carrier: %s says `%s`, want `libmh twin`" % (key, text.split(" (")[0]))
    if f["conflict"]:
        fails.append("CARRIER-CONFLICT line(s): the carrier report and the hit counters disagree")
    if arm in ("ship", "orig") and f["twin"]:
        fails.append("twin hit counters moved with libmh absent: %s" % f["twin"])
    hits = hits_of(f, arm)
    for name in spec["hits"]:
        if arm == "lib" and name in NO_TWIN_COUNTER:
            continue
        key = HIT_KEY.get(name)
        if key and not any(k in f["carrier"] for k in key.split("|")):
            fails.append("key %s (hit %s) is not in the carrier report" % (key, name))
        if hits.get(name, 0) <= 0:
            fails.append(
                "hit counter %s = 0 (%s): the key's path never ran"
                % (name, "twin" if arm == "lib" else "splice")
            )
    f["arm"] = arm
    fails += spec["out"](f)
    return fails, f


# ---- the 2-peer rows --------------------------------------------------------------------------------------------------------
STAMP = re.compile(r"^\[[0-9:.]+\] ")
MP_CASES = (1, 2, 4, 5, 7, 8, 9)
# the hit counters the five nd cases must move on EVERY peer (splice counters on ship, twin counters on lib)
MP_HITS = [
    "nudge",
    "route_through",
    "route_moving",
    "speed_commit",
    "speed_walk",
    "arr_commit",
    "formation",
]


def norm(ln):
    """A log line without its wall-clock stamp, the twin's ` (libmh twin)` tag and the seam's trailing event counter, so a shipped and a libmh peer's lines compare."""
    out = STAMP.sub("", ln.strip()).replace(" (libmh twin)", "")
    # the seam's per-process event counter (`... (7)`): the splice and the twin count their own events, so it is not part of the event
    return re.sub(r" \(\d+\)$", "", out)


def nd_lines(f, what):
    return [ln for ln in f["h"] if what in ln]


def check_mp(host_dir, client_dir, arm_pair):
    fails = []
    peers = (("host", facts(host_dir)), ("client", facts(client_dir)))
    arms = {"ship": ("ship", "ship"), "lib": ("lib", "lib"), "mixed": ("ship", "lib")}[arm_pair]
    for (name, f), arm in zip(peers, arms):
        for c in MP_CASES:
            if not nd_total(f, c):
                fails.append(
                    "%s: no `; ND TOTAL case=%d` -- the nd probe never finished that case"
                    % (name, c)
                )
        for ln in nd_lines(f, "; ND FAIL"):
            fails.append("%s: %s" % (name, ln.strip()))
        if not f["carrier"]:
            fails.append("%s: no `; move carrier [net]` line" % name)
        for key, (_val, text) in f["carrier"].items():
            if "NOT ACTIVE" in text or "MISMATCH" in text:
                fails.append("%s carrier: %s -- %s" % (name, key, text))
            elif arm == "ship" and not text.startswith("retail splice"):
                fails.append(
                    "%s carrier: %s says `%s`, want `retail splice`"
                    % (name, key, text.split(" (")[0])
                )
            elif arm == "lib" and key not in LIB_SPLICE_OK and "libmh twin" not in text:
                fails.append(
                    "%s carrier: %s says `%s`, want `libmh twin`" % (name, key, text.split(" (")[0])
                )
        hits = hits_of(f, arm)
        for hname in MP_HITS:
            if hits.get(hname, 0) <= 0:
                fails.append(
                    "%s: hit counter %s = 0 (%s)"
                    % (name, hname, "twin" if arm == "lib" else "splice")
                )
        if f["conflict"]:
            fails.append("%s: CARRIER-CONFLICT line(s)" % name)
        # mods:M1G 4b: the formation case (nd_fc=4, far click then an in-place re-order while walking) finished on every peer, nobody failed
        if 4 not in f["fc"]:
            fails.append(
                "%s: no `; FC TOTAL case=4` -- the fc probe never finished the formation case"
                % name
            )
        elif f["fc"][4]["idle"] != f["fc"][4]["n"]:
            fails.append(
                "%s: FC case 4: %d of %d units still moving at the end"
                % (name, f["fc"][4]["n"] - f["fc"][4]["idle"], f["fc"][4]["n"])
            )
        for ln in f["fcfail"]:
            fails.append("%s: %s" % (name, ln.strip()))
    # the two peers must have seen the SAME cases: equal ND VERDICT / ND TOTAL lines, equal M1N seam lines (same tiles, same order)
    (_, fh), (_, fc) = peers
    xh = [norm(ln) for ln in fh["h"] if "; FC VERDICT" in ln or "; FC TOTAL" in ln]
    xc = [norm(ln) for ln in fc["h"] if "; FC VERDICT" in ln or "; FC TOTAL" in ln]
    if xh != xc:
        fails.append(
            "the peers' FC VERDICT / FC TOTAL lines differ (host %d lines, client %d): first difference %r"
            % (len(xh), len(xc), next(((a, b) for a, b in zip(xh, xc) if a != b), None))
        )
    vh = [norm(ln) for ln in nd_lines(fh, "; ND VERDICT") + nd_lines(fh, "; ND TOTAL")]
    vc = [norm(ln) for ln in nd_lines(fc, "; ND VERDICT") + nd_lines(fc, "; ND TOTAL")]
    if vh != vc:
        fails.append(
            "the peers' ND VERDICT / ND TOTAL lines differ (host %d lines, client %d): first difference %r"
            % (len(vh), len(vc), next(((a, b) for a, b in zip(vh, vc) if a != b), None))
        )
    if not vh:
        fails.append("no ND VERDICT line on either peer")
    mh_ = [norm(ln) for ln in fh["m1n"]]
    mc_ = [norm(ln) for ln in fc["m1n"]]
    if sorted(mh_) != sorted(mc_):
        fails.append(
            "the peers' `; M1N` seam lines differ (host %d lines, client %d)" % (len(mh_), len(mc_))
        )
    if not mh_:
        fails.append("no `; M1N` seam line on either peer")
    return fails, peers


def selftest():
    bad = 0
    total = 0

    def expect(name, cond):
        nonlocal bad, total
        total += 1
        print("  %-52s %s" % (name, "ok" if cond else "WRONG"))
        bad += 0 if cond else 1

    tmp = tempfile.mkdtemp(prefix="chk_m1h_")
    try:
        osum = "; MV ORDERSUM ord=0 goal=7,93 units=1 arrived=%d near=0 stopped_short=0 gave_up=%d interrupted=0 dead=0 t_first_arrive=640 t_last_settle=639 max_spread=0 stalls=0 (x)"
        carrier = "[09:08:31.109] ; move carrier [net] %s=1: %s"

        def mk(name, harness, net):
            d = os.path.join(tmp, name)
            os.makedirs(d)
            open(os.path.join(d, "mh_match_harness.log"), "w").write("\n".join(harness) + "\n")
            open(os.path.join(d, "mh_net.log"), "w").write("\n".join(net) + "\n")
            return d

        good_h = ["; MV DONE step=854 rel=854", osum % (1, 0)]
        good_n = [
            carrier % ("move_route_through_idle", "retail splice"),
            "; M1N route-through p0 idle friends opened=2 goal=7,93 (1)",
            "; move hits flood_q=5 route_through=1",
        ]
        r = lambda name, h, n, case="f1", arm="ship": check(mk(name, h, n), case, arm)[0]
        expect("f1 ship ok", not r("a", good_h, good_n))
        expect(
            "f1 ship: hit counter 0", bool(r("b", good_h, good_n[:2] + ["; move hits flood_q=5"]))
        )
        expect("f1 ship: unit gave up", bool(r("c", ["; MV DONE", osum % (0, 1)], good_n)))
        expect("f1 ship: no route-through line", bool(r("d", good_h, [good_n[0], good_n[2]])))
        expect(
            "f1 ship: twin text on a ship run",
            bool(
                r(
                    "e",
                    good_h,
                    [carrier % ("move_route_through_idle", "libmh twin (x)")] + good_n[1:],
                )
            ),
        )
        expect(
            "f1 ship: NOT ACTIVE",
            bool(
                r(
                    "f",
                    good_h,
                    [carrier % ("move_route_through_idle", "NOT ACTIVE: displaced, no twin")]
                    + good_n[1:],
                )
            ),
        )
        expect("f1 ship: no carrier line", bool(r("g", good_h, good_n[1:])))
        expect(
            "f1 ship: CARRIER-CONFLICT",
            bool(
                r(
                    "h",
                    good_h,
                    good_n
                    + [
                        "; move hits route_through=1 CARRIER-CONFLICT:x(reported not reached, hits=1)"
                    ],
                )
            ),
        )
        lib_n = [
            carrier % ("move_route_through_idle", "libmh twin (x)"),
            good_n[1],
            "; move twin hits route_through=1 (libmh twin)",
        ]
        expect("f1 lib ok", not r("i", good_h, lib_n, arm="lib"))
        expect(
            "f1 lib: splice text on a lib run",
            bool(r("j", good_h, [good_n[0]] + lib_n[1:], arm="lib")),
        )
        expect("f1 lib: no twin counter", bool(r("k", good_h, lib_n[:2], arm="lib")))
        expect("f1 off ok", not r("l", ["; MV DONE", osum % (0, 1)], [], arm="off"))
        expect("f1 off: it arrived", bool(r("m", good_h, [], arm="off")))
        expect(
            "f1 off: a key reported",
            bool(r("n", ["; MV DONE", osum % (0, 1)], [good_n[0]], arm="off")),
        )
        expect(
            "f1 off: a counter moved",
            bool(r("p", ["; MV DONE", osum % (0, 1)], ["; move hits route_through=1"], arm="off")),
        )
        # f9 (the slot-0xff fixture), f4 (the centroid group move), f8x, the M1N seam lines at retail, the 2-peer comparison
        nd6 = [
            "; ND TOTAL case=6 steps=287 tiles=9 replans=0 wait_steps=0",
            "; MV OOBFILL player 0 slot 255 filled with 12/1 legs (x) layout=reloc rows=501 pstride=300600",
        ]
        f9_n = [carrier % ("move_detour_slot_guard", "retail splice"), "; move hits detour=1"]
        expect("f9 ship ok", not r("q", nd6, f9_n, case="f9"))
        expect(
            "f9 ship: a write into p2 slot 55",
            bool(
                r(
                    "s",
                    nd6
                    + [
                        "; MV OOBW step=3449 player=0 slot=255 UNOWNED changed_bytes=3 first_off=0 new=0101"
                    ],
                    f9_n,
                    case="f9",
                )
            ),
        )
        expect("f9 ship: no fill line", bool(r("t", nd6[:1], f9_n, case="f9")))
        expect("f9 off: the write is required", bool(r("u", nd6, [], case="f9", arm="off")))
        expect(
            "f9 off ok",
            not r(
                "v",
                nd6
                + [
                    "; MV OOBW step=3449 player=0 slot=255 UNOWNED changed_bytes=3 first_off=0 new=0101"
                ],
                [],
                case="f9",
                arm="off",
            ),
        )
        o4 = "; MV ORDERSUM ord=0 goal=0,2 units=4 arrived=1 near=2 stopped_short=%d gave_up=%d interrupted=1 dead=0 t_first_arrive=506 t_last_settle=627 max_spread=5 stalls=0 (x)"
        f4_n = [
            carrier % ("move_centroid_wrap_fix", "retail splice"),
            "; move hits cent_x=2 corr=3 flood_c=9 flood_q=9 arr_commit=3 arr_retry=1 blocked=4 detour=1",
        ]
        for k_ in (
            "move_distinct_goals",
            "move_near_enough",
            "move_block_wait_ms",
            "move_detour_slot_guard",
            "route_corridor_mode",
            "route_cost_mode",
            "move_flood_queue_guard",
        ):
            f4_n.append(carrier % (k_, "retail splice"))
        expect("f4 ship ok", not r("w", ["; MV DONE", o4 % (0, 0)], f4_n, case="f4"))
        expect("f4 ship: stopped short", bool(r("x", ["; MV DONE", o4 % (2, 0)], f4_n, case="f4")))
        expect(
            "f4 off: it still arrives",
            bool(r("y", ["; MV DONE", o4 % (0, 0)], [], case="f4", arm="off")),
        )
        expect("f4 off ok", not r("z", ["; MV DONE", o4 % (2, 0)], [], case="f4", arm="off"))
        expect(
            "f1 off: a seam line fired",
            bool(
                r(
                    "aa",
                    ["; MV DONE", osum % (0, 1)],
                    ["; M1N route-through p0 idle friends opened=1 (1)"],
                    arm="off",
                )
            ),
        )
        o8 = "; MV ORDERSUM ord=3 goal=20,51 units=1 arrived=0 near=0 stopped_short=0 gave_up=0 interrupted=0 dead=%d t_first_arrive=-1 t_last_settle=0 max_spread=0 stalls=0 (x)"
        expect(
            "f8x lib ok without the merge",
            not r(
                "ab",
                ["; MV DONE", o8 % 0],
                [carrier % ("move_squad_merge_fix", "libmh twin (x)")],
                case="f8x",
                arm="lib",
            ),
        )
        expect(
            "f8x lib: order 3 never issued",
            bool(
                r(
                    "ac",
                    ["; MV DONE"],
                    [carrier % ("move_squad_merge_fix", "libmh twin (x)")],
                    case="f8x",
                    arm="lib",
                )
            ),
        )
        mp_h = ["; ND TOTAL case=%d steps=9" % c_ for c_ in MP_CASES] + [
            "; FC TOTAL case=4 steps=9 n=4 tiles=9 straight=9 moved=4 maxtiles=3 maxcheb=1 dup=0 maxratio=100 onwall=0 idle=4 bw=2 bh=2 rigid=4 lastmove=9",
            "; ND VERDICT case=1 k=0 unit=2 xy=1,1 goal=1,1 arrived_el=5 tiles=3 replans=0 wait_steps=0 max_retry=0 state=0x01",
        ]
        mp_n = [
            carrier % ("move_nudge_idle", "retail splice"),
            "; M1N nudge p0 unit 2 at 1,1 / unit 3 at 2,2 (1)",
            "; move hits nudge=1 route_through=1 route_moving=1 speed_commit=1 speed_walk=1 arr_commit=1 formation=1",
        ]
        dh, dc = mk("mp_h", mp_h, mp_n), mk("mp_c", mp_h, mp_n)
        expect("mp ship ok", not check_mp(dh, dc, "ship")[0])
        dc2 = mk(
            "mp_c2",
            mp_h[:-1]
            + [
                "; ND VERDICT case=1 k=0 unit=2 xy=1,1 goal=1,1 arrived_el=6 tiles=3 replans=0 wait_steps=0 max_retry=0 state=0x01"
            ],
            mp_n,
        )
        expect("mp: the peers' ND VERDICT lines differ", bool(check_mp(dh, dc2, "ship")[0]))
        dc3 = mk("mp_c3", mp_h[1:], mp_n)
        expect("mp: a case never finished on one peer", bool(check_mp(dh, dc3, "ship")[0]))
        dc4 = mk(
            "mp_c4", mp_h, [mp_n[0], "; M1N nudge p0 unit 2 at 9,9 / unit 3 at 2,2 (1)", mp_n[2]]
        )
        expect("mp: the peers' M1N lines differ", bool(check_mp(dh, dc4, "ship")[0]))
        expect(
            "mp mixed ok (twin text on the client)",
            not check_mp(
                dh,
                mk(
                    "mp_c5",
                    mp_h,
                    [
                        carrier % ("move_nudge_idle", "libmh twin (x)"),
                        mp_n[1] + " (libmh twin)",
                        "; move twin hits nudge=1 (libmh twin)",
                        "; move twin hits route_through=1 (libmh twin)",
                        "; move twin hits route_moving=1 (libmh twin)",
                        "; move twin hits speed_commit=1 (libmh twin)",
                        "; move twin hits speed_walk=1 (libmh twin)",
                        "; move twin hits arr_commit=1 (libmh twin)",
                        "; move twin hits formation=1 (libmh twin)",
                    ],
                ),
                "mixed",
            )[0],
        )
        # the hash comparison
        global HASH_DIR
        old = HASH_DIR
        HASH_DIR = os.path.join(tmp, "hashes")
        os.makedirs(HASH_DIR)
        json.dump({"1": "a", "2": "b"}, open(os.path.join(HASH_DIR, "hash_x__ship.json"), "w"))
        json.dump(
            {"1": "a", "2": "b", "3": "c"}, open(os.path.join(HASH_DIR, "hash_x__lib.json"), "w")
        )
        json.dump({"1": "a", "2": "Z"}, open(os.path.join(HASH_DIR, "hash_x__bad.json"), "w"))
        expect("hash-cmp identical", hash_cmp("x", "ship", "lib") == 0)
        expect("hash-cmp differs", hash_cmp("x", "ship", "bad") == 1)
        HASH_DIR = old
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("check_m1h selftest: %d/%d" % (total - bad, total))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--case", choices=sorted(CASES))
    ap.add_argument("--arm", choices=["ship", "orig", "lib", "off", "mixed"])
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--hash-cmp", nargs=3, metavar=("CASE", "ARM_A", "ARM_B"))
    ap.add_argument(
        "--mp", action="store_true", help="the 2-peer rows: two session dirs (host, client)"
    )
    ap.add_argument(
        "--vs",
        choices=["ship", "lib"],
        help="also demand a step-identical hash stream against the one an earlier step saved for this arm (same --tag)",
    )
    ap.add_argument(
        "--expect-diff",
        action="store_true",
        help="with --vs: the KNOWN-DIVERGENCE tripwire -- pass only if the pair is NOT step-identical",
    )
    ap.add_argument("--tag", help="name the saved hash stream hash_<tag>__<arm>.json (bisect rows)")
    ap.add_argument("run_dir", nargs="*")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.hash_cmp:
        return hash_cmp(*a.hash_cmp)
    if a.mp:
        if len(a.run_dir) != 2 or a.arm not in ("ship", "lib", "mixed"):
            ap.error(
                "usage: check_m1h.py --mp --arm ship|lib|mixed <host-session-dir> <client-session-dir>"
            )
        fails, peers = check_mp(a.run_dir[0], a.run_dir[1], a.arm)
        for name, f in peers:
            print(
                "  %s: hits %s; nd totals %s"
                % (
                    name,
                    {
                        k: v
                        for k, v in sorted(
                            hits_of(
                                f,
                                "lib"
                                if (a.arm == "lib" or (a.arm == "mixed" and name == "client"))
                                else "ship",
                            ).items()
                        )
                        if k in HIT_KEY
                    },
                    sorted(r[1] for r in f["nd"] if r[0] == "T"),
                )
            )
        if fails:
            for x in fails:
                print("FAIL " + x)
            print("check_m1h: FAIL (%d clause(s)) [mp %s]" % (len(fails), a.arm))
            return 1
        print("check_m1h: PASS [mp %s]" % a.arm)
        return 0
    if not (a.case and a.arm and len(a.run_dir) == 1):
        ap.error(
            "usage: check_m1h.py --case <f1..f10|d2|d5|d7> --arm ship|orig|lib|off <session-run-dir>"
        )
    a.run_dir = a.run_dir[0]
    fails, f = check(a.run_dir, a.case, a.arm)
    label = a.tag or a.case
    nh = save_hashes(a.run_dir, label, a.arm)
    if a.vs:
        # twin == splice, step by step: this step's stream against the one an EARLIER step of the same row saved for --vs ARM
        try:
            other = json.load(open(os.path.join(HASH_DIR, "hash_%s__%s.json" % (label, a.vs))))
        except OSError:
            other = None
        mine = json.load(open(os.path.join(HASH_DIR, "hash_%s__%s.json" % (label, a.arm))))
        if other is None:
            fails.append(
                "--vs %s: no saved hash stream hash_%s__%s.json (the %s step did not run first)"
                % (a.vs, label, a.vs, a.vs)
            )
        else:
            common = sorted(set(mine) & set(other), key=int)
            bad = next((st for st in common if mine[st] != other[st]), None)
            print(
                "  vs %s: %d common steps (%s %d, %s %d), first differing step %s"
                % (a.vs, len(common), a.vs, len(other), a.arm, len(mine), bad)
            )
            if len(common) < 1200:
                fails.append(
                    "--vs %s: only %d common hashed steps, too few to call the pair identical"
                    % (a.vs, len(common))
                )
            if a.expect_diff:
                # the tripwire: every other clause is ignored, only the divergence itself is asserted
                fails = []
                if bad is None:
                    fails.append(
                        "--expect-diff: the pair is now step-identical over %d steps -- the divergence this row pins is FIXED; drop the row and promote the fixture into m1h_twin_eq"
                        % len(common)
                    )
                elif len(common) < 1200:
                    fails.append("--expect-diff: only %d common steps" % len(common))
            elif bad is not None:
                fails.append(
                    "--vs %s: the per-step state hash differs from step %s on (%d common steps): %s is not step-identical to %s"
                    % (a.vs, bad, len(common), a.arm, a.vs)
                )
    hits = hits_of(f, a.arm)
    print(
        "  case %s arm %s: hits %s; orders %s; %d hash steps saved"
        % (
            a.case,
            a.arm,
            {k: v for k, v in sorted(hits.items()) if k in HIT_KEY},
            {k: (v["arrived"], v["gave_up"], v["dead"]) for k, v in f["orders"].items()},
            nh,
        )
    )
    if fails:
        for x in fails:
            print("FAIL " + x)
        print("check_m1h: FAIL (%d clause(s)) [%s %s]" % (len(fails), a.case, a.arm))
        return 1
    print("check_m1h: PASS [%s %s]" % (a.case, a.arm))
    return 0


if __name__ == "__main__":
    sys.exit(main())
