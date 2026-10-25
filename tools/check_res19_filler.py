#!/usr/bin/env python3
"""check_res19_filler.py -- mods:RES23 S0/S2 / RES19: the HUD regions the presenter must keep filled after an Interface size change.

RES19 as the user saw it (d3d11, rc3 replay, S0 capture final_d3d11_r19_*__b): after Options -> Interface size 200 -> 125 -> Apply, the
bottom-strip EXTENSION right of the status bar and the CORNER under the side panel are black (they are redrawn only on the game's
resize-pending flag after clear_strip_surface zeroed them). The rows res19_user_d3d11 / res19_user_gdi take pcapture AND bcapture (what
the presenter put on screen) in the ESC menu after Apply (`r19_menu`), after the resume (`r19_resume`), 600 ticks later (`r19_later`), plus
the 200% reference (`r19_200`).

Three regions per capture, every rect derived from the mh_video.log layout of the phase in force (200% layout for r19_200 / r19_menu --
the latch keeps it -- and the 125% layout for r19_resume / r19_later):
    `; [ui] ui_scale P% ... frame FWxFH: map .., panel PW wide (R frame rows), strip SH high, composition CWxCH at X,Y`
    `; [ui] ui_scale: P% -> effective E.EE (fits K whole rows)`   (whole rows end at frame row LIST_TOP + K * LIST_ROW = 248 + K * 52)
  panel   the side panel's filler below its last whole row (above the strip);
  ext     the strip span right of the status bar and left of the panel (from 65% of those columns);
  corner  the panel's columns in the bottom quarter of the strip.
A region is BLACK when at least BLACK_AT[region] of its pixels have every channel < 16; otherwise it is FILLED.

  check_res19_filler.py <presenter> <run-dir> [--expect filled|any]   (default filled)
      --expect filled: RED when any bcapture region is black (the acceptance).   --expect any: report only.
  check_res19_filler.py --selftest
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

LIST_TOP, LIST_ROW = 248, 52
LAYOUT = re.compile(
    r"; \[ui\] ui_scale (\d+)%.*? on (\d+)x(\d+), frame (\d+)x(\d+): map .*?, panel (\d+) wide \((\d+) frame rows\), "
    r"strip (\d+) high, composition (\d+)x(\d+) at (-?\d+),(-?\d+)"
)
EFFECTIVE = re.compile(
    r"; \[ui\] ui_scale: (\d+)% -> effective (\d+)\.(\d+) \(fits (\d+) whole rows\)"
)
PHASE_LAYOUT = {
    "r19_200": 0,
    "r19_menu": -1,  # RES26: the 125% layout shows in the menu right after Apply
    "r19_resume": -1,
    "r19_later": -1,
}  # index into the layout lines (in presentation order)


def layouts(text):
    """[(pct, screen_w, screen_h, panel_w, comp_w, comp_h, x, y, eff_pct, whole_rows, strip_h)] for each multi-quad layout line, in order."""
    out = []
    lines = text.splitlines()
    for i, ln in enumerate(lines):
        m = LAYOUT.search(ln)
        if not m:
            continue
        g = [int(v) for v in m.groups()]
        eff, k = None, None
        for back in (1, 2):  # the `effective` line is logged just before the layout line
            if i - back >= 0:
                e = EFFECTIVE.search(lines[i - back])
                if e:
                    eff, k = int(e.group(2)) * 100 + int(e.group(3)), int(e.group(4))
        out.append((g[0], g[1], g[2], g[5], g[8], g[9], g[10], g[11], eff, k, g[7]))
    return out


def regions(layout):
    """{name: (x0, y0, x1, y1)} in screen pixels, all derived from the layout line (composition CWxCH at X,Y; panel PW wide; strip SH high).

    panel      the side panel's filler below its last whole row (needs the `fits K whole rows` line; absent otherwise)
    ext        the bottom-strip EXTENSION: the strip's span right of the status bar, left of the side panel. The status bar is at most
               ~60% of the strip's columns left of the panel (the ESC-menu prompt bar is the widest), so the band starts at 65%.
    corner     the strip corner under the side panel: the panel's columns, bottom quarter of the strip's rows (RES19 as the user
               saw it on d3d11: both are BLACK after an Interface size change).
    """
    pct, sw, sh, pw, cw, ch, x, y, eff, k, strip = layout
    x1 = x + cw
    out = {}
    if eff is not None and k is not None:
        # the panel quad's REAL scale is its width over the 160 frame columns it shows (a map fit can exceed the percentage in force)
        y0 = y + (LIST_TOP + k * LIST_ROW) * pw // 160
        mx = max(4, pw // 12)
        if y0 + 3 < y + ch - 1 - strip:  # no gap, no band (the whole rows reach the strip)
            out["panel"] = (x1 - pw + mx, y0 + 3, x1 - mx, y + ch - 1 - strip)
    ys = y + ch - strip
    left = x1 - pw - x  # columns left of the panel
    out["ext"] = (x + left * 65 // 100, ys + strip // 4, x + left * 97 // 100, y + ch - strip // 10)
    out["corner"] = (x1 - pw + max(4, pw // 12), y + ch - strip // 4, x1 - max(4, pw // 12), y + ch)
    return out


# a region is BLACK when this fraction of its pixels has every channel < 16
BLACK_AT = {"panel": 0.9, "ext": 0.5, "corner": 0.25}


def measure(path, b):
    from PIL import Image

    im = Image.open(path).convert("RGB")
    x0, y0, x1, y1 = b
    x0, y0, x1, y1 = max(0, x0), max(0, y0), min(im.size[0], x1), min(im.size[1], y1)
    if x1 <= x0 or y1 <= y0:
        return None
    import numpy as np

    a = np.asarray(im.crop((x0, y0, x1, y1)), dtype=np.int32).reshape(-1, 3)
    n = len(a)
    black = int((a.max(axis=1) < 16).sum())
    mean = tuple(int(v) for v in a.mean(axis=0))
    return {"band": (x0, y0, x1, y1), "mean": mean, "black_frac": black / n, "n": n}


def verdict(m, region="panel"):
    return "BLACK" if m["black_frac"] >= BLACK_AT[region] else "filled"


def run(presenter, run_dir, expect="filled"):
    probs, rep = [], []
    vlog = os.path.join(run_dir, "mh_video.log")
    if not os.path.isfile(vlog):
        return ["no mh_video.log in %s" % run_dir], rep
    lays = layouts(open(vlog, encoding="utf-8", errors="replace").read())
    if len(lays) < 2:
        return [
            "fewer than two multi-quad layout lines in mh_video.log (%d): the 200%% and 125%% layouts were not both logged"
            % len(lays)
        ], rep
    reproduced = False
    for name, ix in PHASE_LAYOUT.items():
        lay = lays[ix]
        for kind, fn in (
            ("bcapture", "capture_%s__b.bmp" % name),
            ("pcapture", "capture_%s.bmp" % name),
        ):
            path = os.path.join(run_dir, fn)
            if not os.path.isfile(path):
                if kind == "pcapture":
                    continue  # the pcapture twin is only taken where a pair is compared (r19_menu, r19_resume)
                probs.append("%s: %s missing (%s)" % (name, kind, fn))
                continue
            for rname, b in regions(lay).items():
                m = measure(path, b)
                if not m:
                    probs.append("%s: %s %s band %r is outside the image" % (name, kind, rname, b))
                    continue
                v = verdict(m, rname)
                rep.append(
                    "%s %-8s [%-10s] %-6s layout %d%% band x%d..%d y%d..%d: mean rgb %r, %.1f%% black -> %s"
                    % (
                        presenter,
                        kind,
                        name,
                        rname,
                        lay[0],
                        m["band"][0],
                        m["band"][2],
                        m["band"][1],
                        m["band"][3],
                        m["mean"],
                        m["black_frac"] * 100,
                        v,
                    )
                )
                if kind == "bcapture" and v == "BLACK":
                    reproduced = True
                    if expect == "filled":
                        probs.append(
                            "%s [%s]: the %s band is BLACK in the presenter output (RES19)"
                            % (presenter, name, rname)
                        )
    rep.append(
        "RES19 %s on the %s presenter (a black panel filler / strip extension / strip corner in a bcapture)"
        % ("REPRODUCED" if reproduced else "NOT reproduced", presenter)
    )
    return probs, rep


def selftest():
    import tempfile

    from PIL import Image

    fails = []
    text = (
        "; [ui] ui_scale: 200% -> effective 2.00 (fits 9 whole rows)\n"
        "; [ui] ui_scale 200% on 2560x1440, frame 1280x960: map 1660x1376 x1.48, panel 320 wide (720 frame rows), strip 64 high, composition 1980x1440 at 290,0; ideal render 2048x1408\n"
        "; [ui] ui_scale: 125% -> effective 1.25 (fits 11 whole rows)\n"
        "; [ui] ui_scale 125% on 2560x1440, frame 1280x960: map 1280x960 1:1, panel 200 wide (960 frame rows), strip 40 high, composition 1600x1200 at 480,120; ideal render 2048x1408\n"
    )
    ls = layouts(text)
    if len(ls) != 2 or ls[0][8] != 200 or ls[1][9] != 11:
        fails.append("layout parse: %r" % (ls,))
    with tempfile.TemporaryDirectory() as td:
        open(os.path.join(td, "mh_video.log"), "w").write(text)
        for n in PHASE_LAYOUT:
            Image.new("RGB", (2560, 1440), (0, 0, 0)).save(
                os.path.join(td, "capture_%s__b.bmp" % n)
            )
            Image.new("RGB", (2560, 1440), (0, 90, 90)).save(os.path.join(td, "capture_%s.bmp" % n))
        if not run("d3d11", td, "filled")[0]:
            fails.append("a black bcapture band with --expect filled must be RED")
        if run("d3d11", td, "any")[0]:
            fails.append("--expect any must stay green on a black band")
        if "REPRODUCED" not in run("d3d11", td, "any")[1][-1]:
            fails.append("the verdict line must say REPRODUCED")
    for f in fails:
        print("SELFTEST FAIL:", f)
    print("check_res19_filler selftest: %s" % ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv):
    if argv and argv[0] == "--selftest":
        return selftest()
    expect = "filled"
    args = []
    i = 0
    while i < len(argv):
        if argv[i] == "--expect":
            expect = argv[i + 1]
            i += 1
        else:
            args.append(argv[i])
        i += 1
    if len(args) != 2:
        print(__doc__)
        return 2
    probs, rep = run(args[0], args[1], expect)
    for r in rep:
        print("  res19: " + r)
    for p in probs:
        print("RED: " + p)
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
