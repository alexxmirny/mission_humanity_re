#!/usr/bin/env python3
"""check_mode_guards.py -- D13: did the mode-change crash guards arm AND fire on this run?

A green UI walk cannot say which code drew a background or whether the cursor really was outside the new
mode, so the two rows that cover the guards read mh_video.log (the PROCESS run directory test_ui hands
over) instead of trusting the frame:

  check_mode_guards.py --scaled <run-dir>   galaxy_bg_picker: all three arm lines, and a
                                            `background WxH scaled to ...` line (the header-aware scaler
                                            took a mismatched image), no `SKIPPED`.
  check_mode_guards.py --cursor <run-dir>   quit_from_large_mode: all three arm lines, and a
                                            `mode-change cursor clamp LIVE` line (the stale cursor really
                                            was outside the new mode, so the row cannot pass vacuously).
  check_mode_guards.py --native WxH <run-dir>
                                            galaxy_native_*: the planet-select screen switched from the picker
                                            size WxH to the stock 1024x768 and restored the picker size on exit
                                            (RES6), and the stretched-art scaler did NOT run.
  Each of the three takes `--segment <name>` for a multi-segment walk (res_walk.txt): the arm lines are read from the
  whole log, the rest from that segment's slice only (tools/walk_segments.py).
  check_mode_guards.py --selftest

Absence of the log, or of any required line, is RED.
"""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from walk_segments import pop_segment, slice_or_problem  # noqa: E402

ARMED = (
    "mode-change cursor clamp armed",
    "galaxy background index clamp armed",
    "background restore scaler armed",
)
NOT_ARMED = re.compile(r"NOT armed|mode-change guards DISABLED")


def problems(text, mode, size=None, segment=None):
    """`segment`: the walk segment's own slice of the log (tools/walk_segments.py). The arm lines are boot lines, so
    they are read from the WHOLE log; every claim about what happened is read from the segment alone."""
    out = []
    for needle in ARMED:
        if needle not in text:
            out.append("mh_video.log has no `%s` line" % needle)
    if NOT_ARMED.search(text):
        out.append("a guard reports NOT armed / DISABLED")
    whole = text
    if segment is not None:
        text = segment
    if mode == "scaled":
        if not re.search(r"background \d+x\d+ scaled to \d+x\d+ \(\w+\)", text):
            out.append(
                "no `background WxH scaled to ...` line: the scaler never took a mismatched image"
            )
        if "background restore SKIPPED" in text:
            out.append("`background restore SKIPPED` -- a background had no image")
    elif mode == "native":
        if "galaxy native armed" not in whole:
            out.append("no `galaxy native armed` line")
        m = re.search(r"galaxy native: (\d+x\d+) \(picker (\d+)\) -> 1024x768", text)
        if not m:
            out.append(
                "no `galaxy native: WxH (picker N) -> 1024x768` line: the screen never switched"
            )
        else:
            if size and m.group(1) != size:
                out.append("switched from %s, expected %s" % (m.group(1), size))
            r = re.search(r"galaxy native: leaving the screen, restoring picker (\d+)", text)
            if not r:
                out.append(
                    "no `galaxy native: leaving the screen` line: the size was never restored"
                )
            elif r.group(1) != m.group(2):
                out.append("restored picker %s, entered from %s" % (r.group(1), m.group(2)))
        if re.search(r"background \d+x\d+ scaled to", text):
            out.append("a background was stretched: the screen was drawn at the picker size")
    elif mode == "cursor":
        if "mode-change cursor clamp LIVE" not in text:
            out.append(
                "no `cursor clamp LIVE` line: the stale cursor never left the new mode (vacuous pass)"
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


NATIVE_IN = (
    "; [video] galaxy native: 1280x768 (picker 3) -> 1024x768 for the planet-select screen\n"
)
NATIVE_OK = (
    "; [video] galaxy native armed: x\n"
    + NATIVE_IN
    + "; [video] galaxy native: leaving the screen, restoring picker 3\n"
)


def selftest():
    good = "\n".join("; [video] " + n for n in ARMED) + "\n"
    cases = [
        (
            "scaled good",
            good + "; [video] background 1024x768 scaled to 1280x768 (point) -- x\n",
            "scaled",
            False,
        ),
        ("scaled none", good, "scaled", True),
        (
            "scaled skipped",
            good + "background 1x1 scaled to 2x2 (point)\nbackground restore SKIPPED\n",
            "scaled",
            True,
        ),
        (
            "native good",
            good + NATIVE_OK,
            "native",
            False,
        ),
        ("native never switched", good + "; [video] galaxy native armed: x\n", "native", True),
        (
            "native not restored",
            good + "; [video] galaxy native armed: x\n" + NATIVE_IN,
            "native",
            True,
        ),
        (
            "native stretched",
            good + NATIVE_OK + "; [video] background 1024x768 scaled to 1280x768 (point)\n",
            "native",
            True,
        ),
        ("cursor good", good + "; [video] mode-change cursor clamp LIVE -- x\n", "cursor", False),
        ("cursor vacuous", good, "cursor", True),
        (
            "not armed",
            good + "; [video] foo NOT armed (x)\nmode-change cursor clamp LIVE\n",
            "cursor",
            True,
        ),
        ("missing arm line", "mode-change cursor clamp LIVE\n", "cursor", True),
    ]
    fails = [n for n, t, m, red in cases if bool(problems(t, m, "1280x768")) != red]
    # segments: the claim must be IN the segment; another segment's lines do not count
    seg_a = "; [mark] a\n" + NATIVE_OK
    seg_b = "; [mark] b\n; [video] nothing here\n"
    whole = good + seg_a + seg_b
    from walk_segments import segment_text

    if problems(whole, "native", "1280x768", segment_text(whole, "a")):
        fails.append("segment a holds the switch")
    if not problems(whole, "native", "1280x768", segment_text(whole, "b")):
        fails.append("segment b has no switch: must be RED although segment a has one")
    if not problems(whole, "native", "1920x1440", segment_text(whole, "a")):
        fails.append("wrong size in the segment must be RED")
    with tempfile.TemporaryDirectory() as td:
        t, err = load(td)
        if not err:
            fails.append("missing log must be an error")
    for n in fails:
        print("SELFTEST FAIL:", n)
    print("check_mode_guards selftest: %s" % ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    seg_name, argv = pop_segment(argv)
    mode = "scaled" if "--scaled" in argv else "cursor" if "--cursor" in argv else None
    size = None
    if "--native" in argv:
        mode = "native"
        i = argv.index("--native")
        size = argv[i + 1] if i + 1 < len(argv) else None
        argv = argv[:i] + argv[i + 2 :]
    paths = [a for a in argv if not a.startswith("--")]
    if not mode or not paths:
        print("usage: check_mode_guards.py (--scaled|--cursor|--native WxH) <run-dir> | --selftest")
        return 2
    text, err = load(paths[0])
    seg = None
    if not err and seg_name:
        seg, err = slice_or_problem(text, seg_name)
    probs = [err] if err else problems(text, mode, size, seg)
    for p in probs:
        print("FAIL:", p)
    if not probs:
        print(
            "PASS: mode-change guards armed and %s"
            % (
                "scaled a background"
                if mode == "scaled"
                else "switched the galaxy screen to 1024x768 and back"
                if mode == "native"
                else "clamped a stale cursor"
            )
        )
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
