#!/usr/bin/env python3
"""gen_libmh_calls.py -- the OUTWARD-CALL census: what libmh still calls in the original binary.

LIB0's static half (the endgame plan D-E3, tracker LIB0/LIB-ABI). Every `mh::call::<name>`
token in src/mh_dll/mh/** is a source-level dependency on original machine code being mapped at a
fixed VA -- exactly what a standalone libmh cannot have. This tool measures that surface and JOINS
each callee against what the tree already knows about it, so the LIB-ABI adjudication starts from
classified candidates instead of raw prefix guesses:

  translated  a migration-ledger row at `verified` (or a pre-ledger MH_EXPORT/SHADOW install)
              exists -- the callee has a proven C++ body. The outward call is the G21 shape:
              correct during shadow verification (call the original for per-call fidelity), a
              REBIND candidate for the standalone build. In the HOSTED config a call into a
              PROMOTED callee already self-heals (the E9 at the entry lands in ours).
  planned     a ledger row exists but is not proven yet -- scheduled work with an owner.
  dead        the callee is a ledger `dead` row (must never be migrated). A live call INTO one is
              a finding: either the row's settle evidence or the call site is wrong.
  original    no claim anywhere -- the pool the LIB-ABI adjudication ledger must classify into
              {translate-into-libmh, host-callback, cfg-snapshot, map-blob, dead}.

The per-module "lib-complete" burn-down number is distinct callees (and sites) whose status is not
`translated` -- the calls a standalone lib genuinely cannot make yet.

MEASUREMENT NOTES. Comments are stripped before scanning, so a mention in prose does not count; an
address-taken `&mh::call::name` DOES count (it is still an outward dependency). Generated headers
(mh/addr/) and the dead mh/attic/ are excluded; the injection layer (seams/effects/hook/shadow/
include) is measured but reported as HARNESS, separate from the migrated modules -- it never joins
libmh, so its outward calls are not lib-complete debt.

Output: tools/data/libmh_call_census.json (stable ordering) + a printed summary.
  --check   regenerate in memory and fail (exit 1) if it differs from the committed file -- the
            drift gate; adding one outward call and running --check demonstrates the count rises
            (LIB0's mutation clause).
"""

import argparse
import io
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _cstrip  # noqa: E402
import _dllsrc  # noqa: E402
import _reimpl  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MH_SRC = os.path.join(REPO, "src", "mh_dll", "mh")
OUT_PATH = os.path.join(REPO, "tools", "data", "libmh_call_census.json")

# Top-level dirs under mh/ that are (or become) libmh; everything else measured is harness.
MIGRATED_MODULES = ("sim", "ai", "orders", "tact", "lockstep", "save", "state")
HARNESS_MODULES = ("seams", "hook", "shadow", "include")  # "effects" dropped at fork F2F
EXCLUDED_DIRS = ("addr", "attic")  # generated / dead -- not scanned at all

CALL_RE = re.compile(r"\bmh::call::([A-Za-z_]\w*)")
# LIB-REBIND (the libmh rebind notes R8). A rewritten calls-struct binder site reads
# `MH_LIBMH_BIND(<callee>),` and contains no mh::call:: token -- but it is NOT config-independent:
#   hosted     the row's R11 arm bit decides. WHETHER THE SHIPPING CONFIGURATION ARMS IS READ FROM
#              THE SOURCE, not assumed here -- see ship_rebind_arms(). It has been armed since
#              2026-09-04; this comment asserted otherwise until 2026-09-08 and the HOSTED measure
#              was computed on that stale premise the whole time. Since fork F2E the question is
#              `[config] mode`'s default rather than a per-row ship constant.
#   standalone the site binds our C++ (or a named trap), and the dependency is gone.
# So the site is counted in the GATES-OFF measure and not in the STANDALONE one; the SHIPPING
# measure counts it only when the row is an R6 trap/unbindable, which binds the original in the
# hosted arm however the gate is set. Counting it in NEITHER -- which is what happens if you only
# scan for mh::call:: -- makes the census read 802 -> 177 distinct callees the moment the rewrite
# lands, i.e. it reports the whole debt discharged by a source transform that changed no runtime
# behaviour at all. That is the failure mode this file's own mutation clause exists to prevent, one
# level up; keeping all three measures is what makes both readings available without either lying.
BIND_RE = re.compile(r"\bMH_LIBMH_BIND\(([A-Za-z_]\w*)\)")
# LIB-CRT (2026-09-08). A vendored CRT site reads `MH_CRT(<callee>)` (crt/crt_select.h) and, exactly
# like a rewritten binder row, contains no mh::call:: token -- but it is config-dependent in the SAME
# direction and for the same reason:
#   hosted      the macro expands to ::mh::call::<callee>, so the VA dependency is still there and is
#               still what the shipping game runs, forever, by policy (the endgame plan D-E4).
#   standalone  it expands to ::mh::crt::<callee> and the dependency is genuinely gone.
# Counting it in NEITHER measure -- which is what a scan for mh::call:: alone does -- has two
# consequences, and the second is the one that bites: the hosted measure silently drops 60 sites the
# shipping game still makes, and every vendored callee's ledger row reads as an ORPHAN, because its
# callee "left the pool". So MH_CRT sites are counted here, and a `crt` row with vendored sites is
# ROUTED rather than orphaned -- the third way a row legitimately leaves the mh::call pool, after
# host-callback (stage C) and event-channel (LIFT-EVQ/NOTIFY).
CRT_RE = re.compile(r"\bMH_CRT\(([A-Za-z_]\w*)\)")
# LIB-REF-SPLIT (2026-09-11). The `promoted` class does the same dance one population over. A
# site reads `MH_PROMOTED(<callee>, <our body>)` or `MH_PROMOTED_ROW(<callee>)`
# (state/promoted_select.h) and contains no mh::call:: token, while being config-dependent in
# the SAME direction:
#   hosted      the macro expands to ::mh::call::<callee> -- the token the site had before the
#               conversion, so the shipping game makes exactly the call it made yesterday.
#   standalone  it expands to our own body (for a rebind row, the body the binder itself binds).
# Not counting it would repeat MH_CRT's recorded failure exactly: the hosted measure would
# silently drop the sites the shipping game still makes, and each callee's ledger row would read
# as an ORPHAN because the callee 'left the pool'. So these join `bound`/`vendored` as the third
# config-dependent channel, and a row with promoted sites is ROUTED rather than orphaned.
PROMOTED_RE = re.compile(r"\bMH_PROMOTED(?:_ROW)?\(([A-Za-z_]\w*)[,)]")


def _strip_comments(text):
    # tools/_cstrip.py. The block-then-line pair this replaced was order-dependent the wrong way
    # round: a `/*` inside a `//` banner ate everything to the next `*/`, which in
    # sim_bldg_state_dismantle.cpp hid a nine-member `mh::call::` calls struct from THIS census -- the
    # very measure LIB-REBIND's R8 mutation arm and SIM1-P clause 2 are read off.
    return _cstrip.strip_comments(text)


def _module_of(relpath):
    return relpath.replace("\\", "/").split("/", 1)[0]


def scan():
    """-> (hits, bound, vendored, promoted) -- mh::call::, MH_LIBMH_BIND, MH_CRT, MH_PROMOTED.

    The last three dicts are the config-DEPENDENT halves: those sites are outward in the hosted
    configuration and not in the standalone one -- `bound` by R8's rebind, `vendored` by LIB-CRT's
    MH_CRT selection, `promoted` by LIB-REF-SPLIT's MH_PROMOTED.
    """
    hits = {}
    bound = {}
    vendored = {}
    promoted = {}
    for tree, root, dirs, files in _dllsrc.walk():
        rel_root = _dllsrc.rel_dir(tree, root)
        top = "" if rel_root == "." else _module_of(rel_root)
        if top in EXCLUDED_DIRS:
            dirs[:] = []
            continue
        for fn in files:
            if not fn.endswith((".cpp", ".h")):
                continue
            rel = _dllsrc.rel_or_raise(os.path.join(root, fn))
            mod = _module_of(rel) if rel_root != "." else "(root)"
            text = _strip_comments(
                io.open(os.path.join(root, fn), encoding="utf-8", errors="replace").read()
            )
            for line in text.splitlines():
                # A #define's mh::call::PARAM is a macro parameter, not a callee -- the census once
                # counted `FN` from MH_INTERNAL_CALL as a function (caught by the adjudication).
                if line.lstrip().startswith("#"):
                    continue
                for m in CALL_RE.finditer(line):
                    name = m.group(1)
                    if name == "detail":  # thunk internals, only reachable via generated code
                        continue
                    hits.setdefault(name, {}).setdefault(mod, 0)
                    hits[name][mod] += 1
                for m in BIND_RE.finditer(line):
                    name = m.group(1)
                    bound.setdefault(name, {}).setdefault(mod, 0)
                    bound[name][mod] += 1
                for m in CRT_RE.finditer(line):
                    name = m.group(1)
                    vendored.setdefault(name, {}).setdefault(mod, 0)
                    vendored[name][mod] += 1
                for m in PROMOTED_RE.finditer(line):
                    name = m.group(1)
                    promoted.setdefault(name, {}).setdefault(mod, 0)
                    promoted[name][mod] += 1
    return hits, bound, vendored, promoted


def load_adjudication():
    """-> {name: class} from the LIB-ABI adjudication ledger (empty if absent)."""
    path = os.path.join(REPO, "tools", "data", "libmh_call_ledger.json")
    if not os.path.exists(path):
        return {}
    doc = json.load(io.open(path, encoding="utf-8"))
    return {k: v["class"] for k, v in doc.get("callees", {}).items()}


def load_claims():
    """-> (ledger {name: (domain, state, tier, promoted, rank)}, installed name-set)."""
    ledger = {}
    for domain, path in _reimpl._manifests(REPO):
        d = _reimpl._load_json(path)
        if not d:
            continue
        for row in d.get("functions", ()):
            name = row.get("name")
            if not name:
                continue
            st = row.get("state")
            # Keep the strongest claim if a name appears in two ledgers (verified > dead > other).
            rank = {_reimpl.DONE_STATE: 2, _reimpl.DEAD_STATE: 1}.get(st, 0)
            if name not in ledger or rank > ledger[name][4]:
                ledger[name] = (
                    domain,
                    st,
                    row.get("evidence_tier"),
                    bool(row.get("promoted")),
                    rank,
                )
    installed = _reimpl.load_installed_names(REPO)  # pre-ledger MH_EXPORT/SHADOW bodies
    return ledger, installed


def classify(hits, ledger, installed, adjudication):
    """Join each callee against the migration ledgers + the pre-ledger install scan."""
    out = []
    for name in sorted(hits):
        modules = hits[name]
        row = ledger.get(name)
        if row and row[1] == _reimpl.DONE_STATE:
            status, domain, tier, promoted = "translated", row[0], row[2], row[3]
        elif row and row[1] == _reimpl.DEAD_STATE:
            status, domain, tier, promoted = "dead", row[0], None, False
        elif row:
            status, domain, tier, promoted = "planned", row[0], row[2], False
        elif name in installed:
            status, domain, tier, promoted = "translated", "(pre-ledger)", None, False
        else:
            status, domain, tier, promoted = "original", None, None, False
        rec = {
            "name": name,
            "status": status,
            "domain": domain,
            "tier": tier,
            "promoted": promoted,
            "sites": sum(modules.values()),
            "modules": {k: modules[k] for k in sorted(modules)},
        }
        if status in ("original", "planned"):
            rec["adjudication"] = adjudication.get(name)
        out.append(rec)
    return out


def summarize(callees):
    per_mod = {}
    for c in callees:
        for mod, n in c["modules"].items():
            bucket = per_mod.setdefault(
                mod, {"sites": 0, "distinct": 0, "outward_sites": 0, "outward_distinct": 0}
            )
            bucket["sites"] += n
            bucket["distinct"] += 1
            if c["status"] != "translated":
                bucket["outward_sites"] += n
                bucket["outward_distinct"] += 1
    by_status = {}
    for c in callees:
        by_status[c["status"]] = by_status.get(c["status"], 0) + 1
    return per_mod, by_status


# The fn-ptr dispatch tables and the REGISTRAR that fills each -- lib-complete clause (b): a
# closure owns its dispatch tables when OUR code fills them with OUR pointers, i.e. the registrar
# has a proven C++ body. The registrar name is the check's input; ownership is DERIVED from the
# same ledger/install join as everything else, so this cannot rot into a hand-claimed "owned".
DISPATCH_TABLES = (
    ("strat_state_handlers", "llm_strat_register_state_handlers"),  # SIM1-DISPATCH, 62 handlers
    ("bldg_type_callbacks", "llm_strat_register_bldg_type_callbacks"),  # SIM1-BLDGCB
)


def dispatch_ownership(installed, ledger):
    out = {}
    for table, registrar in DISPATCH_TABLES:
        row = ledger.get(registrar)
        owned = (row is not None and row[1] == _reimpl.DONE_STATE) or registrar in installed
        out[table] = {"registrar": registrar, "owned": owned}
    return out


def ship_rebind_arms():
    """-> 1 if the SHIPPING configuration arms the rebind rows, READ rather than assumed.

    This existed as an assumption for five days and was wrong for four of them. BIND_RE's comment
    and the printed banner both asserted the gate "DEFAULTS TO 0, so the site binds the ORIGINAL",
    which was true when the rebind landed (2026-09-03) and false the next day. The consequence was
    not cosmetic -- the HOSTED measure read ~1800 sites when the shipping DLL binds all but a handful
    of them to OUR code, overstating the VA surface by about an order of magnitude in the one number
    a reader would quote. So it is still read out of the source rather than restated here.

    WHAT IT READS CHANGED AT FORK F2E. It used to be the rebind's ship-default constant in
    seams/net_internal.h, an int the harness threaded into the per-row gate. Both are gone: arming
    follows `[config] mode` alone, whose default is `brokered`, and harness.cpp says so in one line.
    `mh::config::mode_t::brokered` being the ini default IS the shipping answer, so that default is
    what this parses -- a build whose selector defaulted to `original` would make the shipping
    measure the gates-off one again, and this would report it.
    """
    path = os.path.join(MH_SRC, "config", "config.h")
    if not os.path.exists(path):
        return None
    text = io.open(path, encoding="utf-8").read()
    m = re.search(r'GetPrivateProfileStringA\(\s*"config",\s*"mode",\s*"(\w+)"', text)
    if not m:
        return None
    return 1 if m.group(1) == "brokered" else 0


def _hosted_original_binders():
    """-> {callee} whose calls-struct member binds the ORIGINAL even when the gate is armed.

    R6: a `trap` row is `standalone -> the named trap; hosted -> the ORIGINAL, always`, because a
    deferred body is not rebindable yet and trapping it hosted would break a game that runs fine
    today (mh_rebind.gen.h's bind-macro note). So arming the gates does NOT clear these.
    """
    path = os.path.join(REPO, "tools", "data", "libmh_rebind.json")
    if not os.path.exists(path):
        return set()
    doc = json.load(io.open(path, encoding="utf-8"))
    return {r["name"] for r in doc.get("traps", ())} | {
        r["name"] for r in doc.get("unbindable", ())
    }


def config_measures(hits, bound, vendored, promoted):
    """-> the three R8 measures plus `hosted_ship`, over the MIGRATED modules only.

    `vendored` (LIB-CRT's MH_CRT sites) and `promoted` (LIB-REF-SPLIT's MH_PROMOTED sites) join
    `bound` on the hosted side of every measure and are absent from `standalone`, which is precisely
    what those macros do at compile time.
    """

    def tally(*sources, **kw):
        only = kw.get("only")
        sites, names = 0, set()
        for src in sources:
            for name, mods in src.items():
                if only is not None and name not in only:
                    continue
                for mod, n in mods.items():
                    if mod in MIGRATED_MODULES:
                        sites += n
                        names.add(name)
        return {"sites": sites, "distinct": len(names)}

    ship_default = ship_rebind_arms()
    # With the gates armed a bound site resolves to OUR body, so the only binder sites still
    # reaching original code are the R6 trap/unbindable rows. With them unarmed, every bound site
    # does -- which is what `hosted_gates_off` keeps describing.
    still_original = _hosted_original_binders() if ship_default else None
    return {
        "hosted": tally(hits, bound, vendored, promoted),  # kept under its old name: gates OFF
        "hosted_gates_off": tally(hits, bound, vendored, promoted),
        "hosted_ship": _merge(
            tally(hits, vendored, promoted),
            {"sites": 0, "distinct": 0}
            if ship_default is None
            else tally(bound, only=still_original),
        ),
        "ship_rebind_arms": ship_default,
        "standalone": tally(hits),
        "config_dependent": tally(bound),
        "vendored_crt": tally(vendored),
        "promoted_direct": tally(promoted),
    }


def _merge(a, b):
    return {"sites": a["sites"] + b["sites"], "distinct": a["distinct"] + b["distinct"]}


def build():
    hits, bound, vendored, promoted = scan()
    ledger, installed = load_claims()
    adjudication = load_adjudication()
    callees = classify(hits, ledger, installed, adjudication)
    per_mod, by_status = summarize(callees)
    # LIB-ABI's gate halves: (a) every original callee reached from a MIGRATED module carries an
    # adjudication class; (b) no ledger row is orphaned (its callee left the pool -- translated,
    # renamed, or gone), so the ledger cannot silently rot away from the census.
    # `planned` STAYS in the pool: a callee with an unproven migration-ledger row still executes
    # its ORIGINAL bytes, and its adjudication row is the record of the translate decision until a
    # VERIFIED row retires it (LIB-TRANS's burn-down). Orphaning at `planned` would let the
    # translate class reach zero by decomposition alone -- vacuously.
    pool = {
        c["name"]
        for c in callees
        if c["status"] in ("original", "planned")
        and any(m in MIGRATED_MODULES for m in c["modules"])
    }
    # host-callback rows leave the mh::call pool by DESIGN (stage C routed their call sites
    # through the generated table; the only mh::call references left are the harness binder in
    # addr/, which is not scanned). They are NOT orphans: gen_libmh_hostapi consumes them as the
    # table's source of truth, and its --check refuses a row without a committed prototype.
    # event-channel rows (LIFT-EVQ/NOTIFY) leave the pool the same way one step further: the
    # call site emits a typed record (libmh_host_events.h) and mh.dll's SINK -- a harness TU,
    # not scanned -- routes it onto the thunk. The row stays as the conversion's record (which
    # kind replaced the entry).
    hostapi = {n for n, cls in adjudication.items() if cls.startswith("host-callback")}
    eventchannel = {n for n, cls in adjudication.items() if cls.startswith("event-channel")}
    # LIB-CRT rows leave the pool a THIRD way: the site now reads MH_CRT(name), which is the same
    # call in the hosted build and mh::crt:: in the standalone one. The row is not stale -- it is the
    # record of the vendoring decision, and the exemption is deliberately NOT "any crt row": it is
    # only a crt row this scan can SEE a vendored site for. A crt callee that simply vanished from
    # the tree still orphans, which is the property that makes the gate worth having.
    vendored_crt = {
        n
        for n, cls in adjudication.items()
        if cls == "crt" and any(m in MIGRATED_MODULES for m in vendored.get(n, {}))
    }
    # And a FOURTH way, the terminal one (SIMABI-HOOKS, 2026-09-10): a `deleted` row's entry left the
    # host table and libmh makes NO call in its place -- the six retail hook stubs whose bodies are a
    # stack probe and a return, where calling and not calling are the same machine state. The row is
    # kept as the record of that decision, so it must not orphan; but unlike the three above it names
    # a callee nothing may reach again, which is what `resurrected` below gates.
    deleted = {n for n, cls in adjudication.items() if cls == "deleted"}
    # And a FIFTH (LIB-REF-SPLIT): the site reads MH_PROMOTED(name, ours) -- the same call in the
    # hosted build, our own body in the standalone one. Same exemption shape and same deliberate
    # narrowness as vendored_crt: only a row this scan can SEE a promoted site for, so a callee that
    # merely vanished from the tree still orphans.
    promoted_direct = {
        n for n in adjudication if any(m in MIGRATED_MODULES for m in promoted.get(n, {}))
    }
    routed = hostapi | eventchannel | vendored_crt | promoted_direct | deleted
    unadjudicated = sorted(n for n in pool if n not in adjudication)
    orphaned = sorted(n for n in adjudication if n not in pool and n not in routed)
    # And the inverse gate: a MIGRATED module reaching a routed callee through mh::call::
    # bypasses the table/channel (and the no-op-host arm with it) -- module code must use
    # mh::host() for a table entry and the state/host_events.h emitter for a converted one.
    # NOTE the set here is `must_route`, NOT `routed`. A vendored `crt` callee reached through
    # mh::call:: is not misrouted -- that IS the hosted arm, and MH_CRT expands to exactly it. The
    # four that show up this way are ai_state.cpp's SHADOW arm, which is compiled out of libmh
    # entirely and calls the original on purpose. Only host-callback and event-channel rows have a
    # replacement path a migrated module is OBLIGED to take.
    must_route = hostapi | eventchannel
    misrouted = sorted(
        c["name"]
        for c in callees
        if c["name"] in must_route and any(m in MIGRATED_MODULES for m in c["modules"])
    )
    # The terminal class's own gate: a `deleted` callee reached from a migrated module again -- by
    # mh::call::, by a rewritten binder or through MH_CRT -- means the deletion was undone without
    # the row being reclassified. Without this, adding `deleted` to `routed` above would make the
    # class a silent amnesty instead of a ratchet.
    resurrected = sorted(
        n
        for n in deleted
        if any(
            m in MIGRATED_MODULES
            for m in (
                list(hits.get(n, {}))
                + list(bound.get(n, {}))
                + list(vendored.get(n, {}))
                + list(promoted.get(n, {}))
            )
        )
    )
    return {
        "_generated_by": "tools/gen_libmh_calls.py -- do not hand-edit; regenerate instead",
        "_measures": "mh::call:: tokens in src/mh_dll/mh/** (comments stripped; addr/+attic/ excluded)",
        "summary": {
            "distinct_callees": len(callees),
            # R8's two measures. `hosted` is what the tree actually runs today: a rebound binder site
            # still resolves to the ORIGINAL until its [rebind] gate is set, so it counts here.
            # `standalone` is the libmh configuration, where the same site binds our C++ (or a named
            # trap) and the VA dependency is genuinely gone. Reporting only one of the two would let
            # a pure source transform look like discharged debt.
            "config_measures": config_measures(hits, bound, vendored, promoted),
            "by_status": {k: by_status[k] for k in sorted(by_status)},
            "modules": {k: per_mod[k] for k in sorted(per_mod)},
            "dispatch_tables": dispatch_ownership(installed, ledger),
            "adjudication": {
                "pool": len(pool),
                "unadjudicated": unadjudicated,
                "orphaned": orphaned,
                "misrouted_host_callbacks": misrouted,
                "resurrected_deleted": resurrected,
                "hostapi_entries": len(hostapi),
                "by_class": _count_classes(adjudication, pool),
            },
        },
        "callees": callees,
    }


def _count_classes(adjudication, pool):
    counts = {}
    for n in pool:
        c = adjudication.get(n)
        if c:
            counts[c] = counts.get(c, 0) + 1
    return {k: counts[k] for k in sorted(counts)}


def render(doc):
    return json.dumps(doc, indent=1, sort_keys=False) + "\n"


def print_summary(doc):
    s = doc["summary"]
    cm = s.get("config_measures")
    if cm:
        # R8: BOTH measures, always, with the config-dependent middle term named. Printing only the
        # standalone one would report the rebind as debt discharged on the day the source changed.
        sd = cm.get("ship_rebind_arms")
        print(
            "outward calls from the migrated modules -- SHIPPING %d site(s)/%d callee(s) "
            "[the shipping [config] mode arms the rebind: %s -- read from config/config.h], "
            "STANDALONE %d/%d, "
            "ORIGINAL-MODE %d/%d.  The last figure is what `[config] mode=original` does; it is NOT "
            "what ships, and quoting it overstates the VA surface by the %d rebound site(s)."
            % (
                cm["hosted_ship"]["sites"],
                cm["hosted_ship"]["distinct"],
                {1: "yes", 0: "NO", None: "UNREADABLE"}.get(sd, sd),
                cm["standalone"]["sites"],
                cm["standalone"]["distinct"],
                cm["hosted_gates_off"]["sites"],
                cm["hosted_gates_off"]["distinct"],
                cm["config_dependent"]["sites"],
            )
        )
    print(
        "libmh call census: %d distinct callees  (%s)"
        % (
            s["distinct_callees"],
            ", ".join("%s %d" % (k, v) for k, v in s["by_status"].items()),
        )
    )
    print(
        "  %-10s %8s %9s | %13s %16s   (lib-complete debt)"
        % ("module", "sites", "distinct", "outward_sites", "outward_distinct")
    )
    for mod, b in s["modules"].items():
        kind = "migrated" if mod in MIGRATED_MODULES else "HARNESS"
        print(
            "  %-10s %8d %9d | %13d %16d   [%s]"
            % (mod, b["sites"], b["distinct"], b["outward_sites"], b["outward_distinct"], kind)
        )
    adj = s["adjudication"]
    print(
        "  adjudication: pool %d, unadjudicated %d, orphaned %d, misrouted %d, resurrected %d; "
        "hostapi entries %d  (%s)"
        % (
            adj["pool"],
            len(adj["unadjudicated"]),
            len(adj["orphaned"]),
            len(adj["misrouted_host_callbacks"]),
            len(adj["resurrected_deleted"]),
            adj["hostapi_entries"],
            ", ".join("%s %d" % (k, v) for k, v in adj["by_class"].items()),
        )
    )


def _adjudication_gate(doc):
    """LIB-ABI: every pool callee adjudicated, no ledger row orphaned. Returns failure count."""
    adj = doc["summary"]["adjudication"]
    bad = 0
    if adj["unadjudicated"]:
        print(
            "[gen_libmh_calls] FAIL: %d outward callee(s) have NO adjudication class in "
            "tools/data/libmh_call_ledger.json:" % len(adj["unadjudicated"])
        )
        for n in adj["unadjudicated"]:
            print("  %s" % n)
        bad += 1
    if adj["orphaned"]:
        print(
            "[gen_libmh_calls] FAIL: %d ledger row(s) name a callee no longer in the pool "
            "(translated or gone) -- remove or update them:" % len(adj["orphaned"])
        )
        for n in adj["orphaned"]:
            print("  %s" % n)
        bad += 1
    if adj["misrouted_host_callbacks"]:
        print(
            "[gen_libmh_calls] FAIL: %d host-callback callee(s) reached through mh::call:: from a "
            "MIGRATED module -- module code must route through mh::host() (the generated table), "
            "or the no-op-host arm cannot see the call:" % len(adj["misrouted_host_callbacks"])
        )
        for n in adj["misrouted_host_callbacks"]:
            print("  %s" % n)
        bad += 1
    if adj.get("resurrected_deleted"):
        print(
            "[gen_libmh_calls] FAIL: %d callee(s) classed `deleted` are called from a MIGRATED "
            "module again -- the entry left the host surface because libmh makes NO call in its "
            "place; restore the deletion or reclassify the row:" % len(adj["resurrected_deleted"])
        )
        for n in adj["resurrected_deleted"]:
            print("  %s" % n)
        bad += 1
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail if the committed census is stale")
    args = ap.parse_args()

    doc = build()
    text = render(doc)
    if args.check:
        if not os.path.exists(OUT_PATH):
            print("[gen_libmh_calls] FAIL: %s does not exist -- run the generator" % OUT_PATH)
            return 1
        committed = io.open(OUT_PATH, encoding="utf-8").read()
        if committed != text:
            print(
                "[gen_libmh_calls] FAIL: committed census is stale -- an outward call changed; "
                "rerun tools/gen_libmh_calls.py and review the diff"
            )
            return 1
        if _adjudication_gate(doc):
            return 1
        print("[gen_libmh_calls] OK: census is current and the pool is fully adjudicated")
        return 0

    io.open(OUT_PATH, "w", encoding="utf-8", newline="\n").write(text)
    print_summary(doc)
    print("wrote %s" % os.path.relpath(OUT_PATH, REPO))
    return 0


if __name__ == "__main__":
    sys.exit(main())
