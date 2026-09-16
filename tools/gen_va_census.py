#!/usr/bin/env python3
"""gen_va_census.py -- every VA call left in libmh, routed to an owning item, behind a ratchet.

WHAT IT GATES (tracker LIB-VA0). Every `mh::call::<name>` token in a migrated module is a
source-level dependency on original machine code being mapped at a fixed VA -- exactly what a
standalone libmh cannot have, and what LIB-REF's clause 1 asserts is absent. `gen_libmh_calls.py`
already MEASURES that surface. What it does not do is say WHO IS GOING TO REMOVE EACH SITE, and a
measure with no owner is how R9's direct-site class sat unowned for five days: the libmh rebind notes
said "a follow-up item owns them", LIB-REBIND's progress said "HANDED FORWARD", and no such item was
ever created. The count fell 104 -> 90 -> 31 on its own as translation proceeded, which is exactly
what a number with no owner looks like while it is still moving.

So this tool answers the routing question and then ratchets it:

  ROUTED    every site carries a class and an owning tracker item, derived from the committed
            census + adjudication ledger rather than listed here. The unrouted count is printed
            and must be ZERO -- a site with no owner is the finding, so it may not be silently
            bucketed into an "other" pile.
  RATCHETED --check fails when the total RISES (a new VA call landed) and equally when it FALLS
            (the baseline is stale). Monotone-down-only, re-recorded deliberately. A ratchet that
            only fails upward rots into a ceiling nobody lowers.

WHY THE OWNER IS DERIVED AND THE HANDOFFS ARE NOT. Every class falls out of the callee's own
adjudication class and migration status, so a callee that changes status re-routes itself. The four
save/load roots in HANDED_TO carry something the derivation cannot produce: not a different OWNER
(the derivation already sends them to LIFT-TABLE, since they are pre-ledger) but the REASON and the
SHAPE of the work -- the body was lent to the host on 2026-09-08, and three of the four sites are
one-line A/B trigger functions that should be MOVED to seams/ rather than converted. A later session
reading only the derived class would convert them and quietly destroy the two-arm trigger property
those functions exist for. So the handoff is a note attached to a route, not an override of it, and
it is reported separately for exactly that reason.

TWO MEASUREMENT TRAPS, both already paid for elsewhere in this tree:

  * `promoted` IS FALSE FOR EVERY PRE-LEDGER ROW. gen_libmh_calls.classify() hardcodes
    promoted=False for a name found by the install scan rather than in a manifest
    (gen_libmh_calls.py:170). So "translated, not promoted" contains rows that ARE promoted at
    runtime -- the save/load container roots among them. Routing on `promoted` alone would file
    them as unpromoted work. This tool separates PRE_LEDGER out and never calls it unpromoted.
  * A SECOND SCAN THAT DISAGREES WITH THE FIRST IS WORSE THAN NO SCAN. This file re-scans for
    file:line detail rather than reusing gen_libmh_calls' per-module aggregate, so `--check`
    asserts the two totals agree and fails naming both if they do not.

Usage:
    python tools/gen_va_census.py            # the routing report
    python tools/gen_va_census.py --check    # the ratchet + drift gate (tools/lint_repo.py)
    python tools/gen_va_census.py --record   # re-record the baseline after the total falls
    python tools/gen_va_census.py --sites    # every site, file:line, with its owner
    python tools/gen_va_census.py --selftest # prove each refusal fires, and does not over-fire
"""

from __future__ import annotations

import argparse
import collections
import copy
import io
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))

import _dllsrc  # noqa: E402
import gen_libmh_calls as GC  # noqa: E402
import gen_libmh_rebind as GR  # noqa: E402

OUT_PATH = os.path.join(REPO, "tools", "data", "va_census.json")
CALL_CENSUS = os.path.join(REPO, "tools", "data", "libmh_call_census.json")

BIND_RE = GC.BIND_RE
# A function DEFINITION in this tree starts at column 0. Good enough to name the enclosing function
# for the shadow-arm predicate, and it fails in the safe direction: a missed definition leaves `fn`
# pointing at the previous one, which can only ever make the predicate MISS a shadow site (routing
# it as ordinary work, which is visible) rather than silently exempt a real one.
FN_DEF_RE = re.compile(r"^[A-Za-z_][\w:<>,&*\s]*?\b(\w+)\s*\(")

# The modules that become libmh. seams/effects/hook/shadow are the injection layer and may hold VA
# calls forever -- they never link into the lib, so they are not this measure's business.
#
# The two tools this file joins define that set SEPARATELY, so they can silently drift apart and
# then measure different things while both look healthy. Refuse at import rather than reconcile:
# there is no right answer to "which module list is the real one", and a census whose scope moved
# under it is the failure this file's own two-scan cross-check exists to catch one level down.
if set(GR.MIGRATED_MODULES) != set(GC.MIGRATED_MODULES):
    sys.exit(
        "gen_va_census: gen_libmh_rebind and gen_libmh_calls disagree about which modules become "
        "libmh (%s vs %s). Fix the divergence; do not pick one here."
        % (sorted(GR.MIGRATED_MODULES), sorted(GC.MIGRATED_MODULES))
    )
MIGRATED_MODULES = GR.MIGRATED_MODULES

# ---- routing ------------------------------------------------------------------------------------
#
# Owner per class. Derived from the callee's adjudication class and migration status; see the
# module docstring for why HANDED_TO is the one exception.
CLASS_OWNER = {
    "crt": "LIB-CRT",
    "dead": "LIFT-TABLE",
    "cfg-snapshot": "LIB-BOOT",
    "mixed": "LIFT-TABLE",
    "promoted": "LIFT-TABLE",
    "pre-ledger": "LIFT-TABLE",
    "unpromoted": "LIB-VA-PROMOTE",
    "shadow-arm": "LIB-VA0",
    "harness-tramp": "LIFT-TABLE",
    # A KEEP, not work. LIB-VA0 owns it because LIB-VA0 is the item whose clause 4 says a survivor
    # may stay iff it carries a recorded reason that is PRINTED beside the zero -- so the keep and
    # the instrument that prints it belong to the same item. Routing it to the item that would
    # "fix" it would be wrong twice: there is no fix, and the owner named would be closed.
    "constsig-keep": "LIB-VA0",
}

CLASS_WHY = {
    "harness-tramp": "an A/B harness call through a RUNTIME TRAMPOLINE pointer, not a committed "
    "callee -- it runs the original's relocated prologue, so standalone cannot execute it either. "
    "Uncounted until 2026-09-09 because the shape names no callee; the disposition is to move the "
    "A/B machinery out of libmh rather than to convert the call",
    "crt": "the statically-linked Watcom CRT -- LIB-CRT vendors a bit-equivalent set for the "
    "standalone build",
    "dead": "a seam the DLL supplies, not original game logic -- to the host API "
    "(`dead` is the wrong class for a live one)",
    "cfg-snapshot": "written by the cfg parser -- LIB-BOOT imports it as a snapshot instead",
    "mixed": "adjudication `mixed` -- needs a settled class before it can be routed further",
    "promoted": "our body is promoted, so hosted the entry E9 lands in ours; the site is a VA "
    "dependency ONLY in the standalone build",
    "pre-ledger": "translated before the ledgers existed, so `promoted` is not measured for it "
    "(gen_libmh_calls.py:170) -- classify by hand at LIFT-TABLE, do not assume unpromoted",
    "unpromoted": "we have a verified C++ body and still call the original through the VA",
    "shadow-arm": "the SHADOW arm deliberately calling the original for per-call fidelity -- its "
    "live sibling in the same file already binds ours. NOT promotion work: the fix is that the "
    "shadow arm must not be compiled into the standalone lib, where no original exists",
    "constsig-keep": "KEPT, with the reason recorded (LIB-CONSTSIG 2026-09-04): the calls-struct "
    "slot is const and our body is correctly non-const, so there is no conversion to make -- see "
    "the KEPT table for the full adjudication",
}

# The one hand-routed set: the user's 2026-09-08 call that the save/load roots cross the host API
# rather than being kept as direct VA calls. Not derivable -- the derivation only knows the body is
# translated, not that its OWNER was lent to the host.
HANDED_TO = {
    "map_SavePlanetToDisk": (
        "LIFT-TABLE",
        "save/load lent to the HOST; libmh reaches a host-owned body across the `io` group. NOTE "
        "the row itself IS bound (target mh::save::promoted_save_planet) -- only save_live's extra "
        "DIRECT sites bypass it, and the two are NOT the same job: :726 is inside "
        "run_ours_savegame, real container-save logic on the closure boundary, and crosses the "
        "`io` group; :1671 is save_planet_now, an A/B trigger that MOVES to seams/ instead.",
    ),
    "map_LoadPlanetFromDisk": (
        "LIFT-TABLE",
        "as map_SavePlanetToDisk; the row is bound (mh::save::promoted_load_planet), the apply-phase "
        "site at save_live.cpp:812 is the closure boundary and crosses the `io` group.",
    ),
    "game_SaveGame": (
        "LIFT-TABLE",
        "not a rebind row at all. Its only site is savegame_now (save_live.cpp:1661), a one-line A/B "
        "trigger routed through the game's ENTRY on purpose so one trigger exercises both arms -- "
        "harness work in a libmh module; move the function to seams/, do not convert the call.",
    ),
    "llm_game_load": (
        "LIFT-TABLE",
        "as game_SaveGame: loadgame_now (save_live.cpp:1656) is an A/B trigger, not sim logic.",
    ),
}


# ---- KEPT: sites that stay, with a recorded reason (LIB-VA0 clause 4) --------------------------
#
# Clause 4 reads "zero OR every surviving site carries a recorded keep-reason and the count of such
# keeps is printed beside the zero; an unprinted keep list is the failure mode". This is that list,
# and it is hand-routed for the same reason HANDED_TO is: the derivation can see that the body is
# translated and promoted, which is true and which is why these two used to read as `promoted` work
# owned by LIFT-TABLE. What it cannot see is that the conversion is IMPOSSIBLE rather than pending,
# and that LIFT-TABLE closed 2026-09-09 -- so the honest output is a keep, not an open row against a
# closed item.
#
# The byte-scan carries the same adjudication from the other end: tools/data/libmh_va_adjudication.json
# holds three `libmh-constsig-residue` occurrences for 0x00466790 (rx_dispatch.obj x1,
# tx_emit_ctrl.obj x2), which are these two sites. The two instruments must agree about a keep or one
# of them is excusing something the other is still counting.
KEPT = {
    "llm_strat_order_pending_enqueue": (
        "constsig-keep",
        "LIB-CONSTSIG, adjudicated 2026-09-04. The calls-struct slot is "
        "`int32_t (*)(const game_order *)`, inherited from the generated prototype, while "
        "mh::orders::pending_enqueue takes `order *` NON-const -- and the non-const is the CORRECT "
        "signature, not an oversight: the body masks four ushort fields of *rec to their low byte IN "
        "PLACE before copying, exactly as the original does at 0x004667b4-0x004667c6. So the const on "
        "the slot is the wrong half, and changing it would change a HOSTED calls-struct member type. "
        "Rediscovered independently at LIB-REF-SPLIT 2026-09-11 when MH_PROMOTED's type check refused "
        "the conversion; gen_libmh_rebind.py prints the same finding as the row's [semantics] reason. "
        "Cross-referenced: libmh_va_adjudication.json class `libmh-constsig-residue`, 3 occurrences "
        "of 0x00466790.",
    ),
}


# The `pre-ledger` hand classification LIFT-TABLE owes (docs/libmh-abi.md sec 6, S6 clause 4). Each
# entry is a checked fact about the tree, not a judgement: the named MH_EXPORT_REPLACE is the entry
# seam that makes the hosted call land in our body, which is exactly what `promoted` means for every
# row the derivation CAN measure.
PRE_LEDGER_HAND = {
    # order_queue.cpp:702 / :704 -- the RI-ORDERS container, promoted since the O2 closure.
    "llm_strat_order_pending_enqueue": "promoted",
    "llm_strat_order_release_due": "promoted",
    # tx_emit_order.cpp:125 -- the order TX seam, promoted alongside them.
    "llm_net_send_order": "promoted",
}


def _owner_for_class(cls):
    """-> the owning item for a routing class, or None.

    `host-callback:<group>` is matched by PREFIX rather than listed: LIB-ABI's adjudication opens a
    new group whenever one is needed (utils_abort became `host-callback:fatal` on 2026-09-08), and a
    per-group entry here would leave every future group unrouted for no reason -- they all land at
    LIFT-TABLE, which is the item that regroups the required services and owns the table.
    """
    if cls and cls.startswith("host-callback"):
        return "LIFT-TABLE"
    return CLASS_OWNER.get(cls)


def _class_of(rec, site=None):
    """-> the routing class for one callee record from libmh_call_census.json.

    `site` is consulted FIRST because one class is a property of the SITE, not the callee. A
    two-arm calls struct binds ours in live_calls() and calls the ORIGINAL in shadow_calls(), and
    the shadow arm's call is the entire point of shadow mode -- converting it would delete the
    oracle. Routing on the callee alone cannot see that: those callees carry SHADOW seams and no
    entry seam, so `promoted` is False and all 15 of them read as unpromoted promotion work.
    Which is what this tool reported on its first run, and what a session would then have gone and
    "fixed".
    """
    if site is not None and site.get("shadow_arm"):
        return "shadow-arm"
    if rec["status"] == "original":
        # Every no-body callee is adjudicated by LIB-ABI; an unadjudicated one is unroutable on
        # purpose rather than defaulted, because that is the case worth seeing.
        return rec.get("adjudication")
    if rec["status"] != "translated":
        return None  # `planned` / `dead`-status rows are not this measure's shape
    if rec.get("domain") == "(pre-ledger)":
        # HAND-CLASSIFIED at LIFT-TABLE (2026-09-09), which is what the `pre-ledger` class asked for
        # in its own why-text: it is not a disposition, it is an instruction to go and look, because
        # `promoted` is not MEASURED for a body translated before the ledgers existed and defaulting
        # such a row to `unpromoted` would invent promotion work. All three surviving callees carry an
        # MH_EXPORT_REPLACE, checked in the tree rather than inferred -- see PRE_LEDGER_HAND.
        return PRE_LEDGER_HAND.get(rec["name"], "pre-ledger")
    return "promoted" if rec.get("promoted") else "unpromoted"


def route(rec, site=None):
    """-> (owner, cls, why, handed, kept). owner is None when the site cannot be routed."""
    cls = _class_of(rec, site)
    if cls == "shadow-arm":
        return CLASS_OWNER[cls], cls, CLASS_WHY[cls], False, False
    # BEFORE the derivation, and before HANDED_TO: a keep is a settled END STATE, so nothing derived
    # can overrule it. Checked on the callee, like HANDED_TO, because the adjudication is a property
    # of the callee's signature rather than of any one call site.
    if rec["name"] in KEPT:
        cls, why = KEPT[rec["name"]]
        return CLASS_OWNER[cls], cls, why, False, True
    if rec["name"] in HANDED_TO:
        owner, why = HANDED_TO[rec["name"]]
        return owner, cls, "HANDED (user 2026-09-08): " + why, True, False
    if cls is None:
        return None, None, None, False, False
    why = CLASS_WHY.get(cls)
    if why is None and cls.startswith("host-callback"):
        why = (
            "an adjudicated HOST CALLBACK (%s) -- libmh reaches it across the host API, so it is "
            "LIFT-TABLE's to group and route, not a body anyone vendors or promotes" % cls
        )
    return _owner_for_class(cls), cls, why, False, False


# ---- scanning -----------------------------------------------------------------------------------


def scan_sites():
    """-> [{file, line, callee, module, shape}] for every mh::call:: site in a migrated module.

    `shape` is `binder` (a calls-struct member, which the rebind binder can see) or `direct` (an
    inline call expression, which it cannot -- R9). The split matters because it says whether a
    site is fixable by binding or only by editing the call.
    """
    out = []
    for path in GR.walk_sources((".cpp", ".h"), skip=("attic", "addr")):
        mod = GR.module_of(path)
        if mod not in MIGRATED_MODULES:
            continue
        rel = _dllsrc.rel_or_raise(path)
        text = GR.strip_comments(io.open(path, encoding="utf-8", errors="replace").read())
        # Everything this TU binds to OUR body. Half of the shadow-arm predicate: a callee with no
        # live binding in this file is not one arm of a two-arm calls struct.
        bound_here = set(BIND_RE.findall(text))
        fn = None
        # The MH_LIBMH_BUILD preprocessor state. A site inside a region the standalone build does not
        # compile is NOT part of libmh's VA surface, and counting it makes the measure unable to
        # record the one fix that most reliably removes a block of sites. Tracked as a stack so
        # nesting works; `excl` is the count of enclosing levels that exclude the standalone build.
        # Each level is (is_a_MH_LIBMH_BUILD_condition, this_branch_is_excluded_from_libmh). The
        # first half matters at #else: flipping an UNRELATED condition's branch would wrongly
        # exclude code that libmh does compile.
        cond = []
        for i, line in enumerate(text.splitlines(), 1):
            m = FN_DEF_RE.match(line)
            if m:
                fn = m.group(1)
            stripped = line.lstrip()
            if stripped.startswith("#"):
                d = stripped[1:].lstrip()
                if d.startswith("ifndef MH_LIBMH_BUILD"):
                    cond.append((True, True))  # the !defined branch: hosted only
                elif d.startswith("ifdef MH_LIBMH_BUILD"):
                    cond.append((True, False))  # the defined branch: libmh only
                elif d.startswith(("ifdef", "ifndef", "if")):
                    cond.append((False, False))  # unrelated: assume compiled in both
                elif d.startswith("else") and cond:
                    is_libmh, excl = cond[-1]
                    cond[-1] = (is_libmh, (not excl) if is_libmh else False)
                elif d.startswith("endif") and cond:
                    cond.pop()
                continue  # a #define's mh::call::PARAM is a macro parameter, not a callee
            guarded = any(excl for _, excl in cond)
            # The trampoline form FIRST, because ANY_CALL's `!= "detail"` filter erases it and the
            # site would otherwise vanish -- four of them did, in save/save_live.cpp, until
            # 2026-09-09. It executes original machine code through a runtime pointer, so it is a
            # VA site; the "callee" recorded is the trampoline variable, which is what a reader
            # needs to find it.
            for tramp in GR.TRAMP_CALL.findall(line):
                out.append(
                    {
                        "file": rel,
                        "line": i,
                        "callee": "detail::" + tramp,
                        "module": mod,
                        "shape": "tramp",
                        "fn": fn,
                        "shadow_arm": False,
                        "guarded": guarded,
                    }
                )
            names = [n for n in GR.ANY_CALL.findall(line) if n != "detail"]
            if not names:
                continue
            binder = GR.BINDER.match(line.rstrip()) is not None or bool(
                GR.BINDER_CALL.findall(line)
            )
            for n in names:
                out.append(
                    {
                        "file": rel,
                        "line": i,
                        "callee": n,
                        "module": mod,
                        "shape": "binder" if binder else "direct",
                        "fn": fn,
                        # BOTH halves are required and neither alone would do: "shadow" in an
                        # enclosing name could be an unrelated helper, and a live binding elsewhere
                        # in a large TU could be coincidence.
                        "shadow_arm": bool(fn and "shadow" in fn.lower() and n in bound_here),
                        "guarded": guarded,
                    }
                )
    out.sort(key=lambda r: (r["file"], r["line"], r["callee"]))
    return out


def load_call_census():
    if not os.path.exists(CALL_CENSUS):
        sys.exit("gen_va_census: %s missing -- run gen_libmh_calls.py first" % CALL_CENSUS)
    doc = json.load(io.open(CALL_CENSUS, encoding="utf-8"))
    return {r["name"]: r for r in doc["callees"]}, doc["summary"]


def build():
    """-> the census document."""
    sites = scan_sites()
    by_name, summary = load_call_census()

    rows = []
    unrouted = []
    # Sites the standalone build does not compile. NOT silently dropped: they are the single most
    # effective way to remove a block of VA sites (LIB-VA0 took 32 out this way), so the count is
    # reported and it is what reconciles this scan with gen_libmh_calls', which counts SOURCE sites
    # and knows nothing about the guards.
    guarded = [s for s in sites if s.get("guarded")]
    sites = [s for s in sites if not s.get("guarded")]
    for s in sites:
        if s.get("shape") == "tramp":
            # No committed callee to look up: the target is a runtime pointer. Routed by SHAPE, and
            # to LIFT-TABLE, which owns save_live's residue -- see docs/libmh-abi.md section 6, S6.
            rows.append(
                dict(
                    s,
                    owner="LIFT-TABLE",
                    cls="harness-tramp",
                    why=CLASS_WHY["harness-tramp"],
                    handed=False,
                )
            )
            continue
        rec = by_name.get(s["callee"])
        if rec is None:
            unrouted.append(dict(s, why="callee absent from libmh_call_census.json"))
            continue
        owner, cls, why, handed, kept = route(rec, s)
        if owner is None:
            unrouted.append(
                dict(s, why="no routing class (adjudication %r)" % rec.get("adjudication"))
            )
            continue
        rows.append(dict(s, owner=owner, cls=cls, why=why, handed=handed, kept=kept))

    by_owner = collections.Counter(r["owner"] for r in rows)
    by_class = collections.Counter(r["cls"] for r in rows)
    by_shape = collections.Counter(r["shape"] for r in rows)
    by_module = collections.Counter(r["module"] for r in rows)

    return {
        "_generated_by": "tools/gen_va_census.py -- do not hand-edit; regenerate instead",
        "_measures": (
            "every mh::call:: site in the modules that become libmh (%s), routed to the tracker "
            "item that will remove it. LIB-VA0's instrument." % " ".join(sorted(MIGRATED_MODULES))
        ),
        "summary": {
            # Counted over rows AND guarded: gen_libmh_calls sees neither, so guarding a tramp
            # site must not silently move it from one side of the reconciliation to the other.
            "tramp_sites": sum(1 for r in list(rows) + list(guarded) if r.get("shape") == "tramp"),
            "total_sites": len(rows) + len(unrouted),
            "routed": len(rows),
            # LIB-VA0 clause 4's two halves, separated so the headline can print them apart: a KEPT
            # site is a settled end state with a recorded reason, not an open row. `needing_work` is
            # the number the item drives to zero; `kept` is the number that must stay PRINTED.
            "kept": sum(1 for r in rows if r.get("kept")),
            "needing_work": sum(1 for r in rows if not r.get("kept")),
            "unrouted": len(unrouted),
            "guarded_out": len(guarded),
            "by_owner": dict(sorted(by_owner.items())),
            "by_class": dict(sorted(by_class.items())),
            "by_shape": dict(sorted(by_shape.items())),
            "by_module": dict(sorted(by_module.items())),
            # The cross-check: gen_libmh_calls reaches the same number by a different scan.
            "call_census_standalone_sites": summary["config_measures"]["standalone"]["sites"],
        },
        "sites": rows,
        "unrouted_sites": unrouted,
        "guarded_sites": guarded,
    }


# ---- the ratchet --------------------------------------------------------------------------------


def _site_key(r):
    return "%s:%d:%s" % (r["file"], r["line"], r["callee"])


def _wrap(text, width):
    out, line = [], ""
    for word in text.split():
        if line and len(line) + 1 + len(word) > width:
            out.append(line)
            line = word
        else:
            line = (line + " " + word).strip()
    if line:
        out.append(line)
    return out


def compare(cur, committed):
    """-> list of failure strings. Empty means green."""
    fails = []
    c_sum, b_sum = cur["summary"], committed["summary"]

    if cur["unrouted_sites"]:
        fails.append(
            "%d site(s) have NO OWNING ITEM. A site with no owner is the finding this gate "
            "exists to surface:\n%s"
            % (
                len(cur["unrouted_sites"]),
                "\n".join(
                    "    %-52s %s" % (_site_key(r), r["why"]) for r in cur["unrouted_sites"][:20]
                ),
            )
        )

    # The two scans reconcile through the guarded count, not by being equal: gen_libmh_calls counts
    # SOURCE sites and knows nothing about MH_LIBMH_BUILD, this one counts sites the standalone
    # build actually compiles. Checking the SUM is what keeps the difference explainable -- an
    # unexplained gap means one scan drifted, which is the failure a second scan exists to catch.
    # The tramp term, added 2026-09-09 with the shape itself: gen_libmh_calls is a per-CALLEE
    # census and a trampoline call names no callee, so it cannot count one. Subtracting it here --
    # rather than teaching that scan to invent a pseudo-callee and demand an adjudication row for
    # our own harness machinery -- keeps each tool measuring its own thing. The count is printed on
    # every run so the term can never quietly absorb a real drift.
    reconciled = c_sum["total_sites"] + c_sum.get("guarded_out", 0) - c_sum.get("tramp_sites", 0)
    if reconciled != c_sum["call_census_standalone_sites"]:
        fails.append(
            "TWO SCANS DISAGREE: this file counts %d in-libmh + %d guarded-out = %d, "
            "gen_libmh_calls' standalone measure counts %d source site(s). The difference must be "
            "exactly the guarded ones; anything else means a scan drifted, and neither number may "
            "be quoted until they agree. (Regenerate libmh_call_census.json first -- it may simply "
            "be stale.)"
            % (
                c_sum["total_sites"],
                c_sum.get("guarded_out", 0),
                reconciled,
                c_sum["call_census_standalone_sites"],
            )
        )

    cur_keys = {_site_key(r) for r in cur["sites"]} | {_site_key(r) for r in cur["unrouted_sites"]}
    old_keys = {_site_key(r) for r in committed["sites"]} | {
        _site_key(r) for r in committed["unrouted_sites"]
    }
    added = sorted(cur_keys - old_keys)
    removed = sorted(old_keys - cur_keys)

    if c_sum["total_sites"] > b_sum["total_sites"]:
        fails.append(
            "THE VA SURFACE ROSE: %d -> %d site(s). libmh may not gain a call into original code "
            "at a fixed VA. New site(s):\n%s"
            % (
                b_sum["total_sites"],
                c_sum["total_sites"],
                "\n".join("    %s" % k for k in added[:20]) or "    (moved, not added)",
            )
        )
    elif c_sum["total_sites"] < b_sum["total_sites"]:
        fails.append(
            "THE BASELINE IS STALE: %d -> %d site(s). The surface FELL, which is the point -- "
            "re-record it with `python tools/gen_va_census.py --record` so the ratchet tightens. "
            "A ratchet that only fails upward becomes a ceiling nobody lowers.\n%s"
            % (
                b_sum["total_sites"],
                c_sum["total_sites"],
                "\n".join("    -%s" % k for k in removed[:20]),
            )
        )
    elif added or removed:
        fails.append(
            "SITES MOVED without the total changing (%d added, %d removed). Re-record to keep the "
            "committed site list usable for naming a regression:\n%s"
            % (
                len(added),
                len(removed),
                "\n".join(
                    ["    +%s" % k for k in added[:10]] + ["    -%s" % k for k in removed[:10]]
                ),
            )
        )

    return fails


# ---- reporting ----------------------------------------------------------------------------------


def report(doc, show_sites=False):
    s = doc["summary"]
    print(
        "VA CENSUS: %d mh::call:: site(s) in the modules that become libmh -- %d needing work, "
        "%d printed keep(s), %d unrouted."
        % (s["total_sites"], s.get("needing_work", s["routed"]), s.get("kept", 0), s["unrouted"])
    )
    print(
        "  by shape: %s   (binder = a calls-struct member the rebind binder can see; direct = an "
        "inline call it cannot, R9)"
        % ", ".join("%s %d" % kv for kv in sorted(s["by_shape"].items()))
    )
    print(
        "  guarded out of the standalone build: %d site(s) -- counted by gen_libmh_calls' source "
        "scan, not compiled into libmh" % s.get("guarded_out", 0)
    )
    print("  by module: %s" % ", ".join("%s %d" % kv for kv in sorted(s["by_module"].items())))
    print()
    seen = collections.Counter()
    for r in doc["sites"]:
        if not r.get("handed") and not r.get("kept"):
            seen[(r["owner"], r["cls"])] += 1
    # Only when there IS work. An empty table under a header reads like a report that failed to
    # render; "0 needing work" in the headline plus the printed keep list below is the whole story.
    if seen:
        print("  %-16s %-14s %5s  %s" % ("owner", "class", "sites", "why"))
    for (owner, cls), n in sorted(seen.items(), key=lambda kv: (-kv[1], kv[0])):
        why = next(
            r["why"]
            for r in doc["sites"]
            if r["owner"] == owner and r["cls"] == cls and not r.get("handed") and not r.get("kept")
        )
        print("  %-16s %-14s %5d  %s" % (owner, cls, n, why[:96]))
    print()
    for owner, n in sorted(s["by_owner"].items(), key=lambda kv: -kv[1]):
        print("  TOTAL %-16s %4d site(s)" % (owner, n))
    # LIB-VA0 clause 4: the keeps are PRINTED, every run, with their reason -- an unprinted keep
    # list is the failure mode the clause names, so this block is not conditional on a verbosity
    # flag and does not truncate the reason.
    kept = [r for r in doc["sites"] if r.get("kept")]
    if kept:
        print()
        print(
            "  KEPT with a recorded reason (%d site(s)) -- NOT open work; clause 4's printed keep "
            "list:" % len(kept)
        )
        for callee in sorted({r["callee"] for r in kept}):
            rs = [r for r in kept if r["callee"] == callee]
            print(
                "    %-38s -> %-14s %s"
                % (callee, rs[0]["cls"], ", ".join(_site_key(r) for r in rs))
            )
            for chunk in _wrap(rs[0]["why"], 92):
                print("        %s" % chunk)
    handed = [r for r in doc["sites"] if r.get("handed")]
    if handed:
        # Reported separately because the derived class does not carry the SHAPE of the work, and a
        # session that reads only the class would convert an A/B trigger it should be relocating.
        print()
        print(
            "  HANDED by decision (%d site(s)) -- the route is derived, the REASON is not:"
            % len(handed)
        )
        for callee in sorted({r["callee"] for r in handed}):
            rs = [r for r in handed if r["callee"] == callee]
            print(
                "    %-38s -> %-12s %s"
                % (callee, rs[0]["owner"], ", ".join(_site_key(r) for r in rs))
            )
            for chunk in _wrap(rs[0]["why"], 92):
                print("        %s" % chunk)
    if doc["unrouted_sites"]:
        print()
        print("  UNROUTED (%d) -- each is a finding:" % len(doc["unrouted_sites"]))
        for r in doc["unrouted_sites"]:
            print("    %-52s %s" % (_site_key(r), r["why"]))
    if show_sites:
        print()
        print("  %-46s %-6s %-42s %-16s %s" % ("file", "line", "callee", "owner", "shape"))
        for r in doc["sites"]:
            print(
                "  %-46s %-6d %-42s %-16s %s"
                % (r["file"], r["line"], r["callee"], r["owner"], r["shape"])
            )


# ---- selftest -----------------------------------------------------------------------------------


def selftest():
    """Prove each refusal fires -- and, at the end, that a clean tree fires none of them."""
    base = build()
    if base["unrouted_sites"]:
        print("selftest: FAIL -- the live tree already has unrouted sites; fix those first")
        return 1

    cases = []

    def case(desc, mutate, expect):
        cur = copy.deepcopy(base)
        committed = copy.deepcopy(base)
        mutate(cur, committed)
        fails = compare(cur, committed)
        hit = any(expect in f for f in fails)
        cases.append((desc, hit, expect, fails))

    def _add_site(cur, _committed):
        cur["sites"].append(
            {
                "file": "sim/made_up.cpp",
                "line": 1,
                "callee": "llm_made_up",
                "module": "sim",
                "shape": "direct",
                "owner": "LIB-VA-PROMOTE",
                "cls": "unpromoted",
                "why": "x",
            }
        )
        cur["summary"]["total_sites"] += 1
        cur["summary"]["call_census_standalone_sites"] += 1

    case("a new VA call in a migrated module is refused", _add_site, "THE VA SURFACE ROSE")

    def _drop_site(cur, _committed):
        cur["sites"].pop()
        cur["summary"]["total_sites"] -= 1
        cur["summary"]["call_census_standalone_sites"] -= 1

    case("a FALLEN total is refused until re-recorded", _drop_site, "THE BASELINE IS STALE")

    def _move_site(cur, _committed):
        cur["sites"][0] = dict(cur["sites"][0], line=cur["sites"][0]["line"] + 1000)

    case("sites that MOVED without a count change are refused", _move_site, "SITES MOVED")

    def _unroute(cur, _committed):
        r = cur["sites"].pop()
        cur["unrouted_sites"].append(dict(r, why="synthetic"))
        cur["summary"]["routed"] -= 1
        cur["summary"]["unrouted"] += 1

    case("a site with no owning item is refused", _unroute, "NO OWNING ITEM")

    def _desync(cur, _committed):
        cur["summary"]["call_census_standalone_sites"] += 7

    case("a disagreement with gen_libmh_calls is refused", _desync, "TWO SCANS DISAGREE")

    # THE SHADOW-ARM PREDICATE NEEDS BOTH HALVES, and it exempts a site from ordinary work, so an
    # over-firing one hides a real VA call. Tested directly rather than through compare(): each half
    # alone must NOT classify, and the live tree must actually contain some (a predicate that
    # matches nothing would pass both negative arms while being broken).
    rec = {"name": "x", "status": "translated", "domain": "sim", "promoted": False}
    half_a = {"fn": "shadow_calls", "shadow_arm": False}  # named shadow, no live binding here
    half_b = {"fn": "live_calls", "shadow_arm": False}  # bound here, not the shadow builder
    cases.append(
        (
            "shadow-arm needs BOTH halves -- neither alone classifies",
            _class_of(rec, half_a) != "shadow-arm" and _class_of(rec, half_b) != "shadow-arm",
            "not shadow-arm",
            [],
        )
    )
    # THE KEEP CLASS NEEDS A POSITIVE ARM TOO, and a live one, because a keep that stopped being
    # routed would silently become ordinary work owned by a CLOSED item -- which is the exact state
    # this class was introduced to end. Two assertions, because either alone can rot: the class must
    # actually appear in the live tree, and every site carrying it must carry a reason (clause 4's
    # requirement is the reason, not the label).
    kept_rows = [r for r in base["sites"] if r.get("kept")]
    cases.append(
        (
            "the keep class is LIVE in the tree (clause 4's printed keeps exist)",
            len(kept_rows) > 0,
            "kept rows present",
            [],
        )
    )
    cases.append(
        (
            "every kept site carries a recorded reason and a keep owner",
            all(r.get("why") and r.get("owner") for r in kept_rows),
            "kept rows reasoned",
            [],
        )
    )

    # The POSITIVE arm, synthetic since LIB-VA0 guarded the ai shadow arm out and the class now has
    # no live instances. Testing it against the tree would have made this arm decay into a tautology
    # the day the work it measures succeeded -- which is exactly when a predicate stops being
    # exercised and starts rotting.
    cases.append(
        (
            "shadow-arm classifies when both halves hold (synthetic: the class has no live "
            "instances since the guard)",
            _class_of(rec, {"fn": "shadow_calls", "shadow_arm": True}) == "shadow-arm",
            "shadow-arm",
            [],
        )
    )
    # And the guarded-out accounting must reconcile, or the drop is silent. The tramp term is
    # subtracted for the reason given at the reconciliation itself: gen_libmh_calls counts CALLEES
    # and a trampoline call names none.
    cases.append(
        (
            "guarded-out sites are counted, not dropped (%d today)"
            % base["summary"].get("guarded_out", 0),
            base["summary"]["total_sites"]
            + base["summary"].get("guarded_out", 0)
            - base["summary"].get("tramp_sites", 0)
            == base["summary"]["call_census_standalone_sites"],
            "in-libmh + guarded - tramp == source scan",
            [],
        )
    )
    # THE TRAMPOLINE SHAPE (2026-09-09). Three arms, because this detection was added to close a
    # blind spot and a detector that matches nothing would look exactly like a fixed one: it must
    # match the real form, must NOT match an ordinary named call (or every site would double-count),
    # and the live tree must still contain instances -- when it stops containing them the work is
    # done and this arm should be replaced by a synthetic one rather than deleted, the way the
    # shadow-arm arm above was.
    cases.append(
        (
            "tramp shape matches a call through a trampoline pointer",
            GR.TRAMP_CALL.findall(
                "return mh::call::detail::s_u32_EAX_EDX("
                "reinterpret_cast<uintptr_t>(g_load_tramp), planet_index, mode);"
            )
            == ["g_load_tramp"],
            "captures the trampoline",
            [],
        )
    )
    cases.append(
        (
            "tramp shape does NOT match an ordinary named call",
            GR.TRAMP_CALL.findall("mh::call::llm_game_load(save_name);") == [],
            "no false positive on a named callee",
            [],
        )
    )
    cases.append(
        (
            "the tramp class has live instances (%d today) -- a detector matching nothing "
            "passes both arms above while being broken" % base["summary"].get("tramp_sites", 0),
            base["summary"].get("tramp_sites", 0) > 0,
            "tramp sites present",
            [],
        )
    )

    # THE OVER-REFUSAL ARM. Every arm above mutates something; this one mutates nothing, and a gate
    # that cannot stay quiet on an unchanged tree would make every one of the arms above meaningless.
    quiet = compare(copy.deepcopy(base), copy.deepcopy(base))
    cases.append(("an UNCHANGED tree is accepted", not quiet, "(no failures)", quiet))

    bad = 0
    for desc, hit, expect, fails in cases:
        print("  [%s] %s" % ("ok" if hit else "FAIL", desc))
        if not hit:
            bad += 1
            print("      expected %r, got: %s" % (expect, fails or "(nothing)"))
    print("gen_va_census selftest: %s (%d case(s))" % ("PASS" if not bad else "FAIL", len(cases)))
    return 1 if bad else 0


# ---- main ---------------------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="the ratchet + drift gate (lint_repo)")
    ap.add_argument("--record", action="store_true", help="re-record the baseline after a fall")
    ap.add_argument("--sites", action="store_true", help="print every site with its owner")
    ap.add_argument("--selftest", action="store_true", help="prove each refusal fires")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    doc = build()

    if args.check:
        if not os.path.exists(OUT_PATH):
            print("gen_va_census --check: %s missing -- run --record first" % OUT_PATH)
            return 1
        committed = json.load(io.open(OUT_PATH, encoding="utf-8"))
        fails = compare(doc, committed)
        if fails:
            for f in fails:
                print("gen_va_census: %s" % f)
            return 1
        # The lint line says the same thing the headline does, because this is the line that ends
        # up in a gate log and a reader of THAT should not have to run the tool to learn that the
        # survivors are keeps rather than debt.
        s = doc["summary"]
        print(
            "va census: %d site(s) needing work, %d printed keep(s), 0 unrouted, ratchet green"
            % (s.get("needing_work", s["routed"]), s.get("kept", 0))
        )
        return 0

    report(doc, show_sites=args.sites)

    if args.record:
        with io.open(OUT_PATH, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(doc, fh, indent=1, sort_keys=False)
            fh.write("\n")
        print("\nwrote %s" % os.path.relpath(OUT_PATH, REPO))
    return 0


if __name__ == "__main__":
    sys.exit(main())
