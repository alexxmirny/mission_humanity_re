#!/usr/bin/env python3
"""gen_world_snapshot.py -- the LIB-WORLD step-0 world SCHEMA, derived and fully accounted.

WHAT IT GATES (tracker LIB-WORLD). A standalone libmh has no cfg parser and no map-file reader, so
LIB-REF cannot start a replay unless something hands it a world. That something is a blob captured
at REPLAY STEP 0 alongside an order+clock recording: every bound region, dumped at the instant
before the first sim step runs. This file decides WHICH regions that is, and the question that makes
the fixture honest is not "does the hash match" -- a blob of three regions matches too. It is:
IS EVERY BOUND REGION EITHER IN THE BLOB OR EXCLUDED FOR A STATED REASON, and the unaccounted count
printed and zero.

THE POPULATION IS THE REGION REGISTRY, NOT A LIST. tools/data/state_regions.json is the merge of the
five manifests (module views, shadow sets, save blocks, patch sites, the determinism hash), and
"every bound region" is mechanical rather than editorial: mh::state::bind_stock() answers for all
RID_COUNT of them, so bound_count() == RID_COUNT and the universe is the whole array. A hand-listed
population works once and then rots; a derived one silently GAINS and LOSES members, which is why
`--check` reports the delta against the disposition file and a gained member arrives UNACCOUNTED and
turns the gate red.

THE VERDICTS -- and note how few are editorial:

  carry            reach > 0. THE DEFAULT, and deliberately so. LIB-BOOT's derivation used a
                   reader rule and needed a `carry-added` escape hatch twice (`Stones` was
                   invisible to the write closure because the state matrix has no
                   region entry for that address AT ALL). At step 0 there is no equivalent rule to
                   get wrong: the fixture is a memory image, so the safe direction is to carry, and
                   an EXCLUSION is the thing that must be argued for.
  exclude:no-extent  reach == 0. Not a judgement -- there is no byte to carry. 16 regions today.
  exclude:<class>  a stated class + a `why`, in tools/data/world_snapshot_dispositions.json.

THE ONE EXCLUSION THIS FILE WILL NOT ACCEPT AT ANY PRICE is one that hides a region backing a
determinism-hash slice. That is not a style rule: the item's oracle re-derives the recording peer's
step-0 lockstep hash from the blob, so excluding a hashed region would make the oracle compare
against memory the blob never filled -- a pass over an empty comparison, in the exact shape the
done_when's negative arm exists to forbid. The refusal is at BUILD time so it never reaches the run.

WHAT IS DELIBERATELY *NOT* CARRIED AND MUST BE RE-DERIVED. Some step-0 state is not a region at all
and a bound-region dump physically cannot hold it. Those live in the dispositions file's `re_derive`
section with the evidence, because they are obligations on the IMPORTING host rather than gaps in
the blob, and a fixture that silently omitted them would look complete:

  * the torus masks (general.{width,height,big_*,b*_mask}) and pathfinder_params->width_mask --
    llm_map_setup_dimensions derives them from width/height and libmh owns that body; the
    pathfinder byte is a malloc'd heap block, not a bound region, so it could not be carried.
  * the link-time-baked pointers into bound regions -- tile_objects_ptr, fow_ptr and friends.
    MEASURED at the image bytes (see the section's own notes), never written at runtime, and
    therefore wrong the moment a host binds the pointee anywhere but its stock .bss.

Usage:
    python tools/gen_world_snapshot.py            # schema -> addr/mh_world_snapshot.gen.h
    python tools/gen_world_snapshot.py --refresh  # re-derive the schema from the registry
    python tools/gen_world_snapshot.py --check    # accounting + drift gate (tools/lint_repo.py)
    python tools/gen_world_snapshot.py --report   # the accounting table + the overlap census
    python tools/gen_world_snapshot.py --selftest # prove each refusal fires, and does not over-fire
"""

from __future__ import annotations

import argparse
import collections
import copy
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

REGIONS = os.path.join(REPO, "tools", "data", "state_regions.json")
DISPOSITIONS = os.path.join(REPO, "tools", "data", "world_snapshot_dispositions.json")
SCHEMA = os.path.join(REPO, "tools", "data", "world_snapshot_schema.json")
HEADER = os.path.join(REPO, "src", "mh_dll", "mh", "addr", "mh_world_snapshot.gen.h")
REGISTRY_HEADER = os.path.join(REPO, "src", "mh_dll", "mh", "addr", "mh_regions.gen.h")


def _load(path):
    with open(path, encoding="utf-8-sig") as fh:
        return json.load(fh)


# ---- inputs ------------------------------------------------------------------------------------


def load_regions():
    """[(rid, id, name, base, extent)] in RID order -- the registry's own order, which is the order
    mh_regions.gen.h's enum assigns and therefore the order the emitted table must use."""
    regs = _load(REGIONS)["regions"]
    return [
        {
            "rid": i,
            "id": r["id"],
            "name": r["name"],
            "base": r["base"],
            "extent": int(r.get("extent") or 0),
            "size": int(r.get("size") or 0),
        }
        for i, r in enumerate(regs)
    ]


def hash_backed_ids():
    """The registry ids that back a determinism-hash slice, read out of the GENERATED registry
    header rather than out of hash_manifest.json.

    Deliberately the header: it is what the runtime actually hashes, and it carries BOTH tables
    (strategic HASH_REGIONS and tactical TACT_HASH_REGIONS). Reading the manifest instead would be a
    second derivation of the same fact, which is the thing mh_regions.gen.h's own banner exists to
    abolish."""
    src = open(REGISTRY_HEADER, encoding="utf-8").read()
    out = set()
    for table in ("HASH_REGIONS", "TACT_HASH_REGIONS"):
        m = re.search(r"inline constexpr hash_region %s\[\] = \{(.*?)\n\};" % table, src, re.S)
        if not m:
            sys.exit("gen_world_snapshot: %s not found in mh_regions.gen.h" % table)
        out |= set(re.findall(r"\{\"[^\"]+\",\s*RID_(\w+),", m.group(1)))
    return out


# ---- the derivation ----------------------------------------------------------------------------


def derive(regions, disp):
    """Per region: a verdict, and for an exclusion the class + why. Returns (rows, problems)."""
    classes = disp.get("exclusion_classes", {})
    declared = disp.get("regions", {})
    hashed = hash_backed_ids()
    rows, problems = [], []

    for r in regions:
        rid_id = r["id"]
        d = declared.get(rid_id)
        if d is not None:
            cls = d.get("class")
            why = (d.get("why") or "").strip()
            if not cls:
                problems.append("%s: a disposition with no `class`" % rid_id)
                continue
            if cls not in classes:
                problems.append(
                    "%s: exclusion class %r has no definition in exclusion_classes" % (rid_id, cls)
                )
                continue
            if not why:
                problems.append("%s: exclusion class %r with an empty `why`" % (rid_id, cls))
                continue
            if rid_id in hashed:
                problems.append(
                    "%s: EXCLUDED but it backs a determinism-hash slice. The oracle re-derives the "
                    "step-0 lockstep hash FROM THIS BLOB, so hiding a hashed region makes that "
                    "comparison run over memory the blob never filled -- a pass over an empty "
                    "comparison. Carry it, or the item's negative arm means nothing." % rid_id
                )
                continue
            if r["extent"] == 0:
                problems.append(
                    "%s: excluded with class %r, but it has no measured extent -- it is already "
                    "accounted as exclude:no-extent and a second verdict is drift, not caution."
                    % (rid_id, cls)
                )
                continue
            rows.append({**r, "verdict": "exclude:" + cls, "why": why})
            continue

        if r["extent"] == 0:
            rows.append({**r, "verdict": "exclude:no-extent", "why": ""})
        else:
            rows.append({**r, "verdict": "carry", "why": ""})

    # A disposition naming a region the registry no longer has is stale, and a stale exclusion is
    # how a region silently re-enters the blob under a reason nobody re-read.
    have = {r["id"] for r in regions}
    for rid_id in declared:
        if rid_id not in have:
            problems.append(
                "%s: a disposition for a region the registry does not have (stale -- delete it)"
                % rid_id
            )

    for cls, body in classes.items():
        if not (body.get("why") or "").strip():
            problems.append("exclusion class %r has an empty `why`" % cls)

    return rows, problems


def overlaps(carried):
    """Carried blocks whose [base, base+extent) ranges intersect.

    Not a refusal. Capture is ONE pass over a quiescent image, so two blocks covering the same byte
    carry the same value and import order cannot matter -- but that is an argument, and an argument
    about a file format belongs in the report where it can be re-checked when the count moves."""
    out = []
    ordered = sorted(carried, key=lambda r: r["base"])
    for a, b in zip(ordered, ordered[1:]):
        if a["base"] + a["extent"] > b["base"]:
            out.append(
                {
                    "a": a["id"],
                    "b": b["id"],
                    "bytes": a["base"] + a["extent"] - b["base"],
                }
            )
    return out


def build_schema(regions=None, disp=None):
    regions = regions if regions is not None else load_regions()
    disp = disp if disp is not None else _load(DISPOSITIONS)
    rows, problems = derive(regions, disp)
    carried = [r for r in rows if r["verdict"] == "carry"]
    excluded = [r for r in rows if r["verdict"].startswith("exclude:")]
    unaccounted = len(regions) - len(rows)
    return {
        "_generated_by": "tools/gen_world_snapshot.py --refresh",
        "_do_not_hand_edit": "verdicts are derived; the reasons live in world_snapshot_dispositions.json",
        "universe": len(regions),
        "carried": len(carried),
        "carried_bytes": sum(r["extent"] for r in carried),
        "excluded": len(excluded),
        "unaccounted": unaccounted,
        "hash_backed_regions": len(hash_backed_ids()),
        "overlapping_blocks": overlaps(carried),
        "blocks": [
            {"rid": r["rid"], "id": r["id"], "name": r["name"], "len": r["extent"]} for r in carried
        ],
        "exclusions": [
            {"id": r["id"], "name": r["name"], "verdict": r["verdict"], "why": r["why"]}
            for r in excluded
        ],
    }, problems


# ---- emission ----------------------------------------------------------------------------------

BANNER = '''//
// addr/mh_world_snapshot.gen.h -- GENERATED by tools/gen_world_snapshot.py from
// tools/data/world_snapshot_schema.json. DO NOT EDIT: regenerate.
//
// THE STEP-0 WORLD BLOCK TABLE (LIB-WORLD). A standalone libmh has no cfg parser and no map
// reader, so LIB-REF's replay starts from a blob captured at REPLAY STEP 0 -- the instant
// before the first sim step runs -- alongside the order+clock recording it replays. This
// table is WHAT gets carried: one block per bound region, addressed BY RID so the capture
// reads and the importer writes through live_base(), the stock .bss hosted and the host's
// own allocation standalone.
//
// CARRY IS THE DEFAULT AND AN EXCLUSION IS THE THING THAT MUST BE ARGUED FOR. Every region
// with a measured extent is a block here; anything else carries a stated exclusion in
// tools/data/world_snapshot_dispositions.json, and the unaccounted count is derived on every
// --check and is 0. A region backing a determinism-hash slice CANNOT be excluded at all
// (the generator refuses at build time), because the item's oracle re-derives the recording
// peer's step-0 lockstep hash out of this blob.
//
// WHAT THIS TABLE DOES NOT AND CANNOT CARRY is in the dispositions file's `re_derive`
// section: the torus masks and pathfinder_params->width_mask (derived from width/height by a
// body libmh owns; the pathfinder byte is malloc'd heap, not a bound region), and the
// link-time-baked pointers into bound regions (tile_objects_ptr, fow_ptr) which are wrong the
// moment a host binds the pointee anywhere but its stock .bss. Those are obligations on the
// importing host, not gaps -- LIB-REF's standalone replay is what enforces them.
//
#pragma once
#include <cstdint>

#include "addr/mh_regions.gen.h"
#include "state/blob_snapshot.h"

namespace mh::state {

'''


def emit_header(schema):
    b = schema["blocks"]
    out = [BANNER]
    out.append("inline constexpr int WORLD_SNAPSHOT_BLOCK_COUNT = %d;\n\n" % len(b))
    out.append(
        "// %d bytes of carried world, %d regions excluded (all with a stated why), 0 unaccounted.\n"
        % (schema["carried_bytes"], schema["excluded"])
    )
    out.append(
        "inline constexpr mh::state::blob::snapshot_block\n"
        "    WORLD_SNAPSHOT_BLOCKS[WORLD_SNAPSHOT_BLOCK_COUNT] = {\n"
    )
    for r in b:
        out.append('        {RID_%s, %uu, "%s"},\n' % (r["id"], r["len"], r["name"]))
    out.append("};\n\n} // namespace mh::state\n")
    return "".join(out)


# ---- the commands ------------------------------------------------------------------------------


def cmd_refresh():
    schema, problems = build_schema()
    if problems:
        for p in problems:
            print("  !! " + p)
        sys.exit("gen_world_snapshot --refresh: %d unresolved disposition(s)" % len(problems))
    with open(SCHEMA, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(schema, fh, indent=1, ensure_ascii=False)
        fh.write("\n")
    print(
        "wrote %s (%d carried / %d bytes, %d excluded, %d unaccounted)"
        % (
            os.path.relpath(SCHEMA, REPO),
            schema["carried"],
            schema["carried_bytes"],
            schema["excluded"],
            schema["unaccounted"],
        )
    )


def cmd_emit():
    schema = _load(SCHEMA)
    text = emit_header(schema)
    with open(HEADER, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print(
        "wrote %s (%d blocks, %d bytes)"
        % (os.path.relpath(HEADER, REPO), len(schema["blocks"]), schema["carried_bytes"])
    )


def check(verbose=True):
    """Returns a list of problems. Empty == green."""
    problems = []
    fresh, derive_problems = build_schema()
    problems += derive_problems

    if fresh["unaccounted"] != 0:
        problems.append(
            "UNACCOUNTED: %d registry region(s) reached no verdict. Every bound region is in the "
            "blob or carries a stated exclusion; there is no third state." % fresh["unaccounted"]
        )

    try:
        stored = _load(SCHEMA)
    except OSError:
        problems.append("no %s -- run --refresh" % os.path.relpath(SCHEMA, REPO))
        stored = None

    if stored is not None:
        a = json.dumps(stored.get("blocks"), sort_keys=True)
        b = json.dumps(fresh.get("blocks"), sort_keys=True)
        if a != b:
            sa = {(r["id"], r["len"]) for r in stored.get("blocks", [])}
            sb = {(r["id"], r["len"]) for r in fresh.get("blocks", [])}
            gained = sorted(x[0] for x in sb - sa)
            lost = sorted(x[0] for x in sa - sb)
            problems.append(
                "SCHEMA DRIFT: the committed block table disagrees with a fresh derivation "
                "(+%d %s / -%d %s) -- run --refresh and re-emit."
                % (len(gained), gained[:6], len(lost), lost[:6])
            )
        if stored.get("exclusions") != fresh.get("exclusions"):
            problems.append(
                "EXCLUSION DRIFT: the committed exclusion list disagrees with a fresh derivation "
                "-- run --refresh."
            )

    if os.path.exists(HEADER):
        want = emit_header(stored if stored is not None else fresh)
        have = open(HEADER, encoding="utf-8").read()
        if want != have:
            problems.append(
                "HEADER DRIFT: %s differs from a fresh emit -- run gen_world_snapshot.py."
                % os.path.relpath(HEADER, REPO)
            )
    else:
        problems.append("no %s -- run gen_world_snapshot.py" % os.path.relpath(HEADER, REPO))

    if verbose:
        if problems:
            for p in problems:
                print("  !! " + p)
        else:
            print(
                "ok: world snapshot schema accounted (%d regions = %d carried / %d bytes + %d "
                "excluded across %d class(es), UNACCOUNTED 0; %d hash-backed regions all carried; "
                "%d overlapping block pair(s))"
                % (
                    fresh["universe"],
                    fresh["carried"],
                    fresh["carried_bytes"],
                    fresh["excluded"],
                    len({r["verdict"] for r in fresh["exclusions"]}),
                    fresh["hash_backed_regions"],
                    len(fresh["overlapping_blocks"]),
                )
            )
    return problems


def cmd_report():
    schema, problems = build_schema()
    print(
        "UNIVERSE %d registry regions (the whole registry -- bind_stock answers for every one, so "
        "bound_count() == RID_COUNT)" % schema["universe"]
    )
    print("  carried      %4d blocks / %d bytes" % (schema["carried"], schema["carried_bytes"]))
    by_cls = collections.Counter(r["verdict"] for r in schema["exclusions"])
    print("  excluded     %4d" % schema["excluded"])
    for cls, n in by_cls.most_common():
        print("      %-28s %d" % (cls, n))
    print("  UNACCOUNTED  %4d" % schema["unaccounted"])
    print()
    print("  hash-backed regions: %d (none may be excluded)" % schema["hash_backed_regions"])
    ov = schema["overlapping_blocks"]
    print("  overlapping block pairs: %d" % len(ov))
    for o in ov:
        print("      %-40s / %-40s  %d bytes" % (o["a"], o["b"], o["bytes"]))
    rd = _load(DISPOSITIONS).get("re_derive", {})
    print()
    print("  RE-DERIVE (not carried, and the importing host owes each one): %d" % len(rd))
    for k, v in rd.items():
        print("      %-46s %s" % (k, (v.get("why") or "")[:110]))
    if problems:
        print()
        for p in problems:
            print("  !! " + p)


# ---- the selftest ------------------------------------------------------------------------------
#
# Every refusal, watched to FIRE -- and the last arm watches the whole set NOT to fire on the real
# inputs, because a gate that refuses everything is as useless as one that refuses nothing.


def cmd_selftest():
    regions = load_regions()
    base = _load(DISPOSITIONS)
    ok = True

    def arm(name, mutate, want):
        nonlocal ok
        d = copy.deepcopy(base)
        r = copy.deepcopy(regions)
        r2 = mutate(d, r)
        if r2 is not None:
            r = r2
        _, problems = derive(r, d)
        hit = any(want in p for p in problems)
        print("  [%s] %s" % ("ok" if hit else "FAIL", name))
        if not hit:
            ok = False
            print("        wanted %r, got %s" % (want, problems[:3]))

    # The arms INJECT the class they use rather than borrowing one from the committed file. That
    # file legitimately declares NO exclusion class today (carry is the default and nothing has
    # earned an exclusion yet), and a selftest that could only run once a real exclusion existed
    # would be a gate nobody had ever seen fire.
    TEST_CLASS = "selftest-injected-class"

    def a_class_name(d):
        d.setdefault("exclusion_classes", {}).setdefault(
            TEST_CLASS, {"why": "injected by --selftest; never present in the committed file"}
        )
        return TEST_CLASS

    def a_carried_id(r, d):
        return next(x["id"] for x in r if x["extent"] > 0 and x["id"] not in d.get("regions", {}))

    print("=== gen_world_snapshot --selftest (every refusal, watched to fire) ===")

    arm(
        "an exclusion naming a region the registry does not have is refused",
        lambda d, r: d.setdefault("regions", {}).update(
            {"NO_SUCH_REGION_XYZ": {"class": a_class_name(d), "why": "x"}}
        ),
        "stale",
    )
    arm(
        "an exclusion with no class is refused",
        lambda d, r: d.setdefault("regions", {}).update({a_carried_id(r, d): {"why": "x"}}),
        "no `class`",
    )
    arm(
        "an exclusion naming an undefined class is refused",
        lambda d, r: d.setdefault("regions", {}).update(
            {a_carried_id(r, d): {"class": "no-such-class", "why": "x"}}
        ),
        "has no definition",
    )
    arm(
        "an exclusion with an empty why is refused",
        lambda d, r: d.setdefault("regions", {}).update(
            {a_carried_id(r, d): {"class": a_class_name(d), "why": "  "}}
        ),
        "empty `why`",
    )
    arm(
        "a class whose own why is empty is refused",
        lambda d, r: d["exclusion_classes"].__setitem__(a_class_name(d), {"why": ""}),
        "has an empty `why`",
    )
    arm(
        "EXCLUDING A HASH-BACKED REGION IS REFUSED (the oracle would compare against unfilled "
        "memory)",
        lambda d, r: d.setdefault("regions", {}).update(
            {"TILE_OBJECTS": {"class": a_class_name(d), "why": "a plausible-sounding reason"}}
        ),
        "backs a determinism-hash slice",
    )
    arm(
        "a second verdict on a zero-extent region is refused as drift",
        lambda d, r: d.setdefault("regions", {}).update(
            {
                next(x["id"] for x in r if x["extent"] == 0): {
                    "class": a_class_name(d),
                    "why": "x",
                }
            }
        ),
        "no measured extent",
    )

    # THE OVER-REFUSAL ARM. Each arm above proves a refusal FIRES; this one proves the same rules
    # stay quiet on the committed inputs, which is the half a gate with a wrong predicate fails.
    _, problems = derive(regions, base)
    print("  [%s] the committed inputs produce NO problem" % ("ok" if not problems else "FAIL"))
    if problems:
        ok = False
        for p in problems[:5]:
            print("        " + p)

    # AND THE ACCOUNTING ITSELF IS NON-VACUOUS: a schema that carried nothing would satisfy
    # "unaccounted 0" trivially, so assert the carried set is the registry minus the exclusions.
    schema, _ = build_schema(regions, base)
    want = schema["universe"] - schema["excluded"]
    good = schema["carried"] == want and schema["unaccounted"] == 0 and schema["carried"] > 700
    print(
        "  [%s] carried(%d) + excluded(%d) == universe(%d), unaccounted 0"
        % ("ok" if good else "FAIL", schema["carried"], schema["excluded"], schema["universe"])
    )
    if not good:
        ok = False

    # And the hash-backed set is genuinely inside the carried set rather than merely un-excluded.
    carried_ids = {r["id"] for r in schema["blocks"]}
    missing = sorted(hash_backed_ids() - carried_ids)
    print(
        "  [%s] every hash-backed region is a carried block (%d)"
        % ("ok" if not missing else "FAIL", len(hash_backed_ids()))
    )
    if missing:
        ok = False
        print("        missing: %s" % missing[:8])

    print("=== gen_world_snapshot --selftest: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--refresh", action="store_true", help="re-derive the schema from the registry")
    ap.add_argument("--check", action="store_true", help="accounting + drift gate")
    ap.add_argument("--report", action="store_true", help="the accounting table")
    ap.add_argument("--selftest", action="store_true", help="prove every refusal fires")
    args = ap.parse_args()

    if args.selftest:
        sys.exit(cmd_selftest())
    if args.report:
        cmd_report()
        return
    if args.check:
        sys.exit(1 if check() else 0)
    if args.refresh:
        cmd_refresh()
    cmd_emit()


if __name__ == "__main__":
    main()
