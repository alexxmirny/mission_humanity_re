#!/usr/bin/env python3
"""check_res4_input.py -- mods:RES4 stages 3-4: the scaled HUD takes its frame from the setting, sizes its lists to what the screen
shows, and maps pointer input back through the quad list.

Rows res4_s150 / res4_s200 (tools/uiscripts/res4_s*.txt) leave these lines in mh_video.log:

  `; [video] ui_scale=P%: the session frame is WxH ...`        stage 3: the game was asked for the ideal frame
  `; [ui] side-panel list rows follow the visible panel: Hp N`  stage 3: the six panel row-count sites take min(WindowHeight, Hp)
  `; [sel] mm_start|mm_screen|mm_frame cam=X,Y ...`             stage 4: the minimap clicked on the SCALED panel (an absolute pointer
                                                                packet at a client pixel -> owned DirectInput -> quad inverse map)
                                                                moves the camera exactly where the same click in FRAME coordinates does
  `; [sel] e_start|e_right|e_bottom|e_top|e_left cam=X,Y ...`   stage 4: the edge rule -- the pointer on the screen's last column / row
                                                                (a scaled panel / strip pixel) is the frame's W-1 / H-1: all four edges scroll,
                                                                each in the direction opposite to its pair

  check_res4_input.py <pct> <frame WxH> <Hp> <run-dir>
"""

import os
import re
import sys

CAM = re.compile(r"; \[sel\] (\w+) cam=(-?\d+),(-?\d+)")


def problems(pct, frame, hp, run_dir):
    out = []
    log = os.path.join(run_dir, "mh_video.log")
    if not os.path.isfile(log):
        return ["no mh_video.log in %s" % run_dir]
    text = open(log, encoding="utf-8", errors="replace").read()
    if not re.search(
        r"; \[video\] ui_scale=%d%%: the session frame is %dx%d " % (pct, frame[0], frame[1]), text
    ):
        out.append(
            "no `ui_scale=%d%%: the session frame is %dx%d` line: stage 3 did not ask the game for the ideal frame"
            % (pct, *frame)
        )
    m = re.findall(r"; \[ui\] side-panel list rows follow the visible panel: Hp (\d+) rows", text)
    if not m:
        out.append(
            "no `side-panel list rows follow the visible panel` line: the row-count sites never saw the scale"
        )
    elif int(m[-1]) != hp:
        out.append("panel list rows follow Hp %s, wanted %d" % (m[-1], hp))
    cams = {}
    for name, x, y in CAM.findall(text):
        cams[name] = (int(x), int(y))
    for k in (
        "mm_start",
        "mm_screen",
        "mm_frame",
        "e_start",
        "e_right",
        "e_bottom",
        "e_top",
        "e_left",
    ):
        if k not in cams:
            out.append("no `[sel] %s` line" % k)
    if out:
        return out
    if cams["mm_screen"] != cams["mm_frame"]:
        out.append(
            "minimap: the click on the scaled panel put the camera at %s, the same click in frame coordinates at %s"
            % (cams["mm_screen"], cams["mm_frame"])
        )
    if cams["mm_screen"] == cams["mm_start"]:
        out.append("minimap: the camera did not move (vacuous)")
    s = cams["e_start"]
    dx = {k: cams[k][0] - s[0] for k in ("e_right", "e_left")}
    dy = {k: cams[k][1] - s[1] for k in ("e_bottom", "e_top")}
    if dx["e_right"] == 0 or dx["e_left"] == 0 or (dx["e_right"] > 0) == (dx["e_left"] > 0):
        out.append(
            "edge scroll sideways: right %+d, left %+d (each must move, in opposite directions)"
            % (dx["e_right"], dx["e_left"])
        )
    if dy["e_bottom"] == 0 or dy["e_top"] == 0 or (dy["e_bottom"] > 0) == (dy["e_top"] > 0):
        out.append(
            "edge scroll vertically: bottom %+d, top %+d (each must move, in opposite directions)"
            % (dy["e_bottom"], dy["e_top"])
        )
    if cams["e_right"][1] != s[1] or cams["e_left"][1] != s[1]:
        out.append("sideways edge scroll also moved the camera vertically")
    if cams["e_bottom"][0] != s[0] or cams["e_top"][0] != s[0]:
        out.append("vertical edge scroll also moved the camera sideways")
    return out


def auto_problems(pct, frame, run_dir):
    """Row res4_auto: [video] ui_scale=auto with a faked display scale resolves to `pct`, logs it, and the frame is the ideal one."""
    log = os.path.join(run_dir, "mh_video.log")
    if not os.path.isfile(log):
        return ["no mh_video.log in %s" % run_dir]
    text = open(log, encoding="utf-8", errors="replace").read()
    out = []
    m = re.search(r"; \[ui\] ui_scale=auto: display scale (\d+)% \(([^)]*)\) -> (\d+)%", text)
    if not m:
        out.append("no `[ui] ui_scale=auto: display scale N% (...) -> P%` line")
    elif int(m.group(3)) != pct:
        out.append("auto resolved to %s%%, wanted %d%%" % (m.group(3), pct))
    if not re.search(
        r"; \[video\] ui_scale=%d%%: the session frame is %dx%d " % (pct, frame[0], frame[1]), text
    ):
        out.append("no `ui_scale=%d%%: the session frame is %dx%d` line" % (pct, *frame))
    return out


def selftest():
    import tempfile

    d = tempfile.mkdtemp()
    good = (
        "; [video] ui_scale=150%: the session frame is 1216x800 (map)\n; [ui] side-panel list rows follow the visible panel: Hp 534 rows (WindowHeight 800)\n"
        "; [sel] mm_start cam=1,1 x\n; [sel] mm_screen cam=5,5 x\n; [sel] mm_frame cam=5,5 x\n; [sel] e_start cam=10,10 x\n"
        "; [sel] e_right cam=2,10 x\n; [sel] e_bottom cam=10,2 x\n; [sel] e_top cam=10,18 x\n; [sel] e_left cam=18,10 x\n"
    )
    open(os.path.join(d, "mh_video.log"), "w").write(good)
    assert not problems(150, (1216, 800), 534, d), problems(150, (1216, 800), 534, d)
    open(os.path.join(d, "mh_video.log"), "w").write(
        good.replace("mm_frame cam=5,5", "mm_frame cam=6,5")
    )
    assert any("minimap" in p for p in problems(150, (1216, 800), 534, d))
    open(os.path.join(d, "mh_video.log"), "w").write(
        good.replace("e_left cam=18,10", "e_left cam=2,10")
    )
    assert any("sideways" in p for p in problems(150, (1216, 800), 534, d))
    open(os.path.join(d, "mh_video.log"), "w").write(good.replace("Hp 534", "Hp 600"))
    assert any("Hp" in p for p in problems(150, (1216, 800), 534, d))
    open(os.path.join(d, "mh_video.log"), "w").write(
        "; [ui] ui_scale=auto: display scale 150% ([video] display_scale override) -> 150%\n; [video] ui_scale=150%: the session frame is 1216x800 (x)\n"
    )
    assert not auto_problems(150, (1216, 800), d)
    assert auto_problems(125, (1216, 800), d)
    assert auto_problems(150, (1280, 800), d)
    print("check_res4_input selftest: PASS")
    return 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    if argv and argv[0] == "--auto":
        probs = auto_problems(int(argv[1]), tuple(int(v) for v in argv[2].split("x")), argv[3])
        for p in probs:
            print("FAIL:", p)
        if not probs:
            print(
                "PASS: ui_scale=auto resolved to %s%% and asked for the %s frame"
                % (argv[1], argv[2])
            )
        return 1 if probs else 0
    if len(argv) < 4:
        print(__doc__)
        return 2
    pct = int(argv[0])
    frame = tuple(int(v) for v in argv[1].split("x"))
    probs = problems(pct, frame, int(argv[2]), argv[3])
    for p in probs:
        print("FAIL:", p)
    if not probs:
        print(
            "PASS: ui_scale %d%%: frame %s, Hp %s, minimap click maps back, all four edges scroll"
            % (pct, argv[1], argv[2])
        )
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
