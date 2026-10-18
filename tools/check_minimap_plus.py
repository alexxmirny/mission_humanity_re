#!/usr/bin/env python3
"""check_minimap_plus.py -- mods:MM1-MM3: the minimap's widest zoom keeps its resources, corners and view box.

mh.dll's seams/ui_minimap_plus.cpp logs one boot line to mh_net.log:

  ; minimap: resources at every zoom ON, top-level mask off, top-level box ON

(the mask word is the state of the MASK: off = removed, ON = the stock round mask). res_walk's `minimap_plus` segment
(ayy30, a 128-tile map) presses "-" to zoom 1 -- the whole map, where the stock renderer hides the resources, masks
the corners and draws no box -- and the RES9 draw logs a `zoom 1` geometry line for the box it then draws.

What must hold:
  * the boot line says all three parts are armed (the default `[ui] minimap_plus=1`);
  * the segment has a `; RES9: minimap box zoom 1` line that DREW pixels (the stock code draws none at zoom 1);
  * the pixels themselves (resources, corners) are the capture baselines' job: minimap_plus__a_128_z1 and the
    res_minimap_box / map_zoom_* frames of the walk.
Absence of a line is RED.

  check_minimap_plus.py [--segment <name>] <run-dir>
  check_minimap_plus.py --selftest
"""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_minimap_box import STAMPED_MARK, read_logs  # noqa: E402
from walk_segments import pop_segment, segment_text  # noqa: E402

BOOT = "; minimap: resources at every zoom ON, top-level mask off, top-level box ON"
Z1 = re.compile(r"; RES9: minimap box zoom 1 box (\d+)x(\d+) .*?clip drew (\d+) px")


def problems(run_dir, segment=None):
    if not os.path.isdir(run_dir):
        return ["no run dir %s" % run_dir]
    text = read_logs(run_dir)
    out = []
    if BOOT not in text:
        out.append("mh_net.log has no `%s` line" % BOOT)
    if segment:
        seg = segment_text(STAMPED_MARK.sub(r"\1", text), segment)
        if seg is None:
            return out + [
                "mh_net.log has no `; [mark] %s` line: the walk never reached that segment"
                % segment
            ]
        text = seg
    if not any(int(m.group(3)) > 0 for m in Z1.finditer(text)):
        out.append(
            "no `; RES9: minimap box zoom 1` line that drew pixels: the top-level box was not drawn"
        )
    return out


def selftest():
    fails = 0

    def run(menu_lines, sess_lines, want_red, label, segment=None):
        nonlocal fails
        with tempfile.TemporaryDirectory() as td:
            menu = os.path.join(td, "2026-10-16T17-00-00Z_menu_solo")
            sess = os.path.join(td, "2026-10-16T17-00-02Z_abc_planet4_campaign")
            for d in (menu, sess):
                os.makedirs(d)
            open(os.path.join(menu, "mh_net.log"), "w").write(menu_lines)
            open(os.path.join(sess, "mh_net.log"), "w").write(sess_lines)
            red = bool(problems(menu, segment))
        ok = red == want_red
        fails += not ok
        print("  %s %s" % ("ok  " if ok else "FAIL", label))

    boot = "[00:00:00.000] " + BOOT + "\n"
    z1 = (
        "[00:00:05.000] ; RES9: minimap box zoom 1 box 60x40 at (30,30) from the minimap origin (6,6), surface 1 pitch 320: "
        "stock writes 0 px before the surface (lowest byte 0) and 0 px outside the minimap; clip drew %d px, clipped 0 px; "
        "canary -1 new DEFF words before the surface (window 0 bytes)\n"
    )
    mark = "[00:00:04.000] ; [mark] minimap_plus\n"
    run(boot, mark + z1 % 200, False, "good run", "minimap_plus")
    run("", mark + z1 % 200, True, "no boot line", "minimap_plus")
    run(boot, mark + z1 % 0, True, "box drew nothing", "minimap_plus")
    run(boot, mark, True, "no zoom 1 line", "minimap_plus")
    run(boot, z1 % 200, True, "absent mark", "minimap_plus")
    run(boot.replace("mask off", "mask ON"), mark + z1 % 200, True, "mask still on", "minimap_plus")
    print("check_minimap_plus --selftest: %s" % ("FAIL" if fails else "PASS"))
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
        print("check_minimap_plus: PASS (%s)" % argv[1])
    return 1 if out else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
