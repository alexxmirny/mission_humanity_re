#!/usr/bin/env python3
"""gen_region_accessors.py -- how many ORIGINAL accessors does each state region still have?

WHY THIS EXISTS (SIM-BOUNDARY / RI-STATE). Law 1 (the reimplementation plan) says the layout of a shared
state region is the ABI between old and new code, and is frozen for as long as even one ORIGINAL
accessor remains. Every consumer of that law -- ST3's `owned_by_dll` mark, SIM1-P's release clause,
the whole question of whether a region may leave the binary -- needs one number per region: how many
original functions still reach it, split read vs write. Nothing measured that. SIM1-P's own
`done_when` carries the clause ("no original accessor of a migrated roster remains for any region
declared moved") and it passes VACUOUSLY today, because no region is declared moved, so the clause
quantifies over the empty set.

This file is the measurement half. The INTERLOCK half is rules 4-6 in gen_state_registry.py's
`check_ownership()` -- deliberately over there, next to rules 1-3, so `owned_by_dll` is enforced in
exactly one place. Same split as the rest of the tree: a generator measures, a checker refuses.

THE CANDIDATE SET, stated next to every verdict because a check whose candidate set is narrower than
its claim is how `check_dispatch_closure` reported OK for days while 30 functions were missing
(a candidate set narrower than the claim). Here it is: EVERY region in the registry (tools/data/state_regions.json), not
just the rosters and not just the sim-written ones -- and every function in the binary that is not
ours, not just the ones in a migration manifest.

WHERE THE EDGES COME FROM, and this one decides whether the answer is right at all: the STATE MATRIX
(tmp/state_matrix.json), which is an instruction-level scan that folds indirect edges in via
the fn-pointer reference materializer. NOT tools/data/call_graph_no_crt.json, which is a DIRECT-call graph and is
exactly the measurement that missed the 68 fn-ptr-dispatched state handlers (2026-08-19) and then the
30 building-type callbacks (2026-08-22). Any future contributor tempted to swap the source: don't.

"OURS" is tools/_reimpl.py's `done` set -- a reimplementation that exists AND has been proven
equivalent. `planned` deliberately does NOT count: a ledger row is a schedule, not an owned function,
and counting it here would report regions as releasable because someone intends to do the work.

ATTRIBUTION IS BY ADDRESS CONTAINMENT, not by name. The matrix names interior windows as their own
regions (`p0_local` and friends live inside the one `player_data` region), so a name join would
silently drop those accessors and report a region cleaner than it is -- the dangerous direction. Each
matrix region is resolved to the registry region that HOSTS its address, the same way
gen_state_registry.claim_windows() resolves manifest claims.

MEASURED vs ZERO, and the distinction is the point. A registry region that no matrix region resolves
into is `measured: false` -- UNMEASURED, not clean. 25 of the 553 registry regions are in that state
today (they enter the registry from the save/patch manifests and the instruction scan never saw
them). Rule 5 in the interlock refuses to let an unmeasured region be declared moved, because
"nothing measured it" and "nothing accesses it" are not the same sentence.

Usage:
  python tools/gen_region_accessors.py --refresh   # matrix + registry -> tools/data/region_accessors.json
  python tools/gen_region_accessors.py             # the census report (top N), --all for everything
  python tools/gen_region_accessors.py --check     # the data file is present, complete and current-shaped
  python tools/gen_region_accessors.py --region ID # one region, with its accessors named
"""

import argparse
import bisect
import collections
import contextlib
import hashlib
import io
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import _subsys
from _reimpl import load_dead, load_reimpl

MATRIX = REPO / "tmp" / "state_matrix.json"
REGISTRY = REPO / "tools" / "data" / "state_regions.json"
FUNCTIONS = REPO / "tools" / "data" / "en_functions.json"
OWNERSHIP = REPO / "tools" / "data" / "region_ownership.json"
OUT = REPO / "tools" / "data" / "region_accessors.json"

# SCHEMA 5 (2026-09-06, SB-HOSTFREE): `external_addr_takers`. Until now a cell with only
# `addr_of`/`imm`/`ref` counts was DROPPED -- "the address is taken, the region is not touched
# here" -- which is right for the question "who reads or writes this" and catastrophically wrong
# for "may a host move this". An original body that PASSES the address (push offset X; call
# CreateMutexA) reaches those bytes just as surely as one that dereferences them, and no bind can
# follow it. That gap made _G_LLM_SINGLE_INSTANCE_MUTEX_NAME read as accessor-free; relocating it
# renamed the lane mutex to garbage and every game exited on the single-instance guard, at 0 steps,
# with no crash to look at.
SCHEMA = 5


def _load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def registry_regions():
    """The registry's regions as a list, whatever shape the data file happens to use."""
    regions = _load(REGISTRY)["regions"]
    return list(regions.values()) if isinstance(regions, dict) else list(regions)


def function_table():
    """name -> (entry_va, size). The extent table, not docs/symbols.md.

    symbols.md is scoped BY NAMING CONVENTION to `llm_`/`_G_LLM_` names, so every hand-named
    `game_*`/`map_*` function and every still-`FUN_` body is absent from it -- the trap that dropped
    38 load-bearing sim functions out of the first closure measurement.
    """
    out = {}
    for f in _load(FUNCTIONS)["functions"]:
        out[f["name"]] = (int(f["entry"], 16), f.get("size", 0))
    return out


def host_resolver(regions):
    """addr -> the registry region that HOSTS it, or None. Same containment rule as claim_windows."""
    by_base = {r["base"]: r for r in regions}
    keys = sorted(by_base)
    reach = [max(by_base[k].get("extent", 0), by_base[k]["size"] or 0) or 1 for k in keys]

    def resolve(addr):
        i = bisect.bisect_right(keys, addr) - 1
        if i < 0 or addr >= keys[i] + reach[i]:
            return None
        return by_base[keys[i]]

    return resolve


def region_key(r):
    """The key region_ownership.json uses. `id` when the registry has one, else the raw base."""
    return r.get("id") or ("0x%08x" % r["base"])


def fingerprints(ours=None):
    """A content fingerprint per COMMITTED input the census is derived from (SB2's currency guard).

    THE GAP THIS CLOSES, and it bit one day after the census shipped: `--check` verified the region
    SET was complete and said nothing about whether the accessor DATA was still current. Two
    functions were counted as external readers of sim state because the census had been built
    against a stale `en_functions.json`, where they were still `FUN_`; both were already ours.

    The state matrix is deliberately absent from this list. It is 7 MB, not committed, and needs
    Ghidra to regenerate -- fingerprinting it would make `--check` unrunnable on a fresh clone, which
    is the opposite of the offline contract every other generator here keeps. Its identity is
    recorded in `_provenance` (program, cell and region counts) and checked by eye, and that limit is
    stated rather than hidden.
    """
    out = {}
    for key, path in (("en_functions", FUNCTIONS), ("state_regions", REGISTRY)):
        out[key] = hashlib.sha256(path.read_bytes()).hexdigest()[:16]
    # Derived, not a file: `done` comes from the ledgers plus a source scan, so hash the SET rather
    # than any one input. This is the direction that moves most often -- every verified row changes
    # it -- which is exactly why its drift is classified separately below.
    if ours is None:
        ours = {a for a, v in load_reimpl(str(REPO)).items() if v == "done"}
    blob = ",".join("%08x" % a for a in sorted(ours))
    out["reimpl_done"] = hashlib.sha256(blob.encode()).hexdigest()[:16]
    return out


def dirty_inputs():
    """Which of the fingerprinted inputs git reports as MODIFIED in the working tree.

    THE FAILURE THIS EXISTS TO MAKE UNCONSTRUCTIBLE, measured 2026-09-01. `--refresh` records a
    fingerprint of `en_functions.json` AS IT SITS ON DISK. If that copy has been regenerated and not
    committed, the census and the file agree in THIS tree and `--check` passes -- and then the commit
    carries the OLD en_functions.json, so the committed pair is inconsistent and every later checkout
    is red. That is not hypothetical: the census committed on 2026-08-30 records a fingerprint that
    NO commit of en_functions.json has ever had (all 22 checked), the gate had been red for ~40
    commits, and two separate commit messages in that window say "lint_repo: PASS" -- truthfully,
    because each ran against its own uncommitted tree.

    A prose rule ("commit the dump first") had already failed twice here, so this is a gate. Returns
    [] when git cannot answer -- unknown is not the same as dirty, and a generator that refuses to
    run outside a git checkout would be worse than the bug.
    """
    rels = ["tools/data/en_functions.json", "tools/data/state_regions.json"]
    try:
        r = subprocess.run(
            ["git", "status", "--porcelain", "--"] + rels,
            cwd=str(REPO),
            capture_output=True,
            text=True,
            timeout=30,
        )
    except (OSError, subprocess.SubprocessError):
        return []
    if r.returncode != 0:
        return []
    return [ln[3:].strip() for ln in r.stdout.splitlines() if ln.strip()]


def currency(doc):
    """(unsafe, conservative) lists of input names whose fingerprint has moved since --refresh.

    THE TWO DIRECTIONS ARE NOT THE SAME RISK, and collapsing them would make this check either
    useless or unusable:

      conservative -- `reimpl_done` grew. A function we have since proven is still listed as an
                      original accessor, so the census OVER-reports and the interlock refuses moves
                      it should allow. Safe, and it happens on every verified row, so failing the
                      gate on it would fail the gate constantly for a drift that cannot grant a
                      wrong permission.
      unsafe       -- `en_functions` or `state_regions` moved. A rename, a newly defined function or
                      a new region can make the census UNDER-report: an accessor nobody counted. That
                      can grant an owned_by_dll claim that should have been refused, so it is fatal.

    A census with no fingerprint block at all (schema < 3) is unsafe by definition -- unknown is not
    the same as unchanged.
    """
    old = (doc.get("_provenance") or {}).get("inputs")
    if not old:
        return ["(no fingerprint recorded -- census predates the currency guard)"], []
    now = fingerprints()
    unsafe = [k for k in ("en_functions", "state_regions") if old.get(k) != now.get(k)]
    conservative = ["reimpl_done"] if old.get("reimpl_done") != now.get("reimpl_done") else []
    return unsafe, conservative


def refresh(allow_dirty=False):
    dirty = dirty_inputs()
    if dirty and not allow_dirty:
        print("REFUSING TO REFRESH -- fingerprinted input(s) MODIFIED in the working tree:")
        for f in dirty:
            print("  " + f)
        print(
            "The fingerprint this records is of the file ON DISK, so refreshing now produces a "
            "census that agrees with THIS tree and disagrees with whatever gets committed -- a "
            "gate that is green for you and red for everyone after you. COMMIT THE INPUT FIRST, "
            "then refresh. --allow-dirty-inputs overrides and stamps it into _provenance."
        )
        return 1
    if not MATRIX.exists():
        print(
            "MISSING %s -- regenerate it in Ghidra first:\n"
            "  run-script {programPath: '/eng/mh.exe', scriptName: 'mh_gen_state_matrix.py'}"
            % MATRIX
        )
        return 1
    matrix = _load(MATRIX)
    regions = registry_regions()
    fns = function_table()
    ours = {a for a, v in load_reimpl(str(REPO)).items() if v == "done"}
    dead_addrs = load_dead(str(REPO))
    resolve = host_resolver(regions)

    # matrix region name -> hosting registry region key. Resolved once; a matrix region that lands
    # outside every registry extent is counted and dropped (reported in the provenance block).
    host = {}
    unhosted = 0
    for name, meta in matrix["regions"].items():
        r = resolve(int(meta["addr"], 16))
        if r is None:
            unhosted += 1
            continue
        host[name] = region_key(r)

    acc = collections.defaultdict(
        lambda: {
            "ext_w": set(),
            "ext_r": set(),
            "our_w": set(),
            "our_r": set(),
            "ext_a": set(),
            "rw": False,
            "subsys": {},
        }
    )
    for cell in matrix["cells"]:
        key = host.get(cell["region"])
        if key is None:
            continue
        counts = cell["counts"]
        writes = bool(counts.get("write") or counts.get("rw"))
        reads = bool(counts.get("read") or counts.get("rw"))
        fn = cell["fn"]
        entry = fns.get(fn, (None, 0))[0]
        if not (writes or reads):
            # ADDRESS TAKEN, NOT DEREFERENCED -- `push offset X` / an immediate. Recorded rather
            # than dropped since schema 5: it answers no question about who reads the bytes, and it
            # settles the only question SB-HOSTFREE asks, which is whether a HOST may move them.
            # A pointer baked into an original instruction cannot follow a bind.
            if entry is not None and entry not in ours:
                acc[key]["ext_a"].add(fn)
                acc[key]["subsys"][fn] = cell.get("fn_subsystem") or "?"
            continue
        slot = acc[key]
        slot["rw"] = True  # a real read/write cell -- what `measured` has always meant
        if entry is not None and entry in ours:
            slot["our_w" if writes else "our_r"].add(fn)
        else:
            slot["ext_w" if writes else "ext_r"].add(fn)
            slot["subsys"][fn] = cell.get("fn_subsystem") or "?"

    out = {}
    for r in regions:
        key = region_key(r)
        slot = acc.get(key)
        # A function that BOTH writes and reads is a writer, never counted twice.
        ext_w = sorted(slot["ext_w"]) if slot else []
        ext_r = sorted(slot["ext_r"] - slot["ext_w"]) if slot else []
        subsys = collections.Counter(slot["subsys"][f] for f in ext_w + ext_r) if slot else {}
        out[key] = {
            "name": r["name"],
            "base": r["base"],
            "size": r["size"],
            # UNCHANGED MEANING ACROSS SCHEMA 5: `measured` is "an instruction READS OR WRITES
            # these bytes", not "the matrix mentions the address". An addr_of-only cell now
            # creates a slot (it has to -- that is the point of external_addr_takers), and letting
            # it flip `measured` would quietly satisfy the ownership interlock's "an UNMEASURED
            # region may not be declared moved" rule for exactly the regions least safe to move.
            "measured": bool(slot and slot["rw"]),
            # NAMES, not counts, since schema 2: `written_by` seed queries (MIG-SET) have to ask
            # "which regions does the SIM manifest write", and a count cannot answer that.
            "ours_writers": sorted(slot["our_w"]) if slot else [],
            "ours_readers": sorted(slot["our_r"] - slot["our_w"]) if slot else [],
            "external_writers": ext_w,
            # SCHEMA 4: the subset of external_writers whose ledger row is terminal `dead`, i.e.
            # NOTHING REACHES IT (evidenced, not asserted -- see _reimpl.load_dead). Stored as a
            # subset rather than removed from external_writers because the readers domain seeds off
            # the complete list for legibility. Read it through live_external_writers().
            "external_writers_dead": [f for f in ext_w if fns.get(f, (None, 0))[0] in dead_addrs],
            "external_readers": ext_r,
            # SCHEMA 5: original bodies that TAKE the address without dereferencing it. Empty for
            # almost every region and decisive for the few it is not -- see the SCHEMA note above.
            "external_addr_takers": (
                sorted(slot["ext_a"] - slot["ext_w"] - slot["ext_r"]) if slot else []
            ),
            "external_bytes": sum(fns.get(f, (None, 0))[1] for f in ext_w + ext_r),
            "subsystems": dict(sorted(subsys.items(), key=lambda kv: -kv[1])),
        }

    measured = sum(1 for v in out.values() if v["measured"])
    doc = {
        "_generated_by": "tools/gen_region_accessors.py --refresh",
        "_do_not_hand_edit": "Regenerate after any migration promotion or state-matrix refresh.",
        "_schema": SCHEMA,
        "_candidate_set": (
            "Every region in tools/data/state_regions.json, against every function in the binary "
            "that is not in _reimpl's `done` set. Edges from tmp/state_matrix.json (instruction "
            "scan, folds fn-ptr edges), NOT from the direct-call graph."
        ),
        "_provenance": {
            "program": matrix.get("program"),
            "matrix_cells": len(matrix["cells"]),
            "matrix_regions": len(matrix["regions"]),
            "matrix_regions_unhosted": unhosted,
            "reimpl_done": len(ours),
            "reimpl_dead": len(dead_addrs),
            "registry_regions": len(regions),
            "measured": measured,
            "unmeasured": len(regions) - measured,
            # SB2's currency guard. Compared on every --check; see currency().
            "inputs": fingerprints(ours),
            # Empty on every honest refresh. Non-empty means --allow-dirty-inputs was used and the
            # fingerprints above are of files that were NOT what got committed -- so a later --check
            # comparing against the committed copies can disagree for a reason that is recorded here
            # rather than mysterious. See dirty_inputs().
            "inputs_dirty_at_refresh": dirty,
        },
        "regions": out,
    }
    OUT.write_text(json.dumps(doc, indent=1) + "\n", encoding="utf-8")

    free = [
        k
        for k, v in out.items()
        if v["measured"] and not v["external_writers"] and not v["external_readers"]
    ]
    print(
        "wrote %s\n  %d registry regions (%d measured, %d UNMEASURED)\n"
        "  %d with no original accessor at all\n"
        "  %d matrix regions resolved into none of them (outside the registry's extents)"
        % (OUT.relative_to(REPO), len(out), measured, len(regions) - measured, len(free), unhosted)
    )
    return 0


def load_ownership():
    """The hand-declared ownership file, keyed by the same region id (SB2). Absent = nothing declared."""
    if not OWNERSHIP.exists():
        return {}
    return json.loads(OWNERSHIP.read_text(encoding="utf-8-sig"))["regions"]


def cross_writes(doc, ownership=None):
    """Per region: the original writers whose SUBSYSTEM differs from the region's declared owner.

    THE COLUMN SB2 EXISTS FOR. One owner per region makes "own all the writers" finite; this is the
    other half -- the list of writers that disagree with that declaration, which is either a
    re-attribution (the owner is wrong) or a fact about the code that has to be written down
    (`cross_write_accepted`). Anything else is an unadjudicated cross-write, and the count of those
    is the number this item is closed against.

    DERIVED AT READ TIME, NOT STORED, for two reasons that both matter:
      - the owner is a hand declaration and the census is a Ghidra-derived artifact. Baking the join
        into `--refresh` would mean re-running Ghidra to change a `why` string.
      - `ours` is recomputed here from the COMMITTED ledger + en_functions, so a conservatively stale
        census (the reimplemented set grew since the last refresh) cannot invent a cross-writer out
        of a function that is now ours. Only the unsafe direction can mislead this, and that one is
        already fatal in check().

    WRITERS ONLY. Readers are cross-cutting by nature -- 471 external accessors across the registry,
    most of them reads -- and a rule that demanded a reason for every cross-subsystem READ would be a
    rule nobody could satisfy or would satisfy honestly. The boundary decision (SB3 D3) is stated in
    terms of writers for the same reason.
    """
    if ownership is None:
        ownership = load_ownership()
    fns = function_table()
    ours = {a for a, v in load_reimpl(str(REPO)).items() if v == "done"}
    out = {}
    for rid, v in doc["regions"].items():
        writers = [
            f for f in (v.get("external_writers") or []) if fns.get(f, (None, 0))[0] not in ours
        ]
        if not writers:
            continue
        decl = ownership.get(rid) or {}
        owner = decl.get("owner_subsystem")
        cross = [(f, _subsys.classify_fn(f)) for f in writers]
        cross = [(f, sub) for f, sub in cross if sub != owner]
        if not cross:
            continue
        out[rid] = {
            "owner": owner,
            "cross": cross,
            "accepted": (decl.get("cross_write_accepted") or "").strip() or None,
        }
    return out


def unadjudicated(xw):
    """The re-attribution worklist: cross-written regions with no accepted-reason recorded."""
    return {rid: v for rid, v in xw.items() if not v["accepted"]}


# ---------------------------------------------------------------------------------------------
# PER-WRITER adjudication (TACT-WRITERS, 2026-08-24)
#
# WHY A SECOND LAYER, when SB2's `cross_write_accepted` already made the region-level count zero.
# A region-level string adjudicates the REGION: "tactical and strategic share this plane by design."
# That sentence is true of `tile_objects` as a whole and says nothing about whether
# `llm_tact_unit_teleport` in particular is accounted for. SB-SOLE has to cite a population of
# WRITERS, not of regions, so an adjudication whose unit is the region cannot discharge it -- and,
# worse, a region-level reason silently absorbs a NEW writer that appears later, which is the exact
# shape of the drift RD-READY caught in the readers manifest (a rename shrinking a scope without
# anyone noticing). A per-writer entry has to be added by hand for each new writer, so growth is
# visible.
#
# THE STALENESS DIRECTION THAT MATTERS. A per-writer entry naming a function that is no longer a
# cross-writer (renamed, reimplemented, re-attributed) is refused by check() rather than ignored.
# Ignoring it is the unsafe direction: the entry would keep asserting an adjudication over a name
# nothing measures any more, and the count would still print zero.
#
# HOST CONFIGURATION IS STRUCTURED, NOT PROSE. Under D4 (docs/state-boundary.md) an original write
# to a region libmh owns is HARMLESS IN-PROCESS -- the host answers the bind table with the stock
# .bss base, so the original writes the same bytes we read -- and WRONG under a relocated bind
# (SB-HOSTFREE) or a standalone host (LIB-REF), where the stock address is abandoned memory. An
# exception that does not say which configuration it holds in is therefore not an exception, it is
# an omission. `cross_write_host_config` carries that as fields the checker can refuse, not as a
# sentence a reader has to trust.

HOST_CONFIGS = ("in-process", "standalone", "any")


def host_config_problems(rid, decl):
    """Structural complaints about a region's `cross_write_host_config`. Empty list = well-formed."""
    hc = decl.get("cross_write_host_config")
    if not isinstance(hc, dict):
        return ["%s: has per-writer adjudication but no `cross_write_host_config` block" % rid]
    bad = []
    if hc.get("holds_in") not in HOST_CONFIGS:
        bad.append(
            "%s: cross_write_host_config.holds_in=%r is not one of %s"
            % (rid, hc.get("holds_in"), ", ".join(HOST_CONFIGS))
        )
    fails = hc.get("fails_in")
    if not isinstance(fails, list):
        bad.append("%s: cross_write_host_config.fails_in must be a list (may be empty)" % rid)
    elif hc.get("holds_in") != "any" and not fails:
        bad.append(
            "%s: holds_in=%r but fails_in is empty -- an exception that holds everywhere is "
            "`any`, and one that does not must name where it breaks" % (rid, hc.get("holds_in"))
        )
    if not (hc.get("check") or "").strip():
        bad.append(
            "%s: cross_write_host_config.check is empty -- name the check that catches this if "
            "the configuration stops holding" % rid
        )
    return bad


def ratchet_holes(doc, ownership=None):
    """(region, writer) pairs on a region that DEMANDS per-writer adjudication but lacks it.

    THE RATCHET, and without it this whole layer is a snapshot rather than a guarantee. A region
    that opts in with `cross_write_per_writer_required` has said: every original cross-writer of
    mine is named individually. A writer that appears later -- a new tactical function, a rename
    that lands in this subsystem, a re-attribution -- would otherwise be absorbed silently by the
    region-level string and the ledger would keep printing zero. Here it is a hole, and check()
    refuses it.
    """
    if ownership is None:
        ownership = load_ownership()
    want = {rid for rid, d in ownership.items() if d.get("cross_write_per_writer_required")}
    return [
        (r["region"], r["writer"])
        for r in writer_rows(doc, ownership)
        if r["region"] in want and r["state"] != "writer"
    ]


def live_external_writers(region):
    """The external writers of `region` that can actually RUN.

    `external_writers` is the complete original-writer list and stays that way: the readers domain
    seeds off it for LEGIBILITY (a dead function still has ref sites SB4 must cover, and still has
    to be named and prototyped), so narrowing the stored field would silently shrink that scope.

    But for every question of the form "does anything still write this region" -- SB-SOLE's
    sole-writer boundary, gen_state_registry's owned_by_dll refusal, the frontier counts the SB-*
    items are scoped against -- a `dead` row is not a writer. `dead` here is an evidenced claim that
    NOTHING REACHES the function (three or four concurring tools; see _reimpl.load_dead), so its
    writes never happen. This helper is the one place that distinction lives, so a consumer cannot
    get it wrong by forgetting.
    """
    dead = set(region.get("external_writers_dead") or ())
    return [f for f in (region.get("external_writers") or ()) if f not in dead]


def writer_rows(doc, ownership=None, subsystem=None):
    """One row per (region, original cross-writer), with how it is adjudicated.

    state is one of:
      writer  -- a `cross_write_writers` entry names this function
      region  -- covered only by the region-level `cross_write_accepted` string
      none    -- unadjudicated at either level
    """
    xw = cross_writes(doc, ownership)
    if ownership is None:
        ownership = load_ownership()
    rows = []
    for rid, v in sorted(xw.items()):
        decl = ownership.get(rid) or {}
        per = decl.get("cross_write_writers") or {}
        for fn, sub in sorted(v["cross"]):
            if subsystem and sub != subsystem:
                continue
            reason = (per.get(fn) or "").strip()
            rows.append(
                {
                    "region": rid,
                    "owner": v["owner"],
                    "writer": fn,
                    "subsystem": sub,
                    "state": "writer" if reason else ("region" if v["accepted"] else "none"),
                    "reason": reason or v["accepted"] or None,
                    "host_config": decl.get("cross_write_host_config"),
                }
            )
    return rows


def stale_writer_entries(doc, ownership=None):
    """`cross_write_writers` names that are not (any longer) cross-writers of their region."""
    xw = cross_writes(doc, ownership)
    if ownership is None:
        ownership = load_ownership()
    out = []
    for rid, decl in sorted(ownership.items()):
        per = decl.get("cross_write_writers") or {}
        if not per:
            continue
        live = {f for f, _ in (xw.get(rid) or {}).get("cross", [])}
        for fn in sorted(per):
            if fn not in live:
                out.append((rid, fn))
    return out


def report_writers(doc, subsystem, show_all):
    """The per-writer ledger. `--writers "Tactical mode"` is what TACT-WRITERS is closed against."""
    rows = writer_rows(doc, subsystem=subsystem)
    scope = subsystem or "every subsystem"
    if not rows:
        print("no original cross-writers for %s." % scope)
        return 0
    by = collections.Counter(r["state"] for r in rows)
    regions = sorted({r["region"] for r in rows})
    print(
        "CROSS-WRITER LEDGER -- %s: %d (region, writer) pair(s), %d distinct function(s), over %d "
        "region(s).\n"
        "  %d adjudicated PER WRITER, %d covered only by the region-level reason, %d UNADJUDICATED.\n"
        % (
            scope,
            len(rows),
            len({r["writer"] for r in rows}),
            len(regions),
            by["writer"],
            by["region"],
            by["none"],
        )
    )
    if by["none"]:
        print("UNADJUDICATED -- each needs a re-attribution or a reason:")
        for r in rows:
            if r["state"] == "none":
                print("  %-30s %-46s owner=%s" % (r["region"], r["writer"], r["owner"]))
        print()
    if by["region"]:
        print("REGION-LEVEL ONLY -- the reason does not name these writers individually:")
        for r in rows:
            if r["state"] == "region":
                print("  %-30s %-46s" % (r["region"], r["writer"]))
        print()
    for rid in regions:
        decl = load_ownership().get(rid) or {}
        hc = decl.get("cross_write_host_config")
        mine = [r for r in rows if r["region"] == rid]
        print("%s -- owner=%s, %d %s writer(s)" % (rid, mine[0]["owner"], len(mine), scope))
        if hc:
            print(
                "  HOST CONFIG: holds in %s; fails in %s"
                % (hc.get("holds_in"), ", ".join(hc.get("fails_in") or []) or "(nowhere)")
            )
            print("    why:   %s" % hc.get("why", ""))
            print("    check: %s" % hc.get("check", ""))
        else:
            print("  HOST CONFIG: (none declared)")
        if show_all:
            # The region-level reason is ONE string shared by every writer under it -- printing it
            # per row would repeat the same paragraph 13 times and name none of the 13, which is
            # the readability failure that motivated the per-writer layer in the first place.
            shared = next((r["reason"] for r in mine if r["state"] == "region"), None)
            if shared:
                print("  REGION-LEVEL REASON (covers every [region] row below):")
                print("    %s" % shared)
            for r in mine:
                if r["state"] == "region":
                    print("    %-46s [region-level]" % r["writer"])
                else:
                    print("    %-46s [%s] %s" % (r["writer"], r["state"], r["reason"] or ""))
        print()
    print("UNADJUDICATED %s WRITERS: %d" % ((subsystem or "CROSS-SUBSYSTEM").upper(), by["none"]))
    return 0


def report_worklist(doc, show_all):
    """SB2's by-product: who writes what they do not own, grouped by the class of the reason."""
    xw = cross_writes(doc)
    todo = unadjudicated(xw)
    prov = doc["_provenance"]
    print(
        "CROSS-SUBSYSTEM WRITES: %d of %d region(s) with an original writer are written by a "
        "subsystem other than their declared owner.\n"
        "%d unadjudicated (no `cross_write_accepted` in tools/data/region_ownership.json).\n"
        % (
            len(xw),
            sum(1 for v in doc["regions"].values() if v.get("external_writers")),
            len(todo),
        )
    )
    if todo:
        print("UNADJUDICATED -- each needs a re-attribution or an accepted-reason:")
        for rid, v in sorted(todo.items()):
            print(
                "  %-44s owner=%-22s %s"
                % (rid, v["owner"] or "(none)", ", ".join("%s[%s]" % c for c in v["cross"][:6]))
            )
        print()
    classes = collections.Counter(
        (v["accepted"].split(":", 1)[0] if ":" in v["accepted"] else "(unclassed)")
        for v in xw.values()
        if v["accepted"]
    )
    print("ACCEPTED, by reason class:")
    for k, n in classes.most_common():
        print("  %-20s %d" % (k, n))
    if show_all:
        print()
        for rid, v in sorted(xw.items()):
            if v["accepted"]:
                print("  %-44s %s" % (rid, v["accepted"]))
    print(
        "\nSCOPE: this adjudicates the %d REGISTRY regions. %d matrix regions resolve into none of "
        "them (outside every registry extent) and are not covered by any of these numbers -- the "
        "registry is not every global in the binary."
        % (prov["registry_regions"], prov["matrix_regions_unhosted"])
    )
    return 0


def load():
    if not OUT.exists():
        return None
    return _load(OUT)


def report(limit, show_all, region_id):
    doc = load()
    if doc is None:
        print("no %s -- run --refresh first" % OUT.relative_to(REPO))
        return 1
    regions = doc["regions"]
    print("CANDIDATE SET: %s" % doc["_candidate_set"])
    print("PROVENANCE: %s" % json.dumps(doc["_provenance"]))
    print()

    if region_id:
        v = regions.get(region_id)
        if v is None:
            print("no such region: %s" % region_id)
            return 1
        print("%s  (%s, 0x%08x, %d B)" % (region_id, v["name"], v["base"], v["size"]))
        if not v["measured"]:
            print("  UNMEASURED -- the instruction scan never saw this region.")
            return 0
        print(
            "  ours:     %d writer(s), %d reader(s)"
            % (len(v["ours_writers"]), len(v["ours_readers"]))
        )
        live_w = live_external_writers(v)
        n_dead = len(v["external_writers"]) - len(live_w)
        print(
            "  external: %d writer(s)%s, %d reader(s), %d B of code"
            % (
                len(live_w),
                (" (+%d DEAD, nothing reaches them)" % n_dead) if n_dead else "",
                len(v["external_readers"]),
                v["external_bytes"],
            )
        )
        decl = load_ownership().get(region_id) or {}
        print(
            "  owner:    %s -- %s"
            % (decl.get("owner_subsystem") or "(UNCLASSIFIED)", decl.get("why") or "(no why)")
        )
        x = cross_writes(doc).get(region_id)
        if x:
            print(
                "  cross-written by %d writer(s) of another subsystem: %s"
                % (len(x["cross"]), ", ".join("%s[%s]" % c for c in x["cross"]))
            )
            print("  accepted: %s" % (x["accepted"] or "NO -- unadjudicated"))
        dead_w = set(v.get("external_writers_dead") or ())
        for label, key in (("WRITERS", "external_writers"), ("READERS", "external_readers")):
            if v[key]:
                print("  original %s:" % label)
                for f in v[key]:
                    print(
                        "    %s%s" % (f, "   [DEAD -- nothing reaches it]" if f in dead_w else "")
                    )
        return 0

    rows = sorted(
        regions.items(),
        key=lambda kv: (
            -(len(kv[1]["external_writers"]) + len(kv[1]["external_readers"])),
            -kv[1]["size"],
        ),
    )
    if not show_all:
        rows = rows[:limit]
    xw = cross_writes(doc)
    own = load_ownership()
    print(
        "%-40s %9s %5s %5s %6s  %-22s %s"
        % ("region", "bytes", "extW", "extR", "state", "owner", "xsub")
    )
    for key, v in rows:
        live_w = live_external_writers(v)
        state = (
            "-" if not v["measured"] else ("FREE" if not (live_w or v["external_readers"]) else "")
        )
        x = xw.get(key)
        print(
            "%-40s %9d %5d %5d %6s  %-22s %s"
            % (
                key[:40],
                v["size"],
                len(live_w),
                len(v["external_readers"]),
                state or "held",
                ((own.get(key) or {}).get("owner_subsystem") or "(unclassified)")[:22],
                "-" if not x else ("%d%s" % (len(x["cross"]), "" if x["accepted"] else " TODO")),
            )
        )
    free = [
        k
        for k, v in regions.items()
        if v["measured"] and not live_external_writers(v) and not v["external_readers"]
    ]
    held_w = [k for k, v in regions.items() if live_external_writers(v)]
    print()
    todo = unadjudicated(xw)
    prov = doc["_provenance"]
    print(
        "%d region(s) with NO original accessor (releasable under Law 1 today) -- %d B of state.\n"
        "%d region(s) still have an original WRITER.\n"
        "%d region(s) are cross-written (an original writer of a subsystem other than the declared "
        "owner); %d UNADJUDICATED -- see --worklist.\n"
        "%d matrix region(s) lie outside the registry entirely, so none of these counts describe "
        "them: this adjudicates the registry, not the address space."
        % (
            len(free),
            sum(regions[k]["size"] for k in free),
            len(held_w),
            len(xw),
            len(todo),
            prov["matrix_regions_unhosted"],
        )
    )
    return 0


def check():
    """The data file is present, complete and current-shaped. The INTERLOCK is gen_state_registry."""
    doc = load()
    if doc is None:
        print("MISSING %s -- run gen_region_accessors.py --refresh" % OUT.relative_to(REPO))
        return 1
    if doc.get("_schema") != SCHEMA:
        print("SCHEMA %r != %d -- regenerate" % (doc.get("_schema"), SCHEMA))
        return 1
    have = set(doc["regions"])
    want = {region_key(r) for r in registry_regions()}
    missing = want - have
    extra = have - want
    if missing or extra:
        print(
            "REGION SET DRIFT: %d region(s) in the registry with no accessor census (%s), "
            "%d census entries for regions the registry no longer has (%s). Run --refresh."
            % (
                len(missing),
                ", ".join(sorted(missing)[:5]) or "-",
                len(extra),
                ", ".join(sorted(extra)[:5]) or "-",
            )
        )
        return 1
    unsafe, conservative = currency(doc)
    if unsafe:
        print(
            "CENSUS OUT OF DATE (unsafe direction): %s changed since the census was built. A rename, "
            "a newly defined function or a new region can make the accessor lists UNDER-report -- an "
            "accessor nobody counted -- which is what could grant an owned_by_dll claim that should "
            "be refused. Run `python tools/gen_region_accessors.py --refresh` -- which needs "
            "tmp/state_matrix.json, regenerated in Ghidra with mh_gen_state_matrix.py."
            % ", ".join(unsafe)
        )
        return 1
    prov = doc["_provenance"]
    note = ""
    if conservative:
        note = (
            "\n  STALE (conservative): the reimplemented set has grown since this census was "
            "built, so it over-reports original accessors and the interlock will refuse moves it "
            "could allow. Not a failure -- refresh when convenient."
        )
    # SB2: the adjudication ratchet. A region written by a subsystem other than its declared owner
    # is either re-attributed or carries a reason; an unadjudicated one is the state SB2 closed
    # against, so it fails HERE rather than in a report nobody runs. Safe to make fatal because
    # cross_writes() recomputes `ours` from the committed ledger -- conservative staleness cannot
    # manufacture a cross-writer, and the unsafe direction already returned above.
    todo = unadjudicated(cross_writes(doc))
    if todo:
        names = ", ".join(sorted(todo)[:8])
        more = "" if len(todo) <= 8 else " (and %d more)" % (len(todo) - 8)
        print(
            "UNADJUDICATED CROSS-SUBSYSTEM WRITE(S): %d region(s) are written by an original "
            "function of a subsystem other than their declared owner, with no "
            "`cross_write_accepted` reason -- %s%s. Either the owner is wrong (re-attribute it) or "
            "the cross-write is a fact about the code that has to be written down. Run "
            "`python tools/gen_region_accessors.py --worklist`." % (len(todo), names, more)
        )
        return 1
    # TACT-WRITERS: the per-writer layer's two structural refusals. Both are the unsafe direction --
    # an entry that no longer names a live cross-writer, or an exception that never says which host
    # configuration it holds in, would both leave the ledger printing a green zero over nothing.
    ownership = load_ownership()
    stale = stale_writer_entries(doc, ownership)
    if stale:
        print(
            "STALE `cross_write_writers` ENTR(IES): %d name(s) are no longer a cross-writer of "
            "their region -- %s. A renamed, reimplemented or re-attributed function leaves its "
            "adjudication behind; delete the entry or fix the name."
            % (len(stale), ", ".join("%s/%s" % t for t in stale[:8]))
        )
        return 1
    holes = ratchet_holes(doc, ownership)
    if holes:
        print(
            "PER-WRITER RATCHET HOLE(S): %d cross-writer(s) of a region that declares "
            "`cross_write_per_writer_required` are covered only by the region-level reason -- %s. "
            "A region-level string absorbs a NEW writer silently; that is what the ratchet exists "
            "to stop. Name each one in `cross_write_writers`, or re-attribute it."
            % (len(holes), ", ".join("%s/%s" % t for t in holes[:8]))
        )
        return 1
    bad = []
    for rid, decl in sorted(ownership.items()):
        if decl.get("cross_write_writers"):
            bad += host_config_problems(rid, decl)
    if bad:
        print(
            "MALFORMED PER-WRITER ADJUDICATION(S): %d problem(s).\n  %s\n"
            "Under D4 (docs/state-boundary.md) an original cross-write is harmless in-process and "
            "wrong under a relocated or standalone bind, so an exception that does not say which "
            "configuration it holds in is an omission, not an exception."
            % (len(bad), "\n  ".join(bad))
        )
        return 1
    xw = cross_writes(doc)
    per_writer = sum(1 for r in writer_rows(doc, ownership) if r["state"] == "writer")
    print(
        "region accessors: %d regions (%d measured, %d unmeasured) against %d reimplemented "
        "functions; %d cross-written region(s), all adjudicated; %d matrix region(s) outside the "
        "registry; %d cross-writer(s) adjudicated per WRITER%s"
        % (
            len(have),
            prov["measured"],
            prov["unmeasured"],
            prov["reimpl_done"],
            len(xw),
            prov["matrix_regions_unhosted"],
            per_writer,
            note,
        )
    )
    return 0


def selftest():
    """Prove the currency guard classifies drift correctly, in both directions and in neither.

    Tested against `currency()` rather than by corrupting the committed census: the classification IS
    the logic, and a test that rewrites the data file would leave the tree dirty on failure.
    """
    ok = True
    now = fingerprints()

    def arm(label, inputs, want_unsafe, want_conservative):
        nonlocal ok
        doc = {"_provenance": ({"inputs": inputs} if inputs is not None else {})}
        unsafe, cons = currency(doc)
        got = (bool(unsafe), bool(cons))
        if got != (want_unsafe, want_conservative):
            print(
                "SELFTEST FAIL (%s): expected unsafe=%s conservative=%s, got unsafe=%s conservative=%s"
                % (label, want_unsafe, want_conservative, unsafe, cons)
            )
            ok = False
        else:
            print("ok: %s" % label)

    arm("an unchanged census is current", dict(now), False, False)
    arm(
        "a moved en_functions is UNSAFE (a rename can hide an accessor)",
        {**now, "en_functions": "0" * 16},
        True,
        False,
    )
    arm(
        "a moved region registry is UNSAFE (a new region has no measured accessors)",
        {**now, "state_regions": "0" * 16},
        True,
        False,
    )
    arm(
        "a grown reimplemented set is CONSERVATIVE (it can only over-report)",
        {**now, "reimpl_done": "0" * 16},
        False,
        True,
    )
    arm(
        "a census with NO fingerprint block is unsafe -- unknown is not unchanged",
        None,
        True,
        False,
    )

    # SB2b (2026-09-01): the DIRTY-INPUT guard. `--refresh` fingerprints the file on disk, so a
    # refresh taken over an uncommitted en_functions.json records a value that may never be
    # committed -- green here, red for everyone after. That shipped: the census committed 2026-08-30
    # records a fingerprint NO commit of en_functions.json has ever had, and the gate was red for
    # ~40 commits behind two truthful "lint_repo: PASS" messages. Both directions are armed because
    # a guard that cannot be shown to FIRE is the same shape of evidence as the bug it replaces.
    def guard_arm(label, make_dirty, allow, want_refuse):
        nonlocal ok
        real_dirty, real_matrix = globals()["dirty_inputs"], globals()["MATRIX"]
        globals()["dirty_inputs"] = lambda: ["tools/data/en_functions.json"] if make_dirty else []
        # MATRIX is pointed at an absent path so the ALLOW arm cannot reach the write. Both arms
        # therefore return 1, which is why the discriminator below is the printed REASON and not the
        # exit code -- a guard proved only by an exit code shared with the missing-matrix path would
        # be proving nothing.
        globals()["MATRIX"] = REPO / "tmp" / "__selftest_absent_matrix__.json"
        try:
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                refresh(allow_dirty=allow)
            refused = "REFUSING TO REFRESH" in buf.getvalue()
            if refused != want_refuse:
                print("SELFTEST FAIL (%s): refused=%s, expected %s" % (label, refused, want_refuse))
                ok = False
            else:
                print("ok: %s" % label)
        finally:
            globals()["dirty_inputs"], globals()["MATRIX"] = real_dirty, real_matrix

    guard_arm("a DIRTY fingerprinted input refuses the refresh", True, False, True)
    guard_arm("--allow-dirty-inputs overrides the refusal", True, True, False)
    guard_arm("clean inputs do not trip the guard", False, False, False)

    # SB2: the cross-write ratchet, armed the same way -- against the real census and the real
    # declarations, because the property being checked is a JOIN of the two and a fixture would
    # prove only that the join code runs.
    doc = load()
    if doc is None:
        print("SELFTEST FAIL: no census on disk")
        return 1
    ownership = load_ownership()
    xw = cross_writes(doc, ownership)
    todo = unadjudicated(xw)
    if todo:
        print(
            "SELFTEST FAIL (committed tree): %d unadjudicated cross-write(s): %s"
            % (len(todo), ", ".join(sorted(todo)[:6]))
        )
        return 1
    print("ok: every cross-written region is adjudicated -- clean, as it must be")

    victim = next(iter(sorted(xw)))
    stripped = json.loads(json.dumps(ownership))
    del stripped[victim]["cross_write_accepted"]
    if victim not in unadjudicated(cross_writes(doc, stripped)):
        print("SELFTEST FAIL: dropping %s's accepted-reason did not make it unadjudicated" % victim)
        return 1
    print("ok: dropping one accepted-reason -- %s goes unadjudicated" % victim)

    # And the other direction: a WRONG owner manufactures cross-writes, which is what makes the
    # column a check on the declaration rather than a restatement of it. `units` is written by 22
    # original sim/AI functions; declaring it Sound-owned must light every one of them up.
    # TACT-WRITERS: the per-writer layer, armed the same way. Region-level adjudication is not
    # enough for SB-SOLE's population, so the arms below have to fail on a per-WRITER hole even
    # while the region-level count stays a green zero.
    per = {
        rid: list((ownership.get(rid) or {}).get("cross_write_writers") or {})
        for rid in sorted(ownership)
        if (ownership.get(rid) or {}).get("cross_write_writers")
    }
    if not per:
        print("SELFTEST FAIL: no region carries per-writer adjudication -- nothing to arm against")
        return 1
    # PICK A WRITER THAT IS STILL EXTERNAL. `cross_write_writers` is authored data and can outlive
    # the fact it describes: when a batch translates a function, that function stops being a CROSS
    # writer, `writer_rows` stops emitting a row for it, and a probe that happened to pick it fails
    # with "did not demote it to region-level" -- a broken selftest reported as a real finding.
    # SIM1-H hit exactly that (G_PLANET_STATUS/SwitchToPlanet, 2026-09-10). Skipping owned writers
    # keeps the arm pointed at a live one instead of re-breaking on the next translation batch.
    owned_now = {r["writer"] for r in writer_rows(doc, ownership) if r["state"] != "region"} | {
        r["writer"] for r in writer_rows(doc, ownership)
    }
    prid = pfn = None
    for rid in per:
        for fn in per[rid]:
            if any(r["region"] == rid and r["writer"] == fn for r in writer_rows(doc, ownership)):
                prid, pfn = rid, fn
                break
        if prid:
            break
    if prid is None:
        print(
            "SELFTEST FAIL: every per-writer adjudication names a function that is no longer a "
            "cross-writer -- the authored data is stale, not the check"
        )
        return 1
    holed = json.loads(json.dumps(ownership))
    del holed[prid]["cross_write_writers"][pfn]
    got = [
        r
        for r in writer_rows(doc, holed)
        if r["region"] == prid and r["writer"] == pfn and r["state"] == "region"
    ]
    if not got:
        print(
            "SELFTEST FAIL: dropping %s's per-writer entry for %s did not demote it to "
            "region-level" % (prid, pfn)
        )
        return 1
    print("ok: dropping one per-writer entry -- %s/%s demotes to region-level" % (prid, pfn))

    # ... and with the region-level string gone too, that same writer must go UNADJUDICATED. This is
    # the arm that proves the count TACT-WRITERS closes against can actually go non-zero.
    holed[prid].pop("cross_write_accepted", None)
    got = [
        r
        for r in writer_rows(doc, holed)
        if r["region"] == prid and r["writer"] == pfn and r["state"] == "none"
    ]
    if not got:
        print(
            "SELFTEST FAIL: %s/%s did not go unadjudicated with both layers removed" % (prid, pfn)
        )
        return 1
    print("ok: with both layers removed -- %s/%s is UNADJUDICATED" % (prid, pfn))

    if ownership[prid].get("cross_write_per_writer_required"):
        if (prid, pfn) not in ratchet_holes(doc, holed):
            print("SELFTEST FAIL: the ratchet did not report %s/%s as a hole" % (prid, pfn))
            return 1
        print("ok: the ratchet reports %s/%s as a hole when its entry is dropped" % (prid, pfn))
        if ratchet_holes(doc, ownership):
            print(
                "SELFTEST FAIL (committed tree): %d ratchet hole(s)"
                % len(ratchet_holes(doc, ownership))
            )
            return 1
        print("ok: the committed tree has no ratchet hole")
    else:
        print("SELFTEST FAIL: %s does not declare cross_write_per_writer_required" % prid)
        return 1

    renamed = json.loads(json.dumps(ownership))
    renamed[prid]["cross_write_writers"]["llm_no_such_function_fixture"] = "fixture: not a writer"
    if (prid, "llm_no_such_function_fixture") not in stale_writer_entries(doc, renamed):
        print("SELFTEST FAIL: a per-writer entry naming a non-writer was not reported stale")
        return 1
    print("ok: a per-writer entry naming a non-writer is STALE, not silently accepted")

    for label, mutate in (
        ("a missing host-config block", lambda d: d.pop("cross_write_host_config", None)),
        (
            "an unknown holds_in",
            lambda d: d.update(
                cross_write_host_config={**d["cross_write_host_config"], "holds_in": "somewhere"}
            ),
        ),
        (
            "holds_in != any with an empty fails_in",
            lambda d: d.update(
                cross_write_host_config={**d["cross_write_host_config"], "fails_in": []}
            ),
        ),
        (
            "an empty check",
            lambda d: d.update(
                cross_write_host_config={**d["cross_write_host_config"], "check": "  "}
            ),
        ),
    ):
        broken = json.loads(json.dumps(ownership))
        mutate(broken[prid])
        if not host_config_problems(prid, broken[prid]):
            print("SELFTEST FAIL: %s was accepted as a well-formed host config" % label)
            return 1
        print("ok: %s is refused" % label)
    if host_config_problems(prid, ownership[prid]):
        print(
            "SELFTEST FAIL (committed tree): %s's host config is malformed: %s"
            % (prid, host_config_problems(prid, ownership[prid]))
        )
        return 1
    print("ok: %s's committed host config is well-formed" % prid)

    rewired = json.loads(json.dumps(ownership))
    rewired["UNITS"]["owner_subsystem"] = "Sound"
    rewired["UNITS"].pop("cross_write_accepted", None)
    got = cross_writes(doc, rewired).get("UNITS")
    # The expectation is every ORIGINAL writer, i.e. the same not-ours filter cross_writes itself
    # applies -- not the raw external_writers list. A reimplemented writer is not an original one, so
    # once any UNITS writer is promoted to `done` the raw count is unreachable BY CONSTRUCTION and
    # this case would fail for a reason that has nothing to do with owner declarations. (It did, the
    # day AI1D's seven roster writers were verified: 12 vs the raw list.) Recomputing the expectation
    # the same way keeps the case's real question -- does a wrong owner cross-write EVERYTHING it can
    # -- while the answer stops depending on how much of the binary has been migrated.
    fns = function_table()
    ours = {a for a, v in load_reimpl(str(REPO)).items() if v == "done"}
    expected = [
        f
        for f in (doc["regions"]["UNITS"].get("external_writers") or [])
        if fns.get(f, (None, 0))[0] not in ours
    ]
    if not got or len(got["cross"]) != len(expected):
        print(
            "SELFTEST FAIL: mis-declaring UNITS as Sound-owned did not cross-write all %d of its "
            "still-original writers (got %s)" % (len(expected), got and len(got["cross"]))
        )
        return 1
    print(
        "ok: a wrong owner cross-writes all %d of UNITS' still-original writers" % len(got["cross"])
    )

    # ---- SCHEMA 4: the DEAD-writer discount, both arms ---------------------------------------
    # A `dead` row is an evidenced "nothing reaches it", so it must not hold a region -- and a LIVE
    # writer must still hold one. Both arms, because a discount that fires on everything would read
    # exactly like a clean boundary.
    probe = {
        "external_writers": ["a_live_writer", "a_dead_writer"],
        "external_writers_dead": ["a_dead_writer"],
    }
    if live_external_writers(probe) != ["a_live_writer"]:
        print("SELFTEST FAIL: live_external_writers did not discount the dead writer")
        return 1
    print("ok: a dead writer is discounted, a live one is not")
    if live_external_writers({"external_writers": ["w"], "external_writers_dead": []}) != ["w"]:
        print("SELFTEST FAIL (over-discount): a region with no dead rows lost a live writer")
        return 1
    print("ok: the discount does NOT over-fire on a region with no dead writers")

    # The stored subset must really be a subset, and must agree with the ledger -- otherwise the
    # discount could hide a live writer by mislabelling it.
    from _reimpl import load_dead as _ld

    _dead_addrs = _ld(str(REPO))
    _fns = function_table()
    bad = []
    for _k, _v in doc["regions"].items():
        _d = set(_v.get("external_writers_dead") or ())
        if not _d <= set(_v.get("external_writers") or ()):
            bad.append((_k, "not a subset of external_writers"))
        for _f in _d:
            if _fns.get(_f, (None, 0))[0] not in _dead_addrs:
                bad.append((_k, "%s is marked dead but has no dead ledger row" % _f))
    if bad:
        print("SELFTEST FAIL: external_writers_dead disagrees with the ledger: %s" % bad[:5])
        return 1
    _n = sum(len(v.get("external_writers_dead") or ()) for v in doc["regions"].values())
    print(
        "ok: all %d external_writers_dead entr(ies) are a real subset with a dead ledger row" % _n
    )
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--refresh", action="store_true", help="rebuild the census from tmp/state_matrix.json"
    )
    ap.add_argument(
        "--allow-dirty-inputs",
        action="store_true",
        help="refresh even though en_functions.json / state_regions.json are modified in git. The "
        "fingerprint recorded is then of a file that may never be committed, which is exactly the "
        "failure the guard exists for -- so it is stamped into _provenance.inputs_dirty_at_refresh "
        "rather than being invisible. Commit the input first unless you have a reason not to.",
    )
    ap.add_argument("--check", action="store_true", help="drift gate (tools/lint_repo.py)")
    ap.add_argument("--all", action="store_true", help="report every region, not just the top ones")
    ap.add_argument("--limit", type=int, default=30, help="rows in the default report")
    ap.add_argument("--region", help="report one region and name its accessors")
    ap.add_argument(
        "--worklist",
        action="store_true",
        help="the cross-subsystem write worklist (SB2): who writes what they do not own",
    )
    ap.add_argument(
        "--writers",
        nargs="?",
        const="",
        metavar="SUBSYSTEM",
        help="the per-WRITER cross-write ledger; optionally filtered to one subsystem "
        '(e.g. --writers "Tactical mode")',
    )
    ap.add_argument(
        "--selftest", action="store_true", help="prove the currency guard classifies drift"
    )
    args = ap.parse_args()
    if args.refresh:
        return refresh(allow_dirty=args.allow_dirty_inputs)
    if args.selftest:
        return selftest()
    if args.check:
        return check()
    if args.writers is not None:
        doc = load()
        if doc is None:
            print("no %s -- run --refresh first" % OUT.relative_to(REPO))
            return 1
        return report_writers(doc, args.writers or None, args.all)
    if args.worklist:
        doc = load()
        if doc is None:
            print("no %s -- run --refresh first" % OUT.relative_to(REPO))
            return 1
        return report_worklist(doc, args.all)
    return report(args.limit, args.all, args.region)


if __name__ == "__main__":
    sys.exit(main())
