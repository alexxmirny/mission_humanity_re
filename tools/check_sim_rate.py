#!/usr/bin/env python3
"""check_sim_rate.py -- mp:P13: the sim rate of every peer of a match, read off mh_lockstep.log.

WHAT THIS IS FOR. mp:P13's acceptance is a RATE, and a rate eyeballed from a log is how the tracker
carried numbers nobody could re-derive. This is the checker the row names: for each peer's session
dir it reads the per-frame `mh_lockstep.log` table (net_lockstep.cpp; tools/data/log_formats.json
`net.lockstep_table`) and reports

  whole    d(clock_ms) / d(wall_ms) over the match AFTER a warm-up (default: 5000 ms of wall from
           the first row whose clock advanced -- the lobby/handoff rows before it are not the sim),
           up to the last row whose clock still advanced (the teardown tail, where the clock stops
           and the wall keeps going, is not a rate either);
  worst30  the lowest d(clock)/d(wall) over any 30 s wall window inside that span;
  mode8    rows whose `game` column reads 8 (the resync barrier's own frame) inside that span.

1.000x = the sim advanced exactly as fast as the wall clock. The thresholds are P13's done_when:
whole >= 0.95x and worst30 >= 0.85x on EVERY peer, 0 mode-8 rows. A span shorter than the window
is a FAIL (a too-short match cannot answer the worst-window clause), not a vacuous pass.

The same arithmetic as tmp/wave2_rig/rate.py, the ad-hoc script P13's first numbers came from
(2026-09-23), so those numbers stay comparable; this file is its tested, registered successor.

USAGE
  python tools/check_sim_rate.py <peer-run-dir> [<peer-run-dir> ...]   (post_check_peers)
  python tools/check_sim_rate.py --min-whole 0.95 --min-window 0.85 ... dirs
  python tools/check_sim_rate.py --selftest                          planted tables; negatives RED
"""

import argparse
import os
import shutil
import sys
import tempfile

WARM_MS = 5000.0
WINDOW_MS = 30000.0
LOG = "mh_lockstep.log"


def read_table(path):
    """[(wall_ms, clock_ms, game)] from one mh_lockstep.log; the header row names the columns."""
    hdr, rows = None, []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            if ln.startswith("#"):
                hdr = ln[1:].split()
                continue
            if not hdr:
                continue
            t = ln.split()
            try:
                iw, ic = hdr.index("wall_ms"), hdr.index("clock_ms")
            except ValueError:
                return []
            ig = hdr.index("game") if "game" in hdr else -1
            if len(t) <= max(iw, ic, ig):
                continue
            try:
                g = int(t[ig]) if ig >= 0 else 0
                rows.append((float(t[iw]), float(t[ic]), g))
            except ValueError:
                continue
    return rows


def measure(rows, warm_ms=WARM_MS, window_ms=WINDOW_MS):
    """{whole, worst, worst_at_s, span_s, game_s, mode8} or {error}."""
    if len(rows) < 3:
        return {"error": "fewer than 3 table rows"}
    w = [r[0] for r in rows]
    c = [r[1] for r in rows]
    i0 = next((i for i in range(1, len(c)) if c[i] > c[0]), None)
    if i0 is None:
        return {"error": "the clock never advanced"}
    t0 = w[i0] + warm_ms
    s = next((i for i in range(len(w)) if w[i] >= t0), None)
    if s is None:
        return {"error": "the table ends inside the %.0f ms warm-up" % warm_ms}
    e = len(w) - 1
    while e > s and c[e] == c[e - 1]:
        e -= 1
    if e <= s or w[e] <= w[s]:
        return {"error": "no clock advance after the warm-up"}
    span = w[e] - w[s]
    res = {
        "whole": (c[e] - c[s]) / span,
        "span_s": span / 1000.0,
        "game_s": (c[e] - c[s]) / 1000.0,
        "mode8": sum(1 for r in rows[s : e + 1] if r[2] == 8),
        "worst": None,
        "worst_at_s": None,
    }
    j = s
    for i in range(s, e + 1):
        while j <= e and w[j] - w[i] < window_ms:
            j += 1
        if j > e:
            break
        r = (c[j] - c[i]) / (w[j] - w[i])
        if res["worst"] is None or r < res["worst"]:
            res["worst"], res["worst_at_s"] = r, (w[i] - w[s]) / 1000.0
    return res


def check(dirs, min_whole, min_window, warm_ms=WARM_MS, window_ms=WINDOW_MS):
    """(fails, lines) over every peer dir."""
    fails, lines = [], []
    if not dirs:
        return ["no peer dirs given"], lines
    for d in dirs:
        name = os.path.basename(os.path.abspath(d).rstrip("\\/")) or d
        p = os.path.join(d, LOG)
        if not os.path.isfile(p):
            fails.append("%s: no %s" % (name, LOG))
            continue
        m = measure(read_table(p), warm_ms, window_ms)
        if "error" in m:
            fails.append("%s: %s" % (name, m["error"]))
            continue
        if m["worst"] is None:
            fails.append(
                "%s: span %.1f s after warm-up is shorter than the %.0f s window -- too short to judge"
                % (name, m["span_s"], window_ms / 1000.0)
            )
            continue
        lines.append(
            "%s: whole %.3fx, worst %.0f s %.3fx (at +%.0f s), span %.1f s wall / %.1f s game, "
            "mode-8 rows %d"
            % (
                name,
                m["whole"],
                window_ms / 1000.0,
                m["worst"],
                m["worst_at_s"],
                m["span_s"],
                m["game_s"],
                m["mode8"],
            )
        )
        if m["whole"] < min_whole:
            fails.append("%s: whole-match rate %.3fx < %.2fx" % (name, m["whole"], min_whole))
        if m["worst"] < min_window:
            fails.append("%s: worst-window rate %.3fx < %.2fx" % (name, m["worst"], min_window))
        if m["mode8"]:
            fails.append("%s: %d resync-barrier (game mode 8) rows" % (name, m["mode8"]))
    return fails, lines


HDR = "# wall_ms clock_ms total_ms sess game flags\n"


def plant(root, name, rows):
    d = os.path.join(root, name)
    os.makedirs(d)
    with open(os.path.join(d, LOG), "w", encoding="utf-8") as fh:
        fh.write(HDR)
        for w, c, g in rows:
            fh.write("%d %d %d 3 %d 0x00\n" % (w, c, c, g))
    return d


def table(seconds, rate=1.0, stall=None, mode8_at=None, lobby_s=3, tail_s=2):
    """100 ms rows: a lobby (clock 0), `seconds` of match at `rate`, a stall (start_s, len_s)
    where the clock stops, a teardown tail where it stops too."""
    rows, clock, t = [], 0.0, 1000000.0
    for _ in range(lobby_s * 10):
        rows.append((t, 0, 3))
        t += 100
    for k in range(seconds * 10):
        sec = k / 10.0
        stalled = stall and stall[0] <= sec < stall[0] + stall[1]
        if not stalled:
            clock += 100 * rate
        g = 8 if (mode8_at is not None and abs(sec - mode8_at) < 0.05) else 3
        rows.append((t, int(clock), g))
        t += 100
    for _ in range(tail_s * 10):
        rows.append((t, int(clock), 3))
        t += 100
    return rows


def selftest():
    cases = [
        # (label, per-peer tables, expect_pass)
        ("realtime pair", [table(45), table(45)], True),
        ("one peer at 0.90x whole", [table(45), table(45, rate=0.90)], False),
        (
            "a 6 s stall: whole ok-ish, worst 30 s under 0.85",
            [table(60), table(60, stall=(20, 6))],
            False,
        ),
        # the window clause on its OWN: 150 s with one 6 s stall is 0.96x whole but 0.80x in the
        # worst 30 s -- only the worst-window test can red this one
        (
            "a 6 s stall in a long match: whole passes, window reds",
            [table(150), table(150, stall=(70, 6))],
            False,
        ),
        ("a 1 s stall: still above both floors", [table(60), table(60, stall=(20, 1))], True),
        ("a mode-8 barrier row", [table(45), table(45, mode8_at=20)], False),
        ("too short for the window", [table(20), table(20)], False),
        ("teardown tail is not a rate", [table(45, tail_s=30), table(45, tail_s=30)], True),
    ]
    bad = 0
    root = tempfile.mkdtemp(prefix="check_sim_rate_")
    try:
        for i, (label, tabs, want) in enumerate(cases):
            dirs = [plant(root, "c%d_p%d" % (i, k), t) for k, t in enumerate(tabs)]
            fails, _ = check(dirs, 0.95, 0.85)
            ok = (not fails) == want
            bad += 0 if ok else 1
            print("  %s %s%s" % ("ok  " if ok else "BAD ", label, "" if ok else " -> %s" % fails))
        # a missing log is a FAIL, never a vacuous pass
        empty = os.path.join(root, "empty")
        os.makedirs(empty)
        fails, _ = check([empty], 0.95, 0.85)
        ok = bool(fails)
        bad += 0 if ok else 1
        print("  %s missing mh_lockstep.log is RED" % ("ok  " if ok else "BAD "))
    finally:
        shutil.rmtree(root, ignore_errors=True)
    n = len(cases) + 1
    print("check_sim_rate selftest: %d/%d" % (n - bad, n))
    return 0 if bad == 0 else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--min-whole", type=float, default=0.95)
    ap.add_argument("--min-window", type=float, default=0.85)
    ap.add_argument("--warm-ms", type=float, default=WARM_MS)
    ap.add_argument("--window-ms", type=float, default=WINDOW_MS)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("dirs", nargs="*")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    fails, lines = check(args.dirs, args.min_whole, args.min_window, args.warm_ms, args.window_ms)
    for ln in lines:
        print("  " + ln)
    if fails:
        print("check_sim_rate: FAIL")
        for f in fails:
            print("  " + f)
        return 1
    print(
        "check_sim_rate: PASS -- every peer >= %.2fx whole, >= %.2fx worst window, 0 barrier rows"
        % (args.min_whole, args.min_window)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
