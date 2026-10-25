#!/usr/bin/env python3
"""check_res19_panel.py -- mods:RES23-S2 / RES19: a NON-list side panel (a selected unit / ship card) is redrawn when Interface size changes.

The row res19_unit_panel selects the mother ship's group (the panel shows the unit card, game panel mode 2 -- not a building list, which hp_pump used to be
the only panel mode it redrew), then changes Interface size 200 -> 125 in Options and resumes. The HUD geometry commit owes EVERY panel mode and the bottom
strip a redraw; this checks both halves, per presenter output (bcapture):
  1. LOG: the commit that took the 125% logged `hud geometry epoch N: ... (changed: every panel mode and the strip are redrawn)`, and the 125%
     is put in force (RES26: already in the ESC menu right after Apply, no longer only at the resume);
  2. PIXELS: in every capture (200% reference, ESC menu, after the resume, 600 ticks later) the panel's lower part (the panel's columns, from 60% of the
     composition's height down to the strip) is not black, and neither is the strip extension (check_res19_filler's `ext` band) -- a panel that kept the old
     size's pixels, or a strip zeroed by the size change, shows black there.
Rects are derived from the layout lines of mh_video.log (200% layout for up_200 / up_menu, the 125% one for up_resume / up_later).

  check_res19_panel.py <run-dir>
  check_res19_panel.py --selftest
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_res19_filler as f  # noqa: E402

# RES26: the 125% layout shows in the ESC menu right after Apply (it used to wait for the resume), so up_menu reads the second layout too
PHASE_LAYOUT = {"up_200": 0, "up_menu": -1, "up_resume": -1, "up_later": -1}
COMMIT = re.compile(
    r"; \[ui\] hud geometry epoch (\d+): pct (\d+), whole panel rows Hp (\d+), frame (\d+)x(\d+) \(changed: every panel mode and the strip are redrawn\)"
)
FORCE = re.compile(r"; \[ui\] ui_scale in force: (\d+)% \(game mode (\d+)\)")


def panel_band(layout):
    pct, sw, sh, pw, cw, ch, x, y, eff, k, strip = layout
    x1 = x + cw
    mx = max(4, pw // 12)
    return (x1 - pw + mx, y + ch * 6 // 10, x1 - mx, y + ch - strip - 2)


def log_problems(text):
    out = []
    commits = COMMIT.findall(text)
    if not commits:
        out.append(
            "no `hud geometry epoch N ... (changed: every panel mode and the strip are redrawn)` line: the Interface size change committed nothing"
        )
    elif int(commits[-1][1]) != 125:
        out.append("the redraw-owing commit took %s%%, not 125%%" % commits[-1][1])
    forces = [(int(a), int(b)) for a, b in FORCE.findall(text)]
    if not any(a == 125 for a, _ in forces):
        out.append("no `ui_scale in force: 125%%` line (forces: %r)" % (forces,))
    return out


def run(run_dir):
    probs = []
    vlog = os.path.join(run_dir, "mh_video.log")
    if not os.path.isfile(vlog):
        return ["no mh_video.log in %s" % run_dir]
    text = open(vlog, encoding="utf-8", errors="replace").read()
    probs += log_problems(text)
    lays = f.layouts(text)
    if len(lays) < 2:
        return probs + ["fewer than two multi-quad layout lines in mh_video.log (%d)" % len(lays)]
    for name, ix in PHASE_LAYOUT.items():
        path = os.path.join(run_dir, "capture_%s__b.bmp" % name)
        if not os.path.isfile(path):
            probs.append("%s: bcapture missing" % name)
            continue
        lay = lays[ix]
        for rname, b in (("panel", panel_band(lay)), ("ext", f.regions(lay)["ext"])):
            m = f.measure(path, b)
            if not m:
                probs.append("%s: %s band %r is outside the image" % (name, rname, b))
                continue
            print(
                "  res19panel: %-8s %-5s x%d..%d y%d..%d mean rgb %r, %.1f%% black"
                % (
                    name,
                    rname,
                    m["band"][0],
                    m["band"][2],
                    m["band"][1],
                    m["band"][3],
                    m["mean"],
                    m["black_frac"] * 100,
                )
            )
            if m["black_frac"] >= (0.5 if rname == "ext" else 0.4):
                probs.append(
                    "%s: the %s band is BLACK (%.1f%%) in the presenter output"
                    % (name, rname, m["black_frac"] * 100)
                )
    return probs


def selftest():
    import tempfile

    from PIL import Image

    fails = []
    ok_log = (
        "; [ui] ui_scale 200% on 2560x1440, frame 1280x960: map 1660x1376 x1.48, panel 320 wide (720 frame rows), strip 64 high, composition 1980x1440 at 290,0\n"
        "; [ui] ui_scale in force: 125% (game mode 2)\n"
        "; [ui] hud geometry epoch 2: pct 125, whole panel rows Hp 960, frame 1280x960 (changed: every panel mode and the strip are redrawn)\n"
        "; [ui] ui_scale 125% on 2560x1440, frame 1280x960: map 1280x960 1:1, panel 200 wide (960 frame rows), strip 40 high, composition 1600x1200 at 480,120\n"
    )
    if log_problems(ok_log):
        fails.append("a good log must be clean: %r" % log_problems(ok_log))
    if log_problems(ok_log.replace("(game mode 2)", "(game mode 3)")):
        fails.append("125%% put in force in mode 3 is correct since RES26")
    if not log_problems(ok_log.replace("in force: 125%", "in force: 100%")):
        fails.append("no 125%% in force must be a problem")
    if not log_problems(
        ok_log.replace(" (changed: every panel mode and the strip are redrawn)", "")
    ):
        fails.append("a commit with no redraw owed must be a problem")
    with tempfile.TemporaryDirectory() as td:
        open(os.path.join(td, "mh_video.log"), "w").write(ok_log)
        for n in PHASE_LAYOUT:
            Image.new("RGB", (2560, 1440), (0, 90, 40)).save(
                os.path.join(td, "capture_%s__b.bmp" % n)
            )
        if run(td):
            fails.append("filled captures must be clean")
        Image.new("RGB", (2560, 1440), (0, 0, 0)).save(os.path.join(td, "capture_up_resume__b.bmp"))
        if not run(td):
            fails.append("a black resume capture must be RED")
    for x in fails:
        print("SELFTEST FAIL:", x)
    print("check_res19_panel selftest: %s" % ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv):
    if argv and argv[0] == "--selftest":
        return selftest()
    if len(argv) != 1:
        print(__doc__)
        return 2
    probs = run(argv[0])
    for p in probs:
        print("RED: " + p)
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
