#!/usr/bin/env python3
"""check_res4_opt.py -- mods:RES4: the in-game Options screen's "Interface size" row.

The row res4_opt walks ESC > Options, steps Interface size 100% -> 125% -> 150%, presses Ok, plays on, then opens the screen
again and wraps 150% -> 175% -> 200% -> Auto -> 100% and presses Ok. This checks what a picture cannot say:

  1. mh_video.log carries `options menu: Interface size -> 150 (saved; the ini reads back [video] ui_scale=150)` and then the same
     line for 100: the choice reached the ini the launcher writes (the DLL reads the key back after writing it);
  2. the presenter went 100 -> 150 -> 100 live: `; [ui] ui_scale -> 150%` then `; [ui] ui_scale -> 100%`, and the layout line of
     the scaled strategic view (composition of the 1920x1080 screen, frame 1856x1088) was logged;
  3. the presented image at 150% (capture_res4_150) is not the one at 100% (capture_res4_opt_100a): the scale really changed the
     picture (tools/check_ui_quads.py proves WHERE the quads land, pixel for pixel); the image back at 100% is the single-quad one
     again (same size as the first, and not the 150% composition).

  check_res4_opt.py <run-dir>
  check_res4_opt.py --selftest
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

SAVED = re.compile(
    r"; \[ui\] options menu: Interface size -> (\w+) \(saved; the ini reads back \[video\] ui_scale=(\w+)\)"
)
CHANGE = re.compile(r"; \[ui\] ui_scale -> (\d+)%")
LAYOUT = re.compile(r"; \[ui\] ui_scale 150%.* on 1920x1080, frame 1856x1088: ")


def read_bmp(path):
    """(w, h, bytes of the pixel array) of an uncompressed 24/32-bit BMP, or None."""
    try:
        d = open(path, "rb").read()
    except OSError:
        return None
    if d[:2] != b"BM":
        return None
    off = int.from_bytes(d[10:14], "little")
    w = int.from_bytes(d[18:22], "little", signed=True)
    h = int.from_bytes(d[22:26], "little", signed=True)
    return abs(w), abs(h), d[off:]


def problems_in(video_log_text, a100, p150, b100):
    probs = []
    saved = SAVED.findall(video_log_text)
    if [s[0] for s in saved] != ["150", "100"]:
        probs.append("expected the saved choices 150 then 100 in mh_video.log, got %r" % (saved,))
    for choice, back in saved:
        if choice != back:
            probs.append("the ini reads back ui_scale=%s after writing %s" % (back, choice))
    changes = CHANGE.findall(video_log_text)
    if changes != ["150", "100"]:
        probs.append("expected the live presenter changes 150 then 100, got %r" % (changes,))
    if not LAYOUT.search(video_log_text):
        probs.append("no 150%% layout line for the 1920x1080 screen / 1856x1088 frame")
    for name, img in (
        ("capture_res4_opt_100a", a100),
        ("capture_res4_150", p150),
        ("capture_res4_opt_100b", b100),
    ):
        if img is None:
            probs.append("%s.bmp is missing or not a BMP" % name)
    if probs:
        return probs
    if not (a100[0] == b100[0] == p150[0] and a100[1] == b100[1] == p150[1]):
        probs.append("presented sizes differ: %s %s %s" % (a100[:2], p150[:2], b100[:2]))
    elif a100[2] == p150[2]:
        probs.append("the picture at 150%% equals the one at 100%% -- the scale changed nothing")
    elif b100[2] == p150[2]:
        probs.append("the picture back at 100%% still equals the 150%% composition")
    return probs


def problems(run_dir):
    vlog = os.path.join(run_dir, "mh_video.log")
    if not os.path.exists(vlog):
        return ["no mh_video.log in %s" % run_dir]
    text = open(vlog, encoding="utf-8", errors="replace").read()
    return problems_in(
        text,
        read_bmp(os.path.join(run_dir, "capture_res4_opt_100a.bmp")),
        read_bmp(os.path.join(run_dir, "capture_res4_150.bmp")),
        read_bmp(os.path.join(run_dir, "capture_res4_opt_100b.bmp")),
    )


def selftest():
    good = (
        "; [ui] options menu: Interface size -> 150 (saved; the ini reads back [video] ui_scale=150)\n"
        "; [ui] ui_scale -> 150% (applied at the next strategic present)\n"
        "; [ui] ui_scale 150% on 1920x1080, frame 1856x1088: map 1680x1032 1:1\n"
        "; [ui] options menu: Interface size -> 100 (saved; the ini reads back [video] ui_scale=100)\n"
        "; [ui] ui_scale -> 100% (applied at the next strategic present)\n"
    )
    a, p, b = (4, 2, b"\x01" * 8), (4, 2, b"\x02" * 8), (4, 2, b"\x03" * 8)
    assert problems_in(good, a, p, b) == [], problems_in(good, a, p, b)
    assert problems_in(
        good.replace("->  150", "").replace("Interface size -> 150", "Interface size -> 125"),
        a,
        p,
        b,
    )
    assert problems_in(
        good.replace("ui_scale=150)", "ui_scale=100)"), a, p, b
    )  # write did not stick
    assert problems_in(good, a, a, b)  # 150 looks like 100
    assert problems_in(good, a, p, p)  # never went back
    assert problems_in(good, None, p, b)
    assert problems_in("", a, p, b)
    print("check_res4_opt selftest OK")
    return 0


def main(argv):
    if argv and argv[0] == "--selftest":
        return selftest()
    if not argv:
        print(__doc__)
        return 2
    probs = problems(argv[-1])
    if probs:
        for p in probs:
            print("RED: " + p)
        return 1
    print(
        "res4_opt OK: Interface size 100 -> 150 -> 100 through the Options screen, saved to the ini, applied live"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
