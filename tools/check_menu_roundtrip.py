#!/usr/bin/env python3
"""check_menu_roundtrip.py -- RES3: did the borderless window stay put through the game's mode changes?

The game MoveWindow's its window to (0,0,w,h) on every display-mode change (llm_gfx_apply_window_resolution), and
the borderless owner used to restore it to the monitor on the next present: one frame of a shrunken window, seen
as a flicker. A captured frame cannot show that, so the row reads mh_video.log (the PROCESS run directory test_ui
hands over):

  check_menu_roundtrip.py <run-dir>   the window was placed on its monitor (`window: borderless WxH`); after that
                                      NO `[tbstate] POSCHANGED` / `SIZE` line carries a window rect other than the
                                      monitor's; at least two mode changes happened and at least one game
                                      MoveWindow was swallowed (so the row cannot pass vacuously); no
                                      `SetDisplayMode` repeats the size it just set (a redundant re-apply).
  Both take `--segment <name>` for a multi-segment walk (res_walk_auto.txt): the placement is read from the whole log,
  the window and mode-change claims from that segment's slice only (tools/walk_segments.py).
  check_menu_roundtrip.py --selftest

Absence of the log, of the placement line or of any required line is RED.
"""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from walk_segments import pop_segment, segment_text, slice_or_problem  # noqa: E402

PLACED = re.compile(r"; \[gfx\] window: borderless (\d+)x(\d+) on its monitor")
TB = re.compile(r"; \[tbstate\] t=\d+ (POSCHANGED|SIZE)\b.* wr=(\S+) mon=(\S+)")
SETMODE = re.compile(r"; \[gfx\] SetDisplayMode (\d+x\d+)")
SWALLOWED = re.compile(r"; \[gfx\] window: game MoveWindow\(.*\) swallowed")


def problems(text, segment=None):
    """`segment`: a walk segment's slice of mh_video.log (tools/walk_segments.py). The placement line is a boot line,
    read from the whole log; the window and mode-change claims are read from the segment alone."""
    out = []
    lines = text.splitlines()
    placed_at = next((i for i, ln in enumerate(lines) if PLACED.search(ln)), None)
    if placed_at is None:
        return [
            "no `window: borderless WxH on its monitor` line: the window was never placed (vacuous)"
        ]
    swallow_lines = lines[
        placed_at + 1 :
    ]  # the swallow proof is the whole run's: the segment may not trigger one itself
    if segment is not None:
        lines, placed_at = segment.splitlines(), -1
    bad = 0
    for ln in lines[placed_at + 1 :]:
        m = TB.search(ln)
        if m and m.group(2) != m.group(3):
            bad += 1
            if bad <= 3:
                out.append(
                    "window moved after placement (%s wr=%s, monitor %s)"
                    % (m.group(1), m.group(2), m.group(3))
                )
    modes = [m.group(1) for m in (SETMODE.search(ln) for ln in lines[placed_at + 1 :]) if m]
    if len(modes) < 2:
        out.append("only %d mode change(s) after placement; the row needs at least 2" % len(modes))
    for a, b in zip(modes, modes[1:]):
        if a == b:
            out.append("SetDisplayMode %s repeated back to back (a redundant re-apply)" % a)
            break
    if not any(SWALLOWED.search(ln) for ln in swallow_lines):
        out.append(
            "no `game MoveWindow ... swallowed` line: the game never moved the window (vacuous pass)"
        )
    return out


def load(path):
    p = path
    if os.path.isdir(p):
        p = os.path.join(p, "mh_video.log")
    if not os.path.isfile(p):
        return None, "no mh_video.log at %s" % p
    with open(p, encoding="utf-8", errors="replace") as f:
        return f.read(), None


def selftest():
    mon = "mon=0,0,1920,1080"
    placed = (
        "; [gfx] window: borderless 1920x1080 on its monitor (not topmost), frame 640x480 scaled\n"
    )
    mode = lambda s: "; [gfx] SetDisplayMode %s (no real mode switch)\n" % s
    swal = "; [gfx] window: game MoveWindow(0,0,1024,768) swallowed (borderless geometry is ours)\n"
    ok_tb = "; [tbstate] t=5 POSCHANGED flags=1 drop=0 hwnd=1 wr=0,0,1920,1080 %s fg=1\n" % mon
    bad_tb = "; [tbstate] t=6 SIZE type=0 drop=0 hwnd=1 wr=0,0,640,480 %s fg=1\n" % mon
    good = placed + mode("1024x768") + swal + ok_tb + mode("1280x1024") + mode("1024x768")
    cases = [
        ("good", good, False),
        ("no placement", mode("1024x768") + swal, True),
        ("window shrank", good + bad_tb, True),
        ("shrink before placement is the boot, not a failure", bad_tb + good, False),
        ("repeat", placed + mode("1024x768") + mode("1024x768") + swal, True),
        ("no swallow", placed + mode("1024x768") + mode("1280x1024"), True),
        ("one mode change", placed + mode("1024x768") + swal, True),
    ]
    fails = [n for n, t, red in cases if bool(problems(t)) != red]
    nl = chr(10)
    seg = "; [mark] trips" + nl + mode("1280x1024") + swal + mode("1024x768") + ok_tb
    whole = placed + mode("1024x768") + seg + "; [mark] later" + nl + mode("800x576") + bad_tb
    if problems(whole, segment_text(whole, "trips")):
        fails.append("segment good (the later segment's shrink is not this segment's)")
    if not problems(whole, segment_text(whole, "later")):
        fails.append("segment later: a shrink and one mode change must be RED")
    if not problems(seg, segment_text(seg, "trips")):
        fails.append("no placement line anywhere must be RED")
    with tempfile.TemporaryDirectory() as td:
        t, err = load(td)
        if not err:
            fails.append("missing log must be an error")
    for n in fails:
        print("SELFTEST FAIL:", n)
    print("check_menu_roundtrip selftest: %s" % ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    seg_name, argv = pop_segment(argv)
    paths = [a for a in argv if not a.startswith("--")]
    if not paths:
        print("usage: check_menu_roundtrip.py <run-dir> | --selftest")
        return 2
    text, err = load(paths[0])
    seg = None
    if not err and seg_name:
        seg, err = slice_or_problem(text, seg_name)
    probs = [err] if err else problems(text, seg)
    for p in probs:
        print("FAIL:", p)
    if not probs:
        print("PASS: the borderless window never left its monitor rect through the mode changes")
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
