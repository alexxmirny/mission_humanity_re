#!/usr/bin/env python3
"""gen_boot_snapshot.py -- the LIB-BOOT post-cfg snapshot SCHEMA, derived and fully accounted.

WHAT IT GATES (tracker LIB-BOOT). libmh cannot own the 65 KB cfg parser, so it imports that
parser's OUTPUT instead. The question that decides whether the import is honest is not "does the
hash match" -- a hash over three blocks matches too. It is: WHICH REGIONS DOES THE CFG LOAD WRITE,
and is every one of them either in the snapshot or excluded for a stated reason. So this file
derives that population every run and prints the unaccounted count, which must be ZERO.

THE POPULATION IS DERIVED, NEVER LISTED. It is the WRITE CLOSURE of boot stage 5's root --
`InitSafe` (reached from `llm_boot_stage_tick`; `cfg_Init` is its only callee) -- taken over the
committed call graph and tmp/state_matrix.json, exactly as the write-closure derivation takes a
shadow site's closure. A hand-listed population works once and then rots; a derived one silently
GAINS and LOSES members, so the delta against the disposition file is what `--check` reports and a
gained member arrives UNACCOUNTED and turns the gate red.

THE FOUR VERDICTS, and the first three are DERIVED rather than declared:

  carry-direct   the region has a live reader outside the cfg cluster, so its content outlives the
                 parse and the snapshot must carry it.
  carry-covered  the region's bytes lie inside a region already carried (`Tree`'s claim reaches
                 past three matrix regions; `_G_LLM_ANIM_PLACE_ALLOWED` sits inside the save
                 block's extent for `_G_LLM_ANIM_PLACE_DENIED`). Carrying it separately would
                 double-carry the same bytes, so it is accounted, not added.
  carry-added    a region the derivation could NOT produce, carried on named outside evidence.
                 Two exist and both are recorded, not guessed: `Stones` (the matrix has no region
                 entry for that address AT ALL, so no cell can attribute cfg_ConstructStones'
                 write -- found only by diffing retail's own dead snapshot pair) and the tutorial
                 step table (a cfg-CLASS parse that runs at tutorial start, not at boot).
  exclude:<cls>  a stated class + a `why`, in tools/data/boot_snapshot_dispositions.json.

WHY THE READER RULE IS NOT ENOUGH ON ITS OWN, and this is the trap that shaped the design.
`G_TEXT_BLOCK` -- 300,000 bytes, the largest single item the cfg load produces -- has ZERO readers
by address. Every consumer dereferences a pointer out of `G_TEXT_PTRS`. A reader-based rule buries
it in the exclude bucket, and the snapshot then imports 806 pointers into an arena that was never
carried. That is the ref-manager-blindness class, and the answer here is a
`pointed-into` carry class whose declaration is CHECKED AT CAPTURE: the capture tool scans every
carried block for dwords landing inside the region and reports the count, so a declaration that has
stopped being true stops being green.

TWO SEPARATE THINGS THIS FILE EMITS:
  * the SCHEMA (tools/data/boot_snapshot_schema.json) -- the derived answer, committed so `--check`
    runs on a machine with no tmp/state_matrix.json, the same contract gen_dll_shadow.py has.
  * the BLOCK TABLE (src/mh_dll/mh/addr/mh_boot_snapshot.gen.h) -- region ids + extents, which is
    what the capture walks and the importer writes through. Carried regions are addressed BY RID,
    which is why a carried region that is not in the registry is a hard refusal: the importer
    resolves through `live_base(rid)`, so a region with no rid cannot be written into a host's
    memory at all.

Usage:
    python tools/gen_boot_snapshot.py --refresh   # re-derive the closure -> the schema data file
    python tools/gen_boot_snapshot.py             # schema -> addr/mh_boot_snapshot.gen.h
    python tools/gen_boot_snapshot.py --check     # accounting + drift gate (tools/lint_repo.py)
    python tools/gen_boot_snapshot.py --report    # the accounting table + the retail diff
    python tools/gen_boot_snapshot.py --selftest  # prove each refusal fires, and does not over-fire
"""

from __future__ import annotations

import argparse
import collections
import copy
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))

from call_graph import load_graph, reachable  # noqa: E402

MATRIX = os.path.join(REPO, "tmp", "state_matrix.json")
REGIONS = os.path.join(REPO, "tools", "data", "state_regions.json")
DISPOSITIONS = os.path.join(REPO, "tools", "data", "boot_snapshot_dispositions.json")
SCHEMA = os.path.join(REPO, "tools", "data", "boot_snapshot_schema.json")
HEADER = os.path.join(REPO, "src", "mh_dll", "mh", "addr", "mh_boot_snapshot.gen.h")

# Boot stage 5's root. `llm_boot_stage_tick` calls it once per process; the stage machine cannot
# repeat (_G_LLM_BOOT_STAGE has an init writer and its own INC), which is the property that makes a
# one-shot snapshot of this closure's output sound and makes the same trick illegal for anything
# session-scoped.
CFG_ROOT = "InitSafe"

# The deferred cfg-CLASS parse: same shape (a text file parsed once into a table that is read-only
# afterwards) but triggered at tutorial start rather than at boot. The snapshot capture calls it
# once at stage 9 and RESTORES everything it writes, so its restore set is this closure, derived.
DEFERRED_ROOT = "llm_tutorial_load_script"

# A read from the shutdown path is not a consumer. `FreeGlobalData`'s only caller is
# `llm_fatal_cleanup`; what it does with a parse-time class descriptor is free() it. Counting it as
# a reader would carry a dozen heap pointers into a process where they mean nothing.
SHUTDOWN_READERS = ("FreeGlobalData", "llm_fatal_cleanup")

WRITE_KINDS = ("write", "rw", "movs")


def _load(path):
    with open(path, encoding="utf-8-sig") as fh:
        return json.load(fh)


# ---- the derivation ---------------------------------------------------------------------------


def cluster(root):
    """The function set reachable from `root` over the committed graph (incl. tail jumps)."""
    name_by_id, ids_by_name, succ = load_graph()
    if root not in ids_by_name:
        sys.exit("gen_boot_snapshot: %s is not in the committed call graph" % root)
    ids = reachable(ids_by_name[root], succ)
    return {name_by_id[i] for i in ids if i in name_by_id}


def matrix_access(m):
    """(writers, readers) as {region: {fn}} over the state matrix's cells."""
    writers, readers = collections.defaultdict(set), collections.defaultdict(set)
    for c in m["cells"]:
        n = c["counts"]
        if any(n.get(k, 0) for k in WRITE_KINDS):
            writers[c["region"]].add(c["fn"])
        if n.get("read", 0) or n.get("rw", 0):
            readers[c["region"]].add(c["fn"])
    return writers, readers


def derive():
    """Re-derive the closure and the per-region reader evidence. Needs tmp/state_matrix.json."""
    m = _load(MATRIX)
    writers, readers = matrix_access(m)
    fns = cluster(CFG_ROOT)
    meta, sizes = m["regions"], m["sizes"]

    universe = {}
    for region, ws in writers.items():
        if not (ws & fns):
            continue
        ext = sorted(readers[region] - fns - set(SHUTDOWN_READERS))
        universe[region] = {
            "addr": (meta.get(region) or {}).get("addr"),
            "size": sizes.get(region, 0),
            "external_readers": ext,
            "shutdown_only": bool(readers[region] & set(SHUTDOWN_READERS)) and not ext,
        }

    restore = {}
    dfns = cluster(DEFERRED_ROOT)
    for region, ws in writers.items():
        if not (ws & dfns):
            continue
        restore[region] = {
            "addr": (meta.get(region) or {}).get("addr"),
            "size": sizes.get(region, 0),
        }
    return universe, restore


# ---- registry resolution ----------------------------------------------------------------------


def registry():
    rows = _load(REGIONS)["regions"]
    for r in rows:
        r["reach"] = max(r.get("extent", 0), r.get("size", 0))
    return rows


def rid_covering(rows, addr, size):
    """The registry region whose [base, base+reach) contains [addr, addr+size). None if no one does.

    Containment of the WHOLE window, not just its first byte: a region straddling two registry
    entries is not covered by either, and pretending otherwise is how a snapshot block would write
    half its bytes into a neighbour.
    """
    if addr is None:
        return None
    a = int(addr, 16)
    n = max(size, 1)
    for r in rows:
        if r["base"] <= a and a + n <= r["base"] + max(r["reach"], 1):
            return r
    return None


# ---- the accounting ---------------------------------------------------------------------------


def account(universe, restore, disp, rows):
    """(schema, findings). One verdict per universe member; a finding per gap or ill-formed entry."""
    findings = []
    classes = disp["exclusion_classes"]
    entries = disp["regions"]
    added = disp["carry_added"]

    # PASS 1 -- the DERIVED verdict: a live reader outside the cluster means the content outlives
    # the parse. This is the only tier nothing declares.
    derived = {n: "carry-direct" for n, v in universe.items() if v["external_readers"]}

    # PASS 2 -- the DECLARED verdict wins, and the interesting case is an exclusion that overrules a
    # derived carry-direct (WIDTH/HEIGHT: real readers, but at stage 9 they hold the LAST planet's
    # map dimensions, and LIB-WORLD owns the session's values). That is a legitimate answer and it
    # is also exactly how a needed region would get dropped by accident -- so a class may only
    # overrule a live reader if it says so, and the report prints every instance.
    verdict = dict(derived)
    for name, e in entries.items():
        if name not in universe:
            continue  # reported as stale below
        v = e.get("verdict")
        if v == "carry":
            verdict[name] = "carry-override:" + (e.get("class") or "?")
        elif v == "exclude":
            cls = e.get("class")
            verdict[name] = "exclude:" + (cls or "?")
            if derived.get(name) == "carry-direct" and not (classes.get(cls) or {}).get(
                "may_override_direct"
            ):
                findings.append(
                    "%s is EXCLUDED under class %r, but the derivation says carry-direct (%d live "
                    "reader(s) outside the cfg cluster, e.g. %s). Only a class declaring "
                    "`may_override_direct` may overrule a live reader -- otherwise this is how a "
                    "region the sim needs gets silently dropped."
                    % (
                        name,
                        cls,
                        len(universe[name]["external_readers"]),
                        ", ".join(universe[name]["external_readers"][:3]),
                    )
                )
        else:
            findings.append("%s: verdict %r is not carry or exclude." % (name, v))

    for name in added:
        verdict[name] = "carry-added"

    # PASS 3 -- COVERAGE. Two carried regions can resolve to ONE registry rid: the rid's reach is
    # what a host binds, so the block already carries both windows and a second block would write
    # the same bytes twice. The region whose window IS the rid keeps the block; the others are
    # accounted as carry-covered. Derived from addresses, not declared.
    def carried_now():
        out = {}
        for n, vv in verdict.items():
            if not vv.startswith("carry") or vv == "carry-covered":
                continue
            src = universe.get(n) or added.get(n) or {}
            r = rid_covering(rows, src.get("addr"), src.get("size", 0))
            if r is not None:
                out.setdefault(r["id"], []).append(n)
        return out

    for by_rid in (carried_now(),):
        for rid, names in by_rid.items():
            if len(names) < 2:
                continue
            r = next(x for x in rows if x["id"] == rid)
            keeper = None
            for n in names:
                src = universe.get(n) or added.get(n) or {}
                if src.get("addr") and int(src["addr"], 16) == r["base"]:
                    keeper = n
                    break
            keeper = keeper or sorted(names)[0]
            for n in names:
                if n != keeper:
                    verdict[n] = "carry-covered"

    # A region with no verdict at all is the finding this file exists to produce.
    for name in sorted(universe):
        if name in verdict:
            continue
        findings.append(
            "%s (%d B) is written by the cfg cluster, has no reader outside it, and carries NO "
            "disposition. A region the closure newly matched arrives exactly like this -- give "
            "it a verdict in boot_snapshot_dispositions.json." % (name, universe[name]["size"])
        )

    # Well-formedness of every entry that was consulted.
    for name, e in sorted(entries.items()):
        if name not in universe and name not in added:
            findings.append(
                "%s has a disposition but is not in the cfg cluster's write closure and is not a "
                "recorded carry_added. A reason that outlives its region reads as an accepted one "
                "forever -- delete it." % name
            )
        if e.get("verdict") == "exclude" and e.get("class") not in classes:
            findings.append(
                "%s: exclusion class %r is not one of %s -- a class with no definition cannot say "
                "where it holds." % (name, e.get("class"), sorted(classes))
            )
        if not (e.get("why") or "").strip():
            findings.append("%s: a disposition with no `why` is not an adjudication." % name)

    # THE RID RULE. The importer writes through live_base(rid); a carried region with no registry
    # entry cannot be addressed in a host's memory at all, so this is what makes the registry
    # additions mechanically complete rather than a thing someone remembered to do.
    blocks = []
    for name in sorted(verdict):
        v = verdict[name]
        if not v.startswith("carry") or v == "carry-covered":
            continue
        src = universe.get(name) or added.get(name) or {}
        r = rid_covering(rows, src.get("addr"), src.get("size", 0))
        if r is None:
            findings.append(
                "%s is CARRIED but resolves to no registry region -- the importer addresses blocks "
                "by rid through live_base(), so this block cannot be written. Add it to "
                "tools/data/dll_addr_manifest.json (and declare its owner)." % name
            )
            continue
        blocks.append(
            {
                "region": name,
                "rid": r["id"],
                "rid_name": r["name"],
                "base": "0x%08x" % r["base"],
                "len": r["reach"],
                "verdict": v,
            }
        )

    seen = collections.Counter(b["rid"] for b in blocks)
    for rid, n in sorted(seen.items()):
        if n > 1:
            findings.append(
                "rid %s is claimed by %d carried regions -- one block per rid, or the same bytes "
                "are written twice." % (rid, n)
            )

    # The restore set: derived from the deferred parse's own closure, so it cannot miss a region.
    rdisp = disp["restore"]
    restore_rows = []
    for name in sorted(restore):
        e = rdisp.get(name)
        if e is None:
            findings.append(
                "%s is written by %s and has no restore disposition. The capture CALLS that parser "
                "at boot stage 9, so every region it writes is either restored or carries a stated "
                "reason why it need not be." % (name, DEFERRED_ROOT)
            )
            continue
        if not (e.get("why") or "").strip():
            findings.append("restore %s: no `why`." % name)
        r = rid_covering(rows, restore[name]["addr"], restore[name]["size"])
        if e.get("restore") and r is None:
            findings.append(
                "%s must be restored but resolves to no registry region -- the restore walks rids "
                "like the block table does." % name
            )
        restore_rows.append(
            {
                "region": name,
                "rid": r["id"] if r else None,
                "len": r["reach"] if r else restore[name]["size"],
                "restore": bool(e.get("restore")),
                "class": e.get("class"),
            }
        )
    for name in sorted(rdisp):
        if name not in restore:
            findings.append(
                "restore entry %s names a region %s does not write any more -- stale."
                % (name, DEFERRED_ROOT)
            )

    unaccounted = sum(1 for n in universe if n not in verdict)
    schema = {
        "_generated_by": "tools/gen_boot_snapshot.py --refresh",
        "_do_not_hand_edit": "verdicts are derived; the reasons live in boot_snapshot_dispositions.json",
        "cfg_root": CFG_ROOT,
        "deferred_root": DEFERRED_ROOT,
        "universe": sorted(universe),
        "verdicts": {k: verdict[k] for k in sorted(verdict) if k in universe},
        "blocks": sorted(blocks, key=lambda b: b["rid"]),
        "restore": sorted(restore_rows, key=lambda r: r["region"]),
        "unaccounted": unaccounted,
    }
    return schema, findings


# ---- the retail cross-check --------------------------------------------------------------------
#
# llm_cfg_save_final_snapshot @0x004489a7 LZW-writes NINE baked cfg::final tables to init\<name>.
# Both it and its loader are dead (zero call sites), but they are an INDEPENDENT answer to "what is
# a post-cfg snapshot" written by the people who shipped the parser -- so the diff is evidence, and
# it has already paid for itself twice (Stones, and Tree's size).
RETAIL_BLOCKS = [
    ("Unit", 0xE09C, "Unit", None),
    ("Building", 0x339C8, "Building", None),
    ("Progress", 0x78B4, "Progress", None),
    ("Weapon", 0x2D80, "Weapon", None),
    ("&Stones", 800, "Stones", None),
    # THE TWO THIS DIFF FOUND. `Tree` agrees only because the retail literal is where our 4400 came
    # from -- the state matrix measures 44 and the Ghidra retype is still outstanding
    # `&Stones` agrees only because the retail literal is the ONLY evidence the
    # region exists at all; the matrix has no entry for that address.
    ("Tree", 0x1130, "Tree", None),
    ("&System", 0x230, "System", None),
    ("Planets", 0x84E0, "Planets", None),
    # EXPECTED, NOT A DEFECT: retail writes from `Anim + 1`, i.e. it skips element 0 of a
    # 0x10-byte-stride array. 40016 - 16 = 40000 exactly. Ours is the superset.
    ("Anim + 1", 40000, "Anim", 16),
]


def retail_diff(schema, rows):
    by_region = {b["region"]: b for b in schema["blocks"]}
    out = []
    for label, size, region, skew in RETAIL_BLOCKS:
        b = by_region.get(region)
        if b is None:
            out.append((label, size, None, "RETAIL-ONLY -- not a carried block"))
        elif b["len"] == size:
            out.append((label, size, b["len"], "agree"))
        elif skew is not None and b["len"] - size == skew:
            out.append(
                (label, size, b["len"], "agree (retail skips element 0; ours is the superset)")
            )
        else:
            out.append((label, size, b["len"], "SIZE DIFFERS"))
    ours = sorted(set(by_region) - {r[2] for r in RETAIL_BLOCKS})
    return out, ours


# ---- the emitted header -------------------------------------------------------------------------


def header_text(schema):
    L = [
        "//",
        "// addr/mh_boot_snapshot.gen.h -- GENERATED by tools/gen_boot_snapshot.py from",
        "// tools/data/boot_snapshot_schema.json. DO NOT EDIT: regenerate.",
        "//",
        "// THE POST-CFG SNAPSHOT BLOCK TABLE (LIB-BOOT). The cfg->prototype load runs once per",
        "// process and its output is read-only afterwards, so libmh imports that output instead of",
        "// owning the 65 KB parser. This table is WHAT gets carried: one block per carried region,",
        "// addressed BY RID so the capture reads and the importer writes through live_base() -- the",
        "// stock .bss in the hosted build, the host's own allocation standalone.",
        "//",
        "// Every region the cfg cluster writes is either a block here or carries a stated exclusion",
        "// in tools/data/boot_snapshot_dispositions.json; the unaccounted count is derived on every",
        "// --check and is %d." % schema["unaccounted"],
        "//",
        "// THE WALK IS state_sink/state_source IN PERSIST MODE, not region_view::emit_slice, and the",
        "// difference is load-bearing rather than stylistic: emit_slice is indexed by HASH_REGIONS[],",
        "// which covers only the determinism set. Most of the table below is MF_VIEW/MF_SAVE only --",
        "// Progress, Building, Unit, Anim, Projects, Weapon and Upgrades are all outside the hash --",
        "// so routing this walk through emit_slice would silently drop the majority of the snapshot.",
        "// It is also why the snapshot's own hash is NOT the determinism hash and CAN see a wrong",
        "// Progress, which is the G128 hole this item's negative arm had to close.",
        "//",
        "#pragma once",
        "#include <cstdint>",
        "",
        '#include "addr/mh_regions.gen.h"',
        "",
        "namespace mh::state {",
        "",
        "struct boot_snapshot_block {",
        "    region_id   rid;",
        "    uint32_t    len;  // the region's REACH -- what a host binds (host_bind.cpp note 2)",
        "    const char *name; // the Ghidra symbol, for the capture/import log and the arms",
        "};",
        "",
        "inline constexpr int BOOT_SNAPSHOT_BLOCK_COUNT = %d;" % len(schema["blocks"]),
        "",
        "inline constexpr boot_snapshot_block BOOT_SNAPSHOT_BLOCKS[BOOT_SNAPSHOT_BLOCK_COUNT] = {",
    ]
    # The trailing comments are pre-ALIGNED here rather than left ragged, because clang-format
    # aligns them and the emitted file is checked with `--dry-run -Werror`: a generator whose output
    # its own formatter would rewrite makes the drift gate permanently red.
    entries = [
        '    {RID_%s, %uu, "%s"},' % (b["rid"], b["len"], b["rid_name"]) for b in schema["blocks"]
    ]
    col = max(len(e) for e in entries) + 1 if entries else 0
    for e, b in zip(entries, schema["blocks"]):
        L.append("%-*s// %s" % (col, e, b["verdict"]))
    L += [
        "};",
        "",
        "// The regions the capture's ONE call to llm_tutorial_load_script writes and must put back,",
        "// derived from that function's own write closure rather than listed -- a restore set that is",
        "// hand-listed is one that misses the region added next year.",
    ]
    rest = [r for r in schema["restore"] if r["restore"]]
    L += [
        "inline constexpr int BOOT_SNAPSHOT_RESTORE_COUNT = %d;" % len(rest),
        "",
        "inline constexpr boot_snapshot_block BOOT_SNAPSHOT_RESTORE[BOOT_SNAPSHOT_RESTORE_COUNT] = {",
    ]
    for r in rest:
        L.append('    {RID_%s, %uu, "%s"},' % (r["rid"], r["len"], r["region"]))
    L += [
        "};",
        "",
        "} // namespace mh::state",
        "",
    ]
    return "\n".join(L)


# ---- reporting / CLI -----------------------------------------------------------------------------


def report(schema, disp, rows):
    v = schema["verdicts"]
    counts = collections.Counter(x.split(":")[0] for x in v.values())
    total = sum(b["len"] for b in schema["blocks"])
    print(
        "BOOT SNAPSHOT: %d region(s) in the cfg cluster's write closure (root %s); %d carried "
        "block(s), %d bytes."
        % (len(schema["universe"]), schema["cfg_root"], len(schema["blocks"]), total)
    )
    for k, n in sorted(counts.items()):
        print("  %-16s %d" % (k, n))
    print("  UNACCOUNTED      %d" % schema["unaccounted"])
    print("\n  %-34s %-10s %9s  %s" % ("block (rid)", "verdict", "bytes", "symbol"))
    for b in sorted(schema["blocks"], key=lambda b: -b["len"]):
        print(
            "  %-34s %-10s %9d  %s"
            % (b["rid"], b["verdict"].split(":")[0], b["len"], b["rid_name"])
        )
    by_class = collections.defaultdict(list)
    for name, verdict in sorted(v.items()):
        if verdict.startswith("exclude:"):
            by_class[verdict.split(":", 1)[1]].append(name)
    print("\n  EXCLUSIONS by class:")
    for cls, names in sorted(by_class.items()):
        meta = disp["exclusion_classes"][cls]
        tag = "  [MAY OVERRULE A LIVE READER]" if meta.get("may_override_direct") else ""
        print("    %-28s %2d%s" % (cls, len(names), tag))
        print("        %s" % meta["why"])
        print("        %s" % ", ".join(names))
    print("\n  RESTORE set (%s's own write closure):" % schema["deferred_root"])
    for r in schema["restore"]:
        print(
            "    %-38s %-8s %s"
            % (r["region"], "restore" if r["restore"] else r["class"], r["rid"] or "(no rid)")
        )
    diff, ours_only = retail_diff(schema, rows)
    print("\n  RETAIL CROSS-CHECK (llm_cfg_save_final_snapshot @0x004489a7, nine LZW'd blocks):")
    for label, size, mine, verdict in diff:
        print(
            "    %-12s retail %7d   ours %-8s  %s" % (label, size, mine if mine else "-", verdict)
        )
    print("    ours and NOT retail's: %s" % ", ".join(ours_only))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--refresh", action="store_true", help="re-derive the closure into the schema")
    ap.add_argument("--check", action="store_true", help="accounting + drift gate (lint_repo)")
    ap.add_argument("--report", action="store_true", help="the accounting table + the retail diff")
    ap.add_argument("--selftest", action="store_true", help="prove each refusal fires")
    a = ap.parse_args()

    disp = _load(DISPOSITIONS)
    rows = registry()

    if a.selftest:
        return selftest(disp, rows)

    if a.refresh:
        if not os.path.exists(MATRIX):
            sys.exit(
                "gen_boot_snapshot --refresh needs tmp/state_matrix.json (mh_gen_state_matrix.py)"
            )
        universe, restore = derive()
        schema, findings = account(universe, restore, disp, rows)
        with open(SCHEMA, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(schema, fh, indent=1)
            fh.write("\n")
        print(
            "wrote %s (%d universe, %d blocks, %d unaccounted)"
            % (SCHEMA, len(schema["universe"]), len(schema["blocks"]), schema["unaccounted"])
        )
        for f in findings:
            print("  FINDING: %s" % f)
        return 1 if findings else 0

    schema = _load(SCHEMA)

    if a.report:
        report(schema, disp, rows)
        return 0

    if a.check:
        rc = 0
        # 1. the committed schema must still be well-formed against the committed dispositions.
        if schema["unaccounted"]:
            print(
                "boot snapshot: %d UNACCOUNTED region(s) in the committed schema"
                % schema["unaccounted"]
            )
            rc = 1
        # 2. the emitted header must match the schema.
        want = header_text(schema)
        have = open(HEADER, encoding="utf-8").read() if os.path.exists(HEADER) else ""
        if want != have:
            print("DRIFT: %s differs from a fresh regen -- run gen_boot_snapshot.py" % HEADER)
            rc = 1
        # 3. every carried rid must still exist in the registry at the recorded extent.
        by_id = {r["id"]: r for r in rows}
        for b in schema["blocks"]:
            r = by_id.get(b["rid"])
            if r is None:
                print("boot snapshot: rid %s is no longer a registry region" % b["rid"])
                rc = 1
            elif r["reach"] != b["len"]:
                print(
                    "boot snapshot: %s reach moved %d -> %d; --refresh and re-record"
                    % (b["rid"], b["len"], r["reach"])
                )
                rc = 1
        # 4. if the matrix is here, the derivation must still produce the committed answer.
        if os.path.exists(MATRIX):
            universe, restore = derive()
            fresh, findings = account(universe, restore, disp, rows)
            for f in findings:
                print("  FINDING: %s" % f)
            if findings:
                rc = 1
            if fresh["verdicts"] != schema["verdicts"] or fresh["blocks"] != schema["blocks"]:
                gained = sorted(set(fresh["verdicts"]) - set(schema["verdicts"]))
                lost = sorted(set(schema["verdicts"]) - set(fresh["verdicts"]))
                print(
                    "boot snapshot: the derived closure MOVED (gained %s, lost %s) -- --refresh"
                    % (gained or "none", lost or "none")
                )
                rc = 1
        else:
            print(
                "  (tmp/state_matrix.json absent -- closure re-derivation skipped, schema checked)"
            )
        if rc == 0:
            print(
                "boot snapshot: %d block(s), %d bytes, 0 unaccounted of %d cfg-cluster region(s); "
                "restore set %d region(s)"
                % (
                    len(schema["blocks"]),
                    sum(b["len"] for b in schema["blocks"]),
                    len(schema["universe"]),
                    sum(1 for r in schema["restore"] if r["restore"]),
                )
            )
        return rc

    with open(HEADER, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(header_text(schema))
    print(
        "wrote %s (%d blocks, %d restore)"
        % (HEADER, len(schema["blocks"]), sum(1 for r in schema["restore"] if r["restore"]))
    )
    return 0


def selftest(base, rows):
    """Each refusal fires on a mutated input, and none of them fires on the committed tree."""
    if not os.path.exists(MATRIX):
        print("SELFTEST SKIPPED: tmp/state_matrix.json absent -- the arms mutate a real derivation")
        return 0
    universe, restore = derive()
    derived = {n: "carry-direct" for n, v in universe.items() if v["external_readers"]}
    _schema, findings = account(universe, restore, base, rows)
    if findings:
        print("SELFTEST FAIL (committed tree): %d finding(s): %s" % (len(findings), findings[:3]))
        return 1
    print("ok: the committed tree has 0 findings")

    victim = next(n for n, e in base["regions"].items() if e.get("verdict") == "exclude")
    ok = True

    def arm(label, mutate, want):
        nonlocal ok
        d = json.loads(json.dumps(base))
        mutate(d)
        _s, f = account(universe, restore, d, rows)
        hit = any(want in x for x in f)
        print(("ok: %s" if hit else "SELFTEST FAIL: %s -- did not fire") % label)
        if not hit:
            print("     findings: %s" % f[:2])
        ok = ok and hit

    arm(
        "a cfg-cluster region with no disposition is refused",
        lambda d: d["regions"].pop(victim),
        "carries NO disposition",
    )
    arm(
        "an exclusion class with no definition is refused",
        lambda d: d["regions"][victim].update({"class": "vibes"}),
        "is not one of",
    )
    arm(
        "a disposition with an empty `why` is refused",
        lambda d: d["regions"][victim].update({"why": "   "}),
        "no `why`",
    )
    arm(
        "a disposition naming a region the closure no longer writes is STALE",
        lambda d: d["regions"].update(
            {
                "llm_no_such_region_fixture": {
                    "verdict": "exclude",
                    "class": "parse-cursor",
                    "why": "fixture",
                }
            }
        ),
        "is not in the cfg cluster's write closure",
    )
    arm(
        "a carried region with no registry rid is refused",
        lambda d: d["carry_added"].update(
            {"llm_no_such_carried_fixture": {"addr": "0x7ffe0000", "size": 16, "why": "fixture"}}
        ),
        "resolves to no registry region",
    )
    arm(
        "a region the deferred parse writes with no restore disposition is refused",
        lambda d: d["restore"].pop(next(iter(d["restore"]))),
        "has no restore disposition",
    )
    arm(
        "a restore entry naming a region the parse no longer writes is STALE",
        lambda d: d["restore"].update(
            {"llm_no_such_restore_fixture": {"restore": True, "why": "fixture"}}
        ),
        "stale",
    )

    # THE ARM THAT FIRED FOR REAL. `SYSTEM` was first written down as an ordinary pre-static-table
    # exclusion; the guard refused it because the derivation says it has a live reader, and the
    # answer was to CARRY it rather than to widen the class. Re-stage that exact mistake: demote a
    # carried region to an exclusion under a class that may not overrule a reader.
    direct_victim = next(
        n for n, vv in derived.items() if vv == "carry-direct" and n not in base["regions"]
    )
    arm(
        "excluding a region with live readers under a class that may not overrule one is refused",
        lambda d: d["regions"].update(
            {direct_victim: {"verdict": "exclude", "class": "parse-cursor", "why": "fixture"}}
        ),
        "Only a class declaring `may_override_direct`",
    )

    # THE OVER-REFUSAL ARM. Carrying a region that IS covered by another carried block must not be
    # reported as a duplicate rid, or the covered tier would be unusable.
    d = json.loads(json.dumps(base))
    _s, f = account(universe, restore, d, rows)
    if f:
        print("SELFTEST FAIL (over-refusal): the unmutated tree produced %s" % f[:2])
        ok = False
    else:
        print("ok: the unmutated tree produces no finding -- the gate is not red by design")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
