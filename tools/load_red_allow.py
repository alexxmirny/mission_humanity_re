#!/usr/bin/env python3
"""tooling:TL-SUITE-LOADRED -- the solo-rerun allow-list.

`tools/test_ui.py` re-runs a red capture-suite row ALONE once, after every row has finished; a row
that passes alone is LOAD-RED (red only under suite contention) rather than a real red, but LOAD-RED
still fails the gate UNLESS the row is named here. An entry is the row name, the tracker id that
explains WHY it flakes (or, for a row that was never actually racing anything, the reason it is not
peer-dependent at all), the date it was added, and an expiry date -- an allow-list entry does not
renew itself, so `--check` REDS on one that has already expired (TL-HARN19/TL-GATE-LOADFLAKE-0925 is
exactly what an allow-list nobody re-dates turns into: reds nobody reads any more).

An expired or malformed entry is IGNORED by `allowed()` (the row falls back to failing the gate, same
as having no entry at all) -- `--check` is what turns that into a visible lint failure instead of a
silent, un-investigated gate red.

    python tools/load_red_allow.py --check       # validates the committed file (a lint_repo row)
    python tools/load_red_allow.py --selftest    # planted negatives
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATH = os.path.join(REPO, "tools", "data", "load_red_allow.json")
REQUIRED = ("row", "tracker_id", "date_added", "expiry")


def _parse_date(s):
    return datetime.datetime.strptime(s, "%Y-%m-%d").date()


def load(path=PATH):
    try:
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return []


def problems(entries, today=None):
    """Every reason `entries` (the parsed JSON list) is not clean. [] means clean."""
    today = today or datetime.date.today()
    probs = []
    if not isinstance(entries, list):
        return ["top level must be a list"]
    seen_rows = set()
    for i, e in enumerate(entries):
        where = "entry #%d" % i
        if not isinstance(e, dict):
            probs.append("%s: not a mapping" % where)
            continue
        where = "entry #%d (row %r)" % (i, e.get("row"))
        missing = [k for k in REQUIRED if k not in e]
        if missing:
            probs.append("%s: missing key(s) %s" % (where, missing))
            continue
        unknown = sorted(set(e) - set(REQUIRED))
        if unknown:
            probs.append("%s: unknown key(s) %s" % (where, unknown))
        if e["row"] in seen_rows:
            probs.append(
                "%s: duplicate row (one active entry per row, or the wrong one may win)" % where
            )
        seen_rows.add(e["row"])
        added = expiry = None
        try:
            added = _parse_date(e["date_added"])
        except (TypeError, ValueError):
            probs.append("%s: date_added %r is not YYYY-MM-DD" % (where, e.get("date_added")))
        try:
            expiry = _parse_date(e["expiry"])
        except (TypeError, ValueError):
            probs.append("%s: expiry %r is not YYYY-MM-DD" % (where, e.get("expiry")))
        if added and expiry and expiry < added:
            probs.append("%s: expiry %s is before date_added %s" % (where, expiry, added))
        if expiry and expiry < today:
            probs.append(
                "%s: EXPIRED %s -- the row now falls back to failing the gate on LOAD-RED; renew "
                "(new expiry, same evidence) or drop the entry" % (where, expiry)
            )
    return probs


def allowed(name, entries=None, today=None):
    """(allowed, reason) for row `name`. A malformed or expired entry is IGNORED (never allows) --
    `problems()` is what surfaces that as a lint failure instead of a silently-wrong pass."""
    entries = load() if entries is None else entries
    today = today or datetime.date.today()
    for e in entries:
        if not isinstance(e, dict) or e.get("row") != name:
            continue
        try:
            expiry = _parse_date(e["expiry"])
        except (KeyError, TypeError, ValueError):
            continue
        if expiry >= today:
            return True, "%s (expires %s)" % (e.get("tracker_id", "?"), expiry)
    return False, ""


# ---- selftest -----------------------------------------------------------------------------------

_GOOD = [
    {"row": "a", "tracker_id": "TL-X", "date_added": "2026-01-01", "expiry": "2099-01-01"},
]


def _case(label, entries, today, want_probs):
    probs = problems(entries, today=today)
    hit = bool(probs) == want_probs
    print("  %s  %-46s %s" % ("ok  " if hit else "FAIL", label, probs or "(clean)"))
    return hit


def selftest():
    fails = 0
    today = datetime.date(2026, 6, 1)
    cases = [
        ("clean entry", _GOOD, today, False),
        ("not a list", {"row": "a"}, today, True),
        ("missing key", [{"row": "a", "tracker_id": "T", "date_added": "2026-01-01"}], today, True),
        (
            "unknown key",
            [dict(_GOOD[0], extra=1)],
            today,
            True,
        ),
        (
            "bad date format",
            [{"row": "a", "tracker_id": "T", "date_added": "01-01-2026", "expiry": "2099-01-01"}],
            today,
            True,
        ),
        (
            "expiry before date_added",
            [{"row": "a", "tracker_id": "T", "date_added": "2026-06-01", "expiry": "2026-01-01"}],
            today,
            True,
        ),
        (
            "expired entry reds",
            [{"row": "a", "tracker_id": "T", "date_added": "2026-01-01", "expiry": "2026-01-02"}],
            today,
            True,
        ),
        (
            "duplicate row",
            _GOOD
            + [
                {"row": "a", "tracker_id": "T2", "date_added": "2026-01-01", "expiry": "2099-01-01"}
            ],
            today,
            True,
        ),
    ]
    for label, entries, t, want in cases:
        fails += not _case(label, entries, t, want)

    # allowed(): a valid entry allows; an expired one falls back to not-allowed rather than raising.
    ok, reason = allowed("a", _GOOD, today=today)
    hit = ok and "TL-X" in reason
    print(
        "  %s  allowed(): a valid entry allows, with its tracker id" % ("ok  " if hit else "FAIL")
    )
    fails += not hit
    expired = [{"row": "a", "tracker_id": "T", "date_added": "2026-01-01", "expiry": "2026-01-02"}]
    ok2, _ = allowed("a", expired, today=today)
    hit2 = not ok2
    print(
        "  %s  allowed(): an EXPIRED entry does not allow (falls back to failing)"
        % ("ok  " if hit2 else "FAIL")
    )
    fails += not hit2
    ok3, _ = allowed("nope", _GOOD, today=today)
    hit3 = not ok3
    print("  %s  allowed(): an unlisted row does not allow" % ("ok  " if hit3 else "FAIL"))
    fails += not hit3

    live = problems(load())
    for p in live:
        print("  " + p)
    print("  %s  committed load_red_allow.json is clean" % ("ok  " if not live else "FAIL"))
    fails += bool(live)

    print("load_red_allow selftest: %s" % ("PASS" if not fails else "FAIL (%d)" % fails))
    return 1 if fails else 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--check", action="store_true", help="validate the committed allow-list")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    probs = problems(load())
    if probs:
        print("load_red_allow: %d problem(s):" % len(probs))
        for p in probs:
            print("  " + p)
        return 1
    print("load_red_allow: OK (%d entr%s)" % (len(load()), "y" if len(load()) == 1 else "ies"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
