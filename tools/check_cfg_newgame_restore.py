#!/usr/bin/env python3
"""check_cfg_newgame_restore.py -- mods:G17: did a New Game start from the BOOT parse of the cfg tables?

mh.dll captures the seven baked cfg tables (Unit, Building, Weapon, System, Planets, Projects,
Upgrades) at the first idle main-menu frame and, at the top of llm_strat_new_game_init, logs

  ; G17: new-game cfg tables restored (before/after, bytes differing from boot): unit=N/M building=N/M ...

(`KEPT ([net] cfg_newgame_restore_fix=0)` instead of `restored` with the fix off). `before` is the
number of bytes that differed from the boot capture when the new game began, `after` the number left
once the restore ran. This reads the LAST such line of each run directory handed over.

  check_cfg_newgame_restore.py <run-dir>...                 after a Load: every table differed before
                                                            (the fixture save changed one byte in each)
                                                            and NONE differs after
  check_cfg_newgame_restore.py --fresh <run-dir>...         a fresh process (no Load): every table
                                                            0/0 -- the fix writes nothing
  check_cfg_newgame_restore.py --peers-equal --fresh-first 1 <host-dir> <client-dir>
                                                            2-peer rows: the host never loaded a save
                                                            (0/0), the joiner did (N/0), and both end
                                                            on the same after-values (all zero)
  check_cfg_newgame_restore.py --selftest

ABSENCE IS A FAILURE: no mh_launch.log, no capture line, or no new-game line is RED, never a pass.
The fix-off twin (`[net] cfg_newgame_restore_fix=0`) runs this same default check and is RED by
construction (after == before), which the registry row declares as expect_red.
"""

import argparse
import glob
import os
import re
import sys
import tempfile

TABLES = ["unit", "building", "weapon", "system", "planets", "projects", "upgrades"]
LINE_RE = re.compile(r"; G17: new-game cfg tables (restored|KEPT)[^:]*:(.*)$")
CAP_RE = re.compile(r"; G17: cfg tables captured:(.*)$")


def logs_in(path):
    if os.path.isfile(path):
        return [path]
    found = glob.glob(os.path.join(path, "**", "mh_launch.log"), recursive=True)
    return sorted(found)


def menu_siblings(path):
    """The capture line is written by the PROCESS (menu) run dir; a session dir's lines are its siblings'."""
    parent = os.path.dirname(os.path.abspath(path))
    return sorted(glob.glob(os.path.join(parent, "*menu*", "mh_launch.log")))


def parse(path):
    """(captured_fnv_by_table, [(verdict, {table: (before, after)})])"""
    cap, lines = {}, []
    own = set(logs_in(path))
    for lp in logs_in(path) + menu_siblings(path):
        with open(lp, encoding="utf-8", errors="replace") as f:
            for ln in f:
                m = CAP_RE.search(ln)
                if m:
                    for t, h in re.findall(r"(\w+)=\d+ B fnv=([0-9a-f]+)", m.group(1)):
                        cap[t] = h
                    continue
                m = LINE_RE.search(ln)
                if m and lp in own:
                    vals = {
                        t: (int(a), int(b))
                        for t, a, b in re.findall(r"(\w+)=(\d+)/(\d+)", m.group(2))
                    }
                    lines.append((m.group(1), vals))
    return cap, lines


def pick(lines, nth):
    """The nth new-game line (1-based; negative counts from the end, -1 = the LAST, the default), or None when
    the run has fewer. A multi-segment walk (solo_menu_walk) starts several new games in one process, each
    logging its own `; G17:` line into the one mh_launch.log (no timestamp, so no `mark` can slice it); the
    segment's claim is made about the line its own game start wrote, counted in the walk's fixed order."""
    try:
        return lines[nth - 1] if nth > 0 else lines[nth]
    except IndexError:
        return None


def problems(path, fresh, mp=False, nth=-1):
    cap, lines = parse(path)
    out = []
    if not logs_in(path):
        return ["%s: no mh_launch.log" % path]
    if set(cap) != set(TABLES):
        out.append("%s: capture line missing/incomplete (%s)" % (path, sorted(cap)))
    if not lines:
        out.append(
            "%s: no '; G17: new-game cfg tables' line -- the new game never reached the restore"
            % path
        )
        return out
    got = pick(lines, nth)
    if got is None:
        out.append("%s: only %d new-game line(s), want line %d" % (path, len(lines), nth))
        return out
    verdict, vals = got
    if set(vals) != set(TABLES):
        out.append("%s: restore line lacks tables: %s" % (path, sorted(set(TABLES) - set(vals))))
        return out
    for t in TABLES:
        b, a = vals[t]
        if fresh:
            if t == "planets" and mp:
                # an MP start writes Planets[31] (the scenario planet) before session begin; only `after` must be 0
                if a != 0:
                    out.append("%s: planets still differs after the restore (%d)" % (path, a))
            elif (b, a) != (0, 0):
                out.append(
                    "%s: fresh process, %s=%d/%d (want 0/0: the fix must write nothing)"
                    % (path, t, b, a)
                )
        else:
            if b == 0:
                out.append(
                    "%s: %s differed 0 bytes before -- the Load never changed it (fixture not in effect)"
                    % (path, t)
                )
            if a != 0:
                out.append(
                    "%s: %s still differs from the boot parse by %d bytes after the new game (%s)"
                    % (path, t, a, verdict)
                )
    return out


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="*")
    ap.add_argument("--fresh", action="store_true")
    ap.add_argument("--peers-equal", action="store_true")
    ap.add_argument(
        "--mp", action="store_true", help="MP start: Planets[31] may differ BEFORE the restore"
    )
    ap.add_argument(
        "--fresh-idx",
        type=int,
        action="append",
        default=[],
        help="dir index (0=host) that never loaded a save",
    )
    ap.add_argument(
        "--fresh-first", type=int, default=0, help="the first N dirs (the host) never loaded a save"
    )
    ap.add_argument(
        "--nth",
        type=int,
        default=-1,
        help="which `; G17:` line to judge: 1-based from the start, or negative from the end (-1 = the last, "
        "the default). A walk that starts several games counts them in its fixed order",
    )
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not a.dirs:
        print("check_cfg_newgame_restore: no run directory given")
        return 2
    bad = []
    for i, d in enumerate(a.dirs):
        bad += problems(d, a.fresh or i < a.fresh_first or i in a.fresh_idx, a.mp, a.nth)
    if a.peers_equal and not bad:
        afters = [tuple(parse(d)[1][-1][1][t][1] for t in TABLES) for d in a.dirs]
        if len(set(afters)) != 1:
            bad.append("peers disagree on the after-values: %s" % afters)
    for p in bad:
        print("RED: " + p)
    if not bad:
        for d in a.dirs:
            v, vals = pick(parse(d)[1], a.nth)
            print("ok %s: %s %s" % (d, v, " ".join("%s=%d/%d" % (t, *vals[t]) for t in TABLES)))
    return 1 if bad else 0


def selftest():
    def mk(text):
        d = tempfile.mkdtemp()
        open(os.path.join(d, "mh_launch.log"), "w").write(text)
        return d

    cap = "; G17: cfg tables captured: " + " ".join("%s=1 B fnv=ab" % t for t in TABLES) + "\n"

    def ln(kind, b, a):
        return (
            "; G17: new-game cfg tables %s (before/after, bytes differing from boot): " % kind
            + " ".join("%s=%d/%d" % (t, b, a) for t in TABLES)
            + "\n"
        )

    ok = True

    def expect(name, text, fresh, want_bad):
        nonlocal ok
        got = bool(problems(mk(text), fresh))
        if got != want_bad:
            print("selftest FAIL: " + name)
            ok = False

    expect("restored after load", cap + ln("restored", 5, 0), False, False)
    expect(
        "kept (fix off) is red",
        cap + ln("KEPT ([net] cfg_newgame_restore_fix=0)", 5, 5),
        False,
        True,
    )
    expect("load never changed tables", cap + ln("restored", 0, 0), False, True)
    expect("fresh 0/0", cap + ln("restored", 0, 0), True, False)
    expect("fresh dirty", cap + ln("restored", 3, 0), True, True)
    expect("no restore line", cap, False, True)
    expect("no capture", ln("restored", 5, 0), False, True)
    # --nth: the first line is a fresh 0/0, the second a loaded 5/0; each judged on its own line
    two = cap + ln("restored", 0, 0) + ln("restored", 5, 0)
    if problems(mk(two), True, False, 1) or problems(mk(two), False, False, 2):
        print("selftest FAIL: --nth picks the wrong line")
        ok = False
    if not problems(mk(two), True, False, -1) or not problems(mk(two), False, False, 1):
        print("selftest FAIL: --nth must judge the line it was given")
        ok = False
    if not problems(mk(two), False, False, 3):
        print("selftest FAIL: a missing nth line must be red")
        ok = False
    print("selftest: %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
