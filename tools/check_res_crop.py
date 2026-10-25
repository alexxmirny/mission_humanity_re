#!/usr/bin/env python3
"""check_res_crop.py -- mods:RES18: a session size that is not a multiple of the 32 px tile is RENDERED rounded up and PRESENTED cropped.

The rows res_crop_1080 / res_crop_720 / res_crop_odd (tools/uiscripts/res_crop_*.txt) run `resolution=WxH` with H or W off the tile grid. The game renders
ceil32(W) x ceil32(H); the presenter shows the frame cut to W x H as three quads (the map's top-left, the strip = the frame's LAST 32 rows, the panel =
the frame's LAST 160 columns), at 1:1 on a screen of exactly W x H. Checked from the files of one run, re-derived here (nothing shared with the C++):

  1. LOG: `; [video] crop: WxH renders as FWxFH ...` (the picker), `SetDisplayMode FWxFH` (the display was asked for the RENDERED size) and the layout
     line `; [ui] crop: render FWxFH, composition CWxCH on a SWxSH screen at X,Y (bars: none) ...` with the quad numbers this file derives itself;
  2. PIXELS: the presented image (capture_<pre>_view.bmp, SWxSH) equals the composition built from the game frame (capture_<pre>_frame.bmp, FWxFH): the map
     frame (0,0)-(Cw-160,Ch-32), the strip frame rows Fh-32.., the panel frame columns Fw-160.. -- no bars, the strip on the last 32 screen rows;
  3. POINTER/EDGE (profiles with `edges`): the camera `; [sel] e_start|e_bottom|e_right` lines show the screen's last row and column scroll the map.

  check_res_crop.py <profile> <run-dir>      profile: 1080 | 720 | odd
  check_res_crop.py --selftest
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

LAYOUT = re.compile(
    r"; \[ui\] crop: render (\d+)x(\d+), composition (\d+)x(\d+) on a (\d+)x(\d+) screen at (-?\d+),(-?\d+) \(bars: (none|yes)\); "
    r"map (\d+)x(\d+) \(frame (\d+)x(\d+)\) (1:1|scaled), strip y (\d+)\.\.(\d+) \(frame rows (\d+)\.\.(\d+)\), "
    r"panel x (\d+)\.\.(\d+) \(frame cols (\d+)\.\.(\d+), (\d+) rows\)"
)
UI_LAYOUT = re.compile(
    r"; \[ui\] ui_scale (\d+)%.* on (\d+)x(\d+), frame (\d+)x(\d+): map (\d+)x(\d+) (1:1|x[\d.]+), panel (\d+) wide \((\d+) frame rows\), strip (\d+) high, "
    r"composition (\d+)x(\d+) at (\d+),(\d+)"
)
CAM = re.compile(r"; \[sel\] (\w+) cam=(-?\d+),(-?\d+)")

PROFILES = {
    "1080": dict(
        pre="res_crop", size=(1920, 1080), render=(1920, 1088), screen=(1920, 1080), edges=False
    ),
    "720": dict(
        pre="res_crop_720",
        size=(1280, 720),
        render=(1280, 736),
        screen=(1280, 720),
        edges=True,
        clicks=True,
    ),
    "odd": dict(
        pre="res_crop_odd", size=(1366, 768), render=(1376, 768), screen=(1366, 768), edges=False
    ),
}


def centre(rel, dst, src):
    return ((2 * rel + 1) * src) // (2 * dst)


def derive_quads(render, comp, org):
    """[(src, dst)] map, strip, panel for a 1:1 crop composition at `org` -- the rule, stated again here."""
    fw, fh = render
    cw, ch = comp
    ox, oy = org
    mw, mh = cw - 160, ch - 32
    return [
        ((0, 0, mw, mh), (ox, oy, ox + mw, oy + mh)),
        ((0, fh - 32, mw, fh), (ox, oy + mh, ox + mw, oy + ch)),
        ((fw - 160, 0, fw, ch), (ox + mw, oy, ox + cw, oy + ch)),
    ]


def expected_pixel(frame, quads, x, y):
    for src, dst in quads:
        if dst[0] <= x < dst[2] and dst[1] <= y < dst[3]:
            sx = src[0] + centre(x - dst[0], dst[2] - dst[0], src[2] - src[0])
            sy = src[1] + centre(y - dst[1], dst[3] - dst[1], src[3] - src[1])
            return frame[2][sy][sx]
    return (0, 0, 0)


def compare(frame, shown, quads):
    bad = tot = 0
    first = None
    for y in range(shown[1]):
        for x in range(shown[0]):
            tot += 1
            if shown[2][y][x] != expected_pixel(frame, quads, x, y):
                bad += 1
                first = first or (x, y)
    return bad, tot, first


def layout_problems(text, prof):
    out = []
    w, h = prof["size"]
    fw, fh = prof["render"]
    if not re.search(r"; \[video\] crop: %dx%d renders as %dx%d " % (w, h, fw, fh), text):
        out.append(
            "no `[video] crop: %dx%d renders as %dx%d` line: the picker never armed the crop"
            % (w, h, fw, fh)
        )
    if not re.search(r"SetDisplayMode %dx%d\b" % (fw, fh), text):
        out.append(
            "no `SetDisplayMode %dx%d`: the display was not asked for the RENDERED size" % (fw, fh)
        )
    m = None
    for ln in text.splitlines():
        m = LAYOUT.search(ln) or m
    if not m:
        return out + [
            "no `; [ui] crop: render ... composition` layout line: the crop never took effect (vacuous)"
        ]
    g = [int(v) if v.lstrip("-").isdigit() else v for v in m.groups()]
    (
        rw,
        rh,
        cw,
        ch,
        sw,
        sh,
        ox,
        oy,
        bars,
        mw,
        mh,
        msw,
        msh,
        k,
        sy0,
        sy1,
        fy0,
        fy1,
        px0,
        px1,
        fx0,
        fx1,
        rows,
    ) = g
    if (rw, rh) != (fw, fh):
        out.append("layout render %dx%d, wanted %dx%d" % (rw, rh, fw, fh))
    if (cw, ch) != (w, h):
        out.append("layout composition %dx%d, wanted %dx%d" % (cw, ch, w, h))
    if (sw, sh) != prof["screen"]:
        out.append("layout screen %dx%d, wanted %dx%d" % (sw, sh, *prof["screen"]))
    if bars != "none" or (ox, oy) != (0, 0):
        out.append("the composition has bars / is off the origin: %s at %d,%d" % (bars, ox, oy))
    want = derive_quads((fw, fh), (w, h), (0, 0))
    if (mw, mh) != (w - 160, h - 32) or (msw, msh) != (w - 160, h - 32) or k != "1:1":
        out.append(
            "map %dx%d (frame %dx%d) %s, wanted the %dx%d region 1:1"
            % (mw, mh, msw, msh, k, w - 160, h - 32)
        )
    if (sy0, sy1, fy0, fy1) != (h - 32, h, fh - 32, fh):
        out.append(
            "strip y %d..%d from frame rows %d..%d: it must sit on the last %d screen rows (%d..%d) from frame rows %d..%d"
            % (sy0, sy1, fy0, fy1, 32, h - 32, h, fh - 32, fh)
        )
    if (px0, px1, fx0, fx1, rows) != (w - 160, w, fw - 160, fw, h):
        out.append(
            "panel x %d..%d from frame cols %d..%d (%d rows), wanted x %d..%d from %d..%d (%d rows)"
            % (px0, px1, fx0, fx1, rows, w - 160, w, fw - 160, fw, h)
        )
    del want
    return out


RUN_DIR = [""]


def problems(profile, run_dir):
    prof = PROFILES[profile]
    RUN_DIR[0] = run_dir
    vlog = os.path.join(run_dir, "mh_video.log")
    if not os.path.isfile(vlog):
        return ["no mh_video.log in %s" % run_dir]
    text = open(vlog, encoding="utf-8", errors="replace").read()
    out = layout_problems(text, prof)
    if out:
        return out
    pre = prof["pre"]
    try:
        import check_ui_quads as q

        frame = q.read_bmp(os.path.join(run_dir, "capture_%s_frame.bmp" % pre))
        view = q.read_bmp(os.path.join(run_dir, "capture_%s_view.bmp" % pre))
    except (OSError, ImportError) as e:
        return ["cannot read the captures: %s" % e]
    fw, fh = prof["render"]
    sw, sh = prof["screen"]
    if (frame[0], frame[1]) != (fw, fh) or (view[0], view[1]) != (sw, sh):
        return [
            "capture sizes: frame %dx%d (want %dx%d, the RENDERED size), presented %dx%d (want %dx%d, the session size)"
            % (frame[0], frame[1], fw, fh, view[0], view[1], sw, sh)
        ]
    bad, tot, first = compare(frame, view, derive_quads((fw, fh), prof["size"], (0, 0)))
    # (`capture` and `pcapture` are two presents apart: a blinking minimap marker / a moving unit differs -- measured under 0.05%)
    if bad * 500 > tot:
        out.append(
            "%d of %d presented pixels differ from the independent composition; first at %s"
            % (bad, tot, first)
        )
    if prof["edges"]:
        out += edge_problems(text)
    return out


def edge_problems(text):
    cams = {}
    for name, x, y in CAM.findall(text):
        cams[name] = (int(x), int(y))
    out = []
    for k in ("e_start", "e_bottom", "e_right"):
        if k not in cams:
            out.append("no `[sel] %s` line" % k)
    if out:
        return out
    s = cams["e_start"]
    if cams["e_bottom"][1] == s[1]:
        out.append(
            "edge scroll: the pointer on the screen's last row did not scroll the map down (camera y %d -> %d)"
            % (s[1], cams["e_bottom"][1])
        )
    if cams["e_bottom"][0] != s[0]:
        out.append("edge scroll on the last row also moved the camera sideways")
    if cams["e_right"][0] == s[0]:
        out.append(
            "edge scroll: the pointer on the screen's last column did not scroll the map right (camera x %d -> %d)"
            % (s[0], cams["e_right"][0])
        )
    return out


def selftest():
    import tempfile

    fails = []
    q = derive_quads((1920, 1088), (1920, 1080), (0, 0))
    if (
        q[0] != ((0, 0, 1760, 1048), (0, 0, 1760, 1048))
        or q[1] != ((0, 1056, 1760, 1088), (0, 1048, 1760, 1080))
        or q[2] != ((1760, 0, 1920, 1080), (1760, 0, 1920, 1080))
    ):
        fails.append("derived quads %r" % (q,))
    good = (
        "; [video] crop: 1920x1080 renders as 1920x1088 and is presented cropped to 1920x1080\n"
        "; [gfx] SetDisplayMode 1920x1088 (no real mode switch)\n"
        "; [ui] crop: render 1920x1088, composition 1920x1080 on a 1920x1080 screen at 0,0 (bars: none); map 1760x1048 (frame 1760x1048) 1:1, strip y 1048..1080 "
        "(frame rows 1056..1088), panel x 1760..1920 (frame cols 1760..1920, 1080 rows)\n"
        "; [sel] e_start cam=10,10 x\n; [sel] e_bottom cam=10,50 x\n; [sel] e_right cam=40,10 x\n"
    )
    if layout_problems(good, PROFILES["1080"]):
        fails.append("good layout: %r" % layout_problems(good, PROFILES["1080"]))
    for what, bad in (
        ("bars", good.replace("(bars: none)", "(bars: yes)").replace("at 0,0", "at 0,4")),
        ("strip not on the last rows", good.replace("strip y 1048..1080", "strip y 1040..1072")),
        ("no crop line", good.replace("renders as", "x")),
        (
            "display asked for 1080",
            good.replace("SetDisplayMode 1920x1088", "SetDisplayMode 1920x1080"),
        ),
        ("map scaled", good.replace("1:1", "scaled")),
        (
            "panel from the wrong columns",
            good.replace("frame cols 1760..1920", "frame cols 1750..1910"),
        ),
    ):
        if not layout_problems(bad, PROFILES["1080"]):
            fails.append("must be RED: " + what)
    if edge_problems(good):
        fails.append("good edges")
    if not edge_problems(good.replace("e_bottom cam=10,50", "e_bottom cam=10,10")):
        fails.append("a bottom edge that does not scroll must be RED")
    with tempfile.TemporaryDirectory() as td:
        if not problems("1080", td):
            fails.append("an empty run dir must be RED")
    for n in fails:
        print("SELFTEST FAIL:", n)
    print("check_res_crop selftest: %s" % ("FAIL" if fails else "ok"))
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
        pr = PROFILES[argv[0]]
        print(
            "res_crop OK: %dx%d rendered as %dx%d, presented cropped (no bars, the strip on the last rows), pixels = the independent composition"
            % (*pr["size"], *pr["render"])
        )
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
