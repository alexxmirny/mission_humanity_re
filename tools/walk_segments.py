#!/usr/bin/env python3
"""walk_segments.py -- slice a multi-segment UI walk's logs by the script's `mark <segment>` lines.

A walk such as res_walk.txt runs many old scenarios in ONE process. Each segment opens with `mark <name>`, which writes
`; [mark] <name>` into mh_video.log and mh_input.log (the logs the post_checks read). A checker given `--segment <name>`
reads only the text between that mark and the next one, so a claim ("the screen switched from 1920x1440") is made about
the segment that did it and cannot be satisfied by another segment's lines.

  segment_text(text, name)   the text after `; [mark] name` up to the next `; [mark] ` line (or the end), or None
  marks(text)                the segment names in order
  pop_segment(argv)          (name or None, argv without `--segment name`)
  --selftest
"""

import re
import sys

MARK = re.compile(r"^; \[mark\] (\S.*?)\s*$", re.M)


def marks(text):
    return [m.group(1) for m in MARK.finditer(text)]


def segment_text(text, name):
    ms = list(MARK.finditer(text))
    for i, m in enumerate(ms):
        if m.group(1) == name:
            end = ms[i + 1].start() if i + 1 < len(ms) else len(text)
            return text[m.end() : end]
    return None


def pop_segment(argv):
    argv = list(argv)
    if "--segment" in argv:
        i = argv.index("--segment")
        name = argv[i + 1] if i + 1 < len(argv) else None
        return name, argv[:i] + argv[i + 2 :]
    return None, argv


def slice_or_problem(text, name):
    """(text, None) -- the whole text when no segment is asked for; else the segment, or (None, why) when absent."""
    if not name:
        return text, None
    seg = segment_text(text, name)
    if seg is None:
        return None, "no `; [mark] %s` line (saw: %s): the walk never reached that segment" % (
            name,
            ", ".join(marks(text)) or "none",
        )
    return seg, None


def selftest():
    t = "a\n; [mark] one\nx1\n; [mark] two\nx2\n; [mark] three\nx3\n"
    fails = []
    if segment_text(t, "two") != "\nx2\n":
        fails.append("middle segment")
    if segment_text(t, "three") != "\nx3\n":
        fails.append("last segment")
    if segment_text(t, "one") != "\nx1\n":
        fails.append("first segment")
    if segment_text(t, "four") is not None:
        fails.append("absent segment must be None")
    if marks(t) != ["one", "two", "three"]:
        fails.append("marks order")
    if slice_or_problem(t, None) != (t, None):
        fails.append("no segment asked = whole text")
    if slice_or_problem(t, "four")[0] is not None:
        fails.append("absent segment is a problem")
    if pop_segment(["--native", "1x1", "--segment", "s", "dir"]) != (
        "s",
        ["--native", "1x1", "dir"],
    ):
        fails.append("pop_segment")
    for f in fails:
        print("SELFTEST FAIL:", f)
    print("walk_segments selftest: %s" % ("FAIL" if fails else "ok"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(selftest() if "--selftest" in sys.argv else 2)
