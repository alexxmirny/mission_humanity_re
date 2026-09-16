#!/usr/bin/env python3
"""gen_tombstones.py -- emit mh/addr/mh_tombstones.gen.h: the extents of every ledger-DEAD body.

X-TOMB (the endgame plan D-E2). The tombstone instrument fills two kinds
of original body with traps and reports any entry into one:

  * bodies PROMOTED this run -- enumerated at RUNTIME from the note_promoted registry joined
    against mh::addr::promotable_ranges (mh_patches.gen.h), so nothing is generated for them here;
  * bodies every migration ledger records as `dead` -- an evidenced NOTHING-REACHES-IT. Those are
    static claims, and this generator is what turns them into a table the DLL can arm, converting
    "evidence says nothing reaches it" into a check that fires by name if the evidence was wrong.

Extents come from tools/data/en_functions.json (the authority gen_dll_patches.py also uses), NOT
from the ledgers' own size_bytes -- a ledger row's size can go stale against a re-dump, and the
fill must never run past the real body. A dead row whose name en_functions cannot resolve, or
resolves ambiguously, FAILS the generation rather than being silently dropped.

A FLAT [entry..end] IS NOT SAFE TO FILL (learned 2026-09-01, the arm_dead false hit): this Watcom
binary shares epilogue TAILS across functions (live siblings JMP into a dead body's last bytes --
llm_strat_ai_build_target_list) and lays out DISJOINT bodies (a live thunk sits inside
llm_strat_ai_expand_adjacent_mine_relay's flat span). 161 functions carry such hazards
(tools/data/interior_flow_refs.json, from the Ghidra-side interior-flow-ref dump). So
every armed range is CLAMPED to a SAFE END -- the byte before the first gap or the first
interior address with an inbound outside flow ref -- and a `tombstone_tail_caps[]` table is
emitted so the RUNTIME arms (promoted remainders, force_arm) clamp the same way. Missing hazard
data FAILS generation: silently arming unclamped is the bug this exists to prevent. The dead
FUNCTION claims themselves stand -- nothing reaches any entry; only the byte-range model was
wrong. Ref-manager blindness caveat: a COMPUTED landing into a tail is invisible to this data
(a reference manager cannot see one) -- one reason arm_dead stays an opt-in audit.

Usage:
  python tools/gen_tombstones.py            # write the header
  python tools/gen_tombstones.py --check    # drift gate (lint_repo.py runs this)
"""

import argparse
import glob
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FUNCS = os.path.join(REPO, "tools", "data", "en_functions.json")
HAZARDS = os.path.join(REPO, "tools", "data", "interior_flow_refs.json")
# X-SPINE (B), 2026-09-10: the section-5 RESIDUE -- rows whose ORIGINAL body stays reachable from
# host code ON PURPOSE. THE SAME FILE report_promotion_reconciliation.py's full `--check` reads, and
# that is the whole design: the arming exclusion and the written adjudication cannot drift apart,
# because there is only one of them.
RESIDUE = os.path.join(REPO, "tools", "data", "reconciliation_hostreach_residue.json")
LEDGER_GLOB = os.path.join(REPO, "tools", "data", "*_migration.json")
OUT = os.path.join(REPO, "src", "mh_dll", "mh", "addr", "mh_tombstones.gen.h")


def load_hazards():
    """-> {entry_int: safe_end_int} for every hazard function; None if the data file is absent."""
    if not os.path.exists(HAZARDS):
        return None
    doc = json.load(open(HAZARDS, encoding="utf-8"))
    caps = {}
    for entry_hex, h in doc.get("hazards", {}).items():
        entry = int(entry_hex, 16)
        ranges = [(int(a, 16), int(b, 16)) for a, b in h["body_ranges"]]
        ranges.sort()
        cands = []
        if len(ranges) > 1:
            cands.append(ranges[0][1])  # arm only up to the first gap
        for r in h.get("interior_inbound", ()):
            cands.append(int(r["dest"], 16) - 1)  # stop before the shared tail
        if cands:
            caps[entry] = (min(cands), h["name"])
    return caps


def load_extents():
    doc = json.load(open(FUNCS, encoding="utf-8"))
    by_name = {}
    for row in doc["functions"]:
        by_name.setdefault(row["name"], []).append(row)
        ident = re.sub(r"\W", "_", row["name"])
        if ident != row["name"]:
            by_name.setdefault(ident, []).append(row)
    return by_name


def dead_rows():
    """[(name, domain, ledger_addr)] for every `state: dead` row in every migration ledger."""
    out = []
    for path in sorted(glob.glob(LEDGER_GLOB)):
        try:
            doc = json.load(open(path, encoding="utf-8"))
        except (OSError, ValueError):
            continue
        fns = doc.get("functions")
        if not isinstance(fns, list):
            continue
        domain = doc.get("_domain") or os.path.basename(path).replace("_migration.json", "")
        for row in fns:
            if row.get("state") == "dead":
                out.append((row["name"], domain, row.get("addr", "?")))
    return out


def residue_rows():
    """[(name, class, why)] for every section-5 dispositioned residue row.

    WHY THE TOMBSTONE MUST SKIP THESE (measured 2026-09-10, and it is the reason this exists). A
    `verified` row whose original stays reachable ON PURPOSE is trapped by `force_arm_domain=` like
    any other, so the sweep goes red the moment a scenario takes that row's recorded forcing edge --
    `tact_panel` loads a planet map, the un-promoted `map_LoadPlanetFromDisk` runs, and it calls the
    original `llm_map_fog_of_war_recompute`. Nothing is wrong there: the disposition says exactly
    that will happen. What was wrong was the CLAIM -- three scenarios passed and a fourth did not, so
    "tombstone_full is green" was a statement about which scenarios someone ran.

    Excluding them makes the green STRUCTURAL instead of scenario-selected. The exclusion is derived
    from the adjudication file rather than listed here, so an exclusion can never outlive its reason:
    delete a disposition and this table loses the row on the next regen (and the drift gate fails
    until then), while the reconciliation's own `--check` fails by name for the undispositioned row.
    One file, two consumers, no third place to forget.
    """
    doc = json.load(open(RESIDUE, encoding="utf-8"))
    return [(r["name"], r["class"], r["why"]) for r in doc["dispositions"]]


def render():
    extents = load_extents()
    caps = load_hazards()
    rows, problems = [], []
    seen = set()
    if caps is None:
        problems.append(
            "tools/data/interior_flow_refs.json is MISSING -- run the Ghidra-side dump "
            "(mh_dump_interior_refs.py) first; arming unclamped ranges is the 2026-09-01 bug"
        )
        caps = {}
    for name, domain, ledger_addr in dead_rows():
        if name in seen:
            continue  # the same body can be adjudicated dead in two ledgers; one tombstone suffices
        seen.add(name)
        hits = extents.get(name)
        if not hits:
            problems.append(f"dead row {name} ({domain}) -- no such function in en_functions.json")
            continue
        if len(hits) > 1:
            problems.append(f"dead row {name} ({domain}) -- {len(hits)} functions share that name")
            continue
        if ledger_addr != "?" and int(ledger_addr, 16) != int(hits[0]["entry"], 16):
            problems.append(
                f"dead row {name} ({domain}) -- ledger addr {ledger_addr} != en_functions entry "
                f"{hits[0]['entry']} (stale ledger or renamed function; re-derive before arming)"
            )
            continue
        entry_i, end_i = int(hits[0]["entry"], 16), int(hits[0]["end"], 16)
        clamped = ""
        if entry_i in caps and caps[entry_i][0] < end_i:
            clamped = " CLAMPED from %s (shared tail / disjoint body)" % hits[0]["end"]
            end_i = caps[entry_i][0]
        rows.append((name, hits[0]["entry"], "0x%08x" % end_i, domain + clamped))
    if problems:
        for p in problems:
            print("gen_tombstones: " + p, file=sys.stderr)
        return None
    rows.sort(key=lambda r: int(r[1], 16))

    w = max((len(r[0]) for r in rows), default=1)
    lines = [
        "// GENERATED by tools/gen_tombstones.py -- DO NOT EDIT.",
        "// Regenerate after a ledger adjudicates a row `dead` or after re-dumping",
        "// tools/data/en_functions.json; `gen_tombstones.py --check` is the drift gate",
        "// (lint_repo.py runs it).",
        "//",
        "// The extents of every function a migration ledger records as DEAD -- an evidenced",
        "// nothing-reaches-it. The tombstone instrument (hook/tombstone.cpp) fills these bodies with",
        "// traps when [tombstone] arm_dead=1, so a wrong `dead` claim fires by name instead of",
        "// staying an unread line in a ledger. Promoted bodies are NOT listed here: they are",
        "// enumerated at runtime from the note_promoted registry against promotable_ranges.",
        "#pragma once",
        "#include <cstdint>",
        "",
        '#include "hook/promoted.h"',
        "",
        "namespace mh::addr {",
        "",
        "// Entry..end INCLUSIVE, EN build (/eng/mh.exe), image base 0x00400000, no ASLR.",
        "inline constexpr mh::hook::owner_range tombstone_dead_ranges[] = {",
    ]
    for name, entry, end, domain in rows:
        lines.append(f'    {{"{name}",{" " * (w - len(name))} {entry}u, {end}u}}, // {domain}')
    cap_rows = sorted(((e, se, nm) for e, (se, nm) in caps.items()), key=lambda t: t[0])
    cw = max((len(nm) for _, _, nm in cap_rows), default=1)
    lines += [
        "};",
        f"inline constexpr int tombstone_dead_range_count = {len(rows)};",
        "",
        "// SAFE-END caps for the RUNTIME arms (promoted remainders, force_arm): for these",
        "// functions the flat [entry..end] contains bytes that are NOT exclusively theirs -- a",
        "// disjoint-body gap holding another function, or a shared epilogue tail that outside",
        "// code jumps into (tools/data/interior_flow_refs.json). Arming must stop at `end` here.",
        "inline constexpr mh::hook::owner_range tombstone_tail_caps[] = {",
    ]
    for e, se, nm in cap_rows:
        lines.append(f'    {{"{nm}",{" " * (cw - len(nm))} 0x{e:08x}u, 0x{se:08x}u}},')
    res_rows = []
    for nm, cls, why in residue_rows():
        hits = extents.get(nm)
        if not hits or len(hits) > 1:
            # Same refusal as a dead row: a residue row we cannot locate is an exclusion that
            # silently does not apply, i.e. the sweep goes red again with no line saying why.
            problems.append(
                "residue row %s -- %s in en_functions.json"
                % (nm, "no such function" if not hits else "%d matches" % len(hits))
            )
            continue
        res_rows.append((nm, hits[0]["entry"], cls, why))
    if problems:
        for p in problems:
            print("gen_tombstones: " + p, file=sys.stderr)
        return None
    res_rows.sort(key=lambda r: int(r[1], 16))
    rw = max((len(r[0]) for r in res_rows), default=1)
    lines += [
        "};",
        f"inline constexpr int tombstone_tail_cap_count = {len(cap_rows)};",
        "",
        "// SECTION-5 RESIDUE -- rows the force-arm paths must NOT trap, derived from",
        "// tools/data/reconciliation_hostreach_residue.json (the same file the promotion",
        "// reconciliation's full `--check` reads, so an exclusion cannot outlive its adjudication).",
        "// Each is a `verified` row whose ORIGINAL body stays reachable from host code ON PURPOSE;",
        "// trapping one turns a decided end state into a red run whose redness depends on which",
        "// scenario you happened to run. The runtime PRINTS every skip with its class -- a silent",
        "// exclusion is the drift this table exists to prevent.",
        "inline constexpr mh::hook::residue_row tombstone_residue_exclusions[] = {",
    ]
    for nm, entry, cls, why in res_rows:
        lines.append(f'    {{"{nm}",{" " * (rw - len(nm))} {entry}u, "{cls}",\n     "{why}"}},')
    lines += [
        "};",
        f"inline constexpr int tombstone_residue_exclusion_count = {len(res_rows)};",
        "",
        "} // namespace mh::addr",
        "",
    ]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail if the header is stale")
    args = ap.parse_args()

    text = render()
    if text is None:
        return 1
    if args.check:
        cur = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        if cur.replace("\r\n", "\n") != text:
            print("gen_tombstones: mh_tombstones.gen.h is STALE -- rerun tools/gen_tombstones.py")
            return 1
        print(
            f"gen_tombstones: up to date ({text.count(chr(10) + '    {')} rows: dead ranges + tail caps)"
        )
        return 0
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
