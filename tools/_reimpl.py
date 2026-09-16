#!/usr/bin/env python3
"""_reimpl.py -- which game functions we have REIMPLEMENTED, and which we plan to.

The coverage report (the codemap generator) answers "how much of this binary do we UNDERSTAND".
That is a different question from "how much of it is OURS", and by 2026-08 the second one had become
the one the M6 plan is actually measured on -- 492 sim functions and 160 AI ones are C++ in
src/mh_dll, and nothing rendered that. This module derives the second answer from committed data so
the report can show both, and so the derivation is one testable place rather than inline in a script
that only runs inside Ghidra.

THREE STATUSES, and the middle one is the point:

  done     a reimplementation exists AND has been proven equivalent -- a migration-ledger row at
           `verified`, or a function outside the ledger era whose C++ body is installed through
           MH_EXPORT_REPLACE / MH_SHADOW_REPLACE.
  planned  a migration ledger has claimed it but it is not proven yet (`not_started` / `translated`
           / `reviewed` / `armed`). This is scheduled work with an owner, NOT a guess about what we
           might do one day: a row exists only because a decomposition put it in a batch.
  (absent) everything else -- including `dead` rows, which are settled as functions that must NEVER
           be migrated. Counting a `dead` row as planned would inflate the remaining work by exactly
           the set someone already did the work of ruling out.

WHAT "done" DOES NOT MEAN. It does not mean the function runs in a shipping game: most promotions
are gated OFF at ship (before fork F2E: a per-mechanism ship constant each, since: `[config] mode`)
and are exercised by an oracle
instead. `ledger_promoted` is reported separately for exactly that reason -- conflating "we have a
proven reimplementation" with "it is what executes" is the kind of over-claim the failure ledger
exists to stop. Read that field narrowly: it is the migration ledgers' own `promoted` flag, and it
covers every STATE-RECORDING domain (the manifest list is derived -- see _manifests). The pre-ledger subsystems below have no such flag, and two of them
(RI-LOCKSTEP, RI-ORDERS) actually DO ship promoted -- so `ledger_promoted` is a floor on what runs
for real, not a measurement of it.

THE NON-LEDGER SOURCE, and why it is a source scan. RI-ORDERS, RI-LOCKSTEP, RI-SAVE, RI-WIRE and
RI-STATE reimplemented their functions BEFORE the migration-manifest era, so there is no ledger row
to read -- the same gap `gen_migration.py`'s `select.owned_elsewhere` exists to paper over. What
those subsystems do have is an installer per function: `MH_EXPORT_REPLACE(<fn>, ...)` (our body
replaces the entry) or `MH_SHADOW_REPLACE(<fn>, ...)` (our body is A/B-compared against it per
call). Either one means a C++ body for that function exists in this tree. Names are resolved to
addresses through docs/symbols.md and tools/data/en_functions.json together -- see name_to_addr().

  KNOWN LIMIT: the scan is a regex over sources, so a macro-EXPANDED MH_EXPORT_REPLACE is invisible
  to it -- the same blind spot gen_dll_patches.py has. The one place that
  happens today is sim_register_state_handlers.cpp's 68 handlers, and all 68 are already `verified`
  ledger rows, so nothing is lost. If that stops being true the count silently under-reports, which
  is the safe direction but is still worth knowing.

Usage:
    from _reimpl import load_reimpl
    st = load_reimpl(repo_root)      # {addr_int: "done"|"planned"}

    python tools/_reimpl.py          # print the roll-up (a quick sanity read, no Ghidra needed)
"""

import json
import os
import re


def _manifests(repo):
    """[(domain_name, absolute manifest path)] for every STATE-RECORDING domain, derived.

    THIS WAS A HAND-KEPT `("sim_migration.json", "ai_migration.json")` and the omission was not
    cosmetic (2026-08-26). Four domains record state. A domain missing from the tuple cannot appear
    in `load_reimpl`, so `load_ledger_rows`'s "a ledger row WINS over the macro scan" precedence --
    stated in a comment three lines below and relied on by gen_migration's `exclude_ours` -- was
    simply false for `tact`: the scan was the only voice, and adding a promotion arm to a verified
    row therefore deleted it from its own domain -- a membership rule that
    deletes a row for having been verified". The second consequence was quieter: gen_codemap's
    reimplementation coverage reads this same list, so the whole tactical closure counted as
    untouched work no matter how much of it was translated.

    Derived the way migration_sweep's `--mode` choices were at TACT-RIG (`all_rig_modes`), and for
    the same reason: a fifth domain must not need an edit here to be counted.

    READINESS domains are excluded on purpose, not forgotten. A `records_state: false` domain
    carries no per-function `state` at all (MIG-SET decision B), so every row would read as
    `planned` -- 381 of them on `readers` -- and inflate "scheduled work with an owner" by a set
    nobody has scheduled to reimplement.

    A profile that fails to LOAD is skipped rather than fatal, the same call `all_rig_modes` makes:
    a profile can be present and deliberately incomplete (`ai_reclassified.json` is), and taking the
    coverage report down over a file it does not need would be worse than counting without it. A
    profile that loads but whose manifest is absent contributes nothing anyway -- `_load_json`
    returns None.
    """
    import sys

    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    # Imported here rather than at module scope so a caller that only wants the regex constants
    # pays nothing, and so an import failure names this function in the traceback.
    from migration_domain import available, load_domain

    out = []
    for name in available():
        try:
            dom = load_domain(name)
        except SystemExit:
            continue
        if not dom.records_state:
            continue
        rel = dom.raw["paths"].get("manifest")
        if rel:
            out.append((name, os.path.join(repo, *rel.split("/"))))
    return out


DLL_SRC = os.path.join("src", "mh_dll")
# `MH_EXPORT_REPLACE(fn, ...)` / `MH_SHADOW_REPLACE(fn, ...)` -- anywhere on the line, because
# several TUs indent them inside a namespace block.
RE_REPLACE = re.compile(r"\bMH_(?:EXPORT|SHADOW)_REPLACE\(\s*([A-Za-z_]\w*)\s*,")
# The terminal ledger state that means "proven equivalent". Kept as a literal rather than imported
# from migration_ledger so this module has no import cost inside PyGhidra.
# Identifiers that are a MACRO PARAMETER at the match site, not a function: the macro's own
# definition, and sim_register_state_handlers.cpp's MH_SIM_HANDLER_ARM(FULL, STEM) wrapper.
# Listed rather than filtered by "does it resolve to an address", so a genuine function that has
# fallen out of both symbol indices is reported as unresolved instead of silently swallowed.
MACRO_PARAM_NAMES = frozenset(("FN", "IMPL", "FULL", "STEM"))

DONE_STATE = "verified"
DEAD_STATE = "dead"


def _load_json(path):
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def load_ledger_rows(repo):
    """[(addr_int, status, domain)] from every migration manifest present."""
    out = []
    for domain, path in _manifests(repo):
        d = _load_json(path)
        if not d:
            continue
        for r in d.get("functions", ()):
            state = r.get("state")
            if state == DEAD_STATE:
                continue
            out.append(
                (
                    int(r["addr"], 16),
                    "done" if state == DONE_STATE else "planned",
                    domain,
                    bool(r.get("promoted")),
                )
            )
    return out


def load_dead(repo):
    """{addr_int: (domain, batch)} for every ledger row in the terminal `dead` state.

    load_ledger_rows DROPS these (a dead row is not planned work), which is right for counting
    remaining effort and wrong for the accessor census: `dead` in this project means a specific,
    evidenced claim -- NOTHING REACHES THIS FUNCTION, settled by three or four concurring tools
    (ref-manager CALL/JMP refs, scan_raw_pointers TOTAL-ORPHAN, materialize_fnptr_refs,
    find-constant-uses on the entry VA). A function nothing reaches never executes, so it never
    writes, so it does not hold a region against the sole-writer boundary. Counting it as an
    external writer overstates the frontier and makes a region that is already clean look dirty.

    Exposed rather than folded into load_reimpl's return because the two answers are different:
    `done` means WE replaced it, `dead` means NOBODY runs it. gen_region_accessors reports both.
    """
    out = {}
    for domain, path in _manifests(repo):
        d = _load_json(path)
        if not d:
            continue
        for r in d.get("functions", ()):
            if r.get("state") == DEAD_STATE:
                out[int(r["addr"], 16)] = (domain, r.get("batch"))
    return out


def load_installed_names(repo):
    """Function NAMES with a C++ body installed through the export/shadow macros."""
    names = set()
    root = os.path.join(repo, DLL_SRC)
    for dirpath, _dirnames, filenames in os.walk(root):
        # addr/ is generated: it DEFINES the macros rather than using them, and matching there
        # would add every function the generator emits a macro for -- i.e. the whole binary.
        if os.path.basename(dirpath) == "addr":
            continue
        for name in filenames:
            if not name.endswith((".cpp", ".h")):
                continue
            with open(os.path.join(dirpath, name), encoding="utf-8", errors="replace") as fh:
                for m in RE_REPLACE.finditer(fh.read()):
                    if m.group(1) not in MACRO_PARAM_NAMES:
                        names.add(m.group(1))
    return names


SYM_ROW = re.compile(r"\| `0x([0-9a-f]{8})` \| `([^`]+)` \|")


def name_to_addr(repo):
    """{function name: entry address}, from BOTH committed indices.

    docs/symbols.md is regenerated every session that renames anything, while
    tools/data/en_functions.json is dumped less often -- so a function renamed recently resolves
    through the first and not the second. Reading both (symbols.md wins) is what stops a rename from
    silently dropping a reimplemented function out of the coverage numbers, which is the failure
    mode that would look like progress going backwards for no reason.
    """
    out = {}
    d = _load_json(os.path.join(repo, "tools", "data", "en_functions.json")) or {}
    for f in d.get("functions", ()):
        out[f["name"]] = int(f["entry"], 16)
    path = os.path.join(repo, "docs", "symbols.md")
    if os.path.isfile(path):
        with open(path, encoding="utf-8", errors="replace") as fh:
            for m in SYM_ROW.finditer(fh.read()):
                out[m.group(2)] = int(m.group(1), 16)
    return out


def load_domain_addrs(repo, domain):
    """Every address a domain's manifest CLAIMS, in whatever state -- `dead` rows included.

    MEMBERSHIP, not progress, and that is the whole distinction: `load_ledger_rows` drops `dead`
    rows because they are not scheduled work, but a `dead` row is still that domain's row, so it
    must not be able to re-enter through `load_reimpl`'s macro scan either.

    Reads the profile directly rather than going through `_manifests`, which yields only
    STATE-RECORDING domains: a readiness domain (`records_state: false`) owns addresses too, and
    scoping it out of its own seed query must not depend on it having a `state` field.
    """
    import sys

    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from migration_domain import load_domain

    if not domain:
        return set()
    try:
        dom = load_domain(domain)
    except SystemExit:
        return set()
    rel = dom.raw["paths"].get("manifest")
    if not rel:
        return set()
    d = _load_json(os.path.join(repo, *rel.split("/"))) or {}
    return {int(r["addr"], 16) for r in d.get("functions", ())}


def load_reimpl(repo, exclude_domain=None):
    """{addr_int: "done" | "planned"} -- see the module docstring for what each means.

    `exclude_domain`: skip everything belonging to this domain -- its ledger rows AND its installed
    macro sites (see `own` below; scoping only the ledger half leaves the hole this parameter was
    added to close). For every caller except a seeded
    domain's own `exclude_ours` check, leave this None -- the whole point of "done" is to be a
    cross-domain, cumulative view (gen_codemap's coverage roll-up, gen_region_accessors' external-
    writer census). The one place a domain-scoped view is needed is `migration_seeds._ours()`: a
    SEEDED domain re-derives its own membership from a query every regeneration, so once one of its
    own rows is marked `verified`, the unscoped "done anywhere" set would start including that row's
    own address and the seed query would exclude its own already-verified member -- a membership
    rule that deletes a row for having been verified (the `tact` incident this
    generalizes; that fix was `exclude_ours: false`, safe only because tact's closure is exclusivity-
    derived and structurally owns nothing cross-domain -- `orders_issue` is not, its seed comment
    names real cross-domain exclusions (the order container's MH_EXPORT_REPLACE'd functions) it must
    keep dropping, so the general fix is scoping "ours" to OTHER domains rather than turning it off).
    """
    status = {}
    own = load_domain_addrs(repo, exclude_domain)
    for addr, st, domain, _promoted in load_ledger_rows(repo):
        if domain == exclude_domain:
            continue
        status[addr] = st
    by_name = name_to_addr(repo)
    for nm in load_installed_names(repo):
        a = by_name.get(nm)
        # A ledger row WINS: it is the graded answer (it knows about evidence tiers), while the
        # macro scan only knows a body exists. Never downgrade `done` and never upgrade `planned`.
        #
        # `own` IS LOAD-BEARING, not belt-and-braces (2026-08-28). Skipping the ledger row above is
        # what makes `a not in status` true for an excluded domain's own address, so without this
        # the scan hands straight back what the exclusion just removed -- and it does so for exactly
        # the rows a domain has ARMED or PROMOTED, i.e. the ones the scoping exists to protect. The
        # first `exclude_domain` implementation had this hole and it recovered 6 of `orders_issue`'s
        # 7 verified rows, missing the only one with a shadow site. See selftest().
        if a is not None and a not in status and a not in own:
            status[a] = "done"
    return status


def summary(repo):
    """Roll-up for the report header and for `python tools/_reimpl.py`."""
    rows = load_ledger_rows(repo)
    by_name = name_to_addr(repo)
    ledger_addrs = {a for a, _s, _d, _p in rows}
    installed = {by_name[n] for n in load_installed_names(repo) if n in by_name}
    per_domain = {}
    for _a, st, dom, promoted in rows:
        d = per_domain.setdefault(dom, {"done": 0, "planned": 0, "ledger_promoted": 0})
        d[st] += 1
        if promoted:
            d["ledger_promoted"] += 1
    extra = sorted(installed - ledger_addrs)
    if extra:
        per_domain["pre-ledger"] = {"done": len(extra), "planned": 0, "ledger_promoted": 0}
    return {
        "per_domain": per_domain,
        "done": sum(d["done"] for d in per_domain.values()),
        "planned": sum(d["planned"] for d in per_domain.values()),
        "ledger_promoted": sum(d["ledger_promoted"] for d in per_domain.values()),
    }


if __name__ == "__main__":
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    s = summary(repo)
    print("reimplementation roll-up (functions):")
    for dom in sorted(s["per_domain"]):
        d = s["per_domain"][dom]
        print(
            "  %-11s done=%-4d planned=%-4d ledger_promoted=%d"
            % (dom, d["done"], d["planned"], d["ledger_promoted"])
        )
    print(
        "  %-11s done=%-4d planned=%-4d ledger_promoted=%d"
        % ("TOTAL", s["done"], s["planned"], s["ledger_promoted"])
    )
