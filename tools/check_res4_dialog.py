#!/usr/bin/env python3
"""check_res4_dialog.py -- mods:RES4 stages 6 / 6b: a gameplay-size dialog is presented at ui_scale as a TRANSLUCENT layer over the true map.

The rows res4_dlg_150 / res4_dlg_200 / res4_dlg_confirm_150 open the ESC menu, the Options screen it pushes and the Quit confirmation over it at
ui_scale > 100 and, per stage <k>, take two captures of the same present:

  capture_res4_df_<k>.bmp   `capture`:  the game's own frame. Since 6b it holds NO dialog: the list draw was diverted into a scratch (the
                            premultiplied layer), so the frame under the dialog is the true background (the map);
  capture_res4_dp_<k>.bmp   `pcapture`: the PRESENTED image: HUD quads + the dialog layers (alpha-blended, scaled) + the owned cursor.

`pcapture` logs `; pcapture dialog x0 y0 x1 y1 screen x0 y0 x1 y1` per dialog quad and `; pcapture dialog layers N` (mh_capture.log, just before
its `written` line). The row also runs with [video] ui_scale_dialog_check=1, which logs (mh_net.log) the recovered layer composited at 1x over
the true backdrop against the stock draw, per list. Per stage this checker demands

  1. QUAD: a dialog line (not `none`) is logged, and `dialog layers` == the number of quads (every quad is a layer, none is a frame copy);
  2. RECT: the frame rectangle is the one the game centred ((W - 160) - w)/2 or (W - w)/2, (H - h)/2 within 2 px (a wrong read is RED);
  3. SIZE: the screen rectangle is round(w s) x round(h s);
  4. COVER + FIT: it contains the 1x place and lies inside the screen, centred on the visible map within 40 px;
  5. PIXELS: outside every dialog quad (and the cursor rect) the presented image equals the independent HUD composition
     (check_ui_quads.expected) of the 1x frame -- so nothing of the dialog is left in the frame, and nothing stale;
  6. NOT VACUOUS: inside the topmost quad the presented image differs from that HUD composition (the layer is drawn), yet the map shows
     through (a share of its pixels is the backdrop colour or near it);
  7. LAYER CHECK (mh_net.log): every `dialog layer check` line is within tolerance: max error <= 2 units of the 565 channel and under 0.05% of the
     pixels above 1 (measured: ESC menu 0 px, Options screen <= 5 px of 119340, all green/G channel);
and across stages: the dialog's pixels differ between the first two stages (the click on the scaled Options button opened another screen).

  check_res4_dialog.py <pct> <screen WxH> <frame WxH> <stage>[@x0,y0,x1,y1] [<stage> ...] <run-dir>
  (`@rect` = a live animation rect exempt from the pixel equality of check 5; used by the stage 7 building dialog, whose sim keeps running)
  check_res4_dialog.py --selftest
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_ui_quads as q  # noqa: E402

DIALOG = re.compile(
    r"; pcapture dialog (none|(-?\d+) (-?\d+) (-?\d+) (-?\d+) screen (-?\d+) (-?\d+) (-?\d+) (-?\d+))"
)
LAYERS = re.compile(r"; pcapture dialog layers (\d+)")
LAYER_CHECK = re.compile(
    r"; RES4: dialog layer check slot (\d+) list (\w+) rect (\d+),(\d+) (\d+)x(\d+): max err r=(\d+) g=(\d+) b=(\d+) \(565 units\), (\d+) of (\d+) px above 1"
)
WRITTEN = re.compile(r"; capture_(\S+)\.bmp written \(presented image")


def dialogs_by_capture(run_dir):
    """{pcapture name: [(frame rect, screen rect), ...] bottom to top, or None for `none`}"""
    log = os.path.join(run_dir, "mh_capture.log")
    out = {}
    if not os.path.isfile(log):
        return out
    pending = False
    acc = []
    for ln in open(log, encoding="utf-8", errors="replace").read().splitlines():
        m = DIALOG.search(ln)
        if m:
            if m.group(1) == "none":
                pending = None
            else:
                if pending is None or pending is False:
                    acc = []
                acc.append(
                    (
                        tuple(int(m.group(i)) for i in range(2, 6)),
                        tuple(int(m.group(i)) for i in range(6, 10)),
                    )
                )
                pending = acc
            continue
        m = WRITTEN.search(ln)
        if m and pending is not False:
            out[m.group(1)] = pending
            pending = False
            acc = []
    return out


def layers_by_capture(run_dir):
    """{pcapture name: number of dialog layers composited}"""
    log = os.path.join(run_dir, "mh_capture.log")
    out = {}
    if not os.path.isfile(log):
        return out
    n = None
    for ln in open(log, encoding="utf-8", errors="replace").read().splitlines():
        m = LAYERS.search(ln)
        if m:
            n = int(m.group(1))
            continue
        m = WRITTEN.search(ln)
        if m:
            out[m.group(1)] = n
            n = None
    return out


LOOSE = False


def layer_check_problems(run_dir, want_lines=1):
    """The dev proof lines of mh_net.log: the recovered layer vs the stock draw, per list."""
    log = os.path.join(run_dir, "mh_net.log")
    out, seen = [], 0
    if not os.path.isfile(log):
        return ["no mh_net.log in the run dir: the layer check cannot be read"]
    for ln in open(log, encoding="utf-8", errors="replace").read().splitlines():
        m = LAYER_CHECK.search(ln)
        if not m:
            continue
        seen += 1
        mx = max(int(m.group(i)) for i in (7, 8, 9))
        over, tot = int(m.group(10)), int(m.group(11))
        # LOOSE (the `loose` stage token): the sell confirmation's resource row (stage 7, res4_bst_200) measured max green error 7 on 37 of 72000 px
        # (0.05%): the same one-alpha limit as the Options screen, on glyph pixels. Still bounded: <= 8 units and <= 0.1% of the pixels.
        lim, per = (8, 1000) if LOOSE else (2, 2000)
        if mx > lim or over * per > tot:
            out.append(
                "layer check red: max err %d, %d of %d px above 1: %s" % (mx, over, tot, ln.strip())
            )
    if seen < want_lines:
        out.append(
            "only %d `dialog layer check` line(s) in mh_net.log, wanted %d: the proof did not run"
            % (seen, want_lines)
        )
    return out


def compose_dialog(base, frame, fsrc, dst):
    """Overlay the dialog quad on a copy of `base` (rows of pixel tuples): nearest by the pixel-centre rule."""
    out = [row[:] for row in base]
    sw, sh = fsrc[2] - fsrc[0], fsrc[3] - fsrc[1]
    dw, dh = dst[2] - dst[0], dst[3] - dst[1]
    for y in range(max(dst[1], 0), min(dst[3], len(out))):
        sy = fsrc[1] + (2 * (y - dst[1]) + 1) * sh // (2 * dh)
        for x in range(max(dst[0], 0), min(dst[2], len(out[0]))):
            sx = fsrc[0] + (2 * (x - dst[0]) + 1) * sw // (2 * dw)
            out[y][x] = frame[sy][sx]
    return out


def geometry_problems(name, pct, screen, frame_size, fsrc, dst):
    out = []
    fw, fh = frame_size
    sw, sh = screen
    w, h = fsrc[2] - fsrc[0], fsrc[3] - fsrc[1]
    # the game centres a dialog in the map VIEW (WindowWidth = the frame less the 160-wide side panel, the ESC menu) or in the whole frame
    # width (the generic confirmation); vertically in WindowHeight
    if (
        min(abs(2 * fsrc[0] + w - (fw - 160)), abs(2 * fsrc[0] + w - fw)) > 4
        or abs(2 * fsrc[1] + h - fh) > 4
    ):
        out.append(
            "%s: frame rect %s (%dx%d) is not centred in the %dx%d frame: the list rect was misread"
            % (name, fsrc, w, h, fw, fh)
        )
    ew, eh = q.rnd(w, pct), q.rnd(h, pct)
    if (dst[2] - dst[0], dst[3] - dst[1]) != (ew, eh):
        out.append(
            "%s: screen rect %s is %dx%d, wanted %dx%d (%d%% of the dialog)"
            % (name, dst, dst[2] - dst[0], dst[3] - dst[1], ew, eh, pct)
        )
    if dst[0] > fsrc[0] or dst[1] > fsrc[1] or dst[2] < fsrc[2] or dst[3] < fsrc[3]:
        out.append("%s: the quad %s does not cover the 1x copy %s" % (name, dst, fsrc))
    if dst[0] < 0 or dst[1] < 0 or dst[2] > sw or dst[3] > sh:
        out.append("%s: the quad %s leaves the %dx%d screen" % (name, dst, sw, sh))
    cx, cy = (dst[0] + dst[2]) / 2.0, (dst[1] + dst[3]) / 2.0
    mx, my = (sw - q.rnd(160, pct)) / 2.0, (sh - q.rnd(32, pct)) / 2.0  # the visible map's centre
    if abs(cx - mx) > 40 or abs(cy - my) > 40:
        out.append(
            "%s: the quad is centred at (%d,%d), not on the visible map's centre (%d,%d)"
            % (name, cx, cy, mx, my)
        )
    return out


def problems(pct, screen, frame_size, run_dir, stages):
    out = []
    dl = dialogs_by_capture(run_dir)
    lc = layers_by_capture(run_dir)
    fw, fh = frame_size
    sw, sh = screen
    frames = []
    for spec in stages:
        # `name@x0,y0,x1,y1`: a LIVE rect (frame = map-quad pixels) where the sim keeps animating between the `capture` and the `pcapture`
        # present (a dialog that does not pause the sim, e.g. the building one: flames / units under it). Differences there are exempt;
        # everything else must still be equal.
        name, _, live_s = spec.partition("@")
        live = tuple(int(v) for v in live_s.split(",")) if live_s else None
        fpath = os.path.join(run_dir, "capture_res4_df_%s.bmp" % name)
        ppath = os.path.join(run_dir, "capture_res4_dp_%s.bmp" % name)
        miss = [os.path.basename(p) for p in (fpath, ppath) if not os.path.isfile(p)]
        if miss:
            out.append("%s: missing %s" % (name, ", ".join(miss)))
            continue
        got = dl.get("res4_dp_%s" % name, False)
        if not got:
            out.append(
                "%s: no dialog quad logged (%r): the dialog is not presented as a quad"
                % (name, got)
            )
            continue
        for k, (fsrc, dst) in enumerate(got):
            out += geometry_problems("%s[%d]" % (name, k), pct, screen, frame_size, fsrc, dst)
        w1, h1, frame = q.read_bmp(fpath)
        w2, h2, shown = q.read_bmp(ppath)
        if (w1, h1) != (fw, fh) or (w2, h2) != (sw, sh):
            out.append(
                "%s: frame %dx%d / presented %dx%d, wanted %dx%d / %dx%d"
                % (name, w1, h1, w2, h2, fw, fh, sw, sh)
            )
            continue
        frames.append((name, shown, got[0][1]))
        if lc.get("res4_dp_%s" % name) != len(got):
            out.append(
                "%s: %d dialog quad(s) but %r layer(s): a quad is still copied from the frame"
                % (name, len(got), lc.get("res4_dp_%s" % name))
            )
        want = q.expected(frame, fw, fh, sw, sh, pct)
        cur = q.cursor_rect(run_dir, "res4_dp_%s" % name)
        bad, first = 0, None
        # RES20 / RES23-S5: the floating notices (the "????" glyph-fallback message at load, an order refusal) are a presenter overlay scaled by the percentage from the
        # map's top left, behind the dialogs too (mode 3): their zone (x < 640 px, y < 150 px at 100%, times the percentage) is not part of the frame composition.
        nz_x, nz_y = 640 * pct // 100, 150 * pct // 100
        for y in range(sh):
            for x in range(sw):
                if shown[y][x] == want[y][x]:
                    continue
                if x < nz_x and y < nz_y:
                    continue
                if cur and cur[0] <= x < cur[2] and cur[1] <= y < cur[3]:
                    continue
                if any(d[0] <= x < d[2] and d[1] <= y < d[3] for _, d in got):
                    continue
                if live and live[0] <= x < live[2] and live[1] <= y < live[3]:
                    continue
                bad += 1
                first = first or (x, y, shown[y][x], want[y][x])
        if bad:
            out.append(
                "%s: %d presented pixels outside the dialogs differ from the HUD composition of the frame; first at %s"
                % (name, bad, first)
            )
        # not vacuous: the layer is drawn (differs from the bare background), and the map shows through (near the background)
        diff = near = tot = 0
        dst = got[-1][1]  # the topmost
        for y in range(max(dst[1], 0), min(dst[3], sh), 3):
            for x in range(max(dst[0], 0), min(dst[2], sw), 3):
                tot += 1
                a, b = shown[y][x], want[y][x]
                diff += a != b
                near += max(abs(a[i] - b[i]) for i in range(3)) <= 24
        if tot and diff * 2 < tot:
            out.append(
                "%s: the dialog region equals the bare background (%d of %d samples differ): no layer drawn"
                % (name, diff, tot)
            )
        if tot and near == 0:
            out.append(
                "%s: no sample in the dialog is near the background: the layer is opaque, not translucent"
                % name
            )
    if len(frames) >= 2:
        # the ESC menu and the Options screen share one sprite (one rect), so the proof that the click on the scaled Options button
        # opened the other screen is that the dialog's own pixels differ between the stages
        (n0, f0, r0), (n1, f1, r1) = frames[0], frames[1]
        tot = sum(1 for y in range(r0[1], r0[3]) for x in range(r0[0], r0[2]))
        diff = sum(
            1 for y in range(r0[1], r0[3]) for x in range(r0[0], r0[2]) if f0[y][x] != f1[y][x]
        )
        if tot and diff * 20 < tot:
            out.append(
                "stages %s and %s show the same dialog pixels (%d of %d differ): the click on the scaled button opened nothing"
                % (n0, n1, diff, tot)
            )
    out += layer_check_problems(run_dir, len(stages))
    return out


def selftest():
    fails = []
    fsrc, dst = (100, 100, 400, 300), (-5, 20, 445, 320)
    if not geometry_problems("t", 150, (1280, 800), (500, 400), fsrc, dst):
        fails.append("a quad leaving the screen / off-centre must be RED")
    ok = (350, 250, 800, 550)
    if geometry_problems(
        "t", 150, (1280, 800), (1216, 800), (378, 300, 678, 500), (295, 226, 745, 526)
    ):
        fails.append(
            "a good geometry must be green: %s"
            % geometry_problems(
                "t", 150, (1280, 800), (1216, 800), (378, 300, 678, 500), (295, 226, 745, 526)
            )
        )
    if not geometry_problems(
        "t", 150, (1280, 800), (1216, 800), (378, 300, 678, 500), (295, 226, 745, 516)
    ):
        fails.append("a wrong size must be RED")
    if not geometry_problems(
        "t", 150, (1280, 800), (1216, 800), (400, 300, 700, 500), (295, 226, 745, 526)
    ):
        fails.append("an off-centre frame rect (misread list) must be RED")
    del ok
    base = [[(0, 0, 0)] * 8 for _ in range(8)]
    frame = [[(x, y, 1) for x in range(8)] for y in range(8)]
    c = compose_dialog(base, frame, (2, 2, 4, 4), (0, 0, 4, 4))
    if c[0][0] != (2, 2, 1) or c[3][3] != (3, 3, 1) or c[4][4] != (0, 0, 0):
        fails.append("compose_dialog sampling")
    import tempfile

    with tempfile.TemporaryDirectory() as td:
        if not problems(150, (400, 200), (448, 192), td, ["a"]):
            fails.append("a run dir with no captures must be RED")
    with tempfile.TemporaryDirectory() as td:
        good = (
            "; RES4: dialog layer check slot 0 list 00654107 rect 333,247 390x306: max err r=1 g=2 b=1 (565 units), 5 of 119340 px above 1 -> FAIL"
            + chr(10)
        )
        bad = good.replace("g=2", "g=4")
        many = good.replace("5 of", "900 of")
        for text, want_red, what in (
            (good, False, "a within-tolerance layer check"),
            (bad, True, "max error 4"),
            (many, True, "900 px above 1"),
            ("", True, "no check line"),
        ):
            with open(os.path.join(td, "mh_net.log"), "w") as f:
                f.write(text)
            if bool(layer_check_problems(td)) != want_red:
                fails.append(
                    "layer_check_problems: %s should be %s" % (what, "RED" if want_red else "green")
                )
    for n in fails:
        print("SELFTEST FAIL:", n)
    print("check_res4_dialog selftest: %s" % ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    if len(argv) < 5:
        print(__doc__)
        return 2
    pct = int(argv[0])
    screen = tuple(int(v) for v in argv[1].split("x"))
    frame = tuple(int(v) for v in argv[2].split("x"))
    global LOOSE
    stages = [s for s in argv[3:-1] if s != "loose"]
    LOOSE = len(stages) != len(argv[3:-1])
    probs = problems(pct, screen, frame, argv[-1], stages)
    for p in probs:
        print("FAIL:", p)
    if not probs:
        print(
            "PASS: %d dialog stage(s) presented as scaled quads at %d%%, layers drawn, map shows through, layer check within tolerance"
            % (len(stages), pct)
        )
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
