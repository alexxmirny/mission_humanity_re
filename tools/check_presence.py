#!/usr/bin/env python3
"""check_presence.py -- dist RL15: DID presence.json FOLLOW A CAMPAIGN PLANET SWITCH?

mh.dll rewrites `<logs root>\\presence.json` whenever its state changes.
The launcher's Discord status reads it, so a planet switch that did not rewrite the file shows the
player the old planet. A green UI walk cannot see that (no pixels), so this reads the file.

MODES
  check_presence.py <run-dir|logs-root|presence.json> [--not-planet NAME]
      one snapshot taken AFTER the switch (what test_ui's post_check can hand over: the lane's newest
      run dir, whose parent is the logs root). Requires: schema 1, state/mode `campaign`, system AND
      planet named, planet != NAME (the planet the script started on), and updated_unix >= started_unix
      -- started_unix is set once per session and survives a switch (the 2026-10-09 poll: Nortus ->
      Aman rewrote the file one second later, same pid, same started_unix).
  check_presence.py --before A.json --after B.json
      two snapshots (a hand poll, or a future harness): same pid, both `campaign`, planet and system
      named in both, the PLANET CHANGED, updated_unix moved forward, started_unix unchanged.
  check_presence.py --selftest

ABSENCE IS A FAILURE: a missing, empty, unparseable or foreign-schema file is RED, never a pass.
"""

import argparse
import json
import os
import sys
import tempfile


def _load(path):
    """(doc, err). A directory resolves to its own presence.json, else its parent's (a run dir)."""
    if os.path.isdir(path):
        own = os.path.join(path, "presence.json")
        parent = os.path.join(os.path.dirname(os.path.abspath(path)), "presence.json")
        path = own if os.path.isfile(own) else parent
    if not os.path.isfile(path):
        return None, "no presence.json at %s" % path
    try:
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, ValueError) as e:
        return None, "unreadable %s: %s" % (path, e)
    if not isinstance(doc, dict):
        return None, "%s is not a JSON object" % path
    return doc, None


def _campaign_problems(doc, tag):
    out = []
    if doc.get("schema") != 1:
        out.append("%s: schema %r, want 1" % (tag, doc.get("schema")))
    for k in ("state", "mode"):
        if doc.get(k) != "campaign":
            out.append("%s: %s %r, want 'campaign'" % (tag, k, doc.get(k)))
    for k in ("system", "planet"):
        v = doc.get(k)
        if not isinstance(v, str) or not v.strip():
            out.append("%s: %s is %r, want a name" % (tag, k, v))
    for k in ("started_unix", "updated_unix", "pid"):
        if not isinstance(doc.get(k), int) or isinstance(doc.get(k), bool):
            out.append("%s: %s is %r, want an int" % (tag, k, doc.get(k)))
    return out


def check_after(doc, not_planet=None):
    """Problems ([] = pass) for ONE post-switch snapshot."""
    probs = _campaign_problems(doc, "after")
    if probs:
        return probs
    if not_planet is not None and doc["planet"] == not_planet:
        probs.append("planet is still %r: the switch never reached presence.json" % not_planet)
    if doc["updated_unix"] < doc["started_unix"]:
        probs.append(
            "updated_unix %d < started_unix %d: a rewrite cannot predate the session start"
            % (doc["updated_unix"], doc["started_unix"])
        )
    return probs


def check_pair(before, after):
    """Problems for a before/after pair of snapshots."""
    probs = _campaign_problems(before, "before") + _campaign_problems(after, "after")
    if probs:
        return probs
    if before["pid"] != after["pid"]:
        probs.append(
            "pid changed %d -> %d: two processes, not one switch" % (before["pid"], after["pid"])
        )
    if before["planet"] == after["planet"]:
        probs.append("planet unchanged (%r)" % after["planet"])
    if after["updated_unix"] <= before["updated_unix"]:
        probs.append(
            "updated_unix did not advance (%d -> %d)"
            % (before["updated_unix"], after["updated_unix"])
        )
    if before["started_unix"] != after["started_unix"]:
        probs.append(
            "started_unix moved %d -> %d: the campaign session restarted"
            % (before["started_unix"], after["started_unix"])
        )
    return probs


def _doc(**kw):
    d = {
        "schema": 1,
        "pid": 100,
        "state": "campaign",
        "mode": "campaign",
        "map": None,
        "players": None,
        "max_players": None,
        "system": "Nortus System",
        "planet": "Nortus",
        "started_unix": 1000,
        "updated_unix": 1000,
    }
    d.update(kw)
    return d


def selftest():
    fails = []

    def expect(name, probs, want_red):
        if bool(probs) != want_red:
            fails.append("%s: %s" % (name, probs or "passed but should be RED"))

    good = _doc(planet="Aman", updated_unix=1090)
    expect("after: good", check_after(good, "Nortus"), False)
    expect("after: planet unchanged", check_after(_doc(updated_unix=1090), "Nortus"), True)
    expect(
        "after: never rewritten", check_after(_doc(planet="Aman", updated_unix=999), "Nortus"), True
    )
    expect(
        "after: menu state",
        check_after(_doc(state="menu", planet="Aman", updated_unix=1090), "Nortus"),
        True,
    )
    expect(
        "after: schema 2",
        check_after(_doc(schema=2, planet="Aman", updated_unix=1090), "Nortus"),
        True,
    )
    expect("after: planet null", check_after(_doc(planet=None, updated_unix=1090), "Nortus"), True)
    expect(
        "after: system empty",
        check_after(_doc(system="", planet="Aman", updated_unix=1090), "Nortus"),
        True,
    )
    expect(
        "after: no --not-planet still checks order",
        check_after(_doc(planet="Aman", updated_unix=999), None),
        True,
    )
    before = _doc()
    expect("pair: good", check_pair(before, good), False)
    expect("pair: same planet", check_pair(before, _doc(updated_unix=1090)), True)
    expect("pair: not advanced", check_pair(before, _doc(planet="Aman")), True)
    expect(
        "pair: other pid", check_pair(before, _doc(planet="Aman", updated_unix=1090, pid=101)), True
    )
    expect(
        "pair: session restarted",
        check_pair(before, _doc(planet="Aman", updated_unix=1090, started_unix=1050)),
        True,
    )
    expect("pair: before not campaign", check_pair(_doc(state="menu"), good), True)
    with tempfile.TemporaryDirectory() as td:
        run = os.path.join(td, "logs", "2026_run")
        os.makedirs(run)
        doc, err = _load(run)
        expect("load: missing file", [err] if err else [], True)
        with open(os.path.join(td, "logs", "presence.json"), "w", encoding="utf-8") as f:
            f.write("{not json")
        doc, err = _load(run)
        expect("load: unparseable", [err] if err else [], True)
        with open(os.path.join(td, "logs", "presence.json"), "w", encoding="utf-8") as f:
            json.dump(good, f)
        doc, err = _load(run)  # a run dir resolves to its parent's file
        expect("load: run dir -> parent file", [err] if err else check_after(doc, "Nortus"), False)
    for f in fails:
        print("SELFTEST FAIL:", f)
    print("check_presence selftest: %s" % ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument(
        "path", nargs="?", help="run dir, logs root or presence.json (the AFTER snapshot)"
    )
    ap.add_argument("--not-planet", help="the planet the run started on; the snapshot must differ")
    ap.add_argument("--before", help="BEFORE snapshot (with --after)")
    ap.add_argument("--after", help="AFTER snapshot (with --before)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if a.before or a.after:
        if not (a.before and a.after):
            print("check_presence: --before and --after go together")
            return 2
        b, e1 = _load(a.before)
        n, e2 = _load(a.after)
        probs = [e for e in (e1, e2) if e] or check_pair(b, n)
        if not probs:
            print("PASS: %s / %s -> %s / %s" % (b["system"], b["planet"], n["system"], n["planet"]))
    else:
        if not a.path:
            print("check_presence: need a path (or --before/--after, or --selftest)")
            return 2
        doc, err = _load(a.path)
        probs = [err] if err else check_after(doc, a.not_planet)
        if not probs:
            print(
                "PASS: campaign %s / %s (started %d, updated %d)"
                % (doc["system"], doc["planet"], doc["started_unix"], doc["updated_unix"])
            )
    for p in probs:
        print("FAIL:", p)
    return 1 if probs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
