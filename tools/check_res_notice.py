#!/usr/bin/env python3
"""check_res_notice.py -- mods:RES20: the floating in-game notices scale with Interface size, like the panel.

The rows res_notice_100 / res_notice_200 print a notice with the game's own routine (`notice <text>`: llm_ui_print_floating_msg_red) and capture the PRESENTED image
(`pcapture`) and the game frame (`capture`) before and after it. The notice lives in the frame's map area from (10,10) down; at ui_scale above 100 the DLL
diverts the stock draw into a scratch and the presenter shows it as an overlay scaled by the percentage on the map quad, anchored at the map's top left.

  1. PRESENTED: the pixels that differ between the before and after captures, inside the notice area (x < 900, y < 140), have a bounding box whose
     height is the 100% height (BOX100) times the percentage, and whose top-left is the 100% one times the percentage (+-3);
  2. FRAME: at a percentage above 100 the game frame holds NO notice (the before/after frames are identical in that area), at 100 it does (they differ).

  3. --stock (RES23-S5 kill switch, [video] ui_scale_notices=0): the notice is the game's OWN at every percentage -- drawn into the frame (the frame
     pair differs) and shown 1:1 (the presented box is the 100% one, not scaled);
  4. --modal (RES23-S5): the row prints six notices, captures the presented image (and the presenter's own output, `bcapture`), opens the ESC menu (game
     mode 3) and captures again. The detour takes mode 3, so the game's own 1x notice is NOT in the frame (`capture`) and the SCALED overlay shows behind the
     menu: the red text outside the dialog is the live one's, the dialog (overlay order notice < dialog layers < cursor) covers it where they overlap, the
     capture log lists the notice overlay both times;
  5. --stale (RES23-S5 stale-entry drop): a notice is printed, then the info screen (game mode 4) opens. The detour takes modes 2 and 3 only: in mode 4 the game's own
     1x text is drawn into the FRAME and the stored overlay entry is cleared, and it is stale anyway (it was stamped for the strategic frame's present). It must be
     DROPPED: `pcapture notice none` and no warm text at the 175% overlay's rows (below the 1x text's) in the CPU composition and the presenter's output; the live
     capture shows it beforehand. Red-proof: with the stamp check AND the clear disabled the log lists the notice over the info screen.

  check_res_notice.py [--stock] <pct> <run-dir>
  check_res_notice.py --modal <pct> <run-dir>
  check_res_notice.py --stale <pct> <run-dir>
  check_res_notice.py --selftest
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

AREA = (0, 0, 900, 140)  # x0, y0, x1, y1 of the notice area in the presented image
BOX100 = (
    10,
    26,
    10,
)  # x0, y0, height of the notice's changed-pixel box at 100% (measured on the rig: the first line of the queue)
PRE = "res_notice"


def diff_box(a, b, area):
    x0, y0, x1, y1 = area
    xs, ys = [], []
    for y in range(y0, min(y1, a[1], b[1])):
        ra, rb = a[2][y], b[2][y]
        for x in range(x0, min(x1, a[0], b[0])):
            if ra[x] != rb[x]:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return min(xs), min(ys), max(xs) + 1, max(ys) + 1, len(xs)


def problems(pct, run_dir, stock=False):
    try:
        import check_ui_quads as q

        vb = q.read_bmp(os.path.join(run_dir, "capture_%s_before.bmp" % PRE))
        va = q.read_bmp(os.path.join(run_dir, "capture_%s_after.bmp" % PRE))
        fb = q.read_bmp(os.path.join(run_dir, "capture_%s_frame_before.bmp" % PRE))
        fa = q.read_bmp(os.path.join(run_dir, "capture_%s_frame_after.bmp" % PRE))
    except (OSError, ImportError) as e:
        return ["cannot read the captures: %s" % e]
    out = []
    box = diff_box(vb, va, AREA)
    if not box:
        return [
            "the presented image did not change when the notice was printed: no notice on screen (vacuous)"
        ]
    x0, y0, x1, y1, n = box
    h = y1 - y0
    f = (
        1.0 if stock else pct / 100.0
    )  # the stock notice is the game's own: 1:1 on the map quad at every percentage
    want_h = BOX100[2] * f
    if abs(h - want_h) > 2 + want_h * 0.1:
        out.append(
            "the notice is %d px high at %d%%; one line is %.0f px (%d x %.0f%%)"
            % (h, pct, want_h, BOX100[2], f * 100)
        )
    wx, wy = BOX100[0] * f, BOX100[1] * f
    if abs(x0 - wx) > 3 or abs(y0 - wy) > 3:
        out.append(
            "the notice starts at %d,%d; wanted about %.0f,%.0f (%d,%d x %.0f%%)"
            % (x0, y0, wx, wy, BOX100[0], BOX100[1], f * 100)
        )
    fbox = diff_box(fb, fa, (0, 0, 900, 140))
    if pct > 100 and not stock and fbox:
        out.append(
            "the game frame holds the notice at %d%% (%d changed pixels): it must be an overlay, not part of the frame"
            % (pct, fbox[4])
        )
    if (pct <= 100 or stock) and not fbox:
        out.append(
            "at %d%% the game frame must hold the notice (the stock path), but it did not change"
            % pct
        )
    return out


SCALED_ROWS_Y = 44  # the 200% overlay's red line starts at 2 x BOX100's y (52); the game's own 1x red line ends by y 36
NOTICE_ZONE_RED = (
    0,
    SCALED_ROWS_Y,
    900,
    140,
)  # where a SCALED notice's red text lives in the presented image


def red_count(img, area=NOTICE_ZONE_RED):
    """Pixels of the notice red (the game's red floating message) inside `area` of a read_bmp image."""
    x0, y0, x1, y1 = area
    n = 0
    for y in range(y0, min(y1, img[1])):
        row = img[2][y]
        for x in range(x0, min(x1, img[0])):
            r, g, b = row[x]
            if r >= 180 and g <= 70 and b <= 70:
                n += 1
    return n


def notice_log_groups(run_dir):
    """mh_capture.log, one entry per pcapture, in order: (notice overlay listed?, [dialog screen rects (x0, y0, x1, y1)])."""
    path = os.path.join(run_dir, "mh_capture.log")
    if not os.path.isfile(path):
        return None
    out, dialogs = [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        f = line.split()
        if line.startswith("; pcapture dialog ") and "screen" in f:
            i = f.index("screen")
            dialogs.append(tuple(int(v) for v in f[i + 1 : i + 5]))
        elif line.startswith("; pcapture notice rect"):
            out.append((True, dialogs))
            dialogs = []
        elif line.startswith("; pcapture notice none"):
            out.append((False, dialogs))
            dialogs = []
    return out


MODAL_MIN_RED = 60  # a printed notice line is hundreds of red pixels at 100%, thousands at 200%
MODAL_ZONE = (0, SCALED_ROWS_Y, 900, 280)  # six scaled lines (200%): y 52 .. ~210


def red_set(img, area, warm=False):
    # warm: the game's red message blinks red / yellow with the game clock -- count both (r high, b low)
    x0, y0, x1, y1 = area
    out = set()
    for y in range(y0, min(y1, img[1])):
        row = img[2][y]
        for x in range(x0, min(x1, img[0])):
            r, g, b = row[x]
            if (r >= 180 and b <= 90) if warm else (r >= 180 and g <= 70 and b <= 70):
                out.add((x, y))
    return out


def _read(run_dir, names):
    import check_ui_quads as q

    imgs = {}
    for name in names:
        try:
            imgs[name] = q.read_bmp(os.path.join(run_dir, "capture_%s.bmp" % name))
        except (OSError, ImportError) as e:
            return None, "cannot read capture_%s.bmp: %s" % (name, e)
    return imgs, None


def modal_problems(pct, run_dir):
    out = []
    pre, mod, frm = "res_nm_notice", "res_nm_modal", "res_nm_frame"
    imgs, err = _read(run_dir, (pre, mod, pre + "__b", mod + "__b", frm))
    if err:
        return [err]
    groups = notice_log_groups(run_dir)
    if groups is None or len(groups) < 2:
        return ["mh_capture.log has no `pcapture notice` lines for both captures (%r)" % (groups,)]
    if not groups[0][0] or not groups[1][0]:
        out.append(
            "pcapture listed `notice none` for the live view or the menu view: %r"
            % ([g[0] for g in groups[:2]],)
        )
    dialogs = groups[1][1]
    if not dialogs:
        out.append("the menu capture lists no dialog quad: the menu is not up")
    m = 14  # the dialog quad's outer margin is transparent bevel shadow: judge the covered part inside it
    inside = lambda p: any(
        d[0] + m <= p[0] < d[2] - m and d[1] + m <= p[1] < d[3] - m for d in dialogs
    )
    for suffix, what in (
        ("", "the CPU composition (pcapture)"),
        ("__b", "the presenter's output (bcapture)"),
    ):
        live = red_set(imgs[pre + suffix], MODAL_ZONE)
        now = red_set(imgs[mod + suffix], MODAL_ZONE)
        under = {p for p in live if inside(p)}
        free = live - under
        if len(free) < MODAL_MIN_RED:
            out.append(
                "%s: only %d live red pixels outside the dialog: the row proves nothing (vacuous)"
                % (what, len(free))
            )
            continue
        if len(under) < 30:
            out.append(
                "%s: only %d live red pixels fall under the dialog: the cover assertion is vacuous (print more notices)"
                % (what, len(under))
            )
        kept = len(free & now)
        if kept < 0.9 * len(free):
            out.append(
                "%s: the scaled notice is missing behind the menu: %d of %d live red pixels outside the dialog remain"
                % (what, kept, len(free))
            )
        shown = len(under & now)
        if under and shown > 0.1 * len(under):
            out.append(
                "%s: the notice is drawn OVER the dialog: %d of %d red pixels under it still red"
                % (what, shown, len(under))
            )
    stock = red_count(imgs[frm], (0, 0, 600, 140))
    if stock:
        out.append(
            "the game frame holds %d red pixels of the stock 1x notice behind the menu: the detour did not take mode 3"
            % stock
        )
    return out


def stale_problems(pct, run_dir):
    out = []
    live, info = "res_ns_live", "res_ns_info"
    imgs, err = _read(run_dir, (live, info, live + "__b", info + "__b"))
    if err:
        return [err]
    groups = notice_log_groups(run_dir)
    if groups is None or len(groups) < 2:
        return ["mh_capture.log has no `pcapture notice` lines for both captures (%r)" % (groups,)]
    if not groups[0][0]:
        out.append(
            "pcapture of the live notice listed `notice none`: the row proves nothing (vacuous)"
        )
    if groups[1][0]:
        out.append(
            "pcapture over the info screen (mode 4) still lists a notice overlay: a stale entry (captured for an earlier present) was not dropped"
        )
    # The info screen (mode 4) still runs the game's notice routine: the game's own 1x text lands in the frame (rows 10..22 at the top left), so only rows
    # below that, where the 175% overlay's line lies (live: y 22..38), tell an overlay from the stock text. x < 420 keeps the info dialog's own warm pixels out.
    zone = (0, 24, 420, 60)
    for suffix, what in (("", "pcapture"), ("__b", "bcapture")):
        n_live = len(red_set(imgs[live + suffix], zone, warm=True))
        n_info = len(red_set(imgs[info + suffix], zone, warm=True))
        if n_live < MODAL_MIN_RED:
            out.append(
                "%s: the live capture shows only %d red pixels at the scaled position (vacuous)"
                % (what, n_live)
            )
        if n_info:
            out.append(
                "%s: %d red pixels at the scaled position over the info screen: a stale notice was drawn"
                % (what, n_info)
            )
    return out


def selftest():
    import tempfile

    def img(w, h, mark=None):
        px = [[(10, 10, 10)] * w for _ in range(h)]
        if mark:
            for y in range(mark[1], mark[3]):
                for x in range(mark[0], mark[2]):
                    px[y][x] = (250, 0, 0)
        return (w, h, px)

    fails = []
    red_img = img(1000, 200, (10, 10, 100, 24))
    if (
        red_count(red_img, (0, 0, 1000, 200)) != 90 * 14
        or red_count(red_img) != 0
        or red_count(img(1000, 200)) != 0
    ):
        fails.append("red_count")
    b = img(1000, 200)
    a = img(1000, 200, (10, 10, 200, 24))
    if diff_box(b, a, AREA)[:4] != (10, 10, 200, 24):
        fails.append("diff box")
    with tempfile.TemporaryDirectory() as td:
        if not problems(100, td):
            fails.append("an empty run dir must be RED")
    print("check_res_notice selftest: %s" % ("FAIL" if fails else "ok"))
    for n in fails:
        print("SELFTEST FAIL:", n)
    return 1 if fails else 0


def main(argv):
    if argv and argv[0] == "--selftest":
        return selftest()
    mode = ""
    if argv and argv[0] in ("--stock", "--modal", "--stale"):
        mode, argv = argv[0], argv[1:]
    if len(argv) != 2:
        print(__doc__)
        return 2
    if mode == "--modal":
        probs = modal_problems(int(argv[0]), argv[1])
        ok = "res_notice modal OK: the scaled notice shows behind the mode-3 menu, the dialog covers it, and no stock 1x notice is in the frame"
    elif mode == "--stale":
        probs = stale_problems(int(argv[0]), argv[1])
        ok = "res_notice stale OK: the live notice is dropped over the info screen (mode 4), in the capture log, the CPU composition and the presenter output"
    else:
        probs = problems(int(argv[0]), argv[1], stock=mode == "--stock")
        ok = (
            "res_notice stock OK: the notice is the game's own at %s%% (in the frame, 1:1; kill switch)"
            % argv[0]
            if mode == "--stock"
            else "res_notice OK: the notice is %s%% scaled (an overlay above 100, the frame's own at 100)"
            % argv[0]
        )
    for p in probs:
        print("RED: " + p)
    if not probs:
        print(ok)
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
