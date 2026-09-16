#!/usr/bin/env python3
"""Generate a migration domain's per-function ledger (--domain ai | sim | ...).

WHY THIS EXISTS. The AI closure proposes batches A-E in PROSE. A prose batch cannot be picked
up by session 12 of a ~20-session loop: there is no list of which functions are in it, no record of
which are translated, and nothing that goes red when the ledger drifts from what the tree actually
contains. This emits the same decomposition as DATA, derived from the three artifacts the closure
analysis itself used, so it can be regenerated instead of maintained.

DOMAIN-PARAMETERISED 2026-08-07, when SIM1 was decomposed and became the second cluster. Everything
domain-specific -- the root, the membership selector, the batch rules, the output path -- now lives
in `tools/data/migration/<domain>.json` (see tools/migration_domain.py). The acceptance test for
that lift was that `--domain ai` reproduces the existing 209-row ledger byte-for-byte.

TWO SELECTOR MODES, because the two clusters are genuinely different shapes:

  name_prefix (ai)  -- membership is "reachable-ish AND name starts with llm_strat_ai_". Works only
                       because someone had already named the cluster consistently.
  closure (sim)     -- membership is the forward closure from the root, NOT descending into the
                       declared cut families (presentation/sound/net), minus the shared helpers Law
                       4 keeps original, minus whatever another domain's manifest already owns.
                       The sim needs this because it has no single prefix and its uncut closure is
                       778 functions -- most of the binary.

THE LEDGER IS DERIVED, NOT AUTHORED. Everything here comes from:

  docs/symbols.md              -- address, name, committed signature, tags (EN-canonical)
  tools/data/en_functions.json -- the real body extent per function (size); see load_symbols()
  tools/data/call_graph_no_crt.json -- reachability from the AI root, and the call layering
  tmp/state_matrix.json        -- the measured per-function write set, shared vs island

The one thing this file does NOT derive is progress: `state`, `evidence_tier` and `promoted` are
WRITTEN BY THE LOOP and preserved across regeneration (see `--merge`). Regenerating never silently
resets work that has been done; it re-derives the facts around it.

THE LAYER FIELD IS THE ARMING CONSTRAINT, NOT A PRIORITY. Shadow sites hook the callee ENTRY, so
arming a caller and a callee in the same run makes the original arm re-enter the inner site
(`reimpl-loop` skill, "Nested sites do not compose"). Two functions may share a rig run only if
neither can reach the other -- an ANTICHAIN in the call graph. `layer` is the longest-path depth
within the AI-only subgraph, which is a sufficient antichain: equal layer implies neither reaches
the other, EXCEPT inside a call cycle, which is why `in_cycle` is emitted separately. Use
`--antichain` to get a checked grouping rather than trusting the layer column by eye.

BATCH ASSIGNMENT IS A PROPOSAL AND SAYS SO. Rules below are mechanical; anything they cannot decide
is emitted as `"unassigned"` rather than guessed, because a wrong batch label that looks authoritative
is worse than an honest gap (docs/conventions.md#documentation-and-generated-docs: stale structured data
reads as authoritative).

Usage:
    python tools/gen_migration.py --domain ai          # regenerate, preserving recorded progress
    python tools/gen_migration.py --domain sim --check # non-zero exit if the file is stale
    python tools/gen_migration.py --domain sim --antichain   # print rig-armable groups
    python tools/gen_migration.py --domain sim --effects     # the outward-effect (shadow-safety) surface
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

from migration_domain import add_domain_arg, load_domain

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYMBOLS = os.path.join(REPO, "docs", "symbols.md")
CALLGRAPH = os.path.join(REPO, "tools", "data", "call_graph_no_crt.json")
ADDR_TAKEN = os.path.join(REPO, "tools", "data", "en_addr_taken.json")
MATRIX = os.path.join(REPO, "tmp", "state_matrix.json")
# COMMITTED, unlike MATRIX: which regions the oracle hashes / snapshots. The join of the two is what
# says whether suppressing a target removes a write the comparison was going to read.
STATE_REGIONS = os.path.join(REPO, "tools", "data", "state_regions.json")
FUNCTIONS = os.path.join(REPO, "tools", "data", "en_functions.json")

# Populated by configure() from the domain profile before anything else runs. They are module
# globals rather than threaded parameters because this is a single-domain-per-invocation CLI and
# the alternative was passing `dom` through nine functions that otherwise carry no configuration.
# configure() is the ONLY writer.
DOM = None
OUT = None
BATCH_OVERRIDES = None
ROOT = None
PREFIX = None
SELECT = None
BATCH_RULES = []
ROSTER_REGIONS = frozenset()
CREATION_PRIMITIVES = frozenset()
PRESERVE_BUGS = {}
RECLASSIFIED_OUT = []


def configure(dom):
    """Bind the module's domain-specific facts from a profile. Call once, before build()."""
    global DOM, OUT, BATCH_OVERRIDES, ROOT, PREFIX, SELECT
    global BATCH_RULES, ROSTER_REGIONS, CREATION_PRIMITIVES, PRESERVE_BUGS, RECLASSIFIED_OUT
    DOM = dom
    OUT = dom.path("manifest")
    BATCH_OVERRIDES = dom.path("batch_overrides", required=False) or ""
    ROOT = dom.root_addr
    SELECT = dom.select
    PREFIX = dom.select.get("prefix", "")
    BATCH_RULES = dom.batch_rules
    ROSTER_REGIONS = dom.roster_regions
    CREATION_PRIMITIVES = frozenset(dom.get("creation_primitives", ()))
    PRESERVE_BUGS = dict(dom.get("preserve_bugs", {}))
    RECLASSIFIED_OUT = load_reclassified(dom)


def load_reclassified(dom):
    """The domain's record of rows that LEFT the ledger because the RE said they never belonged.

    Kept in its own file rather than in the profile because it is a growing prose archive, and a
    profile a human has to read past 60 lines of history to find the batch rules is a profile that
    stops being read. Absent file = no reclassifications yet, which is the normal state of a new
    domain.
    """
    path = dom.path("reclassified", required=False)
    if not path or not os.path.isfile(path):
        return []
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)["reclassified_out"]


# Progress fields the loop owns. Regeneration preserves these; everything else is re-derived.
PROGRESS_FIELDS = ("state", "evidence_tier", "promoted", "notes")
# `dead` is a TERMINAL state parallel to `verified`, not a step on the way to it (added 2026-08-07).
# A function tagged `todo:dead` is settled as unreachable and must NOT be migrated, so it can never
# reach `verified` -- which in this ledger means A REIMPLEMENTATION WAS PROVEN EQUIVALENT, a claim
# there is nothing to back for a function with no reimplementation. Its `notes` carry the settle
# evidence (what was scanned, why nothing reaches it). Consumers that scope on `not_started`
# (the readiness report --untranslated) correctly drop these; the session driver counts them in their own column.
DEFAULT_PROGRESS = {
    # The closed vocabulary for these two fields, and the tier<=>verified invariant, are OWNED by
    # the migration-ledger schema (ALLOWED_STATES / ALLOWED_TIERS) and enforced by lint_repo +
    # migration_session --end. Keep the two literals below in step with that module.
    "state": "not_started",  # not_started | translated | reviewed | armed | verified | dead
    "evidence_tier": None,  # None until VERIFIED, then T1|T2|T3 (grades the proof; set only on a verified row)
    # INSTALLATION FACT, NOT A PROOF GRADE (settled 2026-09-10, X-PROMFLAG).
    # `promoted: true` means exactly one thing: A SOURCE TU IN THIS TREE CARRIES AN
    # `MH_EXPORT_REPLACE(<this row's name>, ...)` -- the entry redirect whose E9 sends the original's
    # entry to our body. It is a statement about the TREE, checkable by grep, and nothing else:
    #   * NOT "our body is proven right"      -- that is `state: verified` + `evidence_tier`.
    #   * NOT "our body ran"                  -- a SHIP_PROMOTE_* key may hold the seam off, and the
    #                                            verdict channel is the tombstone sweep, not this flag.
    #   * NOT "the state was promoted to verified" -- the English sense of the word, which is exactly
    #                                            how five sim rows came to be flagged with no macro
    #                                            anywhere (their notes say "Promoted to verified/T1").
    #   * NOT "ours is what executes here"    -- a closure-internal row is entered by a direct call
    #                                            FROM a promoted body and correctly stays `false`,
    #                                            while its original is just as dead as the root's.
    # It is therefore a FLOOR on what runs, never a measurement of it (tools/_reimpl.py says the same
    # of its `ledger_promoted` roll-up). Ratcheted: `report_promotion_reconciliation.py --check-flags`
    # runs in lint_repo and fails BY NAME on a row whose flag and whose macro disagree either way.
    "promoted": False,
    "notes": "",
}


SYM_RE = re.compile(r"\| `0x([0-9a-f]{8})` \| `([^`]+)` \| `([^`]*)` \|([^|]*)\|")


def load_body_sizes():
    """Return {addr: real body size} from tools/data/en_functions.json, or {} if absent.

    That file is Ghidra-derived (the Ghidra-side function dump) and its `size` is the function's actual
    body extent, so it is the correct source. Measured 2026-08-01 against live Ghidra: it agrees on
    all 218 AI functions exactly.
    """
    if not os.path.isfile(FUNCTIONS):
        return {}
    with open(FUNCTIONS, encoding="utf-8") as fh:
        data = json.load(fh)
    return {f["entry"].lower().replace("0x", ""): f["size"] for f in data.get("functions", [])}


def load_symbols():
    """Return {addr: {...}} for every function in docs/symbols.md.

    SIZE COMES FROM THE REAL BODY EXTENT (tools/data/en_functions.json), not from the symbol gap.
    The gap estimate (next_addr - addr) is only the fallback for an address that file does not
    carry, and such rows are flagged `size_is_gap_estimate`.

    Why this matters, measured 2026-08-01: the gap overstates 27 of 218 AI functions and by up to
    2.7x -- llm_strat_ai_unit_should_abandon_target reads 2275 B by gap and is 833 B of body,
    llm_strat_ai_tick_all_groups 1504 vs 650. The gap swallows alignment padding AND any unclaimed
    dead bytes following the function, of which this cluster has a lot. An upper bound is harmless
    for budgeting but NOT harmless here, because `review_required` routes on `size_bytes > p90`:
    an inflated size sends a small function to adversarial review and, by pushing p90 up, lets a
    genuinely large one past it. A routing input must be the measured quantity, not a bound on it.
    """
    body = load_body_sizes()
    rows = []
    with open(SYMBOLS, encoding="utf-8") as fh:
        for line in fh:
            m = SYM_RE.match(line)
            if m:
                rows.append((int(m.group(1), 16), m.group(2), m.group(3), m.group(4).split()))
    rows.sort()
    out = {}
    for i, (addr, name, sig, tags) in enumerate(rows):
        key = "%08x" % addr
        nxt = rows[i + 1][0] if i + 1 < len(rows) else addr + 64
        gap = max(0, min(nxt - addr, 20000))
        real = body.get(key)
        out[key] = {
            "name": name,
            "size_bytes": real if real is not None else gap,
            "size_is_gap_estimate": real is None,
            "signature": sig,
            "tags": tags,
        }
    return out


def functions_currency():
    """Do `docs/symbols.md` and `tools/data/en_functions.json` agree on every shared address?

    Returns (disagreements, checked) where a disagreement is (addr, symbols_name, functions_name).

    WHY THIS GUARD EXISTS (finding 2026-08-24-1151-19, measured 2026-08-24). A seeded domain selects
    its members by NAME and resolves each name to an address through `en_functions.json`. That file
    is written by the Ghidra-side function dump, which is NOT in the usual per-session dump set (dump_symbols /
    dump_structs / dump_struct_layouts / dump_call_protos) -- so a session that RENAMES functions
    refreshes every other input and leaves this one holding the old names. The seeds then resolve to
    nothing and the renamed rows LEAVE THE DOMAIN, silently.

    It is not hypothetical and it was not caught by reading the output. After renaming 17 `FUN_`
    functions, gen_migration reported `functions 366 ... seeds_unresolved: [the 17 OLD names]`, then
    `functions 349` once the census refreshed -- and migration_ready went on to report
    `315/315 migration fns clear, 0 HARD blocker(s)`: a GREEN over a scope 17 rows smaller than the
    one the session was being scored against, with nothing in either tool's output naming the shrink.
    A readiness predicate is only as honest as the manifest it runs over, so the manifest has to
    refuse rather than shrink.

    MTIME IS NOT THE TEST. A `git checkout` sets mtimes to checkout time in whatever order it likes,
    so an mtime comparison goes red on a clean tree and green on a genuinely stale one. This compares
    CONTENT: the same address named two different things by two inputs is drift, whichever way round
    it falls. `gen_region_accessors` guards its census the same way, by fingerprinting content rather
    than trusting a clock.

    Only addresses present in BOTH files are compared. `symbols.md` is scoped by naming convention
    (its own header says "All `llm_` functions and `_G_LLM_` globals") so it legitimately omits
    hand-named and still-`FUN_` functions -- an absence is not drift, and `augment_symbols` exists
    precisely to fill it.
    """
    if not os.path.isfile(FUNCTIONS) or not os.path.isfile(SYMBOLS):
        return [], 0
    with open(FUNCTIONS, encoding="utf-8") as fh:
        fn_name = {
            f["entry"].lower().replace("0x", ""): f["name"]
            for f in json.load(fh).get("functions", [])
        }
    bad, checked = [], 0
    with open(SYMBOLS, encoding="utf-8") as fh:
        for line in fh:
            m = SYM_RE.match(line)
            if not m:
                continue
            addr, name = m.group(1), m.group(2)
            other = fn_name.get(addr)
            if other is None:
                continue
            checked += 1
            if other != name:
                bad.append((addr, name, other))
    return bad, checked


def augment_symbols(out, name_by_id):
    """Add rows for functions `docs/symbols.md` does not carry. Returns the count added.

    WHY THIS IS NEEDED, and it is the same defect class the manifest's own method note warns about.
    `docs/symbols.md` is scoped BY NAMING CONVENTION -- its header says "All `llm_` functions and
    `_G_LLM_` globals" -- so every hand-named `game_*` / `map_*` function and every still-unnamed
    `FUN_*` is absent from it. That never mattered for `ai`, whose 209 rows are `llm_strat_ai_*` to
    a function. It matters enormously for `sim`: 38 of its 311 members are outside the convention,
    including `game_SetEvent`, `map_CreateBuilding`, `map_unit_Add`, `game_UpdateProgress` and
    `game_HandleInvasion` -- load-bearing sim code, not leaves. Filtering them out for want of a
    row in a doc scoped to a different question would have shipped a ledger that silently omitted
    12% of the subsystem, and nothing downstream would have noticed.

    So a symbol source scoped to a naming convention is a fine PRIMARY and a broken ONLY. Tags and
    the committed signature still come from symbols.md where it has them; name comes from the call
    graph (which carries every function), size from en_functions.json, and the proto-committed bit
    from dll_call_protos.json for rows that have no signature string to inspect.
    """
    body = load_body_sizes()
    protos = {}
    ppath = os.path.join(REPO, "tools", "data", "dll_call_protos.json")
    if os.path.isfile(ppath):
        with open(ppath, encoding="utf-8") as fh:
            for r in json.load(fh).get("functions", []):
                # Skip synthesized varargs-SHAPE rows: they share the base function's `va`, so
                # letting one land here would answer per-function questions with a per-call-shape
                # row (and which one won would be dict-insertion order). The base row stays
                # authoritative -- the function really is variadic.
                if r.get("from_varargs_shape"):
                    continue
                protos[r["va"].lower().replace("0x", "")] = r
    graph_tags = {}
    with open(CALLGRAPH, encoding="utf-8") as fh:
        for n in json.load(fh)["nodes"]:
            graph_tags[n["id"]] = n.get("tags") or []

    added = 0
    for addr, name in name_by_id.items():
        if addr in out or ":" in addr:  # ':' -> EXTERNAL: pseudo-nodes, not code we can migrate
            continue
        p = protos.get(addr)
        out[addr] = {
            "name": name,
            "size_bytes": body.get(addr, 0),
            "size_is_gap_estimate": addr not in body,
            # No signature string exists for these, so `proto_committed` cannot be read off one.
            # It is taken from the committed calling contract instead, which is what actually
            # gates translation (gen_dll_calls.py emits a compile-error stub for anything not ok).
            "signature": "",
            "proto_ok": bool(p and p.get("status") == "ok"),
            "tags": graph_tags.get(addr, []),
        }
        added += 1
    return added


def load_callgraph():
    """Return (name_by_id, callees, callers) over the no-CRT call graph."""
    with open(CALLGRAPH, encoding="utf-8") as fh:
        g = json.load(fh)
    name_by_id = {n["id"]: n.get("name", n["id"]) for n in g["nodes"]}
    callees, callers = {}, {}
    for link in g["links"]:
        s, t = link["source"], link["target"]
        callees.setdefault(s, set()).add(t)
        callers.setdefault(t, set()).add(s)
    return name_by_id, callees, callers


def reachable_from(root, callees):
    """Plain forward BFS. No ui/view cut: this walks the AI subgraph only (see caller)."""
    seen, stack = {root}, [root]
    while stack:
        cur = stack.pop()
        for nxt in callees.get(cur, ()):
            if nxt not in seen:
                seen.add(nxt)
                stack.append(nxt)
    return seen


def load_addr_taken():
    """{function id} whose ADDRESS is materialised somewhere in the image, or None if unavailable.

    Optional enrichment, same contract gen_state_registry uses for the state matrix: absent means
    the caller degrades rather than guesses. Produced by the Ghidra-side address-taken scan.
    """
    if not os.path.isfile(ADDR_TAKEN):
        return None
    with open(ADDR_TAKEN, encoding="utf-8-sig") as fh:
        data = json.load(fh)
    out = {a.lower().replace("0x", "") for a in data.get("addr_taken", {})}
    out |= {a.lower().replace("0x", "") for a in data.get("entry_roots", [])}
    return out


def orphan_closure(nodes, callers, addr_taken):
    """{id} in `nodes` that NO live call chain reaches -- the seeded domain's reachability answer.

    A function is orphaned when it has no callers, or when every one of its callers is itself
    orphaned. That second clause is the whole point: `zero_callers` is one hop deep, and a dead
    function called only by two other dead functions passes it. Measured on sim_resid 2026-08-31,
    llm_strat_bldg_set_footprint_passable had TWO callers and was reachable from nothing --
    `zero_callers` said False and the row was scheduled for translation.

    ADDRESS-TAKEN FUNCTIONS ARE NEVER ORPHANED. A call graph cannot see an indirect entry, so a
    state-machine handler in a function-pointer table has zero call edges and is entered every
    frame; without this exemption the verdict would condemn the whole SIM1-DISPATCH handler set.
    When the scan is unavailable (addr_taken is None) this returns an EMPTY set rather than a
    guess: no verdict at all beats a confident wrong one, in the direction that ref-manager
    blindness makes the expensive one.
    """
    if addr_taken is None:
        return set()
    # Fixpoint from the optimistic side: assume everything is live, then withdraw. Iterating the
    # other way round (assume orphan, promote) would need a root set we do not have.
    orphan = set()
    changed = True
    while changed:
        changed = False
        for n in nodes:
            if n in orphan or n in addr_taken:
                continue
            live_callers = [c for c in callers.get(n, ()) if c not in orphan]
            if not live_callers:
                orphan.add(n)
                changed = True
    return orphan


def compute_layers(nodes, edges):
    """Longest-path depth per node over the induced subgraph, plus cycle membership.

    Longest path (not shortest) is what makes equal-layer safe to co-arm: if A reaches B then A's
    depth is strictly greater, so they land in different layers. Nodes inside a call cycle cannot be
    ordered that way, so they are marked `in_cycle` and the antichain checker treats them as
    mutually exclusive regardless of layer.

    REWRITTEN 2026-08-07 to condense strongly connected components first. The previous version
    relaxed |V| times and called "anything still moving on the last pass" a cycle member, which is
    a proxy that fails in both directions the moment a real cycle exists: on the sim domain it
    reported in_cycle 27 against a true 4, and inflated max_layer to 824 over 273 nodes, because a
    cycle keeps pumping every DESCENDANT of the cycle, not just its members. It went unnoticed
    because the AI subgraph is acyclic (in_cycle 0), so the proxy and the truth agreed vacuously.

    Now: Tarjan for the SCCs, then longest path over the condensation. `in_cycle` is exactly
    membership of a non-trivial SCC, and every member of one shares the component's layer -- which
    is the honest answer, since no ordering exists inside a cycle and the antichain checker must
    treat them as mutually exclusive anyway.
    """
    # -- Tarjan, iterative: this graph is a call graph and can nest deeper than the recursion limit.
    index, low, on_stack, stack = {}, {}, set(), []
    comp_of, comps, counter = {}, [], [0]
    for root in sorted(nodes):
        if root in index:
            continue
        work = [(root, iter(sorted(edges.get(root, ()))))]
        index[root] = low[root] = counter[0]
        counter[0] += 1
        stack.append(root)
        on_stack.add(root)
        while work:
            u, it = work[-1]
            descended = False
            for v in it:
                if v not in nodes:
                    continue
                if v not in index:
                    index[v] = low[v] = counter[0]
                    counter[0] += 1
                    stack.append(v)
                    on_stack.add(v)
                    work.append((v, iter(sorted(edges.get(v, ())))))
                    descended = True
                    break
                if v in on_stack:
                    low[u] = min(low[u], index[v])
            if descended:
                continue
            work.pop()
            if work:
                low[work[-1][0]] = min(low[work[-1][0]], low[u])
            if low[u] == index[u]:
                comp = []
                while True:
                    w = stack.pop()
                    on_stack.discard(w)
                    comp_of[w] = len(comps)
                    comp.append(w)
                    if w == u:
                        break
                comps.append(comp)

    in_cycle = {n for c in comps if len(c) > 1 for n in c}

    # -- longest path over the condensation (a DAG, so the relaxation terminates by construction)
    cedges = {i: set() for i in range(len(comps))}
    for a in nodes:
        for b in edges.get(a, ()):
            if b in comp_of and comp_of[a] != comp_of[b]:
                cedges[comp_of[a]].add(comp_of[b])
    cdepth = {i: 0 for i in range(len(comps))}
    for _ in range(len(comps) + 1):
        moved = False
        for i in range(len(comps)):
            for j in cedges[i]:
                if cdepth[j] < cdepth[i] + 1:
                    cdepth[j] = cdepth[i] + 1
                    moved = True
        if not moved:
            break
    layer = {n: cdepth[comp_of[n]] for n in nodes}
    return layer, in_cycle


def load_write_sets(ai_names):
    """Return {fn_name: {"shared": [...], "island": [...]}} from the measured matrix.

    WRITES ARE `write` + `rw`, AND NOTHING ELSE. This used to also admit any cell with a non-zero
    `movs` provenance count, which is wrong in a way that manufactures blockers: gen_state_matrix
    bills a string-copy site under BOTH its kind (`read` for the ESI side, `write` for the EDI side)
    AND its provenance (`movs`), so `movs > 0` says "reached through a rep-movs pointer", never
    "written". Every MOVS source therefore arrived here as a claimed write -- `MOV ESI,0x500580` in
    llm_tact_mission_load made the .rdata path literal "panelb\\ilp_typ.gfx" a write target, and
    migration_ready then raised an R8 "register this region" blocker on a .rdata constant that
    nothing writes. Measured over the whole binary before the fix: 3550 cells claimed as writes, of
    which 43 (35 functions, 37 regions) had `write == 0 and rw == 0` -- all of them MOVS *sources*
    (`s_setup.dat`, `s_TUTORIAL.MP`, `s_localhost`, the AI scan-target and lobby-slot copy sources).
    Nothing is lost by dropping the clause: of the 74 cells carrying a sampled MOVS-*write* site,
    ALL 74 already have `write > 0`, which is what the `cells[key][kind] += 1` in gen_state_matrix's
    fold() guarantees structurally. Found + fixed 2026-08-24 (finding 2026-08-24-1636-1).
    """
    with open(MATRIX, encoding="utf-8") as fh:
        m = json.load(fh)
    shared_regions = {row["region"] for row in m["shared"]}
    writes = {}
    for cell in m["cells"]:
        fn = cell["fn"]
        if fn not in ai_names:
            continue
        c = cell["counts"]
        if not (c.get("write", 0) or c.get("rw", 0)):
            continue
        bucket = "shared" if cell["region"] in shared_regions else "island"
        writes.setdefault(fn, {"shared": [], "island": []})[bucket].append(cell["region"])
    for v in writes.values():
        v["shared"] = sorted(set(v["shared"]))
        v["island"] = sorted(set(v["island"]))
    return writes


def load_all_roster_writers():
    """Every function -- AI or not -- that DIRECTLY writes a roster region.

    Deliberately not filtered to the AI cluster, unlike load_write_sets(). The whole point of the
    `roster_write_via` column is the edges that leave the cluster: `llm_strat_unit_create` can never
    be a batch-D candidate (it is not `llm_strat_ai_*`), and `llm_strat_ai_create_reinforcement_unit`
    cannot either (it does not write the roster itself, it CALLS a creator). Batch D's rule sits at
    the intersection of those two blind spots, which is exactly where AI unit creation lives.
    """
    with open(MATRIX, encoding="utf-8") as fh:
        m = json.load(fh)
    out = {}
    for cell in m["cells"]:
        c = cell["counts"]
        # `movs` is a PROVENANCE count, not a direction -- see load_write_sets. Same fix, same date.
        if not (c.get("write", 0) or c.get("rw", 0)):
            continue
        if cell["region"] in ROSTER_REGIONS:
            out.setdefault(cell["fn"], set()).add(cell["region"])
    return out


def load_gate_sites():
    """The measured AI/sim seam -- see the Ghidra-side AI gate-site dump for why this is not derivable
    from names. Absent file is not fatal: the column just goes empty rather than silently false."""
    path = DOM.path("gate_sites", required=False)
    if not path or not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as fh:
        return {g["name"] for g in json.load(fh)["gate_sites"]}


def _effect_classes_path():
    """The domain's effect-classes file, or None if the domain declares none.

    DECLARED-BUT-MISSING IS AN ERROR, not an empty dict. A domain that names a file it does not have
    would otherwise report every target UNCLASSIFIED -- which is what a domain with no dispositions
    yet legitimately looks like, so a typo in the path, a file deleted, and honest work-not-started
    would all print the same line. The seam is the one place where "nothing classified" must not be
    ambiguous with "nothing found".
    """
    path = DOM.path("effect_classes", required=False)
    if not path:
        return None
    if not os.path.isfile(path):
        raise SystemExit(
            "migration domain %r declares paths.effect_classes but the file is missing:\n"
            "  %s\n"
            "  Create it (even as `{}`) or drop the declaration -- a missing file reports every\n"
            "  target UNCLASSIFIED, indistinguishable from having authored no dispositions yet."
            % (DOM.name, os.path.relpath(path, REPO).replace("\\", "/"))
        )
    return path


def load_effect_classes():
    """Hand dispositions for the outward-call targets. {name -> {class, why, verified}}.

    THE INVENTORY IS DERIVED; THE DISPOSITION IS NOT. Which edges leave the migration set is a
    measurement (compute_outward_effects below). Whether a given target has an external EFFECT is a
    reading of its body, and no mechanical rule gets it right -- `llm_ui_print_queue_text_id` draws
    and `llm_ui_cursor_lookup_offset_pair` does not, and they share a prefix. So the classification
    is authored, exactly like the batch overrides, and the generator's job is to keep the two honest
    about each other: a target with no entry is reported UNCLASSIFIED, an entry naming a target that
    is no longer called is reported STALE.

    `verified` is the field that matters and it defaults to FALSE. A disposition read off a name is
    a hypothesis; the item that builds the seam has to confirm each one against the body, and until
    it does the count of verified-vs-read is the honest measure of how much of the seam is grounded.
    A green seam built on 24 unverified readings would be exactly the vacuous pass this repo keeps
    catching elsewhere.
    """
    path = _effect_classes_path()
    if path is None:
        return {}
    with open(path, encoding="utf-8") as fh:
        raw = json.load(fh)
    out = {}
    for name, spec in raw.items():
        if name.startswith("_"):  # comment keys
            continue
        if not isinstance(spec, dict) or spec.get("class") not in EFFECT_CLASSES:
            raise SystemExit(
                "effect class for %s must carry `class` in %s"
                % (name, "/".join(sorted(EFFECT_CLASSES)))
            )
        if not spec.get("why"):
            raise SystemExit("effect class for %s must carry a `why`" % name)
        why = spec["why"]
        out[name] = {
            "class": spec["class"],
            "why": "\n".join(why) if isinstance(why, list) else why,
            "verified": bool(spec.get("verified", False)),
            # `gate: false` -- classified but DELIBERATELY not gated (gen_dll_effects.py used to emit
            # the record and no gate; that generator went at fork F2F, so today NOTHING is gated
            # and the flag records the classification decision). It has to survive this load or the
            # soundness walk would stop at a target with no gate, and the report would count a
            # written-down hole as coverage.
            "gate": spec.get("gate", True),
            "gate_why": spec.get("gate_why"),
        }
    return out


# effectful -- has an external side effect a state restore cannot undo, so it DOUBLE-FIRES under
#              shadow and must be seamed.
# pure      -- a query. Safe in both arms, and must NOT be suppressed: a seam built only from the
#              effectful cases passes its own test vacuously, so these are its negative arm.
# state     -- mutates state we can DECLARE as a region. No seam; declare it and let the comparison
#              cover it.
EFFECT_CLASSES = ("effectful", "pure", "state")


def load_benign_passthrough():
    """Names inside an effect family whose BODY has been read and found not irreversible.

    Read from the domain's effect_classes file (`_benign_passthrough`). Deliberately separate from
    the dispositions: a disposition says what the GATE does at a target, this says what the SOUNDNESS
    WALK may descend through. Conflating them would let an exception to the check masquerade as a
    classification.
    """
    path = _effect_classes_path()
    if path is None:
        return {}
    with open(path, encoding="utf-8") as fh:
        raw = json.load(fh).get("_benign_passthrough") or {}
    out = {}
    for name, spec in raw.items():
        if name.startswith("_"):
            continue
        if not isinstance(spec, dict) or not spec.get("why") or not spec.get("evidence"):
            raise SystemExit(
                "_benign_passthrough entry %s must carry `why` AND `evidence` -- it is an exception "
                "to a safety check, so 'it looked fine' is not enough" % name
            )
        out[name] = spec
    return out


def is_effect_family(nm, prefixes, walls):
    """Is `nm` in the outward-effect surface -- by family prefix, by being a NAMED wall, or by being
    a NAMED shared callee (`select.effect_names`)?

    All three, because they express one architectural line over three different kinds of code: a
    closure-mode domain draws it with prefixes; `tact` draws it one wall at a time because no prefix
    predicate survives its closure; and BOTH of those only ever match code inside the closure, so
    neither can see a call out to shared, non-exclusive territory. A soundness walk missing any of
    the three descends straight through it and reports nothing -- which is what happened to
    llm_ui_set_draw_surface, a blit reached from nine tactical members and named by none of the
    predicates until `effect_names` existed.
    """
    return (
        (bool(prefixes) and nm.startswith(prefixes))
        or nm in walls
        or nm in (DOM.effect_names if DOM else ())
    )


def check_gating_soundness(
    targets,
    callees,
    symbols,
    name_by_id,
    prefixes,
    benign,
    walls=frozenset(),
    not_gated=frozenset(),
):
    """Is a `state`/`pure` disposition SOUND? Walk forward and find out.

    THE SEAM GATES THE TARGET, NOT THE CALL SITE, which is what makes it work for an effect reached
    four levels down through code we do not own. The price is that a `state` classification carries
    a hidden premise: *every irreversible thing this target can reach is itself gated*. Nothing about
    the class name shows that, and it is exactly where two independent body reviews of the
    llm_progress_* family split from the classification -- both were right about the reach and both
    recommended seaming the shared leaves, which is what the target-placed gate already does. A
    reader cannot check that by eye, so it is checked here.

    For each non-`effectful` target: BFS forward over the call graph, STOPPING at any gated target
    (all 25 are gated, whatever their class -- an effectful one suppresses, a pure/state one is a
    checked pass-through). Anything reached that (a) lives in an effect family by name, (b) is not
    gated, and (c) is not itself classified `pure` is an UNGATED REACH: an irreversible action the
    seam would let fire twice.

    Reports rather than raises. A finding here means either the gate list is short one target or the
    disposition is wrong -- both are judgement calls, and silently failing a generator that every
    other tool depends on is the wrong way to ask for one.
    """
    # A target carrying `gate: false` is CLASSIFIED but deliberately ungated (w_sprintf's varargs
    # shape, GetResourseFilePtr's unguarded return -- see gen_dll_effects.py, deleted at fork
    # F2F). The walk must NOT stop at one: stopping means "the gate is here, everything below is
    # covered", and there is no gate. Excluding them is what turns a reach into one into a reported
    # finding a human must ack.
    gated = {r["name"] for r in targets} - set(not_gated)
    pure = {r["name"] for r in targets if r["class"] == "pure"}
    by_name = {}
    for fid in set(callees) | {b for v in callees.values() for b in v}:
        by_name.setdefault(
            symbols[fid]["name"] if fid in symbols else name_by_id.get(fid, fid), fid
        )

    findings = []
    benign_used = set()
    for row in targets:
        if row["class"] == "effectful":
            continue
        start = by_name.get(row["name"])
        if start is None:
            continue
        seen, frontier, hits = {start}, [start], []
        while frontier:
            cur = frontier.pop()
            for nxt in callees.get(cur, ()):
                if nxt in seen:
                    continue
                seen.add(nxt)
                nm = symbols[nxt]["name"] if nxt in symbols else name_by_id.get(nxt, nxt)
                if nm in gated:
                    continue  # the gate is here; everything below it is already covered
                if is_effect_family(nm, prefixes, walls) and nm not in pure and nm not in benign:
                    hits.append(nm)
                    continue  # report the boundary, do not enumerate everything under it
                if nm in benign:
                    benign_used.add(nm)
                frontier.append(nxt)
        if hits:
            findings.append(
                {"target": row["name"], "class": row["class"], "reaches": sorted(set(hits))}
            )
    # A benign entry the walk never needed is STALE: either the call went away or the walk
    # stopped reaching it, and in both cases the recorded body evidence is describing something
    # nobody is relying on any more. Same rot check as the readiness ack file.
    return {"findings": findings, "benign_stale": sorted(set(benign) - benign_used)}


def load_blinded_ack():
    """Regions an `effectful` target may write despite the oracle comparing them, with a reason.

    Read from the domain's effect_classes file (`_blinded_ack`), keyed by target name. Same
    discipline as `_benign_passthrough`: an exception to a safety check has to carry a `why` AND
    the `evidence` that settled it, because "we looked and it is fine" read back six months later
    is indistinguishable from nobody having looked.
    """
    path = _effect_classes_path()
    if path is None:
        return {}
    with open(path, encoding="utf-8") as fh:
        raw = json.load(fh).get("_blinded_ack") or {}
    out = {}
    for name, spec in raw.items():
        if name.startswith("_"):
            continue
        if not isinstance(spec, dict) or not spec.get("why") or not spec.get("evidence"):
            raise SystemExit(
                "_blinded_ack entry %s must carry `why` AND `evidence` -- it acknowledges that a "
                "suppression removes a write the oracle compares, which is not a thing to wave "
                "through" % name
            )
        out[name] = spec
    return out


def compute_blinded_writes(rows):
    """For every `effectful` target: which regions it writes that the oracle COMPARES or RESTORES.

    THE FAILURE THIS CATCHES. Shadow mode suppresses an `effectful` target in our arm. If that
    target also writes a region carrying `hash` or `shadow`, our arm ends the window without a write
    the original made, the comparison reports a divergence, and the divergence is manufactured by
    the seam rather than by our code. A reviewer cannot see this from the class name, and neither
    could the two independent reviews that argued about it -- it is a join between the write matrix
    and the region manifests, which is a query, not a reading.

    Returns None when the measured matrix is absent (tmp/ is gitignored), because "not checked" and
    "checked, found nothing" must not print the same line -- the R3 lesson, again.
    """
    if not os.path.isfile(MATRIX):
        return None
    with open(MATRIX, encoding="utf-8") as fh:
        cells = json.load(fh)["cells"]
    with open(STATE_REGIONS, encoding="utf-8") as fh:
        manifests = {r["name"]: set(r.get("manifests") or []) for r in json.load(fh)["regions"]}
    effectful = {r["name"] for r in rows if r["class"] == "effectful"}
    ack = load_blinded_ack()
    writes = {}
    for c in cells:
        if c["fn"] not in effectful:
            continue
        cnt = c["counts"]
        if not (cnt.get("write", 0) or cnt.get("rw", 0)):
            continue
        # `hash` = the comparison reads it; `shadow` = the oracle snapshots and restores it. Either
        # one means a suppressed write is a write the window expected to see.
        if manifests.get(c["region"], set()) & {"hash", "measured"}:
            writes.setdefault(c["fn"], set()).add(c["region"])
    return {
        "checked": True,
        "findings": [
            {"target": n, "regions": sorted(rs), "acked": n in ack}
            for n, rs in sorted(writes.items())
        ],
        "acked": sorted(ack),
    }


def compute_outward_effects(members, callees, symbols, name_by_id, sizes):
    """Every edge from the migration set into the declared effect families.

    WHY THIS IS IN THE GENERATOR AND NOT A ONE-OFF SCAN. Shadow mode snapshots, runs the original,
    RESTORES the pre-state, runs ours, compares -- and restoring state does not un-send a packet or
    un-draw a dialog. So a migrated body that calls out to sound/UI/net fires that effect TWICE per
    armed call, while the state comparison still reads clean. That is not a per-function detail; it
    is a property of the whole subsystem that has to be known before anything is armed, and it was
    measured by throwaway script twice before it earned a place here.

    Returns None when the domain declares no effect families -- which is NOT the same as "no
    effects", and the caller must keep those two apart (the R3 lesson in the readiness report: a
    missing input and a clean result must never look alike).
    """
    prefixes = DOM.effect_prefixes
    # THE NAMED WALLS ARE EFFECT TARGETS TOO, and for a domain whose walls are a name list they are
    # the ONLY ones. `tact` walls 56 functions individually because no prefix predicate survives its
    # closure (a `ui_` wall would take the sole writer of the mission's termination condition), so a
    # prefix-only effect scan would report zero outward effects for a subsystem that draws every
    # frame and plays sounds -- the exact "a missing input and a clean result must never look alike"
    # failure this function's docstring warns about, one level up.
    wall_names = frozenset(load_cut_names())
    if not prefixes and not wall_names and not DOM.effect_names:
        return None
    classes = load_effect_classes()

    edges = []
    for a in sorted(members):
        for b in sorted(callees.get(a, ())):
            if b in members:
                continue
            nm = symbols[b]["name"] if b in symbols else name_by_id.get(b, b)
            if is_effect_family(nm, prefixes, wall_names):
                edges.append((a, b, nm))

    by_target = {}
    for a, b, nm in edges:
        row = by_target.setdefault(
            b,
            {
                "addr": "0x" + b,
                "name": nm,
                "size_bytes": sizes.get(b, 0),
                "call_sites": 0,
                "callers": [],
                "class": None,
                "class_why": None,
                "class_verified": False,
            },
        )
        row["call_sites"] += 1
        caller = symbols[a]["name"] if a in symbols else name_by_id.get(a, a)
        if caller not in row["callers"]:
            row["callers"].append(caller)
    for row in by_target.values():
        row["callers"].sort()
        c = classes.get(row["name"])
        if c:
            row["class"] = c["class"]
            row["class_why"] = c["why"]
            row["class_verified"] = c["verified"]

    targets = sorted(by_target.values(), key=lambda r: (-r["call_sites"], r["name"]))
    for row in targets:
        c = classes.get(row["name"]) or {}
        if c.get("gate") is False:
            row["gated"] = False
            row["gate_why"] = c.get("gate_why")
    not_gated = [r["name"] for r in targets if r.get("gated") is False]
    unclassified = [r["name"] for r in targets if r["class"] is None]
    stale = sorted(set(classes) - {r["name"] for r in targets})
    ungated = check_gating_soundness(
        targets,
        callees,
        symbols,
        name_by_id,
        prefixes,
        load_benign_passthrough(),
        wall_names,
        not_gated,
    )
    blinded = compute_blinded_writes(targets)
    return {
        "_note": (
            "Outward calls from the migration set into the domain's effect families. An `effectful` "
            "target DOUBLE-FIRES under shadow mode, because restoring the pre-state between arms "
            "does not un-send a packet or un-draw a dialog -- and the state comparison still reads "
            "clean, so the run looks green. `pure` targets must NOT be suppressed: they are the "
            "negative arm that stops a seam from passing its own test vacuously. Dispositions are "
            "AUTHORED (see the domain's effect_classes file) and start unverified -- `verified` "
            "means someone read the body, not the name."
        ),
        "effect_prefixes": list(prefixes or ()),
        **({"effect_walls_named": len(wall_names)} if wall_names else {}),
        "edges": len(edges),
        "callers": len({a for a, _, _ in edges}),
        "targets": len(targets),
        "by_class": {c: sum(1 for r in targets if r["class"] == c) for c in EFFECT_CLASSES},
        "unclassified": unclassified,
        "verified": sum(1 for r in targets if r["class_verified"]),
        "stale_class_entries": stale,
        "not_gated": not_gated,
        "ungated_reach": ungated,
        "blinded_writes": blinded,
        "target_rows": targets,
    }


def load_batch_overrides():
    """Hand assignments for functions no mechanical rule can classify. {addr -> {batch, why}}.

    `propose_batch` emits "unassigned" rather than guessing, which is right -- but there was no way
    to RECORD the hand decision it asks for, so the assignment would be re-derived away on the next
    regeneration and the function would sit unassigned forever. That is how the AI ROOT itself
    (`llm_strat_ai_players_tick`) went unscheduled from the day the ledger was built: its
    `batch_why` had been asking for a hand assignment since 2026-07-26 and there was nowhere to put
    one. Each entry MUST carry a `why`; the ledger stays derived, and this file is the one authored
    input it derives FROM.
    """
    if not os.path.exists(BATCH_OVERRIDES):
        return {}
    with open(BATCH_OVERRIDES, encoding="utf-8") as fh:
        raw = json.load(fh)
    out = {}
    for addr, spec in raw.items():
        if addr.startswith("_"):  # comment keys
            continue
        if not isinstance(spec, dict) or not spec.get("batch") or not spec.get("why"):
            raise SystemExit("batch override %s must carry both `batch` and `why`" % addr)
        why = spec["why"]
        # A multi-line rationale is authored as a list so the JSON stays readable; the ledger wants
        # one string.
        spec = dict(spec, why="\n".join(why) if isinstance(why, list) else why)
        out[addr.lower()] = spec
    return out


def propose_batch(name, reachable, shared_written):
    """Mechanical batch proposal. Returns (batch, why).

    THE ROSTER RULE IS PER-DOMAIN AND MUST BE, which is the one place these two clusters disagree
    about what a roster write MEANS. For `ai` a roster write is the R2 architecture violation that
    routes a function to the refactor batch. For `sim` the rosters are the subsystem's own state and
    writing them is the job -- 149 of its 311 functions do -- so `roster_batch` is null there and
    the rule is skipped. A shared rule would have put half the sim in a refactor batch.
    """
    rb = DOM.batches.get("roster_batch")
    if rb:
        roster = sorted(ROSTER_REGIONS.intersection(shared_written))
        if roster:
            return rb["batch"], rb["why"] % ", ".join(roster)
    ub = DOM.batches.get("unreachable_batch")
    if ub and not reachable:
        return ub["batch"], ub["why"]
    tail = name[len(PREFIX) :] if (PREFIX and name.startswith(PREFIX)) else name
    for batch, keys in BATCH_RULES:
        if any(k in tail for k in keys):
            return batch, "sub-domain match on %r" % next(k for k in keys if k in tail)
    return "unassigned", "no mechanical rule matched -- assign by hand before scheduling"


WALL_CLASSES = ("presentation", "platform", "shared_helper")


def load_cut_names():
    """PER-FUNCTION walls, authored with evidence, from `select.cut_names_from`.

    WHY A NAME LIST EXISTS AT ALL, when `cut_prefixes` already walls families. A prefix wall is a
    NAME predicate, and the tactical closure is the domain where that predicate demonstrably breaks
    in both directions, measured rather than feared:

      * `llm_gfx_*` inside this closure is TACTICAL-EXCLUSIVE -- `llm_gfx_blit_sprite_rle` and its
        companions are reached from nothing but `llm_tact_render_view` / `llm_tact_unit_render`.
        Walling the family by prefix is still right (they are pixels), but the family prefix is not
        what makes it right, and the same prefix elsewhere in the binary is a different population.
      * `llm_tact_ui_*` contains `llm_tact_active_unit_count_hud_draw`, the SOLE writer of
        `_G_LLM_TACT_ACTIVE_UNIT_COUNT` -- the mission's termination condition. A `*_draw`/`ui_`
        prefix wall deletes the excursion's exit trigger and nothing downstream ever says so.

    So the wall is authored one function at a time, each entry carrying a `class`, a `why` and the
    `evidence` that settled it. Two refusals, both mechanical:

      STALE    an entry naming a function the closure does not contain -- the wall is describing a
               set that moved, and a wall over a name nobody has is invisible.
      unclassified  reported, never fatal: a member with no entry is simply a MEMBER. The default
               direction is deliberate. A wrongly-kept member costs translation work and is visible
               in the ledger; a wrongly-placed wall silently removes a subtree and is not.
    """
    rel = SELECT.get("cut_names_from")
    if not rel:
        return {}
    path = os.path.join(REPO, *rel.split("/"))
    if not os.path.isfile(path):
        raise SystemExit("select.cut_names_from names a file that does not exist: %s" % rel)
    with open(path, encoding="utf-8") as fh:
        raw = json.load(fh).get("walls") or {}
    out = {}
    for name, spec in raw.items():
        if name.startswith("_"):
            continue
        if not isinstance(spec, dict) or spec.get("class") not in WALL_CLASSES:
            raise SystemExit(
                "wall entry %s must carry `class` in %s" % (name, "/".join(sorted(WALL_CLASSES)))
            )
        if not spec.get("why") or not spec.get("evidence"):
            raise SystemExit(
                "wall entry %s must carry `why` AND `evidence` -- a wall removes a function from "
                "the domain silently, so 'it looks like a renderer' is not enough" % name
            )
        out[name] = spec
    return out


def _named_wall_report(named_walls, stale_walls):
    """The per-function-wall report fields, or nothing when the domain declares no name list."""
    if not SELECT.get("cut_names_from"):
        return {}
    return {"cut_walls_by_name": len(named_walls), "cut_walls_stale": stale_walls}


def _apply_cut_names(seen, name_of):
    """(walled_addrs, stale_names) for the authored per-function wall list over a selected set."""
    walls_by_name = load_cut_names()
    if not walls_by_name:
        return set(), []
    seen_names = {name_of.get(a, a) for a in seen}
    stale = sorted(n for n in walls_by_name if n not in seen_names)
    hit = {a for a in seen if name_of.get(a, a) in walls_by_name}
    return hit, stale


def load_dispatch_seeds():
    """Fn-ptr-dispatch seed function names for the closure walk (SIM1-DISPATCH). Read from the file
    the profile names in select.dispatch_seeds_from -- the SAME disposition file
    the dispatch-closure check uses, so the closure and the check share one seed list. Empty
    when the domain declares none (the ai domain does not)."""
    rel = SELECT.get("dispatch_seeds_from")
    if not rel:
        return []
    path = os.path.join(REPO, rel)
    if not os.path.isfile(path):
        print("warning: dispatch_seeds_from not found: %s" % path)
        return []
    with open(path, encoding="utf-8") as fh:
        return json.load(fh).get("handler_seeds", [])


def load_bldg_type_seeds():
    """The SECOND fn-ptr registry's callback names (SIM1-BLDGCB), from
    select.bldg_type_seeds_from. Kept separate from load_dispatch_seeds() rather than merged into
    one list for two reasons that both matter: the batch router has to know which seed set reached a
    function (their NAMES would all match batch G4's rules, and G4 is closed), and
    the dispatch-closure check's DISPATCH attribution must keep meaning "the state-handler registry"
    so its fold count stays comparable across sessions. Empty when the domain declares none."""
    rel = SELECT.get("bldg_type_seeds_from")
    if not rel:
        return []
    path = os.path.join(REPO, rel)
    if not os.path.isfile(path):
        print("warning: bldg_type_seeds_from not found: %s" % path)
        return []
    with open(path, encoding="utf-8") as fh:
        return [c["name"] for c in json.load(fh).get("callbacks", ())]


def load_member_seeds():
    """Names added to the cohort as MEMBERS, never as roots (SIM1-H), from select.member_seeds_from.

    THE DIFFERENCE FROM THE OTHER TWO SEED SETS IS THE WHOLE POINT. `dispatch_seeds_from` and
    `bldg_type_seeds_from` name functions the forward walk cannot REACH but which belong under the
    root, so they are added as additional ROOTS and their callees come with them. SIM1-H is the
    opposite shape: these are the host-side CALLERS of the closure, sitting one level ABOVE the
    root, surfaced by SIM-HOSTREACH's fixpoint because their original bodies still walk into bodies
    we own. Seeding them as roots would pull their whole forward closure -- session entry, the save
    container, the map loader -- into a domain that has already closed twelve batches. So they are
    injected as members and nothing else: no walk, no descent, no widening.

    The file is the worklist itself (tools/data/migration/sim1h_seeds.json), so the batch and the
    measurement that produced it cannot disagree about which 8 functions this is.
    """
    rel = SELECT.get("member_seeds_from")
    if not rel:
        return []
    # ONE FILE OR A LIST OF THEM. A batch that grows in waves gets a second file rather than an
    # edited first one: sim1h_seeds.json is SIM-HOSTREACH's MEASUREMENT artifact and appending to it
    # would make the measurement disagree with itself.
    out = []
    for r in [rel] if isinstance(rel, str) else list(rel):
        path = os.path.join(REPO, r)
        if not os.path.isfile(path):
            print("warning: member_seeds_from not found: %s" % path)
            continue
        with open(path, encoding="utf-8") as fh:
            out += [s["name"] for s in json.load(fh).get("seeds", ())]
    return out


def _owned_elsewhere_names():
    """`select.owned_elsewhere` as a NAME set, accepting both of its two written forms.

    A BARE STRING is the original form: a function owned by a subsystem that has no migration
    manifest at all, so there is nobody to name (RI-ORDERS reimplemented sim's two order-queue
    helpers before the manifest era). An OBJECT `{name, owner, why}` is a RE-HOMED row -- it moved to
    another domain that does have a ledger, and naming the owner is what lets
    the state-interface item lint check 4 refuse a one-sided move.

    Both mean the same thing HERE (subtract this name from the cohort); they differ only in what can
    be checked about them, which is the lint's business rather than the generator's.
    """
    return {e["name"] if isinstance(e, dict) else e for e in SELECT.get("owned_elsewhere", ())}


def select_members(symbols, callees, name_by_id):
    """The migration set for this domain. Returns (member_addrs, excluded_report).

    Two modes, and the difference is not cosmetic -- see the module docstring. `name_prefix` trusts
    a naming convention; `closure` trusts the call graph and a declared list of walls. The closure
    mode reports what it removed and why, because a selector that silently drops 300 functions is
    indistinguishable from one that is broken.
    """
    mode = SELECT["mode"]
    if mode == "name_prefix":
        members = {a for a, s in symbols.items() if s["name"].startswith(PREFIX)}
        # RE-HOMED ROWS. A prefix selector has no way to say "this one is not mine": it trusts a
        # naming convention, and a function can carry the convention's prefix while belonging to
        # another domain by what it DOES. `llm_strat_ai_group_move_formation_rotating` is the first
        # -- it stamps the order-notify bits llm_strat_unit_notify_status owns and calls a move-order
        # enqueue, i.e. it is an order-issue wrapper wearing the AI prefix.
        #
        # This is the SUBTRACTING half of a two-sided move; the adopting half is a `names` seed in
        # the owner's profile. Neither half alone is safe, so the state-interface item lint check 4 fails
        # unless the `owner` named here actually carries the row -- otherwise a re-home would delete
        # a function from every ledger, which is the "quiet shrink reads as progress" failure the
        # seed language was built to refuse.
        rehomed = {a for a, s in symbols.items() if s["name"] in _owned_elsewhere_names()}
        return members - rehomed, {}, set(), set(), set()

    if mode == "seeded":
        return select_seeded(symbols, callees, name_by_id)

    # closure mode
    cuts = tuple(SELECT.get("cut_prefixes", ()))
    frontier = tuple(SELECT.get("frontier_prefixes", ()))
    name_of = {a: symbols[a]["name"] if a in symbols else name_by_id.get(a, a) for a in name_by_id}

    def starts(addr, prefixes):
        return name_of.get(addr, "").startswith(prefixes) if prefixes else False

    # SIM1-DISPATCH: seed the walk with the fn-ptr-dispatched handlers. A forward DIRECT-CALL closure
    # cannot reach a table-dispatched target, so without this the per-tick unit/
    # building behaviour is silently absent. The seeds are additional ROOTS; the wall check below still
    # stops descent, so a seed that itself matches a cut is simply not added. ONE source with the check
    # tool (dispatch_seeds_from -> the disposition file's handler_seeds), so they cannot disagree.
    addr_by_name = {}
    for a, nm in name_of.items():
        addr_by_name.setdefault(nm, a)
    seed_addrs, seed_missing = set(), []
    for nm in load_dispatch_seeds():
        a = addr_by_name.get(nm)
        if a is None:
            seed_missing.append(nm)
        elif not starts(a, cuts):
            seed_addrs.add(a)

    # SIM1-BLDGCB: the SECOND registry's callbacks, seeded the same way and tracked separately -- see
    # load_bldg_type_seeds() for why they are not merged into one list.
    cb_addrs, cb_missing = set(), []
    for nm in load_bldg_type_seeds():
        a = addr_by_name.get(nm)
        if a is None:
            cb_missing.append(nm)
        elif not starts(a, cuts):
            cb_addrs.add(a)

    # Walk forward, refusing to DESCEND through a wall. The wall functions themselves are reached
    # (they are legitimate outward calls) but nothing below them is pulled in.
    def walk(roots):
        seen_, stack_ = set(roots), list(roots)
        while stack_:
            for nxt in callees.get(stack_.pop(), ()):
                if nxt not in seen_ and not starts(nxt, cuts):
                    seen_.add(nxt)
                    stack_.append(nxt)
        return seen_

    roots = {ROOT} | seed_addrs | cb_addrs
    seen = walk(roots)
    # The walk WITHOUT the second seed set, so batch attribution is by which seed set reached a
    # function rather than by its name -- and so batches A..G5, all closed and verified, keep exactly
    # the membership they had before this registry was folded in.
    seen_pre_cb = walk({ROOT} | seed_addrs)

    walls = {a for a in seen if starts(a, cuts)}
    named_walls, stale_walls = _apply_cut_names(seen, name_of)
    walls |= named_walls
    helpers = {a for a in seen if starts(a, frontier)}
    owned = set()
    for other in SELECT.get("excludes", ()):
        owned |= _addrs_of_domain(other)
    # Functions owned by a subsystem that has NO migration manifest (e.g. RI-ORDERS reimplemented
    # them in order_queue.cpp before the manifest era). A hand list of names, subtracted like `owned`
    # so they never enter the cohort or its dispatch fold.
    owned_names = _owned_elsewhere_names()
    if owned_names:
        owned |= {a for a, nm in name_of.items() if nm in owned_names}
    # SIM1-H: the host-side callers, injected as MEMBERS (see load_member_seeds). Not walked, not
    # wall-tested, not frontier-tested -- an explicit list is a decision, not a discovery. A name the
    # binary does not carry is a HARD error, the `names` seed kind's rot guard: a quietly-dropped
    # member is the failure that reads as progress.
    member_addrs, member_missing = set(), []
    for nm in load_member_seeds():
        a = addr_by_name.get(nm)
        if a is None:
            member_missing.append(nm)
        else:
            member_addrs.add(a)
    if member_missing:
        raise SystemExit(
            "member_seeds_from names %d function(s) the binary does not have: %s"
            % (len(member_missing), ", ".join(sorted(member_missing)))
        )
    members = (seen - walls - helpers - owned) | member_addrs
    # Net-new members the seeds pulled in over a root-only walk -- the honest measure of the fold,
    # and the set that gets routed to the dispatch batch so the completed root-reachable batches
    # (SIM1A-F) stay untouched.
    root_only = walk({ROOT})
    # Attribution is by SEED SET, in this order: anything the bldg-type seeds newly reached is
    # SIM1-BLDGCB's, the rest of the non-root-reachable set is SIM1-DISPATCH's. Written as a
    # difference against `seen_pre_cb` rather than as a name test precisely because the names would
    # route these into a closed batch.
    bldg_cb_members = (seen - seen_pre_cb) & members
    dispatch_members = (seen_pre_cb - root_only) & members
    return (
        members,
        {
            "closure_raw": len(seen),
            "cut_walls": len(walls),
            # Emitted ONLY by a domain that declares `cut_names_from`. A key that appears in every
            # manifest the moment a feature lands would make every OTHER domain's committed file
            # stale on the next --check, for a number that is structurally zero there.
            **_named_wall_report(named_walls, stale_walls),
            "shared_helpers": len(helpers),
            "dispatch_seeds": len(seed_addrs),
            "dispatch_seeds_missing": seed_missing,
            "dispatch_pulled_in": len((seen_pre_cb - root_only) - walls - helpers - owned),
            "bldg_type_seeds": len(cb_addrs),
            "bldg_type_seeds_missing": cb_missing,
            "bldg_type_pulled_in": len(bldg_cb_members),
            **({"member_seeds": len(member_addrs)} if member_addrs else {}),
            "owned_by_other_domains": {
                d: len(_addrs_of_domain(d) & seen) for d in SELECT.get("excludes", ())
            },
        },
        dispatch_members,
        bldg_cb_members,
        member_addrs,
    )


def select_seeded(symbols, callees, name_by_id):
    """MIG-SET: membership from a QUERY, for a domain that is not a walk from one root.

    Two shapes, chosen by `select.walk`:

      forward  the seeds are roots and the closure is taken from them, with the same walls,
               frontier and cross-domain exclusions closure mode uses. This is O4-ISSUE: the 113
               order-issue wrappers ARE the subsystem, and whatever they call below the walls comes
               with them.
      none     the seeds ARE the membership. This is RD-READY, where nothing is translated, so a
               callee frontier would enlarge a set whose only job is to be named and prototyped.

    The report carries the per-query match counts, because a query is a piece of logic and a
    membership number with no derivation next to it is the thing this whole module exists to avoid.
    """
    from migration_seeds import resolve_seeds

    # own_domain=DOM.name: exclude_ours must subtract what OTHER domains/subsystems already own,
    # never this domain's own progress against itself -- otherwise verifying a row deletes it from
    # its own manifest on the next regen (the tact incident this generalizes).
    seed_names, seed_report = resolve_seeds(SELECT.get("seeds"), own_domain=DOM.name)
    name_of = {a: symbols[a]["name"] if a in symbols else name_by_id.get(a, a) for a in name_by_id}
    addr_by_name = {}
    for a, nm in name_of.items():
        addr_by_name.setdefault(nm, a)

    cuts = tuple(SELECT.get("cut_prefixes", ()))
    frontier = tuple(SELECT.get("frontier_prefixes", ()))

    def starts(addr, prefixes):
        return name_of.get(addr, "").startswith(prefixes) if prefixes else False

    seeds, missing = set(), []
    for nm in sorted(seed_names):
        a = addr_by_name.get(nm)
        if a is None:
            missing.append(nm)
        else:
            seeds.add(a)

    if SELECT.get("walk", "forward") == "none":
        seen = set(seeds)
    else:
        seen, stack = set(seeds), list(seeds)
        while stack:
            for nxt in callees.get(stack.pop(), ()):
                if nxt not in seen and not starts(nxt, cuts):
                    seen.add(nxt)
                    stack.append(nxt)

    walls = {a for a in seen if starts(a, cuts)}
    named_walls, stale_walls = _apply_cut_names(seen, name_of)
    walls |= named_walls
    helpers = {a for a in seen if starts(a, frontier)}
    owned = set()
    for other in SELECT.get("excludes", ()):
        owned |= _addrs_of_domain(other)
    owned |= {a for a, nm in name_of.items() if nm in set(SELECT.get("owned_elsewhere", ()))}
    members = seen - walls - helpers - owned
    if not members:
        # NARROWED 2026-09-01, same reason as migration_seeds._domain_is_finished (read its
        # docstring): a `writer_attribution` domain empties its own query BY FINISHING, and the
        # refusal then blocks regeneration of the manifest that records the work. It still fires for
        # a domain with nothing to show; it stands down when the manifest already holds terminal
        # rows, which merge_progress keeps and flags `left_query`.
        from migration_seeds import _domain_is_finished  # noqa: PLC0415

        if not _domain_is_finished(DOM.name):
            raise SystemExit(
                "seeded selection produced no members: %d seed(s) resolved, %d wall(s), %d frontier, "
                "%d owned elsewhere. An empty domain reads as a finished one."
                % (len(seeds), len(walls), len(helpers), len(owned & seen))
            )
        print(
            "  seeded selection is EMPTY and that is this domain finishing: every member is "
            "disposed, so the query returns none. The manifest's terminal rows are kept."
        )
    return (
        members,
        {
            "seeds": seed_report,
            "seeds_unresolved": missing,
            "closure_raw": len(seen),
            "pulled_in_by_walk": len(seen - seeds),
            "cut_walls": len(walls),
            **_named_wall_report(named_walls, stale_walls),
            "shared_helpers": len(helpers),
            "owned_by_other_domains": {
                d: len(_addrs_of_domain(d) & seen) for d in SELECT.get("excludes", ())
            },
        },
        set(),
        set(),
        set(),
    )


def report_membership_delta(new_names):
    """PRINT what this regeneration gained and lost against the committed manifest.

    Printed, not stored: it is a fact about this run, and a manifest field would be rewritten on
    every regeneration while describing the previous one. `--check` already fails when membership
    moves; this is the line that says WHICH names moved.

    MIG-SET. A query-defined membership changes silently as its underlying data changes, and the
    dangerous direction is loss: a set that quietly shrank looks exactly like progress. `name_prefix`
    has the same hazard through renames and answers it with the `_reclassified_out` archive; this is
    the mechanical half, and it applies to every mode because every mode can drift.
    """
    if not OUT or not os.path.isfile(OUT):
        print("membership: first generation, %d function(s)" % len(new_names))
        return
    with open(OUT, encoding="utf-8") as fh:
        old = {r["name"] for r in json.load(fh)["functions"]}
    gained = sorted(new_names - old)
    lost = sorted(old - new_names)
    if not gained and not lost:
        return
    print(
        "MEMBERSHIP MOVED: %d -> %d (+%d / -%d)"
        % (len(old), len(new_names), len(gained), len(lost))
    )
    # Not truncated. The point of the line is that a reader sees every name that LEFT, and a set
    # that quietly shrank is the failure that looks like progress.
    for n in gained:
        print("   + %s" % n)
    for n in lost:
        print("   - %s   <- LOST: renamed, reclassified, or the query narrowed" % n)


def _addrs_of_domain(other):
    """Addresses already owned by another domain's manifest (so we never double-count them)."""
    from migration_domain import load_domain

    path = load_domain(other).path("manifest")
    if not os.path.isfile(path):
        return set()
    with open(path, encoding="utf-8") as fh:
        doc = json.load(fh)
    return {f["addr"].lower().replace("0x", "") for f in doc.get("functions", [])}


def build():
    symbols = load_symbols()
    name_by_id, callees, callers = load_callgraph()
    roster_writers = load_all_roster_writers()
    gate_sites = load_gate_sites()
    ids_by_name = {}
    for _id, _nm in name_by_id.items():
        ids_by_name.setdefault(_nm, []).append(_id)
    # symbols.md covers only the `llm_` naming convention, so back it with the call graph before
    # selecting -- otherwise a closure-mode domain silently loses every game_*/map_*/FUN_ member.
    n_augmented = augment_symbols(symbols, name_by_id)
    ai_ids, select_report, dispatch_members, bldg_cb_members, member_seed_members = select_members(
        symbols, callees, name_by_id
    )
    select_report["symbols_augmented_from_callgraph"] = n_augmented
    # EXTERNAL: pseudo-nodes and anything else with no symbol row at all are not migratable code.
    ai_ids = {a for a in ai_ids if a in symbols}
    ai_names = {symbols[a]["name"] for a in ai_ids}
    effects = compute_outward_effects(ai_ids, callees, symbols, name_by_id, load_body_sizes())

    # The AI-only induced subgraph: what the layering and the antichain are computed over. Edges out
    # to shared helpers are deliberately excluded -- those functions stay ORIGINAL (Law 4, minimise
    # live seams), so they are never armed and cannot nest with an AI site.
    ai_edges = {a: {b for b in callees.get(a, ()) if b in ai_ids} for a in ai_ids}
    layer, in_cycle = compute_layers(ai_ids, ai_edges)

    # REACHABILITY USES THE FULL GRAPH, NOT THE AI SUBGRAPH. An AI function called only THROUGH a
    # shared helper is still reachable from players_tick; walking AI-only edges reports it as an
    # orphan and files it into batch E. That undercounted reachability 143 vs the AI closure's 155
    # and inflated the zero-caller set to 50 against the doc's ~37 -- both artefacts of the wrong
    # graph, and both the kind of "generated index says nothing touches this" trap this tool
    # exists to catch.
    # MIG-SET: a seeded domain has no root to walk from, so reachability is MEASURED BACKWARDS --
    # a member is reachable unless every upward call chain from it dies out (orphan_closure above).
    #
    # THIS USED TO BE `set(ai_ids)`, i.e. the column was the constant True, on the reasoning that a
    # seeded domain's members are "reachable BY CONSTRUCTION (a seed, or something a seed reaches)".
    # That premise is false for a domain seeded by WRITE ATTRIBUTION. sim_resid's seeds are chosen
    # by which regions a body writes, and a dead function still writes regions in its body -- so it
    # is seeded, and nothing ever asked whether anything calls it. Measured 2026-08-31: NINE of
    # sim_resid's 41 rows are reachable from nothing, and five of them had already been translated,
    # oracled and marked `verified` under a column that read True by definition. A field named for
    # a measurement must not be a constant.
    # THE FIXPOINT RUNS OVER THE WHOLE CALL GRAPH, not over ai_ids. A domain member's callers are
    # usually NOT domain members, so a subset walk sees every out-of-domain caller as live and can
    # never propagate the verdict one hop. That is the exact case this check exists for:
    # llm_strat_bldg_set_footprint_passable is called only by two wrappers outside the domain, both
    # of which are themselves unreachable.
    # NODES COME FROM name_by_id, not from the edge endpoints. `callers`/`callees` are keyed by
    # link targets and sources, so a function with NEITHER callers nor outgoing calls appears in
    # neither -- and that is precisely the shape of a fully isolated orphan. Building the node set
    # from the edges silently exempted the most dead functions in the image
    # (llm_strat_unit_path_encode_directions, measured 2026-08-31).
    orphans = (
        orphan_closure(set(name_by_id) | set(ai_ids), callers, load_addr_taken())
        if ROOT is None
        else set()
    )
    reach = set(ai_ids) - orphans if ROOT is None else reachable_from(ROOT, callees)
    writes = load_write_sets(ai_names)
    overrides = load_batch_overrides()

    # 0 for an EMPTY selection -- a domain that has finished disposes its whole query (see the
    # `_domain_is_finished` stand-down above), and a p90 over no rows is not a number. Nothing reads
    # it in that state: the review-required threshold it feeds has no rows to apply to.
    sizes = sorted(symbols[a]["size_bytes"] for a in ai_ids)
    p90 = sizes[int(0.9 * len(sizes))] if sizes else 0

    fns = []
    for addr in sorted(ai_ids):
        s = symbols[addr]
        name = s["name"]
        w = writes.get(name, {"shared": [], "island": []})
        batch, why = propose_batch(name, addr in reach, w["shared"])
        # SIM1-DISPATCH: a fn-ptr-dispatched member is NOT reachable-from-root, so propose_batch would
        # file it under the unreachable_batch (the completed batch F). Route it to its own batch so the
        # closed root-reachable batches stay untouched and this cohort is attributable to SIM1-DISPATCH.
        # SIM1-BLDGCB: checked FIRST -- these names all match batch G4's rules, and G4 is closed.
        # SIM1-H: a member-seeded row is not reachable from the root by construction (it is a CALLER
        # of the closure), so propose_batch would file it under the unreachable_batch -- the closed
        # batch F. Route it mechanically to its own batch instead of paying eight hand overrides for
        # a rule the profile already states.
        if addr in member_seed_members:
            mb = DOM.batches.get("member_batch")
            if mb:
                batch, why = mb["batch"], mb["why"]
        elif addr in bldg_cb_members:
            cb = DOM.batches.get("bldg_type_batch")
            if cb:
                batch, why = cb["batch"], cb["why"]
        elif addr in dispatch_members:
            db = DOM.batches.get("dispatch_batch")
            matched = None
            for rule in DOM.batches.get("dispatch_rules", ()):
                if any(k in name for k in rule["match"]):
                    matched = rule
                    break
            if matched:
                batch, why = (
                    matched["batch"],
                    "dispatch sub-domain: " + matched.get("why", matched["batch"]),
                )
            elif db:
                batch, why = db["batch"], db["why"]
        ov = overrides.get("0x" + addr.lower()) or overrides.get(addr.lower())
        if ov:
            batch, why = ov["batch"], "HAND: " + ov["why"]
        n_callers_all = len(callers.get(addr, ()))
        n_callers_ai = len([c for c in callers.get(addr, ()) if c in ai_ids])
        # DIRECT callees that write a roster region. Distance 1 -- see the column's own note.
        via = {
            name_by_id.get(c, c)
            for c in callees.get(addr, ())
            if name_by_id.get(c, c) in roster_writers
        } - {name}
        # REVIEW ROUTING. The fan-out is triaged, not uniform (reviewers found little on small
        # mechanical bodies). Keyed on ROSTER writes, not on any shared write: player_data alone
        # accounts for 63 of these functions and is the AI's own store, so including it would mark
        # 89 of 218 for review and make the triage meaningless. This is the mechanical half of the
        # trigger; a non-empty `uncertainties[]` from the translator is the other half and cannot be
        # predicted here.
        review = (
            bool(ROSTER_REGIONS.intersection(w["shared"])) or s["size_bytes"] > p90 or addr == ROOT
        )
        fns.append(
            {
                "addr": "0x" + addr,
                "name": name,
                "size_bytes": s["size_bytes"],
                "signature": s["signature"],
                "tags": s["tags"],
                # A row from symbols.md is judged by its signature string; an augmented row has no
                # signature and is judged by the committed calling contract (see augment_symbols).
                "proto_committed": (
                    s["proto_ok"]
                    if "proto_ok" in s
                    else not s["signature"].startswith("undefined ")
                ),
                "reachable_from_root": addr in reach,
                "layer": layer.get(addr, 0),
                "in_cycle": addr in in_cycle,
                "callers_in_ai": n_callers_ai,
                "callers_total": n_callers_all,
                # Counted over the FULL graph. These are NOT to be called dead --
                # settle with the fn-pointer reference materializer + the whole-image raw-pointer scan before scheduling any.
                "zero_callers": n_callers_all == 0 and addr != ROOT,
                "writes_shared": w["shared"],
                "writes_island": w["island"],
                # The roster writes this function performs THROUGH a direct callee. batch/`writes_shared`
                # see only DIRECT writes, so an AI function that mutates the roster via a helper is
                # invisible to rule D -- which is where AI unit creation was hiding. Distance 1 on
                # purpose: distance-N degenerates (the phase drivers reach ~84 roster writers through a
                # 778-function closure, which carries no signal). AI2 scopes on `batch == "D" OR
                # roster_write_via`, not batch D alone.
                "roster_write_via": sorted(via),
                "creates_units": sorted(via & CREATION_PRIMITIVES),
                # Does this function TEST player_data[].ai_enabled? Measured, not inferred from the
                # name (the Ghidra-side AI gate-site dump). These 14 are the AI/sim seam: below one, code
                # runs only for AI players; at one, it runs for everyone. The input AI-SPLIT needs.
                "ai_gate_site": (None if gate_sites is None else name in gate_sites),
                "batch": batch,
                "batch_why": why,
                "review_required": review,
                "preserve_bug": PRESERVE_BUGS.get(name),
                # A readiness domain records no state (MIG-SET decision B): emitting the progress
                # fields would create an inert `state` column that reports `0/N verified` forever
                # for work whose doneness is derived, which is the vacuity this flag exists to avoid.
                **(DEFAULT_PROGRESS if DOM.records_state else {}),
            }
        )

    by_batch = {}
    for f in fns:
        by_batch[f["batch"]] = by_batch.get(f["batch"], 0) + 1
    report_membership_delta({f["name"] for f in fns})
    return {
        "_generated_by": "tools/gen_migration.py --domain %s" % DOM.name,
        "_domain": DOM.name,
        "_profile": "tools/data/migration/%s.json" % DOM.name,
        "_do_not_hand_edit": (
            "Derived fields are regenerated. Only %s are preserved across a regen -- record "
            "progress there and nowhere else." % ", ".join(PROGRESS_FIELDS)
        ),
        "_inputs": [
            "docs/symbols.md",
            "tools/data/en_functions.json",
            "tools/data/call_graph_no_crt.json",
            "tmp/state_matrix.json",
        ],
        "_reclassified_out": RECLASSIFIED_OUT,
        "_select_report": select_report,
        # Omitted entirely, not emitted empty, when the domain declares no effect families -- see
        # compute_outward_effects. "not measured" and "measured, none found" are different claims.
        **({"_outward_effects": effects} if effects is not None else {}),
        "_method_note": method_note(select_report),
        **(
            {"root": "0x" + ROOT}
            if ROOT
            else {
                "root": None,
                "root_note": (
                    "seeded domain -- membership is select.seeds, not a walk from one entry (MIG-SET)"
                ),
            }
        ),
        # Consumed by merge_progress() to refresh KEPT rows' reachability; stripped before write.
        # The ORPHAN set, not the reachable set: `reach` covers only the live query, so a kept row
        # would never appear in it and every one of them would read unreachable. Orphanhood is
        # measured over the whole call graph, so it answers for any function. None for a closure
        # domain, where the question is asked from a root instead and kept rows are left alone.
        "_orphan_ids": (sorted(orphans) if ROOT is None else None),
        "summary": {
            "functions": len(fns),
            "reachable_from_root": sum(1 for f in fns if f["reachable_from_root"]),
            "zero_callers": sum(1 for f in fns if f["zero_callers"]),
            "proto_uncommitted": sum(1 for f in fns if not f["proto_committed"]),
            "writes_shared": sum(1 for f in fns if f["writes_shared"]),
            "review_required": sum(1 for f in fns if f["review_required"]),
            "max_layer": max((f["layer"] for f in fns), default=0),
            "in_cycle": sum(1 for f in fns if f["in_cycle"]),
            "size_bytes_total": sum(f["size_bytes"] for f in fns),
            "size_p90": p90,
            "by_batch": dict(sorted(by_batch.items())),
        },
        "functions": fns,
    }


def method_note(select_report):
    """The membership caveat, which differs per selector mode because the failure modes differ."""
    if SELECT["mode"] == "closure":
        return (
            "MEMBERSHIP IS BY CLOSURE MINUS CUTS, not by name. A function is in this ledger because "
            "the root reaches it without passing through a declared wall, and it is neither a shared "
            "helper nor already owned by another domain. Measured for this run: %d reachable raw, "
            "%d cut walls (presentation/sound/net -- reached but never descended through), %d shared "
            "helpers held as frontier, %s owned elsewhere. THE FAILURE MODE IS THE INVERSE OF THE "
            "NAME-PREFIX ONE: nothing drops out when a function is renamed, but a WRONGLY PLACED "
            "WALL silently removes an entire subtree, and a subtree that never appears is invisible. "
            "Re-read select.cut_prefixes in the profile before concluding that something is absent, "
            "and prefer widening a wall to adding a hand override. Layering is computed on "
            "member-only edges, because only members are ever armed and only they can nest."
            % (
                select_report.get("closure_raw", 0),
                select_report.get("cut_walls", 0),
                select_report.get("shared_helpers", 0),
                select_report.get("owned_by_other_domains", {}),
            )
        )
    return (
        "MEMBERSHIP IS BY NAME PREFIX (%r), not by evidence. A function is in this ledger "
        "because someone once called it AI, and drops out the moment it is renamed -- so a "
        "misnamed function is silently in, and correcting the name silently removes it along "
        "with any progress recorded against it. See _reclassified_out for rows removed that "
        "way, and re-read a row's `callers_in_ai` before trusting its membership: a row with "
        "callers_in_ai 0 AND reachable_from_root false is only in this file on the strength of "
        "its name. "
        "Reachability here is UNCUT: a plain BFS from the root over the whole call graph. "
        "The AI closure cut the BFS at ui/view edges (to keep the closure from swallowing "
        "render/audio at 779 functions), so it reports 155 ai-prefixed functions reachable and "
        "63 not. This file reports more reachable (164) and fewer orphans because a path THROUGH "
        "a ui function still makes an AI function reachable -- the right test for 'does this need "
        "a caller-side hook', which is what batch E turns on. The two are not in conflict; they "
        "answer different questions. Layering is computed on AI-only edges, because only AI "
        "functions are ever armed and only they can nest with each other." % PREFIX
    )


def merge_progress(new, path):
    """Carry recorded progress forward. Regeneration must never reset the loop's own state.

    AND A ROW THAT LEFT THE QUERY IS KEPT, not dropped (2026-08-31). A `writer_attribution` domain
    is seeded from the accessor census's ORIGINAL writers, so the moment a function is translated
    and verified it stops being an original writer and the query stops returning it. Regeneration
    then deleted the row -- taking with it the `state: verified` and `evidence_tier` that are the
    only record the work happened, and that the batch items' own `done_when` clauses read back out
    of this file. Measured on sim_resid the day after its first two rows verified: the very next
    `gen_region_accessors --refresh` made both disappear, and the manifest reported 37 rows with no
    trace that 39 had ever been in scope.

    So: a row whose recorded state is anything but `not_started` SURVIVES, flagged `left_query`
    with the reason. Same rule the tracker already applies to done items -- they are kept, not
    deleted, because the history and the dependency graph both need them. A `not_started` row that
    leaves is still dropped, because that is a genuine scope change with nothing to preserve, and
    `report_membership_delta` prints it either way.
    """
    if not DOM.records_state or not os.path.exists(path):
        return new
    with open(path, encoding="utf-8") as fh:
        old = json.load(fh)
    prev = {f["name"]: f for f in old.get("functions", [])}
    for f in new["functions"]:
        o = prev.get(f["name"])
        if o:
            for k in PROGRESS_FIELDS:
                if k in o:
                    f[k] = o[k]
            o.pop("left_query", None)
    # A KEPT ROW'S DERIVED COLUMNS FREEZE, and one of them is load-bearing. `o` is carried over
    # whole, so every generated field keeps the value it had when the row last appeared in the
    # query. That is harmless for size_bytes and wrong for reachability: sim_resid's batch D was
    # verified while `reachable_from_root` read True by definition (see the reach computation), the
    # rows then left the query, and the stale True would have outlived the fix that made the column
    # honest. Refresh that ONE field -- the others describe the row as it was when it was in scope,
    # which is the record this merge exists to preserve.
    orphan_ids = set(new.get("_orphan_ids") or ())
    live = {f["name"] for f in new["functions"]}
    kept = []
    for name, o in prev.items():
        if name in live or (o.get("state") or "not_started") == "not_started":
            continue
        if new.get("_orphan_ids") is not None and "addr" in o:
            o["reachable_from_root"] = o["addr"].lower().replace("0x", "") not in orphan_ids
        o["left_query"] = (
            "kept by merge_progress: state=%s, so this row is the record of finished work. The "
            "seed query no longer returns it -- for a writer_attribution domain that is exactly "
            "what verifying it does." % o.get("state")
        )
        kept.append(o)
    if kept:
        new["functions"].extend(kept)
        new["functions"].sort(key=lambda f: f.get("addr", ""))
        print(
            "  kept %d finished row(s) that left the query: %s"
            % (len(kept), ", ".join(sorted(f["name"] for f in kept)))
        )
    new.pop("_orphan_ids", None)  # a transport field, not part of the manifest
    return new


def antichain_groups(doc):
    """Group functions into sets that may share ONE rig run (no member reaches another).

    Greedy by layer, then verified: a cycle member is placed alone. This is the grouping the arming
    step should use -- not the raw layer column, which is only sufficient outside cycles.
    """
    groups = {}
    for f in doc["functions"]:
        if f["in_cycle"]:
            groups.setdefault("cycle:%s" % f["name"], []).append(f["name"])
        else:
            groups.setdefault("layer:%d" % f["layer"], []).append(f["name"])
    return dict(sorted(groups.items()))


def selftest_currency():
    """Prove the currency guard classifies drift in both directions, and in neither.

    Tested against `functions_currency()` on a DOCTORED COPY of en_functions.json rather than by
    corrupting the committed one: the classification IS the thing under test, and a guard that has
    never been seen to go red is indistinguishable from one that cannot.
    """
    import tempfile

    global FUNCTIONS
    saved = FUNCTIONS
    ok = True

    def check(label, cond):
        nonlocal ok
        print("  [%s] %s" % ("ok" if cond else "FAIL", label))
        ok = ok and bool(cond)

    try:
        drift, checked = functions_currency()
        check("the committed tree is CURRENT (0 disagreements)", not drift)
        check("  ... and it actually compared something", checked > 100)

        with open(FUNCTIONS, encoding="utf-8") as fh:
            doc = json.load(fh)
        renamed = 0
        for f in doc["functions"]:
            if f["name"].startswith("llm_") and renamed < 3:
                f["name"] = "FUN_" + f["entry"].replace("0x", "")
                renamed += 1
        with tempfile.TemporaryDirectory() as td:
            stale = os.path.join(td, "en_functions.json")
            with open(stale, "w", encoding="utf-8") as fh:
                json.dump(doc, fh)
            FUNCTIONS = stale
            drift, checked = functions_currency()
            check(
                "a stale copy holding %d old FUN_ name(s) is DRIFT" % renamed, len(drift) == renamed
            )
            check("  ... and it names the address and both names", all(len(r) == 3 for r in drift))
            check("  ... and it still compared the whole table", checked > 100)

            FUNCTIONS = os.path.join(td, "absent.json")
            drift, checked = functions_currency()
            check("an ABSENT en_functions is not reported as drift", not drift and checked == 0)
    finally:
        FUNCTIONS = saved
    print("gen_migration currency selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def check_all():
    """--check every ledger-owning domain. THE GATE.

    WHY IT WAS NOT ONE BEFORE, and what that cost. Every sibling derivation in this tree is gated --
    gen_region_accessors --check, gen_state_registry --check, gen_tact_closure --check -- but the
    migration manifests were not, on the reasoning that the generators already PRINT a gained/lost
    membership delta. A printed delta fails nothing and the next run absorbs it into the baseline.
    Measured 2026-08-28: four of five manifests had drifted, and one of them had silently invalidated
    a `done` tracker item (three functions renamed into `ai`'s prefix joined a closed batch as
    not_started). Only regenerating made the ALREADY-gated tracker<->ledger check see it.

    THE WAIVER, and why a waiver rather than an exclusion list in this file. A domain whose manifest
    is knowingly stale -- because regenerating it is an ADJUDICATION rather than a refresh, which is
    `readers` today at -36 members -- declares `select.stale_waiver: {why, tracked_in}` in its own
    profile. That keeps the reason next to the thing it is about, and it means the gate can land now
    instead of waiting: a gate that is red by design is what the architecture lint's own docstring says trains
    people to ignore lint_repo.

    AND THE WAIVER ITSELF IS CHECKED. A waived domain that turns out to be CURRENT fails, because a
    stale waiver is the failure this repo keeps writing checks against -- it reads as a known,
    accepted problem long after the problem is gone, and nothing ever deletes it.
    """
    import contextlib  # noqa: PLC0415
    import io as _io  # noqa: PLC0415

    from migration_domain import available  # noqa: PLC0415

    stale, waived, dead_waivers, unreadable, clean = [], [], [], [], []
    deltas = {}
    for name in sorted(available()):
        try:
            dom = load_domain(name)
        except SystemExit as exc:
            # A profile that owns no ledger (the ai_reclassified archive) is not a domain to check.
            unreadable.append((name, str(exc).splitlines()[0]))
            continue
        configure(dom)
        waiver = (dom.raw.get("select") or {}).get("stale_waiver")
        # build() prints this domain's membership delta. Captured rather than let through: over five
        # domains it buries the verdict, and it is only worth reading for a domain that IS stale --
        # where it is replayed under the failure.
        buf = _io.StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                doc = merge_progress(build(), OUT)
            text = json.dumps(doc, indent=2, ensure_ascii=False) + chr(10)
            with open(OUT, encoding="utf-8") as fh:
                is_stale = fh.read() != text
        except Exception as exc:  # noqa: BLE001 -- report, do not mask
            unreadable.append((name, "%s: %s" % (type(exc).__name__, exc)))
            continue
        deltas[name] = buf.getvalue().rstrip()
        if waiver and not is_stale:
            dead_waivers.append(name)
        elif waiver:
            waived.append((name, waiver))
        elif is_stale:
            stale.append(name)
        else:
            clean.append(name)

    for name, why in unreadable:
        print("  [skip] %s -- %s" % (name, why))
    for name, waiver in waived:
        print(
            "  [waived] %s is STALE by declaration -- %s (tracked in %s)"
            % (name, _flatten(waiver.get("why")), waiver.get("tracked_in", "?"))
        )
    for name in dead_waivers:
        print(
            "  [FAIL] %s declares select.stale_waiver but its manifest is CURRENT. Delete the "
            "waiver -- a waiver that outlives its problem reads as an accepted one forever." % name
        )
    for name in stale:
        print(
            "  [FAIL] tools/data/%s_migration.json is STALE -- run "
            "tools/gen_migration.py --domain %s (and read what MOVED before committing it)"
            % (name, name)
        )
        for line in (deltas.get(name) or "").splitlines():
            print("         " + line)
    bad = len(stale) + len(dead_waivers)
    print(
        "migration manifests: %d checked, %d current, %d stale, %d waived"
        % (
            len(clean) + len(stale) + len(waived) + len(dead_waivers),
            len(clean),
            len(stale),
            len(waived),
        )
    )
    return 1 if bad else 0


def _flatten(v):
    return " ".join(v) if isinstance(v, list) else (v or "no reason given")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    add_domain_arg(ap)
    ap.add_argument("--check", action="store_true", help="exit 1 if the on-disk file is stale")
    ap.add_argument("--antichain", action="store_true", help="print rig-armable groups and exit")
    ap.add_argument(
        "--selftest",
        action="store_true",
        help="prove the en_functions currency guard classifies drift; writes no files",
    )
    ap.add_argument(
        "--effects",
        action="store_true",
        help="print the outward-call effect inventory (shadow safety) and exit",
    )
    ap.add_argument(
        "--check-all",
        action="store_true",
        help="--check EVERY ledger-owning domain; the lint_repo gate. Honours select.stale_waiver",
    )
    args = ap.parse_args()

    if args.selftest:
        return selftest_currency()
    if args.check_all:
        return check_all()

    configure(load_domain(args.domain))

    for src in (SYMBOLS, CALLGRAPH, MATRIX):
        if not os.path.exists(src):
            print("missing input: %s" % os.path.relpath(src, REPO), file=sys.stderr)
            return 2

    # CURRENCY GUARD (finding 2026-08-24-1151-19): refuse rather than shrink. A seeded domain
    # resolves its seed NAMES through en_functions.json, so a session that renamed functions and
    # did not re-dump that file silently loses exactly the rows it renamed -- and the readiness
    # predicate then reports a green over the smaller scope with nothing naming the shrink.
    drift, checked = functions_currency()
    if drift:
        print(
            "REFUSING to generate -- tools/data/en_functions.json disagrees with docs/symbols.md on "
            "%d of %d shared address(es). A seeded domain resolves its seeds by NAME through that "
            "file, so a stale copy drops the renamed rows out of the manifest without a word."
            % (len(drift), checked),
            file=sys.stderr,
        )
        print("Re-dump it, then regenerate:", file=sys.stderr)
        print(
            '  run-script {programPath: "/eng/mh.exe", scriptName: "mh_dump_functions.py"}',
            file=sys.stderr,
        )
        print("  python tools/gen_migration.py --domain %s" % DOM.name, file=sys.stderr)
        print("Disagreements (address: symbols.md -> en_functions.json):", file=sys.stderr)
        for addr, sym_name, fn_name in drift[:20]:
            print("  %s: %s -> %s" % (addr, sym_name, fn_name), file=sys.stderr)
        if len(drift) > 20:
            print("  ... and %d more" % (len(drift) - 20), file=sys.stderr)
        return 2

    doc = merge_progress(build(), OUT)

    # WRITE-GUARD (lever 1): never emit a ledger that breaks the state/tier schema. The merge above
    # preserves each row's progress from the on-disk ledger, so a hand-drifted state/tier would be
    # carried straight through -- and `--check` would then say "up to date" because the regenerated
    # text matches the drifted file. Validating the merged rows HERE, before any mode branch, is the
    # only place that catches it at generation time. The state/tier vocabulary itself is owned by
    # the migration-ledger schema module, which is why the import is here rather than at the top:
    # REGENERATING a manifest is research-tree work, while this file is also imported as a LIBRARY
    # (gen_libmh_inbound uses load_addr_taken/orphan_closure), and a module-level import would make
    # that library unusable wherever the research half is not carried. An ImportError here says so
    # by name instead of failing the import of the whole module.
    # A readiness domain (MIG-SET decision B) emits no progress fields at all, so there is no
    # state/tier vocabulary here to break -- validating would demand the very column the flag exists
    # to remove. Skipped by the same rule the schema check uses, so the two cannot disagree.
    if DOM.records_state:
        import migration_ledger  # noqa: PLC0415 -- see above: library-vs-generator split

        schema_probs = migration_ledger.validate(doc["functions"])
    else:
        schema_probs = []
    if schema_probs:
        print(
            "REFUSING to emit %s -- %d state/tier schema violation(s). Fix the ledger (a reviewed row "
            "with a tier, or an unknown state/tier) first:"
            % (os.path.relpath(OUT, REPO).replace("\\", "/"), len(schema_probs)),
            file=sys.stderr,
        )
        for fn, why in schema_probs[:10]:
            print("  %s: %s" % (fn, why), file=sys.stderr)
        return 1

    text = json.dumps(doc, indent=2, ensure_ascii=False) + "\n"

    if args.effects:
        e = doc.get("_outward_effects")
        if e is None:
            print(
                "domain %r declares no effect families (effect_prefixes / cut_prefixes / cut_names_from),\n"
                "so the outward-effect surface was NOT MEASURED. That is not the same as finding\n"
                "none: add the families to the profile if this domain can call out to sound, UI,\n"
                "input or net." % DOM.name
            )
            return 0
        print(
            "outward effects: %d edges from %d callers into %d targets"
            % (e["edges"], e["callers"], e["targets"])
        )
        print(
            "  classified: %s   verified against a body: %d/%d"
            % (
                ", ".join("%s %d" % (c, e["by_class"][c]) for c in EFFECT_CLASSES),
                e["verified"],
                e["targets"],
            )
        )
        # BOTH COUNTS, UNCONDITIONALLY, AND PER CLASS. The sections below print only when they have
        # rows, so a finished seam prints no UNCLASSIFIED block and no STALE block -- and so does a
        # run where the walk found nothing to classify. "Zero" has to be a number somebody wrote,
        # not the absence of a heading. The per-class ratio is here for the same reason: an aggregate
        # 29/31 hides WHICH two are ungrounded, and the two classes that can double-fire something
        # (effectful, state) are the ones whose ratio has to be read.
        rows_by_class = {
            c: [r for r in e["target_rows"] if r["class"] == c] for c in EFFECT_CLASSES
        }
        print(
            "  unclassified: %d    STALE entries: %d"
            % (len(e["unclassified"]), len(e["stale_class_entries"]))
        )
        print(
            "  verified per class: %s"
            % ", ".join(
                "%s %d/%d"
                % (
                    c,
                    sum(1 for r in rows_by_class[c] if r["class_verified"]),
                    len(rows_by_class[c]),
                )
                for c in EFFECT_CLASSES
            )
        )
        for cls in EFFECT_CLASSES + (None,):
            rows = [r for r in e["target_rows"] if r["class"] == cls]
            if not rows:
                continue
            print(
                "\n-- %s --"
                % (
                    cls.upper()
                    if cls
                    else "UNCLASSIFIED (no entry in the domain's effect_classes file)"
                )
            )
            for r in rows:
                mark = " " if r["class_verified"] else "?"
                print(
                    "  %s %3dx %6dB  %-44s  <- %s"
                    % (
                        mark,
                        r["call_sites"],
                        r["size_bytes"],
                        r["name"],
                        ", ".join(r["callers"][:3])
                        + (" +%d" % (len(r["callers"]) - 3) if len(r["callers"]) > 3 else ""),
                    )
                )
        if e["stale_class_entries"]:
            print(
                "\nSTALE entries (classified, but nothing in the migration set calls them): %s"
                % ", ".join(e["stale_class_entries"])
            )
        # DELIBERATELY UNGATED targets, printed with their reason. Counted separately from the
        # classified total on purpose: they ARE classified, so folding them into "31 classified"
        # would read as 31 covered. A hole that is written down is a different object from a hole
        # nobody noticed, but only if the report names which ones they are on every run.
        if e.get("not_gated"):
            print(
                "\nDELIBERATELY UNGATED (%d) -- classified, but `gate: false` in the domain's "
                "effect_classes file. NOT covered by the seam; each one's `gate_why` says why:"
                % len(e["not_gated"])
            )
            for r in e["target_rows"]:
                if r.get("gated") is False:
                    print("  %-44s %s" % (r["name"], r.get("gate_why") or "(NO REASON GIVEN)"))
        # GATING SOUNDNESS. A `state`/`pure` disposition means "do not suppress this", which is
        # only safe if everything irreversible below it is gated somewhere else. Printed even
        # when empty: "the check found nothing" and "the check did not run" must not look alike.
        ung_all = e.get("ungated_reach")
        ung = ung_all.get("findings") if isinstance(ung_all, dict) else ung_all
        if ung_all is None:
            print("\nGATING SOUNDNESS: NOT CHECKED -- regenerate the manifest to populate it")
        elif ung:
            print(
                "\nUNGATED REACH -- %d non-effectful target(s) reach an irreversible function "
                "without\npassing through a gated one. Either the gate list is short a target, "
                "or the disposition\nis wrong. Both are judgement calls; neither is safe to arm past."
                % len(ung)
            )
            for f in ung:
                print(
                    "  %-44s (%s) reaches: %s" % (f["target"], f["class"], ", ".join(f["reaches"]))
                )
        else:
            print(
                "\nGATING SOUNDNESS: OK -- every `state`/`pure` target's forward closure reaches "
                "an\nirreversible function only THROUGH a gated target, so the seam covers it."
            )
        # THE OTHER DIRECTION, and it had no check at all until TACT-CUT. UNGATED REACH asks
        # whether a NON-suppressed target can reach something irreversible. This asks whether a
        # SUPPRESSED one removes a write the oracle was going to compare -- in which case every
        # armed call reports a divergence the seam itself manufactured. Printed even when empty,
        # for the same reason as the block above.
        bw = e.get("blinded_writes")
        if bw is None:
            print(
                "\nBLINDED WRITES: NOT CHECKED -- tmp/state_matrix.json is absent (it is "
                "gitignored).\n  Regenerate it to find out whether any `effectful` suppression "
                "removes a compared write."
            )
        elif not bw.get("findings"):
            print(
                "\nBLINDED WRITES: OK -- no `effectful` target writes a region the oracle hashes "
                "or\nsnapshots, so suppressing them in our arm removes nothing the comparison reads."
            )
        else:
            live = [f for f in bw["findings"] if not f["acked"]]
            print(
                "\nBLINDED WRITES -- %d `effectful` target(s) write a region the oracle COMPARES.\n"
                "Suppressing them in our arm removes a write the original made, so an armed call\n"
                "diverges because of the seam, not because of our code. Either the disposition is\n"
                "wrong, or the consequence is acceptable and belongs in `_blinded_ack` with its\n"
                "reason." % len(bw["findings"])
            )
            for f in bw["findings"]:
                print(
                    "  %-44s %s%s"
                    % (f["target"], ", ".join(f["regions"]), "   [ACKED]" if f["acked"] else "")
                )
            if not live:
                print("  ...all acknowledged in `_blinded_ack`.")
        # NOTE the floor this check stands on, because it is the same one the tactical profile
        # records: the instruction sweep sees writes to a NAMED global address. It does not see a
        # write through a pointer parameter, nor `&base + i*stride`. A clean report is evidence,
        # not proof: the reference manager is blind to computed accesses.

        stale_b = ung_all.get("benign_stale") if isinstance(ung_all, dict) else None
        if stale_b:
            print(
                "  STALE _benign_passthrough -- the walk never descended through these, so their"
                "\n  recorded body evidence describes something nobody relies on now: %s"
                % ", ".join(stale_b)
            )
        print("\n('?' = disposition read off the name, not yet confirmed against the body)")
        return 0

    if args.antichain:
        for name, members in antichain_groups(doc).items():
            print(
                "%-12s %3d  %s"
                % (
                    name,
                    len(members),
                    ", ".join(sorted(members)[:4]) + (" ..." if len(members) > 4 else ""),
                )
            )
        return 0

    if args.check:
        cur = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        if cur != text:
            print(
                "%s is STALE -- run tools/gen_migration.py --domain %s"
                % (os.path.relpath(OUT, REPO).replace("\\", "/"), DOM.name),
                file=sys.stderr,
            )
            return 1
        print(
            "%s up to date (%d functions)"
            % (
                os.path.relpath(OUT, REPO).replace("\\", "/"),
                doc["summary"]["functions"],
            )
        )
        return 0

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write(text)
    s = doc["summary"]
    print("wrote %s" % os.path.relpath(OUT, REPO))
    print(
        "  functions %d  reachable %d  zero-callers %d  proto-uncommitted %d"
        % (s["functions"], s["reachable_from_root"], s["zero_callers"], s["proto_uncommitted"])
    )
    print(
        "  writes-shared %d  review-required %d  layers 0..%d  in-cycle %d"
        % (s["writes_shared"], s["review_required"], s["max_layer"], s["in_cycle"])
    )
    print("  batches: %s" % s["by_batch"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
