#!/usr/bin/env python3
"""check_res_classic.py -- mods:RES25: "Classic sizes only" ([video] resolution=stock) is no resolution work and no Interface size scaling.

The row res_classic (tools/uiscripts/res_classic.txt) runs resolution=stock with ui_scale=175 in the ini on a pinned 2560x1440 screen. Checked from the files of one run:

  1. LOG: `; [ui] ui_scale ignored: classic sizes only`, the `; [video] classic sizes only` line and `options Interface size row OFF (classic sizes only)` are present;
     no scaled layout (`ui_scale N%` layout, `ui_scale in force`), no `crop` / `picker` line; every `view epoch` line says pct in force 100 and a single quad;
  2. PIXELS: the presented image (capture_res_classic_view.bmp) is the game frame (capture_res_classic_frame.bmp, 640x480) scaled to the screen with no panel / strip
     quads: re-derived here (fit, centred); the cursor rect and the top-left notice zone are exempt;
  3. OPTIONS: Screen size steps through exactly the game's three entries -- the captures a, b, c differ from each other and the fourth (d) is the first again.

  check_res_classic.py [run-dir]       (post_check passes the run dir last)
  check_res_classic.py --selftest
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

SCREEN = (2560, 1440)
FRAME = (640, 480)
SPIN = (
    245,
    228,
    350,
    250,
)  # the Screen size value box of the 640x480 Options dialog (the last capture has the pointer parked away)
MUST = [
    "; [ui] ui_scale ignored: classic sizes only",
    "; [video] classic sizes only",
    "; [ui] options Interface size row OFF (classic sizes only)",
]
FORBIDDEN = [
    re.compile(r"; \[ui\] ui_scale \d+%"),
    re.compile(r"; \[ui\] ui_scale in force"),
    re.compile(r"; \[ui\] crop"),
    re.compile(r"; \[video\] (picker|crop|resolution=)"),
]
EPOCH = re.compile(r"view epoch \d+: .*pct in force (\d+) .*\((single quad)\)")


def fit(screen, comp):
    sw, sh = screen
    cw, ch = comp
    dw, dh = sw, sw * ch // cw
    if dh > sh:
        dh, dw = sh, sh * cw // ch
    x0, y0 = (sw - dw) // 2, (sh - dh) // 2
    return x0, y0, x0 + dw, y0 + dh


def centre(rel, dst, src):
    return ((2 * rel + 1) * src) // (2 * dst)


def log_problems(text):
    out = []
    for m in MUST:
        if m not in text:
            out.append("log line missing: %s" % m)
    for ln in text.splitlines():
        for rx in FORBIDDEN:
            if rx.search(ln):
                out.append("forbidden under classic sizes: %s" % ln.strip()[:140])
        if "view epoch" in ln:
            m = EPOCH.search(ln)
            if not m or m.group(1) != "100":
                out.append("view epoch not a 100%% single quad: %s" % ln.strip()[:160])
    return out


def diff_count(a, b, skip=None):
    n = 0
    for y in range(a[1]):
        ra, rb = a[2][y], b[2][y]
        for x in range(a[0]):
            if ra[x] != rb[x] and not (skip and skip(x, y)):
                n += 1
    return n


def problems(run_dir):
    out = []
    vlog = os.path.join(run_dir, "mh_video.log")
    if not os.path.isfile(vlog):
        return ["no mh_video.log in %s" % run_dir]
    out += log_problems(open(vlog, encoding="utf-8", errors="replace").read())
    try:
        import check_ui_quads as q

        rd = lambda n: q.read_bmp(os.path.join(run_dir, "capture_res_classic_%s.bmp" % n))
        frame, view = rd("frame"), rd("view")
        opts = [rd("opt_" + c) for c in "abcd"]
    except (OSError, ImportError) as e:
        return out + ["cannot read the captures: %s" % e]
    if (frame[0], frame[1]) != FRAME or (view[0], view[1]) != SCREEN:
        return out + [
            "capture sizes: frame %dx%d (want %dx%d), presented %dx%d (want %dx%d)"
            % (*frame[:2], *FRAME, *view[:2], *SCREEN)
        ]
    x0, y0, x1, y1 = fit(SCREEN, FRAME)
    cur = q.cursor_rect(run_dir, "res_classic_view")
    bad = tot = 0
    first = None
    for y in range(SCREEN[1]):
        for x in range(SCREEN[0]):
            tot += 1
            if x0 <= x < x1 and y0 <= y < y1:
                e = frame[2][centre(y - y0, y1 - y0, FRAME[1])][centre(x - x0, x1 - x0, FRAME[0])]
            else:
                e = (0, 0, 0)
            if view[2][y][x] == e:
                continue
            if (cur and cur[0] <= x < cur[2] and cur[1] <= y < cur[3]) or (x < 640 and y < 150):
                continue
            bad += 1
            first = first or (x, y)
    if bad * 500 > tot:
        out.append(
            "%d of %d presented pixels differ from the frame scaled with no quads; first at %s"
            % (bad, tot, first)
        )
    n = FRAME[0] * FRAME[1]
    ab, bc, ac, da = (
        diff_count(opts[0], opts[1]),
        diff_count(opts[1], opts[2]),
        diff_count(opts[0], opts[2]),
        diff_count(
            opts[3], opts[0], lambda x, y: not (SPIN[0] <= x < SPIN[2] and SPIN[1] <= y < SPIN[3])
        ),
    )
    if min(ab, bc, ac) * 2000 < n:
        out.append(
            "Screen size captures a/b/c do not differ (%d / %d / %d px): the control did not step"
            % (ab, bc, ac)
        )
    if da > 20:
        out.append(
            "the fourth Screen size capture differs from the first by %d px: the list is not exactly three entries"
            % da
        )
    return out


def selftest():
    fails = []
    if not log_problems(""):
        fails.append("an empty log must be RED")
    good = (
        "\n".join(MUST)
        + "\n; [ui] view epoch 1: present 1, pct in force 100 (requested 100), screen 1x1, frame 1x1, composition 1x1 at 0,0 (single quad), game mode 1"
    )
    if log_problems(good):
        fails.append("a clean log must be green: %r" % log_problems(good))
    if not log_problems(good + "\n; [ui] ui_scale 175% on 2560x1440, frame 1x1"):
        fails.append("a scaled layout line must be RED")
    if not log_problems(good + "\n; [ui] crop: render 1x1"):
        fails.append("a crop line must be RED")
    if fit(SCREEN, FRAME) != (320, 0, 2240, 1440):
        fails.append("fit")
    print("check_res_classic selftest: %s" % ("FAIL" if fails else "ok"))
    for f in fails:
        print("SELFTEST FAIL:", f)
    return 1 if fails else 0


def main(argv):
    if argv and argv[0] == "--selftest":
        return selftest()
    if len(argv) != 1:
        print(__doc__)
        return 2
    probs = problems(argv[0])
    for p in probs:
        print("RED: " + p)
    if not probs:
        print(
            "res_classic OK: ui_scale ignored (logged), no scaled layout / crop / picker, the frame scaled with no quads, Screen size = the game's three entries"
        )
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
