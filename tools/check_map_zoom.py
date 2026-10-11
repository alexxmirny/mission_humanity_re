#!/usr/bin/env python3
"""check_map_zoom.py -- RES2: did the map zoom do what the frames claim, by its own log?

A captured frame says WHAT the picture looks like; it cannot say that the map pixel under the cursor stayed
under the cursor or that the camera absorb moved whole tiles. The zoom module (mh/seams/ui_map_zoom.cpp) writes one `[zoom]` line per zoom change to
mh_input.log, with the map pixel under the cursor before and after (`anchor err dx,dy px`), the camera tiles
it moved and the viewport size it read. This checker reads them:

  check_map_zoom.py --wheel <run-dir>   map_zoom_wheel: armed with wheel=1; >= 3 `wheel in` and >= 3 `wheel out`
                                        lines; every anchor error within 2 px; the wheel notch over the side
                                        panel left no line (exactly 6 wheel lines in all)
  check_map_zoom.py --off <run-dir>     map_zoom_wheel_off: armed with wheel=0 and no wheel line at all
  check_map_zoom.py --nowrap <run-dir>  res_nowrap_64: armed, and NO zoom line at all -- nothing in the game start or
                                        the in-game size changes moved the camera (the retired torus minimum zoom
                                        moved it 6 tiles at game start and left the player start off-centre)
  check_map_zoom.py --det <run-dir>     the determinism host: wheel lines, every anchor error within 2 px, and
                                        at least one line that moved the camera (the absorb ran)
  Any mode takes `--segment <name>` for a multi-segment walk (res_walk.txt): the banner is read from the whole log,
  the zoom lines from that segment's slice only (tools/walk_segments.py), so another segment's wheel notches neither
  satisfy nor break the claim.
  check_map_zoom.py --selftest

Absence of the log, of the `armed` line, or of any required line is RED.
"""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from walk_segments import pop_segment, segment_text, slice_or_problem  # noqa: E402

LINE = re.compile(
    r"; \[zoom\] (?P<why>\S+(?: \S+)?)\s+z (?P<z0>[\d.]+) -> (?P<z1>[\d.]+) cursor=(?P<cx>\d+),(?P<cy>\d+) "
    r"origin (?P<l0>\d+),(?P<t0>\d+) -> (?P<l1>\d+),(?P<t1>\d+) crop (?P<cw>\d+)x(?P<ch>\d+) "
    r"cam (?P<dc>-?\d+),(?P<dr>-?\d+) tiles anchor err (?P<ex>-?\d+),(?P<ey>-?\d+) px "
    r"view=(?P<vw>\d+)x(?P<vh>\d+)"
)
ARMED = re.compile(r"; \[zoom\] armed -- wheel=(\d)")
ANCHOR_TOL = 2


def parse(text):
    rows = []
    for m in LINE.finditer(text):
        d = m.groupdict()
        d["why"] = d["why"].strip()
        for k in (
            "cx",
            "cy",
            "l0",
            "t0",
            "l1",
            "t1",
            "cw",
            "ch",
            "dc",
            "dr",
            "ex",
            "ey",
            "vw",
            "vh",
        ):
            d[k] = int(d[k])
        for k in ("z0", "z1"):
            d[k] = float(d[k])
        rows.append(d)
    return rows


def problems(text, mode, segment=None):
    """`segment`: a walk segment's slice of the logs (tools/walk_segments.py). The banner is a boot line, read from the
    whole text; the zoom lines the claim is about are read from the segment alone."""
    out = []
    armed = ARMED.search(text)
    if not armed:
        out.append("no `[zoom] armed` line in mh_input.log: the module never armed")
        return out
    if "[zoom] DISARMED" in text:
        out.append("a `[zoom] DISARMED` line: an entry of the stock zoom could not be taken")
    if segment is not None:
        text = segment
    rows = parse(text)
    wheel = [r for r in rows if r["why"].startswith("wheel")]
    ins = [r for r in wheel if r["why"] == "wheel in"]
    outs = [r for r in wheel if r["why"] == "wheel out"]
    if mode == "off":
        if armed.group(1) != "0":
            out.append("armed with wheel=%s, expected 0" % armed.group(1))
        if wheel:
            out.append("%d wheel line(s) with wheel_zoom=0" % len(wheel))
        return out
    if armed.group(1) != "1" and mode in ("wheel", "det"):
        out.append("armed with wheel=%s, expected 1" % armed.group(1))
    if mode in ("wheel", "det"):
        for r in wheel:
            if abs(r["ex"]) > ANCHOR_TOL or abs(r["ey"]) > ANCHOR_TOL:
                out.append(
                    "anchor error %d,%d px on `%s z %.2f -> %.2f` at cursor %d,%d"
                    % (r["ex"], r["ey"], r["why"], r["z0"], r["z1"], r["cx"], r["cy"])
                )
    if mode == "wheel":
        if len(ins) < 3:
            out.append("%d `wheel in` line(s), expected >= 3" % len(ins))
        if len(outs) < 3:
            out.append("%d `wheel out` line(s), expected >= 3" % len(outs))
        if len(wheel) != 6:
            out.append(
                "%d wheel line(s), expected exactly 6 (a notch over the side panel must leave none)"
                % len(wheel)
            )
    elif mode == "nowrap":
        if rows:
            out.append(
                "%d zoom line(s) (first: `%s`, cam %d,%d): the zoom module acted with no wheel input"
                % (len(rows), rows[0]["why"], rows[0]["dc"], rows[0]["dr"])
            )
    elif mode == "det":
        if not wheel:
            out.append("no wheel line: the host never zoomed")
        if not any(r["dc"] or r["dr"] for r in wheel):
            out.append(
                "no wheel line moved the camera: the camera absorb never ran (vacuous proof)"
            )
    return out


def load(path):
    """The process folder's mh_input.log (the `armed` banner) plus the session folders opened after it (the
    zoom lines of the match): test_ui hands over the lane's newest PROCESS folder, and SES1 puts the match's
    log in a sibling folder whose name sorts after it. Older runs in the same lane sort before it."""
    p = path.rstrip("\\/")
    if os.path.isdir(p):
        root, me = os.path.split(p)
        cand = [os.path.join(p, "mh_input.log")]
        for d in sorted(os.listdir(root)):
            if d > me and os.path.isdir(os.path.join(root, d)):
                cand.append(os.path.join(root, d, "mh_input.log"))
        found = [c for c in cand if os.path.isfile(c)]
        if not found:
            return None, "no mh_input.log under %s" % p
        text = ""
        for c in found:
            with open(c, encoding="utf-8", errors="replace") as f:
                text += f.read()
        return text, None
    if not os.path.isfile(p):
        return None, "no mh_input.log at %s" % p
    with open(p, encoding="utf-8", errors="replace") as f:
        return f.read(), None


def _line(why, z0, z1, ex=0, ey=0, dc=0, dr=0, vw=1120, l0=0, l1=100):
    return (
        "; [zoom] %s z %.2f -> %.2f cursor=700,300 origin %d,0 -> %d,0 crop 932x640 cam %d,%d tiles "
        "anchor err %d,%d px view=%dx768\n" % (why, z0, z1, l0, l1, dc, dr, ex, ey, vw)
    )


def selftest():
    armed1 = "; [zoom] armed -- wheel=1 filter=area\n"
    armed0 = "; [zoom] armed -- wheel=0 filter=area\n"
    good_wheel = (
        armed1
        + "".join(_line("wheel in ", 1, 1.2) for _ in range(3))
        + "".join(_line("wheel out", 1.2, 1) for _ in range(3))
    )
    cases = [
        ("wheel good", good_wheel, "wheel", False),
        ("wheel not armed", good_wheel.replace("[zoom] armed", "[zoom] x"), "wheel", True),
        ("wheel anchor off", good_wheel + _line("wheel in ", 1, 1.2, ex=5), "wheel", True),
        (
            "wheel only 2 in",
            armed1 + _line("wheel in ", 1, 1.2) * 2 + _line("wheel out", 1.2, 1) * 3,
            "wheel",
            True,
        ),
        ("wheel panel notch leaked", good_wheel + _line("wheel in ", 1, 1.2), "wheel", True),
        ("nowrap good", armed1, "nowrap", False),
        ("nowrap not armed", "", "nowrap", True),
        (
            "nowrap a limit line moved the camera",
            armed1 + _line("limit    ", 1.0, 1.17, dc=6),
            "nowrap",
            True,
        ),
        ("off good", armed0, "off", False),
        ("off but zoomed", armed0 + _line("wheel in ", 1, 1.2), "off", True),
        ("off armed 1", armed1, "off", True),
        (
            "det good",
            armed1 + _line("wheel in ", 1, 1.2) + _line("wheel out", 1.2, 1, dc=-1),
            "det",
            False,
        ),
        (
            "det no camera tile",
            armed1 + _line("wheel in ", 1, 1.2) + _line("wheel out", 1.2, 1),
            "det",
            True,
        ),
        ("disarmed", good_wheel + "; [zoom] DISARMED: x\n", "wheel", True),
    ]
    fails = [n for n, t, m, red in cases if bool(problems(t, m)) != red]
    nl = chr(10)
    whole = (
        armed1
        + "; [mark] other"
        + nl
        + good_wheel.replace(armed1, "")
        + "; [mark] quiet"
        + nl
        + "; [mark] wheeled"
        + nl
        + good_wheel.replace(armed1, "")
    )
    if problems(whole, "wheel", segment_text(whole, "wheeled")):
        fails.append("segment wheel good")
    if not problems(whole, "wheel", segment_text(whole, "quiet")):
        fails.append("a segment with no wheel lines must be RED although another has six")
    if problems(whole, "nowrap", segment_text(whole, "quiet")):
        fails.append("nowrap: the quiet segment has no zoom line")
    if not problems(whole, "nowrap", segment_text(whole, "wheeled")):
        fails.append("nowrap: a zoom line in the segment must be RED")
    with tempfile.TemporaryDirectory() as td:
        t, err = load(td)
        if not err:
            fails.append("missing log must be an error")
    for n in fails:
        print("SELFTEST FAIL:", n)
    print("check_map_zoom selftest: %s" % ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    seg_name, argv = pop_segment(argv)
    mode = next((m for m in ("wheel", "off", "nowrap", "det") if "--" + m in argv), None)
    paths = [a for a in argv if not a.startswith("--")]
    if not mode or not paths:
        print("usage: check_map_zoom.py (--wheel|--off|--nowrap|--det) <run-dir> | --selftest")
        return 2
    text, err = load(paths[0])
    seg = None
    if not err and seg_name:
        seg, err = slice_or_problem(text, seg_name)
    probs = [err] if err else problems(text, mode, seg)
    for p in probs:
        print("FAIL:", p)
    if not probs:
        print(
            "PASS: map zoom log (%s) -- %d zoom line(s)"
            % (mode, len(parse(text if seg is None else seg)))
        )
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
