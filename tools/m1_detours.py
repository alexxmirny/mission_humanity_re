#!/usr/bin/env python3
"""m1_detours.py -- offline detour analysis of the `[harness] mv_replay` movement probe (mods:M1D).

Reads the `; MV ...` lines of one mh_harness.log (the probe is observe-only; see harness.cpp "mods:M1D") and,
for EVERY unit and EVERY order, compares what the game did with what it could have done on the static map of
that moment:

  actual    the tiles the unit really stepped on (cardinal 1, diagonal 1.4), from the `MV T` tile-change lines
  shortest  the shortest 8-neighbour path on the map the order saw (torus wrap; buildings/trees/terrain blocked,
            units NOT blocked; a diagonal needs the target open AND at least one of its two side tiles open --
            the planner's flank rule, libmh sim_map_region_routing.cpp route_search / the member walk)
            from the unit's tile at dispatch to (a) the click tile and (b) its own assigned destination tile
  planned   the legs the game wrote into the unit's path slot at its first wave (`MV WAVEM`), before any replan
  router    this file re-implements the game's two routing stages exactly (flood_reachable: 4-neighbour flood
            whose per-tile cost is the low byte of region_cell.terrain_flags; route_search: descent over the 8
            neighbours with the flank guard) so the centroid route can be compared with what the cost field makes
            of it; the logged flood field (`MV FLD`, orders named in [harness] mv_field) validates the model

Excess is attributed (tiles, 1.0 per cardinal / 1.4 per diagonal):

  (i)   goal assignment   shortest(start -> own destination) - shortest(start -> click)
  (iii) router cost shape the CENTROID route's length - shortest(centroid -> group goal)
  (ii)  group offset      (planned - shortest(start -> own destination)) - (iii)
  (iv)  blocking/replans  actual - planned, when the unit walked its whole plan (else "n/a")
  (v)   re-order          the order was cut short by the next order (tiles wasted are the tiles walked)

Usage:
  python tools/m1_detours.py LOG [--out FILE] [--maps 11,17] [--model]
  python tools/m1_detours.py --exp NAME=LOG [--exp NAME2=LOG2 ...] [--out FILE]   # group-vs-single comparison

Persistent (not a one-off) because every later change to ground movement (mods:M1) wants the same before/after
table; the inputs are probe logs under tmp/, so it is hand-run (tools/data/tool_dispositions.json).
"""

import argparse
import heapq
import math
import re
import sys
from collections import defaultdict

DIAG = 1.4
# 24-heading step table, the 8 headings the planner emits (pipeline.md section 3; _G_LLM_MAP_DIR_STEP_DELTAS)
HDELTA = {
    1: (0, 1),
    4: (-1, 1),
    7: (-1, 0),
    10: (-1, -1),
    13: (0, -1),
    16: (1, -1),
    19: (1, 0),
    22: (1, 1),
}
# route_search direction index -> heading code (even = cardinal, odd = diagonal), _G_LLM_MAP_REGION_ROUTE_DIR_CODES
RDIRS = [1, 4, 7, 10, 13, 16, 19, 22]
BLOCKED = set(
    "#BtX"
)  # terrain / building / tree / other object classes; 'u' (settled unit) and 'm' (stepping) are open


def wrapd(d, n):
    d %= n
    return d - n if d > n // 2 else d


def step_cost(dx, dy):
    return DIAG if dx != 0 and dy != 0 else 1.0


# ---------------------------------------------------------------------------------------------------------
# parsing
# ---------------------------------------------------------------------------------------------------------
KV = re.compile(r"(\w+)=(\S*)")


def parse_kv(rest):
    out = {}
    for tag in ("legs=", "route="):
        i = rest.find(tag)
        if i >= 0:
            out[tag[:-1]] = rest[i + len(tag) :].strip()
            rest = rest[:i]
    for k, v in KV.findall(rest):
        out.setdefault(k, v)
    return out


def i2(s):
    a, b = s.split(",")
    return int(a), int(b)


def parse_legs(s):
    legs = []
    for tok in s.split():
        h, r = tok.split("/")
        legs.append((int(h), int(r)))
    return legs


class Log:
    def __init__(self, path):
        self.path = path
        self.mw = self.mh = 64
        self.maps = defaultdict(dict)  # ord -> {y: row string}
        self.cost = defaultdict(dict)  # ord -> {y: hex string}
        self.fld = defaultdict(dict)  # wave n -> {y: [values]}
        self.orders = {}  # ord -> dict(step, goal)
        self.start = {}  # (ord, u) -> (x, y)
        self.T = defaultdict(list)  # (ord, u) -> [(step, d, x, y)]
        self.disp = {}  # (ord, u) -> dict(step, goal)
        self.waves = {}  # n -> dict(step, ord, centroid, goal, route, members)
        self.wavem = defaultdict(list)  # (ord, u) -> [dict(n, from, tile, slot, end, tiles, legs)]
        self.repath = defaultdict(list)  # (ord, u) -> [dict(step, legs, ...)]
        self.replan = defaultdict(list)
        self.verdict = {}  # (ord, u) -> dict
        self.stalls = defaultdict(list)
        self.roster = {}
        self.ordersum = {}
        self.pathlines = defaultdict(list)
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if not line.startswith("; MV "):
                    continue
                self._line(line[5:].rstrip("\n"))

    def _line(self, s):
        tag, _, rest = s.partition(" ")
        if tag == "MAP":
            o, y, row = rest.split(" ", 2)
            self.maps[int(o.split("=")[1])][int(y.split("=")[1])] = row
            return
        if tag in ("COST",):
            o, y, row = rest.split(" ", 2)
            self.cost[int(o.split("=")[1])][int(y.split("=")[1])] = row
            return
        if tag == "FLD":
            parts = rest.split()
            n = int(parts[0].split("=")[1])
            y = int(parts[2].split("=")[1])
            vals = []
            for tok in parts[3:]:
                vals.append(0 if tok == "." else (-1 if tok == "#" else int(tok)))
            self.fld[n][y] = vals
            return
        kv = parse_kv(rest)
        if tag == "BEGIN":
            m = re.match(r"(\d+)x(\d+)", kv.get("map", "64x64"))
            self.mw, self.mh = int(m.group(1)), int(m.group(2))
        elif tag == "ORDER":
            self.orders[int(kv["ord"])] = {"step": int(kv["step"]), "goal": i2(kv["goal"])}
        elif tag == "START":
            self.start[(int(kv["ord"]), int(kv["u"]))] = i2(kv["xy"])
        elif tag == "T":
            self.T[(int(kv["ord"]), int(kv["u"]))].append(
                (int(kv["step"]), int(kv["d"]), *i2(kv["xy"]))
            )
        elif tag == "DISPATCH":
            self.disp[(int(kv["ord"]), int(kv["u"]))] = {
                "step": int(kv["step"]),
                "goal": i2(kv["goal"]),
            }
        elif tag == "WAVE":
            self.waves[int(kv["n"])] = {
                "step": int(kv["step"]),
                "ord": int(kv["ord"]),
                "centroid": i2(kv["centroid"]),
                "goal": i2(kv["goal"]),
                "mode": int(kv.get("mode", -1)),
                "members": int(kv["members"]),
                "route": parse_legs(kv.get("route", "")),
                "mem": [],
            }
        elif tag == "WAVEM":
            n = int(kv["n"])
            d = {
                "n": n,
                "u": int(kv["u"]),
                "from": i2(kv["from"]),
                "tile": i2(kv["tile"]),
                "slot": int(kv["slot"]),
                "end": i2(kv["end"]),
                "tiles": int(kv["tiles"]),
                "legs": parse_legs(kv.get("legs", "")),
            }
            if n in self.waves:
                self.waves[n]["mem"].append(d)
            self.wavem[(int(kv["ord"]), int(kv["u"]))].append(d)
        elif tag == "REPATH":
            self.repath[(int(kv["ord"]), int(kv["u"]))].append(
                {
                    "step": int(kv["step"]),
                    "xy": i2(kv["xy"]),
                    "legs": parse_legs(kv.get("legs", "")),
                }
            )
        elif tag == "REPLAN":
            self.replan[(int(kv["ord"]), int(kv["u"]))].append(
                {"step": int(kv["step"]), "xy": i2(kv["xy"]), "reason": kv.get("reason", "")}
            )
        elif tag == "VERDICT":
            words = rest.split()
            kv["verdict"] = words[2]
            self.verdict[(int(kv["ord"]), int(kv["u"]))] = kv
        elif tag == "STALL":
            self.stalls[(int(kv["ord"]), int(kv["u"]))].append(
                {
                    "step": int(kv["step"]),
                    "retry": kv.get("retry"),
                    "blocker": kv.get("blocker"),
                    "xy": i2(kv["xy"]),
                }
            )
        elif tag == "ORDERSUM":
            self.ordersum[int(kv["ord"])] = kv
        elif tag == "ROSTER":
            self.roster[int(kv["u"])] = kv

    def grid(self, o):
        g = self.maps.get(o) or self.maps.get(-1)
        return [g[y] for y in range(self.mh)]


# ---------------------------------------------------------------------------------------------------------
# geometry on the static map
# ---------------------------------------------------------------------------------------------------------
class Map:
    def __init__(self, rows, mw, mh, planner_view=False):
        self.mw, self.mh = mw, mh
        self.rows = rows
        self.planner = planner_view
        self.extra_open = (
            set()
        )  # tiles forced open (the wave's members are lifted off the collision map)

    def ch(self, x, y):
        return self.rows[y % self.mh][x % self.mw]

    def open(self, x, y):
        c = self.ch(x, y)
        if (x % self.mw, y % self.mh) in self.extra_open:
            return True
        if self.planner:
            return c in ".m?"  # the planner also sees settled units (passable 0) as walls
        return c not in BLOCKED

    def dijkstra(self, sx, sy):
        INF = float("inf")
        dist = {(sx % self.mw, sy % self.mh): 0.0}
        prev = {}
        pq = [(0.0, sx % self.mw, sy % self.mh)]
        while pq:
            d, x, y = heapq.heappop(pq)
            if d > dist.get((x, y), INF):
                continue
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    if dx == 0 and dy == 0:
                        continue
                    nx, ny = (x + dx) % self.mw, (y + dy) % self.mh
                    if not self.open(nx, ny):
                        continue
                    if dx and dy and not (self.open(x + dx, y) or self.open(x, y + dy)):
                        continue  # flank rule: at least one side tile open
                    nd = d + step_cost(dx, dy)
                    if nd < dist.get((nx, ny), INF) - 1e-9:
                        dist[(nx, ny)] = nd
                        prev[(nx, ny)] = (x, y)
                        heapq.heappush(pq, (nd, nx, ny))
        return dist, prev

    def path_to(self, prev, start, goal):
        p = [goal]
        while p[-1] != start:
            if p[-1] not in prev:
                return []
            p.append(prev[p[-1]])
        return p[::-1]


def near_dist(m, dist, tile, dist_to_nearest=True):
    """Distance to `tile`; a closed tile (the game moves such a goal) falls back to its nearest open neighbour."""
    t = (tile[0] % m.mw, tile[1] % m.mh)
    if t in dist:
        return dist[t], t
    best = None
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            q = ((t[0] + dx) % m.mw, (t[1] + dy) % m.mh)
            if q in dist and (best is None or dist[q] < best[0]):
                best = (dist[q], q)
    return best if best else (float("inf"), t)


def expand_legs(start, legs, mw, mh):
    pts = [start]
    x, y = start
    odd = False
    for h, r in legs:
        if h not in HDELTA:
            odd = True
            continue
        dx, dy = HDELTA[h]
        for _ in range(r):
            x, y = (x + dx) % mw, (y + dy) % mh
            pts.append((x, y))
    return pts, odd


def path_len(pts, mw, mh):
    s = 0.0
    for a, b in zip(pts, pts[1:]):
        s += step_cost(wrapd(b[0] - a[0], mw), wrapd(b[1] - a[1], mh))
    return s


# ---------------------------------------------------------------------------------------------------------
# the game's router, re-implemented (sim_map_region_routing.cpp flood_reachable + route_search)
# ---------------------------------------------------------------------------------------------------------
def router_field(m, low, goal):
    """flood_reachable: value(seed)=1; a neighbour gets value(cur)+low(cur) when that improves it. 4-neighbour."""
    mw, mh = m.mw, m.mh
    val = {}
    g = (goal[0] % mw, goal[1] % mh)
    val[g] = 1
    pq = [(1, g)]
    while pq:
        v, c = heapq.heappop(pq)
        if v > val.get(c, 1 << 60):
            continue
        pv = v + low[c[1]][c[0]]
        for dx, dy in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            n = ((c[0] + dx) % mw, (c[1] + dy) % mh)
            if not m.open(*n):
                continue
            if pv < val.get(n, 1 << 60):
                val[n] = pv
                heapq.heappush(pq, (pv, n))
    return val


def router_descend(m, val, start, goal, limit=600):
    """route_search: from the current cell take the strictly-lowest neighbour value (cardinals first, then diagonals that
    also pass the flank guard); stop on the goal or a local minimum."""
    mw, mh = m.mw, m.mh
    cur = (start[0] % mw, start[1] % mh)
    goal = (goal[0] % mw, goal[1] % mh)
    pts = [cur]
    for _ in range(limit):
        if cur == goal:
            break
        best, bdir = val.get(cur, 0), -1
        cand = {}
        for d in range(8):
            dx, dy = HDELTA[RDIRS[d]]
            cand[d] = ((cur[0] + dx) % mw, (cur[1] + dy) % mh)
        for d in (0, 2, 4, 6):
            v = val.get(cand[d], 0)
            if v != 0 and v < best:
                best, bdir = v, d
        for d in (1, 3, 5, 7):
            v = val.get(cand[d], 0)
            if v == 0:
                continue
            if not (val.get(cand[(d - 1) % 8], 0) or val.get(cand[(d + 1) % 8], 0)):
                continue
            if v < best:
                best, bdir = v, d
        if bdir < 0:
            break
        cur = cand[bdir]
        pts.append(cur)
    return pts


def low_plane(log, ordn=-1):
    c = log.cost.get(ordn) or log.cost.get(-1)
    if not c:
        return None
    low = [[0] * log.mw for _ in range(log.mh)]
    for y in range(log.mh):
        row = c[y]
        for x in range(log.mw):
            low[y][x] = int(row[2 * x : 2 * x + 2], 16)
    return low


# ---------------------------------------------------------------------------------------------------------
# per unit-order analysis
# ---------------------------------------------------------------------------------------------------------
def actual_track(log, o, u):
    ts = log.T.get((o, u), [])
    v = log.verdict.get((o, u))
    end_step = int(v["step"]) if v else 1 << 60
    pre = [t for t in ts if t[1] == 0]
    st = pre[-1][2:] if pre else log.start.get((o, u))
    pts = [st]
    for step, d, x, y in ts:
        if d == 1 and step <= end_step:
            pts.append((x, y))
    return pts


def analyze(log):
    rows = []
    for (o, u), v in sorted(log.verdict.items()):
        if o not in log.orders:
            continue
        click = log.orders[o]["goal"]
        m = Map(log.grid(o), log.mw, log.mh)
        track = actual_track(log, o, u)
        st = track[0]
        end = i2(v["end"])
        dist, prev = m.dijkstra(*st)
        s_click, click_t = near_dist(m, dist, click)
        wm = [w for w in log.wavem.get((o, u), []) if log.waves.get(w["n"], {}).get("ord") == o]
        w0 = wm[0] if wm else None
        goal_t = w0["tile"] if w0 else None
        s_goal = near_dist(m, dist, goal_t)[0] if goal_t else float("nan")
        s_end = near_dist(m, dist, end)[0]
        A = path_len(track, log.mw, log.mh)
        planned, plan_len, plan_end, odd = [], float("nan"), None, False
        if w0:
            planned, odd = expand_legs(w0["from"], w0["legs"], log.mw, log.mh)
            plan_len = path_len(planned, log.mw, log.mh)
            plan_end = planned[-1]
        wave = log.waves.get(w0["n"]) if w0 else None
        cen_len = s_cen = float("nan")
        cen_pts = []
        cerr = 0
        if wave:
            # torus-correct mean of the wave's members against the centroid the game used (the 0x80 wrap constant)
            ms = [d["from"] for d in wave["mem"]]
            if len(ms) > 1:
                ax = sum(wrapd(q[0] - ms[0][0], log.mw) for q in ms[1:])
                ay = sum(wrapd(q[1] - ms[0][1], log.mh) for q in ms[1:])
                tcx = (int(ax / len(ms)) + ms[0][0]) % log.mw
                tcy = (int(ay / len(ms)) + ms[0][1]) % log.mh
                cerr = max(
                    abs(wrapd(tcx - wave["centroid"][0], log.mw)),
                    abs(wrapd(tcy - wave["centroid"][1], log.mh)),
                )
            cen_pts, _ = expand_legs(wave["centroid"], wave["route"], log.mw, log.mh)
            cen_len = path_len(cen_pts, log.mw, log.mh)
            cd, _ = m.dijkstra(*wave["centroid"])
            s_cen = near_dist(m, cd, wave["goal"])[0]
        verdict = v["verdict"]
        completed = verdict in ("ARRIVED", "ARRIVED-NEAR", "STOPPED-SHORT", "GAVE-UP")
        finished_plan = plan_end is not None and (
            end == plan_end or (completed and track[-1] == plan_end)
        )
        # first divergence from the plan
        div = None
        if planned:
            k = 0
            while k < len(track) and k < len(planned) and track[k] == planned[k]:
                k += 1
            if k < len(track):
                div = k  # track[k] is the first tile that is not the planned tile k
        a_tiles = len(track) - 1
        prog = s_click - s_end if s_click != float("inf") else float("nan")
        g_i = s_goal - s_click if goal_t else float("nan")
        r_iii = cen_len - s_cen if wave else float("nan")
        o_ii = (plan_len - s_goal) - r_iii if (goal_t and wave) else float("nan")
        b_iv = (A - plan_len) if (finished_plan and planned) else None
        rows.append(
            dict(
                ord=o,
                u=u,
                verdict=verdict,
                start=st,
                end=end,
                click=click,
                goal=goal_t,
                s_click=s_click,
                s_goal=s_goal,
                s_end=s_end,
                plan_len=plan_len,
                plan_tiles=len(planned) - 1 if planned else None,
                A=A,
                a_tiles=a_tiles,
                prog=prog,
                g_i=g_i,
                r_iii=r_iii,
                o_ii=o_ii,
                b_iv=b_iv,
                cen_len=cen_len,
                s_cen=s_cen,
                div=div,
                replans=int(v["replans"]),
                stalls=int(v["stalls"]),
                d_click=int(v["d_click"]),
                interrupted=verdict in ("INTERRUPTED", "STILL-MOVING-AT-END"),
                track=track,
                planned=planned,
                shortest=m.path_to(prev, (st[0] % log.mw, st[1] % log.mh), click_t)
                if click_t in dist
                else [],
                cen_pts=cen_pts,
                wave=wave,
                odd=odd,
                cerr=cerr,
                repaths=len(log.repath.get((o, u), [])),
            )
        )
    return rows


def cause(r):
    """The dominant cause of a unit-order's excess, with the numbers that decided it."""
    if r["verdict"] == "NEVER-DISPATCHED":
        return "not dispatched"
    comps = {}
    if r["goal"] and not math.isnan(r["g_i"]):
        comps["(i) own goal != click"] = r["g_i"]
    if not math.isnan(r["r_iii"]):
        comps["(iii) router cost shape"] = r["r_iii"]
    if not math.isnan(r["o_ii"]):
        comps["(ii) group offset"] = r["o_ii"]
    if r["b_iv"] is not None:
        comps["(iv) blocking/replans"] = r["b_iv"]
    tag = ""
    if r["cerr"] >= 3:
        tag = "centroid wrap bug (game centroid %d tiles off); " % r["cerr"]
    if r["verdict"] == "STOPPED-SHORT":
        tag += "stopped short (d=%d); " % r["d_click"]
    elif r["interrupted"]:
        tag += "(v) re-ordered after %d tiles; " % r["a_tiles"]
    pos = {k: v for k, v in comps.items() if v >= 2.0}
    if not pos:
        return tag + "efficient (no component >= 2 tiles)"
    k = max(pos, key=pos.get)
    return tag + "%s %+.0f" % (k, pos[k])


def fmt(x, w=5):
    if x is None:
        return "n/a"
    if isinstance(x, float):
        if math.isnan(x):
            return "n/a"
        if x == float("inf"):
            return "inf"
        return "%.1f" % x
    return str(x)


def path_cost(pts, low, mw, mh):
    """Router cost of walking a tile path: the sum of the low bytes of the tiles it leaves (the goal tile excluded)."""
    return sum(low[y % mh][x % mw] for x, y in pts[:-1])


def ring_blame(log, ordn, pts):
    """For path tiles with an obstacle-proximity ring (low byte > 1): which kind of obstacle is nearest (Chebyshev <= 5)?
    Returns {kind: tiles} with kinds '#' cliff/water/rock, 'B' building, 't' tree, 'u' unit, '?' none found."""
    g = log.grid(ordn)
    low = low_plane(log)
    out = defaultdict(int)
    for x, y in pts[:-1]:
        if low[y % log.mh][x % log.mw] <= 1:
            continue
        best, kind = 99, "?"
        for dx in range(-5, 6):
            for dy in range(-5, 6):
                c = g[(y + dy) % log.mh][(x + dx) % log.mw]
                if c in "#BtXu":
                    d = max(abs(dx), abs(dy))
                    if d < best or (d == best and c == "B"):
                        best, kind = d, c
        out[kind] += 1
    return dict(out)


def table_cost(log, rows):
    low = low_plane(log)
    if not low:
        return ""
    out = [
        "| ord | unit | short path (tiles / weighted) | router cost of the shortest path | planned route (tiles / weighted) | router cost of the planned route | ring tiles on the shortest path by nearest obstacle | ring tiles on the planned route by nearest obstacle |",
        "| --- | --- | --- | ---: | --- | ---: | --- | --- |",
    ]
    for r in rows:
        if not r["shortest"] or not r["planned"]:
            continue
        sp, pp = r["shortest"], r["planned"]
        bs, bp = ring_blame(log, r["ord"], sp), ring_blame(log, r["ord"], pp)
        out.append(
            "| %d | %d | %d / %s | %d | %d / %s | %d | %s | %s |"
            % (
                r["ord"],
                r["u"],
                len(sp) - 1,
                fmt(path_len(sp, log.mw, log.mh)),
                path_cost(sp, low, log.mw, log.mh),
                len(pp) - 1,
                fmt(path_len(pp, log.mw, log.mh)),
                path_cost(pp, low, log.mw, log.mh),
                ", ".join("%s:%d" % kv for kv in sorted(bs.items())) or "-",
                ", ".join("%s:%d" % kv for kv in sorted(bp.items())) or "-",
            )
        )
    return "\n".join(out)


def table(rows):
    out = [
        "| ord | unit | outcome | start | click | own goal | short->click | short->goal | planned | actual | actual tiles | ratio | (i) goal | (iii) cost shape | (ii) offset | (iv) blocking | replans | stalls | dominant cause |",
        "| --- | --- | --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |",
    ]
    for r in rows:
        if r["verdict"] in ("ARRIVED", "ARRIVED-NEAR"):
            ratio = r["A"] / r["s_click"] if r["s_click"] > 0 else float("nan")
            rs = fmt(ratio)
        elif r["interrupted"] or r["verdict"] in ("STOPPED-SHORT", "GAVE-UP"):
            # partial: walked length against the progress it bought (shortest(start->click) - shortest(end->click))
            rs = ("%s*" % fmt(r["A"] / r["prog"])) if r["prog"] and r["prog"] > 0.5 else "n/a*"
        else:
            rs = "n/a"
        out.append(
            "| %d | %d | %s | %d,%d | %d,%d | %s | %s | %s | %s | %s | %d | %s | %s | %s | %s | %s | %d | %d | %s |"
            % (
                r["ord"],
                r["u"],
                r["verdict"],
                r["start"][0],
                r["start"][1],
                r["click"][0],
                r["click"][1],
                ("%d,%d" % r["goal"]) if r["goal"] else "n/a",
                fmt(r["s_click"]),
                fmt(r["s_goal"]),
                fmt(r["plan_len"]),
                fmt(r["A"]),
                r["a_tiles"],
                rs,
                fmt(r["g_i"]),
                fmt(r["r_iii"]),
                fmt(r["o_ii"]),
                fmt(r["b_iv"]),
                r["replans"],
                r["stalls"],
                cause(r),
            )
        )
    return "\n".join(out)


# ---------------------------------------------------------------------------------------------------------
# ASCII maps
# ---------------------------------------------------------------------------------------------------------
def circ_window(vals, n, margin):
    """Smallest circular window (start, length) of 0..n-1 that holds every value plus a margin: the complement of the
    largest empty circular gap."""
    occ = sorted({v % n for v in vals})
    if not occ:
        return 0, n
    best_gap, best_start = -1, 0
    for i, v in enumerate(occ):
        nxt = occ[(i + 1) % len(occ)]
        gap = (nxt - v - 1) % n if len(occ) > 1 else n - 1
        if gap > best_gap:
            best_gap, best_start = gap, nxt
    length = n - best_gap + 2 * margin
    if length >= n:
        return 0, n
    return (best_start - margin) % n, length


def _draw(log, o, cells, marks, title, legend, margin=2):
    """cells: {(x, y): char} overlay (mod map size); marks: {(x, y): char} drawn on top. Torus-aware crop."""
    mw, mh = log.mw, log.mh
    m = Map(log.grid(o), mw, mh)
    pts = list(cells) + list(marks)
    x0, wx = circ_window([p[0] for p in pts], mw, margin)
    y0, wy = circ_window([p[1] for p in pts], mh, margin)
    lines = ["```", title]
    lines.append(
        "window x %d..%d, y %d..%d (torus %dx%d, wraps where the numbers do)"
        % (x0, (x0 + wx - 1) % mw, y0, (y0 + wy - 1) % mh, mw, mh)
    )
    hdr = "".join(
        ("%d" % (((x0 + i) % mw) // 10)) if ((x0 + i) % mw) % 10 == 0 else " " for i in range(wx)
    )
    lines.append("    " + hdr)
    for j in range(wy):
        y = (y0 + j) % mh
        row = []
        for i in range(wx):
            x = (x0 + i) % mw
            c = m.ch(x, y)
            if c == "m":
                c = "u"
            if (x, y) in marks:
                c = marks[(x, y)]
            elif (x, y) in cells:
                c = cells[(x, y)]
            row.append(c)
        lines.append("%3d %s" % (y, "".join(row)))
    lines.append(legend)
    lines.append("```")
    return "\n".join(lines)


def render(log, r, title, margin=2):
    mw, mh = log.mw, log.mh
    layers = {1: r["shortest"], 2: r["planned"], 4: r["track"]}
    bits = {}
    for b, pts in layers.items():
        for x, y in pts:
            bits[(x % mw, y % mh)] = bits.get((x % mw, y % mh), 0) | b
    cmap = {1: "*", 2: "+", 4: "@", 3: "x", 5: "&", 6: "%", 7: "$"}
    cells = {p: cmap[b] for p, b in bits.items()}
    marks = {
        (r["start"][0] % mw, r["start"][1] % mh): "S",
        (r["click"][0] % mw, r["click"][1] % mh): "C",
    }
    if r["goal"] and r["goal"] != r["click"]:
        marks[(r["goal"][0] % mw, r["goal"][1] % mh)] = "G"
    legend = (
        "legend: S start, C click, G own goal | # cliff/water, B building, t tree, u unit | * shortest, + planned (offset route), @ actual, "
        "x shortest+planned, & shortest+actual, % planned+actual, $ all three"
    )
    return _draw(log, r["ord"], cells, marks, title, legend, margin)


def render_group(log, o, rows_o, title, margin=2):
    """One map per order: the centroid route (c), every unit's actual track (its unit number mod 10), the shortest path of the first unit (*)."""
    mw, mh = log.mw, log.mh
    cells = {}
    for r in rows_o:
        if r["cen_pts"]:
            for x, y in r["cen_pts"]:
                cells[(x % mw, y % mh)] = "c"
            break
    for x, y in rows_o[0]["shortest"]:
        cells[(x % mw, y % mh)] = "-"
    for r in rows_o:
        for x, y in r["track"]:
            p = (x % mw, y % mh)
            cells[p] = str(r["u"] % 10) if cells.get(p) in (None, "c", "-") else "*"
    c = log.orders[o]["goal"]
    marks = {(c[0] % mw, c[1] % mh): "C"}
    legend = (
        "legend: C click | c centroid route (the group's one planned route) | - shortest path for unit %d | digit = unit number mod 10, its actual track | "
        "* two tracks overlap | # cliff/water, B building, t tree, u parked unit" % rows_o[0]["u"]
    )
    return _draw(log, o, cells, marks, title, legend, margin)


# ---------------------------------------------------------------------------------------------------------
# router model validation
# ---------------------------------------------------------------------------------------------------------
def rings_report(log):
    """Which obstacle kinds put the proximity ring (low-byte bit 16: an obstacle within 1 tile) on an open tile? Measured on
    the map at load: open tiles grouped by the set of obstacle kinds among their 8 neighbours."""
    low = low_plane(log)
    g = log.grid(-1)
    res = {}
    for y in range(log.mh):
        for x in range(log.mw):
            if g[y][x] != ".":
                continue
            kinds = set()
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    c = g[(y + dy) % log.mh][(x + dx) % log.mw]
                    if c in "#BtXu":
                        kinds.add(c)
            if kinds:
                a = res.setdefault("".join(sorted(kinds)), [0, 0])
                a[0] += 1
                a[1] += bool(low[y][x] & 16)
    names = "#=cliff/water/rock B=building t=tree u=parked unit"
    lines = [
        "adjacent-obstacle ring (cost bit 16) by the kinds of obstacle touching an open tile (%s):"
        % names
    ]
    for k, (n, r) in sorted(res.items()):
        lines.append(
            "  touches only {%s}: %d open tiles, %d carry the ring (%.0f%%)"
            % (k, n, r, 100.0 * r / n)
        )
    return "\n".join(lines)


def model_report(log):
    out = []
    low = low_plane(log)
    if not low or not log.waves:
        return "no COST plane or no waves in this log: router model skipped"
    out.append(
        "Router model check (flood = 4-neighbour, per-tile cost = low byte of terrain_flags; descent = 8-neighbour, flank guard):"
    )
    cs = defaultdict(int)
    for lv in low:
        for v in lv:
            cs[v] += 1
    out.append(
        "cost histogram over the map (low byte -> tiles): "
        + ", ".join("%d:%d" % (k, cs[k]) for k in sorted(cs))
    )
    # field validation
    for n, rowsf in sorted(log.fld.items()):
        w = log.waves.get(n)
        if not w:
            continue
        o = w["ord"]
        m = Map(log.grid(o), log.mw, log.mh, planner_view=True)
        m.extra_open = {tuple(d["from"]) for d in w["mem"]}
        val = router_field(m, low, w["goal"])
        same = diff = 0
        worse = []
        for y, vs in rowsf.items():
            for x, gv in enumerate(vs):
                if gv <= 0:
                    continue
                mv = val.get((x, y))
                if mv is None:
                    diff += 1
                    continue
                if mv == gv:
                    same += 1
                else:
                    diff += 1
                    if len(worse) < 4:
                        worse.append("(%d,%d) game %d model %d" % (x, y, gv, mv))
        out.append(
            "wave %d (order %d, goal %d,%d): logged flood field cells %d equal to the model, %d different%s"
            % (
                n,
                o,
                w["goal"][0],
                w["goal"][1],
                same,
                diff,
                (" e.g. " + "; ".join(worse)) if worse else "",
            )
        )
    # route reproduction for every wave
    ok = tot = same_len = 0
    notes = []
    for n, w in sorted(log.waves.items()):
        if not w["route"]:
            continue
        m = Map(
            log.grid(w["ord"]) if w["ord"] in log.maps else log.grid(-1),
            log.mw,
            log.mh,
            planner_view=True,
        )
        m.extra_open = {tuple(d["from"]) for d in w["mem"]}
        val = router_field(m, low, w["goal"])
        pts = router_descend(m, val, w["centroid"], w["goal"])
        real, _ = expand_legs(w["centroid"], w["route"], log.mw, log.mh)
        tot += 1
        if (
            abs(path_len(pts, log.mw, log.mh) - path_len(real, log.mw, log.mh)) < 1e-6
            and pts[-1] == real[-1]
        ):
            same_len += 1
        if pts == real:
            ok += 1
        elif len(notes) < 6:
            notes.append(
                "wave %d ord %d: model route %d tiles (len %.1f) vs game %d tiles (len %.1f)"
                % (
                    n,
                    w["ord"],
                    len(pts) - 1,
                    path_len(pts, log.mw, log.mh),
                    len(real) - 1,
                    path_len(real, log.mw, log.mh),
                )
            )
    out.append(
        "centroid route reproduced tile-for-tile by the model: %d of %d waves; same end tile and same weighted length: %d of %d%s"
        % (ok, tot, same_len, tot, ("; misses: " + " | ".join(notes)) if notes else "")
    )
    out.append(rings_report(log))
    return "\n".join(out)


# ---------------------------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("log", nargs="?")
    ap.add_argument("--out")
    ap.add_argument(
        "--maps", default="", help="comma list of orders to draw (per-unit maps + a group map)"
    )
    ap.add_argument(
        "--units",
        default="",
        help="with --maps: ORD:UNIT list (e.g. 11:2,17:2); only those unit maps are drawn (default: every unit)",
    )
    ap.add_argument(
        "--summary",
        action="store_true",
        help="print the aggregate statistics over completed unit-orders",
    )
    ap.add_argument("--maps-all", action="store_true", help="draw the group map of every order")
    ap.add_argument(
        "--model",
        action="store_true",
        help="validate the re-implemented router against the logged flood fields",
    )
    ap.add_argument(
        "--exp",
        action="append",
        default=[],
        help="NAME=LOG: group-vs-single comparison (repeatable)",
    )
    a = ap.parse_args()
    parts = []
    if a.exp:
        logs = {}
        for e in a.exp:
            n, _, p = e.partition("=")
            logs[n] = Log(p)
        parts.append(exp_report(logs))
    elif a.log:
        log = Log(a.log)
        rows = analyze(log)
        parts.append(
            "## Per unit and order\n\nlog: %s\nmap %dx%d, %d orders, %d unit-orders\n"
            % (a.log, log.mw, log.mh, len(log.orders), len(rows))
        )
        parts.append(table(rows))
        parts.append("\n## Router cost of the shortest path vs the planned route\n")
        parts.append(table_cost(log, rows))
        if a.summary:
            parts.append("\n## Summary\n\n```\n" + summary(rows) + "\n```")
        if a.model:
            parts.append("\n## Router model\n\n```\n" + model_report(log) + "\n```")
        want = [int(t) for t in a.maps.split(",") if t.strip()]
        for o in want:
            rows_o = [r for r in rows if r["ord"] == o]
            if not rows_o:
                continue
            parts.append(
                "\n### Order %d  click %d,%d\n"
                % (o, log.orders[o]["goal"][0], log.orders[o]["goal"][1])
            )
            parts.append(
                render_group(
                    log, o, rows_o, "order %d: centroid route and every unit's actual track" % o
                )
            )
            for r in rows_o:
                if a.units and ("%d:%d" % (o, r["u"])) not in a.units.split(","):
                    continue
                parts.append(
                    render(
                        log,
                        r,
                        "order %d unit %d (%s): shortest %s, planned %s, actual %s tiles-weighted"
                        % (
                            o,
                            r["u"],
                            r["verdict"],
                            fmt(r["s_click"]),
                            fmt(r["plan_len"]),
                            fmt(r["A"]),
                        ),
                    )
                )
        if a.maps_all:
            for o in sorted(log.orders):
                if o in want:
                    continue
                rows_o = [r for r in rows if r["ord"] == o]
                if rows_o:
                    parts.append(
                        render_group(
                            log, o, rows_o, "order %d (click %d,%d)" % (o, *log.orders[o]["goal"])
                        )
                    )
    else:
        ap.error("need LOG or --exp")
    text = "\n".join(parts) + "\n"
    if a.out:
        with open(a.out, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
    else:
        sys.stdout.write(text)


def summary(rows):
    done = [r for r in rows if r["verdict"] in ("ARRIVED", "ARRIVED-NEAR") and r["s_click"] > 0]
    out = []
    ratios = sorted(r["A"] / r["s_click"] for r in done)
    out.append("completed unit-orders (ARRIVED / ARRIVED-NEAR): %d of %d" % (len(done), len(rows)))
    if ratios:
        out.append(
            "ratio actual / shortest-to-click: min %.2f, median %.2f, mean %.2f, max %.2f; >= 1.3 in %d, >= 1.5 in %d, >= 2.0 in %d"
            % (
                ratios[0],
                ratios[len(ratios) // 2],
                sum(ratios) / len(ratios),
                ratios[-1],
                sum(1 for x in ratios if x >= 1.3),
                sum(1 for x in ratios if x >= 1.5),
                sum(1 for x in ratios if x >= 2.0),
            )
        )
        tot_ex = sum(r["A"] - r["s_click"] for r in done)
        comp = defaultdict(float)
        for r in done:
            for k, key in (
                ("(i) goal", "g_i"),
                ("(iii) cost shape", "r_iii"),
                ("(ii) offset", "o_ii"),
                ("(iv) blocking", "b_iv"),
            ):
                v = r[key]
                if v is not None and not (isinstance(v, float) and math.isnan(v)):
                    comp[k] += v
        out.append("total excess over the shortest path (tiles, summed): %.1f" % tot_ex)
        out.append("  attributed: " + ", ".join("%s %+.1f" % (k, v) for k, v in comp.items()))
    by = defaultdict(int)
    for r in rows:
        by[r["verdict"]] += 1
    out.append("outcomes: " + ", ".join("%s %d" % kv for kv in sorted(by.items())))
    cut = [r for r in rows if r["interrupted"]]
    out.append(
        "cut short by the next order: %d unit-orders, %.0f tiles walked (weighted) toward targets that were then replaced"
        % (len(cut), sum(r["A"] for r in cut))
    )
    return "\n".join(out)


def exp_report(logs):
    """Group-vs-single: one row per (arm, unit): outcome, steps-of-length actually walked vs shortest to the click."""
    out = ["## Controlled group-vs-single comparison\n"]
    out.append(
        "| arm | order | unit | outcome | start | end | short->click | short->end | actual | actual tiles | ratio vs click | route eff (actual/short->end) | replans | stalls |"
    )
    out.append(
        "| --- | --- | --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"
    )
    for name, log in logs.items():
        for r in analyze(log):
            eff = (
                r["A"] / r["s_end"]
                if r["s_end"] and r["s_end"] > 0 and r["s_end"] != float("inf")
                else float("nan")
            )
            rat = (
                r["A"] / r["s_click"]
                if r["verdict"] in ("ARRIVED", "ARRIVED-NEAR") and r["s_click"] > 0
                else float("nan")
            )
            out.append(
                "| %s | %d | %d | %s | %d,%d | %d,%d | %s | %s | %s | %d | %s | %s | %d | %d |"
                % (
                    name,
                    r["ord"],
                    r["u"],
                    r["verdict"],
                    *r["start"],
                    *r["end"],
                    fmt(r["s_click"]),
                    fmt(r["s_end"]),
                    fmt(r["A"]),
                    r["a_tiles"],
                    fmt(rat),
                    fmt(eff),
                    r["replans"],
                    r["stalls"],
                )
            )
    return "\n".join(out)


if __name__ == "__main__":
    main()
