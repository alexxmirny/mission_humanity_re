#!/usr/bin/env python3
"""check_res4_latch.py -- mods:RES19 -> RES26: an Interface size chosen in Options shows RIGHT AFTER Apply (RES19 held it until the game resumed; the user asked for the redraw, 2026-10-23).

The row res4_opt_latch starts at 200% on a presenting 1280x960 lane, opens ESC > Options, steps Interface size 200 -> 125 and presses Ok. The ESC menu
(game mode 3) is up again over the frame the strategic view last showed. RES19 kept the 200% HUD there until the resume (a 125% panel quad over a 200%
frame had blank rows); RES26 moves the dialog-mode stamp to the applied frame and the asked size at once (ddraw_own.cpp owned_hud_commit_applied), and the
commit redraws the panel rows for the new size, so the menu already shows the 125% HUD.

  1. LOG: `ui_scale -> 125% (applied ...)` is logged, then `ui_scale in force: 125%` (any game mode -- RES26 puts it in force in the menu), and the layout
     lines are in presentation order: 200, then the narrower 125;
  2. BEFORE: the presented image in the ESC menu (capture_res4_lt_before) is NOT the 200% reference (capture_res4_lt_200): the panel changed at Apply;
  3. AFTER: back in the game (capture_res4_lt_after) the panel rows equal the menu's -- the same 125% layout both times (cursor rects exempt).

  check_res4_latch.py <run-dir>
  check_res4_latch.py --selftest
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_ui_quads as q  # noqa: E402

LAYOUT = re.compile(
    r"; \[ui\] ui_scale (\d+)%.* on (\d+)x(\d+), frame (\d+)x(\d+): map .*?, panel (\d+) wide"
)
FORCE = re.compile(r"; \[ui\] ui_scale in force: (\d+)% \(game mode (\d+)\)")


def log_problems(text):
    out = []
    forces = [(int(a), int(b)) for a, b in FORCE.findall(text)]
    if not any(p == 125 for p, _ in forces):
        out.append("no `ui_scale in force: 125%%` line (forces: %r)" % (forces,))
    if "; [ui] ui_scale -> 125%" not in text:
        out.append("no `ui_scale -> 125%` line: the Options choice never reached the presenter")
    lay = [(int(m.group(1)), int(m.group(6))) for m in LAYOUT.finditer(text)]
    pcts = [p for p, _ in lay]
    if len(pcts) < 2 or pcts[-1] >= pcts[-2] or pcts[-2] <= 150:
        out.append("layout lines %r: wanted ... 200-ish, then the narrower 125-ish one" % (pcts,))
    return out, lay


def in_rect(r, x, y):
    return bool(r) and r[0] <= x < r[2] and r[1] <= y < r[3]


def problems(run_dir):
    vlog = os.path.join(run_dir, "mh_video.log")
    if not os.path.isfile(vlog):
        return ["no mh_video.log in %s" % run_dir]
    out, lay = log_problems(open(vlog, encoding="utf-8", errors="replace").read())
    if out:
        return out
    pw_old, pw_new = lay[-2][1], lay[-1][1]
    try:
        ref = q.read_bmp(os.path.join(run_dir, "capture_res4_lt_200.bmp"))
        before = q.read_bmp(os.path.join(run_dir, "capture_res4_lt_before.bmp"))
        after = q.read_bmp(os.path.join(run_dir, "capture_res4_lt_after.bmp"))
    except (OSError, ImportError) as e:
        return ["cannot read the captures: %s" % e]
    if not (ref[0] == before[0] == after[0] and ref[1] == before[1] == after[1]):
        return ["presented sizes differ"]
    sw, sh = ref[0], ref[1]
    cr, cb = q.cursor_rect(run_dir, "res4_lt_200"), q.cursor_rect(run_dir, "res4_lt_before")
    ca = q.cursor_rect(run_dir, "res4_lt_after")
    x0, y0 = sw - pw_old, (sh * 2) // 5
    out = []
    if pw_new >= pw_old:
        out.append(
            "the 125%% panel (%d wide) is not narrower than the 200%% one (%d)" % (pw_new, pw_old)
        )
    # BEFORE vs the 200% reference: the panel region must have changed at Apply (RES19's hold read as unchanged)
    diff = sum(
        1
        for y in range(y0, sh, 3)
        for x in range(x0, sw, 3)
        if not (in_rect(cr, x, y) or in_rect(cb, x, y)) and ref[2][y][x] != before[2][y][x]
    )
    if diff * 4 < ((sh - y0) // 3) * (pw_old // 3):
        out.append(
            "in the ESC menu the side panel is still the 200%% one: the 125%% layout did not show at Apply"
        )
    # BEFORE vs AFTER: the list rows of the 125% panel are the same in the menu and after the resume
    x1 = sw - pw_new + pw_new // 12
    bad = tot = 0
    for y in range(max(y0, 250), sh - 80):
        for x in range(x1, sw - pw_new // 12):
            if in_rect(cb, x, y) or in_rect(ca, x, y):
                continue
            tot += 1
            bad += before[2][y][x] != after[2][y][x]
    if tot == 0 or bad * 1000 > tot * 5:
        out.append(
            "the 125%% panel in the ESC menu is not the one after the resume: %d of %d list-row pixels differ"
            % (bad, tot)
        )
    return out


def selftest():
    import tempfile

    fails = []
    good = (
        "; [ui] ui_scale in force: 200% (game mode 2)\n"
        "; [ui] ui_scale 192% on 1280x960, frame 1120x928: map 960x896 1:1, panel 307 wide (563 frame rows)\n"
        "; [ui] ui_scale -> 125% (applied at the next strategic present)\n"
        "; [ui] ui_scale in force: 125% (game mode 2)\n"
        "; [ui] ui_scale 123% on 1280x960, frame 1120x928: map 960x896 x1.03, panel 197 wide (879 frame rows)\n"
    )
    if log_problems(good)[0]:
        fails.append("a good log is RED: %r" % (log_problems(good)[0],))
    if not log_problems(good.replace("in force: 125%", "in force: 100%"))[0]:
        fails.append("no 125% in force must be RED")
    if log_problems(good.replace("in force: 125% (game mode 2)", "in force: 125% (game mode 3)"))[
        0
    ]:
        fails.append("125% in force in mode 3 is now CORRECT (RES26)")
    if not log_problems("; nothing\n")[0]:
        fails.append("an empty log must be RED")
    with tempfile.TemporaryDirectory() as td:
        if not problems(td):
            fails.append("an empty run dir must be RED")
    for n in fails:
        print("SELFTEST FAIL:", n)
    print("check_res4_latch selftest: %s" % ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv):
    if argv and argv[0] == "--selftest":
        return selftest()
    if not argv:
        print(__doc__)
        return 2
    probs = problems(argv[-1])
    for p in probs:
        print("RED: " + p)
    if not probs:
        print(
            "res4_latch OK: 200 -> 125 in Options: the ESC menu already shows the 125% HUD and it is unchanged after the resume"
        )
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
