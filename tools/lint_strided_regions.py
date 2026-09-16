#!/usr/bin/env python3
"""lint_strided_regions.py -- a region indexed by a ROW STRIDE must be longer than one row.

WHAT THIS CATCHES, AND WHY IT TOOK A STANDALONE HOST TO FIND IT
---------------------------------------------------------------
`_G_LLM_STRAT_MOVE_MICROSTEPS` was claimed at 96 bytes in the region registry -- ONE row -- while
the sim indexes it `move_microsteps[move_heading * MICROSTEPS_PER_HEADING + move_microstep]`, with
`move_heading` taken `% 24`. So every read for a unit whose heading is not 0 was out of bounds.

INSIDE mh.exe THAT CANNOT FAIL. The over-index lands in the game's own `.bss`, a few hundred bytes
further along, where the other 23 rows really are -- so the reads are CORRECT and no oracle,
hash, shadow site or A/B can see anything wrong. The bug is invisible by construction.

It becomes visible the moment a host binds each region into its OWN allocation, because then the
over-index reads whatever region the allocator placed next. LIB-REF's standalone replay diverged at
step 3 in exactly two bytes (a unit's facing_target/facing_current) and the cause was this.

A caution note already existed. mh_addrs.gen.h's entry for the symbol said, verbatim, "GHIDRA TYPES
IT `[1][32]`, ONE ROW / 96 BYTES, AND THAT IS SHORT". Nobody acted on it for five weeks, because
nothing could act on it: a comment is not a gate. This file is the gate.

WHY THE ASSERTION IS EMITTED AS C++ AND NOT EVALUATED HERE
----------------------------------------------------------
The stride in the source is in ELEMENTS (`* MICROSTEPS_PER_HEADING`, 32) and the registry's reach is
in BYTES (2304). Converting between them needs `sizeof(element)`, which only the compiler knows --
the first draft of this tool compared 2304 against 32, passed, and its own mutation arm caught it
before it was wired to anything. So this file DERIVES the triples and EMITS static_asserts; the
COMPILER evaluates them. `--check` is then a drift gate on the generated header, exactly like every
other generated gate here, and a shrunk claim fails the BUILD rather than a lint.

SCOPED TO THE SIM VIEW, deliberately. The assertion needs the element type in scope, and each
module's state view binds its own types; sim is where both known instances of this bug live. A
second module joins by adding its binder and its header to BINDERS below.

THE PREDICATE, and why it needs no hand-authored table
------------------------------------------------------
A region indexed `[row * STRIDE + i]` whose measured reach is EXACTLY ONE STRIDE is only ever
correct for row 0. That is a complete statement of the bug and it needs no knowledge of how many
rows there really are -- which is the part a hand-written table would get wrong and then rot. So:

    reach_of(rid) > STRIDE * sizeof(element)      -- more than the row being indexed into

A "whole number of rows" arm was written and REMOVED: `extent` is the region's REACH, and eleven
regions legitimately reach past their array because a save block overruns its symbol (host_bind.cpp
note 2). _G_LLM_PROD_SHUTTLE_SLOTS is one -- 63700 reach over a 63680 array -- and the arm called it
red. The registry does not promise what that arm assumed.

Both are derived: the member -> rid map comes from the state binder (`v.x = ptr<T>(RID_Y);`), the
stride from the index expression, and the reach from the committed registry. Nothing is listed here.

WHAT IT DELIBERATELY DOES NOT CLAIM. It does not verify the row COUNT -- proving `reach >= 24 * 96`
needs the modulus, which lives in the code and varies per member. It catches the one-row claim,
which is the shape that actually occurred twice (`_G_LLM_CURSOR_ANIM_STATES`, fixed at LT1E
2026-09-02, and this one). A region claiming 3 of 24 rows would pass; say so rather than imply
otherwise.

Usage:
    python tools/lint_strided_regions.py             # regenerate the header + report
    python tools/lint_strided_regions.py --check     # drift gate (tools/lint_repo.py)
    python tools/lint_strided_regions.py --report    # the derived triples
"""

from __future__ import annotations

import argparse
import io
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MH = os.path.join(REPO, "src", "mh_dll", "mh")
# F5O: the seven roster domains moved to libmh/; mh/ still holds addr/, which is OUT.
LIBMH = os.path.join(REPO, "src", "mh_dll", "libmh")
REGIONS = os.path.join(REPO, "tools", "data", "state_regions.json")
OUT = os.path.join(REPO, "src", "mh_dll", "mh", "addr", "mh_strided_regions.gen.h")

# (the binder that names the members, the module tree whose sources index them). One entry today; a
# second module joins by adding its pair here. See the banner's SCOPED note.
BINDERS = ((os.path.join(LIBMH, "sim", "sim_state.cpp"), os.path.join(LIBMH, "sim")),)

# The header the binder includes, and therefore the only place a stride constant is guaranteed to be
# in scope where the assertions land. A stride declared in a per-function header (there are several)
# cannot be asserted from here; those rows are EMITTED AS COMMENTS rather than dropped, so the
# population is always visible and a later slice can hoist a constant to make one assertable.
SCOPE_HEADER = os.path.join(LIBMH, "sim", "sim_state.h")

# `v.move_microsteps = ptr<const move_microstep>(RID_STRAT_MOVE_MICROSTEPS);`
BIND_RE = re.compile(r"\b\w+\.(\w+)\s*=\s*ptr<([^>]*)>\(\s*(RID_\w+)\s*\)")
# `v.move_microsteps[<expr> * STRIDE + <expr>]` -- the row-strided index.
IDX_RE = re.compile(r"\b\w+\.(\w+)\s*\[\s*[^\[\]]*?\*\s*([A-Za-z_]\w*|\d+)\s*\+")


def sources(root):
    for r, _d, files in os.walk(root):
        for fn in sorted(files):
            if fn.endswith((".cpp", ".h")):
                yield os.path.join(r, fn)


def region_ids():
    doc = json.loads(io.open(REGIONS, encoding="utf-8-sig").read())
    rows = doc if isinstance(doc, list) else doc["regions"]
    return {"RID_" + r["id"]: r["name"] for r in rows}


def region_reach():
    doc = json.loads(io.open(REGIONS, encoding="utf-8-sig").read())
    rows = doc if isinstance(doc, list) else doc["regions"]
    return {"RID_" + r["id"]: int(r.get("extent") or 0) for r in rows}


def in_scope_constants():
    """-> {name} for every constexpr integer SCOPE_HEADER DECLARES.

    A DECLARATION scan, not a containment test, and the difference is load-bearing: several stride
    constants are MENTIONED in sim_state.h's prose (its field comments cite them) while being
    declared in a per-function header. A containment oracle passes those, and the assertion then
    fails to COMPILE rather than failing -- which reads as a broken gate instead of a red one.
    Measured: PLAYER_RESOURCE_SLOTS and PROD_SHUTTLE_SLOTS_PER_PLAYER are exactly that shape."""
    text = io.open(SCOPE_HEADER, encoding="utf-8", errors="replace").read()
    return set(re.findall(r"constexpr\s+[\w:]+\s+(\w+)\s*=\s*[^;]+;", text))


def name_in_scope(tok, scope):
    """Is `tok` (a stride constant) nameable at the binder? A literal always is."""
    return tok.isdigit() or tok in scope


def in_scope_types():
    """-> the SCOPE_HEADER text; elem_is_nameable uses it as a containment oracle.

    Types get the cheap test and constants get the strict one, because the two fail differently.
    An over-permissive TYPE guess costs a compile error naming the type, which is recoverable and
    loud; the constants above needed the strict scan because their over-permissive case was
    actually occurring."""
    return io.open(SCOPE_HEADER, encoding="utf-8", errors="replace").read()


# Fundamental and <cstdint> names: always nameable, never declared in a project header.
FUNDAMENTAL = {
    "char",
    "signed char",
    "unsigned char",
    "short",
    "int",
    "long",
    "float",
    "double",
    "bool",
    "int8_t",
    "int16_t",
    "int32_t",
    "int64_t",
    "uint8_t",
    "uint16_t",
    "uint32_t",
    "uint64_t",
    "size_t",
    "intptr_t",
    "uintptr_t",
}


def elem_is_nameable(elem, scope_text):
    """The element type reduced to its bare identifier, and whether the binder can name it."""
    bare = elem
    for q in ("const", "volatile", "struct", "class"):
        bare = bare.replace(q + " ", " ")
    bare = " ".join(bare.split())
    if not bare or "<" in bare:
        return False
    if bare in FUNDAMENTAL or "::" in bare:
        return True
    return any((sep + bare + " ") in scope_text for sep in (" ", "\n", "\t", "*"))


def derive():
    """-> sorted [(rid, region name, member, elem type, stride token, site)], deduped per (rid, stride)."""
    ids = region_ids()
    out, seen = [], set()
    for binder, tree in BINDERS:
        binds = {}
        btext = io.open(binder, encoding="utf-8", errors="replace").read()
        for m in BIND_RE.finditer(btext):
            binds.setdefault(m.group(1), (m.group(2).strip(), m.group(3)))
        for path in sources(tree):
            text = io.open(path, encoding="utf-8", errors="replace").read()
            for m in IDX_RE.finditer(text):
                b = binds.get(m.group(1))
                if b is None or b[1] not in ids:
                    continue  # not a bound region member (a local array, a struct field)
                elem, rid = b
                key = (rid, m.group(2))
                if key in seen:
                    continue
                seen.add(key)
                rel = os.path.relpath(path, REPO).replace(os.sep, "/")
                site = "%s:%d" % (rel, text.count("\n", 0, m.start()) + 1)
                out.append((rid, ids[rid], m.group(1), elem, m.group(2), site))
    return sorted(out)


BANNER = """// mh_strided_regions.gen.h -- GENERATED by tools/lint_strided_regions.py. DO NOT EDIT.
//
// A region indexed [row * STRIDE + i] must be longer than ONE row. The registry measures reach in
// BYTES and the source strides in ELEMENTS, so only the compiler can compare them -- which is why
// these are static_asserts and not a Python check.
//
// The bug that earned this file is _G_LLM_STRAT_MOVE_MICROSTEPS: claimed 96 bytes (one row of 32
// 3-byte entries) while the sim indexes row `move_heading % 24`. Inside mh.exe the over-index lands
// in the game's own .bss where the other 23 rows really are, so every read was correct and no hash,
// shadow site or A/B could see anything wrong. The standalone host binds each region into its own
// allocation, so the same read took the NEXT region's bytes and LIB-REF's replay diverged at step 3
// in two bytes. A caution note about the short Ghidra type had existed for five weeks; a comment is
// not a gate.
//
// Included at the end of the state binder that names these members, and OPENED IN THAT MODULE'S
// NAMESPACE: the stride constants (MICROSTEPS_PER_HEADING, UNITS_PER_PLAYER, ...) and the element
// types are declared in mh::sim, so an assertion emitted anywhere else cannot name either of them.
#pragma once

namespace mh::sim {
"""


def render(rows):
    scope = in_scope_constants()
    reach = region_reach()
    L = [BANNER]
    types = in_scope_types()

    def assertable(r):
        # reach 0 means the region has NO MEASUREMENT AT ALL, which gen_state_registry already
        # reports as UNMEASURED. That is a different defect from a claim that is one row short, and
        # asserting on it would make this gate red for a reason it does not own -- so it is listed.
        if reach.get(r[0], 0) == 0:
            return False
        return name_in_scope(r[4], scope) and elem_is_nameable(r[3], types)

    skipped = [r for r in rows if not assertable(r)]
    if skipped:
        L.append("// NOT ASSERTED -- the stride constant or the element type is not nameable in")
        L.append("// sim_state.h, so it is not in scope where these assertions land. LISTED rather")
        L.append("// than dropped, so the population is always visible: hoisting the constant or")
        L.append("// the type into sim_state.h is what makes one of these assertable.")
        for rid, name, member, elem, stride, site in skipped:
            if reach.get(rid, 0) == 0:
                # No measured extent at all -- gen_state_registry reports it as UNMEASURED. A
                # different defect from a one-row claim, and not this gate's to call red.
                why = "UNMEASURED"
            elif not name_in_scope(stride, scope):
                why = "stride"
            else:
                why = "element type"
            L.append(
                "//   %-42s %-12s out of scope: %-28s %s" % (name, why, stride + " / " + elem, site)
            )
        L.append("")
    for rid, name, member, elem, stride, site in rows:
        if not assertable((rid, name, member, elem, stride, site)):
            continue
        L.append("// %s -- %s, indexed [row * %s + i] at %s" % (name, member, stride, site))
        L.append(
            "static_assert(::mh::state::reach_of(::mh::state::%s) > (%s) * sizeof(%s),"
            % (rid, stride, elem)
        )
        L.append(
            '              "%s is claimed at ONE ROW or less of the stride it is indexed by: '
            "only row 0 is in the region. The stride and the index site are named in the comment "
            "above. Widen the claim -- a `size` on its view entry in "
            'tools/data/dll_addr_manifest.json is the Tree/MOVE_MICROSTEPS precedent.");' % name
        )
        L.append("")
    L.append("} // namespace mh::sim")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail if the committed header is stale")
    ap.add_argument("--report", action="store_true", help="the derived triples")
    args = ap.parse_args()

    rows = derive()
    text = render(rows)
    if args.report:
        for rid, name, member, elem, stride, site in rows:
            print("  %-44s %-20s stride %-24s %s" % (name, member, stride, site))
    if args.check:
        if not os.path.exists(OUT) or io.open(OUT, encoding="utf-8").read() != text:
            print(
                "[lint_strided_regions] FAIL: %s is stale -- rerun tools/lint_strided_regions.py"
                % os.path.relpath(OUT, REPO)
            )
            return 1
        scope, types, reach = in_scope_constants(), in_scope_types(), region_reach()
        n = sum(
            1
            for r in rows
            if reach.get(r[0], 0) and name_in_scope(r[4], scope) and elem_is_nameable(r[3], types)
        )
        print(
            "[lint_strided_regions] OK: %d strided region(s), %d asserted, %d listed "
            "out-of-scope; header current" % (len(rows), n, len(rows) - n)
        )
        return 0
    io.open(OUT, "w", encoding="utf-8", newline="\n").write(text)
    scope, types, reach = in_scope_constants(), in_scope_types(), region_reach()
    asserted = sum(
        1
        for r in rows
        if reach.get(r[0], 0) and name_in_scope(r[4], scope) and elem_is_nameable(r[3], types)
    )
    print(
        "wrote %s (%d strided region(s): %d asserted, %d listed out-of-scope)"
        % (os.path.relpath(OUT, REPO), len(rows), asserted, len(rows) - asserted)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
