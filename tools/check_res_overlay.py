#!/usr/bin/env python3
"""check_res_overlay.py -- mods:RES21: an explicit resolution at Interface size above 100 shows the WHOLE frame exactly as at 100 and draws the side panel and the
bottom strip OVER it (the map behind them is simply hidden).

The rows res_overlay_100 / _150 / _200 (resolution=2048x1152 on a pinned 2560x1440 screen) and res_crop_150 (1920x1080, rendered 1920x1088, cropped, on the same
screen) capture the PRESENTED image (`pcapture`) and the game frame (`capture`). This file re-derives the composition on its own (nothing shared with the C++):

  1. LOG: the layout line `; [ui] ui_scale P% ... composition CWxCH at X,Y` says the composition is the WHOLE screen at 0,0 (no bars) and the panel is the
     percentage's width -- or, at 100, there is no scaled layout at all;
  2. PIXELS: the presented image equals base composition + overlays: the frame (or its crop) fitted to the screen at the SAME zoom at every percentage, the strip
     (frame's last 32 rows, left of the panel) and the panel (frame's last 160 columns, the rows the screen shows) drawn over it at the percentage, anchored to the
     composition's bottom / right edge. The cursor rect and the top-left notice zone are exempt.

  check_res_overlay.py <profile> <run-dir>     profile: 2048_100 | 2048_150 | 2048_200 | crop_150
  check_res_overlay.py --selftest
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

SCREEN = (2560, 1440)
# eff = the effective percentage (150 snaps to 147 on a 1440-high screen so a whole number of list rows fits)
PROFILES = {
    "2048_100": dict(pre="res_overlay_100", frame=(2048, 1152), crop=(2048, 1152), eff=100),
    "2048_150": dict(pre="res_overlay_150", frame=(2048, 1152), crop=(2048, 1152), eff=147),
    "2048_200": dict(pre="res_overlay_200", frame=(2048, 1152), crop=(2048, 1152), eff=200),
    "crop_150": dict(pre="res_crop_150", frame=(1920, 1088), crop=(1920, 1080), eff=147),
}
UI_LAYOUT = re.compile(
    r"; \[ui\] ui_scale (\d+)%.* on (\d+)x(\d+), frame (\d+)x(\d+): map (\d+)x(\d+) \S+, panel (\d+) wide \((\d+) frame rows\), strip (\d+) high, "
    r"composition (\d+)x(\d+) at (-?\d+),(-?\d+)"
)


def rnd(base, pct):
    return (base * pct + 50) // 100


def centre(rel, dst, src):
    return ((2 * rel + 1) * src) // (2 * dst)


def fit(screen, comp):
    sw, sh = screen
    cw, ch = comp
    dw, dh = sw, sw * ch // cw
    if dh > sh:
        dh, dw = sh, sh * cw // ch
    x0, y0 = (sw - dw) // 2, (sh - dh) // 2
    return x0, y0, x0 + dw, y0 + dh


def quads(prof):
    """[(src, dst)] bottom to top: the base (map, strip, panel at the fit zoom) then, when the percentage exceeds the fit, the strip and panel overlays."""
    fw, fh = prof["frame"]
    cw, ch = prof["crop"]
    x0, y0, x1, y1 = fit(SCREEN, (cw, ch))
    dw, dh = x1 - x0, y1 - y0

    def X(c):
        return x0 + (c * dw + cw // 2) // cw

    def Y(c):
        return y0 + (c * dh + ch // 2) // ch

    mw, mh = cw - 160, ch - 32
    out = [
        ((0, 0, mw, mh), (X(0), Y(0), X(mw), Y(mh))),
        ((0, fh - 32, mw, fh), (X(0), Y(mh), X(mw), Y(ch))),
        ((fw - 160, 0, fw, ch), (X(mw), Y(0), X(cw), Y(ch))),
    ]
    pct = prof["eff"]
    pw, ds = rnd(160, pct), rnd(32, pct)
    if pct > 100 and (pw > X(cw) - X(mw) or ds > Y(ch) - Y(mh)):
        pw = max(pw, X(cw) - X(mw))
        ds = max(ds, Y(ch) - Y(mh))
        rows = min(ch, -((-dh * 100) // pct))
        cols = min(cw - 160, -((-(dw - pw) * 100) // pct))
        out.append(((0, fh - 32, cols, fh), (x0, y1 - ds, x1 - pw, y1)))
        out.append(((fw - 160, 0, fw, rows), (x1 - pw, y0, x1, y1)))
        return out, (pw, ds, rows), (x0, y0, x1, y1)
    return out, None, (x0, y0, x1, y1)


def expected_pixel(frame, qs, x, y):
    for src, dst in reversed(qs):
        if dst[0] <= x < dst[2] and dst[1] <= y < dst[3]:
            sx = src[0] + centre(x - dst[0], dst[2] - dst[0], src[2] - src[0])
            sy = src[1] + centre(y - dst[1], dst[3] - dst[1], src[3] - src[1])
            return frame[2][sy][sx]
    return (0, 0, 0)


def problems(profile, run_dir):
    prof = PROFILES[profile]
    pre = prof["pre"]
    out = []
    vlog = os.path.join(run_dir, "mh_video.log")
    if not os.path.isfile(vlog):
        return ["no mh_video.log in %s" % run_dir]
    text = open(vlog, encoding="utf-8", errors="replace").read()
    qs, ov, used = quads(prof)
    if used != (0, 0, SCREEN[0], SCREEN[1]):
        out.append("the independent fit %s is not the whole screen (a profile with bars)" % (used,))
    m = None
    for ln in text.splitlines():
        m = UI_LAYOUT.search(ln) or m
    if prof["eff"] > 100:
        if not m:
            out.append(
                "no `; [ui] ui_scale N% ... composition` layout line: the scale never took effect (vacuous)"
            )
        else:
            g = [int(v) for v in m.groups()]
            pct, _sw, _sh, fw, fh, _mw, _mh, pw, _rows, ds, cw, ch, ox, oy = g
            if (pct, fw, fh) != (prof["eff"], prof["frame"][0], prof["frame"][1]):
                out.append(
                    "layout line says %d%% on a %dx%d frame, wanted %d%% on %dx%d"
                    % (pct, fw, fh, prof["eff"], *prof["frame"])
                )
            if (cw, ch, ox, oy) != (SCREEN[0], SCREEN[1], 0, 0):
                out.append(
                    "composition %dx%d at %d,%d: bars (it must be the whole %dx%d screen at 0,0)"
                    % (cw, ch, ox, oy, *SCREEN)
                )
            if ov and (pw, ds) != (ov[0], ov[1]):
                out.append(
                    "panel %d wide / strip %d high on the layout line, wanted %d / %d"
                    % (pw, ds, ov[0], ov[1])
                )
            if not ov:
                out.append(
                    "the percentage does not exceed the fit zoom: the profile expects overlays but this file derives none"
                )
    elif m:
        out.append("a scaled layout line at 100%%: %s" % m.group(0)[:120])
    try:
        import check_ui_quads as q

        frame = q.read_bmp(os.path.join(run_dir, "capture_%s_frame.bmp" % pre))
        view = q.read_bmp(os.path.join(run_dir, "capture_%s_view.bmp" % pre))
    except (OSError, ImportError) as e:
        return out + ["cannot read the captures: %s" % e]
    if (frame[0], frame[1]) != prof["frame"] or (view[0], view[1]) != SCREEN:
        return out + [
            "capture sizes: frame %dx%d (want %dx%d), presented %dx%d (want %dx%d)"
            % (frame[0], frame[1], *prof["frame"], view[0], view[1], *SCREEN)
        ]
    cur = q.cursor_rect(run_dir, pre + "_view")
    nz_x, nz_y = 640 * max(prof["eff"], 100) // 100, 150 * max(prof["eff"], 100) // 100
    bad = tot = 0
    first = None
    for y in range(SCREEN[1]):
        for x in range(SCREEN[0]):
            tot += 1
            if view[2][y][x] == expected_pixel(frame, qs, x, y):
                continue
            if (cur and cur[0] <= x < cur[2] and cur[1] <= y < cur[3]) or (x < nz_x and y < nz_y):
                continue
            bad += 1
            first = first or (x, y)
    if bad * 500 > tot:
        out.append(
            "%d of %d presented pixels differ from the independent composition; first at %s"
            % (bad, tot, first)
        )
    # the map zoom is the SAME at every percentage: the pixel at the middle of the map region samples the same frame pixel
    mid = (1000, 600)
    base_src, base_dst = qs[0]
    sx = base_src[0] + centre(
        mid[0] - base_dst[0], base_dst[2] - base_dst[0], base_src[2] - base_src[0]
    )
    if prof["crop"] == (2048, 1152) and sx != 800:
        out.append(
            "the base map zoom is not the 100 one (screen x 1000 samples frame column %d, wanted 800)"
            % sx
        )
    return out


def selftest():
    fails = []
    q100, ov100, used = quads(PROFILES["2048_100"])
    if ov100 or used != (0, 0, 2560, 1440):
        fails.append("100: no overlays, whole screen")
    q200, ov200, _ = quads(PROFILES["2048_200"])
    if (
        not ov200
        or ov200[:2] != (320, 64)
        or q200[-1][1] != (2240, 0, 2560, 1440)
        or q200[-2][1] != (0, 1376, 2240, 1440)
    ):
        fails.append("200: overlay geometry %r" % (ov200,))
    if q200[0] != q100[0]:
        fails.append("the base map quad differs between 100 and 200 (the zoom must be identical)")
    q150, ov150, _ = quads(PROFILES["crop_150"])
    if not ov150 or q150[0][0] != (0, 0, 1760, 1048):
        fails.append("crop_150: the base map region is the cropped one")
    import tempfile

    with tempfile.TemporaryDirectory() as td:
        if not problems("2048_200", td):
            fails.append("an empty run dir must be RED")
    print("check_res_overlay selftest: %s" % ("FAIL" if fails else "ok"))
    for f in fails:
        print("SELFTEST FAIL:", f)
    return 1 if fails else 0


def main(argv):
    if argv and argv[0] == "--selftest":
        return selftest()
    if len(argv) != 2 or argv[0] not in PROFILES:
        print(__doc__)
        return 2
    probs = problems(argv[0], argv[1])
    for p in probs:
        print("RED: " + p)
    if not probs:
        print(
            "res_overlay OK: %s -- the whole frame at the 100 zoom, no bars, panel / strip overlays at %d%%, pixels = the independent composition"
            % (argv[0], PROFILES[argv[0]]["eff"])
        )
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
