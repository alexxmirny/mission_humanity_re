#!/usr/bin/env python3
# check_net_lockstep_refs.py -- fork item F1(a): D1's teeth. Flipped to --require-empty at F3D.
#
# THE CLAIM UNDER TEST (the fork plan, ruling D1). The reimplemented lockstep/session closure
# (`src/mh_dll/libmh/lockstep/`) ships in libmh.dll. `desync_watch` is EXTRACTED back to mh.dll because
# it is a runtime instrument needed in both configs. D1 then asserts: "Preflight F1 verifies zero
# residual link-level references from config-1 code into the closure." Config (1) = the `original`
# selector -- since F2E that is literally `[config] mode=original` (mh/config/config.h), and
# `mh::config::ours_run()` is the one predicate that answers it.
#
# THE EXTRACTION HAPPENED AT F3D, and this file is no longer describing a plan. desync_watch lives in
# `src/mh_dll/mh/desync/` and names nothing in mh/lockstep; the four `mh::desync::` references from
# net_seams.cpp are therefore references into an mh.dll module, not into the closure, and this tool
# stopped enumerating them (REF_RE below). What it still measures every run is the INVERSE: that the
# instrument has not grown a new edge back INTO the closure -- see extraction_tail(), which is a hard
# failure rather than a report line, because a silent regression there would re-create exactly the
# coupling D1 spent an item removing.
#
# So the question this tool answers is NOT "does the closure execute under config (1)" -- that was
# already measured -- but the strictly stronger LINK-level one: after desync_watch is pulled out, do
# the net seam TUs still NAME anything in mh/lockstep? A guarded runtime call is still a link-level
# reference, and a link-level reference is what makes mh_net.dll need libmh.dll on its import list.
# The tool therefore records BOTH dimensions per reference:
#
#   link  -- does the reference survive into the object file as an undefined external?
#   exec  -- does the referring code RUN under config (1)?  (source-level, from the guard above it)
#
# A `constexpr` constant or an `inline` function in a lockstep HEADER scores link=no / exec=yes: the
# linker never sees it, but the header must still travel with mh_net.dll. That asymmetry is exactly
# why this tool runs TWO independent mechanisms instead of trusting either.
#
#   SRC  -- comment/string-stripped scan of the five net seam sources for `mh::lockstep::` and
#           `mh::sim::` qualified names, plus `#include "lockstep/..."` edges.
#           Never stale; sees header-inlines, constants and types that never reach the linker.
#           Cannot see: a reference reached through a `using` alias, a macro that pastes the
#           namespace, a function pointer handed over by a third party, or a name introduced by a
#           header this TU includes transitively (there is no `using namespace` in any of the five
#           today -- the tool asserts that, and fails if one appears).
#   OBJ  -- dumpbin -symbols over the built seam objects: UNDEF externals in the seam objs that are
#           DEFINED externals in the 17 mh/lockstep objs. This is the load-bearing evidence for the
#           link dimension. Cannot see: header-inlines/constants (never undefined), anything the
#           optimiser folded, and anything a STALE object predates -- so the tool compares mtimes and
#           says so. The Release objects are /GL (ANONYMOUS OBJECT) and unreadable by dumpbin; the
#           Debug set is the one to read.
#
# Every reference either matches a RULING below (bucket + the guard that justifies it) or the run
# FAILS as drift. That is what makes this wirable into lint: it is not "count went up", it is
# "someone added a coupling nobody classified".
#
# BUCKETS. The task's three, with bucket 1 narrowed by F3D:
#   1 instrument    -- the D21 desync instrument's INSTALL PATH. It was "the desync_watch family";
#                      after the extraction the only member is the mh::sim promotion question the
#                      install site asks, which is outside mh/lockstep by construction. Never residue.
#   2 config2_only  -- linked, but PROVABLY not executed under config (1) (a guard that is false
#                      under `mode=original` dominates the call). Still a link-level reference.
#   3 residue       -- referenced AND executed (or unconditionally linked) under config (1).
#
# Usage:
#   python tools/check_net_lockstep_refs.py                 # full report, both mechanisms
#   python tools/check_net_lockstep_refs.py --src-only      # skip dumpbin (no build needed)
#   python tools/check_net_lockstep_refs.py --objdir DIR    # read objects from DIR
#   python tools/check_net_lockstep_refs.py --json          # machine-readable
#   python tools/check_net_lockstep_refs.py --require-empty # THE GATE (lint): fail on ANY residue
#
# `--baseline` (pass while the residue set equals a hardcoded 9-symbol BASELINE_RESIDUE, fail on any
# drift) was DELETED at F3D along with the set itself. It was scaffolding with a stated expiry: a
# drift detector for a residue that was known non-empty and being drained item by item, and it said
# so in its own docstring ("After F3, use --require-empty instead and delete this"). Keeping it would
# leave two gates that can disagree, one of which passes on a state the other calls a failure.
#
# Exit 0 = clean for the mode asked for.
# Exit 1 = config-(1) residue present.
# Exit 2 = the TOOL is not to be trusted: a self-test coupling went missing, an unruled reference
#          appeared, a `using namespace` defeated the source scan, or the extracted instrument has
#          grown a reference back into the closure. Never read a 2 as "no refs".

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src", "mh_dll")
SEAM_DIR = os.path.join(SRC, "mh", "seams")
LOCKSTEP_DIR = os.path.join(SRC, "mh", "lockstep")
# Where the extracted instrument lives since F3D. Named here so extraction_tail() reads the real
# module rather than a path that silently matches nothing once the files move again.
DESYNC_DIR = os.path.join(SRC, "mh", "desync")
DESYNC_TUS = ("desync_watch.cpp", "desync_watch.h")

# The net seam translation units + their shared header. This is the set the fork moves into
# mh_net.dll; net_transport.cpp/net_key.cpp are the socket layer and name nothing here.
# net_session_probe.cpp was the sixth until fork F2F deleted it (R6).
SEAM_TUS = [
    "net_seams.cpp",
    "net_lockstep.cpp",
    "net_discovery.cpp",
    "net_diag.cpp",
    "net_internal.h",
]

# Object basenames for the OBJ mechanism. The seam side is the 4 .cpp above; the closure side is
# every .cpp in mh/lockstep (discovered, not listed, so a new closure file cannot slip past).
SEAM_OBJS = [t[:-4] for t in SEAM_TUS if t.endswith(".cpp")]

DEFAULT_OBJDIRS = [
    os.path.join(SRC, "mh", "Debug"),
    os.path.join(SRC, "mh_nettest", "Win32", "Debug"),
]

# THE CLOSURE'S OBJECTS MOVED AT FORK F4D, and the OBJ mechanism has to follow them or it quietly
# stops being a mechanism. mh/lockstep and mh/sim are part of the 627-TU roster, which mh.vcxproj no
# longer compiles: their objects are libmh.dll's now, while the four SEAM TUs are still mh.dll's. So
# a closure object is looked for in the chosen objdir FIRST (which keeps a single-tree
# mh_nettest run working unchanged) and then here.
#
# Why this matters more than it looks: the missing-object path only appends a NOTE. Left alone, the
# split would have turned the load-bearing OBJ arm into a run that reported "closure object missing"
# for every file and still printed OK -- the silent-zero shape this project keeps re-learning.
SPINE_OBJDIRS = [
    os.path.join(SRC, "libmh_dll", "Debug"),
    os.path.join(SRC, "libmh", "libmh", "Release"),
]


# WHICH DEFINED SYMBOLS IN A CLOSURE OBJECT ARE ACTUALLY THE CLOSURE'S.
#
# An `inline` function in a shared header is emitted as a COMDAT in EVERY object that uses it, so
# "defined by a mh/lockstep object" is not by itself evidence of closure ownership. The OBJ pass
# therefore keeps only symbols in the two namespaces it reports on -- mh::lockstep (the closure) and
# mh::sim (the separately-scoped promotion state) -- which is exactly the predicate the SRC pass
# already uses on the source text, so the two mechanisms now answer the same question two ways.
# Measured exact: every .cpp in mh/lockstep defines mh::lockstep:: and nothing else.
#
# LIVE AT FORK F4D, not theoretical. mh::state::live() (addr/mh_regions.gen.h, the region registry)
# became a cross-DLL contract row, so the net seam objects carry it as an undefined external while
# lockstep_state.obj and overlay_hoist.obj still emit its COMDAT among a dozen other roster objects.
# Without this filter the tool reported it as a link-level reference the SRC pass had missed and
# exited 2 -- a tool failure naming a symbol with nothing to do with lockstep.
SCOPE_SYM_RE = re.compile(r"@lockstep@mh@@|@sim@mh@@")


def find_obj(objdir, base):
    """The object for `base`, from the chosen objdir or from the spine's own build trees."""
    for d in [objdir] + SPINE_OBJDIRS:
        p = os.path.join(d, base + ".obj")
        if os.path.exists(p):
            return p
    return os.path.join(objdir, base + ".obj")  # the miss, so the caller reports the expected path


# The namespace that IS the closure (its bodies live in libmh/lockstep/).
#
# `desync` LEFT THIS TUPLE AT F3D. It was here because desync_watch.{cpp,h} were compiled out of
# mh/lockstep, so a `mh::desync::` name in a seam TU was textually a reference into the closure
# directory even though it was never closure residue (that is what bucket 1 existed to say). The
# files moved to mh/desync/ and the namespace now denotes an mh.dll module both configurations run,
# so enumerating it here would mean reporting a reference from mh.dll code into mh.dll code.
LOCKSTEP_NS = ("lockstep",)
# `mh::sim` is NOT in mh/lockstep -- it is the sim promotion state that the desync install path
# reads. Enumerated separately (scope "sim") so it cannot contaminate the D1 verdict, which is about
# mh/lockstep only.
SIM_NS = "sim"

REF_RE = re.compile(r"\bmh::(lockstep|sim)::(?:detail::)?([A-Za-z_]\w*)")
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"(lockstep/[^"]+)"', re.M)
USING_NS_RE = re.compile(r"^\s*using\s+namespace\b", re.M)

# ---------------------------------------------------------------------------------------------
# RULINGS -- one row per referenced symbol. Keyed by the qualified name, because call sites move
# and symbols do not. `guard` is the dominating condition read off the source; `exec1` is whether
# that guard lets the call run in a config-(1) run with a stock ini.
#
# Evidence for the exec column, in one place so it can be re-checked rather than re-derived:
#   * `[promote] lockstep=0` makes turn_engine.cpp's install_promotion() return before its seam
#     table, so g_want_time_tick / g_want_sim_tick / promoted::g_any_installed all stay false.
#   * install_promotion itself is invoked from seams/reimpl_probe.cpp (harness domain), NOT from
#     any net seam TU -- which is why it is absent from the enumeration below.
#   * R1 RESOLVED by fork F2B: the three promotion predicates (promotion_active,
#     time_tick_requested, sim_tick_requested) are no longer called from net code. reimpl_probe.cpp
#     records install_promotion's outcome into `g_lockstep_promoted` (seams/net_internal.h) at the
#     call site it owns, and net_lockstep.cpp reads that instead. The semantics are unchanged -- it
#     is still "what the install did", not "what the selector says" -- and the three rulings are
#     DELETED rather than re-bucketed, so a re-introduced call fails as UNRULED (exit 2) instead of
#     merely drifting the baseline.
#   * install_overlay_patches() is UNCONDITIONAL inside lockstep_install_core, but its
#     set_icon_counters() line is behind `mh::config::ours_run()` since F3D (R3), and so is the whole
#     reimpl_fixes handoff block (R2).
#   * install_resync_order_horizon() is gated on `[net] resync_order_horizon`, DEFAULT 1 -- a net
#     tuning knob, not the selector. Its DISPLACED branch, which is the only thing in it that names a
#     closure symbol, is additionally dominated by mh::hook::promoted_owner_of(), false under (1).
#   * `[net] fix_audit` and fix_audit_tick() are GONE (F3D / R5).
# ---------------------------------------------------------------------------------------------
RULINGS = {
    # ---- bucket 1: the D21 instrument's install path (outside mh/lockstep) -------------------
    # The four `mh::desync::` rows were HERE. They are not "resolved" the way R1/R6/R7 were -- they
    # are simply no longer in scope: F3D moved desync_watch.{cpp,h} to src/mh_dll/mh/desync/, so the
    # namespace stopped denoting anything in the closure and REF_RE stopped enumerating it. What is
    # enforced instead is stronger and runs every invocation: extraction_tail() re-measures the
    # instrument's own reach back into mh/lockstep and FAILS (exit 2) if it is ever non-empty.
    "mh::sim::sim_step_promoted": dict(
        bucket=1,
        scope="sim",
        guard="unconditional, inside install_desync_watch's refusal branch",
        exec1=True,
        note="net_seams.cpp. Sim promotion state read from net code to decide WHICH hook the D21 "
        "sampler gets. Outside mh/lockstep, so never part of the D1 verdict. A self-test anchor.",
    ),
    # mh::sim::set_sim_step_pre_hook was HERE. RESOLVED by fork F3D: install_desync_watch registers
    # through the named point (mh::hook::register_callback(point::sim_step_pre, ...)) instead of
    # naming the sim installer across the fork boundary -- the conversion F3C scoped but deferred,
    # because the reference was one of this tool's SELFTEST anchors and killing an anchor without
    # re-picking it in the same commit exits 2 for the wrong reason. DELETED rather than re-bucketed,
    # exactly as R1's / R6's / R7's were, so a re-introduced call fails as UNRULED.
    # ---- bucket 2: linked, provably not executed under config (1) ---------------------------
    "mh::lockstep::time_tick_entry_thunk": dict(
        bucket=2,
        scope="lockstep",
        guard="if (!g_lockstep_promoted.time_tick) return;",
        exec1=False,
        note="install_time_tick_promotion, dominated by the recorded promotion outcome (F2B).",
    ),
    "mh::lockstep::sim_tick_entry_thunk": dict(
        bucket=2,
        scope="lockstep",
        guard="if (!g_lockstep_promoted.sim_tick) return;",
        exec1=False,
        note="install_sim_tick_promotion, dominated by the recorded promotion outcome (F2B).",
    ),
    # ---- bucket 2 continued: the R2 / R3 handoffs, hoisted behind the selector at F3D ---------
    #
    # These four were bucket 3 until F3D, and NOT because they did anything under config (1): every
    # one of them is a WRITE INTO the closure -- configure the fix struct, register two counter sinks
    # -- which the original bodies never read, because nothing of ours is promoted there. They were
    # residue for the reason F1A's ruling named: they RAN. Putting the block and the registration
    # behind `mh::config::ours_run()` makes the guard say what was already true, and the exec column
    # becomes NO by the same argument that retires any config-(2) row.
    #
    # A GUARD, NOT A DELETION, so they stay ruled rather than removed: the calls still exist and must
    # still be classified, and `ours_run()` is the strongest guard in the tree -- one selector, read
    # from the process's own image path, with no ordering to get right (mh/config/config.h).
    "mh::lockstep::fixes": dict(
        bucket=2,
        scope="lockstep",
        guard="if (mh::config::ours_run()) around the whole R2 block; the other site is inside "
        "install_resync_order_horizon's DISPLACED branch, dominated by promoted_owner_of()",
        exec1=False,
        note="R2 fix-config handoff. Both call sites are now unreachable under mode=original: the "
        "second one needs a PROMOTED broadcast_resync_state, which config (1) does not have.",
    ),
    "mh::lockstep::set_fixes": dict(
        bucket=2,
        scope="lockstep",
        guard="if (mh::config::ours_run()) around the whole R2 block",
        exec1=False,
        note="R2 fix-config handoff.",
    ),
    "mh::lockstep::reimpl_fixes": dict(
        bucket=2,
        scope="lockstep",
        guard="if (mh::config::ours_run()) -- TYPE, link-invisible",
        exec1=False,
        note="R2. A closure struct declared in turn_engine.h; the header must still travel with "
        "mh_net.dll, which is what link-invisible-but-required means.",
    ),
    "mh::lockstep::SYNC_OVERLAY_AFTER": dict(
        bucket=2,
        scope="lockstep",
        guard="if (mh::config::ours_run()) -- constexpr, link-invisible",
        exec1=False,
        note="R2-adjacent: a closure constant quoted in the desync_icon_gate arming line, which "
        "lives inside the hoisted block.",
    ),
    # R3 -- registration of net-side counters INTO the closure.
    "mh::lockstep::set_icon_counters": dict(
        bucket=2,
        scope="lockstep",
        guard="if (mh::config::ours_run()) inside install_overlay_patches",
        exec1=False,
        note="R3 counter registration. The BYTE-PATCH carrier of the same two counters "
        "(install_overlay_gate) is deliberately NOT behind the selector -- it is what counts the "
        "icon when our body is not live, i.e. exactly config (1). A self-test anchor.",
    ),
    # ---- bucket 3: config-(1) residue -------------------------------------------------------
    # EMPTY SINCE F3D, and that is the gate: --require-empty fails on any member.
    #
    # R1 (promotion_active / time_tick_requested / sim_tick_requested) was HERE -- resolved F2B.
    # R6 (net_session_probe's 7 references) -- resolved F2F, the TU is deleted.
    # R2 / R3 (fixes, set_fixes, reimpl_fixes, SYNC_OVERLAY_AFTER, set_icon_counters) -- resolved
    #   F3D by hoisting both handoffs behind mh::config::ours_run(); they are bucket 2 above.
    # R4 (detail::resync_order_exec_time) -- resolved F3D by RELOCATING the function. It was the one
    #   residue no guard could remove, because config (1) genuinely runs the D14 clamp: the detour
    #   that applies it is the carrier for the ORIGINAL body. So the arithmetic moved to a neutral
    #   header (src/mh_dll/mh/fix/resync_clamp.h, `mh::fix::`) that both sides include, byte-identical
    #   and still driven by lockstest's eight-case test_d14_resync_order_exec_time.
    # R5 (dispatch_gate_audit, emit_gate_audit, gate_audit) -- resolved F3D by DELETING their only
    #   net-side reader, the `[net] fix_audit` sampler (ruling Q3). The closure keeps the tallies:
    #   rx_dispatch/tx_emit still increment them, lockstep_state.cpp still exposes them, and
    #   lockstest's test_w5_sent_gate_audit_tally still asserts them. What went is the net-side READ.
    #
    # None of the five sets is re-bucketed as "resolved" -- the rulings are DELETED, so a
    # re-introduced reference fails as UNRULED (exit 2) instead of quietly drifting a count.
    # R6 (net_session_probe.cpp's 7 references -- session_globals_reset_calls + the six SESSION_*
    # constants) was HERE. RESOLVED by fork F2F: the probe TU is deleted, its offline successor
    # (tools/check_net_session_addrs.py, F1B) names no lockstep symbol at all, and the rulings are
    # DELETED rather than re-bucketed -- exactly as R1's were at F2B, so a re-introduced reference
    # fails as UNRULED (exit 2) instead of quietly drifting the baseline.
    #
    # R7 (install_promotion_lt_frame / set_time_resync_instrument_hooks /
    # set_session_begin_multi_observer -- the three sim-promotion installs living in net code) was
    # HERE. RESOLVED by fork F3C: all three now go through the NAMED HOOK POINTS
    # (src/mh_dll/mh/hook/hookpoint.h), so net code registers its hooks by name and mh.dll performs
    # the install. The rulings are DELETED rather than re-bucketed -- exactly as R1's were at F2B and
    # R6's at F2F -- so a re-introduced call fails as UNRULED (exit 2) instead of quietly drifting.
    # scope "sim" never entered BASELINE_RESIDUE (which is mh/lockstep only), so their removal takes
    # the report's `residue(sim)` count 3 -> 0 and leaves the baseline set at 9, unchanged.
}

# KNOWN COUPLINGS THE SCAN MUST BE ABLE TO SEE. If it cannot see these, the scan is broken -- a
# silent zero here would be the exact negative-claim trap this gate exists to prevent, and it
# is the one thing a gate that reports EMPTINESS has to defend against. Exit 2, never 0.
#
# RE-PICKED AT F3D, because three of the four original anchors were symbols F3D removes:
# `mh::desync::session_reset` (the namespace left the scan with the module), `mh::sim::
# set_sim_step_pre_hook` (converted to the named hook point) and `mh::lockstep::detail::
# resync_order_exec_time` (relocated to mh::fix). An anchor that dies with the change it is watching
# turns a correct result into exit 2 with a misleading message, so the set is re-chosen in the same
# commit -- and re-chosen for DURABILITY rather than convenience:
#
#   * the two promotion ENTRY THUNKS are the net seam's reason to know the closure exists at all.
#     install_time_tick_promotion / install_sim_tick_promotion are how net code asks for the lockstep
#     bodies; F3's remaining items (E/F/G) touch lobby UI, the no-transport arm and the patcher, and
#     F4 splits the harness out -- none of them removes the promotion edge. If these two ever DO go,
#     mh_net.dll no longer references the closure at all and this whole tool is finished.
#   * `set_icon_counters` covers a bucket-2-by-guard row, so a regression that deleted the
#     `ours_run()` hoist without deleting the call still has an anchor sitting on it.
#   * `sim_step_promoted` is the surviving original anchor and the only one in net_seams.cpp -- it
#     keeps BOTH seam TUs and BOTH namespaces under the self-test, which is the property that
#     matters: an anchor set confined to one file cannot detect a scan that lost the other.
SELFTEST = [
    ("mh::lockstep::time_tick_entry_thunk", "net_lockstep.cpp"),
    ("mh::lockstep::sim_tick_entry_thunk", "net_lockstep.cpp"),
    ("mh::lockstep::set_icon_counters", "net_lockstep.cpp"),
    ("mh::sim::sim_step_promoted", "net_seams.cpp"),
]


# ---------------------------------------------------------------------------------------------
# SRC mechanism
# ---------------------------------------------------------------------------------------------
def strip_comments_and_strings(text):
    """Blank out //, /* */, "..." and '...' while preserving line structure.

    Blanking rather than deleting keeps every byte offset (and therefore every line number) exact,
    which is what lets the reported file:line be quoted straight into a report.
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
        elif c == "/" and nxt == "*":
            out[i] = out[i + 1] = " "
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            if i < n:
                out[i] = " "
                out[min(i + 1, n - 1)] = " "
                i += 2
        elif c in ('"', "'"):
            quote = c
            out[i] = " "
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    out[i] = " "
                    i += 1
                if i < n and text[i] != "\n":
                    out[i] = " "
                i += 1
            if i < n:
                out[i] = " "
                i += 1
        else:
            i += 1
    return "".join(out)


def scan_sources():
    """Return (refs, includes, problems).

    refs: {qualified_name: [(tu, line), ...]} over the five net seam files, code only.
    """
    refs = {}
    includes = []
    problems = []
    for tu in SEAM_TUS:
        path = os.path.join(SEAM_DIR, tu)
        if not os.path.exists(path):
            problems.append("MISSING seam source: %s" % path)
            continue
        raw = open(path, encoding="utf-8", errors="replace").read()
        for m in INCLUDE_RE.finditer(raw):
            includes.append((tu, raw[: m.start()].count("\n") + 1, m.group(1)))
        code = strip_comments_and_strings(raw)
        if USING_NS_RE.search(code):
            problems.append(
                "%s has a `using namespace` -- the qualified-name scan can no longer see every "
                "reference in this TU. Treat this run as UNSOUND." % tu
            )
        for m in REF_RE.finditer(code):
            ns, name = m.group(1), m.group(2)
            detail = "detail::" if "detail::" in m.group(0) else ""
            qual = "mh::%s::%s%s" % (ns, detail, name)
            line = code[: m.start()].count("\n") + 1
            refs.setdefault(qual, []).append((tu, line))
    return refs, includes, problems


# ---------------------------------------------------------------------------------------------
# OBJ mechanism
# ---------------------------------------------------------------------------------------------
def find_dumpbin(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    env = os.environ.get("MH_DUMPBIN")
    if env and os.path.exists(env):
        return env
    roots = []
    try:
        sys.path.insert(0, os.path.join(REPO, "tools"))
        import machine_config  # noqa: E402

        vs = getattr(machine_config, "VS_INSTALL_ROOT", None) or machine_config.get(
            "VS_INSTALL_ROOT"
        )
        if vs:
            roots.append(vs)
    except Exception:
        pass
    for base in (
        r"C:\Program Files (x86)\Microsoft Visual Studio\2022",
        r"C:\Program Files\Microsoft Visual Studio\2022",
    ):
        for edition in ("BuildTools", "Community", "Professional", "Enterprise"):
            roots.append(os.path.join(base, edition))
    for root in roots:
        pat = os.path.join(root, "VC", "Tools", "MSVC", "*", "bin", "Host*", "*", "dumpbin.exe")
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[-1]
    return None


SYM_LINE_RE = re.compile(
    r"^[0-9A-F]{3,}\s+[0-9A-F]{8}\s+(?P<sect>\S+)\s+.*?\bExternal\b\s*\|\s*(?P<name>\S+)"
    r"(?:\s+\((?P<undec>.*)\))?\s*$"
)


def dump_externals(dumpbin, obj):
    """Return (undefined:set, defined:set, note). Decorated names."""
    try:
        res = subprocess.run(
            [dumpbin, "-symbols", obj],
            capture_output=True,
            text=True,
            errors="replace",
            timeout=180,
        )
    except Exception as exc:
        return set(), set(), "dumpbin failed: %s" % exc
    if "ANONYMOUS OBJECT" in res.stdout:
        return set(), set(), "ANONYMOUS OBJECT (/GL link-time codegen) -- not readable by dumpbin"
    undef, defd = set(), set()
    for line in res.stdout.splitlines():
        m = SYM_LINE_RE.match(line.strip())
        if not m:
            continue
        (undef if m.group("sect") == "UNDEF" else defd).add(m.group("name"))
    return undef, defd, ""


def undecorate(dumpbin, obj):
    """Map decorated -> undecorated for one object, so link findings can be quoted in C++ terms."""
    out = {}
    try:
        res = subprocess.run(
            [dumpbin, "-symbols", obj],
            capture_output=True,
            text=True,
            errors="replace",
            timeout=180,
        )
    except Exception:
        return out
    for line in res.stdout.splitlines():
        m = SYM_LINE_RE.match(line.strip())
        if m and m.group("undec"):
            out[m.group("name")] = m.group("undec")
    return out


def scan_objects(objdir, dumpbin):
    """Return (findings, notes). findings: list of dicts with tu/symbol/undecorated/provider."""
    notes = []
    # The closure itself, PLUS mh/sim -- the latter only so the `link` column is honest for the
    # mh::sim couplings the desync_watch install path carries. They are reported under scope "sim"
    # and never counted toward the D1 verdict, which is about mh/lockstep alone.
    lockstep_srcs = sorted(glob.glob(os.path.join(LOCKSTEP_DIR, "*.cpp"))) + sorted(
        glob.glob(os.path.join(SRC, "mh", "sim", "**", "*.cpp"), recursive=True)
    )
    provider = {}  # decorated symbol -> [defining obj basenames]
    for src in lockstep_srcs:
        base = os.path.splitext(os.path.basename(src))[0]
        closure = os.path.dirname(src) == LOCKSTEP_DIR  # only the closure's own state is reportable
        obj = find_obj(objdir, base)
        if not os.path.exists(obj):
            if closure:
                notes.append("closure object missing (not built here): %s.obj" % base)
            continue
        _, defd, note = dump_externals(dumpbin, obj)
        if note:
            if closure:
                notes.append("%s.obj: %s" % (base, note))
            continue
        if closure and os.path.getmtime(src) > os.path.getmtime(obj):
            notes.append("STALE: %s newer than %s.obj" % (os.path.basename(src), base))
        for s in defd:
            if not SCOPE_SYM_RE.search(s):
                continue  # see SCOPE_SYM_RE: a shared header's COMDAT is not a closure symbol
            provider.setdefault(s, []).append(base)

    findings = []
    for base in SEAM_OBJS:
        obj = os.path.join(objdir, base + ".obj")
        src = os.path.join(SEAM_DIR, base + ".cpp")
        if not os.path.exists(obj):
            notes.append("seam object missing (not built here): %s.obj" % base)
            continue
        undef, _, note = dump_externals(dumpbin, obj)
        if note:
            notes.append("%s.obj: %s" % (base, note))
            continue
        if os.path.exists(src) and os.path.getmtime(src) > os.path.getmtime(obj):
            notes.append(
                "STALE: %s.cpp is newer than %s.obj -- the OBJ column for this TU may be missing "
                "recent references. Rebuild the Debug config to close the gap." % (base, base)
            )
        undec = None
        for s in sorted(undef):
            if s in provider:
                if undec is None:
                    undec = undecorate(dumpbin, obj)
                findings.append(
                    dict(
                        tu=base + ".cpp",
                        symbol=s,
                        undecorated=undec.get(s, ""),
                        provider=sorted(provider[s]),
                    )
                )
    return findings, notes


def extraction_tail(objdir, dumpbin):
    """Does the EXTRACTED instrument still reach back into the closure?

    D1 moved `desync_watch` to mh.dll "and anything only it needs". Before F3D this measured the
    extraction's remaining cost: two uses of `mh::lockstep::host_binds()`, for two fields that were
    plain region-registry reads (G_TEXT_TMP, GAME_SESSION_MODE). F3D sourced them from
    `mh::state::ptr<>` directly and moved the files to mh/desync/, so the expected answer is now the
    EMPTY SET -- and this function's job flipped from reporting a number to defending a zero.

    It is a hard failure (exit 2), not a report line, and deliberately so: mh/desync/ is a module a
    future edit can casually `#include "lockstep/..."` from, the build would be perfectly happy, and
    nothing else in the tree would notice. The residue enumeration above would not catch it either --
    it scans the SEAM TUs, and this is a different file reaching a different way.

    SRC pass always; OBJ pass when objects are readable.
    """
    src_refs = {}
    problems = []
    seen_any = False
    for name in DESYNC_TUS:
        path = os.path.join(DESYNC_DIR, name)
        if not os.path.exists(path):
            continue
        seen_any = True
        raw = open(path, encoding="utf-8", errors="replace").read()
        # A header edge is as real a coupling as a named symbol, and it is the one a careless edit
        # adds first -- so it is checked on the RAW text through the same INCLUDE_RE the seam scan
        # uses, and reported as a pseudo-reference so one verdict covers both shapes.
        for m in INCLUDE_RE.finditer(raw):
            src_refs.setdefault('#include "%s"' % m.group(1), []).append(
                "%s:%d" % (name, raw[: m.start()].count("\n") + 1)
            )
        code = strip_comments_and_strings(raw)
        for m in REF_RE.finditer(code):
            if m.group(1) != "lockstep":
                continue
            detail = "detail::" if "detail::" in m.group(0) else ""
            qual = "mh::lockstep::%s%s" % (detail, m.group(2))
            src_refs.setdefault(qual, []).append(
                "%s:%d" % (name, code[: m.start()].count("\n") + 1)
            )
    if not seen_any:
        # An empty tail because the files are not where this tool looks is not a pass -- it is the
        # silent-zero shape the self-test above exists to refuse, one directory over.
        problems.append(
            "the extracted instrument is not at %s (looked for %s). Either it moved again -- point "
            "DESYNC_DIR at it -- or it is gone; either way this run cannot say the extraction held."
            % (DESYNC_DIR, "/".join(DESYNC_TUS))
        )

    obj_refs = []
    if objdir and dumpbin:
        obj = os.path.join(objdir, "desync_watch.obj")
        if os.path.exists(obj):
            undef, _, note = dump_externals(dumpbin, obj)
            if not note:
                undec = undecorate(dumpbin, obj)
                # The same closure-ownership filter the main OBJ pass applies -- see SCOPE_SYM_RE.
                # Without it, fork F4D's cross-DLL mh::state::live() read as the extracted
                # instrument growing an edge back into the closure, which is a hard failure.
                for other in sorted(glob.glob(os.path.join(LOCKSTEP_DIR, "*.cpp"))):
                    base = os.path.splitext(os.path.basename(other))[0]
                    if base == "desync_watch":
                        continue
                    o = find_obj(objdir, base)
                    if not os.path.exists(o):
                        continue
                    _, defd, n2 = dump_externals(dumpbin, o)
                    if n2:
                        continue
                    for s in sorted(x for x in (undef & defd) if SCOPE_SYM_RE.search(x)):
                        obj_refs.append(
                            dict(
                                symbol=qualified_from_undecorated(undec.get(s, "")) or s,
                                provider=base + ".cpp",
                            )
                        )
    return src_refs, obj_refs, problems


def qualified_from_undecorated(undec):
    """Pull `mh::ns::name` back out of dumpbin's undecorated form so OBJ and SRC can be joined.

    dumpbin prints the WHOLE signature -- `struct mh::lockstep::gate_audit & __cdecl
    mh::lockstep::dispatch_gate_audit(void)` -- so the first qualified name in the string is the
    RETURN TYPE, not the symbol. Take the one immediately followed by `(` (the declarator), and
    fall back to the last match when the symbol is a variable rather than a function.
    """
    matches = list(REF_RE.finditer(undec or ""))
    if not matches:
        return None
    m = next((x for x in matches if undec[x.end() :].lstrip()[:1] == "("), matches[-1])
    detail = "detail::" if "detail::" in m.group(0) else ""
    return "mh::%s::%s%s" % (m.group(1), detail, m.group(2))


# ---------------------------------------------------------------------------------------------
def attribute(sym):
    """Which mh/lockstep file declares/defines this name (textual, for the report only)."""
    bare = sym.rsplit("::", 1)[1]
    hits = []
    for path in sorted(glob.glob(os.path.join(LOCKSTEP_DIR, "*.cpp"))) + sorted(
        glob.glob(os.path.join(LOCKSTEP_DIR, "*.h"))
    ):
        txt = open(path, encoding="utf-8", errors="replace").read()
        if re.search(r"\b%s\b" % re.escape(bare), strip_comments_and_strings(txt)):
            hits.append(os.path.basename(path))
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1] if __doc__ else None)
    ap.add_argument("--objdir", help="directory holding the built .obj files (default: mh/Debug)")
    ap.add_argument("--dumpbin", help="path to dumpbin.exe")
    ap.add_argument("--src-only", action="store_true", help="skip the object-symbol mechanism")
    ap.add_argument("--json", action="store_true", help="machine-readable dump to stdout")
    ap.add_argument(
        "--require-empty",
        action="store_true",
        help="THE GATE (lint): fail if any config-(1) residue remains",
    )
    args = ap.parse_args()

    refs, includes, problems = scan_sources()

    # --- OBJ -----------------------------------------------------------------------------------
    obj_findings, obj_notes, objdir_used, dumpbin = [], [], None, None
    if not args.src_only:
        dumpbin = find_dumpbin(args.dumpbin)
        if not dumpbin:
            obj_notes.append("dumpbin.exe not found -- OBJ mechanism SKIPPED (SRC only).")
        else:
            cands = [args.objdir] if args.objdir else DEFAULT_OBJDIRS
            for d in cands:
                if d and os.path.isdir(d) and os.path.exists(os.path.join(d, "net_seams.obj")):
                    objdir_used = d
                    break
            if not objdir_used:
                obj_notes.append(
                    "no readable object dir (looked in: %s) -- OBJ mechanism SKIPPED."
                    % ", ".join(c for c in cands if c)
                )
            else:
                obj_findings, notes = scan_objects(objdir_used, dumpbin)
                obj_notes.extend(notes)

    link_level = {}
    for f in obj_findings:
        q = qualified_from_undecorated(f["undecorated"]) or f["symbol"]
        link_level.setdefault(q, []).append(f)

    # --- classify ------------------------------------------------------------------------------
    rows, unruled = [], []
    for sym in sorted(refs):
        r = RULINGS.get(sym)
        if r is None:
            unruled.append(sym)
            continue
        rows.append(
            dict(
                symbol=sym,
                bucket=r["bucket"],
                scope=r["scope"],
                link=sym in link_level,
                exec1=r["exec1"],
                guard=r["guard"],
                sites=sorted({"%s:%d" % (tu, ln) for tu, ln in refs[sym]}),
                provider=sorted({p for f in link_level.get(sym, []) for p in f["provider"]})
                or attribute(sym),
                note=r["note"],
            )
        )

    # OBJ found something SRC did not: either a `using` alias, a macro, or a stale-object artefact.
    obj_only = [q for q in link_level if q not in refs]

    # --- self-test -----------------------------------------------------------------------------
    st_fail = []
    for sym, tu in SELFTEST:
        if sym not in refs or not any(t == tu for t, _ in refs[sym]):
            st_fail.append("%s in %s" % (sym, tu))

    residue = {r["symbol"] for r in rows if r["bucket"] == 3 and r["scope"] == "lockstep"}
    residue_sim = {r["symbol"] for r in rows if r["bucket"] == 3 and r["scope"] == "sim"}
    dw_src, dw_obj, dw_problems = extraction_tail(objdir_used, dumpbin)
    problems.extend(dw_problems)

    if args.json:
        print(
            json.dumps(
                dict(
                    objdir=objdir_used,
                    dumpbin=dumpbin,
                    rows=rows,
                    includes=[dict(tu=t, line=ln, header=h) for t, ln, h in includes],
                    obj_notes=obj_notes,
                    obj_only=obj_only,
                    unruled=unruled,
                    selftest_failures=st_fail,
                    problems=problems,
                    residue=sorted(residue),
                    residue_sim=sorted(residue_sim),
                    extraction_tail_src=dw_src,
                    extraction_tail_obj=dw_obj,
                ),
                indent=2,
            )
        )
    else:
        print("net seam -> mh/lockstep reference enumeration (fork F1a / ruling D1)")
        print("  SRC: %s" % SEAM_DIR)
        print("  OBJ: %s" % (objdir_used or "SKIPPED"))
        print()
        print(
            "  %-46s %-3s %-9s %-5s %-5s %s" % ("symbol", "bkt", "scope", "link", "exec1", "sites")
        )
        print("  " + "-" * 116)
        for r in sorted(rows, key=lambda r: (r["bucket"], r["scope"], r["symbol"])):
            print(
                "  %-46s %-3d %-9s %-5s %-5s %s"
                % (
                    r["symbol"].replace("mh::", ""),
                    r["bucket"],
                    r["scope"],
                    "yes" if r["link"] else "no",
                    "yes" if r["exec1"] else "NO",
                    ", ".join(r["sites"]),
                )
            )
        print()
        print("  header edges into the closure (#include \"lockstep/...\"):")
        for tu, ln, h in includes:
            print("    %s:%d  %s" % (tu, ln, h))
        print()
        print(
            "  the EXTRACTION TAIL -- mh/desync/'s own reach back into mh/lockstep (MUST be empty):"
        )
        if not dw_src and not dw_obj:
            print("    (none -- the extracted instrument is self-contained)")
        for sym in sorted(dw_src):
            prov = sorted({f["provider"] for f in dw_obj if f["symbol"] == sym})
            print(
                "    %-40s %s  [%s]"
                % (sym, ", ".join(dw_src[sym]), ", ".join(prov) if prov else "header/inline")
            )
        for f in dw_obj:
            if f["symbol"] not in dw_src:
                print("    %-40s (link only)  [%s]" % (f["symbol"], f["provider"]))
        print()
        for note in obj_notes:
            print("  [obj] %s" % note)
        if obj_only:
            print(
                "  [obj] symbols the OBJ pass saw that the SRC pass did not: %s"
                % ", ".join(obj_only)
            )
        print()
        print(
            "  buckets: 1 instrument=%d  2 config2_only=%d  3 residue(lockstep)=%d  3 residue(sim)=%d"
            % (
                sum(1 for r in rows if r["bucket"] == 1),
                sum(1 for r in rows if r["bucket"] == 2),
                len(residue),
                len(residue_sim),
            )
        )
        print()

    # --- verdict -------------------------------------------------------------------------------
    if problems:
        for p in problems:
            print("FAIL(tool): %s" % p, file=sys.stderr)
        return 2
    if st_fail:
        print(
            "FAIL(tool): self-test could not see known coupling(s): %s.\n"
            "The scan is broken -- do NOT read this run as 'no references'." % "; ".join(st_fail),
            file=sys.stderr,
        )
        return 2
    if unruled:
        print(
            "FAIL(drift): %d reference(s) into the closure carry no ruling: %s\n"
            "Classify each in RULINGS (bucket + the guard that justifies it) before this passes."
            % (len(unruled), ", ".join(unruled)),
            file=sys.stderr,
        )
        return 2
    if obj_only:
        print(
            "FAIL(tool): the object pass found link-level references the source pass missed (%s).\n"
            "Either an alias/macro defeats the textual scan or the objects are stale."
            % ", ".join(obj_only),
            file=sys.stderr,
        )
        return 2
    if dw_src or dw_obj:
        # The extraction REGRESSED. Loud, and separate from the residue verdict, because it is a
        # different claim failing: D1's residue enumeration is about the SEAM TUs, and this is the
        # extracted instrument itself growing an edge back into the closure -- which would put
        # mh/lockstep back on mh.dll's link line by a route no seam scan can see.
        print(
            "FAIL(tool): the extracted desync instrument reaches back into mh/lockstep: %s\n"
            "F3D removed the last such edge (two mh::lockstep::host_binds() field reads, replaced "
            "by mh::state::ptr<>). Source the value from mh/state or mh/addr, or move what it needs."
            % ", ".join(sorted(set(dw_src) | {f["symbol"] for f in dw_obj})),
            file=sys.stderr,
        )
        return 2

    # In --json mode stdout is the document; every human line goes to stderr so the output stays
    # parseable (a verdict appended after the JSON made `json.load` fail once -- fixed here).
    out = sys.stderr if args.json else sys.stdout

    # `--baseline` / BASELINE_RESIDUE lived HERE, and F3D deleted both rather than updating the set
    # to the empty one. The drift mode existed to pin a residue that was known non-empty while it was
    # drained item by item; with the set empty, `--require-empty` says the same thing and says it as
    # a claim rather than as a comparison against a literal somebody has to remember to edit.

    if not residue:
        print(
            "OK: the config-(1) residue set into mh/lockstep is EMPTY (%d ruled reference(s): "
            "%d instrument-install, %d config-(2)-only). D1 holds at source level."
            % (
                len(rows),
                sum(1 for r in rows if r["bucket"] == 1),
                sum(1 for r in rows if r["bucket"] == 2),
            ),
            file=out,
        )
        return 0

    print(
        "RESIDUE: %d symbol(s) in mh/lockstep are referenced by net seam code that runs under "
        "config (1) -- D1's 'zero residual link-level references' does NOT hold. This set was "
        "drained to empty at fork F3D, so a member here is a REGRESSION, not a backlog item: either "
        "guard the call with mh::config::ours_run(), move what it reaches, or delete the caller."
        % len(residue),
        file=out,
    )
    for s in sorted(residue):
        print("  - %s" % s, file=out)
    if residue_sim:
        print(
            "  (plus %d mh::sim coupling(s) outside mh/lockstep: %s)"
            % (len(residue_sim), ", ".join(sorted(residue_sim))),
            file=out,
        )
    return 1


if __name__ == "__main__":
    sys.exit(main())
