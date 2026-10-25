#!/usr/bin/env python3
"""check_res_fallback.py -- mods:RES18/RES23 (R720 fix): at an Interface size the layout cannot use, the game's OWN cursor and ESC menu show.

An Interface size that does not exceed the fit zoom (1280x720 cropped to a 2560x1440 screen at 175%: the fit zoom 2 already gives the panel 320 px, the 175% panel is
280; 2048x1152 at 125% on the same screen: zoom 1.25 = the 125% panel) makes uilayout::build fall back to the plain 100 base layout (map / strip / panel at the fit
zoom, no overlay). The hooks that keep the cursor, the dialog lists and the notices OUT of the frame to draw them at the Interface size afterwards must then stand down
(ddraw_own.cpp scaled_layout: `L.pct > MIN_PCT`), or the cursor and the ESC menu are drawn nowhere (the user's rc4 report: "no mouse cursor and no ESC menu").

The rows res_crop_720_175 (d3d11 + gdi twin) and res_overlay_125 take, in the strategic view, the game frame (`capture`), the presenter's CPU twin (`pcapture`) and the
presenter's output (`bcapture`) with the game cursor parked on A, the frame and the presenter output with it parked on B, then the same three for the ESC menu with the
cursor back on A. Everything below is derived from the files of one run (nothing shared with the C++):

  1. LOG: the percentage was asked for AND the layout is the 100 base (`ui_scale in force: P%`, then a `ui_scale 100% ... composition` layout line) -- otherwise the
     run does not exercise the fallback (vacuous); no `OVERLAY LOST` line (the defensive log of overlay_collect);
  2. EXACTLY ONE CURSOR SOURCE: the cursor is IN THE FRAME (frame A and frame B differ around both hotspots) and the presenter's CPU twin has NO cursor overlay
     (`; pcapture cursor rect none`). Both, or neither, is RED;
  3. THE CURSOR IS ON SCREEN at the expected point: the presented images A and B differ inside the boxes around zoom x A and zoom x B (the fit zoom of the
     composition), and nowhere else near them is required to match;
  4. THE ESC MENU IS ON SCREEN: it is in the game frame (the frame differs from the strategic one over a block no animation explains) and the presented ESC image
     differs from the presented strategic one over most of that block at the zoom.

  check_res_fallback.py <profile> <run-dir>      profile: crop720_175 | fit_125
  check_res_fallback.py --selftest
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

SCREEN = (2560, 1440)
PROFILES = {
    "crop720_175": dict(
        pre="res_crop_720_175",
        frame=(1280, 736),
        comp=(1280, 720),
        pct=175,
        A=(500, 350),
        B=(900, 500),
    ),
    "fit_125": dict(
        pre="res_overlay_125",
        frame=(2048, 1152),
        comp=(2048, 1152),
        pct=125,
        A=(700, 400),
        B=(1100, 700),
    ),
}
LAYOUT = re.compile(
    r"; \[ui\] ui_scale (\d+)%.* on (\d+)x(\d+), frame (\d+)x(\d+): map (\d+)x(\d+)"
)
HOT_BOX = (-8, 56)  # the cursor sprite's box around its hotspot, in frame pixels
CURSOR_MIN_PX = 20  # changed pixels a cursor box must show (frame px / presented px)
MENU_MIN_FRAC = 0.8


def zoom_of(prof):
    """The fit zoom of the composition on the screen (the base map quad's scale) and its origin."""
    cw, ch = prof["comp"]
    sw, sh = SCREEN
    dw, dh = sw, sw * ch // cw
    if dh > sh:
        dh, dw = sh, sh * cw // ch
    return dw / cw, ((sw - dw) // 2, (sh - dh) // 2)


def load(run_dir, name):
    import numpy as np
    from PIL import Image

    return np.asarray(
        Image.open(os.path.join(run_dir, "capture_%s.bmp" % name)).convert("RGB")
    ).astype(np.int16)


def changed(a, b):
    """Boolean mask of the pixels whose colour differs by more than 24 summed channel units."""
    import numpy as np

    return np.abs(a - b).sum(axis=2) > 24


def box_count(mask, x0, y0, x1, y1):
    h, w = mask.shape
    x0, y0, x1, y1 = max(0, int(x0)), max(0, int(y0)), min(w, int(x1)), min(h, int(y1))
    if x1 <= x0 or y1 <= y0:
        return 0
    return int(mask[y0:y1, x0:x1].sum())


def cursor_overlay_rect(run_dir, name):
    """The `; pcapture cursor rect` of the pcapture `name`: None = `none`, a tuple = an overlay cursor, False = no such line."""
    log = os.path.join(run_dir, "mh_capture.log")
    if not os.path.isfile(log):
        return False
    found = False
    for ln in open(log, encoding="utf-8", errors="replace").read().splitlines():
        m = re.search(r"; pcapture cursor rect (none|(-?\d+) (-?\d+) (-?\d+) (-?\d+))", ln)
        if m:
            found = None if m.group(1) == "none" else tuple(int(m.group(i)) for i in range(2, 6))
        elif ln.startswith("; capture_%s.bmp written (presented image" % name):
            return found
    return False


def problems(profile, run_dir):
    prof = PROFILES[profile]
    pre = prof["pre"]
    out = []
    vlog = os.path.join(run_dir, "mh_video.log")
    if not os.path.isfile(vlog):
        return ["no mh_video.log in %s" % run_dir]
    text = open(vlog, encoding="utf-8", errors="replace").read()
    if not re.search(r"; \[ui\] ui_scale in force: %d%%" % prof["pct"], text):
        out.append(
            "no `ui_scale in force: %d%%` line: the percentage never took effect (vacuous)"
            % prof["pct"]
        )
    lay = None
    for ln in text.splitlines():
        lay = LAYOUT.search(ln) or lay
    if not lay or int(lay.group(1)) != 100:
        out.append(
            "the layout line is not the 100 base (`ui_scale 100%% ... composition`): %s -- the row does not exercise the fallback"
            % (lay.group(0)[:80] if lay else "no layout line")
        )
    if "OVERLAY LOST" in text:
        out.append(
            "an `OVERLAY LOST` line: a source was captured for a frame the layout cannot place"
        )
    try:
        fa, fb, fe = (load(run_dir, pre + s) for s in ("_a_frame", "_b_frame", "_esc_frame"))
        ba, bb, be = (load(run_dir, pre + s + "__b") for s in ("_a", "_b", "_esc"))
    except (OSError, ImportError) as e:
        return out + ["cannot read the captures: %s" % e]
    if (fa.shape[1], fa.shape[0]) != prof["frame"] or (ba.shape[1], ba.shape[0]) != SCREEN:
        return out + [
            "capture sizes: frame %dx%d (want %dx%d), presented %dx%d (want %dx%d)"
            % (fa.shape[1], fa.shape[0], *prof["frame"], ba.shape[1], ba.shape[0], *SCREEN)
        ]
    z, (ox, oy) = zoom_of(prof)
    lo, hi = HOT_BOX
    (ax, ay), (bx, by) = prof["A"], prof["B"]
    # 2. exactly one cursor source
    fd = changed(fa, fb)
    in_frame = (
        box_count(fd, ax + lo, ay + lo, ax + hi, ay + hi) >= CURSOR_MIN_PX
        and box_count(fd, bx + lo, by + lo, bx + hi, by + hi) >= CURSOR_MIN_PX
    )
    ov = cursor_overlay_rect(run_dir, pre + "_a")
    if ov is False:
        out.append("no `; pcapture cursor rect` line for %s_a in mh_capture.log" % pre)
    overlay = bool(ov)
    if in_frame == overlay:
        out.append(
            "cursor sources: %s -- exactly one is required (the game's cursor in the frame at the fit zoom, OR an overlay, never both / neither)"
            % (
                "BOTH the frame and an overlay"
                if in_frame
                else "NEITHER the frame nor an overlay (drawn nowhere)"
            )
        )
    elif overlay:
        out.append(
            "the cursor is an overlay (pcapture cursor rect %s) in a layout that falls back to the 100 base: it must be the game's own, in the frame"
            % (ov,)
        )
    # 3. the cursor on screen at the expected point
    pd = changed(ba, bb)
    for nm, (cx, cy) in (("A", (ax, ay)), ("B", (bx, by))):
        n = box_count(
            pd, ox + (cx + lo) * z, oy + (cy + lo) * z, ox + (cx + hi) * z, oy + (cy + hi) * z
        )
        if n < CURSOR_MIN_PX:
            out.append(
                "presented image: %d changed pixels around the cursor at %s=%s (zoom %.2f): the cursor is not on screen there"
                % (n, nm, (cx, cy), z)
            )
    # 4. the ESC menu
    noise = changed(fa, fb)
    import numpy as np

    k = np.ones((9, 9), dtype=bool)  # dilate the animation / cursor noise by 4 px
    pad = np.pad(noise, 4)
    dil = np.zeros_like(noise)
    for dy in range(9):
        for dx in range(9):
            if k[dy, dx]:
                dil |= pad[dy : dy + noise.shape[0], dx : dx + noise.shape[1]]
    ed = changed(fa, fe) & ~dil
    ys, xs = np.nonzero(ed)
    menu_px = int(ed.sum())
    pe = changed(ba, be)
    if menu_px < 3000:
        out.append(
            "the ESC menu is not in the game frame (%d changed frame pixels vs the strategic frame): it was captured out of the frame"
            % menu_px
        )
    else:
        # every in-frame menu pixel must be on screen: its presented point (at the fit zoom) differs from the strategic presented image
        sx = np.minimum(SCREEN[0] - 1, (ox + (xs + 0.5) * z).astype(int))
        sy = np.minimum(SCREEN[1] - 1, (oy + (ys + 0.5) * z).astype(int))
        frac = float(pe[sy, sx].mean())
        if frac < MENU_MIN_FRAC:
            out.append(
                "ESC menu: only %.0f%% of its %d in-frame pixels differ at their presented point from the strategic image: the menu is not on screen"
                % (100 * frac, menu_px)
            )
    # the presented ESC image must differ from the strategic one (a menu drawn nowhere changes nothing)
    cx0, cy0 = SCREEN[0] // 2 - 120, SCREEN[1] // 2 - 120
    if box_count(pe, cx0, cy0, cx0 + 240, cy0 + 240) < 0.2 * 240 * 240:
        out.append(
            "presented ESC image == the strategic one around the screen centre: no menu on screen"
        )
    return out


def selftest():
    fails = []
    import tempfile

    z, org = zoom_of(PROFILES["crop720_175"])
    if z != 2.0 or org != (0, 0):
        fails.append("crop720_175 zoom %r %r" % (z, org))
    z, org = zoom_of(PROFILES["fit_125"])
    if z != 1.25 or org != (0, 0):
        fails.append("fit_125 zoom %r %r" % (z, org))
    with tempfile.TemporaryDirectory() as td:
        if not problems("crop720_175", td):
            fails.append("an empty run dir must be RED")
        open(os.path.join(td, "mh_video.log"), "w").write("; nothing\n")
        if not problems("crop720_175", td):
            fails.append("a log without the layout lines must be RED")
        import numpy as np
        from PIL import Image

        # synthetic run: cursor in neither source, no menu -> RED on both; then a cursor in the frame + a menu -> GREEN
        def mk(green):
            fr, sc = PROFILES["crop720_175"]["frame"], SCREEN
            base_f = np.full((fr[1], fr[0], 3), 40, np.uint8)
            base_s = np.full((sc[1], sc[0], 3), 40, np.uint8)
            P = PROFILES["crop720_175"]
            pre = P["pre"]
            imgs = {}
            for s, pt in (("_a", P["A"]), ("_b", P["B"])):
                f, p = base_f.copy(), base_s.copy()
                if green:
                    f[pt[1] : pt[1] + 20, pt[0] : pt[0] + 12] = 255
                    p[pt[1] * 2 : pt[1] * 2 + 40, pt[0] * 2 : pt[0] * 2 + 24] = 255
                imgs[s] = (f, p)
            fe, pe = imgs["_a"][0].copy(), imgs["_a"][1].copy()
            if green:
                fe[250:450, 500:800] = 200
                pe[500:900, 1000:1600] = 200
            for nm, im in (
                (pre + "_a_frame", imgs["_a"][0]),
                (pre + "_b_frame", imgs["_b"][0]),
                (pre + "_esc_frame", fe),
                (pre + "_a__b", imgs["_a"][1]),
                (pre + "_b__b", imgs["_b"][1]),
                (pre + "_esc__b", pe),
            ):
                Image.fromarray(im).save(os.path.join(td, "capture_%s.bmp" % nm), format="BMP")
            open(os.path.join(td, "mh_video.log"), "w").write(
                "; [ui] ui_scale in force: 175% (game mode 2)\n; [ui] ui_scale 100% on 2560x1440, frame 1280x736: map 2240x1376 x2.00, panel 320 wide\n"
            )
            open(os.path.join(td, "mh_capture.log"), "w").write(
                "; pcapture cursor rect none\n; capture_%s_a.bmp written (presented image 2560x1440 from a 1280x736 frame, ui_scale 100%%, 3 quads)\n"
                % pre
            )

        mk(False)
        if not problems("crop720_175", td):
            fails.append("no cursor and no menu anywhere must be RED")
        mk(True)
        p = problems("crop720_175", td)
        if p:
            fails.append("a cursor in the frame and a menu on screen must be GREEN: %r" % (p,))
        open(os.path.join(td, "mh_capture.log"), "w").write(
            "; pcapture cursor rect 1000 700 1100 800 sprite 0 0 1 1\n; capture_res_crop_720_175_a.bmp written (presented image 2560x1440 from a 1280x736 frame, ui_scale 100%, 3 quads)\n"
        )
        if not any("BOTH" in x or "overlay" in x for x in problems("crop720_175", td)):
            fails.append("cursor in the frame AND as an overlay must be RED")
    print("check_res_fallback selftest: %s" % ("FAIL" if fails else "ok"))
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
            "res_fallback OK: %s -- Interface size %d%% falls back to the 100 base: the game's own cursor is in the frame (no overlay) and on screen, the ESC menu is in the frame and on screen"
            % (argv[0], PROFILES[argv[0]]["pct"])
        )
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
