#!/usr/bin/env python3
"""check_a2_progress.py -- mods:A2: does a conquered planet's lab surface its research?

Reads the `; PROGRESS` probe lines the a2_project_gate segment of tools/uiscripts/a_walk.txt (a_walk_ship.txt) leaves in mh_harness.log (harness
emit_progress; the conquest and the lab's unlock are played by the harness through game_UpdateProgress):

  tag=a   before the conquest: the save's planet (Nortus, inv 262) is NOT acquired
  tag=b   after the conquest + the lab's inventions: inv_acquired=1

  check_a2_progress.py --expect fixed  <run-dir>   [net] project_gate_fix=1: after the conquest NO PROJECT whose
                                                   depend[] are all acquired is left hidden (proj_hidden=0) and
                                                   at least one is offered (proj_avail >= 1)
  check_a2_progress.py --expect retail <run-dir>   default (0): the projects the lab unlocks are withheld
                                                   (proj_hidden >= 1, proj_avail = 0)
  check_a2_progress.py --selftest

The row's FIX-OFF twin runs the default config against `--expect fixed` and is expected red.
Absence of the log or of either probe line is RED.
"""

import os
import re
import sys


def probes(text):
    out = {}
    for ln in text.splitlines():
        if not ln.startswith("; PROGRESS "):
            continue
        rec = dict(re.findall(r"(\w+)=([^\s]+)", ln))
        out[rec.get("tag", "?")] = rec
    return out


def problems(text, expect):
    p = probes(text)
    out = []
    a, b = p.get("a"), p.get("b")
    if not a or not b:
        return [
            "mh_harness.log lacks the `; PROGRESS tag=a` / `tag=b` probe lines (probe never fired)"
        ]
    if a.get("inv_acquired") != "0":
        out.append("tag=a: the planet's invention is already acquired -- not a pre-conquest state")
    if b.get("inv_acquired") != "1":
        out.append("tag=b: the planet's invention is not acquired -- the conquest was not played")
    depmet = int(b.get("proj_depmet", "0"))
    hidden = int(b.get("proj_hidden", "0"))
    avail = int(b.get("proj_avail", "0"))
    if depmet < 1:
        out.append("tag=b: no PROJECT has its depend[] met -- the scenario is vacuous")
    if expect == "fixed":
        if hidden != 0:
            out.append(
                "tag=b: %d PROJECT(s) with all depend[] met are still hidden (ids %s)"
                % (hidden, b.get("hidden_ids"))
            )
        if avail < 1:
            out.append("tag=b: no PROJECT is offered after the conquest")
    else:
        if hidden < 1:
            out.append("tag=b: nothing is hidden -- the retail gate did not reproduce")
        if avail != 0:
            out.append("tag=b: %d PROJECT(s) offered -- retail should offer none here" % avail)
    return out


def selftest():
    base = "; PROGRESS tag=a side=0 planet=1 inv=262 inv_acquired=0 inv_available=1 proj_total=86 proj_depmet=0 proj_avail=0 proj_hidden=0 hidden_ids=- cand=-\n"
    hid = (
        base
        + "; PROGRESS tag=b side=0 planet=1 inv=262 inv_acquired=1 inv_available=1 proj_total=86 proj_depmet=2 proj_avail=0 proj_hidden=2 hidden_ids=131,173, cand=-\n"
    )
    ok = (
        base
        + "; PROGRESS tag=b side=0 planet=1 inv=262 inv_acquired=1 inv_available=1 proj_total=86 proj_depmet=2 proj_avail=2 proj_hidden=0 hidden_ids=- cand=-\n"
    )
    assert problems(hid, "retail") == []
    assert problems(ok, "fixed") == []
    assert problems(hid, "fixed")
    assert problems(ok, "retail")
    assert problems(base, "fixed")
    print("check_a2_progress: selftest PASS")
    return 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    expect = "fixed"
    if "--expect" in argv:
        expect = argv[argv.index("--expect") + 1]
    run_dir = argv[-1]
    log = os.path.join(run_dir, "mh_harness.log")
    if not os.path.isfile(log):
        print("check_a2_progress: RED -- no mh_harness.log in %s" % run_dir)
        return 1
    with open(log, encoding="latin1", errors="replace") as fh:
        probs = problems(fh.read(), expect)
    if probs:
        print("check_a2_progress: RED (--expect %s)" % expect)
        for q in probs:
            print("  " + q)
        return 1
    print("check_a2_progress: PASS (--expect %s)" % expect)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
