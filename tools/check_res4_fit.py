#!/usr/bin/env python3
"""check_res4_fit.py -- mods:RES17: an explicit frame smaller than the screen's room is FITTED (the whole frame, as at 100), with the panel and strip drawn OVER it (RES21).

The row res4_fit_explicit runs resolution=1280x960 with ui_scale=175 on a (dev-pinned) 2560x1440 screen. The old layout drew the map 1:1 and
centred (composition 1355x975 in the 0.2.2-rc2 playtest). The rule now: the map is enlarged by one factor k = min((Dw - 160 s)/(Fw - 160),
(Dh - 32 s)/(Fh - 32)) (a whole number with integer scaling), the panel and strip stay at s. Here k = 1.49 (height-limited), composition 1950x1440.

Checked from the files of one run (an independent re-derivation; nothing is shared with the C++):
  1. LAYOUT: the layout line of mh_video.log says map 1670x1384 (x1.49), panel 280 wide, strip 56 high, composition 1950x1440 at 305,0 -- it fills the
     screen HEIGHT and the bars are only left and right (aspect bars);
  2. PIXELS: the presented image (capture_res4_fe_view.bmp) equals the composition built from the game frame (capture_res4_fe_frame.bmp): the map quad
     (frame 0,0-1120,928) nearest-sampled into 1670x1384, the strip and the panel at s, black bars; cursor rect exempt;
  3. DIALOG: the ESC capture (capture_res4_fe_esc.bmp) has a dialog quad whose screen rect covers the map quad's image of the dialog's frame rect;
     outside that quad and the cursor the image is still the composition.

  check_res4_fit.py <run-dir>
  check_res4_fit.py --selftest
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_res4_dialog as d  # noqa: E402
import check_ui_quads as q  # noqa: E402

LAYOUT = re.compile(
    r"; \[ui\] ui_scale (\d+)%.* on (\d+)x(\d+), frame (\d+)x(\d+): map (\d+)x(\d+) (1:1|x[\d.]+), panel (\d+) wide \((\d+) frame rows\), strip (\d+) high, composition (\d+)x(\d+) at (\d+),(\d+)"
)
# The two rows: res4_fit_explicit (the dev-pinned 2560x1440 screen, captures res4_fe_*) and `--click` = res4_fit_click (a real 1280x1000 window, res4_fc_*).
PROFILES = {
    "fit": dict(
        want=dict(
            pct=175,
            screen=(2560, 1440),
            frame=(1280, 960),
            map=(1640, 1384),
            panel_w=280,
            strip_h=56,
            comp=(1920, 1440),
            org=(320, 0),
        ),
        pre="res4_fe",
    ),
    "click": dict(
        want=dict(
            pct=175,
            screen=(1280, 1000),
            frame=(1024, 768),
            map=(1000, 904),
            panel_w=280,
            strip_h=56,
            comp=(1280, 960),
            org=(0, 20),
        ),
        pre="res4_fc",
        # units fight on the map (the sim runs between `capture` and `pcapture`): only the left quarter of the map quad, the strip and the panel are compared
        live_map_from=0.25,
    ),
}


def centre(rel, dst, src):
    return ((2 * rel + 1) * src) // (2 * dst)


def build_quads(pct, fw, fh, comp, org, pw, ds):
    """[(src rect, dst rect)] bottom to top -- the RES21 layout as this file derives it from the logged numbers: the whole frame fitted into the composition
    (map / strip / panel at the fit zoom), then the strip and the panel OVER it at the percentage, anchored to the composition's bottom / right edge."""
    cw, ch = comp
    ox, oy = org

    def X(c):
        return ox + (c * cw + fw // 2) // fw

    def Y(c):
        return oy + (c * ch + fh // 2) // fh

    mw, mh = fw - 160, fh - 32
    rows = min(fh, -((-ch * 100) // pct))
    cols = min(fw - 160, -((-(cw - pw) * 100) // pct))
    return [
        ((0, 0, mw, mh), (X(0), Y(0), X(mw), Y(mh))),
        ((0, fh - 32, mw, fh), (X(0), Y(mh), X(mw), Y(fh))),
        ((fw - 160, 0, fw, fh), (X(mw), Y(0), X(fw), Y(fh))),
        ((0, fh - 32, cols, fh), (ox, oy + ch - ds, ox + cw - pw, oy + ch)),
        ((fw - 160, 0, fw, rows), (ox + cw - pw, oy, ox + cw, oy + ch)),
    ]


def expected_pixel(frame, quads, x, y):
    for src, dst in reversed(quads):
        if dst[0] <= x < dst[2] and dst[1] <= y < dst[3]:
            sx = src[0] + centre(x - dst[0], dst[2] - dst[0], src[2] - src[0])
            sy = src[1] + centre(y - dst[1], dst[3] - dst[1], src[3] - src[1])
            return frame[2][sy][sx]
    return (0, 0, 0)


def compare(frame, shown, quads, exempt):
    bad = tot = 0
    first = None
    for y in range(shown[1]):
        for x in range(shown[0]):
            if any(r and r[0] <= x < r[2] and r[1] <= y < r[3] for r in exempt):
                continue
            tot += 1
            if shown[2][y][x] != expected_pixel(frame, quads, x, y):
                bad += 1
                first = first or (x, y)
    return bad, tot, first


def problems(run_dir, profile="fit"):
    out = []
    WANT, pre = PROFILES[profile]["want"], PROFILES[profile]["pre"]
    vlog = os.path.join(run_dir, "mh_video.log")
    if not os.path.isfile(vlog):
        return ["no mh_video.log in %s" % run_dir]
    m = None
    for ln in open(vlog, encoding="utf-8", errors="replace").read().splitlines():
        m = LAYOUT.search(ln) or m
    if not m:
        return [
            "no `; [ui] ui_scale ... composition` layout line: the scale never took effect (vacuous)"
        ]
    g = m.groups()
    pct, sw, sh, fw, fh, mw, mh = (int(v) for v in g[:7])
    k = g[7]
    pw, _rows, ds, cw, ch, ox, oy = (int(v) for v in g[8:])
    got = dict(
        pct=pct,
        screen=(sw, sh),
        frame=(fw, fh),
        map=(mw, mh),
        panel_w=pw,
        strip_h=ds,
        comp=(cw, ch),
        org=(ox, oy),
    )
    for key, want in WANT.items():
        if got[key] != want:
            out.append("layout %s is %r, wanted %r" % (key, got[key], want))
    if ch != sh and cw != sw:
        out.append("the composition %dx%d fills neither the screen height nor its width" % (cw, ch))
    if out:
        return out
    try:
        frame = q.read_bmp(os.path.join(run_dir, "capture_%s_frame.bmp" % pre))
        view = q.read_bmp(os.path.join(run_dir, "capture_%s_view.bmp" % pre))
        esc = q.read_bmp(os.path.join(run_dir, "capture_%s_esc.bmp" % pre))
        escf = q.read_bmp(os.path.join(run_dir, "capture_%s_esc_frame.bmp" % pre))
    except (OSError, ImportError) as e:
        return ["cannot read the captures: %s" % e]
    if (view[0], view[1]) != (sw, sh) or (frame[0], frame[1]) != (fw, fh):
        return [
            "capture sizes: presented %dx%d (want %dx%d), frame %dx%d (want %dx%d)"
            % (view[0], view[1], sw, sh, frame[0], frame[1], fw, fh)
        ]
    quads = build_quads(pct, fw, fh, (cw, ch), (ox, oy), pw, ds)
    cur = q.cursor_rect(run_dir, pre + "_view")
    exempt = [cur]
    lm = PROFILES[profile].get("live_map_from")
    if lm is not None:
        m = quads[0][1]
        exempt.append((m[0] + int((m[2] - m[0]) * lm), m[1], m[2], m[3]))
    bad, tot, first = compare(frame, view, quads, exempt)
    if (
        bad * (300 if lm is not None else 2000) > tot
    ):  # (a blinking minimap marker lives in the panel: `capture` and `pcapture` are two presents apart; measured 0.011%)
        out.append(
            "%d of %d presented pixels differ from the independent composition; first at %s"
            % (bad, tot, first)
        )
    # the ESC dialog
    dlg = d.dialogs_by_capture(run_dir).get(pre + "_esc")
    if not dlg:
        out.append("the ESC capture logged no dialog quad")
        return out
    fr, sc = dlg[-1]
    msrc, mdst = quads[0]
    # the map quad's image of the dialog's frame rect (the 1x copy on screen) must be inside the dialog quad
    cx0 = mdst[0] + (fr[0] - msrc[0]) * (mdst[2] - mdst[0]) // (msrc[2] - msrc[0])
    cx1 = mdst[0] + -((-(fr[2] - msrc[0]) * (mdst[2] - mdst[0])) // (msrc[2] - msrc[0]))
    cy0 = mdst[1] + (fr[1] - msrc[1]) * (mdst[3] - mdst[1]) // (msrc[3] - msrc[1])
    cy1 = mdst[1] + -((-(fr[3] - msrc[1]) * (mdst[3] - mdst[1])) // (msrc[3] - msrc[1]))
    if not (sc[0] <= cx0 and sc[1] <= cy0 and sc[2] >= cx1 and sc[3] >= cy1):
        out.append(
            "the dialog quad %s does not cover the enlarged copy %s" % (sc, (cx0, cy0, cx1, cy1))
        )
    if not (ox <= sc[0] and oy <= sc[1] and sc[2] <= ox + cw and sc[3] <= oy + ch):
        out.append("the dialog quad %s leaves the composition" % (sc,))
    if (sc[2] - sc[0]) * 100 < (fr[2] - fr[0]) * pct - 100:
        out.append(
            "the dialog quad is not scaled by s: %d wide for a %d px frame rect"
            % (sc[2] - sc[0], fr[2] - fr[0])
        )
    curn = q.cursor_rect(run_dir, pre + "_esc")
    bad, tot, first = compare(escf, esc, quads, [curn, sc] + exempt[1:])
    if (
        bad * 1000 > tot
    ):  # the dialog's blank list rect is a layer: the rest of the image is the composition (a stray unit may move)
        out.append(
            "ESC capture: %d of %d pixels outside the dialog differ from the composition; first at %s"
            % (bad, tot, first)
        )
    return out


WANT_MSG = {
    "fit": "whole frame fitted at 1.5, overlays at 175%",
    "click": "whole frame fitted at 1.25 in the 1280x1000 window",
}


def selftest():
    import tempfile

    fails = []
    quads = build_quads(175, 1280, 960, (1920, 1440), (320, 0), 280, 56)
    if (
        quads[0][1] != (320, 0, 2000, 1392)
        or quads[4][1] != (1960, 0, 2240, 1440)
        or quads[3][1] != (320, 1384, 1960, 1440)
    ):
        fails.append("quad destinations")
    if quads[4][0] != (1120, 0, 1280, 823):
        fails.append("panel source rows %r" % (quads[4][0],))
    with tempfile.TemporaryDirectory() as td:
        if not problems(td):
            fails.append("an empty run dir must be RED")
        with open(os.path.join(td, "mh_video.log"), "w") as f:
            f.write(
                "; [ui] ui_scale 175% on 2560x1440, frame 1280x960: map 1120x928 1:1, panel 280 wide (823 frame rows), strip 56 high, composition 1400x984 at 580,228\n"
            )
        if not problems(td):
            fails.append("a 1:1 small box must be RED")
    for n in fails:
        print("SELFTEST FAIL:", n)
    print("check_res4_fit selftest: %s" % ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv):
    if argv and argv[0] == "--selftest":
        return selftest()
    if not argv:
        print(__doc__)
        return 2
    probs = problems(argv[-1], "click" if "--click" in argv else "fit")
    for p in probs:
        print("RED: " + p)
    if not probs:
        print(
            "res4_fit OK: the explicit frame at 175%% is fitted (map %s, composition %s), pixels = the independent composition"
            % (WANT_MSG[("click" if "--click" in argv else "fit")], "filling the screen")
        )
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
