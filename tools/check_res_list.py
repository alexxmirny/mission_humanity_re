#!/usr/bin/env python3
"""check_res_list.py -- RES3: did the run boot at the configured generated size and present it centred?

A UI capture is the composed game frame, so it cannot say what the PRESENTER was asked for. mh_video.log can:

  check_res_list.py <W>x<H> <integer|fit> <run-dir>

requires, in the process run directory's mh_video.log:
  * `resolution=<spec>: desktop DxD, display scale N% -> WxH (entry i of n)`  the generated list armed and
    chose the expected size;
  * `[gfx] SetDisplayMode WxH`  the owned display was asked for the RENDERED size (not the desktop height:
    1920x1080 renders 1056 rows, and asking for 1080 leaves 24 black rows at the bottom);
  * `presentation: WxH on a DxD desktop -> <integer|fit> scaling`  the scaling rule the size got;
  * the `list [i] WxH (...)` lines in the generated ORDER: the classic three (640x480, 800x576, 1024x768), then every
    4:3 size, then every 16:9 size, then the rest, each group ascending by area (the rule of gfx/res_list.cpp
    aspect_group, repeated here on purpose: a second statement of it that cannot drift silently).
Absence of the log or of any line is RED.
  check_res_list.py --ingame --segment <name> <W>x<H> <integer|fit> <run-dir>
      a size reached IN GAME in a multi-segment walk: that segment's own slice of mh_video.log holds
      `SetDisplayMode WxH` and `presentation: WxH ... -> <rule> scaling` (tools/walk_segments.py)
  check_res_list.py --selftest
"""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from walk_segments import pop_segment, segment_text, slice_or_problem  # noqa: E402


CLASSIC = [(640, 480), (800, 576), (1024, 768)]


def group(w, h):
    """The tail's grouping: 0 = 4:3, 1 = 16:9, 2 = the rest (the 32-px snap allowed for)."""
    if -96 <= 3 * w - 4 * h <= 96:
        return 0
    if -288 <= 9 * w - 16 * h <= 496:
        return 1
    return 2


def order_problems(text):
    rows = {}
    for i, w, h in re.findall(r"\blist \[(\d+)\] (\d+)x(\d+) \(", text):
        rows[int(i)] = (int(w), int(h))
    if not rows:
        return ["no `list [i] WxH (...)` lines: the generated list was never logged"]
    sizes = [rows[i] for i in sorted(rows)]
    if sorted(rows) != list(range(len(rows))):
        return ["the list lines are not 0..n-1 (saw %s)" % sorted(rows)]
    out = []
    if sizes[:3] != CLASSIC:
        out.append("the list does not start with the classic three (saw %s)" % sizes[:3])
    tail = sizes[3:]
    want = sorted(tail, key=lambda e: (group(*e), e[0] * e[1], e[0]))
    if tail != want:
        out.append(
            "tail order is not 4:3, 16:9, others by area: saw %s, want %s"
            % (["%dx%d" % e for e in tail], ["%dx%d" % e for e in want])
        )
    return out


def problems(text, size, scaling):
    out = order_problems(text)
    m = re.search(
        r"resolution=\S+: desktop \d+x\d+, display scale \d+% -> (\d+x\d+) \(entry \d+ of \d+\)",
        text,
    )
    if not m:
        out.append(
            "no `resolution=...: desktop ... -> WxH (entry i of n)` line: the generated list never armed"
        )
    elif m.group(1) != size:
        out.append("the list chose %s, expected %s" % (m.group(1), size))
    if not re.search(r"SetDisplayMode %s\b" % re.escape(size), text):
        out.append(
            "no `SetDisplayMode %s`: the display was never asked for the rendered size" % size
        )
    pm = re.findall(r"presentation: (\d+x\d+) on a \d+x\d+ desktop -> (\w+) scaling", text)
    if not [1 for s, k in pm if s == size and k == scaling]:
        out.append(
            "no `presentation: %s ... -> %s scaling` line (saw %s)" % (size, scaling, pm or "none")
        )
    return out


def ingame_problems(segment, size, scaling):
    """res_walk_auto's res_frac segment: the size was reached IN GAME (Options > Screen size), so the segment itself
    holds the display request for the RENDERED size and the presentation line with the scaling rule it got."""
    out = []
    if not re.search(r"SetDisplayMode %s\b" % re.escape(size), segment):
        out.append(
            "segment has no `SetDisplayMode %s`: the display was never asked for that size" % size
        )
    pm = re.findall(r"presentation: (\d+x\d+) on a \d+x\d+ desktop -> (\w+) scaling", segment)
    if not [1 for sz, k in pm if sz == size and k == scaling]:
        out.append(
            "segment has no `presentation: %s ... -> %s scaling` line (saw %s)"
            % (size, scaling, pm or "none")
        )
    return out


def load(path):
    p = os.path.join(path, "mh_video.log") if os.path.isdir(path) else path
    if not os.path.isfile(p):
        return None, "no mh_video.log at %s" % p
    with open(p, encoding="utf-8", errors="replace") as f:
        return f.read(), None


def selftest():
    good = (
        "; [video] resolution=auto: desktop 1920x1080, display scale 100% -> 1920x1056 (entry 10 of 11)\n"
        "; [video]   list [0] 640x480 (classic)\n"
        "; [video]   list [1] 800x576 (classic)\n"
        "; [video]   list [2] 1024x768 (classic)\n"
        "; [video]   list [3] 672x512 (4:3, 2.11x)\n"
        "; [video]   list [4] 1408x1056 (4:3, 1:1)\n"
        "; [video]   list [5] 960x512 (2x)\n"
        "; [video]   list [6] 1920x1056 (1:1, sharpest)\n"
        "; [gfx] SetDisplayMode 1920x1056 (no real mode switch)\n"
        "; [video] presentation: 1920x1056 on a 1920x1080 desktop -> integer scaling\n"
    )
    cases = [
        ("good", good, "1920x1056", "integer", False),
        ("wrong size chosen", good, "1280x704", "fit", True),
        ("wrong scaling", good, "1920x1056", "fit", True),
        (
            "desktop height requested",
            good.replace("SetDisplayMode 1920x1056", "SetDisplayMode 1920x1080"),
            "1920x1056",
            "integer",
            True,
        ),
        ("no resolution line", good.replace("resolution=auto", "x"), "1920x1056", "integer", True),
        ("no list lines", good.replace("list [", "lst ["), "1920x1056", "integer", True),
        (
            "a 16:9 size before a 4:3 one",
            good.replace("list [3] 672x512", "list [3] 960x512").replace(
                "list [5] 960x512", "list [5] 672x512"
            ),
            "1920x1056",
            "integer",
            True,
        ),
        (
            "a tail ordered by area only",
            good.replace("list [4] 1408x1056", "list [4] 1280x704"),
            "1920x1056",
            "integer",
            True,
        ),
        (
            "a classic size missing from the head",
            good.replace("list [1] 800x576", "list [1] 800x600"),
            "1920x1056",
            "integer",
            True,
        ),
        ("empty", "", "1920x1056", "integer", True),
    ]
    fails = [n for n, t, s, k, red in cases if bool(problems(t, s, k)) != red]
    nl = chr(10)
    seg = (
        "; [gfx] SetDisplayMode 1280x704 (no real mode switch)"
        + nl
        + "; [video] presentation: 1280x704 on a 1920x1080 desktop -> fit scaling"
        + nl
    )
    whole = good + "; [mark] a" + nl + "; [mark] res_frac" + nl + seg
    if ingame_problems(segment_text(whole, "res_frac"), "1280x704", "fit"):
        fails.append("ingame good")
    if not ingame_problems(segment_text(whole, "a"), "1280x704", "fit"):
        fails.append("ingame: another segment's lines must not count")
    if not ingame_problems(seg, "1280x704", "integer"):
        fails.append("ingame wrong scaling")
    if not ingame_problems(
        seg.replace("SetDisplayMode 1280x704", "SetDisplayMode 1280x720"), "1280x704", "fit"
    ):
        fails.append("ingame wrong display size")
    with tempfile.TemporaryDirectory() as td:
        t, err = load(td)
        if not err:
            fails.append("missing log must be an error")
    for n in fails:
        print("SELFTEST FAIL:", n)
    print("check_res_list selftest: %s" % ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    seg_name, argv = pop_segment(argv)
    ingame = "--ingame" in argv
    argv = [a for a in argv if a != "--ingame"]
    if len(argv) != 3 or (ingame and not seg_name):
        print(
            "usage: check_res_list.py <W>x<H> <integer|fit> <run-dir> | "
            "--ingame --segment <name> <W>x<H> <integer|fit> <run-dir> | --selftest"
        )
        return 2
    text, err = load(argv[2])
    if ingame:
        seg = None
        if not err:
            seg, err = slice_or_problem(text, seg_name)
        probs = [err] if err else ingame_problems(seg, argv[0], argv[1])
    else:
        probs = [err] if err else problems(text, argv[0], argv[1])
    for p in probs:
        print("FAIL:", p)
    if not probs:
        print(
            "PASS: generated list armed at %s, rendered size requested, %s scaling"
            % (argv[0], argv[1])
        )
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
