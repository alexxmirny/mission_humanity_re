#!/usr/bin/env python3
"""tooling:TL-SUITE-LOADRED (b) -- refuse clock-only synchronization in a UI scenario.

TL-P9W-TRIGGER is the motivating case: `resync_receiver_deadline_proof`'s shim blackholes at a fixed
t+50s, "~97% odds" of landing inside an active barrier -- and on 2026-09-25 it did not, 4/4 standalone
runs FAIL with the receiver-side deadline never exercised. A wall-clock GUESS about where a peer will
be is not synchronization; it is a coin flip that gets more expensive to lose as the box gets busier
(TL-HARN19/TL-GATE-LOADFLAKE-0925: contention moves the odds).

Two things are scanned, both against a vocabulary read from the DLL's own interpreter
(`src/mh_dll/mh/seams/ui_drive.cpp`'s `parse_line`, mirrored in KNOWN_WAIT_OPS/KNOWN_ACTION_OPS below
-- re-grep `strcmp(op, "` there if this drifts) and from `tools/ui_registry.py`'s SCHEMA:

1. **tools/uiscripts/*.txt scripts.** Every non-comment line's first token must be a known WAIT or
   ACTION op. The interpreter has NO wall-clock or frame-count WAIT at all (confirmed by the same
   grep: no `sleep`/`wait`/`delay` case exists), so this is a vocabulary allowlist, not a semantic
   read of each script -- an unrecognized op is refused by name (a typo, a retired directive, or a
   deliberate attempt to add a clock-only wait outside the interpreter's own grammar), and a small
   DENYLIST of clock/sleep synonyms is refused with a specific message even though none currently
   parse, so the day one is added to the interpreter without a matching grammar review here, this
   lint is what catches the FIRST script that uses it rather than the fiftieth.
2. **registry.yaml `tests` rows.** A row with `shim_timeline` (a net_shim wall-clock schedule of
   blackhole/stall/delay commands) and NO `shim_triggers` (the evidence-bound log-line gate) has no
   state-based synchronization for its scheduled action at all -- the timeline IS the only sync, which
   is exactly TL-P9W-TRIGGER's shape. Refused unless the row is on `tools/data/ui_sync_lint_allow.json`
   (row + one-line reason + the tracker id that explains it, no expiry -- unlike load_red_allow.json
   this is a design fact about the row, not a transient load flake with a shelf life).

    python tools/lint_ui_sync.py             # scan the committed scripts + registry
    python tools/lint_ui_sync.py --selftest  # planted negatives (scripts and registry rows)
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS_GLOB = os.path.join(REPO, "tools", "uiscripts", "*.txt")
ALLOW_PATH = os.path.join(REPO, "tools", "data", "ui_sync_lint_allow.json")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_registry  # noqa: E402

# ---- the grammar (mirrored from src/mh_dll/mh/seams/ui_drive.cpp's parse_line, 2026-09-26) --------
# WAIT ops: state predicates that BLOCK until true. `gameclock` is here deliberately -- it reads the
# SIM clock (a deterministic quantity that only advances when steps are simulated: a horizon-starved
# peer never reaches it and the script times out, which is the correct failure), never the wall clock.
KNOWN_WAIT_OPS = frozenset(
    {
        "screen",
        "present",
        "absent",
        "onscreen",
        "screensettled",
        "settled",
        "enabled",
        "disabled",
        "hovered",
        "gamemode",
        "tactsel",
        "sessions",
        "peers",
        "occ",
        "race",
        "retryready",
        "lobbygen",
        "lobbyping",
        "awaitsignal",
        "gameclock",
        "stalled",
        "simstep",
        "field",
        "wmsg",
    }
)
# ACTION ops: fire once, never a synchronization mechanism themselves (a `cursorhold`/`keyhold`
# <frames> argument is a DWELL duration for the injected input, not a wait for anything to become
# true: the DLL holds the input down for that many frames, then releases it).
KNOWN_ACTION_OPS = frozenset(
    {
        "signal",
        "clickv",
        "clickl",
        "clicki",
        "cursor",
        "click",
        "press",
        "release",
        "rclick",
        "simclick",
        "cursorhold",
        "key",
        "keyhold",
        "hotkey",
        "type",
        "layout",
        "keyjournal",
        "capture",
        "log",
        "pokerace",
        "injectstart",
        "dump",
        "end",
    }
)
KNOWN_OPS = KNOWN_WAIT_OPS | KNOWN_ACTION_OPS
# Named explicitly so a refusal on one of these reads as "clock-only sync", not a generic typo --
# none of these parse today (grep confirms it), but a script author reaching for exactly one of these
# words is reaching for a wall-clock wait, and the message should say so.
CLOCK_DENYLIST = frozenset(
    {"sleep", "wait", "delay", "pause", "waitframes", "waitms", "waitsecs", "waitfor"}
)


def scan_scripts(paths=None):
    """[(path, line_no, op, message)] for every disallowed op. `paths` defaults to the committed
    tools/uiscripts/*.txt; a selftest passes a synthetic file list instead."""
    problems = []
    for path in sorted(paths if paths is not None else glob.glob(SCRIPTS_GLOB)):
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                lines = fh.readlines()
        except OSError:
            continue
        for i, raw in enumerate(lines, 1):
            line = raw.strip()
            if not line or line[0] in "#;":
                continue
            op = line.split(None, 1)[0]
            # `retry <game-ms> <back> <tries> <WAIT ...>` wraps a wait (TL-SUITE-SPLICE-HOSTCLICK):
            # its three numbers are sim-time/step counts, so judge the wrapped op, which must be a WAIT.
            toks = line.split()
            if op == "retry" and len(toks) >= 5 and toks[4] in KNOWN_WAIT_OPS:
                if all(t.lstrip("-").isdigit() for t in toks[1:4]):
                    continue
            if op in KNOWN_OPS:
                continue
            try:
                rel = os.path.relpath(path, REPO)
            except ValueError:
                rel = path  # a selftest's tempdir may sit on a different drive than REPO on Windows
            if op in CLOCK_DENYLIST:
                problems.append(
                    (
                        rel,
                        i,
                        op,
                        "%r is a clock-only sync primitive, not part of the grammar -- gate on a "
                        "state predicate (settled/gameclock/...) or a simstep fence instead" % op,
                    )
                )
            else:
                problems.append(
                    (
                        rel,
                        i,
                        op,
                        "%r is not a known WAIT/ACTION op (typo, or KNOWN_OPS is stale)" % op,
                    )
                )
    return problems


def load_allow(path=ALLOW_PATH):
    try:
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return []


def allow_problems(entries):
    probs = []
    if not isinstance(entries, list):
        return ["top level must be a list"]
    seen = set()
    for i, e in enumerate(entries):
        where = "entry #%d" % i
        if not isinstance(e, dict):
            probs.append("%s: not a mapping" % where)
            continue
        where = "entry #%d (row %r)" % (i, e.get("row"))
        missing = [k for k in ("row", "reason", "tracker_id") if k not in e]
        if missing:
            probs.append("%s: missing key(s) %s" % (where, missing))
            continue
        unknown = sorted(set(e) - {"row", "reason", "tracker_id"})
        if unknown:
            probs.append("%s: unknown key(s) %s" % (where, unknown))
        if e["row"] in seen:
            probs.append("%s: duplicate row" % where)
        seen.add(e["row"])
    return probs


def scan_registry(tests, allow_entries=None):
    """[(name, message)] for a `shim_timeline` row with no `shim_triggers`, not on the allow-list."""
    allow_entries = load_allow() if allow_entries is None else allow_entries
    allowed = {e["row"]: e for e in allow_entries if isinstance(e, dict) and "row" in e}
    problems = []
    for t in tests:
        if not t.get("shim_timeline") or t.get("shim_triggers"):
            continue
        if t["name"] in allowed:
            continue
        problems.append(
            (
                t["name"],
                "shim_timeline=%r schedules a blackhole/stall on a WALL CLOCK with no shim_triggers "
                "log-line gate -- the timeline is the ONLY synchronization for a peer-dependent event "
                "(TL-P9W-TRIGGER's shape). Add a shim_trigger on the evidence the action actually "
                "needs, or add a tools/data/ui_sync_lint_allow.json entry with the reason this row is "
                "not peer-dependent." % t["shim_timeline"],
            )
        )
    return problems


def run():
    """(script_problems, allow_problems, registry_problems) against the committed tree."""
    allow_entries = load_allow()
    a_probs = allow_problems(allow_entries)
    s_probs = scan_scripts()
    regs = ui_registry.load()
    r_probs = scan_registry(regs["tests"], allow_entries if not a_probs else [])
    return s_probs, a_probs, r_probs


# ---- selftest -------------------------------------------------------------------------------------


def _write(tmp_dir, name, text):
    p = os.path.join(tmp_dir, name)
    with open(p, "w", encoding="utf-8") as fh:
        fh.write(text)
    return p


def selftest():
    import tempfile

    ok = True

    def check(label, cond, detail=""):
        nonlocal ok
        print(
            "  %s  %s%s"
            % ("ok  " if cond else "FAIL", label, ("  -- %s" % detail) if detail else "")
        )
        ok = ok and cond

    with tempfile.TemporaryDirectory(prefix="lint_ui_sync_selftest_") as tmp:
        clean = _write(tmp, "clean.txt", "# a comment\nsettled Ok\nclickl Ok\ncapture m1\nend\n")
        probs = scan_scripts([clean])
        check("a clean script (state predicates only) passes", not probs)

        sleepy = _write(tmp, "sleepy.txt", "settled Ok\nsleep 5000\ncapture m1\n")
        probs = scan_scripts([sleepy])
        check(
            "a planted `sleep` step is refused as clock-only sync",
            any(op == "sleep" and "clock-only" in msg for _, _, op, msg in probs),
        )

        waitframes = _write(tmp, "waitn.txt", "waitframes 600\ncapture m1\n")
        probs = scan_scripts([waitframes])
        check(
            "a planted `waitframes` step is refused as clock-only sync",
            any(op == "waitframes" and "clock-only" in msg for _, _, op, msg in probs),
        )

        typo = _write(tmp, "typo.txt", "setled Ok\ncapture m1\n")
        probs = scan_scripts([typo])
        check(
            "an unknown op is refused (but not mislabelled clock-only)",
            any(op == "setled" and "clock-only" not in msg for _, _, op, msg in probs),
        )

        comments = _write(
            tmp, "comments.txt", "# sleep 5000  <- inside a comment, must not fire\n; also\n"
        )
        probs = scan_scripts([comments])
        check("a comment line is never parsed as a step", not probs)

        retry_ok = _write(tmp, "retry_ok.txt", "press 1 1\nretry 1500 1 3 settled value:100\n")
        check("`retry` wrapping a WAIT passes", not scan_scripts([retry_ok]))
        retry_bad = _write(tmp, "retry_bad.txt", "press 1 1\nretry 1500 1 3 clickv 100\n")
        check(
            "`retry` wrapping an ACTION is refused",
            any(op == "retry" for _, _, op, _ in scan_scripts([retry_bad])),
        )

    # registry rows: a fake `tests` list, bypassing ui_registry entirely (this is a pure function).
    no_trigger = [{"name": "r1", "shim_timeline": "x.txt"}]
    probs = scan_registry(no_trigger, allow_entries=[])
    check(
        "shim_timeline with no shim_triggers, not allow-listed, is refused",
        any(n == "r1" for n, _ in probs),
    )
    with_trigger = [
        {"name": "r2", "shim_timeline": "x.txt", "shim_triggers": [{"cmd": "c", "when": []}]}
    ]
    probs = scan_registry(with_trigger, allow_entries=[])
    check("shim_timeline WITH shim_triggers is allowed", not probs)
    allow_listed = [{"row": "r1", "reason": "static idle window", "tracker_id": "TL-FAKE"}]
    probs = scan_registry(no_trigger, allow_entries=allow_listed)
    check("an allow-listed row is allowed", not probs)
    no_timeline = [{"name": "r3"}]
    probs = scan_registry(no_timeline, allow_entries=[])
    check("a row with no shim_timeline at all is never flagged", not probs)

    # the allow-list file's own shape checks.
    check(
        "a clean allow-list passes",
        not allow_problems([{"row": "a", "reason": "x", "tracker_id": "T"}]),
    )
    check("a malformed allow-list entry is refused", bool(allow_problems([{"row": "a"}])))
    check(
        "a duplicate allow-list row is refused",
        bool(
            allow_problems(
                [
                    {"row": "a", "reason": "x", "tracker_id": "T"},
                    {"row": "a", "reason": "y", "tracker_id": "T2"},
                ]
            )
        ),
    )

    s_probs, a_probs, r_probs = run()
    for rel, line_no, op, msg in s_probs:
        print("  " + "%s:%d: %s" % (rel, line_no, msg))
    for p in a_probs:
        print("  allow-list: " + p)
    for name, msg in r_probs:
        print("  registry row %r: %s" % (name, msg))
    check(
        "the committed tree is clean (scripts + allow-list + registry)",
        not (s_probs or a_probs or r_probs),
    )

    print("lint_ui_sync selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    s_probs, a_probs, r_probs = run()
    if a_probs:
        print(
            "lint_ui_sync: %d problem(s) in %s:" % (len(a_probs), os.path.relpath(ALLOW_PATH, REPO))
        )
        for p in a_probs:
            print("  " + p)
    if s_probs:
        print("lint_ui_sync: %d clock-only-sync problem(s) in uiscripts:" % len(s_probs))
        for rel, line_no, op, msg in s_probs:
            print("  %s:%d: %s" % (rel, line_no, msg))
    if r_probs:
        print(
            "lint_ui_sync: %d registry row(s) with no state-based sync for their shim_timeline:"
            % len(r_probs)
        )
        for name, msg in r_probs:
            print("  %s: %s" % (name, msg))
    if a_probs or s_probs or r_probs:
        return 1
    print("lint_ui_sync: PASS (0 clock-only-sync offenders)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
