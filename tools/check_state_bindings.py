#!/usr/bin/env python3
"""check_state_bindings.py -- can a translation actually WRITE every region its domain writes?

WHY THIS EXISTS, and it is a defect in a TEST rather than in anyone's work. SIM-RESID-IF closed on
2026-08-31 on a fold of the ledger's `writes_shared` + `writes_island` against the RID set mentioned
anywhere in `sim_state.cpp`, and reported zero unbound. That fold asks

    is this region bound?

and the question a translation needs answered is

    is it bound in the HALF that a WRITER needs?

A region bound once as `ptr<const T>(RID_X)` into `sim_view` satisfies the first and fails the
second. Sixteen regions the sim_resid batch-A/B slice writes were in exactly that state, which is
why eight of its ten translations were complete, linted, and unbuildable: each named a `sim_store`
accessor that does not exist. The bodies were right; the acceptance test could not see the gap.

THE RULE THIS ENCODES. A region is WRITE-BOUND for a module iff that module's state file resolves it
MUTABLY somewhere -- `ptr<T>(RID_X)` with a non-const `T`. It is READ-BOUND iff the file resolves it
as `ptr<const T>(RID_X)`. The two are independent, both may hold (the same RID legitimately appears
in both halves -- see sim_state.h's cur_unit/sim_view::cur_unit pair), and only the first lets a
translated body store to it.

WHY IT SCANS FOR RESOLUTIONS AND NOT FOR CONSTRUCTOR ARGUMENTS. The first version of this
measurement keyed on the literal argument list of `sim_store own(...)` and mis-reported
RID_STRAT_CUR_BUILDING as unbound -- it is bound, through a local. Any `ptr<>` resolution counts,
wherever it appears in the file, because that is what actually produces a writable pointer. The
same reasoning rules out grepping the HEADER for a region ID, which is the mistake that made the
SIM-RESID-IF count read 18 instead of 15: the header binds through RID_* constants and lowercase
accessors, so bound regions read as absent.

WHAT IT CANNOT SEE. A region reached through an accessor that takes its pointer from somewhere other
than `mh::state::ptr` -- which is the ST1 hazard sim_state.h:1591 already names as the thing not to
do. If one is ever added, this check reports it unbound and that is the correct answer.

Usage:
  python tools/check_state_bindings.py --domain sim_resid            # the report
  python tools/check_state_bindings.py --domain sim_resid --check    # exit 1 if a write is unbound
  python tools/check_state_bindings.py --domain sim_resid --batch A B

SECOND LENS (2026-08-31): ADDRESS-TAKEN AND UNBOUND. The write columns attribute a store to the
function whose INSTRUCTION performs it, so a whole-region fill done through utils_fill_data is
credited to utils_fill_data and the caller looks as if it never touched the region. That is how
map_FillDefaults' three unbound fill destinations survived SIM-RESID-IF's closure proof. When
tmp/state_matrix.json is present this now also reports the regions a translate-set function
materializes the address of and never stores to -- a warning, not a verdict, because an address
escaping to an original callee is often legitimate. See load_addr_taken().
"""

import argparse
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(REPO, "tools", "data")
REGIONS_H = os.path.join(REPO, "src", "mh_dll", "mh", "addr", "mh_regions.gen.h")

# Which module state file owns each migration domain's store. A domain not listed here has no
# module state layer of its own and this check does not apply to it.
STATE_FILES = {
    "ai": "src/mh_dll/libmh/ai/ai_state.cpp",
    "sim": "src/mh_dll/libmh/sim/sim_state.cpp",
    "sim_resid": "src/mh_dll/libmh/sim/sim_state.cpp",
    "orders_issue": "src/mh_dll/libmh/sim/sim_state.cpp",
    "tact": "src/mh_dll/libmh/tact/tact_state.cpp",
}

# `    RID_STRAT_ADVISOR_PHASE = 298,` -- the enum, which is the COMPLETE set. The REGIONS[] table
# further down the same header is NOT: it carries only the regions with a measured base/extent, so
# keying on it reported 133 of sim_resid's 141 written regions as unregistered.
RID_ENUM = re.compile(r"^\s*(RID_[A-Z0-9_]+)\s*=\s*\d+\s*,", re.M)
# `ptr<const unit>(RID_UNITS)` / `ptr<unit>(RID_UNITS)` -- the const-ness of T is the whole verdict.
PTR_RESOLVE = re.compile(r"\bptr\s*<\s*(const\s+)?([^>]+?)\s*>\s*\(\s*(RID_[A-Z0-9_]+)\s*\)")


def load_region_ids():
    """{region name -> RID}.

    The mapping is the registry's own `id` field with the `RID_` prefix -- that is exactly what
    gen_state_registry.py emits into the enum -- and the enum is then read back purely to VALIDATE
    it, so a rename that desynchronises the two is reported rather than silently producing a
    region nobody checks.
    """
    if not os.path.isfile(REGIONS_H):
        sys.exit("missing %s -- run gen_state_registry.py --refresh" % REGIONS_H)
    known = set(RID_ENUM.findall(open(REGIONS_H, encoding="utf-8").read()))
    regs = json.load(open(os.path.join(DATA, "state_regions.json"), encoding="utf-8"))["regions"]
    out, orphan = {}, []
    for r in regs:
        rid = "RID_" + (r.get("id") or "")
        if rid in known:
            out[r["name"]] = rid
        else:
            orphan.append(r["name"])
    if orphan:
        print(
            "WARNING: %d registry region(s) have no RID in mh_regions.gen.h -- regenerate "
            "(gen_state_registry.py --refresh): %s" % (len(orphan), ", ".join(sorted(orphan)[:6]))
        )
    return out


def load_addr_taken(names):
    """{region -> {fn}} for regions a translate-set function MATERIALIZES THE ADDRESS OF and never
    itself stores to. Optional enrichment: absent matrix = empty, same contract gen_state_registry
    uses for its own --refresh.

    THIS IS THE LENS THE WRITE COLUMNS CANNOT HAVE, and it cost a whole batch to learn. The ledger
    attributes a write by the address of the INSTRUCTION that performs it, so a bulk fill done by a
    callee -- `MOV EAX,0xcc46e0; MOV EBX,0x3a980; CALL utils_fill_data` -- is credited to
    utils_fill_data, and the caller's cell shows `addr_of: 1, write: 0`. map_FillDefaults zeroes 25
    whole regions that way. SIM-RESID-IF closed on 2026-08-31 having proved every region the
    domain's translate set WRITES was bound, and three of map_FillDefaults' fill destinations were
    not bound, not typed, and not even named -- because the proof's input could not see them.

    The report is a WARNING, not a verdict, and deliberately so: an address materialized and passed
    to an original callee is often a legitimate address-escape (a sprintf format string, a widget
    list) that no binding is owed for. What it buys is that the need is VISIBLE at slice time
    instead of surfacing as a deferral halfway through a translation.
    """
    path = os.path.join(REPO, "tmp", "state_matrix.json")
    if not os.path.isfile(path):
        return None
    m = json.load(open(path, encoding="utf-8"))
    out = {}
    for cell in m["cells"]:
        if cell["fn"] not in names:
            continue
        c = cell["counts"]
        if c.get("write", 0) or c.get("rw", 0):
            continue  # already answered by the write columns
        if not c.get("addr_of", 0):
            continue
        out.setdefault(cell["region"], set()).add(cell["fn"])
    return out


def load_bindings(state_file):
    """(write_bound RIDs, read_bound RIDs) for one module state file."""
    path = os.path.join(REPO, state_file)
    if not os.path.isfile(path):
        sys.exit("missing module state file %s" % path)
    src = open(path, encoding="utf-8").read()
    # Comments carry example code (`v.X = ptr<const T>(RID_Y)` appears in several explanations), so
    # a raw scan would credit a binding the compiler never sees. Strip them first.
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    writes, reads = set(), set()
    for is_const, _ty, rid in PTR_RESOLVE.findall(src):
        (reads if is_const else writes).add(rid)
    return writes, reads


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--domain", default="sim_resid")
    ap.add_argument("--batch", nargs="+", help="limit to these ledger batches")
    ap.add_argument("--check", action="store_true", help="exit 1 if any written region is unbound")
    ap.add_argument("--json", help="write the verdict here")
    args = ap.parse_args()

    state_file = STATE_FILES.get(args.domain)
    if not state_file:
        sys.exit("domain %r has no module state file in STATE_FILES" % args.domain)
    ledger_path = os.path.join(DATA, "%s_migration.json" % args.domain)
    if not os.path.isfile(ledger_path):
        sys.exit("no ledger %s" % ledger_path)

    ids = load_region_ids()
    write_bound, read_bound = load_bindings(state_file)
    led = json.load(open(ledger_path, encoding="utf-8"))

    # WHO IS ASKED. Every function the domain will TRANSLATE -- not the excepted walls, whose writes
    # must stay unbound (binding an excepted wall's input/camera/chat state into the sim store is
    # the W1 regression SIM-RESID-IF's record calls out by name).
    rows = [f for f in led["functions"] if "todo:dead" not in (f.get("tags") or [])]
    if args.batch:
        rows = [f for f in rows if f.get("batch") in set(args.batch)]

    by_region = {}
    for f in rows:
        for g in list(f.get("writes_shared") or []) + list(f.get("writes_island") or []):
            by_region.setdefault(g, []).append(f["name"])

    unregistered, read_only, unbound, ok = [], [], [], []
    for region in sorted(by_region):
        rid = ids.get(region)
        if rid is None:
            unregistered.append(region)
        elif rid in write_bound:
            ok.append(region)
        elif rid in read_bound:
            read_only.append((region, rid))
        else:
            unbound.append((region, rid))

    print(
        "%s: %d region(s) written by %d translate-set function(s) -- %d write-bound, "
        "%d READ-ONLY, %d unbound, %d unregistered"
        % (
            args.domain,
            len(by_region),
            len(rows),
            len(ok),
            len(read_only),
            len(unbound),
            len(unregistered),
        )
    )

    if read_only:
        print(
            "\nBOUND IN THE READ HALF ONLY (%d) -- `ptr<const ...>` into the view, no store "
            "accessor. A translation that writes one of these names an accessor that does not "
            "exist:" % len(read_only)
        )
        for region, rid in read_only:
            print(
                "   %-46s %-44s %s" % (region, rid, ", ".join(sorted(set(by_region[region])))[:90])
            )
    if unbound:
        print("\nBOUND IN NEITHER HALF (%d):" % len(unbound))
        for region, rid in unbound:
            print(
                "   %-46s %-44s %s" % (region, rid, ", ".join(sorted(set(by_region[region])))[:90])
            )
    if unregistered:
        print(
            "\nNOT IN THE REGION REGISTRY AT ALL (%d) -- R8's question, not this one:"
            % len(unregistered)
        )
        for region in unregistered:
            print("   %-46s %s" % (region, ", ".join(sorted(set(by_region[region])))[:90]))

    # ---- the second lens: address-taken and unbound (see load_addr_taken) ----------------------
    addr_taken = load_addr_taken({f["name"] for f in rows})
    blind = []
    if addr_taken is None:
        print(
            "\n[address-taken lens] SKIPPED -- no tmp/state_matrix.json. Regenerate it "
            "(run-script mh_gen_state_matrix.py) to see the regions this domain reaches through a "
            "callee, which the write columns cannot report."
        )
    else:
        for region in sorted(addr_taken):
            rid = ids.get(region)
            if rid is not None and (rid in write_bound or rid in read_bound):
                continue
            blind.append((region, rid, sorted(addr_taken[region])))
        if blind:
            print(
                "\nADDRESS-TAKEN AND BOUND IN NEITHER HALF (%d) -- a region one of these functions "
                "materializes the address of and does not itself store to, so the write columns "
                "above are silent about it. Some are legitimate address-escapes to an original "
                "callee (a format string, a widget list); a bulk fill through utils_fill_data is "
                "NOT, and is a translation blocker:" % len(blind)
            )
            for region, rid, fns in blind:
                print("   %-46s %-30s %s" % (region, rid or "(unregistered)", ", ".join(fns)[:70]))
        else:
            print("\n[address-taken lens] clean -- nothing reached by address only is unbound")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(
                {
                    "_generated_by": "tools/check_state_bindings.py",
                    "domain": args.domain,
                    "state_file": state_file,
                    "write_bound": sorted(ok),
                    "read_only": [r for r, _ in read_only],
                    "unbound": [r for r, _ in unbound],
                    "unregistered": unregistered,
                    "addr_taken_unbound": [
                        {"region": r, "rid": rid, "fns": fns} for r, rid, fns in blind
                    ],
                },
                fh,
                indent=1,
            )
        print("wrote %s" % args.json)

    if args.check and (read_only or unbound or unregistered):
        return 1
    if not (read_only or unbound or unregistered):
        print("\nevery region this domain writes is bound in the WRITE half")
    return 0


if __name__ == "__main__":
    sys.exit(main())
