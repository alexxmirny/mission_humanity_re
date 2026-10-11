#!/usr/bin/env python3
"""check_land_hud.py -- RES3: the landing footprint and the selected landed ship's panel at 2048x1152.

The campaign start lands at a spot whose camera depends on the lane's carried state, so the terrain differs between
two runs of the same script; a pixel baseline of the whole frame is therefore not a stable oracle and the row's
pixel tolerance is opened up. What must hold is read from the captured frames themselves (the PROCESS run directory
test_ui hands over, holding capture_<name>.bmp):

  capture_a_landing_footprint  the hatched green footprint under the descending ship is on the map view: at least
                               MIN_HATCH pixels of the footprint's green (an effect animation the stock view test
                               rejects at 2048 wide on a 64-tile map). The selection ellipse is a thin line and does
                               not reach the count.
  capture_c_selected_ship_panel the selected landed ship's side panel is complete: the dark-green grid area of the
                               ship's tabs and resource rows fills the lower panel (at least MIN_GRID pixels), where
                               the broken panel (icon table overrun at heights above 768) shows only the teal body.

  check_land_hud.py <run-dir>     both frames
  check_land_hud.py --selftest    synthetic frames plant every negative

Absence of a frame is RED.
"""

import os
import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None

EXPECT = "2048x1152"  # the row's pinned render size
W, H = (int(v) for v in EXPECT.split("x"))
PANEL_W = 160
STRIP_H = 32
MIN_HATCH = 1200
MIN_GRID = 40000


def hatch_pixels(img):
    """Pixels of the footprint's saturated green on the map view (right of the panel, above the strip excluded)."""
    px = img.load()
    n = 0
    for y in range(0, H - STRIP_H, 1):
        for x in range(0, W - PANEL_W, 1):
            r, g, b = px[x, y][:3]
            if g >= 150 and r <= 110 and b <= 70:
                n += 1
    return n


def grid_pixels(img):
    """Pixels of the dark-green widget grid in the lower panel (green well above blue; the teal body has blue >= green)."""
    px = img.load()
    n = 0
    for y in range(300, H - STRIP_H, 1):
        for x in range(W - PANEL_W, W, 1):
            r, g, b = px[x, y][:3]
            if g > b + 12 and g > r + 12:
                n += 1
    return n


def find(run_dir, name):
    for ext in (".png", ".bmp"):
        p = os.path.join(run_dir, "capture_%s%s" % (name, ext))
        if os.path.isfile(p):
            return p
    return None


def problems(run_dir, prefix=""):
    """`prefix`: the capture-name prefix of a multi-segment walk (res_walk names them <old row>__<name>)."""
    out = []
    if Image is None:
        return ["PIL is not installed"]
    pa = find(run_dir, prefix + "a_landing_footprint")
    pc = find(run_dir, prefix + "c_selected_ship_panel")
    if pa is None:
        out.append("no capture_%sa_landing_footprint in %s" % (prefix, run_dir))
    else:
        img = Image.open(pa).convert("RGB")
        if img.size != (W, H):
            out.append("capture a is %dx%d, not %dx%d" % (img.size[0], img.size[1], W, H))
        else:
            n = hatch_pixels(img)
            if n < MIN_HATCH:
                out.append(
                    "landing footprint not drawn: %d hatch-green pixels on the map view (< %d)"
                    % (n, MIN_HATCH)
                )
    if pc is None:
        out.append("no capture_%sc_selected_ship_panel in %s" % (prefix, run_dir))
    else:
        img = Image.open(pc).convert("RGB")
        if img.size != (W, H):
            out.append("capture c is %dx%d, not %dx%d" % (img.size[0], img.size[1], W, H))
        else:
            n = grid_pixels(img)
            if n < MIN_GRID:
                out.append(
                    "selected ship's panel incomplete: %d grid pixels in the lower panel (< %d)"
                    % (n, MIN_GRID)
                )
    return out


def selftest():
    import tempfile

    def frame(hatch, grid):
        img = Image.new("RGB", (W, H), (60, 40, 20))
        px = img.load()
        for x in range(W - PANEL_W, W):  # teal panel body
            for y in range(300, H - STRIP_H):
                px[x, y] = (30, 80, 90)
        if hatch:
            for y in range(400, 528):
                for x in range(600, 760):
                    if (x + y) % 4 < 2:
                        px[x, y] = (40, 190, 30)
        if grid:
            for y in range(320, 1100):
                for x in range(W - PANEL_W + 5, W - 5):
                    px[x, y] = (10, 60, 20)
        return img

    cases = [
        ("good", frame(True, True), frame(True, True), False),
        ("no footprint", frame(False, True), frame(True, True), True),
        ("broken panel", frame(True, True), frame(True, False), True),
    ]
    fails = []
    with tempfile.TemporaryDirectory() as td:
        for name, a, c, red in cases:
            a.save(os.path.join(td, "capture_a_landing_footprint.png"))
            c.save(os.path.join(td, "capture_c_selected_ship_panel.png"))
            a.save(os.path.join(td, "capture_x__a_landing_footprint.png"))
            c.save(os.path.join(td, "capture_x__c_selected_ship_panel.png"))
            if bool(problems(td)) != red or bool(problems(td, "x__")) != red:
                fails.append(name)
            if not problems(td, "y__"):
                fails.append(name + ": a wrong prefix must be RED")
    with tempfile.TemporaryDirectory() as td:
        if not problems(td):
            fails.append("missing frames must be RED")
    for n in fails:
        print("SELFTEST FAIL:", n)
    print("check_land_hud selftest: %s" % ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    prefix = ""
    if "--prefix" in argv:
        i = argv.index("--prefix")
        prefix = argv[i + 1] if i + 1 < len(argv) else ""
        argv = argv[:i] + argv[i + 2 :]
    paths = [a for a in argv if not a.startswith("--")]
    if not paths:
        print("usage: check_land_hud.py [--prefix <capture prefix>] <run-dir> | --selftest")
        return 2
    probs = problems(paths[0], prefix)
    for p in probs:
        print("FAIL:", p)
    if not probs:
        print(
            "PASS: the landing footprint is drawn and the selected ship's panel is complete at 2048x1152"
        )
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
