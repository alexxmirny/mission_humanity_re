#!/usr/bin/env python3
"""check_panel_filler.py -- mods:RES22: the side panel of the game FRAME below the last list row is not stray pink.

The approved rule (mods:RES4, user review 2026-10-19): WHOLE empty list slots show the teal empty-slot art, only the partial slot smaller than one row under the
last whole row shows the pink panel background. The lists size themselves to the rows the screen shows (Hp), so a frame taller than that (a scaled layout, an
explicit frame) once kept the bare pink panel for every slot below the visible end. `seams/video_mode_guards.cpp panel_fill_plan` draws the slot icons to the
last whole slot of the window. This reads mh_video.log (`side-panel list rows follow the visible panel: Hp N rows (WindowHeight W)`, the LAST such line) and the
game-frame capture, and probes the panel's middle column (frame width - 80):

  1. every whole empty slot from the end of the list's whole rows (slot tops at 248 + k * 52) to the last whole slot of the window, EXCEPT the slot the screen's
     visible edge cuts, is TEAL (blue and green above red) for at least 85% of its probed rows;
  2. the slot the visible edge cuts (the leftover under the last whole visible row) and the partial slot under the last whole one are NOT teal (the pink panel
     background) for at least 85% of their probed rows. User rule 2026-10-23: "teal only where a whole row fits; the leftover under the last whole visible row is pink".

  check_panel_filler.py <capture-name> <run-dir>      e.g. res4_frame  (reads capture_<name>.bmp)
  check_panel_filler.py --selftest
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

TOP, ROW = 248, 52
HP = re.compile(
    r"side-panel list rows follow the visible panel: Hp (\d+) rows \(WindowHeight (\d+)\)"
)


def teal(p):
    r, g, b = p
    return g >= r + 16 and b >= r + 16


def regions(hp, wh):
    """([(y0, y1)] whole empty slots from the end of the list's whole rows to the last whole slot of the frame, minus the cut one; (y0, y1) or None: the partial
    slot under the last whole one; (y0, y1) or None: the slot the visible edge cuts)."""
    cap_end = TOP + ((hp - TOP) // ROW) * ROW  # where the list's whole rows stop
    last = TOP + ((wh - TOP) // ROW) * ROW  # the last whole slot of the window
    cut = None
    if hp < wh and cap_end < hp:
        cut = (cap_end, cap_end + ROW)  # the slot the visible edge cuts
    slots = [(y, y + ROW) for y in range(cap_end, last, ROW) if not cut or y != cut[0]]
    gap = (last, wh) if wh - last > 8 else None
    return slots, gap, cut


def problems_for(frame, hp, wh):
    fw, fh, px = frame
    out = []
    x = fw - 80
    slots, gap, cut = regions(hp, wh)
    if hp >= wh:
        return out, "Hp = the window height: no hidden rows"
    if not slots and not gap and not cut:
        return ["nothing to probe (Hp %d of %d): the check would be vacuous" % (hp, wh)], ""
    for y0, y1 in slots:
        ys = range(y0 + 8, min(y1 - 8, fh))
        n = sum(1 for y in ys if teal(px[y][x]))
        if len(ys) and n * 100 < 85 * len(ys):
            out.append(
                "the whole empty slot at frame rows %d..%d is not teal (%d of %d probed rows): stray pink fill"
                % (y0, y1, n, len(ys))
            )
    for what, r in (("partial slot", gap), ("slot the visible edge cuts", cut)):
        if r:
            ys = range(r[0] + 4, min(r[1] - 4, fh))
            n = sum(1 for y in ys if teal(px[y][x]))
            if len(ys) and n * 100 > 15 * len(ys):
                out.append(
                    "the %s at frame rows %d..%d is teal (%d of %d probed rows): it must be the pink panel background"
                    % (what, r[0], r[1], n, len(ys))
                )
    return out, "%d whole empty slot(s) teal, cut slot %s and gap %s pink" % (len(slots), cut, gap)


def problems(name, run_dir):
    import check_ui_quads as q

    vlog = os.path.join(run_dir, "mh_video.log")
    if not os.path.isfile(vlog):
        return ["no mh_video.log in %s" % run_dir], ""
    m = None
    for ln in open(vlog, encoding="utf-8", errors="replace").read().splitlines():
        m = HP.search(ln) or m
    if not m:
        return ["no `side-panel list rows follow the visible panel: Hp` line"], ""
    hp, wh = int(m.group(1)), int(m.group(2))
    try:
        frame = q.read_bmp(os.path.join(run_dir, "capture_%s.bmp" % name))
    except OSError as e:
        return ["cannot read the frame capture: %s" % e], ""
    if frame[1] != wh:
        return ["the capture is %d rows high, the log says WindowHeight %d" % (frame[1], wh)], ""
    return problems_for(frame, hp, wh)


def selftest():
    fails = []
    fw, fh = 1216, 800
    pink, tl = (57, 40, 33), (24, 60, 66)

    def build(slot_color, gap_color, cut_color=pink):
        px = [[pink] * fw for _ in range(fh)]
        slots, gap, cut = regions(533, fh)
        for y0, y1 in slots:
            for y in range(y0, y1):
                px[y][fw - 80] = slot_color
        if gap:
            for y in range(gap[0], gap[1]):
                px[y][fw - 80] = gap_color
        if cut:
            for y in range(cut[0], cut[1]):
                px[y][fw - 80] = cut_color
        return (fw, fh, px)

    ok, _ = problems_for(build(tl, pink), 533, fh)
    if ok:
        fails.append("good frame must be clean: %r" % ok)
    bad, _ = problems_for(build(pink, pink), 533, fh)
    if not bad:
        fails.append("pink whole empty slots must be RED")
    bad2, _ = problems_for(build(tl, tl), 533, fh)
    if not bad2:
        fails.append("a teal partial gap must be RED")
    bad3, _ = problems_for(build(tl, pink, tl), 533, fh)
    if not bad3:
        fails.append("a teal cut slot must be RED")
    r = regions(533, 800)
    if r[0][0] != (560, 612) or r[1] != (768, 800) or r[2] != (508, 560):
        fails.append("regions arithmetic %r" % (regions(533, 800),))
    print("check_panel_filler selftest: %s" % ("FAIL" if fails else "ok"))
    for f in fails:
        print("SELFTEST FAIL:", f)
    return 1 if fails else 0


def main(argv):
    if argv and argv[0] == "--selftest":
        return selftest()
    if len(argv) != 2:
        print(__doc__)
        return 2
    probs, note = problems(argv[0], argv[1])
    for p in probs:
        print("RED: " + p)
    if not probs:
        print("panel filler OK: %s" % note)
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
