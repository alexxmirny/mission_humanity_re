#!/usr/bin/env python3
"""check_cam_trace.py -- mp:SES3: read the camera half of the `[input] mouse_trace` line.

WHAT IT IS FOR. The stuck-strategic-scroll report (mp:SC2) does not reproduce on this machine, so
the evidence has to arrive in a player's log -- and a log nobody can read mechanically is a log
nobody reads. SES3 put the four `_G_LLM_CAM_EDGE_*_ACTIVE` flags, the four
`_G_LLM_CAM_SCROLL_*_HELD` latches and `_G_LLM_MAP_CAM_COL/ROW` on the existing `; [mtrace]` line;
this tool is the reader, and the registered `cam_edge_scroll` scenario's post-check.

THE THREE QUESTIONS IT ANSWERS, each of which a green scenario cannot:

  1. DID A LATCH STICK WHILE THE CURSOR WAS PINNED -- the H1 shape: consecutive samples with the
     same `cur=x,y`, an edge flag set, and the camera still moving. That is the stuck scroll as it
     looks from inside the game, and it is what the rig scenario reproduces deliberately by leaving
     the injected cursor at the screen edge (the harness never moves it back on its own, which is
     exactly what the VM clamp does to a real cursor -- the VM-input page, section 9c).
  2. DOES A NORMAL SCROLL RISE AND FALL -- an edge flag observed going clear->set and later
     set->clear. Without this clause a build whose latches were stuck ON from frame 1 would satisfy
     (1) vacuously.
  3. WHAT DOES THE TRACE COST -- printed lines per observed frame. SES3's contract is that the
     trace is cheap enough to ship on, i.e. well under one line per frame when nothing is changing;
     a regression in the sampling rule shows up here as a number, not as an opinion.

ABSENCE IS A FAILURE, the same rule check_module_bind.py carries: no run directory, no log, zero
`[mtrace]` lines (the knob was off, so the run proves nothing), or a `--expect-di` count that is not
exactly one, all REFUSE rather than pass quietly.

  python tools/check_cam_trace.py <run-dir|lane-dir>                    the report
  python tools/check_cam_trace.py <lane> --expect-pinned 3 --expect-rise-fall \
                                         --max-lines-per-frame 1.0 --expect-di
  python tools/check_cam_trace.py --selftest                            planted logs, every negative RED
"""

import argparse
import glob
import os
import re
import sys
import tempfile

UIDRIVE_LOG = "mh_uidrive.log"
NET_LOG = "mh_net.log"

# ; [mtrace] f=12 produced=2 depth=0 max=1 di=on last=3,4 cur=3,4 vis=1 edge=-R-- held=---- cam=7,9
#            camd=2 gest=00      <- gest is LMB then RMB gesture state (0 none / 1 begun / 2 active)
SAMPLE = re.compile(
    r"; \[mtrace\] f=(?P<f>\d+) produced=(?P<produced>\d+) depth=(?P<depth>\d+) max=(?P<max>\d+) "
    r"di=(?P<di>\w+) last=(?P<lx>-?\d+),(?P<ly>-?\d+) cur=(?P<cx>-?\d+),(?P<cy>-?\d+) "
    r"vis=(?P<vis>-?\d+) edge=(?P<edge>[LRUD-]{4}) held=(?P<held>[LRUD-]{4}) "
    r"cam=(?P<col>-?\d+),(?P<row>-?\d+) camd=(?P<camd>\d+) gest=(?P<lmb>\d)(?P<rmb>\d)"
)
QUIET = re.compile(r"; \[mtrace\] (?P<n>\d+) quiet frame\(s\)")
DI_KEYBOARD = re.compile(r"; \[input\] di_keyboard=(?P<live>\d+) dev=0x(?P<dev>[0-9a-f]{8})")


class Refusal(Exception):
    pass


def newest_run(path):
    """Accept a run folder or a LANE folder; return the run folder holding mh_uidrive.log."""
    if os.path.isfile(os.path.join(path, UIDRIVE_LOG)):
        return path
    runs = sorted(glob.glob(os.path.join(path, "logs", "*", UIDRIVE_LOG)), key=os.path.getmtime)
    if runs:
        return os.path.dirname(runs[-1])
    raise Refusal("no run directory with %s under %s" % (UIDRIVE_LOG, path))


def parse(run_dir):
    p = os.path.join(run_dir, UIDRIVE_LOG)
    if not os.path.isfile(p):
        raise Refusal("no %s in %s" % (UIDRIVE_LOG, run_dir))
    samples, quiet = [], 0
    with open(p, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = SAMPLE.search(line)
            if m:
                d = m.groupdict()
                samples.append(
                    {
                        "f": int(d["f"]),
                        "produced": int(d["produced"]),
                        "cur": (int(d["cx"]), int(d["cy"])),
                        "edge": d["edge"],
                        "held": d["held"],
                        "cam": (int(d["col"]), int(d["row"])),
                        "camd": int(d["camd"]),
                        "gest": (int(d["lmb"]), int(d["rmb"])),
                        "di": d["di"],
                    }
                )
                continue
            m = QUIET.search(line)
            if m:
                quiet += 1
    if not samples:
        raise Refusal(
            "zero [mtrace] sample lines in %s -- [input] mouse_trace was off, so this run says "
            "nothing about the camera latches" % p
        )
    return samples, quiet


def di_lines(run_dir):
    """Every `; [input] di_keyboard=` line in this run's mh_net.log (U25 step 1)."""
    p = os.path.join(run_dir, NET_LOG)
    if not os.path.isfile(p):
        raise Refusal("no %s in %s" % (NET_LOG, run_dir))
    out = []
    with open(p, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = DI_KEYBOARD.search(line)
            if m:
                out.append((int(m.group("live")), m.group("dev")))
    return out


def set_letters(mask):
    return {c for c in mask if c != "-"}


def analyse(samples, quiet_lines):
    """Derive the three answers. Returns a dict of measured facts; no judgment here."""
    frames = max(s["f"] for s in samples)
    lines = len(samples) + quiet_lines
    transitions = []
    prev = None
    for s in samples:
        cur = (s["edge"], s["held"])
        if prev is not None and cur != prev:
            transitions.append((s["f"], prev, cur))
        prev = cur
    # RISE AND FALL, per edge letter: clear -> set, then later set -> clear.
    risen, fallen = set(), set()
    prev_edge = None
    for s in samples:
        e = set_letters(s["edge"])
        if prev_edge is not None:
            risen |= e - prev_edge
            fallen |= (prev_edge - e) & risen
        prev_edge = e
    # THE PINNED RUN: consecutive samples at one cursor position with an edge flag set.
    best = {"n": 0, "cam": 0, "cur": None, "from": 0, "to": 0, "edge": "", "gest": set()}
    run = None
    for s in samples:
        pinned = bool(set_letters(s["edge"]))
        if run and pinned and s["cur"] == run["cur"]:
            run["n"] += 1
            run["cam"] += s["camd"]
            run["to"] = s["f"]
            run["edge"] = s["edge"]
            # WHICH GATE was open during the run, carried rather than judged: a latch held while
            # gest=00 is a cursor really at the edge; one held while a gesture byte is 1 is the
            # frozen-clear path, and only the log can tell a reader which happened.
            run["gest"].add(s["gest"])
        elif pinned:
            run = {
                "n": 1,
                "cam": 0,
                "cur": s["cur"],
                "from": s["f"],
                "to": s["f"],
                "edge": s["edge"],
                "gest": {s["gest"]},
            }
        else:
            run = None
        if run and (run["n"], run["cam"]) > (best["n"], best["cam"]):
            best = dict(run)
    return {
        "frames": frames,
        "samples": len(samples),
        "quiet_lines": quiet_lines,
        "lines": lines,
        "lines_per_frame": lines / float(frames) if frames else 0.0,
        "transitions": transitions,
        "risen": risen,
        "fallen": fallen,
        "pinned": best,
    }


def held_run(samples, letter):
    """mp:SES3c -- the KEYBOARD counterpart of `analyse()`'s pinned-edge run, for `--expect-held`.

    The mouse-edge clause (`--expect-pinned`) also requires the camera to be MOVING and the cursor
    to sit at one fixed position -- neither applies to a keyboard hold, whose only claim is "the
    HELD bit for this letter stayed set across >= N consecutive samples, and later cleared". So this
    does not reuse `analyse()`'s `pinned` run at all; it is its own, narrower measurement over the
    `held=` column alone.

    The done_when's "with cam moving" clause IS checked (Wave 2 fix): `cam` sums the run's camd
    column, and the caller requires it > 0 -- a latch that is held but scrolls nothing is the
    latch-with-no-effect shape the mouse-edge arm already refuses.

    Returns (best_run, cleared): `best_run` is {"n", "from", "to", "cam"} for the longest consecutive
    stretch of samples with `letter` in the held set (n=0 if it was never set at all); `cleared` is
    True iff some sample AFTER that stretch ends has `letter` absent from the held set (the release).
    """
    best = {"n": 0, "from": 0, "to": 0, "cam": 0}
    run = None
    end_i = None
    for i, s in enumerate(samples):
        is_set = letter in set_letters(s["held"])
        if run and is_set:
            run["n"] += 1
            run["to"] = s["f"]
            run["to_i"] = i
            run["cam"] += s["camd"]
        elif is_set:
            run = {"n": 1, "from": s["f"], "to": s["f"], "to_i": i, "cam": s["camd"]}
        else:
            run = None
        if run and run["n"] > best["n"]:
            best = {"n": run["n"], "from": run["from"], "to": run["to"], "cam": run["cam"]}
            end_i = run["to_i"]
    cleared = False
    if end_i is not None:
        cleared = any(letter not in set_letters(s["held"]) for s in samples[end_i + 1 :])
    return best, cleared


def report(a, di):
    out = []
    out.append(
        "  cam-trace: %d [mtrace] lines over %d observed frames (%.4f lines/frame; %d sample, "
        "%d quiet-summary)"
        % (a["lines"], a["frames"], a["lines_per_frame"], a["samples"], a["quiet_lines"])
    )
    out.append(
        "  latch transitions: %d; edge flags that rose: %s; that later fell: %s"
        % (
            len(a["transitions"]),
            "".join(sorted(a["risen"])) or "(none)",
            "".join(sorted(a["fallen"])) or "(none)",
        )
    )
    for f, (pe, ph), (ne, nh) in a["transitions"][:12]:
        out.append("    f=%-6d edge %s -> %s   held %s -> %s" % (f, pe, ne, ph, nh))
    if len(a["transitions"]) > 12:
        out.append("    ... %d more" % (len(a["transitions"]) - 12))
    p = a["pinned"]
    if p["n"]:
        out.append(
            "  longest EDGE-LATCHED-WHILE-PINNED run: %d samples, f=%d..%d, cursor %s, edge=%s, "
            "camera moved %d tile(s) during it; gesture state(s) seen: %s"
            % (
                p["n"],
                p["from"],
                p["to"],
                p["cur"],
                p["edge"],
                p["cam"],
                ", ".join("%d%d" % g for g in sorted(p["gest"])) or "(none)",
            )
        )
    else:
        out.append("  no sample had an edge flag set")
    if di is not None:
        out.append(
            "  di_keyboard lines in mh_net.log: %d %s"
            % (len(di), ["(live=%d dev=0x%s)" % d for d in di])
        )
    return out


def check(run_dir, args):
    samples, quiet = parse(run_dir)
    a = analyse(samples, quiet)
    di = di_lines(run_dir) if args.expect_di else None
    lines = report(a, di)
    fails = []
    if args.expect_pinned:
        p = a["pinned"]
        if p["n"] < args.expect_pinned:
            fails.append(
                "expected an edge latch held over >= %d consecutive samples at one cursor "
                "position; longest was %d" % (args.expect_pinned, p["n"])
            )
        elif p["cam"] <= 0:
            fails.append(
                "the latch was held for %d samples at cursor %s but the camera never moved -- "
                "that is a latch with no effect, not the stuck-scroll shape" % (p["n"], p["cur"])
            )
    if args.expect_rise_fall and not a["fallen"]:
        fails.append(
            "no edge flag was observed rising and then falling -- a latch that is never cleared "
            "in a normal scroll would make the pinned clause vacuous"
        )
    if args.max_lines_per_frame is not None and a["lines_per_frame"] > args.max_lines_per_frame:
        fails.append(
            "trace cost %.4f lines/frame, over the %.4f budget"
            % (a["lines_per_frame"], args.max_lines_per_frame)
        )
    if args.expect_di and len(di) != 1:
        fails.append(
            "expected exactly one `; [input] di_keyboard=` line per run, found %d" % len(di)
        )
    if getattr(args, "expect_held", None):
        letter = args.expect_held
        held_min = getattr(args, "expect_held_min", 3) or 3
        run, cleared = held_run(samples, letter)
        lines.append(
            "  longest %s-HELD run: %d samples, f=%d..%d, camd sum %d, cleared after: %s"
            % (letter, run["n"], run["from"], run["to"], run["cam"], cleared)
        )
        if run["n"] < held_min:
            fails.append(
                "expected the %s held latch set over >= %d consecutive samples; longest was %d"
                % (letter, held_min, run["n"])
            )
        elif run["cam"] <= 0:
            fails.append(
                "the %s held latch was set for %d samples but the camera never moved (camd sum 0)"
                % (letter, run["n"])
            )
        elif not cleared:
            fails.append(
                "the %s held latch was set for %d samples (f=%d..%d) and never observed clearing "
                "afterwards -- keyhold's UP either did not fire or did not reach the latch"
                % (letter, run["n"], run["from"], run["to"])
            )
    for ln in lines:
        print(ln)
    for f in fails:
        print("  cam-trace FAILED: %s" % f)
    print("check_cam_trace: %s" % ("PASS" if not fails else "FAIL"))
    return 0 if not fails else 1


# ---- selftest: plant a log per outcome and require the negatives to go RED ----------------------

SAMPLE_FMT = (
    "[  1.000] ; [mtrace] f=%d produced=%d depth=0 max=0 di=on last=%d,%d cur=%d,%d vis=1 "
    "edge=%s held=%s cam=%d,%d camd=%d gest=00\n"
)


def _write(dirpath, rows, di="; [input] di_keyboard=1 dev=0x0066155c -- DirectInput keyboard live"):
    os.makedirs(dirpath, exist_ok=True)
    with open(os.path.join(dirpath, UIDRIVE_LOG), "w", encoding="utf-8") as fh:
        fh.write("".join(rows))
    with open(os.path.join(dirpath, NET_LOG), "w", encoding="utf-8") as fh:
        fh.write((di + "\n") if di else "")


def _rows(seq):
    """seq of (f, cur, edge, camd) -> log rows."""
    out = []
    for f, cur, edge, camd in seq:
        out.append(
            SAMPLE_FMT % (f, 0, cur[0], cur[1], cur[0], cur[1], edge, "----", 40 + camd, 10, camd)
        )
    return out


def _rows_held(seq):
    """seq of (f, cur, held, camd) -> log rows, edge fixed clear -- exercises held= alone (SES3c)."""
    out = []
    for f, cur, held, camd in seq:
        out.append(
            SAMPLE_FMT % (f, 0, cur[0], cur[1], cur[0], cur[1], "----", held, 40 + camd, 10, camd)
        )
    return out


class _A(object):
    expect_pinned = 3
    expect_rise_fall = True
    max_lines_per_frame = 1.0
    expect_di = True
    expect_held = None
    expect_held_min = 3


class _K(object):
    """mp:SES3c -- the `--expect-held L --expect-held-min 3` arm, isolated from the mouse-edge
    clauses (which a held-only log, all edge=----, would otherwise fail vacuously)."""

    expect_pinned = 0
    expect_rise_fall = False
    max_lines_per_frame = None
    expect_di = False
    expect_held = "L"
    expect_held_min = 3


def selftest():
    good = [(1, (320, 240), "----", 0), (2, (639, 240), "-R--", 0)]
    good += [(10 + i, (639, 240), "-R--", 1) for i in range(5)]
    good += [(20, (320, 240), "----", 0), (21, (320, 240), "----", 0)]
    cases = [
        ("the shape the scenario produces", good, 0),
        (
            "latched but the camera never moves (a latch with no effect)",
            [(1, (320, 240), "----", 0)]
            + [(10 + i, (639, 240), "-R--", 0) for i in range(5)]
            + [(20, (320, 240), "----", 0)],
            1,
        ),
        (
            "never pinned long enough",
            [(1, (320, 240), "----", 0), (2, (639, 240), "-R--", 1), (3, (320, 240), "----", 0)],
            1,
        ),
        (
            "latched from the first sample and never cleared (no rise/fall)",
            [(i + 1, (639, 240), "-R--", 1) for i in range(6)],
            1,
        ),
    ]
    rc = 0
    with tempfile.TemporaryDirectory() as td:
        for i, (label, seq, want) in enumerate(cases):
            d = os.path.join(td, "case%d" % i)
            _write(d, _rows(seq))
            got = check(d, _A())
            ok = got == want
            rc |= 0 if ok else 1
            print(
                "  selftest %-58s want=%d got=%d %s"
                % (label, want, got, "ok" if ok else "MISMATCH")
            )
        # the cost clause, on its own: 40 lines over 40 frames is 1.0, and the budget is exclusive
        d = os.path.join(td, "cost")
        _write(d, _rows([(i + 1, (320, 240), "----", 0) for i in range(40)]))

        class _C(_A):
            expect_pinned = 0
            expect_rise_fall = False
            max_lines_per_frame = 0.5

        got = check(d, _C())
        ok = got == 1
        rc |= 0 if ok else 1
        print(
            "  selftest %-58s want=1 got=%d %s"
            % ("over the lines/frame budget", got, "ok" if ok else "MISMATCH")
        )
        # absence: a log with no [mtrace] lines at all must REFUSE, not pass
        d = os.path.join(td, "empty")
        _write(d, ["[  0.000] ; uidrive enabled=0\n"])
        try:
            check(d, _A())
            print("  selftest %-58s MISMATCH (no refusal)" % "mouse_trace off")
            rc |= 1
        except Refusal:
            print("  selftest %-58s ok" % "mouse_trace off refuses")
        # the DI line must be exactly one
        d = os.path.join(td, "di")
        _write(d, _rows(good), di=None)
        got = check(d, _A())
        ok = got == 1
        rc |= 0 if ok else 1
        print(
            "  selftest %-58s want=1 got=%d %s"
            % ("missing di_keyboard line", got, "ok" if ok else "MISMATCH")
        )
        # mp:SES3c -- `--expect-held`, the keyhold scenario's own clause, isolated from every
        # mouse-edge one (di=on is still asserted by SAMPLE_FMT/parse(); di_lines() is only
        # consulted when --expect-di is set, which _K leaves off).
        held_cases = [
            ("L held exactly 3 samples then clears", [("L", 3, True)], 0),
            ("L held only 2 samples then clears -- under the min", [("L", 2, True)], 1),
            ("L held 3 samples and never clears", [("L", 3, False)], 1),
            ("L never held at all", [("L", 0, False)], 1),
            ("L held 3 samples, clears, camera never moves", [("L", 3, True, 0)], 1),
        ]
        for hi, (label, spec, want) in enumerate(held_cases):
            letter, n, clears = spec[0][:3]
            camd = spec[0][3] if len(spec[0]) > 3 else 1
            seq = [(i + 1, (320, 240), letter + "---", camd) for i in range(n)]
            if clears:
                seq.append((n + 1, (320, 240), "----", 0))
            if not seq:
                seq = [(1, (320, 240), "----", 0)]
            d = os.path.join(td, "held_%d" % hi)
            _write(d, _rows_held(seq))
            got = check(d, _K())
            ok = got == want
            rc |= 0 if ok else 1
            print(
                "  selftest %-58s want=%d got=%d %s"
                % (label, want, got, "ok" if ok else "MISMATCH")
            )
    print("check_cam_trace --selftest: %s" % ("PASS" if rc == 0 else "FAIL"))
    return rc


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "run_dir", nargs="?", help="a run folder (…/logs/<stamp>_<role>) or a LANE folder"
    )
    ap.add_argument(
        "--expect-pinned",
        type=int,
        default=0,
        metavar="N",
        help="require an edge latch held over >= N consecutive samples at ONE cursor position, "
        "with the camera moving during it (the stuck-scroll shape)",
    )
    ap.add_argument(
        "--expect-rise-fall",
        action="store_true",
        help="require some edge flag to rise and later fall (a normal scroll), so the pinned "
        "clause cannot pass on latches that were simply never cleared",
    )
    ap.add_argument(
        "--max-lines-per-frame",
        type=float,
        default=None,
        metavar="X",
        help="require the trace to cost at most X printed lines per observed frame",
    )
    ap.add_argument(
        "--expect-di",
        action="store_true",
        help="require exactly one `; [input] di_keyboard=` line in this run's mh_net.log (U25)",
    )
    ap.add_argument(
        "--expect-held",
        default=None,
        metavar="L|R|U|D",
        help="mp:SES3c: require the keyboard HELD latch for this letter set over >= "
        "--expect-held-min consecutive samples with the camera moving, then cleared",
    )
    ap.add_argument(
        "--expect-held-min",
        type=int,
        default=3,
        metavar="N",
        help="--expect-held: minimum consecutive samples (default 3)",
    )
    ap.add_argument("--selftest", action="store_true", help="planted logs: every negative goes RED")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.run_dir:
        ap.error("give a run or lane directory (or --selftest)")
    try:
        return check(newest_run(args.run_dir), args)
    except Refusal as e:
        print("check_cam_trace REFUSED: %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())
