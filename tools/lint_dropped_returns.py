#!/usr/bin/env python3
"""lint_dropped_returns.py -- a PROMOTED body may not drop a return its caller reads.

PROTO-RETSCAN. The Ghidra-FREE half of the pair: the Ghidra-side epilogue-return dump reads the
disassembly and writes tools/data/epilogue_returns.json; this consumes it, so lint_repo.py stays
fast and needs no Ghidra. Same split as the Ghidra-side call-proto dump -> gen_dll_calls.py --check.

WHAT IT REFUSES, and why that exact intersection.

A function is a defect here only when THREE things hold at once:

  1. its committed prototype says it returns `void`,
  2. its ORIGINAL epilogue really computes a value into EAX (verdict REAL -- see the dumper for how
     that is told apart from the Watcom uninitialised-slot artifact), and
  3. something CONSUMES that value: a caller reads EAX after the call, or -- the dangerous case --
     the function's address is TAKEN and it is invoked through a stored pointer, so the consumer is
     whatever pumps that slot and cannot be found by looking at call sites.

...and the body is PROMOTED. That last clause is what makes it a hard failure rather than a note:
an unpromoted body still runs the original's own epilogue, so a wrong prototype is latent. Promote
it and our C++ returns whatever happened to be in EAX.

THIS IS THE INVERSE OF A TRAP THE LEDGER ALREADY CARRIES. It records the Watcom FAKE
return -- a value that looks real and is garbage -- which trains a reader to DISBELIEVE returns.
llm_tutorial_step_driver was the opposite: a real `return 0` typed away, and because it is installed
as a menu async callback whose pump uninstalls anything returning nonzero, the tutorial ran exactly
one tick. Shipped, player-facing, and invisible to every gate we had.

THE WORKLIST IS NOT THE GATE. Unpromoted hits are reported with a count and never fail: they are
real prototype bugs, but latent, and a lint that goes red on work nobody has scheduled gets
switched off. Promote one of them and this turns red the same day.
"""

import argparse
import glob
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(REPO, "tools", "data", "epilogue_returns.json")

PROMOTE_RE = re.compile(r"MH_EXPORT_REPLACE\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,")


def promoted_bodies(root=None):
    """Every function name installed over its original via MH_EXPORT_REPLACE.

    Derived from the source rather than a list, so a promotion added later cannot be forgotten
    here -- the same reasoning write_all_original_ini uses for the [promote] keys.
    """
    root = root or os.path.join(REPO, "src", "mh_dll")
    names = set()
    for pat in ("**/*.cpp", "**/*.h"):
        for path in glob.glob(os.path.join(root, pat), recursive=True):
            try:
                with open(path, encoding="utf-8", errors="replace") as fh:
                    names.update(PROMOTE_RE.findall(fh.read()))
            except OSError:
                continue
    return names


def actionable(rows):
    """The three-way intersection described in the module docstring (promotion aside)."""
    return [
        r
        for r in rows
        if r.get("ret") == "void" and r.get("verdict") == "REAL" and r.get("consumed")
    ]


def evaluate(rows, promoted):
    """(failures, worklist) -- failures are promoted, worklist is everything else actionable."""
    act = actionable(rows)
    fails = [r for r in act if r["name"] in promoted]
    rest = [r for r in act if r["name"] not in promoted]
    return fails, rest


def selftest():
    """The negative cases. A gate that cannot go red is not a gate.

    Case 1 IS the historical bug, replayed on committed data: llm_tutorial_step_driver as it stood
    at EN v396, when its committed prototype said `void`. Its BYTES have not changed since -- only
    the prototype moved -- so feeding its real epilogue facts with ret="void" reproduces v396's
    input for that function exactly, which is what the item asked for without checking out an old
    DB revision.
    """
    ok = True

    def check(cond, msg):
        nonlocal ok
        print("  %s %s" % ("[ok]  " if cond else "[FAIL]", msg))
        if not cond:
            ok = False

    driver_v396 = {
        "name": "llm_tutorial_step_driver",
        "ret": "void",
        "verdict": "REAL",
        "consumed": True,
        "consumer": "ADDRESS-TAKEN at 004baf6e",
    }
    fails, _ = evaluate([driver_v396], {"llm_tutorial_step_driver"})
    check(
        [r["name"] for r in fails] == ["llm_tutorial_step_driver"],
        "the EN v396 shape of llm_tutorial_step_driver is NAMED (the bug this gate exists for)",
    )

    fails, work = evaluate([driver_v396], set())
    check(
        not fails and len(work) == 1, "the same row UNPROMOTED is a worklist entry, not a failure"
    )

    fake = dict(driver_v396, verdict="FAKE")
    fails, _ = evaluate([fake], {"llm_tutorial_step_driver"})
    check(not fails, "a Watcom FAKE return is not a failure (the void prototype is correct there)")

    unconsumed = dict(driver_v396, consumed=False)
    fails, _ = evaluate([unconsumed], {"llm_tutorial_step_driver"})
    check(
        not fails, "a real return NOBODY reads is not a failure -- harmless, and 543 of them exist"
    )

    typed = dict(driver_v396, ret="int")
    fails, _ = evaluate([typed], {"llm_tutorial_step_driver"})
    check(not fails, "the FIXED prototype (ret=int) passes -- the gate tracks the fix")

    check(
        "llm_tutorial_step_driver" in promoted_bodies(),
        "promoted_bodies() finds a real MH_EXPORT_REPLACE body in the tree",
    )
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--selftest", action="store_true", help="run the negative cases and exit")
    ap.add_argument("--worklist", action="store_true", help="list the unpromoted hits too")
    args = ap.parse_args()

    if args.selftest:
        print("[lint_dropped_returns] selftest")
        return selftest()

    if not os.path.isfile(DATA):
        print(
            "[lint_dropped_returns] MISSING %s -- it is a DUMP, not a derivation: the epilogue "
            "return-value scan runs INSIDE Ghidra against /eng/mh.exe and commits its answer here. "
            "Regenerating it needs the database and the Ghidra-side dumper, which is research "
            "tooling; without the file this check has no population and refuses rather than "
            "reporting a green over nothing." % DATA
        )
        return 1
    with open(DATA, encoding="utf-8") as fh:
        data = json.load(fh)
    rows = data.get("functions", [])
    fails, work = evaluate(rows, promoted_bodies())

    if fails:
        print(
            "[lint_dropped_returns] FAIL: %d PROMOTED body(ies) drop a return something reads."
            % len(fails)
        )
        for r in fails:
            print(
                "    %s %s -- %s; %s" % (r.get("va", ""), r["name"], r["evidence"], r["consumer"])
            )
        print(
            "    Fix the PROTOTYPE (the prototype-commit step), regenerate the interop layer, and"
        )
        print("    return the value from the C++ body -- see EN v397 / llm_tutorial_step_driver.")
        return 1

    print(
        "[lint_dropped_returns] ok -- 0 promoted bodies drop a consumed return "
        "(%d unpromoted hit(s) on the worklist)" % len(work)
    )
    if args.worklist:
        for r in sorted(work, key=lambda r: r["name"]):
            print("    %s %s -- %s" % (r.get("va", ""), r["name"], r["consumer"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
