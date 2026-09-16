#!/usr/bin/env python3
"""lint_x87_asm.py -- inline `__asm` in the libmh-destined modules only ever goes DOWN (CRT-X87).

WHAT IT GATES. The translated bodies carried inline x87 because a few Watcom idioms have no C++
spelling we can assert rather than assume. That is a debt, not a style: it is unportable, it is
invisible to every offline oracle, and it was DUPLICATED -- 60 blocks over 26 distinct instruction
sequences, one of which appeared 26 times under six different local names.

THE COUNT OUTSIDE mh/fp/ IS NOW ZERO (2026-09-08). CRT-X87's hoist is complete: every block lives in
the helper module and the translated bodies forward to it. The asm MOVED, it did not change, so the
hoist needed no equivalence proof -- whether a given helper later becomes C++ is a separate,
per-helper decision (mh/fp/x87.h's step-2 banner records the one that cleared and the three shapes
that were measured and refused).

So this began as a ratchet and has become a BAN, without a rule change: the mechanism is unchanged --
the count may not RISE, and may not sit stale after it falls -- but from zero there is only one
direction, and a new `__asm` in sim/ai/orders/tact/lockstep/save/state now fails the gate by name.
Both directions still fail, for the reason the VA census gives: a ratchet that only fails upward
becomes a ceiling nobody lowers. The helper module itself is deliberately uncounted -- it is the
destination, and capping it would penalise hoisting.

The count is of REAL blocks. `// clang-format would fold the __asm block` is a comment about asm, not
asm, and counting it would make the measure drift every time someone edits a banner.

Usage:
    python tools/lint_x87_asm.py            # the report
    python tools/lint_x87_asm.py --check    # the gate (tools/lint_repo.py)
    python tools/lint_x87_asm.py --record   # re-record after the count falls
    python tools/lint_x87_asm.py --selftest # prove each refusal fires, and does not over-fire
"""

from __future__ import annotations

import argparse
import collections
import io
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _dllsrc  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MH = os.path.join(REPO, "src", "mh_dll", "mh")
OUT = os.path.join(REPO, "tools", "data", "x87_asm_baseline.json")

# The modules that become libmh. seams/hook are the injection layer -- they may hold asm forever.
LIB = ("sim", "ai", "orders", "tact", "lockstep", "save", "state")
HOME = "fp"  # where a hoisted block is allowed to live
OPENER = re.compile(r"^__asm\s*\{?\s*$")


def scan():
    """-> ({file: count} outside the home, home_count)."""
    outside, home = collections.Counter(), 0
    for tree, root, dirs, files in _dllsrc.walk():
        top = _dllsrc.rel_dir(tree, root).split("/", 1)[0]
        if top in ("attic", "addr"):
            dirs[:] = []
            continue
        if top not in LIB and top != HOME:
            continue
        for fn in sorted(files):
            if not fn.endswith((".cpp", ".h")):
                continue
            path = os.path.join(root, fn)
            rel = _dllsrc.rel_or_raise(path)
            n = sum(
                1
                for line in io.open(path, encoding="utf-8", errors="replace").read().split("\n")
                if OPENER.match(line.strip())
            )
            if not n:
                continue
            if top == HOME:
                home += n
            else:
                outside[rel] += n
    return outside, home


def build():
    outside, home = scan()
    return {
        "_generated_by": "tools/lint_x87_asm.py -- do not hand-edit; use --record",
        "_measures": "real inline __asm blocks in the libmh-destined modules, outside mh/%s/"
        % HOME,
        "total_outside": sum(outside.values()),
        "in_home": home,
        "by_file": dict(sorted(outside.items())),
    }


def compare(cur, base):
    fails = []
    c, b = cur["total_outside"], base["total_outside"]
    if c > b:
        gained = {
            f: n - base["by_file"].get(f, 0)
            for f, n in cur["by_file"].items()
            if n > base["by_file"].get(f, 0)
        }
        fails.append(
            "INLINE ASM ROSE: %d -> %d block(s) outside mh/%s/. The hoist is COMPLETE -- the "
            "translated bodies hold none, and a new one there is a regression, not a starting "
            "point. Put it in mh/%s/ (x87_shapes.h for a numeric idiom, st0_call.h for a calling "
            "convention) and forward to it, so it can be named, shared and later measured:\n%s"
            % (
                b,
                c,
                HOME,
                HOME,
                "\n".join("    +%d  %s" % (n, f) for f, n in sorted(gained.items())),
            )
        )
    elif c < b:
        fails.append(
            "THE BASELINE IS STALE: %d -> %d block(s). The count FELL, which is the point -- "
            "re-record with `python tools/lint_x87_asm.py --record` so the ratchet tightens."
            % (b, c)
        )
    return fails


def selftest():
    base = build()
    cases = []

    def case(desc, cur, expect):
        fails = compare(cur, base)
        cases.append((desc, any(expect in f for f in fails), expect, fails))

    up = json.loads(json.dumps(base))
    up["total_outside"] += 1
    up["by_file"]["sim/made_up.cpp"] = 1
    case("a NEW asm block outside the helper module is refused", up, "INLINE ASM ROSE")

    down = json.loads(json.dumps(base))
    down["total_outside"] -= 1
    case("a FALLEN count is refused until re-recorded", down, "THE BASELINE IS STALE")

    # The over-refusal arm: an unchanged tree must be silent, or neither arm above means anything.
    quiet = compare(json.loads(json.dumps(base)), base)
    cases.append(("an UNCHANGED tree is accepted", not quiet, "(no failures)", quiet))

    bad = 0
    for desc, hit, expect, fails in cases:
        print("  [%s] %s" % ("ok" if hit else "FAIL", desc))
        if not hit:
            bad += 1
            print("      expected %r, got %s" % (expect, fails or "(nothing)"))
    print("lint_x87_asm selftest: %s (%d case(s))" % ("PASS" if not bad else "FAIL", len(cases)))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--record", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    if a.selftest:
        return selftest()

    cur = build()
    if a.check:
        if not os.path.exists(OUT):
            print("lint_x87_asm --check: %s missing -- run --record first" % OUT)
            return 1
        fails = compare(cur, json.load(io.open(OUT, encoding="utf-8")))
        if fails:
            for f in fails:
                print("lint_x87_asm: %s" % f)
            return 1
        print(
            "x87 asm: %d block(s) outside mh/%s/, %d in it -- ratchet green"
            % (cur["total_outside"], HOME, cur["in_home"])
        )
        return 0

    print(
        "inline __asm in the libmh-destined modules: %d outside mh/%s/, %d in it"
        % (cur["total_outside"], HOME, cur["in_home"])
    )
    for f, n in sorted(cur["by_file"].items(), key=lambda kv: -kv[1]):
        print("   %-58s %d" % (f, n))
    if a.record:
        with io.open(OUT, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(cur, fh, indent=1)
            fh.write("\n")
        print("\nwrote %s" % os.path.relpath(OUT, REPO))
    return 0


if __name__ == "__main__":
    sys.exit(main())
