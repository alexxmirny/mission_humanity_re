#!/usr/bin/env python3
"""
tools/check_avail_rows.py -- patching:PT-CAP12: the per-player build list is read at the stock row stride for every seat.

The v0.2.2 cap relocation layer once rewrote 13 IMUL immediates 200 -> 1016 (the LABS row stride) that are really the
AvailableBuildings[8][50] row stride (50 ints x 4 B = 200 = 25 labs x 8 B, a collision). The build and research panels then read
seat k's list at 0x50aa37 + k*1016: seat 1 survived by landing inside the array, seat >= 2 read AvailableProjects (project ids as
building types: Main base, civil Chatka, blank zero-cost rows) and the colony / power plant / mine were missing.

The stimulus is tools/uiscripts/avail_rows_solo.txt (1 human + 3 AI, all Human race); its `logavail` op writes
`; [avail] <label> side= ui_stride= s0=.. s7=.. u0=.. u7=..` lines into mh_video.log: s<k> is the table as the game's writers filled
it (stock stride 200), u<k> is the same table as the retail panels read it (the stride immediate live at 0x004163a1).

Clauses, on every `landed` and `late` line (the table is filled at landing; `pre_landing` is only reported):
  1  ui_stride == 200 (the panel still reads the stock stride)
  2  s0 is the stock Human list: non-empty, no zero-cost / civil rows -- it holds the colony, power plant and mine ids (--need)
  3  seats 0..3 (the seated players, all Human): u<k> == s<k> == s0 (the same stock list for every seat)
  4  the unseated seats 4..7 read empty (u<k> == s<k>)

  python tools/check_avail_rows.py [--need ID,ID,..] [--expect-red] <run-dir> [<run-dir> ...]
  python tools/check_avail_rows.py --selftest

--expect-red inverts the verdict (exit 0 only when at least one clause fails): the pre-fix twin of the row.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

LINE = re.compile(
    r"; \[avail\] (?P<label>\S+) side=(?P<side>\d+) ui_stride=(?P<stride>\d+)(?P<rest>.*)$"
)
ROW = re.compile(r"\b([su])(\d)=(\S+)")
SEATED = (0, 1, 2, 3)
CHECKED = ("landed", "late")


def video_logs(run_dir):
    """mh_video.log of the run dir and of the process folders that start at or after it (the check_bigmap shape)."""
    run_dir = os.path.normpath(run_dir)
    paths = []
    p = os.path.join(run_dir, "mh_video.log")
    if os.path.isfile(p):
        paths.append(p)
    else:
        logs_dir, me = os.path.dirname(run_dir), os.path.basename(run_dir)
        for n in sorted(
            x for x in os.listdir(logs_dir) if os.path.isdir(os.path.join(logs_dir, x))
        ):
            q = os.path.join(logs_dir, n, "mh_video.log")
            if n >= me and os.path.isfile(q):
                paths.append(q)
    return paths


def parse(text):
    """-> [{label, side, stride, s: {k: tuple}, u: {k: tuple}}]"""
    out = []
    for raw in text.splitlines():
        m = LINE.search(raw)
        if not m:
            continue
        rec = {
            "label": m["label"],
            "side": int(m["side"]),
            "stride": int(m["stride"]),
            "s": {},
            "u": {},
        }
        for kind, k, val in ROW.findall(m["rest"]):
            rec[kind][int(k)] = () if val == "-" else tuple(int(x) for x in val.split(","))
        out.append(rec)
    return out


def problems(recs, need=()):
    bad = []
    seen = [r for r in recs if r["label"] in CHECKED]
    if not seen:
        return [
            "no `; [avail] landed|late` line in mh_video.log (the script did not reach the game, or logavail is missing)"
        ]
    for r in seen:
        tag = "[%s]" % r["label"]
        if r["stride"] != 200:
            bad.append(
                "%s the build panel stride immediate is %d, not the stock 200" % (tag, r["stride"])
            )
        s0 = r["s"].get(0, ())
        if not s0:
            bad.append("%s seat 0 has an empty build list" % tag)
        for n in need:
            if n not in s0:
                bad.append("%s seat 0 list lacks building id %d" % (tag, n))
        for k in SEATED:
            if r["s"].get(k) != s0:
                bad.append(
                    "%s seat %d list (stock stride) differs from seat 0: %r vs %r"
                    % (tag, k, r["s"].get(k), s0)
                )
            if r["u"].get(k) != r["s"].get(k):
                bad.append(
                    "%s seat %d: the panel reads %r but the game wrote %r"
                    % (tag, k, r["u"].get(k), r["s"].get(k))
                )
        for k in range(4, 8):
            if r["u"].get(k) != r["s"].get(k):
                bad.append(
                    "%s unseated slot %d: the panel reads %r but the table holds %r"
                    % (tag, k, r["u"].get(k), r["s"].get(k))
                )
    return bad


def run(run_dir, need, expect_red):
    text = "\n".join(
        open(p, encoding="utf-8", errors="replace").read() for p in video_logs(run_dir)
    )
    recs = parse(text)
    for r in recs:
        print(
            "  %s side=%d ui_stride=%d s0=%s u2=%s s2=%s"
            % (r["label"], r["side"], r["stride"], r["s"].get(0), r["u"].get(2), r["s"].get(2))
        )
    bad = problems(recs, need)
    for b in bad[:12]:
        print("  check_avail_rows: " + b)
    if expect_red:
        ok = bool(bad) and any("panel" in b or "differs" in b for b in bad)
        print(
            "check_avail_rows --expect-red: %s (%d clause failure(s))"
            % ("RED as expected" if ok else "NOT RED", len(bad))
        )
        return 0 if ok else 1
    print("check_avail_rows: %s" % ("FAIL" if bad else "PASS"))
    return 1 if bad else 0


def selftest():
    good = (
        "; [avail] landed side=0 ui_stride=200 s0=1,2,3 s1=1,2,3 s2=1,2,3 s3=1,2,3 s4=- s5=- s6=- s7=- "
        "u0=1,2,3 u1=1,2,3 u2=1,2,3 u3=1,2,3 u4=- u5=- u6=- u7=-"
    )
    bad_stride = good.replace("ui_stride=200", "ui_stride=1016").replace("u2=1,2,3", "u2=46,47,48")
    fails = []

    def expect(what, ok):
        print("  [%s] %s" % ("ok" if ok else "FAIL", what))
        if not ok:
            fails.append(what)

    expect("a consistent table is clean", not problems(parse(good)))
    expect(
        "the id check fires on a missing id",
        any("lacks" in b for b in problems(parse(good), need=(9,))),
    )
    p = problems(parse(bad_stride))
    expect(
        "a 1016 stride is red (immediate + seat 2 read)",
        any("stride immediate" in b for b in p) and any("seat 2: the panel reads" in b for b in p),
    )
    expect(
        "a seat >= 2 with a different list is red",
        any("seat 2 list" in b for b in problems(parse(good.replace("s2=1,2,3", "s2=-")))),
    )
    expect("an unreached script is red", bool(problems(parse("nothing"))))
    expect(
        "pre_landing is only reported",
        not problems(
            parse(good) + parse(good.replace("landed", "pre_landing").replace("1,2,3", "-"))
        ),
    )
    print("check_avail_rows --selftest: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    need = ()
    if "--need" in argv:
        i = argv.index("--need")
        need = tuple(int(x) for x in argv[i + 1].split(",") if x)
        argv = argv[:i] + argv[i + 2 :]
    expect_red = "--expect-red" in argv
    args = [a for a in argv if not a.startswith("--")]
    if not args:
        print(__doc__)
        return 2
    return max(run(d, need, expect_red) for d in args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
