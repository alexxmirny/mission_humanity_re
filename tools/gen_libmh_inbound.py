#!/usr/bin/env python3
"""gen_libmh_inbound.py -- LIB-REF-IN: the INBOUND (host -> libmh) surface, derived not typed.

THE QUESTION, and why it is not the reconciliation's. `report_promotion_reconciliation.py`
section 5 asks a PRE-FORK question -- which owned bodies can the original binary still walk into
as originals -- and answers it with every installed MH_EXPORT_REPLACE CUT from the graph. Run
today it prints 4 rows, because SIM-HOSTREACH's installations moved the cut. That is the wrong
model for an ABI: at the fork there are no promotion seams at all, so the question is simply

    which ORIGINAL function directly calls a body libmh owns, and what does the fork do with it?

so this tool walks the graph CUT-FREE and over ALL SIX per-function ledgers, not `sim` alone.
docs/libmh-abi.md section 8 measured the same thing by hand once; its 41/88/111 does not
reproduce even at its own commit (replaying its model there gives 47/120/94 -- the gap is the 22
pruned roots), which is exactly why membership lives here and not in prose. That section says
"reproduce, do not re-type". This is the reproduction.

THREE SOURCES, none of them prose:
  * tools/data/*_migration.json        -- what we own (state: verified)
  * tools/data/call_graph_no_crt.json  -- who calls it
  * tools/data/dll_call_protos.json    -- the committed prototype, i.e. the real arity and types

TWO HAND-WRITTEN ADJUDICATIONS, both hard-gated, neither defaultable:
  * libmh_inbound_callers.json  -- what CLASS each original caller is (what the fork does with it)
  * libmh_inbound_entries.json  -- per owned body: the inbound entry that serves it, or a recorded
                                   exclusion code + reason

THE FAILURE THIS GUARDS is the one that reads like success. A candidate row nobody dispositioned
reads exactly like a row that needs no API; an adjudication row whose body is no longer a
candidate reads exactly like a live decision. Both fail here, by name.

    python tools/gen_libmh_inbound.py            # regenerate + print the report
    python tools/gen_libmh_inbound.py --check    # the lint gate: stale output or a gap -> exit 1
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LEDGER_GLOB = os.path.join(REPO, "tools", "data", "*_migration.json")
CALLGRAPH = os.path.join(REPO, "tools", "data", "call_graph_no_crt.json")
PROTOS = os.path.join(REPO, "tools", "data", "dll_call_protos.json")
CALLERS_ADJ = os.path.join(REPO, "tools", "data", "libmh_inbound_callers.json")
ENTRIES_ADJ = os.path.join(REPO, "tools", "data", "libmh_inbound_entries.json")
PRUNED_ROOTS = os.path.join(REPO, "tools", "data", "reconciliation_pruned_roots.json")

OUT_PUBLIC = os.path.join(REPO, "src", "mh_dll", "libmh", "include", "libmh_host_in.gen.h")
OUT_DISPATCH = os.path.join(REPO, "src", "mh_dll", "libmh", "libmh_host_in_dispatch.gen.h")
HAND_HEADER = os.path.join(REPO, "src", "mh_dll", "libmh", "include", "libmh_host_in.h")

# A caller class that implies an inbound entry (the fork's host is the one making the call).
API_CLASSES = {"FE", "NET", "SESSION"}

# Entries the hand-authored header declares that serve no derived ROW: the surface's own envelope
# (handshake, diagnostics, the order-table introspection) plus the one design-completeness addition
# recorded as such in the header's banner. Anything ELSE declared there must be a covered row's
# entry -- a declaration nobody derives is the stale half, and it reads exactly like a live one.
ENVELOPE_ENTRIES = {
    "libmh_in_open",
    "libmh_in_is_open",
    "libmh_in_unbound",
    "libmh_in_entry_name",
    "libmh_issue_order",
    "libmh_order_arity",
    "libmh_order_name",
    "libmh_drain_outbound_orders",
}

# An entry whose name carries this suffix is served by the REPLAY SPINE already declared in
# libmh.h (bind / import / submit_order / sim_step / state_hash / save / load). Those entries are
# LIB-REF's to implement, not this item's, so they are not declared here, not walked by
# libmh_in_unbound, and not address-taken by the dispatch table -- but they ARE printed, because a
# row quietly dropped from the surface is the failure this whole tool exists for.
SPINE_SUFFIX = " [spine]"


# ---- derivation -------------------------------------------------------------------------------


def _ledger_rows():
    out = []
    for path in sorted(glob.glob(LEDGER_GLOB)):
        try:
            doc = json.load(open(path, encoding="utf-8"))
        except (OSError, ValueError):
            continue
        fns = doc.get("functions")
        if not isinstance(fns, list):
            continue  # legacy_conv_migration.json is a group roster, not a per-function ledger
        domain = doc.get("_domain") or os.path.basename(path).replace("_migration.json", "")
        for row in fns:
            out.append((domain, row))
    return out


HOSTED_SRC = os.path.join(REPO, "src", "mh_dll")


def hosted_demand():
    """Symbols the HOSTED build's own C++ references -- the SECOND consumer of this surface.

    THE SURFACE HAS TWO CONSUMERS AND THIS TOOL ONLY MODELLED ONE. The derived question is a fork
    question: which ORIGINAL function calls a body libmh owns, so which entry does the fork's host
    need. But the entries exist TODAY too, and hosted promotion code calls them directly -- e.g.
    libmh/orders/issue/issue_promote.cpp issues `libmh_issue_order(LIBMH_ORD_UNIT_ORDER_EXIT_STORAGE,
    ...)`. Those two demands are independent: a body whose every original caller is owned or dead
    has no FORK demand and full HOSTED demand.

    IT WENT UNNOTICED BECAUSE A BUG WAS PROPPING IT UP. Before pruned roots were honoured here, the
    dead front-end callers llm_cam_pan_to_building and llm_strat_ui_unload_group_from_storage
    counted as live, so both affected rows had (spurious) fork demand and never came up. Honouring
    the prune removed the prop and the rows went stale -- and the first regeneration DELETED
    LIBMH_ORD_UNIT_ORDER_EXIT_STORAGE out from under a live call site, i.e. broke the build. A
    generator that can delete a symbol its own tree still calls is measuring the wrong thing.

    DERIVED, NOT WRITTEN, deliberately. A hand `"retain": true` would be the silent-keep this
    surface's whole design refuses: it reads identical whether the reason is live or expired. A
    grep over the non-generated sources expires by itself the moment the last call site goes.
    """
    toks = set()
    for root, _dirs, files in os.walk(HOSTED_SRC):
        for f in files:
            if not f.endswith((".cpp", ".h")):
                continue
            if f.endswith((".gen.h", ".gen.cpp")):
                continue  # generated: it would cite every symbol and make the check vacuous
            try:
                text = open(os.path.join(root, f), encoding="utf-8", errors="ignore").read()
            except OSError:
                continue
            toks |= set(re.findall(r"\bLIBMH_ORD_[A-Z0-9_]+\b", text))
            toks |= set(re.findall(r"\bLIBMH_IN_ENTRY_[A-Z0-9_]+\b", text))
            # MACRO-MEDIATED USE, and it is the MAJORITY here: host_in.cpp names its entry id by
            # token paste -- MH_IN_GUARD_V(BLDG_GET_COORDS) expands to LIBMH_IN_ENTRY_BLDG_GET_COORDS
            # -- so a literal-token grep sees 2 entry ids where 44 are in use. A first cut of this
            # function did exactly that and reported libmh_bldg_get_coords as demanded by nobody;
            # the BUILD caught it (host_in.cpp: undeclared identifier), the scan did not. Same
            # blind spot as the macro-mediated mh::call:: census hit.
            toks |= {
                "LIBMH_IN_ENTRY_" + m
                for m in re.findall(r"\bMH_IN_GUARD(?:_V)?\(\s*([A-Z0-9_]+)", text)
            }
    return toks


def pruned_rows():
    """{name: row} -- reconciliation_pruned_roots.json, the WRITTEN half of the dead adjudication."""
    try:
        doc = json.load(open(PRUNED_ROOTS, encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return {r["name"]: r for r in doc.get("pruned_roots", [])}


def dead_callers(by_name, id_name, callers_of):
    """(dead, findings, advisory) -- original callers the fork does not have, because nothing runs them.

    THE THIRD DEAD-SOURCE, and why it was missing. This tool asks "which ORIGINAL function calls a
    body libmh owns", and answered it from the migration ledgers alone (`state: verified|dead`).
    But a ledger row is a MIGRATION artefact -- a function nobody ever scheduled has no row at all,
    so a provably-dead caller was indistinguishable from a live one and kept forcing an exclusion.
    X-TL-DRAIN measured the cost: 14 of the 19 callers in the X-TL class were ALREADY certified dead
    by reconciliation_pruned_roots.json, filed the day before the item that proposed translating
    them. Two generated instruments in one repo disagreed about the same 14 functions, and the
    expensive one won by default.

    MEMBERSHIP IS DERIVED x WRITTEN, and needs BOTH halves:
      * DERIVED -- gen_migration.orphan_closure over the current call graph: no callers, or every
        caller itself orphaned (transitive), with address-taken functions exempt (G94). This half
        moves when the DB moves, so a row cannot outlive its evidence.
      * WRITTEN -- a reconciliation_pruned_roots.json row. This half is what stops a silent shrink,
        the failure that file's own header names: a set that shrank reads like a set that got
        finished. Dropping a caller is a NEGATIVE CLAIM on a ref-manager-blind binary
        on a ref-manager-blind binary, and it gets made by name or not at all.

    ASYMMETRIC ON PURPOSE. Orphaned-but-unwritten is treated as LIVE and merely PRINTED as a prune
    candidate -- that direction can only over-count (a row keeps forcing an exclusion it may not
    need), never hide a real inbound edge. The reverse would be a filter nobody wrote down.

    TWO HARD REFUSALS, both mutation-proven at X-TL-DRAIN step 3:
      * CONTRADICTION -- a function both `verified` in a ledger and pruned here. `verified` means a
        reimplementation of it was proven and runs; pruned means nothing reaches it. Both cannot be
        true, and whichever is wrong is expensive: either the prune is hiding a live inbound edge,
        or a translation was spent on dead code.
      * EXPIRED PRUNE -- a pruned row whose function is no longer in the orphan closure. That is
        report_promotion_reconciliation.py --check-roots' claim, but this tool USES the row to drop
        a caller, so it re-derives rather than trusting; otherwise a stale row silently deletes an
        inbound edge here while only the other tool goes red.
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import gen_migration as gm

    rows = pruned_rows()
    if not rows:
        return set(), [], set()
    addr_taken = gm.load_addr_taken()
    if addr_taken is None:
        # No verdict beats a guess (G94): without the scan every fn-ptr handler reads as orphaned.
        return (
            set(),
            ["NO ADDRESS-TAKEN SCAN: cannot honour pruned roots -- run mh_dump_addr_taken.py"],
            set(),
        )
    orphan = gm.orphan_closure(set(id_name), callers_of, addr_taken)

    dead, findings = set(), []
    for name in sorted(rows):
        i = by_name.get(name)
        if i is None:
            findings.append(
                "EXPIRED PRUNE: %s carries a reconciliation_pruned_roots.json row but is no longer "
                "in the call graph -- remove the row or regenerate the graph" % name
            )
            continue
        if i not in orphan:
            live = sorted({id_name[c] for c in callers_of.get(i, ()) if c not in orphan})
            findings.append(
                "EXPIRED PRUNE: %s is pruned but is REACHABLE again (live caller(s): %s). This tool "
                "drops its inbound edges on that row -- re-establish the evidence or remove it "
                "(report_promotion_reconciliation.py --check-roots)"
                % (name, ", ".join(live[:4]) or "?")
            )
            continue
        dead.add(name)
    return dead, findings, {n for n in id_name.values() if by_name.get(n) in orphan}


def candidates():
    """(rows, findings, advisory) -- the fork's inbound candidate set.

    rows = [(domain, owned_body, [original callers])].

    CUT-FREE on purpose: a promotion seam is what the fork REMOVES, so cutting promoted entries
    here would make the ABI's membership depend on what happens to be installed today.
    """
    g = json.load(open(CALLGRAPH, encoding="utf-8"))
    by_name, id_name = {}, {}
    for n in g["nodes"]:
        i = n["id"].lower()
        by_name.setdefault(n["name"], i)
        id_name[i] = n["name"]
    callers_of = {}
    for l in g["links"]:
        callers_of.setdefault(l["target"].lower(), set()).add(l["source"].lower())

    rows = _ledger_rows()
    verified = {}
    for dom, r in rows:
        if r.get("state") == "verified":
            verified.setdefault(r["name"], dom)
    dead = {r["name"] for _d, r in rows if r.get("state") == "dead"}

    pruned, findings, orphaned = dead_callers(by_name, id_name, callers_of)
    for n in sorted(set(verified) & pruned):
        findings.append(
            "CONTRADICTION: %s is `verified` in the %s ledger AND pruned in "
            "reconciliation_pruned_roots.json -- a body we run cannot be a body nothing reaches. "
            "One of the two is wrong; neither is safe to assume." % (n, verified[n])
        )
    # Callers the fork does not have, for either of two DIFFERENT reasons, and the distinction is
    # worth keeping in view: `verified|dead` are bodies libmh OWNS (their calls become internal C++
    # calls), `pruned` are originals that stay original but never RUN. Both stop being inbound
    # edges; neither is folded into the other. A pruned function can still be a ROW in its own
    # right -- an owned body may be dead-called and live-called at once.
    gone = set(verified) | dead | pruned

    disp = {}
    try:
        disp = {r["name"]: r for r in json.load(open(ENTRIES_ADJ, encoding="utf-8"))["rows"]}
    except (OSError, ValueError, KeyError):
        pass
    demand = hosted_demand()

    out = []
    for name, dom in sorted(verified.items()):
        i = by_name.get(name)
        if not i:
            continue
        cs = sorted(
            id_name.get(s, s) for s in callers_of.get(i, ()) if id_name.get(s, s) not in gone
        )
        if cs:
            out.append((dom, name, cs))
            continue
        # No FORK demand left. Keep the row anyway if the HOSTED build still calls its entry --
        # see hosted_demand(). `cs` stays EMPTY, which is the honest statement: no original calls
        # it, so it contributes no caller edge, forces no class, and needs no adjudication row in
        # libmh_inbound_callers.json. It only keeps its id emitted for the code that calls it.
        d = disp.get(name)
        if not d or "entry" not in d:
            continue
        ids = {_entry_id(d["entry"])}
        if d["entry"] == "libmh_issue_order":
            ids.add(_ord_id(name))
        if ids & demand:
            out.append((dom, name, []))
    # Orphaned callers nobody has written a prune row for. Treated as LIVE above (they still force
    # their rows) and only reported, scoped to callers that actually reach an owned body -- the
    # unscoped orphan set is ~500 functions and would be noise, not a worklist.
    candidates_to_prune = sorted({c for _d, _n, cs in out for c in cs if c in orphaned})
    return out, findings, candidates_to_prune


def protos():
    doc = json.load(open(PROTOS, encoding="utf-8"))
    fns = doc.get("functions", doc)
    if isinstance(fns, dict):
        fns = list(fns.values())
    return {f["name"]: f for f in fns if isinstance(f, dict) and "name" in f}


# ---- adjudication -----------------------------------------------------------------------------


def adjudicate(cand):
    """(rows, findings). A finding is a hard failure; there is no soft state."""
    cadj = json.load(open(CALLERS_ADJ, encoding="utf-8"))
    eadj = json.load(open(ENTRIES_ADJ, encoding="utf-8"))
    cls = {c["name"]: c["class"] for c in cadj["callers"]}
    disp = {r["name"]: r for r in eadj["rows"]}
    p = protos()

    live_callers = {c for _d, _n, cs in cand for c in cs}
    live_rows = {n for _d, n, _cs in cand}
    findings = []

    for c in sorted(live_callers - set(cls)):
        findings.append("UNCLASSIFIED CALLER: %s -- add it to libmh_inbound_callers.json" % c)
    for c in sorted(set(cls) - live_callers):
        findings.append(
            "STALE CALLER ROW: %s no longer calls any owned body -- remove it from "
            "libmh_inbound_callers.json" % c
        )
    for n in sorted(set(disp) - live_rows):
        findings.append(
            "STALE DISPOSITION: %s is no longer an inbound candidate -- remove it from "
            "libmh_inbound_entries.json" % n
        )

    rows, uncovered = [], []
    for dom, name, cs in cand:
        classes = sorted({cls.get(c, "?") for c in cs})
        needs_api = any(k in API_CLASSES for k in classes)
        d = disp.get(name)
        if d is None:
            (uncovered if needs_api else uncovered).append(name)
            findings.append(
                "UNCOVERED: %s (%s) is called by %s and has no entry and no exclusion -- "
                "disposition it in libmh_inbound_entries.json"
                % (
                    name,
                    dom,
                    ", ".join(c for c in cs if cls.get(c) in API_CLASSES) or "original code",
                )
            )
            continue
        if "entry" in d:
            if not d.get("impl") or not d.get("header"):
                findings.append("ENTRY WITHOUT AN IMPLEMENTATION: %s" % name)
            # LIB-SPINE-API: the ENTRY-side half of the contradiction check. A `hosted` block says
            # "the fork does not export this edge, so its seam cannot be routed" -- and an
            # FE/NET/SESSION caller says the opposite, because those three classes are host-side at
            # the fork BY DEFINITION and their call into an owned body needs a designed entry. Until
            # this ran, the check existed only for X-TL/X-BOOT *exclusions*, so a covered row saying
            # the same thing said it unchecked: llm_map_region_pool_reset is disposition'd onto the
            # blob-in libmh_import_world and is called by llm_fatal_cleanup (SESSION), and nothing
            # anywhere noticed. Each such caller is excused BY NAME with a measured reason, or this
            # fails -- the same shape as every other adjudication in this file.
            #
            # SCOPE, and getting it wrong once is what pinned it down: the check fires on the two
            # assertions that actually claim the fork does not export the edge -- a hosted class of
            # `driver-absorbed` / `boot-blob`, or a row served by a SPINE entry (the spine is drivers
            # plus state import, never a per-body service). It does NOT fire on `no-crossing` rows
            # that carry a real entry: llm_strat_bldg_get_coords is called by llm_cam_pan_to_building
            # (FE) and libmh_bldg_get_coords exists FOR that caller -- the absent hosted seam says
            # nothing about the fork.
            h = d.get("hosted")
            asserts_no_export = (h or {}).get("class") in ("driver-absorbed", "boot-blob") or d[
                "entry"
            ].endswith(SPINE_SUFFIX)
            if h and asserts_no_export:
                excused = set((h.get("api_callers_excused") or {}).keys())
                for c in cs:
                    if cls.get(c) in API_CLASSES and c not in excused:
                        findings.append(
                            "HOSTED DISPOSITION CONTRADICTED BY A LIVE EDGE: %s is disposition'd "
                            "hosted-%s (the fork does not export this edge) but is called by %s, "
                            "class %s, which is host-side at the fork -- excuse that caller by name "
                            "in api_callers_excused with a reason, or the row needs a real entry"
                            % (name, h.get("class", "?"), c, cls.get(c))
                        )
                for c in sorted(excused - set(cs)):
                    findings.append(
                        "STALE EXCUSED CALLER: %s excuses %s, which no longer calls it" % (name, c)
                    )
            rows.append(
                {
                    "name": name,
                    "domain": dom,
                    "entry": d["entry"],
                    "impl": d.get("impl"),
                    "header": d.get("header"),
                    "hosted": h,
                    "classes": classes,
                    "callers": cs,
                    "proto": p.get(name),
                }
            )
        elif "exclusion" in d:
            if needs_api and d["exclusion"] in ("X-TL", "X-BOOT"):
                findings.append(
                    "EXCLUSION CONTRADICTED BY A LIVE EDGE: %s is excluded as %s but is called by "
                    "%s"
                    % (name, d["exclusion"], ", ".join(c for c in cs if cls.get(c) in API_CLASSES))
                )
            rows.append(
                {
                    "name": name,
                    "domain": dom,
                    "exclusion": d["exclusion"],
                    "why": d.get("why", ""),
                    "classes": classes,
                    "callers": cs,
                }
            )
        else:
            findings.append("MALFORMED DISPOSITION (neither entry nor exclusion): %s" % name)
    return rows, uncovered, findings


# ---- emission ---------------------------------------------------------------------------------

# What CANNOT ride an int32_t argv. Stated as a refusal rather than an allow-list on purpose: a
# new integer spelling appearing in the committed prototypes should widen automatically, whereas a
# pointer or a float must stop the generator and get its own typed entry. `char` is here as an
# integer (llm_strat_order_set_player_control_mode's mode byte) -- it marshals exactly.
_FLOAT_CTYPES = {"float", "double", "long double"}


def order_rows(rows):
    """The rows dispatched by libmh_issue_order, in stable id order (the name's own sort)."""
    return sorted(
        (r for r in rows if r.get("entry") == "libmh_issue_order"), key=lambda r: r["name"]
    )


def _ord_id(name):
    n = name
    for pre in ("llm_strat_", "llm_"):
        if n.startswith(pre):
            n = n[len(pre) :]
            break
    return "LIBMH_ORD_" + n.upper()


def _entry_id(entry):
    """The id of an ENTRY (not of a row): libmh_post_event -> LIBMH_IN_ENTRY_POST_EVENT.

    Keyed on the entry because that is what the table indexes and what the trap names. Keying it
    on the ROW was the first shape and it was wrong in a way that reads fine: five rows share
    libmh_strat_mode_init, so a row-keyed table declared five slots for one function and the
    unbound walk would have reported the same name up to five times.
    """
    n = entry
    if n.startswith("libmh_"):
        n = n[len("libmh_") :]
    return "LIBMH_IN_ENTRY_" + n.upper()


def entry_names(rows):
    """The inbound entries this header owns, in stable id order. Spine entries are excluded."""
    return sorted(
        {r["entry"] for r in rows if "entry" in r and not r["entry"].endswith(SPINE_SUFFIX)}
    )


def spine_entries(rows):
    return sorted(
        {
            r["entry"][: -len(SPINE_SUFFIX)]
            for r in rows
            if "entry" in r and r["entry"].endswith(SPINE_SUFFIX)
        }
    )


def check_declarations(rows):
    """Both directions between the DERIVED entry set and the HAND-AUTHORED header.

    Forward: an entry a row names but nothing declares is a row served by a function that does not
    exist. Backward: a declaration no row derives is the stale half -- and a stale declaration is
    indistinguishable from a live one by reading, which is the failure mode this whole tool exists
    to make impossible. The SIGNATURES are not checked here on purpose: the implementation TU
    includes this header and forwards into the C++ wrapper, so the COMPILER checks them, end to
    end, which a regex could only approximate.
    """
    bad = []
    try:
        text = open(HAND_HEADER, encoding="utf-8").read()
    except OSError:
        return ["MISSING HAND HEADER: %s" % os.path.relpath(HAND_HEADER, REPO)]

    # Declarations are the header's `... libmh_xxx(` forms outside comments. The header's prose
    # mentions entry names too, so require the open paren -- prose never spells one.
    declared = set()
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("*") or s.startswith("/*") or s.startswith("//") or s.startswith("#"):
            continue
        for m in re.finditer(r"\b(libmh_[A-Za-z0-9_]+)\s*\(", line):
            declared.add(m.group(1))

    want = set(entry_names(rows))
    for e in sorted(want - declared):
        bad.append(
            "ENTRY NOT DECLARED: %s serves a derived row but has no prototype in "
            "libmh/include/libmh_host_in.h" % e
        )
    for d in sorted(declared - want - ENVELOPE_ENTRIES):
        bad.append(
            "STALE DECLARATION: libmh_host_in.h declares %s, which serves no derived row and is "
            "not part of the surface's envelope -- delete it or disposition a row onto it" % d
        )
    return bad


def fnv1a32(text):
    h = 0x811C9DC5
    for b in text.encode("utf-8"):
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def check_order_params(ords):
    """Orders marshal through an int32_t argv; a pointer or float param cannot ride it."""
    bad = []
    for r in ords:
        pr = r.get("proto")
        if not pr:
            bad.append("%s: no committed prototype in dll_call_protos.json" % r["name"])
            continue
        for prm in pr.get("params", []):
            ct = (prm.get("ctype") or "").strip()
            if "*" in ct or ct in _FLOAT_CTYPES:
                bad.append(
                    "%s: parameter %s is %s -- libmh_issue_order marshals integer argv only, so "
                    "this row needs its own typed entry rather than an order id"
                    % (r["name"], prm.get("name"), ct)
                )
    return bad


def render_public(rows, ords):
    covered = [r for r in rows if "entry" in r]
    entries = sorted({r["entry"] for r in covered})
    desc = "|".join(
        "%s:%s:%d" % (r["name"], r["entry"], len((r.get("proto") or {}).get("params", [])))
        for r in sorted(covered, key=lambda r: r["name"])
    )
    ver = fnv1a32(desc)
    L = []
    L.append("/* GENERATED by tools/gen_libmh_inbound.py -- DO NOT EDIT.")
    L.append(" *")
    L.append(" * The INBOUND (host -> libmh) surface's derived half: the order-id table that")
    L.append(
        " * libmh_issue_order dispatches on, the entry-name table libmh_in_unbound walks, and the"
    )
    L.append(
        " * version constant both sides handshake with. The DESIGNED half -- the C prototypes a"
    )
    L.append(" * host actually calls -- is hand-authored in libmh_host_in.h, the same division")
    L.append(" * libmh_host_events.h and libmh_host_api.gen.h already use.")
    L.append(" *")
    L.append(
        " * LIBMH_HOST_IN_VERSION IS COMPUTED, not maintained: it is a hash over every covered"
    )
    L.append(" * row's (body, entry, arity). Adding, removing or re-arity-ing an entry moves it")
    L.append(" * automatically, so a stale host is refused by libmh_in_open without anyone having")
    L.append(" * remembered to bump a constant.")
    L.append(" *")
    L.append(" * `gen_libmh_inbound.py --check` is the drift gate. */")
    L.append("#ifndef LIBMH_HOST_IN_GEN_H")
    L.append("#define LIBMH_HOST_IN_GEN_H")
    L.append("")
    L.append("#include <stdint.h>")
    L.append("")
    L.append("#ifdef __cplusplus")
    L.append('extern "C" {')
    L.append("#endif")
    L.append("")
    L.append("#define LIBMH_HOST_IN_VERSION 0x%08Xu" % ver)
    L.append("")
    L.append(
        "/* ---- order ids (libmh_issue_order) ---------------------------------------------- */"
    )
    L.append("/* Each id names an owned order-issuing body; argv carries that body's committed")
    L.append(
        " * parameters, in declaration order, widened to int32_t. libmh builds the 68-byte wire"
    )
    L.append(
        " * record itself (orders/order_codec.h stays the single encoding), so a host names an"
    )
    L.append(" * INTENT and never an encoding. */")
    for i, r in enumerate(ords):
        n = len((r.get("proto") or {}).get("params", []))
        L.append("#define %-58s %2du /* %s, arity %d */" % (_ord_id(r["name"]), i, r["name"], n))
    L.append("#define %-58s %2du" % ("LIBMH_ORD_COUNT", len(ords)))
    L.append("")
    L.append(
        "/* ---- entry ids (libmh_in_unbound / the trap) ------------------------------------ */"
    )
    L.append("/* One per ENTRY this header owns, so the startup walk can report an entry whose")
    L.append(" * implementation is not wired BY NAME rather than letting it read as a working")
    L.append(" * entry that did nothing -- the hostapitest holey-table arm, applied to this")
    L.append(" * direction. Entries served by the REPLAY SPINE in libmh.h are listed below but")
    L.append(" * are NOT in this table: they are LIB-REF's to implement. */")
    ents = entry_names(rows)
    for i, e in enumerate(ents):
        n = len([r for r in covered if r["entry"] == e])
        L.append("#define %-58s %3du /* %d row%s */" % (_entry_id(e), i, n, "" if n == 1 else "s"))
    L.append("#define %-58s %3du" % ("LIBMH_IN_ENTRY_COUNT", len(ents)))
    L.append("")
    sp = spine_entries(rows)
    if sp:
        L.append("/* Rows served by the replay spine already declared in libmh.h (LIB-REF owns")
        L.append(" * their implementation, so they are outside this table and its walk): */")
        for e in sp:
            n = len([r for r in covered if r["entry"] == e + SPINE_SUFFIX])
            L.append("/*   %-38s %d row%s */" % (e, n, "" if n == 1 else "s"))
        L.append("")
    L.append(
        "/* The entry's stable name, or NULL past the end -- what the walk and the trap print. */"
    )
    L.append("const char *libmh_in_entry_name(uint32_t entry_id);")
    L.append("")
    L.append("#ifdef __cplusplus")
    L.append("} /* extern \"C\" */")
    L.append("#endif")
    L.append("")
    L.append("#endif /* LIBMH_HOST_IN_GEN_H */")
    return "\n".join(L) + "\n", ver


def render_dispatch(rows, ords):
    covered = sorted((r for r in rows if "entry" in r), key=lambda r: r["name"])
    headers = sorted({r["header"] for r in covered if r.get("header")})
    L = []
    L.append("// GENERATED by tools/gen_libmh_inbound.py -- DO NOT EDIT.")
    L.append("//")
    L.append("// The internal half of the inbound surface: one unpack thunk per order id, each")
    L.append(
        "// forwarding argv into the SAME live C++ wrapper the hosted promotion seam calls, and"
    )
    L.append("// the entry-name table. Included by exactly one TU (libmh_host_in.cpp).")
    L.append("//")
    L.append(
        "// The thunks are why `impl`/`header` in libmh_inbound_entries.json cannot rot: a wrong"
    )
    L.append("// symbol or a wrong arity is a COMPILE error here, not a lint warning.")
    L.append("#pragma once")
    L.append("#include <cstdint>")
    L.append("")
    L.append('#include "include/libmh_host_in.h" // the C entries whose addresses ENTRIES[] takes')
    L.append("")
    for h in headers:
        L.append('#include "%s"' % h)
    L.append("")
    L.append("namespace mh::libmh_in::gen {")
    L.append("")
    L.append(
        "// ---- order dispatch ---------------------------------------------------------------"
    )
    for r in ords:
        pr = r.get("proto") or {}
        params = pr.get("params", [])
        args = ", ".join("(%s)a[%d]" % (p.get("ctype", "int32_t"), i) for i, p in enumerate(params))
        ret = (pr.get("ret") or {}).get("ctype", "void")
        L.append("// %s -- %s" % (_ord_id(r["name"]), r["name"]))
        L.append("inline int32_t th_%s(const int32_t *a) {" % r["name"])
        L.append("    (void)a;")
        if ret == "void":
            L.append("    ::%s(%s);" % (r["impl"], args))
            L.append("    return 0;")
        else:
            L.append("    return (int32_t)::%s(%s);" % (r["impl"], args))
        L.append("}")
    L.append("")
    L.append("struct order_row {")
    L.append("    const char *name;")
    L.append("    uint32_t    arity;")
    L.append("    int32_t (*thunk)(const int32_t *);")
    L.append("};")
    L.append("")
    L.append("inline const order_row ORDERS[] = {")
    for r in ords:
        n = len((r.get("proto") or {}).get("params", []))
        L.append('    {"%s", %du, &th_%s},' % (r["name"], n, r["name"]))
    L.append("};")
    L.append("")
    L.append(
        "// ---- the entry table --------------------------------------------------------------"
    )
    L.append("// name + the ADDRESS OF THE C ENTRY ITSELF. The address is the point: an entry a")
    L.append("// row derives but nobody defined is a LINK error naming the symbol, and one nobody")
    L.append("// declared is a compile error -- neither can reach a run and read as working. The")
    L.append("// runtime table libmh_in_unbound walks is a MUTABLE copy of this, so a selftest can")
    L.append("// punch a hole (the hostapitest holey-table arm) without touching generated code.")
    L.append("struct entry_row {")
    L.append("    const char *name;")
    L.append("    const void *impl;")
    L.append("};")
    L.append("")
    L.append("inline const entry_row ENTRIES[] = {")
    for e in entry_names(rows):
        L.append('    {"%s", (const void *)&%s},' % (e, e))
    L.append("};")
    L.append("")
    L.append("} // namespace mh::libmh_in::gen")
    return "\n".join(L) + "\n"


# ---- the hosted routing census -----------------------------------------------------------------
#
# THE DECIDED MECHANISM (the LIB-REF plan sec 0.2) is that each covered row's HOSTED promotion
# seam calls the SAME inbound entry a standalone host would, so the hosted oracle -- the UI suite,
# the determinism run, the A/B soaks -- proves the inbound contract instead of a parallel copy of
# it. A row whose seam still calls the C++ wrapper directly is therefore not a style question: it
# is an entry whose only evidence is synthetic.
#
# So the routing is COUNTED here rather than described in a session report, and an unrouted row with
# no recorded reason FAILS. This is the BROKERED configuration's instrument: original host + libmh
# with no direct calls, as against `original` (no libmh) and `promoted` (libmh bodies reached
# directly). Two non-routed classes survive, both structural rather than excuses:
#
#   sequence            -- DERIVED, never written down: the entry covers SEVERAL bodies because one
#                          original body sequences them (libmh_strat_mode_init,
#                          libmh_session_globals_reset). A per-row seam cannot call a sequence entry
#                          without also executing its siblings, which is a behaviour change rather
#                          than a routing. The remedy, if the hosted proof is ever wanted, is per-body
#                          mirror entries with the sequence entry kept as their composition -- a
#                          surface change, so it is the user's call and not this tool's.
#   measured-impossible -- ADJUDICATED per row, in libmh_inbound_entries.json's `hosted` block, with
#                          a printed reason (see _hosted_classes there). LIB-SPINE-API replaced the
#                          blanket "entry ends with [spine] => excepted" rule with this, because that
#                          rule excused the one row that CAN route: llm_strat_sim_step's outer
#                          crossing IS libmh_sim_step, 1:1, and the blanket excused it by name shape
#                          rather than by measurement.
#
# THE DEFECT THE SAME ITEM FOUND HERE IS WORTH THE PARAGRAPH, because it is the shape a routing
# census fails in. `_seam_sites()` used to take the FIRST `<leaf>(...) {` in the file as the
# adapter's body. For MH_EXPORT_REPLACE(llm_strat_sim_step, mh::sim::promoted_arm::sim_step) that
# was mh::sim::detail::sim_step -- the 16,749-character translated sim step -- and it gave the
# RIGHT answer, because neither text mentioned an inbound entry. The row it misread is exactly the
# row this item routes, so the census would have reported the routed seam unrouted forever and its
# mutation arm would have proved nothing. Extraction is namespace-aware now (_adapter_body).

PROMOTE_GLOB = os.path.join(REPO, "src", "mh_dll", "**", "*.cpp")


# The brace that opens a namespace, recognised by what immediately PRECEDES it.
_NS_OPEN_RE = re.compile(r"\bnamespace(?:\s+([A-Za-z0-9_:]+))?\s*$")


def _ns_marks(text):
    """[(pos_of_brace, namespace_path_tuple_after_it)] -- one entry per brace, comments/strings skipped.

    Needed because the seam census has to read THE ADAPTER's body and nothing else, and a leaf name
    is not unique inside a file. Flattened (`namespace mh::sim {` contributes two segments) and
    anonymous namespaces contribute none, so a path compares against a symbol's qualifier directly.
    """
    marks, ns, ns_depth, depth, i, n = [], [], [], 0, 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            i = text.find("\n", i)
            if i < 0:
                break
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c in '"\'':
            q, i = c, i + 1
            while i < n and text[i] != q:
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue
        if c == "{":
            depth += 1
            m = _NS_OPEN_RE.search(text, max(0, i - 160), i)
            if m and m.end() == i:
                ns.extend([s for s in (m.group(1) or "").split("::") if s])
                ns_depth.append((depth, len([s for s in (m.group(1) or "").split("::") if s])))
            marks.append((i, tuple(ns)))
        elif c == "}":
            if ns_depth and ns_depth[-1][0] == depth:
                _d, k = ns_depth.pop()
                if k:
                    del ns[-k:]
            depth -= 1
            marks.append((i, tuple(ns)))
        i += 1
    return marks


def _ns_at(marks, pos):
    lo, hi, out = 0, len(marks) - 1, ()
    while lo <= hi:
        mid = (lo + hi) // 2
        if marks[mid][0] < pos:
            out, lo = marks[mid][1], mid + 1
        else:
            hi = mid - 1
    return out


def _match_body(text, open_brace_end):
    """The text between an opening `{` (index of the char AFTER it) and its matching `}`."""
    depth, i, n = 1, open_brace_end, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            i = text.find("\n", i)
            if i < 0:
                break
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c in '"\'':
            q, i = c, i + 1
            while i < n and text[i] != q:
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace_end:i]
        i += 1
    return text[open_brace_end:]


def _strip_comments(text):
    """Code only -- comments and string literals blanked, newlines kept so nothing joins up."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
            continue
        if c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(" " * (j - i))
            i = j
            continue
        out.append(c)
        i += 1
    return "".join(out)


def calls_entry(body, entry):
    """Does this adapter actually CALL `entry`? -- code only, and the row's OWN entry, not any libmh_.

    BOTH HALVES OF THAT WERE MEASURED DEFECTS, the second one caught by this item's own mutation arm.
    The test used to be `"libmh_" in body`:

      * over the RAW body, comments included -- so un-routing the seam while leaving a comment that
        explains the routing left the row reading `routed`. The mutation arm deleted the call, the
        census stayed green, and the arm would have certified an instrument that had stopped
        measuring. A substring test over prose is not a measurement.
      * against ANY libmh_ name -- so an adapter that called some OTHER entry, or merely mentioned
        one, satisfied it. The claim being made is "this seam reaches ITS entry", so that is what
        gets checked.
    """
    base = entry[: -len(SPINE_SUFFIX)] if entry.endswith(SPINE_SUFFIX) else entry
    return re.search(r"\b%s\s*\(" % re.escape(base), _strip_comments(body)) is not None


def _adapter_body(text, marks, sym):
    """The body of the function `sym` names, chosen by ITS ENCLOSING NAMESPACE, not by leaf name.

    THE DEFECT THIS REPLACES read the FIRST `<leaf>(...) {` in the file, and a leaf name is not
    unique: for MH_EXPORT_REPLACE(llm_strat_sim_step, mh::sim::promoted_arm::sim_step) that was
    `mh::sim::detail::sim_step` -- 16,749 characters of the translated sim step -- instead of the
    six-line adapter. It happened to give the right ANSWER (neither text mentions an inbound entry),
    which is the worst shape a broken instrument can have: the row it misreads is exactly the row
    LIB-SPINE-API routes, so a census that kept this would have reported the routed seam as
    unrouted forever, and a mutation arm over it would have proved nothing.
    """
    qual = [s for s in sym.rsplit("::", 1)[0].split("::") if s]
    leaf = sym.rsplit("::", 1)[-1]
    fallback = None
    for d in re.finditer(r"\b%s\s*\([^;{)]*\)\s*(?:const\s*)?\{" % re.escape(leaf), text):
        if fallback is None:
            fallback = d
        path = _ns_at(marks, d.start())
        if not qual or (len(path) >= len(qual) and list(path[-len(qual) :]) == qual):
            return _match_body(text, d.end()), True
    if fallback is None:
        return "", False
    return _match_body(text, fallback.end()), False


def _seam_sites():
    """{original_name: (module, adapter_symbol, adapter_body_text)} for every MH_EXPORT_REPLACE."""
    out = {}
    for path in glob.glob(PROMOTE_GLOB, recursive=True):
        try:
            text = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        if "MH_EXPORT_REPLACE(" not in text:
            continue
        mod = os.path.basename(path)
        marks = _ns_marks(text)
        for m in re.finditer(
            r"MH_EXPORT_REPLACE\(\s*([A-Za-z0-9_]+)\s*,\s*([A-Za-z0-9_:]+)\s*\)", text
        ):
            orig, sym = m.group(1), m.group(2)
            body, _exact = _adapter_body(text, marks, sym)
            out[orig] = (mod, sym, body)
    return out


def routing_census(rows):
    """(census_rows, findings). One row per COVERED body, classified by how its seam reaches libmh.

    ONE-ENTRY-MANY-ROWS IS TWO DIFFERENT THINGS and conflating them classified 45 routable rows as
    unroutable on this function's first run. `libmh_issue_order` covers 45 bodies but DISPATCHES on
    an id, so each row routes through it individually and nothing else executes -- routable. A
    SEQUENCE entry (libmh_strat_mode_init, libmh_session_globals_reset) covers several bodies
    because one original body runs them in order, so a per-row seam calling it would execute the
    siblings too. The discriminator is the order table's own membership, not the row count.
    """
    seams = _seam_sites()
    covered = [r for r in rows if "entry" in r]
    dispatched = {r["name"] for r in order_rows(rows)}
    # A row with a `hosted` disposition is NOT part of its entry's sequence weight. Counting it was
    # measurably wrong: three of libmh_sim_step's four rows are the frame driver's tail, adjudicated
    # measured-impossible, and counting them made the fourth -- llm_strat_sim_step, the one row this
    # whole item routes -- read as `exception: sequence`, i.e. excused for a reason that is not its
    # own. The sequence class means "one ORIGINAL body sequences these", which is a statement about
    # the rows that are still asking to be routed.
    per_entry = {}
    for r in covered:
        if r["name"] not in dispatched and not r.get("hosted"):
            per_entry[r["entry"]] = per_entry.get(r["entry"], 0) + 1

    out, findings = [], []
    for r in sorted(covered, key=lambda r: r["name"]):
        entry, name = r["entry"], r["name"]
        hosted = r.get("hosted")
        site = seams.get(name)
        mod = site[0] if site else "-"
        routed = bool(site) and calls_entry(site[2], entry)
        if routed:
            # A row that DOES route while claiming it cannot is the stale half of this adjudication,
            # and it reads exactly like a working row. Same failure mode, same treatment.
            if hosted:
                findings.append(
                    "STALE HOSTED DISPOSITION: %s is disposition'd hosted-%s but its adapter in %s "
                    "does call an inbound entry -- delete the hosted block, the row routes"
                    % (name, hosted.get("class", "?"), mod)
                )
            out.append((name, entry, mod, "routed", ""))
            continue
        if hosted:
            out.append(
                (name, entry, mod, "measured-impossible", "%s" % hosted.get("class", "?")),
            )
            continue
        if per_entry.get(entry, 0) > 1:
            out.append((name, entry, mod, "exception: sequence", ""))
            continue
        out.append((name, entry, mod, "UNROUTED", ""))
        findings.append(
            "UNROUTED SEAM: %s carries an MH_EXPORT_REPLACE in %s whose adapter does not call %s -- "
            "the hosted oracle cannot reach that entry, so route it or record in "
            "libmh_inbound_entries.json why it cannot be routed (a `hosted` block)"
            % (name, mod, entry.replace(SPINE_SUFFIX, ""))
            if site
            else "UNROUTED ROW: %s (%s) has no promotion seam and no `hosted` disposition -- record "
            "why the hosted configuration never crosses into it" % (name, entry)
        )
    return out, findings


def check_order_packing(rows):
    """Every routed order adapter packs argv as ITS OWN parameters, in declaration order.

    THE COMPILER CANNOT SEE THIS ONE. argv is `int32_t[]` at the boundary, so transposing two
    arguments compiles perfectly and passes libmh_issue_order's arity check -- the order is built
    from the wrong values, silently, on the determinism-critical path, and the first symptom would
    be a desync nobody could attribute. So the packing is checked structurally: the argv expressions
    must be the adapter's parameter names in order (modulo the int32_t widening and any cast the
    adapter itself already applied before forwarding), the id must be the row's own, and argc must
    equal the committed arity.
    """
    seams = _seam_sites()
    protos_ = protos()
    ids = {r["name"] for r in order_rows(rows)}
    bad = []
    for name in sorted(ids):
        site = seams.get(name)
        if site is None or not calls_entry(site[2], "libmh_issue_order"):
            continue  # unseamed, or not routed -- routing_census owns that complaint
        mod, sym, body = site
        leaf = sym.rsplit("::", 1)[-1]
        decl = re.search(r"\b%s\s*\(([^;{]*)\)\s*\{" % re.escape(leaf), open_text(mod, seams))
        params = []
        if decl:
            params = [
                p.strip().split()[-1].lstrip("*")
                for p in re.split(r",(?![^()]*\))", decl.group(1))
                if p.strip()
            ]
        arity = len((protos_.get(name) or {}).get("params", []))
        call = re.search(
            r"::libmh_issue_order\(\s*([A-Za-z0-9_]+)\s*,\s*([A-Za-z0-9_]+)\s*,\s*(\d+)u\s*\)", body
        )
        if not call:
            bad.append("%s: routed but no well-formed libmh_issue_order call in %s" % (name, mod))
            continue
        if call.group(1) != _ord_id(name):
            bad.append("%s: dispatches %s, but its id is %s" % (name, call.group(1), _ord_id(name)))
        if int(call.group(3)) != arity:
            bad.append(
                "%s: passes argc %s, committed arity is %d -- libmh_issue_order would refuse it "
                "with LIBMH_IN_E_ARITY at runtime and the order would never be issued"
                % (name, call.group(3), arity)
            )
        if arity == 0:
            continue
        a = re.search(r"const int32_t argv\[(\d+)\] = \{(.*?)\};", body, re.S)
        if not a:
            bad.append("%s: no argv initialiser to check" % name)
            continue
        got = []
        for x in a.group(2).split(","):
            y = x.strip().replace("(int32_t)", "", 1).strip()
            c = re.match(r"\(([A-Za-z0-9_]+)\)\s*(.+)$", y)
            got.append((c.group(2).strip() if c else y))
        if params and got != params:
            bad.append(
                "%s: argv packs %s but the adapter's parameters are %s -- a transposed argument "
                "here builds a wrong order record and nothing else would catch it"
                % (name, got, params)
            )
    return bad


_TEXT_CACHE = {}


def open_text(mod, seams):
    """The source text of the module a seam lives in (cached; keyed by basename)."""
    if mod not in _TEXT_CACHE:
        for path in glob.glob(PROMOTE_GLOB, recursive=True):
            if os.path.basename(path) == mod:
                _TEXT_CACHE[mod] = open(path, encoding="utf-8", errors="replace").read()
                break
        else:
            _TEXT_CACHE[mod] = ""
    return _TEXT_CACHE[mod]


# ---- report / main ----------------------------------------------------------------------------


def report(rows, ords, uncovered, ver):
    from collections import Counter

    covered = [r for r in rows if "entry" in r]
    excl = [r for r in rows if "exclusion" in r]
    L = []
    L.append("LIB-REF-IN inbound surface (derived live, cut-free, all domains)")
    L.append("  candidate rows          %d" % len(rows))
    L.append("  caller edges            %d" % sum(len(r["callers"]) for r in rows))
    L.append("  distinct callers        %d" % len({c for r in rows for c in r["callers"]}))
    L.append(
        "  covered by an entry     %d  (%d inbound entries + %d spine)"
        % (len(covered), len(entry_names(rows)), len(spine_entries(rows)))
    )
    L.append("  order ids               %d" % len(ords))
    L.append(
        "  excluded with a reason  %d  %s"
        % (len(excl), dict(Counter(r["exclusion"] for r in excl)))
    )
    L.append("  UNCOVERED               %d" % len(uncovered))
    L.append("  LIBMH_HOST_IN_VERSION   0x%08X" % ver)

    census, _ = routing_census(rows)
    kinds = Counter(k for _n, _e, _m, k, _x in census)
    why = {r["name"]: (r.get("hosted") or {}).get("why", "") for r in rows}
    L.append("")
    L.append("BROKERED ROUTING (hosted: each covered row's promotion seam -> the inbound entry)")
    L.append("  routed                  %d" % kinds.get("routed", 0))
    L.append("  exception: sequence     %d" % kinds.get("exception: sequence", 0))
    L.append("  measured-impossible     %d" % kinds.get("measured-impossible", 0))
    L.append("  UNROUTED                %d" % kinds.get("UNROUTED", 0))
    for kind in ("exception: sequence", "measured-impossible", "UNROUTED"):
        named = [(n, e, m, x) for n, e, m, k, x in census if k == kind]
        if not named:
            continue
        L.append("  -- %s --" % kind)
        for n, e, m, x in named:
            L.append("     %-46s %-36s %-24s %s" % (n, e, m, x))
            if kind == "measured-impossible" and why.get(n):
                for line in _wrap(why[n], 96):
                    L.append("          %s" % line)
    sp = spine_entries(rows)
    if sp:
        L.append("  -- spine entries, hosted reach --")
        for e in sp:
            hit = [
                (n, k) for n, ent, _m, k, _x in census if ent == e + SPINE_SUFFIX and k == "routed"
            ]
            L.append(
                "     %-46s %s"
                % (e, "ENTERED via %s" % ", ".join(n for n, _k in hit) if hit else "not reached")
            )
        L.append(
            "     %-46s not a crossing: hosted the lockstep hash is computed by the DETERMINISM"
            % "libmh_state_hash"
        )
        L.append(
            "     %-46s HARNESS (mh.dll instrument code), not by original code, so no census row"
            % ""
        )
    return "\n".join(L)


def _wrap(text, width):
    out, line = [], ""
    for w in text.split():
        if line and len(line) + 1 + len(w) > width:
            out.append(line)
            line = w
        else:
            line = (line + " " + w).strip()
    if line:
        out.append(line)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--check", action="store_true", help="fail if an output is stale or a row is uncovered"
    )
    args = ap.parse_args()

    cand, dead_findings, prune_candidates = candidates()
    rows, uncovered, findings = adjudicate(cand)
    findings = dead_findings + findings
    ords = order_rows(rows)
    findings += check_order_params(ords)
    findings += check_declarations(rows)
    findings += routing_census(rows)[1]
    findings += check_order_packing(rows)

    pub, ver = render_public(rows, ords)
    disp = render_dispatch(rows, ords)

    print(report(rows, ords, uncovered, ver))
    if prune_candidates:
        print()
        print(
            "PRUNE CANDIDATES (%d) -- callers measured ORPHANED with no reconciliation_pruned_roots.json\n"
            "row. Counted as LIVE here (the safe direction: they keep forcing their rows). Adjudicate\n"
            "or leave; this is a worklist, not a finding." % len(prune_candidates)
        )
        for c in prune_candidates:
            print("  - %s" % c)
    if findings:
        print()
        print("FINDINGS (%d):" % len(findings))
        for f in findings:
            print("  * %s" % f)

    stale = []
    for path, text in ((OUT_PUBLIC, pub), (OUT_DISPATCH, disp)):
        old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
        if old != text:
            stale.append(os.path.relpath(path, REPO).replace(os.sep, "/"))
            if not args.check:
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "w", encoding="utf-8", newline="\n") as fh:
                    fh.write(text)
                print("wrote %s" % path)

    if args.check:
        for s in stale:
            print("  * STALE OUTPUT: %s -- rerun gen_libmh_inbound.py" % s)
        if findings or stale:
            return 1
        print("ok: inbound surface is derived, complete and current")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
