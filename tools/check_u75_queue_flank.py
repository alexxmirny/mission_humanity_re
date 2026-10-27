#!/usr/bin/env python3
"""
tools/check_u75_queue_flank.py -- mp:U75: the storage-exit shuffle must not cut wall corners.

Player report 01M5WMNKXS2Q0PN3AJWWP5F48Q (triage 2026-10-27): when a garage empties, the "make room" shuffle
(llm_strat_unit_queue_advance_search 0x004d7f3a; also the idle-unit nudge's tile pick) steps units DIAGONALLY
between two blocked tiles -- through the corner of a cliff line, a step the route planner forbids -- and one
unit ended sealed in a one-tile notch. `[net] move_queue_flank_guard` (default on under improved_movement)
allows a diagonal step only past an open flank tile and caps the search depth.

The stimulus is the harness fixture (nd_door=4, mh_harness/harness.cpp qf_*): ~40 soldiers dock in the
5-kofu-c barracks, a diamond wall (radius qf_r, one gap, a few one-tile notches outside the rim) is raised
round its exit tile, and every soldier is undocked in ONE step ("exit all"). The probe logs the terrain window
(`; QF ROW`), every tile change of a soldier that has left the storage (`; QF MV`), the end tiles
(`; QF END`) and a `; QF TOTAL`. This checker replays them:

  corner cut   a tile change by exactly one diagonal step whose two flank tiles are BOTH terrain-blocked ('#')
  sealed       a soldier whose END tile lies in a 4-connected open component of <= 3 tiles that does not touch
               the window border (the notches; the dig's "sealed pockets of 1-3 tiles")

Arms (`--arm`):
  ship / lib      key on (the default). Clauses: the carrier line names the key (`ship`: a retail splice,
                  `lib`: the libmh twin); the probe ran (QF SETUP, QF EXIT, QF TOTAL, n >= 20, a wall of >= 20
                  tiles); 0 corner cuts; 0 sealed; the refusal counter fired (`move hits queue_flank` / `move
                  twin hits queue_flank`), so the guard really decided something; no deadlock: every soldier is
                  out, or the ones still inside are all waiting in EXIT_WAIT (0x22) -- never PARKED with the
                  order lost; `lib` also demands the same MV sequence as the saved `ship` run (`--vs ship`):
                  the twin and the splice are step-identical.
  ship_off / lib_off   key 0 (retail): the same probe, informational about the outcome but the NEGATIVE ARM:
                  > 0 corner cuts, and NO carrier line / refusal counter for the key.

  python tools/check_u75_queue_flank.py --arm ship|ship_off|lib|lib_off [--vs ship] <session-run-dir>
  python tools/check_u75_queue_flank.py --selftest
"""

import argparse
import json
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import check_cancel_task  # noqa: E402  (net_log_lines)
import check_storage_purge  # noqa: E402  (harness_log_lines)

SAVE_DIR = os.path.join(REPO, "tmp", "u75")
KEY = "move_queue_flank_guard"
CARRIER = re.compile(r"; move carrier \[net\] " + KEY + r"=(-?\d+): (.*)$")
HIT_SHIP = re.compile(r"; move hits .*\bqueue_flank=(\d+)")
HIT_LIB = re.compile(r"; move twin hits queue_flank=(\d+)")
ROW = re.compile(r"^; QF ROW y=(\d+) ([#BT.]+)\s*$")
SETUP = re.compile(
    r"^; QF SETUP step=(\d+) door=(\d+),(\d+) r=(\d+) gap=(-?\d+),(-?\d+) walled=(\d+) notches=(\d+) window=(\d+)"
    r"(?: map=(\d+),(\d+) rim=(\d+)/(\d+))?"
)
MV = re.compile(r"^; QF MV step=(\d+) el=(-?\d+) u=(\d+) (\d+),(\d+) (\d+),(\d+) st=0x([0-9a-f]+)")
LAND = re.compile(r"^; QF LAND step=(\d+) el=(-?\d+) u=(\d+) at (\d+),(\d+)")
END = re.compile(r"^; QF END u=(\d+) at (\d+),(\d+) st=0x([0-9a-f]+)")
TOTAL = re.compile(
    r"^; QF TOTAL steps=(\d+) n=(\d+) out=(\d+) waiting=(\d+) parked=(\d+) moves=(\d+) quiet=(\d+) docked_left=(\d+)"
)
EXIT_WAIT = 0x22
PARKED = 0x1F
MIN_UNITS = 20
MIN_WALL = 10


def parse(lines):
    f = {
        "setup": None,
        "rows": {},
        "mv": [],
        "land": [],
        "end": [],
        "total": None,
        "exit": None,
        "fail": [],
    }
    for ln in lines:
        if not ln.startswith("; QF ") and not ln.startswith("; DR FAIL"):
            continue
        if ln.startswith("; DR FAIL"):
            f["fail"].append(ln)
            continue
        m = SETUP.match(ln)
        if m:
            f["setup"] = tuple(int(x) if x is not None else 0 for x in m.groups())
            continue
        m = ROW.match(ln)
        if m:
            f["rows"][int(m.group(1))] = m.group(2)
            continue
        m = MV.match(ln)
        if m:
            g = m.groups()
            f["mv"].append(
                (int(g[0]), int(g[2]), int(g[3]), int(g[4]), int(g[5]), int(g[6]), int(g[7], 16))
            )
            continue
        m = LAND.match(ln)
        if m:
            f["land"].append(tuple(int(x) for x in m.groups()))
            continue
        m = END.match(ln)
        if m:
            f["end"].append(
                (int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4), 16))
            )
            continue
        m = TOTAL.match(ln)
        if m:
            f["total"] = tuple(int(x) for x in m.groups())
            continue
        if ln.startswith("; QF EXIT"):
            f["exit"] = ln
    return f


class Terrain:
    """The logged window as a blocked-tile set, in coordinates RELATIVE to the door and wrapped on the torus map
    (the map is 64 x 64 and a door near an edge has a window straddling it). Tiles outside the window are unknown
    (a flank outside the window is never counted blocked)."""

    def __init__(self, f):
        s = f["setup"]
        self.dx, self.dy, self.w = s[1], s[2], s[8]
        self.mw, self.mh = (s[9] or 64), (s[10] or 64)
        self.size = 2 * self.w + 1
        self.blocked = set()
        self.known = set()
        for y, row in f["rows"].items():
            if len(row) != self.size:
                continue
            ry = self.wrap(y - self.dy, self.mh)
            for i, ch in enumerate(row):
                t = (i - self.w, ry)
                self.known.add(t)
                if ch != ".":
                    self.blocked.add(t)

    @staticmethod
    def wrap(d, m):
        d %= m
        return d - m if d >= m // 2 else d

    def rel(self, x, y):
        return (self.wrap(x - self.dx, self.mw), self.wrap(y - self.dy, self.mh))

    def is_blocked(self, rx, ry):
        return (rx, ry) in self.blocked

    def components(self):
        """4-connected components of the open tiles of the window: [(tiles, touches_border)]"""
        seen, out = set(), []
        for t in sorted(self.known):
            if t in self.blocked or t in seen:
                continue
            stack, comp, border = [t], [], False
            seen.add(t)
            while stack:
                x, y = stack.pop()
                comp.append((x, y))
                if abs(x) == self.w or abs(y) == self.w:
                    border = True
                for n in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if n in self.known and n not in self.blocked and n not in seen:
                        seen.add(n)
                        stack.append(n)
            out.append((comp, border))
        return out


def corner_cuts(f, terr):
    cuts = []
    for step, u, px, py, x, y, st in f["mv"]:
        a, b = terr.rel(px, py), terr.rel(x, y)
        dx, dy = b[0] - a[0], b[1] - a[1]
        if abs(dx) != 1 or abs(dy) != 1:
            continue
        if terr.is_blocked(a[0] + dx, a[1]) and terr.is_blocked(a[0], a[1] + dy):
            cuts.append((step, u, (px, py), (x, y)))
    return cuts


def sealed_units(f, terr):
    comp_of = {}
    for comp, border in terr.components():
        small = len(comp) <= 3 and not border
        for t in comp:
            comp_of[t] = (small, len(comp))
    out = []
    for u, x, y, st in f["end"]:
        small, n = comp_of.get(terr.rel(x, y), (False, 0))
        if small:
            out.append((u, (x, y), n))
    return out


def jumps(f, terr):
    """tile changes that are not a single step (a teleport would make the cut count meaningless)"""
    out = []
    for m in f["mv"]:
        a, b = terr.rel(m[2], m[3]), terr.rel(m[4], m[5])
        if max(abs(b[0] - a[0]), abs(b[1] - a[1])) > 1:
            out.append(m)
    return out


def mv_digest(f):
    return {"mv": [list(m[:6]) for m in f["mv"]], "end": [list(e) for e in sorted(f["end"])]}


def check(run_dir, arm, vs=None, save=True):
    fails = []
    lines = check_storage_purge.harness_log_lines(run_dir)
    net = check_cancel_task.net_log_lines(run_dir)
    f = parse(lines)
    if f["fail"]:
        fails.append("the probe logged FAIL: %s" % f["fail"][0])
    if not f["setup"] or not f["total"] or not f["exit"]:
        fails.append(
            "the probe did not run to its end (SETUP %s, EXIT %s, TOTAL %s): %s"
            % (bool(f["setup"]), bool(f["exit"]), bool(f["total"]), f["fail"][:1])
        )
        return fails, f, {}
    setup, total = f["setup"], f["total"]
    n, out, waiting, parked = total[1], total[2], total[3], total[4]
    if n < MIN_UNITS:
        fails.append(
            "only %d soldiers docked and were undocked (want >= %d): the fixture has no crowd"
            % (n, MIN_UNITS)
        )
    rim = setup[11] if len(setup) > 11 and setup[12] else setup[6]
    if rim < MIN_WALL:
        fails.append(
            "only %d of the diamond's rim tiles are blocked (want >= %d)" % (rim, MIN_WALL)
        )
    terr = Terrain(f)
    if len(terr.known) != terr.size * terr.size:
        fails.append(
            "the terrain window is incomplete (%d of %d tiles)" % (len(terr.known), terr.size**2)
        )
    cuts = corner_cuts(f, terr)
    sealed = sealed_units(f, terr)
    jmp = jumps(f, terr)
    carrier = [CARRIER.search(ln) for ln in net]
    carrier = [m for m in carrier if m]
    hits_ship = [int(m.group(1)) for m in (HIT_SHIP.search(ln) for ln in net) if m]
    hits_lib = [int(m.group(1)) for m in (HIT_LIB.search(ln) for ln in net + lines) if m]
    refusals = max(hits_ship + hits_lib + [0])
    on = arm in ("ship", "lib")
    info = {
        "n": n,
        "out": out,
        "waiting": waiting,
        "parked": parked,
        "moves": len(f["mv"]),
        "cuts": len(cuts),
        "sealed": len(sealed),
        "jumps": len(jmp),
        "refusals": refusals,
        "walled": setup[6],
        "rim_blocked": rim,
        "notches": setup[7],
        "carrier": [m.group(2) for m in carrier],
    }
    if on:
        if not carrier:
            fails.append(
                "no `; move carrier [net] %s=1` line: the key was never reported in force" % KEY
            )
        else:
            text = carrier[-1].group(2)
            want = "retail splice" if arm == "ship" else "libmh twin"
            if want not in text:
                fails.append("carrier line says `%s`, want `%s`" % (text, want))
        if cuts:
            fails.append(
                "%d corner cut(s) with the key on (first: step %d unit %d %s -> %s)"
                % (len(cuts), cuts[0][0], cuts[0][1], cuts[0][2], cuts[0][3])
            )
        if sealed:
            fails.append("%d soldier(s) ended sealed in a notch: %s" % (len(sealed), sealed[:3]))
        if refusals == 0:
            fails.append(
                "the guard never refused anything (hit counter 0): the fixture did not press the shuffle against the wall"
            )
        if out != n and not (out + waiting == n and parked == 0):
            fails.append(
                "deadlock: %d of %d out, %d waiting in EXIT_WAIT, %d still PARKED (an undock order was lost)"
                % (out, n, waiting, parked)
            )
    else:
        if not cuts:
            fails.append(
                "key 0 produced NO corner cut: the negative arm does not reproduce the bug (fixture too gentle)"
            )
        if carrier:
            fails.append(
                "key 0 but the carrier reported the key in force: %s" % carrier[-1].group(2)
            )
        if refusals:
            fails.append("key 0 but the guard counter moved (%d)" % refusals)
    if jmp:
        fails.append(
            "%d non-adjacent tile jump(s) (first %s): the cut count is not trustworthy"
            % (len(jmp), jmp[0])
        )
    if save:
        os.makedirs(SAVE_DIR, exist_ok=True)
        json.dump(mv_digest(f), open(os.path.join(SAVE_DIR, "mv_%s.json" % arm), "w"))
    if vs:
        try:
            other = json.load(open(os.path.join(SAVE_DIR, "mv_%s.json" % vs)))
        except OSError:
            other = None
        mine = mv_digest(f)
        if other is None:
            fails.append("--vs %s: no saved move stream (the %s step did not run first)" % (vs, vs))
        elif other != mine:
            a, b = other["mv"], mine["mv"]
            i = next((k for k in range(min(len(a), len(b))) if a[k] != b[k]), min(len(a), len(b)))
            fails.append(
                "--vs %s: the soldiers' move stream differs (%d vs %d moves, first difference at move %d: %s / %s)"
                % (
                    vs,
                    len(a),
                    len(b),
                    i,
                    a[i] if i < len(a) else None,
                    b[i] if i < len(b) else None,
                )
            )
        else:
            info["vs"] = "%s: identical %d moves, %d end tiles" % (
                vs,
                len(mine["mv"]),
                len(mine["end"]),
            )
    return fails, f, info


def selftest():
    bad = 0

    def expect(name, cond):
        nonlocal bad
        print("  %s %s" % ("ok  " if cond else "FAIL", name))
        if not cond:
            bad += 1

    w = 3
    size = 2 * w + 1
    # door (10,10); a diagonal wall: (9,10) and (10,9) blocked, the rest open; a notch at (12,12) ringed by blocked tiles
    blocked = {(9, 10), (10, 9), (12, 11), (12, 13), (11, 12), (13, 12)}

    def rows():
        out = []
        for y in range(10 - w, 10 + w + 1):
            out.append(
                "; QF ROW y=%d %s"
                % (y, "".join("#" if (x, y) in blocked else "." for x in range(10 - w, 10 + w + 1)))
            )
        return out

    def log(mv, end, n=30, out=30, waiting=0, parked=0, walled=30):
        L = ["; QF SETUP step=1 door=10,10 r=4 gap=1,1 walled=%d notches=1 window=%d" % (walled, w)]
        L += rows() + ["; QF EXIT step=2 n=%d issued here" % n]
        for i, m in enumerate(mv):
            L.append(
                "; QF MV step=%d el=%d u=%d %d,%d %d,%d st=0x01"
                % ((10 + i,) + (10 + i, m[0]) + m[1] + m[2])
            )
        for u, x, y in end:
            L.append("; QF END u=%d at %d,%d st=0x01" % (u, x, y))
        L.append(
            "; QF TOTAL steps=9 n=%d out=%d waiting=%d parked=%d moves=%d quiet=1 docked_left=0"
            % (n, out, waiting, parked, len(mv))
        )
        return L

    cut = [(1, (9, 9), (10, 10))]  # (9,9)->(10,10): flanks (10,9) and (9,10) both blocked
    ok = [(1, (11, 11), (10, 10))]  # flanks (10,11) and (11,10) open
    fcut = parse(log(cut, [(1, 10, 10)]))
    t = Terrain(fcut)
    expect("corner cut detected", len(corner_cuts(fcut, t)) == 1)
    expect(
        "a flank with one open tile is not a cut",
        len(corner_cuts(parse(log(ok, [(1, 10, 10)])), t)) == 0,
    )
    expect(
        "a unit in the notch is sealed", len(sealed_units(parse(log(ok, [(1, 12, 12)])), t)) == 1
    )
    expect(
        "a unit in the open is not sealed", len(sealed_units(parse(log(ok, [(1, 10, 11)])), t)) == 0
    )

    def run(arm, L, carrier=None, hits=0):
        d = tempfile.mkdtemp()
        open(os.path.join(d, "mh_harness.log"), "w").write("\n".join(L) + "\n")
        net = []
        if carrier:
            net.append("[00:00:00.000] ; move carrier [net] %s=1: %s" % (KEY, carrier))
        if hits:
            net.append(
                "; move %shits queue_flank=%d%s"
                % (
                    "twin " if arm.startswith("lib") else "",
                    hits,
                    " (libmh twin)" if arm.startswith("lib") else "",
                )
            )
        open(os.path.join(d, "mh_net.log"), "w").write("\n".join(net) + "\n")
        return check(d, arm, save=False)[0]

    good = log(ok, [(1, 10, 10)])
    expect("ship ok", not run("ship", good, "retail splice", 3))
    expect(
        "ship: a corner cut is red", bool(run("ship", log(cut, [(1, 10, 10)]), "retail splice", 3))
    )
    expect(
        "ship: a sealed unit is red", bool(run("ship", log(ok, [(1, 12, 12)]), "retail splice", 3))
    )
    expect("ship: no carrier line is red", bool(run("ship", good, None, 3)))
    expect("ship: wrong carrier text is red", bool(run("ship", good, "libmh twin", 3)))
    expect("ship: zero refusals is red", bool(run("ship", good, "retail splice", 0)))
    expect(
        "ship: a parked soldier is red",
        bool(run("ship", log(ok, [(1, 10, 10)], out=28, waiting=1, parked=1), "retail splice", 3)),
    )
    expect(
        "ship: waiting soldiers are fine",
        not run("ship", log(ok, [(1, 10, 10)], out=28, waiting=2), "retail splice", 3),
    )
    expect(
        "ship: too few soldiers is red",
        bool(run("ship", log(ok, [(1, 10, 10)], n=5, out=5), "retail splice", 3)),
    )
    expect("lib ok", not run("lib", good, "libmh twin", 3))
    expect(
        "off: a corner cut is the expected outcome", not run("ship_off", log(cut, [(1, 10, 10)]))
    )
    expect("off: no corner cut is red", bool(run("ship_off", good)))
    expect(
        "off: a carrier line is red",
        bool(run("ship_off", log(cut, [(1, 10, 10)]), "retail splice")),
    )
    expect("a probe that never finished is red", bool(run("ship", good[:-1], "retail splice", 3)))
    print("check_u75_queue_flank selftest: %d failure(s)" % bad)
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--arm", choices=["ship", "ship_off", "lib", "lib_off"])
    ap.add_argument(
        "--vs",
        choices=["ship", "ship_off"],
        help="demand the same move stream as the saved run of that arm",
    )
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("run_dir", nargs="*")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.arm or len(a.run_dir) != 1:
        ap.error(
            "usage: check_u75_queue_flank.py --arm ship|ship_off|lib|lib_off <session-run-dir>"
        )
    vs = a.vs or ("ship" if a.arm == "lib" else ("ship_off" if a.arm == "lib_off" else None))
    fails, f, info = check(a.run_dir[0], a.arm, vs)
    print("  U75 %s: %s" % (a.arm, json.dumps(info, sort_keys=True)))
    if fails:
        for x in fails:
            print("FAIL " + x)
        print("check_u75_queue_flank: FAIL (%d clause(s)) [%s]" % (len(fails), a.arm))
        return 1
    print("check_u75_queue_flank: PASS [%s]" % a.arm)
    return 0


if __name__ == "__main__":
    sys.exit(main())
