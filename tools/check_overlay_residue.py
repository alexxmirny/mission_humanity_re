#!/usr/bin/env python3
"""check_overlay_residue.py -- mp:GX1: our DLL-native overlays must not smear on the ground layer.

THE BUG (mp:GX1). Retail
repaints a strategic/tactical ground tile only if the DAMAGE MAP (`_G_LLM_TILE_VIS_MAP`) says the
tile is dirty, and every retail primitive that touches the framebuffer marks the tile it touched.
mh/seams/gfx_overlay.cpp and mh/seams/ui_net_indicator.cpp write pixels of their own and, before
this item, marked nothing -- so once a frame's box shrank (a page switch, a shorter number) or the
overlay hid, the pixels it is no longer covering never get repainted by anything, in any mode with no
ground pass at all. The fix is mh/include/mh_tile_dirty.h's `mark_ground_tiles_dirty` (a mode with a
ground pass) plus gfx_overlay.cpp's own `clear_rect` hard reset (any mode where nothing else will
ever touch those pixels again).

WHAT THIS CHECKS, AND WHY A PIXEL READ RATHER THAN A LOG LINE. The bug is specifically about pixels
staying on screen that should not -- there is no log line that could stand in for "is there still a
glyph there" the way check_net_indicator.py's log lines stand in for "did the numbers make sense".
So this reads two CAPTURED frames (a UI-suite `capture <name>` pair, .bmp as the harness writes them
or .png after pull_local_captures converts them) and asks, over one RECT the caller names (the region
a wider/taller frame covered that a narrower/shorter later frame does not):

  * the FIRST frame really had the overlay's own text colour there -- the sanity clause. A rect with
    nothing drawn in it proves nothing either way, and a scenario that regresses to "nothing is drawn
    at all" must not read as a pass.
  * the SECOND frame has NONE -- the actual assertion.

COLOUR MATCH, NOT A BRIGHTNESS THRESHOLD (learned the hard way, 2026-09-22: the first cut of this
checker used a plain luminance ("ink") test, the same idea tools/ui_test.py's diff_capture(only=
{"mode":"ink"}) already uses for text drawn in a colour the GAME picks per run. That is the right
call there -- masks a colour a diff cannot know ahead of time. It is the WRONG call here: this
checker knows the exact colour it is looking for (the seam's own `[debug] color=` / `[hud]
net_indicator_color=`, always drawn at full un-halved brightness -- only the darkened BACKGROUND
behind the text is halved by dim_rect), and a live in-game capture's ground terrain is its own kind
of bright (measured: strategic ground art peaks around RGB(90,97,99), comfortably over a luminance
threshold that would pass on a black menu background). A luminance-only version of this checker
reported "residue" identically whether the fix was built in or knobbed out, because it was reading
terrain brightness, not glyph colour -- caught by re-running the SAME scenario with the fix compiled
back in and getting the identical false failure. Matching the configured colour within --tol per
channel is what actually tells a stray yellow glyph pixel (255,255,132) apart from teal terrain
(49,93,107): the former is within a few counts of (255,255,128) on every channel, the latter is not
even close on R or G.

    python tools/check_overlay_residue.py <run dir> --rect X,Y,W,H --before NAME --after NAME
    python tools/check_overlay_residue.py --selftest      # no rig, no captures -- synthetic BMPs
"""

import argparse
import glob
import os
import sys
import tempfile

NL = "\n"
DEFAULT_COLOR = (0xFF, 0xFF, 0x80)  # matches this repo's [debug]/[hud] *_color= ini default
DEFAULT_TOL = 40  # per-channel; real glyph pixels observed at (255,255,132) vs target (255,255,128)


def _find_capture(run_dir, name):
    """capture_<name>.bmp|png under run_dir -- the raw .bmp the harness writes, or a .png a prior
    pull_local_captures() already converted it to (tools/ui_test.py bmp_to_png)."""
    for ext in (".bmp", ".png"):
        hits = glob.glob(os.path.join(run_dir, "capture_%s%s" % (name, ext)))
        if hits:
            return hits[0]
    # a bare run_dir sometimes IS the session dir but the captures sit one level down (SES1
    # sub-sessions) -- widen once, the same one hop find_log-style checkers already take.
    for ext in (".bmp", ".png"):
        hits = glob.glob(os.path.join(run_dir, "*", "capture_%s%s" % (name, ext)))
        if hits:
            return sorted(hits)[-1]
    return None


def ink_count(image_path, rect, color, tol):
    """Pixels inside `rect` (x,y,w,h, clipped to the image) within `tol` of `color` on EVERY
    channel -- a colour match, not a brightness threshold (see the module docstring for why a
    threshold false-positived on live terrain art)."""
    import numpy as np
    from PIL import Image

    img = Image.open(image_path).convert("RGB")
    W, H = img.size
    x, y, w, h = rect
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(W, x + w), min(H, y + h)
    if x1 <= x0 or y1 <= y0:
        return 0
    crop = np.asarray(img.crop((x0, y0, x1, y1))).astype(np.int16)  # (h, w, 3)
    target = np.array(color, dtype=np.int16)
    diff = np.abs(crop - target)
    return int((diff.max(axis=2) <= tol).sum())


def check(run_dir, before, after, rect, color, tol, min_before):
    """(ok, text). ok requires: the BEFORE capture had >= min_before pixels of `color` in `rect`
    (the sanity clause -- something was really drawn there) AND the AFTER capture has ZERO."""
    before_path = _find_capture(run_dir, before)
    after_path = _find_capture(run_dir, after)
    if not before_path:
        return False, "no capture named %r under %s" % (before, run_dir)
    if not after_path:
        return False, "no capture named %r under %s" % (after, run_dir)
    before_ink = ink_count(before_path, rect, color, tol)
    after_ink = ink_count(after_path, rect, color, tol)
    lines = [
        "  rect=%s color=%s tol=%d" % (list(rect), list(color), tol),
        "  before (%s): %d matching pixel(s) (want >= %d)"
        % (os.path.basename(before_path), before_ink, min_before),
        "  after  (%s): %d matching pixel(s) (want 0)" % (os.path.basename(after_path), after_ink),
    ]
    if before_ink < min_before:
        lines.append(
            "FAIL: the BEFORE capture shows no glyph in the rect at all -- this proves nothing "
            "(the rect, the page content, or the capture step is wrong, not that the fix works)"
        )
        return False, NL.join(lines)
    if after_ink > 0:
        lines.append(
            "FAIL: the AFTER capture still carries %d pixel(s) matching the overlay's own text "
            "colour -- the ground-tile stamp / hard-clear did not run (or ran over the wrong rect)"
            % after_ink
        )
        return False, NL.join(lines)
    lines.append("ok: the AFTER capture carries no glyph of the BEFORE frame in the rect")
    return True, NL.join(lines)


def _parse_rect(s):
    parts = [int(p) for p in s.split(",")]
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("--rect wants x,y,w,h")
    return tuple(parts)


def _parse_color(s):
    s = s.strip().lstrip("#")
    if len(s) != 6:
        raise argparse.ArgumentTypeError("--color wants RRGGBB")
    return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16))


# ---- selftest: synthetic BMP pairs, no rig, no game ------------------------------------------------


def _make_bmp(path, size, base_rgb, ink_rect=None, ink_rgb=DEFAULT_COLOR):
    from PIL import Image

    img = Image.new("RGB", size, base_rgb)
    if ink_rect:
        x, y, w, h = ink_rect
        px = img.load()
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                px[xx, yy] = ink_rgb
    img.save(path)


def selftest():
    fails = []
    rect = (10, 10, 20, 8)
    size = (64, 40)
    color, tol = DEFAULT_COLOR, DEFAULT_TOL
    # A bright, plausibly in-game-terrain-like colour that is NOT anywhere near the overlay's own
    # (measured live: strategic ground art peaks around this shade) -- this is the case that broke
    # the first (luminance-only) version of this checker: bright, but the WRONG colour.
    TERRAIN_LIKE = (49, 93, 107)

    # POSITIVE arm (the fix in place): BEFORE has the overlay's own colour in the rect (the wide
    # page's title row); AFTER has it cleared (either mark_ground_tiles_dirty's real-terrain repaint
    # or clear_rect's hard reset -- both leave the rect free of that colour) -- must PASS.
    with tempfile.TemporaryDirectory() as tmp:
        _make_bmp(os.path.join(tmp, "capture_wide.bmp"), size, (0, 0, 0), rect)
        _make_bmp(os.path.join(tmp, "capture_narrow.bmp"), size, (0, 0, 0), None)
        ok, text = check(tmp, "wide", "narrow", rect, color, tol, min_before=1)
        print("  positive arm (stamped/cleared)                    ok=%s (want True)" % ok)
        if not ok:
            fails.append("positive arm: %s%s%s" % (NL, text, NL))

    # POSITIVE arm 2 (the real bug this replaced): AFTER is cleared to TERRAIN, not black -- exactly
    # what mark_ground_tiles_dirty's real repaint looks like in a ground-pass mode, and exactly the
    # case a plain luminance threshold could not tell apart from residue. Must still PASS.
    with tempfile.TemporaryDirectory() as tmp:
        _make_bmp(os.path.join(tmp, "capture_wide.bmp"), size, (0, 0, 0), rect)
        _make_bmp(
            os.path.join(tmp, "capture_narrow.bmp"), size, (0, 0, 0), rect, ink_rgb=TERRAIN_LIKE
        )
        ok, text = check(tmp, "wide", "narrow", rect, color, tol, min_before=1)
        print("  positive arm 2 (repainted with real terrain)      ok=%s (want True)" % ok)
        if not ok:
            fails.append("positive arm 2: %s%s%s" % (NL, text, NL))

    # NEGATIVE arm (mp:GX1's own bug, no stamp at all): AFTER is IDENTICAL to BEFORE in the rect --
    # nothing ever touched those pixels, which is exactly what mark_ground_tiles_dirty/clear_rect
    # existing but never being CALLED would look like. Must go RED.
    with tempfile.TemporaryDirectory() as tmp:
        _make_bmp(os.path.join(tmp, "capture_wide.bmp"), size, (0, 0, 0), rect)
        _make_bmp(os.path.join(tmp, "capture_narrow.bmp"), size, (0, 0, 0), rect)  # NOT cleared
        ok, text = check(tmp, "wide", "narrow", rect, color, tol, min_before=1)
        print("  negative arm (no stamp -- the planted bug)        ok=%s (want False)" % ok)
        if ok:
            fails.append(
                "negative arm: reported ok=True but the residue was planted and left in%s%s"
                % (NL, text)
            )

    # SANITY arm: a BEFORE capture with nothing drawn must refuse to pass, not vacuously succeed.
    with tempfile.TemporaryDirectory() as tmp:
        _make_bmp(os.path.join(tmp, "capture_wide.bmp"), size, (0, 0, 0), None)
        _make_bmp(os.path.join(tmp, "capture_narrow.bmp"), size, (0, 0, 0), None)
        ok, text = check(tmp, "wide", "narrow", rect, color, tol, min_before=1)
        print("  sanity arm (before never drew anything)           ok=%s (want False)" % ok)
        if ok:
            fails.append(
                "sanity arm: reported ok=True with no ink in the BEFORE capture at all%s%s"
                % (NL, text)
            )

    # MISSING-CAPTURE arm: a typo'd --before/--after name must refuse rather than silently pass.
    with tempfile.TemporaryDirectory() as tmp:
        _make_bmp(os.path.join(tmp, "capture_wide.bmp"), size, (0, 0, 0), rect)
        ok, text = check(tmp, "wide", "does_not_exist", rect, color, tol, min_before=1)
        print("  missing-capture arm                                ok=%s (want False)" % ok)
        if ok:
            fails.append("missing-capture arm: reported ok=True with no 'after' capture on disk")

    # COLOUR-BLINDNESS arm: the regression this rewrite fixes. A BRIGHT (terrain-like) but
    # WRONG-coloured AFTER must not read as residue -- if this checker were still luminance-only it
    # would fail here exactly as the live rig run did.
    with tempfile.TemporaryDirectory() as tmp:
        _make_bmp(os.path.join(tmp, "capture_wide.bmp"), size, (0, 0, 0), rect)
        _make_bmp(
            os.path.join(tmp, "capture_narrow.bmp"), size, TERRAIN_LIKE, None
        )  # bright BACKGROUND, no glyph
        ok, text = check(tmp, "wide", "narrow", rect, color, tol, min_before=1)
        print("  colour-blindness arm (bright terrain, no glyph)   ok=%s (want True)" % ok)
        if not ok:
            fails.append("colour-blindness arm: %s%s%s" % (NL, text, NL))

    for f in fails:
        print("SELFTEST FAIL: %s" % f)
    print(
        "check_overlay_residue --selftest: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails))
    )
    return 0 if not fails else 1


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("run_dir", help="session/run directory holding capture_<name>.bmp|png")
    ap.add_argument(
        "--before", required=True, help="capture name (without capture_/.bmp) with the glyph"
    )
    ap.add_argument("--after", required=True, help="capture name that must show no residue of it")
    ap.add_argument(
        "--rect", required=True, type=_parse_rect, help="x,y,w,h -- the vacated region to scan"
    )
    ap.add_argument(
        "--color",
        type=_parse_color,
        default=DEFAULT_COLOR,
        help="RRGGBB -- the overlay's own configured text colour (default ffff80, this repo's ini default)",
    )
    ap.add_argument(
        "--tol",
        type=int,
        default=DEFAULT_TOL,
        help="per-channel match tolerance (default %d)" % DEFAULT_TOL,
    )
    ap.add_argument(
        "--min-before",
        type=int,
        default=1,
        help="minimum matching pixels required in --before (default 1)",
    )
    args = ap.parse_args()
    ok, text = check(
        args.run_dir, args.before, args.after, args.rect, args.color, args.tol, args.min_before
    )
    print(text)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
