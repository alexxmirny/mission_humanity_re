#!/usr/bin/env python3
"""check_panel_rows.py -- RES7: the side-panel lists past the 768-px rows, read from the frames and mh_video.log;
RES8: the worker +/- buttons' first click, read from the counter's digits and mh_net.log.

The rows run in the res_walk scenario on two committed saves, live games: units move and fight and the status bar's
resource numbers tick, so a whole-frame pixel baseline is not a stable oracle (the walk masks the volatile areas).
What must hold is read from the captured frames themselves and from the PROCESS run directory's mh_video.log
(test_ui hands that directory over; it holds capture_<old row>__<name>.bmp -- each mode reads its own prefix):

  worker_1024  1024x768, stock settings, 2-almost.sav. mh_net.log: `; RES8: ... amounts primed 2/2 ... press splice
        armed`. a_owned / b_plus / c_minus: the owned tab's row 2 (Extractor; 52-px rows from y 248) worker counter
        reads N, then N + 1 after a single "+" click, then N again after a single "-" click -- the first click of
        each button in the process, which stock orders as 0 workers.
  owned_960    1280x960, the fix on, 2-almost.sav. mh_video.log: `panel rows armed` and `tables built for 1440 px`
        (the picker's tallest mode, not the start mode); mh_net.log: the RES8 arm line. a_avail_8: the available tab
        holds 8 entries (8 rows filled, 5 empty). d_owned: the owned tab holds 13 buildings, rows 0..12 all filled
        (rows 10..12 are below 768). e_worker_minus: row 12's worker counter (y 872..924, a row the stock tables have
        no slot for) reads ONE less than in d_owned after a single "-" click, and f_worker_plus: one more again.
  ayy30_960   1280x960 on ayy30.sav (21 available buildings, 13 rows shown). a_list: rows 0..12 filled. b_arrow: the
        down arrow scrolled one row (b's row 0 is a_list's row 1). c_placing_arrow: a click on row 12 (y 872..924)
        after that scroll started a placement. c_info: a right click on row 12 opened its info screen (the middle of
        the frame is not c_placing_arrow's map). d_drag: the right-button drag on the list body scrolled further
        (d's row 0 is a_list's row k, k >= 2). e_placing_drag: a click on row 12 after the drag started a placement
        of a DIFFERENT building (its status line differs from c's).
  ayy30_1440  1920x1440 on ayy30.sav. b_group51: the selected control group's column (16-px rows from y 288, x
        1846..1916) lists more than 45 entries (the group holds 51) and the ungrouped column none. c_hover45: the
        cursor on row 45 changed the status line. d_click45: the click on row 45 moved that unit out of the group
        (the group column one row shorter, the ungrouped column one row).

  The counter is READ, not compared: its digits are matched against the game's 4x7 panel glyphs (DIGITS below; drawn
  yellow, bright green at the building's maximum, red at 0). An unknown glyph is RED, not a guess.

  check_panel_rows.py <worker_1024|owned_960|ayy30_960|ayy30_1440> <run-dir>
  check_panel_rows.py --selftest    synthetic frames plant every negative

Absence of a frame or a log line is RED.
"""

import os
import re
import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None

PANEL_W = 160
GRID_MIN = 0.01  # fraction of a row's sampled pixels in the widget grid's green (an empty row: 0)
LABEL_MIN = 30  # bright-green status-line text pixels


# The building panel's worker-counter glyphs, 4 px wide x 7 rows, rows joined by "/" ("#" = ink). Measured from
# the 3-kariotyp-2 and 2-almost frames (2026-10-11); a digit not listed here reads as None and the check goes RED.
DIGITS = {
    ".##./#..#/#..#/#..#/#..#/#..#/.##.": 0,
    ".##./#..#/...#/..#./...#/#..#/.##.": 3,
    "#.../#.../#.../#.#./####/..#./..#.": 4,
    "####/#.../###./...#/...#/#..#/.##.": 5,
    ".##./#..#/###./#..#/#..#/#..#/.##.": 6,
    "####/...#/..#./..#./.#../.#../.#..": 7,
    ".##./#..#/#..#/.##./#..#/#..#/.##.": 8,
    ".##./#..#/#..#/.###/...#/#..#/.##.": 9,
}
COUNTER_INK = ((255, 255, 0), (16, 255, 16), (255, 48, 49))  # yellow, at-maximum green, zero red


def read_counter(img, x0, top):
    """The worker counter of the panel row whose top edge is `top` (panel left edge x0), or None if unreadable."""
    px = img.load()
    bx0, by0, bx1, by1 = x0, top, x0 + 40, top + 16
    if by1 > img.size[1]:
        return None

    def ink(x, y):
        return px[x, y][:3] in COUNTER_INK

    cols = [x for x in range(bx0, bx1) if any(ink(x, y) for y in range(by0, by1))]
    rows = [y for y in range(by0, by1) if any(ink(x, y) for x in range(bx0, bx1))]
    if not cols:
        return None
    groups, cur = [], [cols[0]]
    for c in cols[1:]:
        if c == cur[-1] + 1:
            cur.append(c)
        else:
            groups.append(cur)
            cur = [c]
    groups.append(cur)
    value = 0
    for g in groups:
        key = "/".join(
            "".join("#" if ink(x, y) else "." for x in range(g[0], g[-1] + 1))
            for y in range(rows[0], rows[-1] + 1)
        )
        if key not in DIGITS:
            return None
        value = value * 10 + DIGITS[key]
    return value


def draw_counter(img, x0, top, value, color=(255, 255, 0)):
    """Selftest: paint `value` in the panel glyphs where read_counter looks."""
    glyph = {d: k for k, d in DIGITS.items()}
    px = img.load()
    for i, ch in enumerate(str(value)):
        for dy, row in enumerate(glyph[int(ch)].split("/")):
            for dx, c in enumerate(row):
                if c == "#":
                    px[x0 + 14 + 5 * i + dx, top + 4 + dy] = color


def counter_steps(frames, x0, top, steps, out):
    """frames: [(name, img)], steps: the expected change into each frame after the first. Appends REDs to out."""
    vals = []
    for name, img in frames:
        v = read_counter(img, x0, top) if img is not None else None
        if img is not None and v is None:
            out.append("%s: the worker counter (y %d) is unreadable" % (name, top))
        vals.append(v)
    for i, step in enumerate(steps, start=1):
        a, b = vals[i - 1], vals[i]
        if a is not None and b is not None and b - a != step:
            out.append(
                "%s: worker counter %d -> %d, want %+d (one click, one worker)"
                % (frames[i][0], a, b, step)
            )
    return vals


# Mode 1 (the default) logs "first press primed", mode 2 "one worker per press" (mods:RES8 / RES11); both prime
# the first press, which is what this row proves.
RES8_ARM = "; RES8: worker +/- <first press primed|one worker per press>: amounts primed 2/2"
RES8_ARM_RE = re.compile(
    r"; RES8: worker \+/- (?:first press primed|one worker per press): amounts primed 2/2"
)


def net_log_problems(run_dir):
    p = os.path.join(run_dir, "mh_net.log")
    if not os.path.isfile(p):
        return ["no mh_net.log in %s" % run_dir]
    with open(p, encoding="utf-8", errors="replace") as f:
        text = f.read()
    for line in text.splitlines():
        if RES8_ARM_RE.search(line) and "press splice armed" in line:
            return []
    return ["mh_net.log has no `%s ... press splice armed` line" % RES8_ARM]


# The walk (res_walk.txt) names every capture <old row>__<name>; the prefix is the mode's own.
MODE_PREFIX = {
    "worker_1024": "res_worker_click_1024__",
    "owned_960": "res_panel_rows_960__",
    "ayy30_960": "res_panel_rows_ayy30_960__",
    "ayy30_1440": "res_panel_rows_ayy30_1440__",
}
PREFIX = ""


def find(run_dir, name):
    for ext in (".png", ".bmp"):
        p = os.path.join(run_dir, "capture_%s%s%s" % (PREFIX, name, ext))
        if os.path.isfile(p):
            return p
    return None


def is_grid(p):
    r, g, b = p[:3]
    return g > 90 and r < 90 and b < 90 and g > r + 30


def row_fill(img, top, rowh, n, x0, x1):
    """Grid-green fraction of each of the first n rows (inner band, every other pixel)."""
    px = img.load()
    out = []
    for r in range(n):
        y0 = top + rowh * r
        if y0 + rowh > img.size[1]:
            break
        g = t = 0
        for y in range(y0 + 4, y0 + rowh - 4, 2):
            for x in range(x0, x1, 2):
                t += 1
                if is_grid(px[x, y]):
                    g += 1
        out.append(g / t if t else 0.0)
    return out


def label_pixels(img, box):
    px = img.load()
    x0, y0, x1, y1 = box
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = px[x, y][:3]
            if g > 150 and r < 120:
                n += 1
    return n


def load(run_dir, name, size, out):
    p = find(run_dir, name)
    if p is None:
        out.append("no capture_%s%s in %s" % (PREFIX, name, run_dir))
        return None
    img = Image.open(p).convert("RGB")
    if img.size != size:
        out.append(
            "capture %s is %dx%d, not %dx%d" % (name, img.size[0], img.size[1], size[0], size[1])
        )
        return None
    return img


def log_problems(run_dir, mode):
    if mode == "worker_1024":
        return net_log_problems(run_dir)
    if mode == "owned_960":
        return net_log_problems(run_dir) + video_log_problems(run_dir, mode)
    return video_log_problems(run_dir, mode)


def video_log_problems(run_dir, mode):
    p = os.path.join(run_dir, "mh_video.log")
    if not os.path.isfile(p):
        return ["no mh_video.log in %s" % run_dir]
    with open(p, encoding="utf-8", errors="replace") as f:
        text = f.read()
    out = []
    if "; [video] panel rows armed (" not in text:
        out.append("mh_video.log has no `panel rows armed` line")
    if "panel rows: tables built for 1440 px" not in text:
        out.append("mh_video.log has no `panel rows: tables built for 1440 px` line")
    return out


def problems(mode, run_dir):
    global PREFIX
    if Image is None:
        return ["PIL is not installed"]
    if mode not in MODE_PREFIX:
        return ["unknown mode %r" % mode]
    PREFIX = MODE_PREFIX[mode]
    out = log_problems(run_dir, mode)
    if mode == "owned_960":
        size, x0 = (1280, 960), 1280 - PANEL_W
        # a_avail_8: 2-almost's available tab holds 8 entries -- 8 rows filled, the other 5 of the 13 empty
        a = load(run_dir, "a_avail_8", size, out)
        if a is not None:
            fill = row_fill(a, 248, 52, 13, x0 + 80, x0 + 158)
            filled = [v >= GRID_MIN for v in fill]
            if filled != [True] * 8 + [False] * 5:
                out.append(
                    "a_avail_8: rows filled %s, want the first 8 only (%s)" % (filled, fmt(fill))
                )
        # d_owned: the owned tab holds 13 buildings -- rows 0..12 all drawn, rows 10..12 below the stock 768 rows
        d = load(run_dir, "d_owned", size, out)
        if d is not None:
            fill = row_fill(d, 248, 52, 13, x0 + 80, x0 + 158)
            if len(fill) != 13 or sum(1 for v in fill if v >= GRID_MIN) != 13:
                out.append("d_owned: not all 13 owned rows are filled (%s)" % fmt(fill))
        # row 12 (y 872..924, a row the stock tables have no slot for): its worker "-" then "+" move the counter by one
        e = load(run_dir, "e_worker_minus", size, out)
        f = load(run_dir, "f_worker_plus", size, out)
        counter_steps(
            [("d_owned", d), ("e_worker_minus", e), ("f_worker_plus", f)],
            x0,
            248 + 52 * 12,
            [-1, +1],
            out,
        )
    elif mode == "worker_1024":
        size, x0 = (1024, 768), 1024 - PANEL_W
        frames = [(n, load(run_dir, n, size, out)) for n in ("a_owned", "b_plus", "c_minus")]
        counter_steps(
            frames, x0, 248 + 52 * 2, [+1, -1], out
        )  # row 2's counter (Extractor, 39 of 80)
    elif mode == "ayy30_960":
        size, x0 = (1280, 960), 1280 - PANEL_W
        status = (150, size[1] - 39, 600, size[1] - 20)
        a = load(run_dir, "a_list", size, out)
        b = load(run_dir, "b_arrow", size, out)
        c = load(run_dir, "c_placing_arrow", size, out)
        d = load(run_dir, "d_drag", size, out)
        e = load(run_dir, "e_placing_drag", size, out)
        if a is not None:
            fill = row_fill(a, 248, 52, 13, x0 + 80, x0 + 158)
            if sum(1 for v in fill if v >= GRID_MIN) != 13:
                out.append("a_list: not all 13 rows filled (%s)" % fmt(fill))
            if b is not None and best_row(a, b) != 1:
                out.append(
                    "b_arrow: row 0 is a_list's row %s, want 1 (one-row arrow scroll)"
                    % best_row(a, b)
                )
            if d is not None and (best_row(a, d) or 0) < 2:
                out.append(
                    "d_drag: row 0 is a_list's row %s, want >= 2 (the drag did not scroll)"
                    % best_row(a, d)
                )
        for n, img in (("c_placing_arrow", c), ("e_placing_drag", e)):
            if img is not None and label_pixels(img, status) < 200:
                out.append("%s: no placement status line -- the row-12 click started nothing" % n)
        info = load(run_dir, "c_info", size, out)
        if info is not None and c is not None:
            mid = (400, 200, 900, 700)  # the info screen's window covers the middle of the map
            from PIL import ImageChops

            h = ImageChops.difference(info.crop(mid), c.crop(mid)).convert("L").histogram()
            if sum(h[40:]) / max(1, sum(h)) < 0.3:
                out.append(
                    "c_info: the middle of the frame is still the map -- the right click opened no info screen"
                )
        if c is not None and e is not None and crop_bytes(c, status) == crop_bytes(e, status):
            out.append(
                "e_placing_drag: the same building as c_placing_arrow -- row 12 did not follow the scroll"
            )
    elif mode == "ayy30_1440":
        size = (1920, 1440)
        status = (300, size[1] - 35, 700, size[1] - 20)
        b = load(run_dir, "b_group51", size, out)
        c = load(run_dir, "c_hover45", size, out)
        d = load(run_dir, "d_click45", size, out)
        if b is not None:
            lb, rb = sel_rows(b, 1764, 1836), sel_rows(b, 1846, 1916)
            if rb <= 45 or lb != 0:
                out.append(
                    "b_group51: group column %d rows, ungrouped %d (want > 45 and 0)" % (rb, lb)
                )
            if c is not None and crop_bytes(b, status) == crop_bytes(c, status):
                out.append("c_hover45: the status line did not change with the cursor on row 45")
            if d is not None:
                ld, rd = sel_rows(d, 1764, 1836), sel_rows(d, 1846, 1916)
                if (ld, rd) != (lb + 1, rb - 1):
                    out.append(
                        "d_click45: group %d -> %d, ungrouped %d -> %d (want one unit moved)"
                        % (rb, rd, lb, ld)
                    )
    else:
        out.append("unknown mode %r" % mode)
    return out


def best_row(a, b):
    """The a_list row whose picture b's row 0 shows (mean abs difference < 2), or None."""
    from PIL import ImageChops

    r0 = b.crop((1150, 252, 1278, 296))
    for k in range(13):
        rk = a.crop((1150, 252 + 52 * k, 1278, 296 + 52 * k))
        h = ImageChops.difference(r0, rk).convert("L").histogram()
        if sum(i * n for i, n in enumerate(h)) / (r0.size[0] * r0.size[1]) < 2:
            return k
    return None


def sel_rows(img, x0, x1):
    """Filled 16-px selection rows from y 288 in one column (an empty row shows the blue list body)."""
    px = img.load()
    n = 0
    for r in range((img.size[1] - 288) // 16):
        y0 = 288 + 16 * r
        blue = t = 0
        for y in range(y0 + 3, y0 + 13):
            for x in range(x0, x1, 2):
                rr, g, bb = px[x, y][:3]
                t += 1
                if bb > rr + 15 and bb >= g:
                    blue += 1
        if blue / t < 0.3:
            n += 1
    return n


def crop_bytes(img, box):
    return img.crop(box).tobytes()


def fmt(fill):
    return " ".join("%.2f" % v for v in fill)


def selftest():
    import tempfile

    GREEN, BODY, TEXT = (10, 120, 20), (30, 80, 90), (40, 200, 40)

    def frame(size, top, rowh, rows, label=None, counter=None):
        img = Image.new("RGB", size, (60, 40, 20))
        px = img.load()
        w, h = size
        for y in range(top, h):
            for x in range(w - PANEL_W, w):
                px[x, y] = BODY
        for r in range(rows):
            for y in range(top + rowh * r + 2, top + rowh * (r + 1) - 2):
                for x in range(w - PANEL_W + 40, w):
                    if (x + y) % 3 == 0:
                        px[x, y] = GREEN
        if label:
            x0, y0, x1, y1 = label
            for y in range(y0, y1):
                for x in range(x0, x0 + 40):
                    px[x, y] = TEXT
        if counter is not None:
            draw_counter(img, w - PANEL_W, 872, counter, (255, 255, 0))
        return img

    def worker_frame(value):
        img = frame(S10, 248, 52, 10)
        if value is None:  # a glyph the table does not know (a 3x3 block)
            for dy in range(3):
                for dx in range(3):
                    img.putpixel((S10[0] - PANEL_W + 14 + dx, 248 + 52 * 2 + 4 + dy), (255, 255, 0))
        else:
            draw_counter(img, S10[0] - PANEL_W, 248 + 52 * 2, value)
        return img

    def logtext(mode):
        return "; [video] panel rows armed (162 sites): ...\n; [video] panel rows: tables built for 1440 px: ...\n"

    NETLOG = (
        "[17:05:50.362] ; RES8: worker +/- one worker per press: amounts primed 2/2 at 0050BDE7/0050BDFB, "
        "press splice armed at 0041341B\n"
    )

    S9, S14, S10 = (1280, 960), (1920, 1440), (1024, 768)

    def rows_frame(size, offset, n, label_w=0):
        img = Image.new("RGB", size, (60, 40, 20))
        px = img.load()
        w = size[0]
        for r in range(n):
            k = r + offset
            for y in range(248 + 52 * r + 2, 248 + 52 * (r + 1) - 2):
                for x in range(w - PANEL_W + 40, w):
                    px[x, y] = (
                        GREEN
                        if (x + (y - 248) % 52) % 3 == 0
                        else (k * 41 % 256, 255 - k * 17 % 256, 30)
                    )
        for y in range(S9[1] - 39, S9[1] - 20):
            for x in range(150, 150 + label_w):
                px[x, y] = TEXT
        return img

    def sel_frame(left, right, label=0):
        img = Image.new("RGB", S14, (60, 40, 20))
        px = img.load()
        for col, cnt in ((1764, left), (1846, right)):
            for r in range((S14[1] - 288) // 16):
                c = (10, 10, 10) if r < cnt else (30, 80, 90)
                for y in range(288 + 16 * r, 288 + 16 * r + 16):
                    for x in range(col, col + 72):
                        px[x, y] = c
        for y in range(S14[1] - 35, S14[1] - 20):
            for x in range(300, 300 + label):
                px[x, y] = TEXT
        return img

    L9 = (150, S9[1] - 39, 600, S9[1] - 20)
    L14 = (255, S14[1] - 33, 330, S14[1] - 19)

    def info_frame():
        img = rows_frame(S9, 1, 13, 300)
        for y in range(200, 700):
            for x in range(400, 900):
                img.putpixel((x, y), ((x * 7) % 256, (y * 5) % 256, 200))
        return img

    good = {
        "owned_960": {
            "a_avail_8": frame(S9, 248, 52, 8),
            "d_owned": frame(S9, 248, 52, 13, counter=56),
            "e_worker_minus": frame(S9, 248, 52, 13, counter=55),
            "f_worker_plus": frame(S9, 248, 52, 13, counter=56),
        },
        "worker_1024": {
            "a_owned": worker_frame(57),
            "b_plus": worker_frame(58),
            "c_minus": worker_frame(57),
        },
        "ayy30_960": {
            "a_list": rows_frame(S9, 0, 13),
            "b_arrow": rows_frame(S9, 1, 13),
            "c_placing_arrow": rows_frame(S9, 1, 13, 300),
            "c_info": info_frame(),
            "d_drag": rows_frame(S9, 5, 13),
            "e_placing_drag": rows_frame(S9, 5, 13, 250),
        },
        "ayy30_1440": {
            "b_group51": sel_frame(0, 51, 30),
            "c_hover45": sel_frame(0, 51, 60),
            "d_click45": sel_frame(1, 50, 60),
        },
    }
    negatives = [
        ("owned_960", "a_avail_8", frame(S9, 248, 52, 10)),  # the available tab holds 10, not 8
        (
            "owned_960",
            "d_owned",
            frame(S9, 248, 52, 10, counter=56),
        ),  # the owned list stopped at the stock rows
        ("owned_960", "e_worker_minus", frame(S9, 248, 52, 13, counter=56)),  # counter unchanged
        ("owned_960", "e_worker_minus", frame(S9, 248, 52, 13, counter=54)),  # two workers off
        ("owned_960", "f_worker_plus", frame(S9, 248, 52, 13, counter=55)),  # "+" did nothing
        ("owned_960", "f_worker_plus", frame(S9, 248, 52, 13)),  # no counter drawn
        ("worker_1024", "b_plus", worker_frame(57)),  # the first "+" ordered 0 (stock)
        ("worker_1024", "b_plus", worker_frame(59)),  # two workers on one click
        ("worker_1024", "c_minus", worker_frame(58)),  # the first "-" ordered 0 (stock)
        ("worker_1024", "c_minus", worker_frame(55)),  # two workers off one click
        ("worker_1024", "a_owned", worker_frame(None)),  # an unknown glyph is RED, not a guess
        (
            "ayy30_960",
            "c_info",
            rows_frame(S9, 1, 13, 300),
        ),  # the right click opened no info screen
        ("ayy30_960", "b_arrow", rows_frame(S9, 0, 13)),  # the arrow did not scroll
        ("ayy30_960", "d_drag", rows_frame(S9, 1, 13)),  # the drag did not scroll
        ("ayy30_960", "c_placing_arrow", rows_frame(S9, 1, 13)),  # no placement
        (
            "ayy30_960",
            "e_placing_drag",
            rows_frame(S9, 5, 13, 300),
        ),  # same building as before the drag
        ("ayy30_1440", "b_group51", sel_frame(0, 40, 30)),  # capped at 40 rows
        ("ayy30_1440", "c_hover45", sel_frame(0, 51, 30)),  # no hover line
        ("ayy30_1440", "d_click45", sel_frame(0, 51, 60)),  # click did nothing
    ]
    fails = []

    def run(mode, frames, log=True):
        with tempfile.TemporaryDirectory() as td:
            for n, img in frames.items():
                img.save(os.path.join(td, "capture_%s%s.png" % (MODE_PREFIX[mode], n)))
            if log:
                with open(os.path.join(td, "mh_video.log"), "w") as f:
                    f.write(logtext(mode))
                with open(os.path.join(td, "mh_net.log"), "w") as f:
                    f.write(NETLOG)
            return problems(mode, td)

    for mode, frames in good.items():
        p = run(mode, frames)
        if p:
            fails.append("good %s: %s" % (mode, p))
        if not run(mode, frames, log=False):
            fails.append("%s without its logs must be RED" % mode)
        if not run(mode, {}):
            fails.append("%s without frames must be RED" % mode)
    for mode, name, img in negatives:
        frames = dict(good[mode])
        frames[name] = img
        if not run(mode, frames):
            fails.append("negative %s/%s stayed green" % (mode, name))
    for f in fails:
        print("SELFTEST FAIL:", f)
    print("check_panel_rows selftest: %d failures" % len(fails))
    return 1 if fails else 0


def main(argv):
    if argv[1:] == ["--selftest"]:
        return selftest()
    if len(argv) != 3:
        print(__doc__)
        return 2
    mode, run_dir = argv[1], argv[2]
    out = problems(mode, run_dir)
    for p in out:
        print("RED:", p)
    if not out:
        print("check_panel_rows %s: PASS (%s)" % (mode, run_dir))
    return 1 if out else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
