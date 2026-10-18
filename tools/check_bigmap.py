#!/usr/bin/env python3
"""check_bigmap.py -- mods:MM4: the big minimap overlay shows while its key is held and leaves nothing behind.

seams/ui_bigmap.cpp logs to mh_net.log: one `; BIGMAP: shown WxH map, K px/tile at (X,Y) in a VWxVH view ...` line per
new geometry, and `; BIGMAP: hidden after N frame(s)` when the key is released. res_walk's `bigmap` segment holds the
key at 1024x768 (frames a before / b held / c after), then at 640x480 and 2560x1440 (held frames d, e). Read from the
process run directory test_ui hands over (the logs, and the capture_<name>.bmp frames):

  * the segment's log has a `shown` line for each of the three sizes (1024x768 -> an 864x736 view, 640x480 -> 480x448,
    2560x1440 -> 2400x1408) and at least three `hidden` lines (each hold ended);
  * every `shown` line has K >= 1 and the whole map inside the view (K * W + 2 * 7 <= VW, same for the height), and its
    average cost per presented frame is not logged as zero frames;
  * frame b differs from frame a over at least MIN_CHANGED of the view (the overlay and the dim are there);
  * THE RESIDUE TRAP: frame c (after release) matches frame a (before the hold) except for animated sprites: at most
    MAX_CHANGED of all pixels differ, and no 32x32 tile of the view differs in more than 90% of its pixels (a map-sized
    block left behind would light up ~100 tiles).

  check_bigmap.py [--segment bigmap] <run-dir>
  check_bigmap.py --ship [--segment bigmap] <run-dir>   ship_walk's segment: 1024x768 only (frames a_before / b_held /
                                    c_after), plus the chat-open pair d / e (Tab held must draw nothing)
  check_bigmap.py --click [--segment S] <run-dir>   mods:MM4B: the overlay is CLICKABLE. Judged from the logs only
                                    (the frames are baselines): every `; BIGMAP: click at (x,y) -> tile (c,r), camera (a,b)`
                                    line is re-derived from the `shown` geometry before it (tile = pixel cell + centre -
                                    half the map, camera = tile - half the view in tiles); the `; [sel]` lines the script
                                    logs (logsel) show the camera really went there (cam px = 32 * camera) and that the
                                    selected building / unit count are identical before and after every overlay click, drag
                                    and right click, while the control click on the bare world DOES change them
  check_bigmap.py --orders <run-dir>   mods:MM4C: the right click on the overlay gives the side minimap's order. Judged from
                                    the net log's `; BIGMAP: order at (x,y) -> tile (c,r): <what> [group n, double click d, shift s]`
                                    lines (each tile re-derived from the `shown` geometry) and the video log's `; [goal] <label>`
                                    lines (loggoal: the selected unit's goal_x:goal_y): the unit's goal is the clicked tile after a
                                    visible-tile move, a not-visible-tile move, the confirmed move (double click, Shift) and an
                                    attack; a control right click on the bare world gives the SAME goal; a placement is cancelled
                                    (armed -> 0) with no order; nothing selected gives no order
  check_bigmap.py --orders-off <run-dir>   [ui] bigmap_orders=0: no `order at` line, the unit's goal unchanged, the selection too
  check_bigmap.py --orders-ship <run-dir>   the first arms of --orders in the shipped configuration (libmh absent): three orders, the goals, the control
  check_bigmap.py --orders-mp <host-run-dir> <client-run-dir>   --det-bigmap: the host's two overlay orders for its Pioneer; the client's sim holds the same goal
  check_bigmap.py --selftest
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_minimap_box import STAMPED_MARK, read_logs  # noqa: E402
from walk_segments import segment_text  # noqa: E402

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None

SHOWN = re.compile(
    r"; BIGMAP: shown (?P<mw>\d+)x(?P<mh>\d+) map, (?P<k>\d+) px/tile at \((?P<x>-?\d+),(?P<y>-?\d+)\) in a "
    r"(?P<vw>\d+)x(?P<vh>\d+) view of a (?P<w>\d+)x(?P<h>\d+) window"
)
CENTRE = re.compile(r"centre tile \((?P<cx>\d+),(?P<cy>\d+)\)")
CLICK = re.compile(
    r"; BIGMAP: click at \((?P<x>-?\d+),(?P<y>-?\d+)\) -> tile \((?P<tc>\d+),(?P<tr>\d+)\), camera \((?P<cc>\d+),(?P<cr>\d+)\)"
)
DRAG = re.compile(
    r"; BIGMAP: drag at \((?P<x>-?\d+),(?P<y>-?\d+)\) -> tile \((?P<tc>\d+),(?P<tr>\d+)\), camera \((?P<cc>\d+),(?P<cr>\d+)\)"
)
SEL = re.compile(
    r"; \[sel\] (?P<label>\w+) cam=(?P<cx>-?\d+),(?P<cy>-?\d+) gest=\d+ btn=\d+ n=(?P<n>\d+) bldg=(?P<bldg>\d+)"
)
HIDDEN = re.compile(r"; BIGMAP: hidden after (\d+) frame")
WANT_VIEWS = {(1024, 768): (864, 736), (640, 480): (480, 448), (2560, 1440): (2400, 1408)}
FRAME_PX = 7  # the 6 px border and the dark outer line
MIN_CHANGED = 0.2
MAX_CHANGED = 0.01
TILE = 32
DIFF = 30


def log_problems(text, segment, ship=False):
    out = []
    if segment:
        seg = segment_text(STAMPED_MARK.sub(r"\1", text), segment)
        if seg is None:
            return [
                "mh_net.log has no `; [mark] %s` line: the walk never reached that segment"
                % segment
            ]
        text = seg
    shown = [m.groupdict() for m in SHOWN.finditer(text)]
    if not shown:
        return out + ["no `; BIGMAP: shown` line: the overlay never drew"]
    seen = set()
    for g in shown:
        v = {k: int(x) for k, x in g.items()}
        seen.add((v["w"], v["h"]))
        tag = "%dx%d window" % (v["w"], v["h"])
        want = WANT_VIEWS.get((v["w"], v["h"]))
        if want and (v["vw"], v["vh"]) != want:
            out.append("%s: view %dx%d, want %dx%d" % (tag, v["vw"], v["vh"], want[0], want[1]))
        if v["k"] < 1:
            out.append("%s: %d px per tile" % (tag, v["k"]))
        if v["k"] * v["mw"] + 2 * FRAME_PX > v["vw"] or v["k"] * v["mh"] + 2 * FRAME_PX > v["vh"]:
            out.append(
                "%s: the map (%d px/tile) plus its frame does not fit the view" % (tag, v["k"])
            )
    for size in WANT_VIEWS:
        if ship and size != (1024, 768):
            continue
        if size not in seen:
            out.append("no `shown` line at %dx%d" % size)
    if len(HIDDEN.findall(text)) < (1 if ship else 3):
        out.append("fewer than 3 `hidden` lines: a hold never ended")
    if not ship:  # res_walk's segment also clicks the overlay once per size
        out += click_log_problems(text, "", False, 3)
    return out


def floor_div(a, b):
    return a // b


def click_log_problems(net, video, sel_checks=True, min_clicks=1):
    """The click checks over the net log (BIGMAP lines) and the video log (`logsel` lines)."""
    out = []
    # events in log order: ("shown", geometry) / ("click", ...) / ("drag", ...)
    ev = []
    for line in net.splitlines():
        m = SHOWN.search(line)
        if m:
            g = {k: int(x) for k, x in m.groupdict().items()}
            c = CENTRE.search(line)
            if not c:
                continue
            g.update({k: int(x) for k, x in c.groupdict().items()})
            ev.append(("shown", g))
            continue
        m = CLICK.search(line)
        if m:
            ev.append(("click", {k: int(x) for k, x in m.groupdict().items()}))
            continue
        m = DRAG.search(line)
        if m:
            ev.append(("drag", {k: int(x) for k, x in m.groupdict().items()}))
    clicks = [e[1] for e in ev if e[0] == "click"]
    drags = [e[1] for e in ev if e[0] == "drag"]
    if len(clicks) < min_clicks:
        out.append(
            "%d `; BIGMAP: click at` line(s), want at least %d: a click never reached the overlay"
            % (len(clicks), min_clicks)
        )
    geom = None
    for kind, d in ev:
        if kind == "shown":
            geom = d
        elif kind == "click":
            if not geom:
                out.append("click at (%d,%d) with no `shown` line before it" % (d["x"], d["y"]))
                continue
            ix = min(max(floor_div(d["x"] - geom["x"], geom["k"]), 0), geom["mw"] - 1)
            iy = min(max(floor_div(d["y"] - geom["y"], geom["k"]), 0), geom["mh"] - 1)
            tc = (ix + geom["cx"] - geom["mw"] // 2) % geom["mw"]
            tr = (iy + geom["cy"] - geom["mh"] // 2) % geom["mh"]
            cc = (tc - geom["vw"] // 2 // TILE) % geom["mw"]
            cr = (tr - geom["vh"] // 2 // TILE) % geom["mh"]
            if (d["tc"], d["tr"]) != (tc, tr):
                out.append(
                    "click at (%d,%d): tile (%d,%d), the geometry says (%d,%d)"
                    % (d["x"], d["y"], d["tc"], d["tr"], tc, tr)
                )
            if (d["cc"], d["cr"]) != (cc, cr):
                out.append(
                    "click at (%d,%d): camera (%d,%d), the clicked tile at the view centre needs (%d,%d)"
                    % (d["x"], d["y"], d["cc"], d["cr"], cc, cr)
                )
    if not sel_checks:
        return out
    sel = {}
    for line in video.splitlines():
        m = SEL.search(line)
        if m:
            sel[m.group("label")] = {
                k: (int(v) if k != "label" else v) for k, v in m.groupdict().items()
            }
    for need in ("before", "after_click", "control_world_click"):
        if need not in sel:
            out.append("no `[sel] %s` line (logsel)" % need)
    if out:
        return out
    base = sel["before"]
    for label in (
        "after_click",
        "after_rclick",
        "after_wrapped_click",
        "after_drag",
        "released",
        "released_end",
    ):
        s = sel.get(label)
        if s and (s["bldg"], s["n"]) != (base["bldg"], base["n"]):
            out.append(
                "%s: selection changed (bldg %d n %d -> bldg %d n %d): a click on the overlay reached the world"
                % (label, base["bldg"], base["n"], s["bldg"], s["n"])
            )
    ctl = sel["control_world_click"]
    if (ctl["bldg"], ctl["n"]) == (base["bldg"], base["n"]):
        out.append(
            "control click on the bare world changed nothing (bldg %d): the test cannot tell a swallowed click from a lost one"
            % base["bldg"]
        )
    ac = sel["after_click"]
    if (ac["cx"], ac["cy"]) != (clicks[0]["cc"] * TILE, clicks[0]["cr"] * TILE):
        out.append(
            "after_click: the camera is at px (%d,%d), the click set tile (%d,%d)"
            % (ac["cx"], ac["cy"], clicks[0]["cc"], clicks[0]["cr"])
        )
    if "after_wrapped_click" in sel and len(clicks) > 1:
        w = sel["after_wrapped_click"]
        if (w["cx"], w["cy"]) != (clicks[1]["cc"] * TILE, clicks[1]["cr"] * TILE):
            out.append("after_wrapped_click: the camera is not where the second click put it")
    if "after_drag" in sel and drags:
        d = sel["after_drag"]
        if (d["cx"], d["cy"]) != (drags[-1]["cc"] * TILE, drags[-1]["cr"] * TILE):
            out.append("after_drag: the camera is not where the last drag position put it")
    elif "after_drag" in sel:
        out.append("after_drag: no `; BIGMAP: drag at` line")
    return out


ORDER = re.compile(
    r"; BIGMAP: order at \((?P<x>-?\d+),(?P<y>-?\d+)\) -> tile \((?P<tc>\d+),(?P<tr>\d+)\): (?P<what>.*?) "
    r"\[group (?P<group>\d+), double click (?P<dbl>\d+), shift (?P<shift>\d+)"
)
GOAL = re.compile(r"; \[goal\] (?P<label>\w+) n=(?P<n>\d+) goals=(?P<goals>\S*)")
BUILD = re.compile(r"; \[build\] (?P<label>\w+) armed=(?P<armed>\d+)")


def goal_lines(video):
    """{label: [(id, goal_x, goal_y), ...]} from the `; [goal]` lines."""
    out = {}
    for line in video.splitlines():
        m = GOAL.search(line)
        if m:
            out[m.group("label")] = [
                tuple(int(v) for v in g.split(":")) for g in m.group("goals").split(",") if g
            ]
    return out


def order_events(net):
    """The order lines in log order, each with the geometry that was current when it was logged."""
    ev, geom = [], None
    for line in net.splitlines():
        m = SHOWN.search(line)
        if m:
            g = {k: int(x) for k, x in m.groupdict().items()}
            c = CENTRE.search(line)
            if c:
                g.update({k: int(x) for k, x in c.groupdict().items()})
                geom = g
            continue
        m = ORDER.search(line)
        if m:
            d = {k: (v if k == "what" else int(v)) for k, v in m.groupdict().items()}
            d["geom"] = geom
            ev.append(d)
    return ev


def orders_problems(net, video):
    out = []
    ev = order_events(net)
    goals = goal_lines(video)
    armed = {m.group("label"): int(m.group("armed")) for m in BUILD.finditer(video)}
    # the order script: 0 a, 1 b, 2+3 the two clicks of the double click, 4 e (Shift), 5 f (attack), 6 h (placement), 7 g (nothing)
    if len(ev) != 8:
        return ["%d `; BIGMAP: order at` line(s), the script gives 8" % len(ev)]
    for i, o in enumerate(
        ev
    ):  # every logged tile is the one under the pointer, by the overlay's own geometry
        g = o["geom"]
        if not g:
            out.append("order %d: no `shown` line before it" % i)
            continue
        ix = min(max(floor_div(o["x"] - g["x"], g["k"]), 0), g["mw"] - 1)
        iy = min(max(floor_div(o["y"] - g["y"], g["k"]), 0), g["mh"] - 1)
        tc = (ix + g["cx"] - g["mw"] // 2) % g["mw"]
        tr = (iy + g["cy"] - g["mh"] // 2) % g["mh"]
        if (o["tc"], o["tr"]) != (tc, tr):
            out.append(
                "order %d at (%d,%d): tile (%d,%d), the geometry says (%d,%d)"
                % (i, o["x"], o["y"], o["tc"], o["tr"], tc, tr)
            )

    def goal_of(label):
        g = goals.get(label)
        if g is None:
            out.append("no `[goal] %s` line (loggoal)" % label)
            return None
        if len(g) != 1:
            out.append("[goal] %s: %d unit(s) selected, want exactly 1" % (label, len(g)))
            return None
        return g[0][1:]

    def want(i, label, kind, **kw):
        o = ev[i]
        if not o["what"].startswith(kind):
            out.append("order %d: `%s`, want `%s...`" % (i, o["what"], kind))
        for k, v in kw.items():
            if o[k] != v:
                out.append("order %d: %s = %d, want %d (%s)" % (i, k, o[k], v, o["what"]))
        if o["group"] != 1:
            out.append("order %d: group %d, want 1 unit group selected" % (i, o["group"]))
        if label:
            got = goal_of(label)
            if got is not None and got != (o["tc"], o["tr"]):
                out.append(
                    "%s: the unit's goal is %s, the clicked tile is (%d,%d)"
                    % (label, got, o["tc"], o["tr"])
                )

    want(0, "a_overlay", "move (deferred)", dbl=0, shift=0)
    want(1, "b_far", "move (deferred, tile not visible)", dbl=0, shift=0)
    want(2, None, "move (deferred)", dbl=0, shift=0)
    want(3, "d_double", "move (confirmed)", dbl=1, shift=0)
    want(4, "e_shift", "move (confirmed)", dbl=0, shift=1)
    want(5, None, "attack 0x40", dbl=0, shift=0)
    # control: the bare world, the same tile as (a): the same goal as the overlay gave
    a, c = goal_of("a_overlay"), goal_of("c_control")
    if a is not None and c is not None and a != c:
        out.append(
            "c_control: a right click on the bare world gives goal %s, the overlay gave %s" % (c, a)
        )
    # the attack: the goal is not the move before it, and the same building clicked on the bare world gives the same goal
    e, f, away, ctl = (
        goal_of("e_shift"),
        goal_of("f_attack"),
        goal_of("f_away"),
        goal_of("f_attack_control"),
    )
    if e is not None and f is not None and e == f:
        out.append("f_attack: the goal did not change from the move before it (%s)" % (f,))
    if away is not None and f is not None and away == f:
        out.append("f_away: the unit was not moved away before the control attack")
    # an attacker's goal follows its target as it closes in, so the two readings (taken at different times) are compared by
    # distance to the clicked building's tile, not for equality: both within 1 tile, torus 64
    tx, ty = ev[5]["tc"], ev[5]["tr"]
    for label, got in (("f_attack", f), ("f_attack_control", ctl)):
        if got is not None and (
            min((got[0] - tx) % 64, (tx - got[0]) % 64) > 1
            or min((got[1] - ty) % 64, (ty - got[1]) % 64) > 1
        ):
            out.append(
                "%s: the goal %s is not at the clicked building (%d,%d)" % (label, got, tx, ty)
            )
    # a placement is cancelled and no order follows (arming a building drops the unit selection, so nothing is left to order)
    h = ev[6]
    if not h["what"].startswith("cancel placement, no order"):
        out.append("order 6: `%s`, want the placement cancel" % h["what"])
    if armed.get("h_armed", 0) <= 0:
        out.append("[build] h_armed: no building was armed before the right click")
    if armed.get("h_cancelled", 1) != 0:
        out.append("[build] h_cancelled: the building is still armed after the right click")
    # nothing selected: no order, no goal
    g = ev[7]
    if not g["what"].startswith("no order (nothing selected") or g["group"] != 0:
        out.append(
            "order 7: `%s` with group %d, want no order and an empty selection"
            % (g["what"], g["group"])
        )
    if goals.get("g_none", [1]) != []:
        out.append("[goal] g_none: a unit is selected")
    return out


def orders_off_problems(net, video):
    out = []
    if "orders=0" not in net:
        out.append("no `; BIGMAP: ... orders=0` line: the setting did not reach the overlay")
    ev = order_events(net)
    if ev:
        out.append("%d `; BIGMAP: order at` line(s) with [ui] bigmap_orders=0" % len(ev))
    goals = goal_lines(video)
    if "selected" not in goals or "after" not in goals:
        return out + ["no `[goal] selected` / `[goal] after` line (loggoal)"]
    if len(goals["selected"]) != 1:
        out.append("%d unit(s) selected, want 1" % len(goals["selected"]))
    if goals["selected"] != goals["after"]:
        out.append("the unit's goal changed: %s -> %s" % (goals["selected"], goals["after"]))
    sel = {}
    for line in video.splitlines():
        m = SEL.search(line)
        if m:
            sel[m.group("label")] = (int(m.group("n")), int(m.group("bldg")))
    if "selected" not in sel or sel.get("selected") != sel.get("after"):
        out.append(
            "the selection changed: %s -> %s: the right click reached the world"
            % (sel.get("selected"), sel.get("after"))
        )
    return out


def read_video(run_dir):
    run_dir = os.path.normpath(run_dir)
    logs_dir = os.path.dirname(run_dir)
    me = os.path.basename(run_dir)
    names = sorted(n for n in os.listdir(logs_dir) if os.path.isdir(os.path.join(logs_dir, n)))
    out = []
    for i, n in enumerate(x for x in names if x >= me):
        if i > 0 and "_menu_" in n:
            break
        for f in ("mh_video.log", "mh_input.log"):
            p = os.path.join(logs_dir, n, f)
            if os.path.isfile(p):
                with open(p, encoding="utf-8", errors="replace") as fh:
                    out.append(fh.read())
    return "\n".join(out)


def load(run_dir, name):
    for ext in (".bmp", ".png"):
        p = os.path.join(run_dir, "capture_%s%s" % (name, ext))
        if os.path.isfile(p):
            return Image.open(p).convert("RGB")
    return None


def changed_mask(a, b, thr=DIFF):
    import numpy as np

    x = np.asarray(a).astype(int)
    y = np.asarray(b).astype(int)
    return np.abs(x - y).sum(axis=2) > thr


def frame_problems(a, b, c, view):
    out = []
    vw, vh = view
    ab = changed_mask(a, b, 0)[
        :vh, :vw
    ]  # any change at all: the dim moves a dark map's pixels by a few levels
    if ab.mean() < MIN_CHANGED:
        out.append(
            "held frame differs from the before frame over only %.1f%% of the view (want >= %d%%)"
            % (ab.mean() * 100, MIN_CHANGED * 100)
        )
    ac = changed_mask(a, c)
    if ac.mean() > MAX_CHANGED:
        out.append(
            "after-release frame differs from the before frame in %.2f%% of pixels (max %.0f%%): residue"
            % (ac.mean() * 100, MAX_CHANGED * 100)
        )
    hot = 0
    for ty in range(0, vh - TILE + 1, TILE):
        for tx in range(0, vw - TILE + 1, TILE):
            if ac[ty : ty + TILE, tx : tx + TILE].mean() > 0.9:
                hot += 1
    if hot:
        out.append(
            "%d view tiles (32x32) differ in more than 90%% of their pixels after release: residue"
            % hot
        )
    return out


def orders_mp_problems(host_net, host_video, client_video):
    """--det-bigmap: the host right-clicked the overlay (two orders) for its Pioneer; the same unit's goal in the CLIENT's sim agrees."""
    out = []
    ev = order_events(host_net)
    if len(ev) < 2:
        return ["%d `; BIGMAP: order at` line(s) on the host, want 2" % len(ev)]
    for o in ev:
        if not o["what"].startswith("move") or o["group"] != 1:
            out.append(
                "host order at (%d,%d): `%s` with group %d, want a move of 1 selected group"
                % (o["x"], o["y"], o["what"], o["group"])
            )
    h, c = goal_lines(host_video), goal_lines(client_video)
    for label, lines in (("host_final", h), ("host_selected", h), ("client_final", c)):
        if len(lines.get(label, [])) != 1:
            out.append("no single-unit `[goal] %s` line (loggoal)" % label)
    if out:
        return out
    hs, hf, cf = h["host_selected"][0], h["host_final"][0], c["client_final"][0]
    # a far order's goal is the unit's current waypoint on its way, so it is not compared with the clicked tile: the order took
    # (the goal is not the one the unit had when it was selected) and it took on BOTH peers (the same goal in the client's sim)
    if hf[1:] == hs[1:]:
        out.append("host_final: the goal did not change from the selection (%s)" % (hs[1:],))
    if cf != hf:
        out.append(
            "client_final: the host's Pioneer has goal %s in the client's sim, %s in the host's"
            % (cf, hf)
        )
    return out


def orders_ship_problems(net, video):
    """satellite_seq's step (libmh absent): the first arms of the --orders script, judged the same way."""
    out = []
    ev = order_events(net)
    goals = goal_lines(video)
    if len(ev) != 3:
        return ["%d `; BIGMAP: order at` line(s) in the shipped configuration, want 3" % len(ev)]

    def goal_of(label):
        g = goals.get(label)
        if not g or len(g) != 1:
            out.append("no single-unit `[goal] %s` line (loggoal)" % label)
            return None
        return g[0][1:]

    for o, kind, label, shift in (
        (ev[0], "move (deferred)", "a_overlay", 0),
        (ev[1], "move (deferred, tile not visible)", "b_far", 0),
        (ev[2], "move (confirmed)", "e_shift", 1),
    ):
        if not o["what"].startswith(kind) or o["group"] != 1 or o["shift"] != shift:
            out.append(
                "order at (%d,%d): `%s`, want `%s...` of 1 group, shift %d"
                % (o["x"], o["y"], o["what"], kind, shift)
            )
        got = goal_of(label)
        if got is not None and got != (o["tc"], o["tr"]):
            out.append(
                "%s: the unit's goal is %s, the clicked tile is (%d,%d)"
                % (label, got, o["tc"], o["tr"])
            )
    a, c = goal_of("a_overlay"), goal_of("c_control")
    if a is not None and c is not None and a != c:
        out.append(
            "c_control: a right click on the bare world gives goal %s, the overlay gave %s" % (c, a)
        )
    return out


def problems(run_dir, segment=None, ship=False):
    if not os.path.isdir(run_dir):
        return ["no run dir %s" % run_dir]
    out = log_problems(read_logs(run_dir), segment, ship)
    if Image is None:
        return out + ["PIL missing: cannot read the frames"]
    names = (
        ("a_before", "b_held", "c_after")
        if ship
        else ("a_1024_before", "b_1024_held", "c_1024_after")
    )
    a, b, c = (load(run_dir, "bigmap__%s" % n) for n in names)
    if None in (a, b, c):
        return out + ["a bigmap__%s / %s / %s capture is missing from %s" % (names + (run_dir,))]
    out += frame_problems(a, b, c, WANT_VIEWS[(1024, 768)])
    if ship:
        # the console line open: Tab held must draw NO overlay, so frame e matches frame d (sprites aside)
        d, e = load(run_dir, "bigmap__d_chat_open"), load(run_dir, "bigmap__e_chat_open_tab_held")
        if d is None or e is None:
            return out + [
                "a bigmap__d_chat_open / e_chat_open_tab_held capture is missing from %s" % run_dir
            ]
        vw, vh = WANT_VIEWS[(1024, 768)]
        frac = changed_mask(d, e)[:vh, :vw].mean()
        if frac > MAX_CHANGED:
            out.append(
                "chat open: Tab held changed %.2f%% of the view (max %.0f%%): the overlay drew over a chat line"
                % (frac * 100, MAX_CHANGED * 100)
            )
    return out


def orders_selftest(fails):
    """The --orders / --orders-off judges over synthetic logs: the good run passes, each mutation is named."""
    geom = (
        "[00:00:00.000] ; BIGMAP: shown 64x64 map, 7 px/tile at (208,144) in a 864x736 view of a 1024x768 window,"
        " centre tile (58,0) [camera]\n"
    )

    def tile(x, y):  # pixel -> tile with this geometry
        return ((x - 208) // 7 + 26) % 64, ((y - 144) // 7 + 32) % 64

    rows = [  # (x, y, what, group, double click, shift)
        (407, 357, "move (deferred)", 1, 0, 0),
        (230, 170, "move (deferred, tile not visible)", 1, 0, 0),
        (350, 430, "move (deferred)", 1, 0, 0),
        (350, 430, "move (confirmed)", 1, 1, 0),
        (414, 357, "move (confirmed)", 1, 0, 1),
        (456, 371, "attack 0x40 target 3 side 1", 1, 0, 0),
        (414, 357, "cancel placement, no order", 0, 0, 0),
        (407, 357, "no order (nothing selected)", 0, 0, 0),
    ]

    def net_text(mut=None):
        t = geom
        for i, (x, y, what, grp, d, sh) in enumerate(rows):
            if mut == "kind%d" % i:
                what = "move (deferred)" if "confirmed" in what else "move (confirmed)"
            c, r = tile(x, y)
            t += (
                "[00:00:00.000] ; BIGMAP: order at (%d,%d) -> tile (%d,%d): %s [group %d, double click %d, shift %d,"
                " dt 1 window 93]\n" % (x, y, c, r, what, grp, d, sh)
            )
        return t

    def goal(label, g):
        return "; [goal] %s n=%d goals=%s\n" % (label, 1 if g else 0, "5:%d:%d" % g if g else "")

    def video_text(mut=None):
        a, b, d, e, f = tile(407, 357), tile(230, 170), tile(350, 430), tile(414, 357), (61, 1)
        v = (
            goal("a_overlay", a)
            + goal("b_far", b)
            + goal("c_control", b if mut == "control" else a)
        )
        v += (
            goal("d_double", d) + goal("e_shift", e) + goal("f_attack", e if mut == "attack" else f)
        )
        v += goal("f_away", a) + goal("f_attack_control", (40, 40) if mut == "ctlfar" else f)
        v += "; [build] h_armed armed=%d type=21\n; [build] h_cancelled armed=%d type=21\n" % (
            0 if mut == "unarmed" else 21,
            21 if mut == "stillarmed" else 0,
        )
        v += goal("g_none", (1, 1) if mut == "selected" else None)
        return v

    def expect(label, net, video, want):
        if bool(orders_problems(net, video)) != want:
            fails.append("orders: " + label)

    expect("good run", net_text(), video_text(), False)
    expect(
        "a goal that is not the clicked tile",
        net_text(),
        video_text().replace("a_overlay n=1 goals=5:", "a_overlay n=1 goals=5:9"),
        True,
    )
    expect("control differs", net_text(), video_text("control"), True)
    expect("attack did not move the goal", net_text(), video_text("attack"), True)
    expect("the control attack goal is elsewhere", net_text(), video_text("ctlfar"), True)
    expect("placement not armed first", net_text(), video_text("unarmed"), True)
    expect("placement still armed", net_text(), video_text("stillarmed"), True)
    expect("a unit selected in the nothing arm", net_text(), video_text("selected"), True)
    expect("no double click", net_text("kind3"), video_text(), True)
    expect("too few orders", net_text().replace("order at", "ordxr at", 1), video_text(), True)
    expect(
        "a wrong tile in the log",
        net_text().replace("tile (54,62)", "tile (55,62)", 1),
        video_text(),
        True,
    )
    ship_net = geom
    for x, y, what, grp, d, sh in (rows[0], rows[1], rows[4]):
        c, r = tile(x, y)
        ship_net += (
            "[00:00:00.000] ; BIGMAP: order at (%d,%d) -> tile (%d,%d): %s [group %d, double click %d, shift %d,"
            " dt 1 window 93]\n" % (x, y, c, r, what, grp, d, sh)
        )
    a, b, e = tile(407, 357), tile(230, 170), tile(414, 357)
    ship_vid = goal("a_overlay", a) + goal("b_far", b) + goal("c_control", a) + goal("e_shift", e)
    for label, net, vid, want in (
        ("ship good", ship_net, ship_vid, False),
        (
            "ship control differs",
            ship_net,
            ship_vid.replace("c_control n=1 goals=5:54", "c_control n=1 goals=5:53"),
            True,
        ),
        (
            "ship goal is not the tile",
            ship_net,
            ship_vid.replace("b_far n=1 goals=5:", "b_far n=1 goals=5:9"),
            True,
        ),
        ("ship an order missing", ship_net.replace("order at", "ordxr at", 1), ship_vid, True),
    ):
        if bool(orders_ship_problems(net, vid)) != want:
            fails.append("orders-ship: " + label)
    mp_net = geom
    for x, y in ((250, 235), (300, 200)):
        c, r = tile(x, y)
        mp_net += (
            "[00:00:00.000] ; BIGMAP: order at (%d,%d) -> tile (%d,%d): move (deferred) [group 1, double click 0, shift 0,"
            " dt 1 window 93]\n" % (x, y, c, r)
        )
    last = tile(300, 200)
    mp_host = goal("host_selected", (82, 203)) + goal("host_final", last)
    for label, host, client, want in (
        ("mp good", mp_host, goal("client_final", last), False),
        (
            "mp the client's sim did not get the order",
            mp_host,
            goal("client_final", (82, 203)),
            True,
        ),
        (
            "mp the goal never changed",
            goal("host_selected", last) + goal("host_final", last),
            goal("client_final", last),
            True,
        ),
        ("mp no client line", mp_host, "", True),
    ):
        if bool(orders_mp_problems(mp_net, host, client)) != want:
            fails.append("orders-mp: " + label)
    off_net = "[00:00:00.000] ; BIGMAP: armed centre=camera key='Tab' dim=35% orders=0\n"
    off_vid = (
        goal("selected", (61, 1))
        + goal("after", (61, 1))
        + "; [sel] selected cam=1,1 gest=0 btn=0 n=1 bldg=0 ids=5\n; [sel] after cam=1,1 gest=0 btn=0 n=1 bldg=0 ids=5\n"
    )
    for label, net, vid, want in (
        ("off good", off_net, off_vid, False),
        ("off but an order logged", off_net + net_text(), off_vid, True),
        (
            "off but the goal moved",
            off_net,
            off_vid.replace("after n=1 goals=5:61:1", "after n=1 goals=5:54:62"),
            True,
        ),
        (
            "off but the selection changed",
            off_net,
            off_vid.replace("after cam=1,1 gest=0 btn=0 n=1", "after cam=1,1 gest=0 btn=0 n=0"),
            True,
        ),
        (
            "off but the setting never arrived",
            off_net.replace("orders=0", "orders=1"),
            off_vid,
            True,
        ),
    ):
        if bool(orders_off_problems(net, vid)) != want:
            fails.append("orders-off: " + label)


def selftest():
    import numpy as np

    def shown(w, h, vw, vh, k):
        return (
            "[00:00:00.000] ; BIGMAP: shown 64x64 map, %d px/tile at (208,144) in a %dx%d view of a %dx%d window, centre tile (1,1) [camera]\n"
            % (k, vw, vh, w, h)
        )

    # a click per size, derived the way the overlay does it: map 64x64 at (208,144), centre tile (1,1)
    body = ""
    for (w, h), k in (((1024, 768), 7), ((640, 480), 4), ((2560, 1440), 9)):
        vw, vh = WANT_VIEWS[(w, h)]
        x, y = 300, 200
        tc, tr = ((x - 208) // k + 1 - 32) % 64, ((y - 144) // k + 1 - 32) % 64
        cc, cr = (tc - vw // 2 // TILE) % 64, (tr - vh // 2 // TILE) % 64
        body += shown(w, h, vw, vh, k)
        body += "[00:00:00.000] ; BIGMAP: click at (%d,%d) -> tile (%d,%d), camera (%d,%d)\n" % (
            x,
            y,
            tc,
            tr,
            cc,
            cr,
        )
    good = (
        "[00:00:00.000] ; [mark] bigmap\n"
        + body
        + "[00:00:00.000] ; BIGMAP: hidden after 13 frame(s)\n" * 3
    )
    fails = []

    def expect(label, text, want):
        got = bool(log_problems(text, "bigmap"))
        if got != want:
            fails.append(label)

    expect("good log", good, False)
    expect("no shown line", "[00:00:00.000] ; [mark] bigmap\n", True)
    expect("a size missing", good.replace("640x480", "641x480"), True)
    expect("too few hidden", good.replace("hidden", "xidden"), True)
    expect("map does not fit", good.replace("9 px/tile", "40 px/tile"), True)
    expect("click lands off the tile", good.replace("camera (", "camera (1"), True)
    expect("no clicks", "\n".join(ln for ln in good.splitlines() if "click at" not in ln), True)
    base = np.zeros((768, 1024, 3), dtype="uint8")
    held = base.copy()
    held[0:700, 0:800] = 200
    resid = base.copy()
    resid[200:500, 200:500] = 200
    mk = Image.fromarray
    if frame_problems(mk(base), mk(held), mk(base), (864, 736)):
        fails.append("clean frames flagged")
    if not frame_problems(mk(base), mk(base), mk(base), (864, 736)):
        fails.append("overlay-less held frame passed")
    if not frame_problems(mk(base), mk(held), mk(resid), (864, 736)):
        fails.append("residue passed")
    orders_selftest(fails)
    if fails:
        print("check_bigmap selftest FAILED: " + ", ".join(fails))
        return 1
    print("check_bigmap selftest: ok")
    return 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    segment = None
    if "--segment" in argv:
        i = argv.index("--segment")
        segment = argv[i + 1]
        del argv[i : i + 2]
    ship = "--ship" in argv
    if ship:
        argv.remove("--ship")
    click = "--click" in argv
    if click:
        argv.remove("--click")
    if "--orders-mp" in argv:
        argv.remove("--orders-mp")
        if len(argv) != 2:
            print(__doc__)
            return 2
        p = orders_mp_problems(read_logs(argv[0]), read_video(argv[0]), read_video(argv[1]))
        for line in p:
            print("check_bigmap: " + line)
        if not p:
            print("check_bigmap: orders-mp ok (the host's order landed on both peers)")
        return 1 if p else 0
    for flag, fn in (
        ("--orders", orders_problems),
        ("--orders-off", orders_off_problems),
        ("--orders-ship", orders_ship_problems),
    ):
        if flag in argv:
            argv.remove(flag)
            if len(argv) != 1:
                print(__doc__)
                return 2
            p = fn(read_logs(argv[0]), read_video(argv[0]))
            for line in p:
                print("check_bigmap: " + line)
            if not p:
                print("check_bigmap: %s ok" % flag[2:])
            return 1 if p else 0
    if len(argv) != 1:
        print(__doc__)
        return 2
    if click:
        p = click_log_problems(read_logs(argv[0]), read_video(argv[0]))
        for line in p:
            print("check_bigmap: " + line)
        if not p:
            print("check_bigmap: click ok")
        return 1 if p else 0
    p = problems(argv[0], segment, ship)
    for line in p:
        print("check_bigmap: " + line)
    if not p:
        print("check_bigmap: ok")
    return 1 if p else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
