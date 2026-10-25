#!/usr/bin/env python3
"""check_res4_cursor.py -- mods:RES4 stage 5: the owned cursor is at ui_scale, on its hotspot, drawn once, and leaves no trail.

The rows res4_cursor_150 / res4_cursor_200 park the pointer (owned DirectInput, `rawmouse sabs`, CLIENT pixels) on the map, on the
scaled side panel and on the scaled strip, and at each stop take two captures of the same present:

  capture_res4_cf_<k>.bmp   `capture`:  the game's own frame (the framebuffer) -- in a scaled layout the DLL does NOT draw the cursor there;
  capture_res4_cp_<k>.bmp   `pcapture`: the PRESENTED image -- the quad composition with the owned cursor over it.

This checker rebuilds the presented image from the frame with check_ui_quads.expected (its own quad arithmetic, nothing shared with the
C++) and then demands, per stop k at the pointer (cx, cy):

  1. TRAIL / DOUBLE DRAW: every pixel outside the logged cursor rect (`; pcapture cursor rect x0 y0 x1 y1`, mh_capture.log) equals the
     composition -- no stale cursor from the previous stop, nothing else touched;
  2. VISIBLE: inside the rect the picture differs (an overlay that drew nothing is RED);
  3. SCALE + HOTSPOT: the bounding box of the differing pixels matches the sprite's opaque box (`; [ui] cursor sprite bbox`, mh_video.log, in
     scratch pixels with the hotspot at 80,80) scaled by s/100 and hung on the pointer: left = cx - floor(80.5 s) + ceil(bx0 s - .5) ... (+-
     ceil(s) + 1 px: the pointer sits anywhere in its source pixel's span, a pixel equal to its backdrop hides, the rect is clipped to the screen);
  4. NOT IN THE FRAME: the game's frame has no cursor of its own. The frame captures of two stops are compared in the 1x box the cursor
     would have covered at the FIRST stop (the pointer has moved away by the second): at least 97% of those pixels are equal, where a
     cursor drawn into the frame would leave about half of them different.

  check_res4_cursor.py <pct> <screen WxH> <frame WxH> <stop> [<stop> ...] <run-dir>     stop = <name>:<cx>,<cy> (the runner appends the run dir)
  check_res4_cursor.py --selftest
"""

import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_ui_quads as q  # noqa: E402

RECT = re.compile(
    r"; pcapture cursor rect (none|(-?\d+) (-?\d+) (-?\d+) (-?\d+) sprite (-?\d+) (-?\d+) (-?\d+) (-?\d+))"
)
WRITTEN = re.compile(r"; capture_(\S+)\.bmp written \(presented image")


def rects_by_capture(run_dir):
    """{pcapture name: (rect, sprite bbox) or None}: the rect line is logged just before its `written` line."""
    log = os.path.join(run_dir, "mh_capture.log")
    out = {}
    if not os.path.isfile(log):
        return out
    pending = False
    for ln in open(log, encoding="utf-8", errors="replace").read().splitlines():
        m = RECT.search(ln)
        if m:
            pending = (
                None
                if m.group(1) == "none"
                else (
                    tuple(int(m.group(i)) for i in range(2, 6)),
                    tuple(int(m.group(i)) for i in range(6, 10)),
                )
            )
            continue
        m = WRITTEN.search(ln)
        if m and pending is not False:
            out[m.group(1)] = pending
            pending = False
    return out


def predicted_bbox(cx, cy, bb, pct):
    """The screen box the scaled sprite's opaque pixels cover, hung on the pointer (cx, cy)."""
    s = pct / 100.0
    hot = (161 * pct) // 200  # floor(80.5 s)
    x0, y0 = cx - hot, cy - hot
    l = x0 + math.ceil(bb[0] * s - 0.5)
    t = y0 + math.ceil(bb[1] * s - 0.5)
    r = x0 + math.floor((bb[2] + 1) * s - 0.5)
    b = y0 + math.floor((bb[3] + 1) * s - 0.5)
    return l, t, r, b


def frame_point(cx, cy, pct, screen, frame):
    """The frame pixel under the screen point (the pixel-centre rule per quad)."""
    sw, sh = screen
    fw, fh = frame
    pw, ds = q.rnd(160, pct), q.rnd(32, pct)
    mw, mh = sw - pw, sh - ds
    th = mh + ds
    rows = q.ceil_div(th * 100, pct)
    cols = min(fw - 160, q.ceil_div(mw * 100, pct))
    if cx < mw and cy < mh:
        return cx, cy
    if cx < mw:
        return (2 * cx + 1) * cols // (2 * mw), fh - 32 + (2 * (cy - mh) + 1) * 32 // (2 * ds)
    return fw - 160 + (2 * (cx - mw) + 1) * 160 // (2 * pw), (2 * cy + 1) * rows // (2 * th)


def problems(pct, screen, frame_size, run_dir, stops):
    out = []
    rects = rects_by_capture(run_dir)
    bb = None
    fw, fh = frame_size
    sw, sh = screen
    tol = math.ceil(pct / 100.0) + 1
    frames = []
    for name, cx, cy in stops:
        fpath = os.path.join(run_dir, "capture_res4_cf_%s.bmp" % name)
        ppath = os.path.join(run_dir, "capture_res4_cp_%s.bmp" % name)
        for p in (fpath, ppath):
            if not os.path.isfile(p):
                out.append("missing %s" % os.path.basename(p))
        if out:
            continue
        w1, h1, frame = q.read_bmp(fpath)
        w2, h2, shown = q.read_bmp(ppath)
        if (w1, h1) != (fw, fh) or (w2, h2) != (sw, sh):
            out.append(
                "%s: frame %dx%d / presented %dx%d, wanted %dx%d / %dx%d"
                % (name, w1, h1, w2, h2, fw, fh, sw, sh)
            )
            continue
        got_rect = rects.get("res4_cp_%s" % name, False)
        if not got_rect:
            out.append(
                "%s: no cursor rect logged (%r) -- the owned cursor is not in the presented image"
                % (name, got_rect)
            )
            continue
        rect, bb = got_rect
        frames.append((name, cx, cy, frame, bb))
        want = q.expected(frame, fw, fh, sw, sh, pct)
        stale = 0
        first = None
        xs, ys = [], []
        nz_x, nz_y = (
            640 * pct // 100,
            150 * pct // 100,
        )  # RES20: a scaled floating notice is an overlay in the map's top-left zone
        for y in range(sh):
            for x in range(sw):
                if shown[y][x] == want[y][x]:
                    continue
                if x < nz_x and y < nz_y:
                    continue
                if rect[0] <= x < rect[2] and rect[1] <= y < rect[3]:
                    xs.append(x)
                    ys.append(y)
                else:
                    stale += 1
                    first = first or (x, y)
        if stale:
            out.append(
                "%s: %d pixels differ OUTSIDE the cursor rect %s (first %s): a stale or doubled cursor"
                % (name, stale, rect, first)
            )
        if not xs:
            out.append(
                "%s: the presented image shows nothing different inside the cursor rect %s"
                % (name, rect)
            )
            continue
        got = (min(xs), min(ys), max(xs), max(ys))
        pl, pt, pr, pb = predicted_bbox(cx, cy, bb, pct)
        # the part of the predicted box that is on screen
        pl, pt, pr, pb = max(pl, 0), max(pt, 0), min(pr, sw - 1), min(pb, sh - 1)
        if got[0] < pl - tol or got[1] < pt - tol or got[2] > pr + tol or got[3] > pb + tol:
            out.append(
                "%s: drawn box %s leaves the predicted %s (+-%d): wrong scale or hotspot"
                % (name, got, (pl, pt, pr, pb), tol)
            )
        # a hidden pixel (equal to its backdrop) can shave an edge, never much: the drawn box must cover most of the predicted one
        if (got[2] - got[0]) < (pr - pl) - 3 * tol or (got[3] - got[1]) < (pb - pt) - 3 * tol:
            if pl > 0 and pt > 0 and pr < sw - 1 and pb < sh - 1:
                out.append(
                    "%s: drawn box %s is much smaller than the predicted %s: the cursor is not at %d%%"
                    % (name, got, (pl, pt, pr, pb), pct)
                )
    if out or len(frames) < 2:
        return out
    # not in the frame: stop A's 1x cursor box in stop A's frame vs stop B's frame (the pointer has left it)
    (na, cxa, cya, fa, bb), (nb, cxb, cyb, fb, _bb) = frames[0], frames[1]
    px, py = frame_point(cxa, cya, pct, screen, frame_size)
    x0, y0, x1, y1 = (
        max(px - 80 + bb[0], 0),
        max(py - 80 + bb[1], 0),
        min(px - 80 + bb[2], fw - 1),
        min(py - 80 + bb[3], fh - 1),
    )
    tot = same = 0
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            tot += 1
            same += fa[y][x] == fb[y][x]
    if tot and same * 100 < tot * 97:
        out.append(
            "the game frame carries its own cursor: %d of %d pixels of stop %s's 1x cursor box differ once the pointer left (%s)"
            % (tot - same, tot, na, nb)
        )
    return out


def selftest():
    fails = []
    if predicted_bbox(100, 100, (80, 80, 99, 105), 100) != (100, 100, 119, 125):
        fails.append(
            "predicted_bbox at 100%% %s" % (predicted_bbox(100, 100, (80, 80, 99, 105), 100),)
        )
    l, t, r, b = predicted_bbox(500, 400, (80, 80, 99, 105), 200)
    if (l, t) != (499, 399):
        fails.append("hotspot at 200%%: %s" % ((l, t, r, b),))
    if (r - l) < 36 or (r - l) > 41:
        fails.append("width at 200%%: %d" % (r - l))
    if frame_point(10, 20, 150, (400, 200), (448, 192)) != (10, 20):
        fails.append("frame_point on the map")
    import tempfile

    with tempfile.TemporaryDirectory() as td:
        if not problems(150, (400, 200), (448, 192), td, [("a", 1, 1), ("b", 2, 2)]):
            fails.append("a run dir with no captures must be RED")
    for n in fails:
        print("SELFTEST FAIL:", n)
    print("check_res4_cursor selftest: %s" % ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    if len(argv) < 6:
        print(__doc__)
        return 2
    pct = int(argv[0])
    screen = tuple(int(v) for v in argv[1].split("x"))
    frame = tuple(int(v) for v in argv[2].split("x"))
    stops = []
    for a in argv[3:-1]:
        name, xy = a.split(":")
        x, y = xy.split(",")
        stops.append((name, int(x), int(y)))
    probs = problems(pct, screen, frame, argv[-1], stops)
    for p in probs:
        print("FAIL:", p)
    if not probs:
        print(
            "PASS: the owned cursor is at %d%% on its hotspot at %d stops, drawn once, no trail"
            % (pct, len(stops))
        )
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
