#!/usr/bin/env python3
"""order_stream.py -- the HOOK-FREE order channel: what the strategic sim DECIDED, per step.

SIM1-P clause 8. Every other sim verdict channel is either a trajectory hash (which says two runs
differed, never what differed) or an entry hook (which goes silent the moment its target is promoted or
rebound -- G120, G122, G123, three wrong conclusions in two days). This one is
hook-free BY CONSTRUCTION rather than by policy: harness.cpp POLLS the three order regions at a fixed
sample point every step and prints them as `;ord` lines, so it sees an order whoever issued it and no
promotion can quiet it.

  ;ord <step> pend=<n> q=<n> stage=<n>
  ;ord   P<i> own=<hex> unit=<n> code=<hex> p0=<n> exec=<n>ms
  ;ord   Q<i> ...
  ;ord   S<i> ...

THE THIRD REGION AROSE WITH THIS FILE. Until 2026-09-05 the poller read ORDER_PENDING and
ORDER_QUEUE and not ORDER_STAGING, so the channel described two thirds of the decision and nothing
said so. `S` rows are the third.

WHAT THE SAMPLE POINT ACTUALLY SEES (measured 2026-09-10, X-SPINE clause 6; do not re-derive). The
poll lives in on_sim_step, i.e. at llm_strat_sim_step ENTRY, PRE-BODY -- release_due fills the queue
and order_queue_dispatch drains it INSIDE the same step body. Over a real 8000-step A/B
(tmp/ab/logs/sim_closure_control_replay, 3292 `;ord` steps):

    pend=  0 on ALL 3292 steps      -- empty at this point, always
    stage= 0 on ALL 3292 steps      -- empty at this point, always
    q=     1..12+, 8616 `Q` rows    -- NOT empty; this is the two-thirds that does work

So this channel is arm-symmetric AND non-empty for the QUEUE and structurally blind for the other
two: what only exists transiently inside the dispatch can be seen by a detour ON the dispatch and by
nothing else, which is a hook resting on a convention. A green `;ord` comparison is a statement about
queued orders, not about all three regions -- and the P/S rows are kept in the format because "the
poller looked and found nothing" and "the poller does not look" must stay different facts.

WHAT IT IS FOR, and the reason it is worth more than the hash it accompanies: a region hash names the
first diverging STEP, and this names the first diverging ORDER. That is what turned --tact-equiv's
order stream from an advisory note into that gate's finest-grained verdict.

WHAT IT IS NOT. It is a record of what the sim DECIDED, not of what any body EXECUTED -- exactly the
distinction journal_coverage.py's docstring draws between command coverage and execution coverage, and
the one that let a seeded defect survive a green gate (G115). Coverage answers the second question;
this answers the first. Do not let a green order stream stand in for either a trajectory hash or
tools/coverage.py.

USAGE
    python tools/order_stream.py <run_dir_or_log> [<run_dir_or_log>]   # dump, or compare two arms
    python tools/order_stream.py --selftest                            # the comparison's own arms
"""

import argparse
import os
import re
import sys

# `;ord <step> pend= q= stage=` -- `stage=` is optional so a log from before 2026-09-05 still parses
# (it reports stage=None rather than 0, because "the poller did not look" and "there was nothing" are
# different facts and only the first should ever be silently tolerated).
HEAD = re.compile(r"^;ord (\d+) pend=(-?\d+) q=(-?\d+)(?: stage=(-?\d+))?\s*$")
ROW = re.compile(
    r"^;ord\s+([PQS])(\d+) own=([0-9A-Fa-f]+) unit=(\d+) code=([0-9A-Fa-f]+) p0=(-?\d+) exec=(-?\d+)ms\s*$"
)


def parse(text):
    """{step: {"counts": (pend, q, stage), "orders": [(tag, idx, own, unit, code, p0, exec_ms)]}}."""
    out, cur = {}, None
    for line in text.splitlines():
        m = HEAD.match(line)
        if m:
            step = int(m.group(1))
            stage = int(m.group(4)) if m.group(4) is not None else None
            cur = {"counts": (int(m.group(2)), int(m.group(3)), stage), "orders": []}
            out[step] = cur
            continue
        m = ROW.match(line)
        if m and cur is not None:
            cur["orders"].append(
                (
                    m.group(1),
                    int(m.group(2)),
                    int(m.group(3), 16),
                    int(m.group(4)),
                    int(m.group(5), 16),
                    int(m.group(6)),
                    int(m.group(7)),
                )
            )
    return out


def read(path):
    """Accept a run directory or a log file; return its parsed stream."""
    if os.path.isdir(path):
        path = os.path.join(path, "mh_harness.log")
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8", errors="replace") as fh:
        return parse(fh.read())


def compare(a, b):
    """(verdict, detail) over two parsed streams. Verdicts: IDENTICAL / DIVERGED / EMPTY / NO_OVERLAP.

    ABSENCE IS ITS OWN VERDICT, and it is the reason this returns four values rather than a bool. An
    empty stream compares equal to another empty stream for free -- which is the vacuous pass this
    whole family of gates keeps rediscovering (the instrument-channel notes Finding 4 puts it best: a log with
    no line and a log with a clean line look identical to a grep that only asks about failures). So
    "neither arm decided anything" and "both arms decided the same thing" are DIFFERENT answers here.
    """
    if not a or not b:
        return (
            "EMPTY",
            "one arm produced NO `;ord` lines at all -- the channel was not armed (needs "
            "[harness] order_log=1) or the run never stepped. This is not agreement.",
        )
    common = sorted(set(a) & set(b))
    if not common:
        return (
            "NO_OVERLAP",
            "the two arms share no sampled step (%d vs %d steps) -- nothing was "
            "compared" % (len(a), len(b)),
        )
    for step in common:
        ca, cb = a[step]["counts"], b[step]["counts"]
        if ca != cb:
            return "DIVERGED", "step %d: counts pend/q/stage %s vs %s" % (step, ca, cb)
        oa, ob = a[step]["orders"], b[step]["orders"]
        if oa != ob:
            for i, (x, y) in enumerate(zip(oa, ob)):
                if x != y:
                    return "DIVERGED", "step %d order #%d: %s vs %s" % (step, i, x, y)
            return "DIVERGED", "step %d: %d orders vs %d" % (step, len(oa), len(ob))
    decided = sum(1 for s in common if a[s]["orders"])
    if not decided:
        return "EMPTY", (
            "%d step(s) compared and NOT ONE carried an order -- the arms agree about nothing "
            "happening, which is not evidence about the sim's decisions" % len(common)
        )
    return "IDENTICAL", "%d step(s) compared, %d of them carrying orders; every order identical" % (
        len(common),
        decided,
    )


def _selftest():
    """The comparison's own arms. A gate whose FAILING case is never exercised is one nobody has seen
    work, and three of the four verdicts here are about a thing NOT happening."""
    base = ";ord 5 pend=1 q=0 stage=0\n;ord   P0 own=0007 unit=3 code=1A p0=2 exec=1500ms\n"
    cases = [
        ("identical streams", base, base, "IDENTICAL"),
        ("a changed order code", base, base.replace("code=1A", "code=1B"), "DIVERGED"),
        ("a changed count", base, base.replace("pend=1", "pend=2"), "DIVERGED"),
        ("a changed stage count", base, base.replace("stage=0", "stage=1"), "DIVERGED"),
        ("one arm silent", base, "", "EMPTY"),
        ("both arms silent", "", "", "EMPTY"),
        ("no shared step", base, base.replace(";ord 5 ", ";ord 9 "), "NO_OVERLAP"),
        (
            "agreeing about nothing",
            ";ord 5 pend=0 q=0 stage=0\n",
            ";ord 5 pend=0 q=0 stage=0\n",
            "EMPTY",
        ),
    ]
    bad = 0
    for name, ta, tb, want in cases:
        got, why = compare(parse(ta), parse(tb))
        ok = got == want
        print("  %-24s %-11s %s" % (name, got, "ok" if ok else "FAIL (wanted %s)" % want))
        bad += 0 if ok else 1
    # The S row must parse, and a pre-2026-09-05 log must still parse with stage=None rather than 0.
    s = parse(";ord 5 pend=0 q=0 stage=1\n;ord   S0 own=0007 unit=3 code=1A p0=2 exec=1500ms\n")
    ok = s[5]["orders"] and s[5]["orders"][0][0] == "S"
    print("  %-24s %s" % ("an S row parses", "ok" if ok else "FAIL"))
    bad += 0 if ok else 1
    old = parse(";ord 5 pend=0 q=0\n")
    ok = old[5]["counts"][2] is None
    print("  %-24s %s" % ("old log -> stage None", "ok" if ok else "FAIL"))
    bad += 0 if ok else 1
    print("order-stream selftest: %s" % ("PASS" if not bad else "FAIL"))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("paths", nargs="*", help="run dir or mh_harness.log; two = compare them")
    ap.add_argument(
        "--selftest", action="store_true", help="run the comparison's own arms and exit"
    )
    a = ap.parse_args()
    if a.selftest:
        return _selftest()
    if not a.paths:
        ap.print_help()
        return 2
    streams = []
    for p in a.paths:
        st = read(p)
        if st is None:
            print("FAIL: no harness log at %s" % p)
            return 1
        streams.append(st)
    if len(streams) == 1:
        st = streams[0]
        steps = sorted(st)
        n = sum(len(st[s]["orders"]) for s in steps)
        print("%d sampled step(s), %d order(s)" % (len(steps), n))
        if not steps:
            print(
                "NO `;ord` lines -- the channel needs [harness] order_log=1. Absence is not quiet."
            )
            return 1
        for s in steps[:20]:
            print(
                "  step %-6d pend/q/stage=%s  orders=%d"
                % (s, st[s]["counts"], len(st[s]["orders"]))
            )
        return 0
    verdict, detail = compare(streams[0], streams[1])
    print("order stream: %s -- %s" % (verdict, detail))
    return 0 if verdict == "IDENTICAL" else 1


if __name__ == "__main__":
    sys.exit(main())
