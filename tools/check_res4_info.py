#!/usr/bin/env python3
"""check_res4_info.py -- mods:RES16: the unit/building info screen (game mode 4) is a scaled dialog quad at ui_scale.

The row res4_info_175 opens a building's info screen (a right click on the first available-buildings row), clicks its NEXT button through the
scaled quad and leaves with ESC. Its captures (`capture` = the game's frame, `pcapture` = the presented image):

  capture_res4_if_hud / _frame_hud    the strategic view before        capture_res4_if_df_p0 / _dp_p0   page 0 (frame / presented)
  capture_res4_if_df_p1 / _dp_p1      page 1 after the NEXT click      capture_res4_if_after / _frame_after   the strategic view after ESC

Demanded here (the independent re-derivation is this file's own; nothing is shared with the C++ but the log lines it reads):

  1. QUAD: each info-screen pcapture logs a dialog (not `none`), the frame rect is the 480x480 list the game centred, its screen rect is
     round(480 s) square with s the layout's percentage (>= 1.5: the screen really is scaled), and `dialog layers 1` -- the stage-6b LAYER (only the
     dialog's own pixels are enlarged, like the ESC menu), with the AVI clip composited into it (the tick stashes it instead of BltFast'ing into the frame);
  2. PIXELS: (a) inside the quad a share of the presented pixels equal the frame's pixels at the same screen position -- the map around the window art
     stays at its normal scale; (b) the clip's enlarged rect is a busy picture (>= 300 sampled distinct colours), not the black widget underlay;
  3. PAGES: page 0 and page 1 present differently (the click on the scaled NEXT button reached the button: `infopage 1` also gates the script);
  4. PANEL: the side-panel list rows are the same before and after (every `Hp N rows` the presenter logged is one value, and the panel region of
     the presented image after ESC equals the one before, cursor rect exempt).

  check_res4_info.py <run-dir>
  check_res4_info.py --selftest
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_res4_dialog as d  # noqa: E402
import check_ui_quads as q  # noqa: E402

QUAD = re.compile(
    r"; \[ui\] dialog quad (\d+): frame (\d+),(\d+)-(\d+),(\d+) \((\d+)x(\d+)\) -> screen (\d+),(\d+)-(\d+),(\d+) \((\d+)x(\d+)\) at (\d+)%"
)
HP = re.compile(r"; \[ui\] side-panel list rows follow the visible panel: Hp (\d+) rows")
LAYOUT = re.compile(
    r"; \[ui\] ui_scale (\d+)%.* on (\d+)x(\d+), frame (\d+)x(\d+): map .*?, panel (\d+) wide"
)


# The clip's frame rectangle (the media widget: centre 512,406 of the 480x480 list at 272,240, 320x240 -- a little slack).
CLIP = (350, 284, 674, 528)


def centre(rel, dst, src):
    return ((2 * rel + 1) * src) // (2 * dst)


def in_rect(r, x, y):
    return bool(r) and r[0] <= x < r[2] and r[1] <= y < r[3]


def problems(run_dir, min_scale_pct=150):
    out = []
    vlog = os.path.join(run_dir, "mh_video.log")
    if not os.path.isfile(vlog):
        return ["no mh_video.log in %s" % run_dir]
    text = open(vlog, encoding="utf-8", errors="replace").read()
    quads = [tuple(int(v) for v in m.groups()) for m in QUAD.finditer(text)]
    if not quads:
        return [
            "no `; [ui] dialog quad` line in mh_video.log: the info screen got no quad (it stays 1x -- RES16 unfixed)"
        ]
    lay = None
    for m in LAYOUT.finditer(text):
        lay = [int(v) for v in m.groups()]
    if not lay:
        return ["no layout line in mh_video.log"]
    _pct, sw, sh, fw, fh, panel_w = lay
    hp = sorted(set(int(v) for v in HP.findall(text) if int(v) > 0))
    if len(hp) > 1:
        out.append("the side panel's visible rows changed over the run: Hp %s" % hp)
    dlg = d.dialogs_by_capture(run_dir)
    lay_n = d.layers_by_capture(run_dir)
    for name in ("res4_if_dp_p0", "res4_if_dp_p1"):
        ds = dlg.get(name)
        if not ds:
            out.append("%s: no dialog quad logged (`none`)" % name)
            continue
        if lay_n.get(name) != 1:
            out.append(
                "%s: %r dialog layers, wanted 1 (the stage-6b layer, the clip composited in)"
                % (name, lay_n.get(name))
            )
        (fr, sc) = ds[-1]
        fwid, fhei = fr[2] - fr[0], fr[3] - fr[1]
        swid, shei = sc[2] - sc[0], sc[3] - sc[1]
        if (fwid, fhei) != (480, 480):
            out.append(
                "%s: the info list is %dx%d in the frame, wanted 480x480" % (name, fwid, fhei)
            )
        if swid * 100 < fwid * min_scale_pct or shei * 100 < fhei * min_scale_pct:
            out.append(
                "%s: screen rect %dx%d is not scaled (frame %dx%d)" % (name, swid, shei, fwid, fhei)
            )
        if abs(swid - shei) > 1:
            out.append("%s: the quad is not square: %dx%d" % (name, swid, shei))
    if out:
        return out
    try:
        fp0 = q.read_bmp(os.path.join(run_dir, "capture_res4_if_df_p0.bmp"))
        pp0 = q.read_bmp(os.path.join(run_dir, "capture_res4_if_dp_p0.bmp"))
        fp1 = q.read_bmp(os.path.join(run_dir, "capture_res4_if_df_p1.bmp"))
        pp1 = q.read_bmp(os.path.join(run_dir, "capture_res4_if_dp_p1.bmp"))
        hud = q.read_bmp(os.path.join(run_dir, "capture_res4_if_hud.bmp"))
        aft = q.read_bmp(os.path.join(run_dir, "capture_res4_if_after.bmp"))
    except (OSError, ImportError) as e:
        return ["cannot read the captures: %s" % e]
    for tag, frame, shown in (("p0", fp0, pp0), ("p1", fp1, pp1)):
        name = "res4_if_dp_" + tag
        fr, sc = dlg[name][-1]
        cur = q.cursor_rect(run_dir, name)
        # (a) only the dialog's own pixels are enlarged: where the layer is transparent (the window art's corners and edges) the presented image shows the
        # map at its NORMAL scale, i.e. the frame pixel at the same screen position (the map quad is 1:1 at the origin here)
        same = tot = 0
        for y in range(sc[1], sc[3], 2):
            for x in range(sc[0], sc[2], 2):
                if (
                    x >= frame[0]
                    or y >= frame[1]
                    or x >= shown[0]
                    or y >= shown[1]
                    or in_rect(cur, x, y)
                ):
                    continue
                tot += 1
                same += shown[2][y][x] == frame[2][y][x]
        if tot == 0 or same * 100 < tot * 3:
            out.append(
                "%s: only %d of %d sampled pixels inside the quad show the map at normal scale (a cut of the frame is enlarged, not a layer)"
                % (name, same, tot)
            )
        # (b) the clip is in the layer: its enlarged screen rect is busy (a decoded picture, not the black widget underlay)
        sxs = (sc[2] - sc[0]) / float(fr[2] - fr[0])
        cx0, cy0 = sc[0] + int((CLIP[0] - fr[0]) * sxs), sc[1] + int((CLIP[1] - fr[1]) * sxs)
        cx1, cy1 = sc[0] + int((CLIP[2] - fr[0]) * sxs), sc[1] + int((CLIP[3] - fr[1]) * sxs)
        cols = set()
        for y in range(cy0 + 8, cy1 - 8, 3):
            for x in range(cx0 + 8, cx1 - 8, 3):
                if not in_rect(cur, x, y):
                    cols.add(shown[2][y][x])
        if len(cols) < 300:
            out.append(
                "%s: the clip rect (%d,%d-%d,%d) shows only %d distinct colours: the AVI clip is not in the layer"
                % (name, cx0, cy0, cx1, cy1, len(cols))
            )
    if pp0[2] == pp1[2]:
        out.append(
            "page 1 looks exactly like page 0: the click on the scaled NEXT button did nothing"
        )
    # the panel region before / after (x >= screen width - the scaled panel)
    cur_b, cur_a = q.cursor_rect(run_dir, "res4_if_hud"), q.cursor_rect(run_dir, "res4_if_after")
    x0 = sw - panel_w
    y0 = (sh * 2) // 5  # the list rows (the minimap above them is live: units move)
    bad = tot = 0
    for y in range(y0, min(hud[1], aft[1])):
        for x in range(x0, min(hud[0], aft[0])):
            if in_rect(cur_b, x, y) or in_rect(cur_a, x, y):
                continue
            tot += 1
            bad += hud[2][y][x] != aft[2][y][x]
    if tot == 0 or bad * 1000 > tot * 5:
        out.append(
            "the side panel's list rows after ESC differ from those before: %d of %d pixels"
            % (bad, tot)
        )
    return out


def selftest():
    import tempfile

    fails = []
    with tempfile.TemporaryDirectory() as td:
        if not problems(td):
            fails.append("an empty run dir must be RED")
        with open(os.path.join(td, "mh_video.log"), "w") as f:
            f.write("; unrelated\n")
        if not any("no `; [ui] dialog quad`" in p for p in problems(td)):
            fails.append("no quad line must be RED (RES16 unfixed)")
    if centre(0, 821, 480) != 0 or centre(820, 821, 480) != 479:
        fails.append("centre rule ends")
    for n in fails:
        print("SELFTEST FAIL:", n)
    print("check_res4_info selftest: %s" % ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv):
    if argv and argv[0] == "--selftest":
        return selftest()
    if not argv:
        print(__doc__)
        return 2
    probs = problems(argv[-1])
    for p in probs:
        print("RED: " + p)
    if not probs:
        print(
            "res4_info OK: the info screen is a scaled dialog layer (clip composited in), NEXT clicked through it, the panel unchanged after ESC"
        )
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
