#!/usr/bin/env python3
"""check_minimap_box.py -- RES9: the minimap's viewport box stays inside the 128x128 minimap.

mh.dll's seams/ui_minimap_box.cpp replaces llm_strat_minimap_draw_viewport_box (0x004a2344) with a draw that skips
every pixel outside the minimap, and logs one `; RES9:` line to mh_net.log per new box geometry:

  ; RES9: minimap box zoom Z box WxH at (X,Y) from the minimap origin (OX,OY), surface S pitch P: stock writes B px
  before the surface (lowest byte L) and O px outside the minimap; clip drew D px, clipped C px; canary K new DEFF
  words before the surface (window N bytes)

The lines are written while a session is open, so they land in the SESSION folders after the process's menu folder;
test_ui hands over the menu folder and this reads it and every folder up to the next process's menu folder.

A multi-segment walk (res_walk.txt) writes `mark <segment>` into mh_net.log too (stamped like every line there);
`--segment res_minimap_box` reads the geometry lines between that mark and the next one (tools/walk_segments.py), the
arm banner from the whole log (it is a boot line).

What must hold (res_walk's res_minimap_box segment: 2-almost at 2048x1152 then 2560x1440):
  * the arm line `minimap viewport box armed at 004A2344 (mode 1`;
  * every geometry line is the clipped draw (`clip drew`), draws or clips every outline pixel (D + C = 2 * (W + H)),
    and clips nothing when the stock box fits (B = 0 and O = 0 -> C = 0: the stock pixels exactly);
  * zoom 4 at 2048x1152 (box height 141) and at 2560x1440 (height 177) were drawn, each with B > 0 (the stock loop
    would have left the surface), a canary window N > 0 and K = 0 (no new 0xDEFF word before the surface);
  * a zoom 2 box that fits was drawn (D > 0, C = 0) -- whole-log mode only: the stock code draws that box when a game
    starts, not on the zoom button, so res_walk's segment has no such line.
Absence of a line is RED.

  check_minimap_box.py [--segment <name>] <run-dir>
  check_minimap_box.py --selftest    synthetic logs plant every negative
"""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from walk_segments import pop_segment, segment_text  # noqa: E402

ARM = "; RES9: minimap viewport box armed at 004A2344 (mode 1"
GEOM = re.compile(
    r"; RES9: minimap box zoom (?P<zoom>-?\d+) box (?P<w>-?\d+)x(?P<h>-?\d+) at \((?P<x>-?\d+),(?P<y>-?\d+)\) from the "
    r"minimap origin \((?P<ox>-?\d+),(?P<oy>-?\d+)\), surface (?P<surf>[0-9A-F]+) pitch (?P<pitch>-?\d+): stock writes "
    r"(?P<before>-?\d+) px before the surface \(lowest byte (?P<low>-?\d+)\) and (?P<outside>-?\d+) px outside the "
    r"minimap; (?P<arm>[a-z ]+) drew (?P<drawn>-?\d+) px, clipped (?P<clipped>-?\d+) px; canary (?P<canary>-?\d+) new "
    r"DEFF words before the surface \(window (?P<window>-?\d+) bytes\)"
)
REQUIRED_Z4_H = (141, 177)  # zoom 4 at 2048x1152 (36 tile rows) and 2560x1440 (45): 4 * rows - 3
STAMPED_MARK = re.compile(r"^\[[0-9:.]+\] (; \[mark\] )", re.M)


def read_logs(run_dir):
    """The menu dir's mh_net.log and every later run dir's, up to the next process's menu dir."""
    run_dir = os.path.normpath(run_dir)
    logs_dir = os.path.dirname(run_dir)
    me = os.path.basename(run_dir)
    names = sorted(n for n in os.listdir(logs_dir) if os.path.isdir(os.path.join(logs_dir, n)))
    mine = [n for n in names if n >= me]
    out = []
    for i, n in enumerate(mine):
        if i > 0 and "_menu_" in n:
            break
        p = os.path.join(logs_dir, n, "mh_net.log")
        if os.path.isfile(p):
            with open(p, encoding="utf-8", errors="replace") as f:
                out.append(f.read())
    return "\n".join(out)


def problems(run_dir, segment=None):
    if not os.path.isdir(run_dir):
        return ["no run dir %s" % run_dir]
    text = read_logs(run_dir)
    out = []
    if ARM not in text:
        out.append("mh_net.log has no `%s` line" % ARM)
    if segment:
        seg = segment_text(STAMPED_MARK.sub(r"\1", text), segment)
        if seg is None:
            return out + [
                "mh_net.log has no `; [mark] %s` line: the walk never reached that segment"
                % segment
            ]
        text = seg
    lines = [m.groupdict() for m in GEOM.finditer(text)]
    if not lines:
        return out + ["no `; RES9: minimap box` geometry line"]
    for g in lines:
        v = {k: int(g[k]) for k in g if k not in ("surf", "arm")}
        tag = "zoom %d box %dx%d" % (v["zoom"], v["w"], v["h"])
        if g["arm"] != "clip":
            out.append("%s: drawn by `%s`, not the clipped draw" % (tag, g["arm"]))
        if v["drawn"] + v["clipped"] != 2 * (v["w"] + v["h"]):
            out.append(
                "%s: drew %d + clipped %d != the outline's %d px"
                % (tag, v["drawn"], v["clipped"], 2 * (v["w"] + v["h"]))
            )
        if v["before"] == 0 and v["outside"] == 0 and v["clipped"] != 0:
            out.append("%s: the stock box fits but %d px were clipped" % (tag, v["clipped"]))
        if v["canary"] > 0:
            out.append("%s: %d new 0xDEFF words before the surface" % (tag, v["canary"]))
    for h in REQUIRED_Z4_H:
        hit = [g for g in lines if int(g["zoom"]) == 4 and int(g["h"]) == h]
        if not hit:
            out.append("no zoom 4 line with box height %d" % h)
            continue
        g = {k: int(hit[0][k]) for k in hit[0] if k not in ("surf", "arm")}
        if g["before"] <= 0:
            out.append(
                "zoom 4 height %d: the stock loop stays in the surface (%d px before) -- not the case under test"
                % (h, g["before"])
            )
        if g["window"] <= 0:
            out.append("zoom 4 height %d: no canary window before the surface" % h)
        if g["canary"] != 0:
            out.append("zoom 4 height %d: canary %d, want 0" % (h, g["canary"]))
    if not segment and not any(
        int(g["zoom"]) == 2 and int(g["drawn"]) > 0 and int(g["clipped"]) == 0 for g in lines
    ):
        out.append("no zoom 2 box drawn unclipped")
    return out


def _line(zoom, w, h, before, outside, drawn, clipped, canary, window, arm="clip"):
    return (
        "[00:00:00.000] ; RES9: minimap box zoom %d box %dx%d at (-53,-9) from the minimap origin (6,6), surface 03DA0030 "
        "pitch 320: stock writes %d px before the surface (lowest byte -1054) and %d px outside the minimap; %s drew %d px, "
        "clipped %d px; canary %d new DEFF words before the surface (window %d bytes)\n"
        % (zoom, w, h, before, outside, arm, drawn, clipped, canary, window)
    )


GOOD = [
    "[00:00:00.000] " + ARM + ": clipped to the minimap)\n",
    _line(2, 119, 71, 0, 0, 380, 0, -1, 0),
    _line(4, 237, 141, 242, 514, 0, 756, 0, 48),
    _line(4, 237, 177, 298, 570, 0, 828, 0, 48),
]


def selftest():
    fails = 0

    def run(lines, want_red, label, segment=None, marks=False):
        nonlocal fails
        with tempfile.TemporaryDirectory() as td:
            menu = os.path.join(td, "2026-10-11T17-00-00Z_menu_solo")
            sess = os.path.join(td, "2026-10-11T17-00-02Z_abc_planet4_campaign")
            nxt = os.path.join(td, "2026-10-11T17-10-00Z_menu_solo")
            for d in (menu, sess, nxt):
                os.makedirs(d)
            with open(os.path.join(menu, "mh_net.log"), "w") as f:
                f.write(lines[0])
            body = "".join(lines[1:])
            if marks:  # the walk: a foreign segment's geometry line before and after ours
                body = (
                    "[00:00:01.000] ; [mark] other\n"
                    + _line(4, 237, 141, 242, 514, 0, 700, 7, 48)
                    + "[00:00:02.000] ; [mark] mine\n"
                    + body
                    + "[00:00:03.000] ; [mark] later\n"
                    + _line(2, 119, 71, 0, 0, 370, 10, -1, 0)
                )
            with open(os.path.join(sess, "mh_net.log"), "w") as f:
                f.write(body)
            with open(os.path.join(nxt, "mh_net.log"), "w") as f:  # a later process: never read
                f.write(_line(4, 237, 141, 242, 514, 0, 756, 99, 48))
            red = bool(problems(menu, segment))
        ok = red == want_red
        fails += not ok
        print("  %s %s" % ("ok  " if ok else "FAIL", label))

    run(GOOD, False, "good run")
    run([GOOD[0].replace("mode 1", "mode 2")] + GOOD[1:], True, "not armed in mode 1")
    run(GOOD[:3], True, "no 1440 line")
    run([GOOD[0], GOOD[2], GOOD[3]], True, "no zoom 2 line")
    run(
        GOOD[:2] + [_line(4, 237, 141, 242, 514, 0, 756, 3, 48), GOOD[3]],
        True,
        "DEFF before the surface",
    )
    run(GOOD[:2] + [_line(4, 237, 141, 242, 514, 0, 756, 0, 0), GOOD[3]], True, "no canary window")
    run(
        GOOD[:2] + [_line(4, 237, 141, 242, 514, 756, 0, 0, 48, arm="stock body"), GOOD[3]],
        True,
        "stock body drew",
    )
    run(GOOD[:2] + [_line(4, 237, 141, 242, 514, 0, 700, 0, 48), GOOD[3]], True, "pixels lost")
    run(
        [GOOD[0], _line(2, 119, 71, 0, 0, 370, 10, -1, 0)] + GOOD[2:], True, "a fitting box clipped"
    )
    run(GOOD, False, "segment: only its own lines are read", "mine", True)
    run(GOOD, True, "segment: absent mark", "nope", True)
    run([GOOD[0], GOOD[2], GOOD[3]], False, "segment: no zoom 2 line is fine", "mine", True)
    run(GOOD[:2] + [GOOD[3]], True, "segment: a zoom 4 line only another segment has", "mine", True)
    print("check_minimap_box --selftest: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


def main(argv):
    if argv[1:] == ["--selftest"]:
        return selftest()
    segment, argv = pop_segment(argv)
    if len(argv) != 2:
        print(__doc__)
        return 2
    out = problems(argv[1], segment)
    for p in out:
        print("RED:", p)
    if not out:
        print("check_minimap_box: PASS (%s)" % argv[1])
    return 1 if out else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
