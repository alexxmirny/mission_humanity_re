#!/usr/bin/env python3
"""check_ui_quads.py -- mods:RES4 stages 1-2: did the presented image land where the layout says?

The row res4_quads runs [video] ui_scale=<pct> on a pinned screen ([video] ui_screen) with the game at the frame RES4 will
ask for on that screen. Two captures come out of one strategic frame:

  capture_res4_frame.bmp   `capture`: the game's own framebuffer, WxH, unscaled;
  capture_res4_<pct>.bmp     `pcapture`: the PRESENTED image, screen-sized: the C++ compositor (gfx/ui_layout.cpp) over the
                           same frame.

This checker rebuilds the presented image from the first with an INDEPENDENT implementation of the quad arithmetic
(this file; nothing is shared with the C++) and demands the second equal it pixel for pixel:

  map    the frame's (0,0)-(Mw,Mh) shown 1:1 at the screen's top left, Mw = screen_w - round(160 s), Mh = screen_h - round(32 s);
  strip  frame rows H-32..H-1, ceil(Mw/s) columns, scaled to Mw x round(32 s) under the map;
  panel  frame columns W-160..W-1, ceil(screen_h/s) rows, scaled to round(160 s) x screen_h at the right edge;
  nearest-texel sampling by the pixel-centre rule: screen pixel d of a quad shows source pixel floor((2d + 1) * src / (2 dst)).

Stage 5 (the owned cursor): the framebuffer capture has NO cursor in a scaled layout (the DLL draws it as its own overlay at ui_scale),
so the presented image carries it and the composition does not. `pcapture` logs `; pcapture cursor rect x0 y0 x1 y1` (mh_capture.log);
pixels inside that rect are exempt, every other pixel must still be equal, and a logged rect must really differ somewhere (a cursor
overlay that drew nothing is RED). `rect none` = no overlay, strict equality everywhere.

It also reads the `; [ui] ui_scale` layout line the presenter logs in mh_video.log (so a run where the scale never took effect
cannot pass on two identical pictures) and asserts the picture is not vacuous (the panel and the strip differ from the map's
unscaled continuation).

  check_ui_quads.py <pct> <screen WxH> <frame WxH> <run-dir>
  check_ui_quads.py --selftest
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

LAYOUT = re.compile(
    r"; \[ui\] ui_scale (\d+)%.* on (\d+)x(\d+), frame (\d+)x(\d+): map (\d+)x(\d+) 1:1, panel (\d+) wide \((\d+) frame rows\), strip (\d+) high, composition (\d+)x(\d+) at (\d+),(\d+)"
)


def rnd(base, pct):
    return (base * pct + 50) // 100


def ceil_div(a, b):
    return -((-a) // b)


def expected(frame, fw, fh, sw, sh, pct):
    """frame: list of rows of pixel tuples (fh x fw). Returns (rows, quads) of the presented image sw x sh, built the way the
    header of this file describes. Frame must be the ideal one for the screen (the composition then fills it)."""
    pw, ds = rnd(160, pct), rnd(32, pct)
    mw, mh = sw - pw, sh - ds
    th = mh + ds
    rows = ceil_div(th * 100, pct)
    cols = min(fw - 160, ceil_div(mw * 100, pct))
    out = [[(0, 0, 0)] * sw for _ in range(sh)]
    for y in range(sh):
        for x in range(sw):
            if x < mw and y < mh:
                out[y][x] = frame[y][x]
            elif x < mw:  # strip
                sx = (2 * x + 1) * cols // (2 * mw)
                sy = fh - 32 + (2 * (y - mh) + 1) * 32 // (2 * ds)
                out[y][x] = frame[sy][sx]
            else:  # panel
                sx = fw - 160 + (2 * (x - mw) + 1) * 160 // (2 * pw)
                sy = (2 * y + 1) * rows // (2 * th)
                out[y][x] = frame[sy][sx]
    return out


def cursor_rect(run_dir, name=None):
    """The `; pcapture cursor rect` of mh_capture.log: (x0, y0, x1, y1), None for `none`, False for no such line. The rect line is
    written just BEFORE the `capture_<name>.bmp written` line of its pcapture, so with `name` the rect of that capture is returned
    (a row that presents more than once, e.g. res4_opt at 100% / 150% / 100%); without it, or when the capture is not logged, the LAST one."""
    log = os.path.join(run_dir, "mh_capture.log")
    if not os.path.isfile(log):
        return False
    found = False
    for ln in open(log, encoding="utf-8", errors="replace").read().splitlines():
        m = re.search(
            r"; pcapture cursor rect (none|(-?\d+) (-?\d+) (-?\d+) (-?\d+)(?: sprite .*)?)", ln
        )
        if m:
            found = None if m.group(1) == "none" else tuple(int(m.group(i)) for i in range(2, 6))
        elif name and ln.startswith("; capture_%s.bmp written (presented image" % name):
            return found
    return found


def read_bmp(path):
    from PIL import Image

    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()
    return w, h, [[px[x, y] for x in range(w)] for y in range(h)]


def problems(pct, screen, frame_size, run_dir):
    out = []
    log = os.path.join(run_dir, "mh_video.log")
    text = open(log, encoding="utf-8", errors="replace").read() if os.path.isfile(log) else None
    if text is None:
        return ["no mh_video.log in %s" % run_dir]
    m = None
    for ln in text.splitlines():
        m = LAYOUT.search(ln) or m
    if not m:
        out.append(
            "no `; [ui] ui_scale N%% ... composition` layout line in mh_video.log: the scale never took effect (vacuous)"
        )
    else:
        g = [int(v) for v in m.groups()]
        want = [pct, screen[0], screen[1], frame_size[0], frame_size[1]]
        if g[:5] != want:
            out.append("layout line says %s, wanted pct/screen/frame %s" % (g[:5], want))
        pw, ds = rnd(160, pct), rnd(32, pct)
        if (g[5], g[6], g[7], g[9]) != (screen[0] - pw, screen[1] - ds, pw, ds):
            out.append("layout line map/panel/strip %s does not match the formula" % (g[5:10],))
        if (g[10], g[11], g[12], g[13]) != (screen[0], screen[1], 0, 0):
            out.append("composition is not the whole screen: %s" % (g[10:14],))
    fpath = os.path.join(run_dir, "capture_res4_frame.bmp")
    ppath = os.path.join(run_dir, "capture_res4_%d.bmp" % pct)
    for p in (fpath, ppath):
        if not os.path.isfile(p):
            out.append("missing %s" % os.path.basename(p))
    if out:
        return out
    fw, fh, frame = read_bmp(fpath)
    pw_, ph_, shown = read_bmp(ppath)
    if (fw, fh) != tuple(frame_size):
        out.append("game frame is %dx%d, wanted %dx%d" % (fw, fh, frame_size[0], frame_size[1]))
        return out
    if (pw_, ph_) != tuple(screen):
        out.append("presented image is %dx%d, wanted %dx%d" % (pw_, ph_, screen[0], screen[1]))
        return out
    want = expected(frame, fw, fh, screen[0], screen[1], pct)
    bad = 0
    first = None
    cur = cursor_rect(run_dir, "res4_%d" % pct)
    in_cur = 0
    # RES20: a floating notice (the "????" glyph-fallback message at load, an order refusal) is an overlay scaled by the percentage from the map's top left:
    # its zone (x < 640 px, y < 150 px at 100%, times the percentage) is not part of the frame composition (res_notice_* proves the notice itself).
    nz_x, nz_y = 640 * pct // 100, 150 * pct // 100
    for y in range(ph_):
        for x in range(pw_):
            if shown[y][x] != want[y][x]:
                if x < nz_x and y < nz_y:
                    continue
                if cur and cur[0] <= x < cur[2] and cur[1] <= y < cur[3]:
                    in_cur += 1
                    continue
                bad += 1
                if first is None:
                    first = (x, y, shown[y][x], want[y][x])
    if cur and in_cur == 0:
        out.append(
            "the cursor rect %s is logged but the presented image shows nothing different there"
            % (cur,)
        )
    if bad:
        out.append(
            "%d presented pixels differ from the independent quad composition; first at %s"
            % (bad, first)
        )
    # not vacuous: the panel region must not be the frame's own 1:1 continuation
    mw = screen[0] - rnd(160, pct)
    same = sum(
        1
        for y in range(0, ph_, 7)
        for x in range(mw, pw_, 5)
        if x < fw and y < fh and shown[y][x] == frame[y][x]
    )
    total = sum(1 for y in range(0, ph_, 7) for x in range(mw, pw_, 5))
    if total and same * 2 > total:
        out.append(
            "the panel region equals the unscaled frame (%d of %d samples): nothing was scaled"
            % (same, total)
        )
    return out


def selftest():
    import random
    import tempfile

    fails = []
    fw, fh, sw, sh, pct = 224, 96, 240, 120, 150
    # ideal frame for a 240x120 screen at 150: pw 240 > screen: use a bigger toy: screen 400x200 @ 150
    sw, sh = 400, 200
    pw, ds = rnd(160, pct), rnd(32, pct)
    mw, mh = sw - pw, sh - ds
    fw, fh = ((mw + 31) // 32) * 32 + 160, ((mh + 31) // 32) * 32 + 32
    random.seed(7)
    frame = [
        [(random.randrange(256), random.randrange(256), random.randrange(256)) for _ in range(fw)]
        for _ in range(fh)
    ]
    want = expected(frame, fw, fh, sw, sh, pct)
    if want[0][0] != frame[0][0] or want[mh - 1][mw - 1] != frame[mh - 1][mw - 1]:
        fails.append("map is not 1:1")
    if want[sh - 1][0][0] is None or any(want[y][x] is None for y in range(sh) for x in range(sw)):
        fails.append("hole in the composition")
    # the panel's first screen column shows the frame's panel column 0 (x = fw - 160)
    if want[0][mw] != frame[0][fw - 160]:
        fails.append("panel left edge")
    if want[sh - 1][0] != frame[fh - 32 + (2 * (sh - 1 - mh) + 1) * 32 // (2 * ds)][0]:
        fails.append("strip bottom row")
    with tempfile.TemporaryDirectory() as td:
        if not problems(pct, (sw, sh), (fw, fh), td):
            fails.append("a run dir without a log must be RED")
        with open(os.path.join(td, "mh_video.log"), "w") as f:
            f.write("; unrelated\n")
        if not any("vacuous" in p for p in problems(pct, (sw, sh), (fw, fh), td)):
            fails.append("no layout line must be RED (vacuous)")
    for n in fails:
        print("SELFTEST FAIL:", n)
    print("check_ui_quads selftest: %s" % ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    if len(argv) < 4:
        print(__doc__)
        return 2
    pct = int(argv[0])
    screen = tuple(int(v) for v in argv[1].split("x"))
    frame = tuple(int(v) for v in argv[2].split("x"))
    probs = problems(pct, screen, frame, argv[3])
    for p in probs:
        print("FAIL:", p)
    if not probs:
        print(
            "PASS: the presented image is the independent quad composition (%d%% on %s, frame %s)"
            % (pct, argv[1], argv[2])
        )
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
