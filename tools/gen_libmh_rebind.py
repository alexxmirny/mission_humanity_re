#!/usr/bin/env python3
"""gen_libmh_rebind.py -- the REBIND BINDER: what a calls-struct member binds when armed.

Implements the libmh rebind notes (rules R1-R11, approved 2026-09-03). Tracker: LIB-REBIND, and --
since the 2026-09-03 reopen -- SIM1-P, whose hosted rebind depends on this machinery.

Every migrated module routes its outward calls through an injectable `struct <x>_calls` of function
pointers bound in a `live_<x>_calls()` aggregate. Today those members bind `mh::call::<fn>`, a thunk
to the original body at a fixed VA. This generator emits the machinery that lets a member bind OUR
C++ instead -- per row at runtime in the hosted build (R11), unconditionally in the standalone one.

WHAT IT EMITS (four files, in two layers on purpose)

  src/mh_dll/mh/addr/mh_rebind.gen.h        the ROW ids, the gate/trap declarations, and the
                                            MH_LIBMH_BIND / MH_REBIND_TARGET_* macros. Lives in
                                            addr/ because it names `mh::call::<fn>` in its hosted
                                            arm, and addr/ is the one tree gen_libmh_calls.py does
                                            NOT scan -- putting it in a module dir would inflate the
                                            outward-call census by one site per rebindable row.
  src/mh_dll/libmh/state/rebind_targets.gen.h  #includes the owning module header of every target, so
                                            the symbols the macros name are DECLARED. Module layer,
                                            because an addr/ header including module headers inverts
                                            the generated-shim direction. Contains no mh::call::
                                            token, so it is census-neutral.
  src/mh_dll/libmh/state/rebind_verify.gen.cpp R4: one static_assert per row, comparing the target's
                                            REAL declared type against ::mh::exp::sig_<fn>, the
                                            committed original signature. This is the gate that lets
                                            R2 derive most targets without a human reading each row.
  tools/data/libmh_rebind.json              R10: per-row provenance -- rule that produced the target
                                            (derived/table), the seam it came from, the class.

THE TARGET (R1). The public live wrapper, never the shadow_arm/promoted_arm adapter: 293 of the 388
available arms carry oracle diagnostics (ai_say trace-budget lines, MH_AI_FIRST_CALL, and in one case
an instrumented calls copy with work counters). Harmless in a standalone lib, not harmless on the
shipping sim path SIM1-P now arms.

THE DERIVATION (R2). From the row's seam `MH_{EXPORT,SHADOW}_REPLACE(<fn>, <qualified adapter>)`:
drop trailing arm namespace components, take the leaf, strip a leading `lt_` if that resolves. The
residue lives in tools/data/libmh_rebind_targets.json (R3) -- and a table row the derivation could
have produced is REFUSED, so the table cannot quietly become the rule.

  --check   regenerate in memory and fail if any emitted file differs from what is committed.
"""

import argparse
import collections
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
MH = os.path.join(REPO, "src", "mh_dll", "mh")
# F5O: the seven MIGRATED_MODULES moved to libmh/. mh/ keeps addr/ (OUT_BIND_H) and the
# harness seams the seam scan still has to see, so BOTH trees are walked; see tools/_dllsrc.py
# for why the module-relative key space did not move with them.
LIBMH = os.path.join(REPO, "src", "mh_dll", "libmh")
CALLS_H = os.path.join(MH, "addr", "mh_calls.gen.h")
TABLE_PATH = os.path.join(REPO, "tools", "data", "libmh_rebind_targets.json")
# The 387 seams the MH_SHADOW_REPLACE scan used to supply, frozen at F2D. See scan_seams().
FROZEN_SEAMS = os.path.join(REPO, "tools", "data", "libmh_rebind_frozen_seams.json")

OUT_BIND_H = os.path.join(MH, "addr", "mh_rebind.gen.h")
OUT_TARGETS_H = os.path.join(LIBMH, "state", "rebind_targets.gen.h")
OUT_VERIFY_CPP = os.path.join(LIBMH, "state", "rebind_verify.gen.cpp")
OUT_TRAPS_CPP = os.path.join(LIBMH, "state", "rebind_traps.gen.cpp")
OUT_JSON = os.path.join(REPO, "tools", "data", "libmh_rebind.json")

MIGRATED_MODULES = ("sim", "ai", "orders", "tact", "lockstep", "save", "state")
# R6/R7: rows whose bodies LIFT-TACT / LIFT-RESID will re-cut. Keyed on the CALLEE's manifest
# domain, not the caller's -- deferral is a property of the body being re-cut.
#
# TACT LEFT THE SET 2026-09-04 (TACT1-P C2, user's call). Deferring tact was meant to avoid wiring
# up bodies LIFT-TACT was about to re-cut, but the liveness census measured the cost of waiting:
# 52 of tact's 98 verified rows were reachable ONLY through a trap, and a trap's hosted arm is the
# ORIGINAL by construction -- so the deferral was not postponing a binding, it was holding half the
# domain permanently out of the shipping configuration while LIFT-TACT converted bodies nothing
# called. A rebind is a POINTER at our body; a later re-cut of that body does not invalidate it.
#
# SIM_RESID LEFT THE SET 2026-09-10 (LIB-REBIND-UI). LIFT-RESID closed 2026-09-09, so the reason the
# domain was held back -- "its shapes are not settled" -- has expired. The set is now EMPTY, and the
# generator prints that affirmatively rather than by the absence of a line.
DEFERRED_DOMAINS = ()

# The three binder spellings (the libmh rebind notes "The measured surface"). A scan matching only the
# first silently misses ai's 454 `&`-prefixed entries and orders' designated initializers -- and a
# C++ aggregate initializer with too few elements is LEGAL, so the omission shifts every later member
# onto the wrong slot instead of failing.
#
# BOTH FORMS, and that is not optional. A site already rewritten to MH_LIBMH_BIND no longer contains
# an `mh::call::` token, so a scanner that knows only the old spelling watches its own row set shrink
# as the rewrite proceeds -- and once every site is converted the binder generates ZERO rows and the
# drift gate goes green over nothing. Caught after the lockstep pilot: 626 rows became 623.
BINDER = re.compile(
    r"^\s*(?:\.\w+\s*=\s*)?(?:&?\s*mh::call::(?P<old>\w+)|MH_LIBMH_BIND\((?P<new>\w+)\))\s*,\s*$"
)
# THE THIRD FORM: a CALL through the binder, `MH_LIBMH_BIND(fn)(args...)`, rather than a struct
# member holding it. A member is how a `calls` table names its callee; this is how a body that has no
# table names one, which is most of the tactical movement/command cluster (TACT1-P C4, 2026-09-04 --
# 51 such sites over 22 rows, every one of them previously a bare `mh::call::` that reached the
# ORIGINAL of a body we own). It is a binder site by the same definition as a member: the arming
# decision selects the target. Missing it here would repeat BINDER's own recorded trap one form over
# -- the rewrite would convert sites while the row set silently shrank.
BINDER_CALL = re.compile(r"MH_LIBMH_BIND\((\w+)\)\s*\(")
# THE FIFTH FORM (LIB-REF-SPLIT, 2026-09-11): `MH_PROMOTED_ROW(fn)` / `MH_PROMOTED(fn, ours)`
# (state/promoted_select.h). Same trap BINDER's own note describes, one spelling further on, and
# this one BITES HARDER than the others: a converted site carries no mh::call:: token, so the row
# it produced disappears -- and with it `MH_REBIND_TARGET_<fn>`, which is the very macro
# MH_PROMOTED_ROW expands to. The generated header would stop declaring the symbol the converted
# site needs, i.e. the conversion would silently delete its own target. Caught by gen_libmh_rebind
# --check going stale on the first conversion batch, before anything shipped.
PROMOTED_SITE = re.compile(r"\bMH_PROMOTED(?:_ROW)?\((\w+)[,)]")
ANY_CALL = re.compile(r"\bmh::call::(\w+)")
# THE FOURTH FORM, found 2026-09-09 (user, reading save_live.cpp): a call through a RUNTIME
# TRAMPOLINE POINTER -- `mh::call::detail::s_u32_EAX_EDX((uintptr_t)g_load_tramp, ...)`. It invokes
# one of the generated register-shim helpers directly, handing it a pointer instead of a committed
# callee address, so it names no callee and the `!= "detail"` filter every consumer applies erases
# it. It still executes ORIGINAL machine code -- a relocated prologue that jumps back into the
# original body -- which is precisely what the standalone build cannot do, so it is a VA site by the
# only definition that matters. FOUR such sites sat uncounted in save/save_live.cpp. Same class of
# blind spot as G155 (a VA reached as DATA rather than as a call token), one indirection over.
TRAMP_CALL = re.compile(
    r"\bmh::call::detail::s_\w+\s*\(\s*(?:reinterpret_cast<\w+>\s*\()?\s*([A-Za-z_]\w*)"
)
SIG_DECL = re.compile(r"^using\s+sig_(\w+)\s*=\s*(.+?)\s*\(__cdecl \*\)\((.*?)\);", re.M)
SEAM = re.compile(r"MH_(EXPORT|SHADOW)_REPLACE\(\s*(\w+)\s*,\s*([\w:]+)\s*\)", re.S)
ARM_NS = re.compile(r"^(shadow_arm|promoted_arm|promoted\w*)$")
# `inline <ret> <name>(<params>) {` -- ret may end in `*`, which a naive `(.+?)\s+(\w+)\(` misses
# (it drops every pointer-returning callee, e.g. llm_strat_pathtrace_dirs_get).
CALL_DECL = re.compile(r"^inline\s+(.+?)\s*\b([A-Za-z_]\w*)\s*\((.*?)\)\s*\{", re.M)
PUB_DECL = re.compile(
    r"^[ \t]*([A-Za-z_][\w:<>,\* &]*?[ \*&])([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*;\s*$", re.M
)


def strip_comments(text):
    # tools/_cstrip.py: the local block-then-line pair this replaced was order-dependent the wrong
    # way round, so a `/*` inside a `//` banner blanked everything up to the next `*/` -- 21 KB over 8
    # files, four of them sim TUs holding `mh::call::` binder members this generator is counting.
    return _cstrip.strip_comments(text)


def norm_type(s):
    """Normalise a type for comparison: drop namespace qualification, collapse pointer spacing.

    CONST IS SIGNIFICANT and is deliberately NOT stripped. The first draft stripped it, which hid a
    real disagreement: mh_calls.gen.h and mh_export.gen.h spell four rows differently (`const T *`
    vs `T *` -- cfg_final_planet_Construct, llm_net_send_order, llm_strat_order_integrity_check,
    llm_strat_order_pending_enqueue). A calls-struct MEMBER's type comes from mh_calls.gen.h, so
    that is the type a target has to match: binding a `T *` body to a `const T *` member needs a
    reinterpret_cast, and the hosted arm's ternary needs both of its arms to be one type. Stripping
    const made this check pass and left the compiler to find it, which it did -- in rx_dispatch.cpp,
    two errors and a cascading "const object must be initialized".
    """
    s = s.replace("::", "")
    s = re.sub(r"\s*\*\s*", "*", s)
    return re.sub(r"\s+", " ", s).strip()


def param_types(params):
    params = params.strip()
    if not params or params == "void":
        return []
    out = []
    for p in params.split(","):
        p = p.strip()
        m = re.search(r"([A-Za-z_]\w*)\s*$", p)
        ty = p[: m.start()].rstrip() if m and p[: m.start()].strip() else p
        out.append(norm_type(ty))
    return out


def load_call_shapes():
    """-> ({callee: (ret, [param types])} normalised, {callee: (ret, params)} verbatim).

    The normalised form is for comparison; the verbatim one is what an emitted declaration must be
    spelled with, since an arm adapter has no declaration of its own to include (see R1's arm class).
    """
    src = io.open(CALLS_H, encoding="utf-8").read()
    norm, raw = {}, {}
    for ret, name, params in CALL_DECL.findall(src):
        norm[name] = (norm_type(ret), param_types(params))
        raw[name] = (ret.strip(), params.strip())
    return norm, raw


# Our own emitted files. They must never be scanned: rebind_targets.gen.h DECLARES the symbols this
# generator is resolving, so indexing it makes the second run classify arm rows as `wrapper` whose
# owning header is this generator's own output -- which then emits no declaration for them and the
# build fails with "not a member of mh". Self-contamination, caught by the first compile.
GENERATED = (
    "mh_rebind.gen.h",
    "rebind_targets.gen.h",
    "rebind_verify.gen.cpp",
    "rebind_traps.gen.cpp",
)


def walk_sources(exts, skip=("attic",)):
    for tree in _dllsrc.ROOTS:
        for root, dirs, files in os.walk(tree):
            if os.path.basename(root) in skip:
                dirs[:] = []
                continue
            for fn in files:
                if fn.endswith(exts) and fn not in GENERATED:
                    yield os.path.join(root, fn)


def module_of(path):
    rel = _dllsrc.rel_or_raise(path)
    return rel.split("/", 1)[0] if "/" in rel else "(root)"


def scan_seams():
    """-> {callee: (kind, qualified adapter)}; EXPORT wins when a row carries both.

    Three sources, in precedence order EXPORT > FROZEN > (scanned SHADOW, none left):

      * MH_EXPORT_REPLACE sites, scanned live out of the tree -- the promotion seams.
      * FROZEN_SEAMS, a committed index (libmh_rebind_frozen_seams.json). 387 rows took their
        seam from an MH_SHADOW_REPLACE site; those sites went with the differential oracle
        (F2D), so the scan cannot see them any more and the measurement is frozen instead --
        the F0 frozen-inputs precedent, per the F2 ruling that a dropped census's DATA survives
        as a static input while its GATE goes. The frozen value is the ALREADY-STRIPPED public
        sibling, so `derive_target` finds it, shape-checks it against the CURRENT declarations
        exactly as before, and names no deleted symbol.
    """
    out = {}
    if os.path.exists(FROZEN_SEAMS):
        with io.open(FROZEN_SEAMS, encoding="utf-8") as f:
            for fn, impl in json.load(f)["seams"].items():
                out[fn] = ("FROZEN", impl.lstrip(":"))
    for p in walk_sources((".cpp", ".h")):
        for m in SEAM.finditer(io.open(p, encoding="utf-8", errors="replace").read()):
            kind, fn, impl = m.group(1), m.group(2), m.group(3)
            if fn not in out or kind == "EXPORT":
                out[fn] = (kind, impl.lstrip(":"))
    return out


def scan_binder_sites():
    """-> ({callee: [(relpath, line, module)]}, {module: direct-site count}) (R9's excluded class)."""
    sites = collections.defaultdict(list)
    direct = collections.Counter()
    for p in walk_sources((".cpp", ".h"), skip=("attic", "addr")):
        mod = module_of(p)
        if mod not in MIGRATED_MODULES:
            continue
        rel = _dllsrc.rel_or_raise(p)
        text = strip_comments(io.open(p, encoding="utf-8", errors="replace").read())
        for i, line in enumerate(text.splitlines(), 1):
            if line.lstrip().startswith("#"):
                continue
            m = BINDER.match(line.rstrip())
            if m is not None:
                # An already-rewritten site (`new`) carries no mh::call:: token at all, so it must be
                # counted from the macro rather than from the token -- see BINDER's own note.
                sites[m.group("old") or m.group("new")].append((rel, i, mod))
                continue
            called = BINDER_CALL.findall(line) + PROMOTED_SITE.findall(line)
            if called:
                for n in called:
                    sites[n].append((rel, i, mod))
                continue
            names = [n for n in ANY_CALL.findall(line) if n != "detail"]
            if names:
                direct[mod] += len(names)
    return sites, direct


def load_ledger():
    """-> {callee: (domain, state)} keeping the strongest claim, + the pre-ledger installed set."""
    ledger = {}
    for domain, path in _reimpl._manifests(REPO):
        doc = _reimpl._load_json(path)
        if not doc:
            continue
        for row in doc.get("functions", ()):
            name = row.get("name")
            if not name:
                continue
            st = row.get("state")
            rank = {_reimpl.DONE_STATE: 2, _reimpl.DEAD_STATE: 1}.get(st, 0)
            if name not in ledger or rank > ledger[name][2]:
                ledger[name] = (domain, st, rank)
    return ledger, _reimpl.load_installed_names(REPO)


def index_public_decls():
    """-> {(ns, leaf): [(ret, [ptypes], header relpath)]} for module-layer PUBLIC declarations.

    Public means: inside `namespace mh::<something>`, not detail/, not an *_arm, not anonymous. The
    `live_*_calls` accessors and `install_*` entry points are excluded -- they are binder and
    installer plumbing, never a rebind target, and leaving them in makes signature matching ambiguous.
    """
    out = collections.defaultdict(list)
    for p in walk_sources((".h",), skip=("attic", "addr")):
        raw = strip_comments(io.open(p, encoding="utf-8", errors="replace").read())
        stack, is_ns, segs, last = [], [], [], 0
        for m in re.finditer(r"\bnamespace\s+([\w:]+)\s*\{|\bnamespace\s*\{|\{|\}", raw):
            segs.append(("::".join(stack), raw[last : m.start()]))
            tok = m.group(0)
            if tok.startswith("namespace"):
                stack.append(m.group(1) or "(anon)")
                is_ns.append(True)
            elif tok == "{":
                is_ns.append(False)
            elif is_ns:
                if is_ns.pop():
                    stack.pop()
            last = m.end()
        segs.append(("::".join(stack), raw[last:]))
        rel = _dllsrc.rel_or_raise(p)
        for ns, seg in segs:
            if not ns.startswith("mh::") or "detail" in ns or "_arm" in ns or "(anon)" in ns:
                continue
            for d in PUB_DECL.finditer(seg):
                leaf = d.group(2)
                if leaf.startswith(("live_", "install_")) or leaf.endswith("_calls"):
                    continue
                out[(ns, leaf)].append((norm_type(d.group(1)), param_types(d.group(3)), rel))
    return out


def derive_target(seam, public, shape, frozen=False):
    """R2: seam adapter -> rebind target. Returns (target, why, class).

    R1 prefers the public live wrapper, because 293 of the arms carry oracle diagnostics. But the
    preference has a REAL exception, and it is the reason the arms exist at all: a wrapper whose
    signature differs from the callee's committed shape cannot be bound to the member. The
    difference is usually a deliberate typing improvement -- the wrapper takes `uint8_t *grid` where
    the original's shape says `void *` -- and the arm is the adapter that bridges it, pinned to
    `sig_<fn>` by its own macro. So: wrapper when it fits, arm when it does not, and the reason is
    recorded per row rather than left to be re-derived.

    A shape mismatch here can also be a false negative -- `mh::game::mh_llm_strat_order *` and the
    `order` alias a module declares for it are the same type, which a textual comparison cannot see
    (norm_type strips `::` and nothing else). That costs us the wrapper and routes through the arm.

    ROUTING THROUGH THE ARM IS **NOT** SAFE, and this docstring said it was until LIB-REF measured
    otherwise (2026-09-11). The old wording -- "routes through the arm, which is safe; it never
    binds anything wrong, because R4's static_assert is the actual gate" -- is true only of the
    HOSTED configuration, where MH_PROMOTED expands to `mh::call::<fn>` and nothing odr-uses the
    target at all. The STANDALONE build takes the target's address, and an arm can fail it two ways
    that R4's static_assert cannot see, because a static_assert checks a DECLARATION:

      * the arm may be COMPILED OUT. `shadow_arm::` bodies sit inside `#ifndef MH_LIBMH_BUILD`
        (LIB-VA0: the shadow arm is harness-only), so routing a row there declares a symbol libmh
        does not contain -- llm_strat_ai_unit_squad_firepower_value, which had no public sibling.
      * the arm may be DEFINED WITH A DIFFERENT SIGNATURE than the one emitted for it. An arm is
        pinned to mh_export.gen.h's `sig_<fn>` by its own MH_EXPORT_REPLACE / MH_SHADOW_REPLACE
        macro, and that header spells four rows non-const where mh_calls.gen.h spells them const
        (see norm_type). rebind_targets.gen.h emits the COMMITTED shape, so the declaration and the
        definition are two different overloads -- llm_net_send_order, where the alias mismatch above
        is what sent it to the arm in the first place.

    Both compiled clean and both were dangling symbols in libmh.lib. Neither the VA census
    (gen_va_census) nor the object-byte scan (scan_libmh_vas) can see them -- a declared-never-
    defined symbol is not a VA and emits no bytes. The instrument that finds them is a LINK of the
    artifact into an executable, which is why LIB-REF's reference host is a permanent gate.

    48 rows target arms today (47 promoted_arm, 1 promoted_order_tx); 2 were dangling. So prefer the
    wrapper harder than the old text implied: when a row lands on an arm, that is a reason to add the
    missing public wrapper or to spell a declaration so this compare can match it, not a routing
    outcome to accept quietly.

    The 8 shadow_arm rows this used to count went at F2D. Seven of them were on the arm BECAUSE
    their public sibling's shape differs, so deleting the oracle did not make them derivable -- they
    now resolve through the residue table to `rebind_arm::` ABI shims that exist for that reason
    alone. `frozen` marks a row whose seam came from the frozen index rather than a live scan; the
    lookup and the shape compare below are the same either way.
    """
    parts = [p for p in seam.split("::") if p]
    leaf, parts = parts[-1], parts[:-1]
    while parts and ARM_NS.match(parts[-1]):
        parts.pop()
    ns = "::".join(parts)
    for cand, how in (
        ((ns, leaf), "same-leaf sibling"),
        (((ns, leaf[3:]) if leaf.startswith("lt_") else None), "lt_-stripped sibling"),
    ):
        if not cand or cand not in public:
            continue
        decls = public[cand]
        if shape and not any(d[0] == shape[0] and d[1] == shape[1] for d in decls):
            return (
                seam,
                "%s %s::%s exists but its shape differs from the committed callee -- the arm bridges it"
                % (
                    how,
                    cand[0],
                    cand[1],
                ),
                "arm",
            )
        if frozen:
            return (
                "%s::%s" % cand,
                "frozen seam -- %s::%s matched the committed callee" % cand,
                "wrapper",
            )
        return "%s::%s" % cand, "%s of %s" % (how, seam), "wrapper"
    return seam, "no public sibling -- the arm is the only original-signature entry", "arm"


KEEP_BLOCKERS = ("no-target", "shape", "semantics")


def keep_original_refusal(name, entry, seam, shape, public):
    """R3's cap, extended to keep_original rows (2026-09-09). -> a problem string, or None.

    A keep_original row is the ONLY row class that asserts a negative -- "nothing here can be
    bound" -- and until now nothing re-checked that assertion against the tree. It went wrong
    exactly as an unchecked negative does: cfg_final_planet_Construct carried "NOT TRANSLATED --
    there is no libmh body to bind" from 2026-09-04 to 2026-09-09, while the body had been in
    libmh.vcxproj and promoted since 2026-09-02. The prose outlived the fact by five days, and no
    generator run could see it, because the derivation that would have contradicted the row is
    skipped the moment `keep_original` is set.

    So the row must now declare a machine-checkable `blocker`, and the two blockers that assert
    "no target exists" are REFUSED when a seam is present -- because a seam is precisely what R2
    derives a target FROM, so their premise is false by construction. What survives the gate is
    `semantics`: a row whose body CAN be bound and must not be, which is a claim about behaviour
    that no signature comparison can settle (llm_strat_order_pending_enqueue masks fields in place
    through the caller's record, so binding it away from the copying thunk would corrupt the
    caller's data). That claim still rests on prose -- the gate narrows the unchecked surface to
    the one class where prose is the only available evidence, rather than pretending to check it.
    """
    blocker = entry.get("blocker")
    if blocker not in KEEP_BLOCKERS:
        return (
            "%s: keep_original row must declare a machine-checkable `blocker` (%s) -- %r found"
            % (
                name,
                "/".join(KEEP_BLOCKERS),
                blocker,
            )
        )
    if blocker in ("no-target", "shape") and seam:
        target, why, cls = derive_target(seam[1], public, shape, seam[0] == "FROZEN")
        return (
            "%s: keep_original blocker %r asserts no bindable target, but seam %s derives %s (%s, %s)"
            " -- re-adjudicate the row: either the blocker is stale (remove the row and let R2 bind"
            " it) or the real objection is behavioural (blocker `semantics`)"
            % (name, blocker, seam[1], target, cls, why)
        )
    return None


def build():
    shapes, shapes_raw = load_call_shapes()
    _export_h = io.open(os.path.join(MH, "addr", "mh_export.gen.h"), encoding="utf-8").read()
    sigs = {m[0]: (m[1].strip(), m[2].strip()) for m in SIG_DECL.findall(_export_h)}
    # R12 (SIM1-P clause 6): each row's ORIGINAL entry address, so a runtime instrument can ask "is
    # the entry I just claimed a rebindable row?" instead of being handed a hand-kept list of names.
    # A row with no exported address gets 0 and simply never matches -- the failure direction that
    # leaves a detour REPORTED rather than silently yielded.
    row_addr = {
        m[0]: int(m[1], 16)
        for m in re.findall(r"inline constexpr uintptr_t addr_(\w+) = 0x([0-9a-fA-F]+)u", _export_h)
    }
    seams = scan_seams()
    sites, direct = scan_binder_sites()
    ledger, installed = load_ledger()
    public = index_public_decls()
    table = _reimpl._load_json(TABLE_PATH) or {}
    table_rows = table.get("targets", {})

    def translated(n):
        row = ledger.get(n)
        return (row and row[1] == _reimpl.DONE_STATE) or n in installed

    core, deferred = [], []
    for name in sorted(sites):
        if not translated(name):
            continue
        domain = ledger.get(name, ("(pre-ledger)",))[0]
        (deferred if domain in DEFERRED_DOMAINS else core).append(name)

    rows, problems, unbindable, keep_original = [], [], [], []
    for name in core:
        shape = shapes.get(name)
        seam = seams.get(name)
        entry = table_rows.get(name)
        if entry and entry.get("keep_original"):
            bad = keep_original_refusal(name, entry, seam, shape, public)
            if bad:
                problems.append(bad)
                continue
            keep_original.append(
                {
                    "name": name,
                    "blocker": entry["blocker"],
                    "reason": entry.get("reason", ""),
                    "sites": len(sites[name]),
                }
            )
            continue
        if seam:
            if name in table_rows:
                # R3's cap: a table row the derivation could have produced would let the table
                # quietly become the rule. Refuse it rather than keep a second source of truth.
                problems.append(
                    "%s: has a table row but R2 derives from seam %s -- remove the table row"
                    % (name, seam[1])
                )
                continue
            target, why, cls = derive_target(seam[1], public, shape, seam[0] == "FROZEN")
            rule = "derived"
        else:
            entry = table_rows.get(name)
            if not entry:
                problems.append(
                    "%s: no seam to derive from and no row in %s -- add one with a reason"
                    % (name, os.path.basename(TABLE_PATH))
                )
                continue
            if entry.get("keep_original"):
                # NOT REBOUND AT ALL, in either arm. The row's two committed descriptions disagree:
                # mh_calls.gen.h (which the calls-struct MEMBER's type comes from) says `const T *`
                # and mh_export.gen.h's sig_<fn> (which the adapter is pinned to) says `T *`, so no
                # single symbol can satisfy both. Binding it would need a const_cast whose safety is
                # a per-row claim about whether the body writes through that pointer -- a real
                # question, not a formality. Until the two generators are reconciled the row stays
                # ORIGINAL and is reported by name, which is the honest state rather than a quiet one.
                keep_original.append(
                    {"name": name, "reason": entry.get("reason", ""), "sites": len(sites[name])}
                )
                continue
            if entry.get("trap"):
                # A row with NO signature-compatible entry point anywhere. Distinct from R6's
                # deferred set: those have a body that is about to be re-cut, these have a body that
                # cannot be reached through the member's ABI at all. Both trap; only the reasons and
                # the retiring item differ, so they are reported as separate classes.
                unbindable.append(
                    {"name": name, "reason": entry.get("reason", ""), "sites": len(sites[name])}
                )
                continue
            target, why, rule = entry["target"], entry.get("reason", ""), "table"
            cls = entry.get("class", "wrapper")
        ns, _, leaf = target.rpartition("::")
        decls = public.get((ns, leaf))
        rows.append(
            {
                "name": name,
                "target": target,
                "rule": rule,
                "class": cls,
                "why": why,
                "seam": seam[1] if seam else None,
                "seam_kind": seam[0] if seam else None,
                "domain": ledger.get(name, ("(pre-ledger)",))[0],
                "header": (table_rows.get(name, {}) or {}).get("header")
                or (decls[0][2] if decls else None),
                "sites": len(sites[name]),
                # R12 (SIM1-P clause 6): 0 when mh_export.gen.h exports no addr_<name>, which matches
                # no claimed entry -- so a row without an address is never yielded on a guess.
                "addr": row_addr.get(name, 0),
            }
        )
    trap_rows = [
        {
            "name": n,
            "domain": ledger.get(n, ("(pre-ledger)",))[0],
            "sites": len(sites[n]),
            "shape": shapes.get(n),
        }
        for n in deferred
    ]
    return {
        "rows": rows,
        "traps": trap_rows,
        "unbindable": unbindable,
        "keep_original": keep_original,
        "problems": problems,
        "direct_sites": dict(sorted(direct.items())),
        "shapes": shapes,
        "shapes_raw": shapes_raw,
        "sigs": sigs,
    }


# ---- emission -------------------------------------------------------------------------------------

BANNER = (
    "//\n"
    "// %s -- GENERATED by tools/gen_libmh_rebind.py. Do not hand-edit; rerun the generator.\n"
    "//\n"
    "// The rebind binder (the libmh rebind notes, rules R1-R11). %s\n"
    "//\n"
)


def emit_bind_header(doc):
    L = []
    w = L.append
    w(
        BANNER
        % (
            "addr/mh_rebind.gen.h",
            "Row ids, the per-row gate (R11), the deferred traps (R6),\n"
            "// and the MH_LIBMH_BIND / MH_REBIND_TARGET_* macros. This file names `mh::call::<fn>` in\n"
            "// its hosted arm, which is why it lives in addr/: gen_libmh_calls.py does not scan addr/,\n"
            "// so the outward-call census is not inflated by one site per rebindable row.",
        )
    )
    w("#pragma once")
    w('#include "addr/mh_calls.gen.h"')
    w("")
    w("namespace mh::rebind {")
    w("")
    w("// One id per rebindable row -- the index into R11's arm bitmap.")
    w("enum row : int {")
    for r in doc["rows"]:
        w("    ROW_%s," % r["name"])
    w("    ROW_COUNT,")
    w("};")
    w("")
    w("// R11: the arm bitmap. Filled ONCE at a NAMED init point, never lazily inside live_calls()")
    w("// -- a live_calls() that consults an unfilled bitmap binds `original` for rows meant to be")
    w(
        "// armed, and the run then reports a clean determinism trajectory that proves nothing. That is"
    )
    w("// the G104 shape; arming sets the loaded flag and armed() complains loudly rather than")
    w("// defaulting quietly.")
    w("//")
    w(
        "// FORK F2E COLLAPSED THE GATE AND KEPT THE BITMAP. The `[rebind]` section and its ship-default"
    )
    w(
        "// constant are gone -- which rows bind ours is `[config] mode` and nothing else -- so the fill"
    )
    w(
        "// takes a bool the caller derived from the selector. The bitmap stays because arming is NOT"
    )
    w(
        "// uniform for a non-configuration reason: a row whose ORIGINAL entry some instrument claimed"
    )
    w(
        "// (the wall-clock pin, a harness trajectory detour) must NOT be rebound, since a rebound caller"
    )
    w(
        "// calls our body directly and never reaches the hook. harness.cpp clears those rows through"
    )
    w("// set_armed() after arming; see state/rebind_arming.cpp.")
    w("void        arm_from_config(bool ours);")
    w("// The offline oracle's arm: nothing, by design. net_selftest drives module bodies through")
    w("// recording stubs, and an armed row would rebind a callee underneath them. Distinct from")
    w("// arm_from_config(false) so a reader does not go looking for the ini that selected it.")
    w("void        arm_none();")
    w("// Force one row on/off AFTER arming, by name; false = no such row. For an owner that")
    w("// only becomes known later -- the wall-clock pin claims time_GetCurrentTime's ENTRY, so a")
    w(
        "// direct rebind of that row would bypass the pin (the promotion arm already yields the same"
    )
    w("// way, via MH_Harness_WantsWallclockPin).")
    w("bool        set_armed(const char *name, bool on);")
    w("bool        armed(int row);")
    w("int         armed_count();")
    w("const char *row_name(int row);")
    w("// The affirmative arming report: count + names, printed even when the count is zero (R11).")
    w("void        report_arming(void (*sink)(const char *));")
    w("")
    w(
        "// R6: the deferred set (%d rows) -- bodies LIFT-TACT/LIFT-RESID re-cut. Named traps:"
        % len(doc["traps"])
    )
    w("// report the callee by name, then fail fast. LIB-REBIND-UI retires them.")
    w("int         trap_count();")
    w("const char *trap_name(int i);")
    w("")
    w("// The row-name table, so armed()/report_arming() need no second source of truth.")
    w("inline constexpr const char *const row_names[] = {")
    for r in doc["rows"]:
        w('    "%s",' % r["name"])
    w("};")
    w("")
    # R12 (SIM1-P clause 6). Emitted at last: this map was COMPUTED and DISCARDED from 2026-09-04 until
    # 2026-09-05, with the comment above row_addr already naming clause 6 as its reason -- so the clause
    # it existed for was closed with a hand-kept list of two string literals instead. Parallel to
    # row_names by index, so a consumer that has a row index has its address without a second lookup.
    #
    # A ROW WITH NO EXPORTED ADDRESS GETS 0, and that is the safe direction on purpose: 0 matches no
    # claimed entry, so such a row is never silently yielded. The alternative failure -- guessing an
    # address -- would yield the WRONG row's binding and be invisible.
    w("// R12: each row's ORIGINAL entry VA, parallel to row_names. 0 = no exported address, which")
    w("// matches no claimed entry, so the row is never yielded on a guess.")
    w("inline constexpr uintptr_t row_addr[] = {")
    for r in doc["rows"]:
        w("    0x%08xu," % r.get("addr", 0))
    w("};")
    w("")
    w("// R6/R3: every row that binds a TRAP rather than a body, and why. `deferred` rows have a")
    w(
        "// body LIFT-TACT/LIFT-RESID will re-cut (LIB-REBIND-UI retires them); `unbindable` rows have"
    )
    w(
        "// no signature-compatible entry point at all, which is a different problem with a different"
    )
    w("// fix -- so they are counted and reported separately rather than blurred together.")
    w("inline constexpr const char *const trap_names[] = {")
    for t in doc["traps"] + doc["unbindable"]:
        w('    "%s",' % t["name"])
    if not (doc["traps"] or doc["unbindable"]):
        w("    nullptr,")
    w("};")
    w("inline constexpr int TRAP_DEFERRED_COUNT   = %d;" % len(doc["traps"]))
    w("inline constexpr int TRAP_UNBINDABLE_COUNT = %d;" % len(doc["unbindable"]))
    w("")
    w("// A trap fired: name the callee, then fail fast. Defined in state/rebind_arming.cpp.")
    w("[[noreturn]] void trap_hit(const char *callee);")
    w("")
    w("} // namespace mh::rebind")
    w("")
    w(
        "// ---- the per-row target macros (R1/R2/R3) --------------------------------------------------"
    )
    for r in doc["rows"]:
        w("#define MH_REBIND_TARGET_%s ::%s" % (r["name"], r["target"].lstrip(":")))
    w("")
    w(
        "// ---- the trap macros (R6): deferred bodies and the unbindable row ---------------------------"
    )
    for t in doc["traps"] + doc["unbindable"]:
        w("#define MH_REBIND_TARGET_%s ::mh::rebind::trap::%s" % (t["name"], t["name"]))
    w("")
    w(
        "// ---- the bind macro (R5) -------------------------------------------------------------------"
    )
    w("//")
    w("// Per row rather than one shared macro, because the two row kinds differ in BOTH arms:")
    w("//")
    w("//   bindable  standalone -> the target, unconditionally (there is no ini to gate on)")
    w(
        "//             hosted     -> the row's R11 arm bit decides: `[config] mode` sets them all, and"
    )
    w("//                           harness.cpp clears the rows an instrument owns the entry of")
    w("//   trap      standalone -> the named trap (R6)")
    w(
        "//             hosted     -> the ORIGINAL, always. A deferred body is not rebindable yet, and"
    )
    w("//                           trapping it hosted would break a game that runs fine today.")
    w("#ifdef MH_LIBMH_BUILD")
    for r in doc["rows"]:
        w("#define MH_LIBMH_BIND_%s (&MH_REBIND_TARGET_%s)" % (r["name"], r["name"]))
    for t in doc["traps"] + doc["unbindable"]:
        w("#define MH_LIBMH_BIND_%s (&MH_REBIND_TARGET_%s)" % (t["name"], t["name"]))
    w("#else")
    for r in doc["rows"]:
        w(
            "#define MH_LIBMH_BIND_%s (::mh::rebind::armed(::mh::rebind::ROW_%s) "
            "? &MH_REBIND_TARGET_%s : &::mh::call::%s)"
            % (r["name"], r["name"], r["name"], r["name"])
        )
    for t in doc["traps"] + doc["unbindable"]:
        w("#define MH_LIBMH_BIND_%s (&::mh::call::%s)" % (t["name"], t["name"]))
    w("#endif")
    w("")
    w("#define MH_LIBMH_BIND(FN) MH_LIBMH_BIND_##FN")
    return "\n".join(L) + "\n"


def emit_targets_header(doc):
    """Declares EVERY rebind target, so a binder TU includes this header and no module headers.

    WHY NOT THE OWNING MODULE HEADERS. The first design had each rewritten TU include the owning
    header of the rows it binds (median 2, max 110). It builds, until it doesn't: pulling module
    headers into TUs that never had them collided their enum constants, and the full build failed
    with 20+ `C2872: 'UNIT_STATE_PARKED': ambiguous symbol` across sim/ and tact/ -- two headers
    declaring the same constant in different namespaces, both suddenly visible. Enum collisions are
    not a fixable accident here; they are what happens when 299 TUs each gain a handful of headers
    they were written without.

    So every target is DECLARED here instead, spelled from the calls-struct member's own type
    (mh_calls.gen.h). A binder TU then needs exactly two includes and gains no module namespace it
    did not already have. The declarations are checked against the real ones in exactly one place --
    rebind_verify.gen.cpp includes both this header AND every owning module header, so a target
    whose real declaration disagrees is a compile error there (an ambiguous `decltype(&X)`), and a
    target that does not exist at all is an unresolved external naming the symbol at link.
    """
    L = []
    w = L.append
    w(
        BANNER
        % (
            "state/rebind_targets.gen.h",
            "Declares every rebind target -- wrapper bodies, arm adapters\n"
            "// and trap stubs alike -- so a binder TU includes THIS and no module headers. Module\n"
            "// layer on purpose: an addr/ header declaring module symbols would invert the\n"
            "// generated-shim direction. No mh::call:: token, so it is census-neutral.",
        )
    )
    w("#pragma once")
    w("#include <cstdint>")
    w("")
    w("// ---- every target: %d row(s) ----" % len(doc["rows"]))
    by_ns = collections.defaultdict(list)
    for r in doc["rows"]:
        ns, _, leaf = r["target"].rpartition("::")
        by_ns[ns].append((leaf, r["name"]))
    for ns in sorted(by_ns):
        w("namespace %s {" % ns)
        for leaf, name in sorted(by_ns[ns]):
            ret, params = doc["shapes_raw"].get(name, ("void", ""))
            w("%s %s(%s);" % (ret, leaf, params))
        w("} // namespace %s" % ns)
    w("")
    w(
        "// ---- trap stubs: %d deferred (R6) + %d unbindable (R3) ----"
        % (len(doc["traps"]), len(doc["unbindable"]))
    )
    w("//")
    w("// Standalone-arm only -- hosted keeps binding the original for these rows. Defined in")
    w("// state/rebind_traps.gen.cpp: name the callee, then fail fast.")
    w("namespace mh::rebind::trap {")
    for t in doc["traps"] + doc["unbindable"]:
        ret, params = doc["shapes_raw"].get(t["name"], ("void", ""))
        w("%s %s(%s);" % (ret, t["name"], strip_param_names(params)))
    w("} // namespace mh::rebind::trap")
    return "\n".join(L) + "\n"


def strip_param_names(params):
    """`int32_t a, void *b` -> `int32_t, void *`. Unnamed, so a trap body warns about nothing."""
    params = params.strip()
    if not params or params == "void":
        return ""
    out = []
    for p in params.split(","):
        p = p.strip()
        m = re.search(r"([A-Za-z_]\w*)\s*$", p)
        out.append(p[: m.start()].rstrip() if m and p[: m.start()].strip() else p)
    return ", ".join(out)


def emit_traps(doc):
    L = []
    w = L.append
    w(
        BANNER
        % (
            "state/rebind_traps.gen.cpp",
            "R6/R3: one named trap per row that binds no body in the\n"
            "// standalone arm -- %d deferred (a body LIFT-TACT/LIFT-RESID re-cuts) and %d unbindable\n"
            "// (no signature-compatible entry point exists). A trap names its callee and fails fast:\n"
            "// the alternative, a silent no-op, would make the lib quietly wrong in a way no oracle\n"
            "// downstream could attribute." % (len(doc["traps"]), len(doc["unbindable"])),
        )
    )
    w('#include "addr/mh_rebind.gen.h"')
    w('#include "state/rebind_targets.gen.h"')
    w("")
    w("namespace mh::rebind::trap {")
    w("")
    for t in doc["traps"] + doc["unbindable"]:
        name = t["name"]
        ret, params = doc["sigs"].get(name) or doc["shapes_raw"].get(name, ("void", ""))
        w(
            "%s %s(%s) { ::mh::rebind::trap_hit(\"%s\"); }"
            % (ret, name, strip_param_names(params), name)
        )
    w("")
    w("} // namespace mh::rebind::trap")
    return "\n".join(L) + "\n"


def emit_verify(doc):
    L = []
    w = L.append
    w(
        BANNER
        % (
            "state/rebind_verify.gen.cpp",
            "R4: one static_assert per row, comparing the target's REAL\n"
            "// declared type against decltype(&mh::call::<fn>) -- the shape the calls-struct member was\n"
            "// generated from. A target whose signature drifted is a COMPILE ERROR naming the row, not a\n"
            "// stack that comes apart at runtime. This is what makes R2's derivation trustworthy without\n"
            "// a human reading every derived row.",
        )
    )
    w("//")
    w(
        "// The compared-against type is `::mh::exp::sig_<fn>` -- the COMMITTED original signature, the"
    )
    w("// same typedef each MH_*_REPLACE macro pins its adapter to -- rather than")
    w(
        "// decltype(&mh::call::<fn>). The two are the same shape, but naming mh::call:: here would put"
    )
    w(
        "// one outward-call token per row into a module-layer TU, and gen_libmh_calls.py counts those:"
    )
    w(
        "// the first draft of this file added 626 phantom direct sites to the census it exists beside."
    )

    w('#include "addr/mh_rebind.gen.h"')
    w('#include "state/rebind_targets.gen.h"')
    w("")
    w("// The wrapper-class targets' owning headers. Only THIS TU needs all of them at once -- a")
    w("// binder TU includes just the ones its own rows bind.")
    for h in sorted({r["header"] for r in doc["rows"] if r["class"] == "wrapper" and r["header"]}):
        w('#include "%s"' % h)
    w("")
    w("#include <type_traits>")
    w("")
    for r in doc["rows"]:
        name = r["name"]
        # The compared-against type is the CALLS-STRUCT MEMBER's type, spelled from mh_calls.gen.h
        # -- which is where the member's type is generated from -- rather than ::mh::exp::sig_<fn>.
        # The two agree on 622 of 626 rows and disagree on four by a pointer `const`; the member is
        # what a target must actually bind to, so sig_ was a proxy that is wrong exactly where it
        # matters. Spelling it out also keeps `mh::call::` out of this module-layer TU: naming it
        # here would put one outward-call token per row into gen_libmh_calls.py's census, which the
        # first draft did -- 626 phantom direct sites.
        ret, params = doc["shapes_raw"].get(name, ("void", ""))
        want = "%s (*)(%s)" % (ret, strip_param_names(params))
        w("static_assert(std::is_same_v<decltype(&MH_REBIND_TARGET_%s), %s>," % (name, want))
        w('              "rebind target signature drifted: %s -> %s");' % (name, r["target"]))
    w("")
    w("// %d row(s) verified." % len(doc["rows"]))
    return "\n".join(L) + "\n"


def emit_json(doc):
    return (
        json.dumps(
            {
                "_generated_by": "tools/gen_libmh_rebind.py -- do not hand-edit; regenerate instead",
                "_rules": "libmh rebind rules R1-R11",
                "summary": {
                    "rows": len(doc["rows"]),
                    "by_rule": dict(
                        sorted(collections.Counter(r["rule"] for r in doc["rows"]).items())
                    ),
                    "traps": len(doc["traps"]),
                    "unbindable": len(doc["unbindable"]),
                    "keep_original": len(doc["keep_original"]),
                    "direct_sites_excluded": doc["direct_sites"],
                },
                "rows": doc["rows"],
                "traps": [{k: t[k] for k in ("name", "domain", "sites")} for t in doc["traps"]],
                "unbindable": doc["unbindable"],
                "keep_original": doc["keep_original"],
            },
            indent=1,
            sort_keys=False,
        )
        + "\n"
    )


def outputs(doc):
    return [
        (OUT_BIND_H, emit_bind_header(doc)),
        (OUT_TARGETS_H, emit_targets_header(doc)),
        (OUT_VERIFY_CPP, emit_verify(doc)),
        (OUT_TRAPS_CPP, emit_traps(doc)),
        (OUT_JSON, emit_json(doc)),
    ]


def report(doc):
    by_rule = collections.Counter(r["rule"] for r in doc["rows"])
    print(
        "libmh rebind binder: %d row(s)  (derived %d, table %d), %d deferred trap(s)"
        % (len(doc["rows"]), by_rule["derived"], by_rule["table"], len(doc["traps"]))
    )
    dom = collections.Counter(r["domain"] for r in doc["rows"])
    print("  by domain: %s" % ", ".join("%s %d" % kv for kv in sorted(dom.items())))
    # R6: the trap count AND names are printed -- an unreported trap set is an unmeasured one.
    if doc["traps"]:
        print("  deferred traps (%d), by name:" % len(doc["traps"]))
        for t in doc["traps"]:
            print("    %-52s [%s] %d site(s)" % (t["name"], t["domain"], t["sites"]))
    else:
        print("  deferred traps: NONE -- the deferred set is empty (LIB-REBIND-UI is done)")
    if doc["unbindable"]:
        print("  UNBINDABLE (no signature-compatible entry point exists), by name:")
        for u in doc["unbindable"]:
            print("    %-52s %d site(s) -- %s" % (u["name"], u["sites"], u["reason"]))
    else:
        print("  unbindable: none")
    if doc["keep_original"]:
        print("  KEPT ORIGINAL (not rebound in EITHER arm), by name, blocker and site count:")
        for k in doc["keep_original"]:
            print(
                "    %-52s [%s] %d site(s) -- %s"
                % (k["name"], k["blocker"], k["sites"], k["reason"])
            )
    else:
        print("  kept original: none")
    # R9: the excluded class prints beside the measure on EVERY run, and affirmatively at zero.
    total = sum(doc["direct_sites"].values())
    if total:
        print(
            "  R9 EXCLUDED from the standalone measure: %d direct non-binder mh::call:: site(s) -- %s"
            % (total, ", ".join("%s %d" % kv for kv in doc["direct_sites"].items()))
        )
    else:
        print("  R9 EXCLUDED: none -- no direct non-binder mh::call:: sites remain")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail if a committed file is stale")
    args = ap.parse_args()

    doc = build()
    if doc["problems"]:
        print("[gen_libmh_rebind] FAIL: %d unresolved row(s):" % len(doc["problems"]))
        for p in doc["problems"]:
            print("  %s" % p)
        return 1

    stale = []
    for path, text in outputs(doc):
        rel = os.path.relpath(path, REPO)
        if args.check:
            have = io.open(path, encoding="utf-8").read() if os.path.exists(path) else None
            if have != text:
                stale.append(rel)
        else:
            io.open(path, "w", encoding="utf-8", newline="\n").write(text)
    if args.check:
        if stale:
            print(
                "[gen_libmh_rebind] FAIL: stale generated file(s) -- rerun the generator: %s"
                % ", ".join(stale)
            )
            return 1
        report(doc)
        print("[gen_libmh_rebind] OK: the binder is current")
        return 0
    report(doc)
    print("wrote %s" % ", ".join(os.path.relpath(p, REPO) for p, _ in outputs(doc)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
