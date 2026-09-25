#!/usr/bin/env python3
# gen_harness_contract.py -- fork F4E: mh_harness.dll's TWO import contracts and its export list,
# derived from objects and gated.
#
# THE PROBLEM. F4E takes seams/harness.cpp (8.3k lines, the determinism/replay instrument) out of
# mh.dll and into mh_harness.dll. That one file reaches in three directions, and the split turns
# every one of them into a name that has to resolve across an image boundary at runtime:
#
#   mh.dll   -> harness   12 `MH_Harness_*` entry points. Hand-listed WITH their absent values in
#                         mh/include/mh_harness_module.h, because "what does this answer with no
#                         instrument" is a semantic decision a derivation cannot make. This tool
#                         re-derives the SET and refuses if the header's list is not exactly it.
#   harness  -> mh.dll    the HOST rows: the hook API, the UI-drive readouts, the host-api tables,
#                         the save entry points, the run paths, MH_Net_Send. mh.dll exports them
#                         through a GENERATED mh.def; the harness resolves them with GetModuleHandle
#                         + GetProcAddress and defines each as a naked forwarding thunk.
#   harness  -> libmh.dll the SPINE rows: state regions, the RNG trace, promotion predicates, the two
#                         tactical force-entry verbs. Same mechanism, different module -- and a
#                         different meaning for absence (ruling Q4, below).
#
# ---- WHERE THE TWO IMPORT LISTS COME FROM ---------------------------------------------------------
#
# Not from a scan of source. The same object-level intersection gen_libmh_contract.py uses, because
# it is the only derivation that sees what no grep can -- F4D-PRE measured a 22-TU edge that NOT ONE
# of those TUs named, because it arrived through a macro:
#
#     HOST  = { UNDEFINED in mh_harness.dll's objects } & { DEFINED in mh.dll's objects }
#     SPINE = { UNDEFINED in mh_harness.dll's objects } & { DEFINED in libmh.dll's objects }
#
# A symbol defined on BOTH sides is a SPINE row. mh.dll's definition of such a symbol is its own
# forwarding shim over the libmh contract (seams/libmh_bind.cpp's eight hand rows, plus the 93
# generated thunks), so routing the harness through mh.dll would put two boundary crossings where one
# belongs -- and for `mh::state::live()` it would matter more than tidiness: that is the ONE region
# registry per process (G179), and the harness must reach the same object mh::ai::island_move()
# rebases, which is libmh's.
#
# ---- WHY THE HARNESS'S THUNKS TRAP INSTEAD OF ANSWERING ZERO --------------------------------------
#
# gen_libmh_contract's thunks answer `xor eax,eax` when the slot is null, and for mh.dll that is not
# a convenience -- it is the DEFINITION of configuration (1). Here it would be a lie. The harness
# refuses to arm at all unless BOTH tables resolved whole (ruling Q4: a hard loud refusal when
# libmh.dll is absent), so an unbound slot is not a configuration, it is a bug in our own bind logic.
# A thunk that reached one would be an instrument reporting invented numbers, which is the single
# failure mode a determinism harness must never have. So the absent path records the row index and
# jumps to a noreturn trap that names the row on three channels and kills the process.
#
# AND IT IS WHY THERE ARE NO CROSSING COUNTERS HERE, unlike F4D's. Those exist because mh.dll's
# absent path is SILENT: a bound lane that never called through reads exactly like one that crossed
# thousands of times, so only a number tells them apart. This boundary cannot be in that state. Any
# call to any row either crosses or kills the process, and a harness that armed and called NOTHING
# wrote no hashes -- which check_arm_order already refuses by name ("a run that logged nothing is not
# a run that armed cleanly"). The trap and the empty-log refusal between them are a stronger witness
# than a counter, and they cost the hot per-step path nothing.
#
# THAT IS NOT A SOFTENING OF F2E'S SCOPING RULE, IT IS THE RULE APPLIED. `[harness] enable=1` with no
# instrument is refused-and-continue, because an absent instrument cannot change which bodies run. An
# ARMED instrument calling through a null slot is a different fact: the harness IS running, it is
# hashing state and rebinding entries, and it has just discovered its own table is incomplete. There
# is no honest way to continue from there, and the path is unreachable by construction -- the bind
# resolves every row before adopting any, exactly as both sibling binds do.
#
# ---- OUTPUTS ---------------------------------------------------------------------------------------
#
#   tools/data/harness_contract.json                     the committed lists
#   src/mh_dll/mh_harness/harness_contract.gen.h         counts, slot enums, table + bind declarations
#   src/mh_dll/mh_harness/harness_contract.gen.cpp       the tables, the names, the thunks, the binds
#   src/mh_dll/mh_harness/mh_harness.def                 the 12 contract rows + the module entries
#   src/mh_dll/mh/mh.def                                 mh.dll's export list = the HOST rows
#
# mh.def is a new artifact and worth one sentence: mh.dll had three exports, all `__declspec`, and
# those three are NOT in the .def -- MSVC unions the two mechanisms and naming a symbol twice is
# LNK4197. `MH_HostedPoolBase` in particular must keep its name and its export: every deployed
# patched exe imports it by name, and renaming or dropping it would unload mh.dll from every rig VM.
#
# ---- MODES -----------------------------------------------------------------------------------------
#
#   python tools/gen_harness_contract.py              re-derive from objects and write everything
#   python tools/gen_harness_contract.py --check      SOURCE-ONLY: re-render the emitted files from
#                                                     the committed json and fail on a diff. Needs no
#                                                     build -- this is the lint_repo row.
#   python tools/gen_harness_contract.py --rederive --check
#                                                     re-measure from the objects and fail if the
#                                                     committed json disagrees. Needs the Debug
#                                                     builds; run_gate runs it after the build,
#                                                     beside gen_libmh_contract --rederive --check.
#   python tools/gen_harness_contract.py --selftest   planted inputs; every negative goes red.
#
# Exit 0 = current. 1 = stale/drift. 2 = the tool cannot be trusted (objects missing, no dumpbin, a
# positive control gone). NEVER read a 2 as "the contract is empty".

from __future__ import annotations

import argparse
import collections
import io
import json
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src", "mh_dll")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Debug rather than Release for all three, for gen_libmh_contract's reason: a /GL Release object is
# an ANONYMOUS OBJECT dumpbin cannot read.
HARNESS_OBJDIR = os.path.join(SRC, "mh_harness", "Debug")
MH_OBJDIR = os.path.join(SRC, "mh", "Debug")
LIB_OBJDIR = os.path.join(SRC, "libmh_dll", "Debug")
# Static libraries mh.dll links, so their objects are part of "what mh.dll defines".
COMMON_OBJDIR = os.path.join(SRC, "mh_common", "Debug")

HARNESS_VCXPROJ = os.path.join(SRC, "mh_harness", "mh_harness.vcxproj")
MH_VCXPROJ = os.path.join(SRC, "mh", "mh.vcxproj")
# Fork F5K's rider. These two existed as object directories with no roster beside them, which is
# exactly the hole F5K closed one generator over -- see live_objs.
LIB_VCXPROJ = os.path.join(SRC, "libmh_dll", "libmh_dll.vcxproj")
COMMON_VCXPROJ = os.path.join(SRC, "mh_common", "mh_common.vcxproj")

JSON_PATH = os.path.join(REPO, "tools", "data", "harness_contract.json")
GEN_H_PATH = os.path.join(SRC, "mh_harness", "harness_contract.gen.h")
GEN_CPP_PATH = os.path.join(SRC, "mh_harness", "harness_contract.gen.cpp")
HARNESS_DEF_PATH = os.path.join(SRC, "mh_harness", "mh_harness.def")
MH_DEF_PATH = os.path.join(SRC, "mh", "mh.def")
MODULE_HEADER = os.path.join(SRC, "mh", "include", "mh_harness_module.h")

# A PROJECT symbol, derived from the decorated name rather than listed -- the same test
# gen_libmh_contract.py and check_libmh_outbound.py use, so a new CRT helper cannot read as a new
# contract row.
PROJECT_RE = re.compile(r"@mh@@|^_?MH_[A-Za-z]|^_?libmh_[a-z]")

# The generated marshalling thunks (Q5's per-image shim, addr/mh_calls.gen.cpp). EVERY image that
# compiles the roster compiles that TU, and mh_harness.dll compiles it too -- harness.cpp calls into
# the original binary through `mh::call::` -- so its `s_*` shapes are defined on this side and are
# not a contract row.
SHIM_RE = re.compile(r"@detail@call@mh@@")

# POSITIVE CONTROLS, one per direction. A derivation that reports a plausible list has to prove it can
# still see the shapes it is supposed to see. RE-PICK RULE (F3C/F3D): if an item genuinely removes one
# of these rows, DELETE the anchor in the same commit -- never widen the tool to tolerate its absence.
HOST_ANCHORS = [
    (
        "?arm_observer@hook@mh@@YA_NW4point@12@PAXPAPAX@Z",
        "the named hook point the harness arms its detours through (ruling D5). If this is not a "
        "host row, the harness is not arming anything and check_fork_d5_hooks is measuring nothing",
    ),
    (
        "_MH_Core_TrapsAtOpen",
        "F4E's own file cut: the inbound-refusal baseline that stayed in mh.dll with MH_Core_Arm_Early",
    ),
]
SPINE_ANCHORS = [
    (
        "?live@state@mh@@YAAAUlive_table@12@XZ",
        "THE region registry -- one table per process (G179). The harness must reach the same object "
        "mh::ai::island_move() rebases, which is libmh's",
    ),
    (
        "?rng_trace_count@sim@mh@@YAHXZ",
        "the RNG trace the harness reports -- pure instrumentation, and the clearest example of a row "
        "with no honest zero answer",
    ),
]

EXPORT_ANCHORS = [
    ("_MH_Harness_Init", "the arm itself; without it nothing about this module is reachable"),
    ("_MH_Harness_OnPresent", "the per-present tick net_lockstep calls unconditionally"),
]

# ---- mp:D29 -- THE CONFIGURATION (1) FALLBACK ROWS ------------------------------------------------
#
# A THIRD ROW CLASS, and the only spine rows with a spine-free answer that is not a lie. The per-step
# region hash (hash_slice -> emit_slice -> owner_serves; hash_base -> live_base) reaches exactly these
# three spine symbols and no other. mh.dll ALREADY DEFINES all three with the configuration (1)
# answer (mh/seams/libmh_bind.cpp: its own table seeded from REGIONS[] and an empty owner table --
# "not a fallback but the correct answer": with no spine nothing can rebase or claim a region). So
# when libmh.dll is absent the harness binds these three OUT OF mh.dll, into the same spine slots,
# and arms "spine-free"; every other spine slot stays null and every call site that could reach one
# is guarded in harness.cpp (or its key is refused by name at arm -- ruling Q4 as amended by D29).
#
# BOUND FROM mh.dll ONLY WHEN libmh IS ABSENT. With the spine present they stay ordinary SPINE rows,
# so the libmh build is byte-for-byte what it was and G179's one-registry rule holds: the harness
# reads the table mh::ai::island_move() rebases. And it is mh.dll's REAL table -- the export IS the
# function mh.dll's own readers call -- never a harness-private copy, which would stay green the day
# mh.dll moved a region (the planted-copy arm in bindtest is what catches that).
#
# RE-PICK RULE, as for the anchors: a row leaves this list only in the commit that removes the need.
CONFIG1_FALLBACK = [
    (
        "?live@state@mh@@YAAAUlive_table@12@XZ",
        "the region registry: hash_base -> live_base -> live().base[rid]",
    ),
    (
        "?owner_count@state@mh@@YAAAHXZ",
        "the owner table's length: emit_slice -> owner_serves -> owner_of",
    ),
    (
        "?owner_table@state@mh@@YAPAUowner_slot@12@XZ",
        "the owner table: emit_slice -> owner_serves -> owner_of",
    ),
]


class Refusal(Exception):
    """Exit 2: this run cannot be trusted."""


def find_dumpbin():
    from check_module_bind import find_dumpbin as f

    return f()


def find_undname():
    import machine_config as mc

    base = os.path.join(mc.VS_INSTALL_ROOT, "VC", "Tools", "MSVC")
    if not os.path.isdir(base):
        return None
    for ver in sorted(os.listdir(base), reverse=True):
        for host in ("Hostx64", "Hostx86"):
            p = os.path.join(base, ver, "bin", host, "x86", "undname.exe")
            if os.path.isfile(p):
                return p
    return None


def dump_symbols(dumpbin, objs):
    from check_libmh_outbound import dump_symbols as d

    return d(dumpbin, objs)


def objs_in(objdir, what):
    if not os.path.isdir(objdir):
        raise Refusal(
            "%s does not exist. Build Debug|Win32 first -- a derivation over a missing %s object "
            "set measures an empty contract, which reads exactly like a closed one."
            % (objdir, what)
        )
    out = sorted(os.path.join(objdir, f) for f in os.listdir(objdir) if f.lower().endswith(".obj"))
    if not out:
        raise Refusal("%s holds no .obj files (%s)" % (objdir, what))
    return out


def roster(vcxproj):
    """The TU basenames a .vcxproj still compiles. Basenames because that is what the compiler names
    the object after, and it is the only thing an object on disk can be matched by."""
    text = io.open(vcxproj, encoding="utf-8-sig").read()
    return {
        os.path.splitext(os.path.basename(x))[0]
        for x in re.findall(r'<ClCompile Include="([^"]+)"', text)
    }


def live_objs(objdir, vcxproj, what, strict=True):
    """Objects belonging to TUs the project still compiles. A stale object is a measurement of a tree
    that no longer exists (F4D R5: eight F2-era objects put two phantom rows in F4D-PRE's count).

    FORK F5K's RIDER. Two of this tool's four object sets -- mh_common/Debug and libmh_dll/Debug --
    used to come in through bare objs_in, i.e. a directory glob with no roster behind it, which is
    the same hole F5K closed in gen_libmh_std_contract: a build directory records every build that
    ever ran there, so an object left behind by a deleted .cpp keeps defining its symbols and walks
    into the derivation as a HOST or SPINE row that no source line produces. Here that row would not
    just be a wrong number -- every row becomes a slot the harness binds at arm time and a thunk that
    TRAPS when the slot is null, so a phantom row is a process kill waiting for the first call.

    SUBJECT: the .vcxproj ClCompile list, not any archive. libmh_dll and mh_common both also produce
    a linkable artifact, and neither is the honest roster -- an archive or import library is a BUILD
    OUTPUT of the same project, so a .cpp deleted and the project regenerated leaves the dead member
    in it until someone rebuilds, and an archive-checked run would pass the one case this exists to
    catch. The ClCompile list is the declaration. (F5K's live_objs carries the long form.)

    strict=True REFUSES a stale object BY NAME. That is the right answer for the two trees this rider
    added -- both are exact today (mh_common 5/5, libmh_dll 630/630, measured 2026-09-14), so no
    legitimate leftover has to be tolerated, and the artifact is a bind table rather than a report.
    strict=False NAMES THEM AND PROCEEDS, and the two pre-existing call sites keep it deliberately:
    mh/Debug is also gen_libmh_contract's object set, that tool tolerates leftovers there by measured
    policy (its comment records 635 pre-spine objects a clean build never removes), and two tools
    refusing and tolerating the SAME DIRECTORY inside one gate run is incoherent. Both trees are in
    fact exact today too (mh 42/42, mh_harness 4/4) -- the difference is policy, not evidence."""
    want = roster(vcxproj)
    if not want:
        raise Refusal(
            "%s declares no <ClCompile> TUs -- an empty roster classifies every object as stale and "
            "is a broken read of the project, not an empty project" % os.path.basename(vcxproj)
        )
    keep, stale = [], []
    for p in objs_in(objdir, what):
        (keep if os.path.splitext(os.path.basename(p))[0] in want else stale).append(p)
    if stale:
        named = ", ".join(os.path.basename(p) for p in stale[:12]) + (
            ", ..." if len(stale) > 12 else ""
        )
        if strict:
            raise Refusal(
                "%d STALE object(s) in %s: %s. No TU of %s compiles them any more, so every symbol "
                "they carry would enter this contract -- and every contract row is a bound slot with "
                "a trapping thunk -- from a source file that no longer exists. Delete them, or "
                "rebuild into a clean directory, and rerun."
                % (len(stale), objdir, named, os.path.basename(vcxproj))
            )
        print(
            "[gen_harness_contract] note: %d stale object(s) in %s IGNORED (not TUs of %s any "
            "more): %s" % (len(stale), objdir, os.path.basename(vcxproj), named)
        )
    have = {os.path.splitext(os.path.basename(p))[0] for p in keep}
    missing = sorted(want - have)
    if missing:
        raise Refusal(
            "%d of %s's %d TUs have no object in %s: %s%s -- build Debug|Win32 first"
            % (
                len(missing),
                os.path.basename(vcxproj),
                len(want),
                objdir,
                ", ".join(missing[:12]),
                ", ..." if len(missing) > 12 else "",
            )
        )
    return keep


def demangle(undname, names):
    out = {}
    if not undname:
        return out
    for i in range(0, len(names), 200):
        chunk = names[i : i + 200]
        r = subprocess.run([undname, "-f"] + chunk, capture_output=True, text=True)
        cur = None
        for line in r.stdout.splitlines():
            s = line.strip()
            m = re.match(r'^Undecoration of :- "(.*)"$', s)
            if m:
                cur = m.group(1)
                continue
            m = re.match(r'^is :- "(.*)"$', s)
            if m and cur is not None:
                out[cur] = m.group(1)
                cur = None
    return out


def export_name(mangled):
    """THE NAME GetProcAddress MUST BE GIVEN. A C++ symbol is exported under its decorated name
    verbatim; an `extern "C"` __cdecl symbol is DECORATED with a leading underscore in the object and
    EXPORTED without one. gen_libmh_contract records what getting this wrong looks like: a named
    refusal of the whole module on the first boot, not a crash."""
    return mangled if mangled.startswith("?") else mangled.lstrip("_")


def slot_name(mangled, pretty):
    m = re.search(r"([A-Za-z_][A-Za-z0-9_:]*)\s*\(", pretty or "")
    base = m.group(1) if m else mangled.lstrip("_?")
    return re.sub(r"[^A-Za-z0-9_]", "_", base.replace("::", "_"))


def parse_module_symbols(path):
    """The 12 rows the X-macro in mh_harness_module.h declares, in its committed order."""
    text = io.open(path, encoding="utf-8").read()
    m = re.search(r"#define MH_HARNESS_MODULE_SYMBOLS\(X\)(.*?)\n\n", text, re.S)
    if not m:
        raise Refusal("%s: no MH_HARNESS_MODULE_SYMBOLS list" % path)
    return re.findall(r"X\(\s*\w+\s*,\s*(MH_Harness_\w+)\s*,", m.group(1))


# ---------------------------------------------------------------------------------------------
# derivation
# ---------------------------------------------------------------------------------------------
def derive(dumpbin, undname):
    # ALL FOUR object sets are rostered (fork F5K's rider); the two `strict=False` ones are the
    # pre-existing sites, and live_objs records why the policy differs rather than the evidence.
    hobjs = live_objs(HARNESS_OBJDIR, HARNESS_VCXPROJ, "mh_harness.dll", strict=False)
    mobjs = live_objs(MH_OBJDIR, MH_VCXPROJ, "mh.dll", strict=False) + live_objs(
        COMMON_OBJDIR, COMMON_VCXPROJ, "mh_common.lib"
    )
    lobjs = live_objs(LIB_OBJDIR, LIB_VCXPROJ, "libmh.dll")

    hdef, hundef = dump_symbols(dumpbin, hobjs)
    mdef, mundef = dump_symbols(dumpbin, mobjs)
    ldef, _lundef = dump_symbols(dumpbin, lobjs)

    def proj(names):
        return {n for n in names if PROJECT_RE.search(n) and not SHIM_RE.search(n)}

    want = proj(set(hundef))
    spine = sorted(want & set(ldef))
    host = sorted((want & set(mdef)) - set(ldef))
    exports = sorted(proj(set(mundef)) & set(hdef))

    unresolved = sorted(want - set(ldef) - set(mdef) - set(hdef))

    pretty = demangle(undname, sorted(set(spine) | set(host)))

    def rows(names):
        out = []
        for n in names:
            p = pretty.get(n, "")
            out.append(
                {
                    "mangled": n,
                    "export": export_name(n),
                    "pretty": p,
                    "slot": slot_name(n, p),
                    "used_by": sorted(hundef.get(n, [])),
                }
            )
        return out

    # mp:D29: a fallback row is recorded only when BOTH sides hold it -- a spine row (so it has a
    # slot) that mh.dll's objects DEFINE (so mh.def can export it). validate() refuses a short list
    # by name, which is how "mh.dll stopped defining live()" arrives as a red rather than as a
    # configuration (1) run that refuses to arm on the rig.
    fallback = [n for n, _why in CONFIG1_FALLBACK if n in set(spine) and n in set(mdef)]

    return {
        "host": rows(host),
        "spine": rows(spine),
        "exports": [{"name": n.lstrip("_")} for n in exports],
        "unresolved": unresolved,
        "config1_fallback": fallback,
    }


def validate(data):
    errs = []
    for side, anchors in (("host", HOST_ANCHORS), ("spine", SPINE_ANCHORS)):
        live = {r["mangled"] for r in data[side]}
        for name, why in anchors:
            if name not in live:
                errs.append(
                    "positive control MISSING from the %s contract: %s (%s). Either the derivation "
                    "is broken or the anchor died with a change -- re-pick it in the SAME commit "
                    "rather than dropping it." % (side, name, why)
                )
        seen = collections.Counter(r["slot"] for r in data[side])
        for slot, n in seen.items():
            if n > 1:
                errs.append(
                    "%s slot name %s is used by %d rows -- disambiguate slot_name()"
                    % (side, slot, n)
                )
        for r in data[side]:
            if r["pretty"] and "__cdecl" not in r["pretty"] and r["mangled"].startswith("?"):
                errs.append(
                    "%s is not __cdecl (%s). The naked forwarding thunk is a __cdecl tail call and "
                    "cannot serve another convention." % (r["mangled"], r["pretty"])
                )
    exports = [r["name"] for r in data["exports"]]
    for name, why in EXPORT_ANCHORS:
        if name.lstrip("_") not in exports:
            errs.append("positive control MISSING from the export list: %s (%s)" % (name, why))
    # mp:D29: the configuration (1) fallback rows. Each must be a SPINE row (it binds into a spine
    # slot) and the recorded list must be exactly CONFIG1_FALLBACK -- a missing one means mh.dll no
    # longer defines it (derive() dropped it), so configuration (1) would refuse to arm on the rig.
    spine_names = {r["mangled"] for r in data["spine"]}
    want_fb = [n for n, _why in CONFIG1_FALLBACK]
    for n, why in CONFIG1_FALLBACK:
        if n not in spine_names:
            errs.append(
                "configuration (1) fallback row %s (%s) is not a SPINE row -- it has no slot to "
                "bind into, so the spine-free harness mode has nothing to read the regions through"
                % (n, why)
            )
    if list(data.get("config1_fallback", [])) != want_fb:
        errs.append(
            "configuration (1) fallback list %s != %s. A row missing here is one mh.dll's objects "
            "no longer DEFINE, so mh.def cannot export it and the harness cannot arm in "
            "configuration (1)." % (data.get("config1_fallback"), want_fb)
        )
    if data.get("unresolved"):
        errs.append(
            "%d project symbol(s) mh_harness.dll references resolve in NEITHER image nor in itself: "
            "%s. That is a row with nowhere to come from -- the split cannot be completed until each "
            "is placed." % (len(data["unresolved"]), " ".join(data["unresolved"][:12]))
        )
    # THE CROSS-CHECK AGAINST THE HAND LIST. mh_harness_module.h carries the 12 rows WITH their
    # absent values, which a derivation cannot produce; this is what stops that hand list drifting.
    declared = parse_module_symbols(MODULE_HEADER)
    if sorted(declared) != sorted(exports):
        errs.append(
            "mh_harness_module.h's MH_HARNESS_MODULE_SYMBOLS does not match the derived export set.\n"
            "  only in the header: %s\n  only in the objects: %s\n"
            "Every row needs an ABSENT VALUE, which is a decision, so the list is hand-written -- and "
            "this check is why it cannot silently stop being the real set."
            % (sorted(set(declared) - set(exports)), sorted(set(exports) - set(declared)))
        )
    return errs


# ---------------------------------------------------------------------------------------------
# emission
# ---------------------------------------------------------------------------------------------
BANNER = "// GENERATED by tools/gen_harness_contract.py -- do not hand-edit; rerun the generator."


def render_h(data):
    out = [
        "#pragma once",
        BANNER,
        "//",
        "// mh_harness.dll's TWO import tables (fork F4E). HOST rows resolve out of mh.dll, SPINE rows",
        "// out of libmh.dll; harness_contract.gen.cpp defines both plus one signature-free naked",
        "// forwarding thunk per row, attached to the real symbol by a /alternatename linker directive",
        "// so not one of harness.cpp's call sites changes.",
        "",
        "#define MH_HARNESS_HOST_COUNT %d" % len(data["host"]),
        "#define MH_HARNESS_SPINE_COUNT %d" % len(data["spine"]),
        "",
        'extern "C" {',
        "// The bound addresses, in slot order. A null slot is not a configuration -- the binds below",
        "// resolve every row before adopting any, and the harness refuses to arm if either table came",
        "// back short. A thunk that finds one null therefore TRAPS rather than answering zero.",
        "extern void *g_mh_harness_host_fn[MH_HARNESS_HOST_COUNT];",
        "extern void *g_mh_harness_spine_fn[MH_HARNESS_SPINE_COUNT];",
        "extern const char *const g_mh_harness_host_name[MH_HARNESS_HOST_COUNT];",
        "extern const char *const g_mh_harness_spine_name[MH_HARNESS_SPINE_COUNT];",
        "",
        "// Resolve a table out of an ALREADY-LOADED module (GetModuleHandle, never LoadLibrary --",
        "// video.cpp:969's rule, adopted verbatim for this edge by docs/dll-split.md). Returns the",
        "// number of rows resolved; the caller refuses unless it equals the count above.",
        "int mh_harness_bind_host(void);",
        "int mh_harness_bind_spine(void);",
        "",
    ]
    fb_slot = {r["mangled"]: i for i, r in enumerate(data["spine"])}
    fb = list(data.get("config1_fallback", []))
    out += [
        "// mp:D29 -- THE CONFIGURATION (1) FALLBACK ROWS: the spine slots the per-step region hash",
        "// reaches, bound out of mh.dll (its own configuration (1) answer) when libmh.dll is absent.",
        "// Every other spine slot stays null in that mode; the call sites that could reach one are",
        "// guarded in harness.cpp or refused by key at arm. Bound whole or not at all (config1.h).",
        "#define MH_HARNESS_CONFIG1_FALLBACK_COUNT %d" % len(fb),
        "static const int mh_harness_config1_fallback_slot[MH_HARNESS_CONFIG1_FALLBACK_COUNT] = {",
    ]
    for n in fb:
        out.append("    %d, // %s" % (fb_slot.get(n, -1), n))
    out += [
        "};",
        "static const char *const "
        "mh_harness_config1_fallback_name[MH_HARNESS_CONFIG1_FALLBACK_COUNT] = {",
    ]
    for n in fb:
        out.append('    "%s",' % export_name(n))
    out += [
        "};",
        "int mh_harness_bind_config1(void);",
        "",
        "// NO CROSSING COUNTERS, and their absence is a decision rather than an omission. F4D's",
        "// exist because mh.dll's absent path answers zero in silence, so only a number separates a",
        "// lane that crossed from one that did not. This boundary cannot be in that state: every",
        "// row either crosses or traps, and an instrument that armed and called nothing wrote no",
        "// hashes, which check_arm_order refuses by name. Two stronger witnesses, and the hot",
        "// per-step path pays nothing for them.",
        "}",
        "",
    ]
    return "\n".join(out) + "\n"


THUNK = """
// %(pretty)s
extern "C" __declspec(naked) void mh_harness_%(side)s_thunk_%(i)d(void) {
    __asm {
        mov eax, dword ptr [g_mh_harness_%(side)s_fn + %(off)d]
        test eax, eax
        jz absent
        jmp eax
    absent:
        mov dword ptr [g_mh_harness_unbound_row], %(i)d
        mov dword ptr [g_mh_harness_unbound_side], %(sideid)d
        jmp mh_harness_unbound_trap
    }
}
#pragma comment(linker, "/alternatename:%(mangled)s=_mh_harness_%(side)s_thunk_%(i)d")
"""


def render_cpp(data):
    out = [
        BANNER,
        "//",
        "// THE FORWARDING SHIMS. Each row gets a signature-free naked stub that tail-jumps through its",
        "// table slot. A `jmp` through the slot is a perfect __cdecl tail call -- the arguments are",
        "// already on the caller's stack, the caller cleans them, and the return value comes back in",
        "// EAX/EDX/ST0 untouched -- so the thunk never needs the arity, the types or the return class.",
        "// It is three instructions on the bound path and carries no counter, for the reason the",
        "// generator's header gives: this boundary cannot be crossed silently, so there is nothing",
        "// for a counter to disambiguate and the hot per-step path pays nothing.",
        "// /alternatename attaches it to the real symbol at LINK time, which is also why that symbol",
        "// stays UNDEFINED in the object and the derivation keeps measuring the same set after the split.",
        "//",
        "// THE ABSENT PATH TRAPS, and that is the one thing this generator does differently from",
        "// gen_libmh_contract.py. There, `xor eax,eax` IS configuration (1). Here the harness refuses",
        "// to arm unless both tables resolved whole (ruling Q4), so reaching a null slot means an ARMED",
        "// instrument has discovered its own table is incomplete -- and an instrument that invents",
        "// numbers is the one failure a determinism harness must never have. Unreachable by",
        "// construction; loud if construction was wrong.",
        '#include "harness_contract.gen.h"',
        "",
        "#ifndef WIN32_LEAN_AND_MEAN",
        "#define WIN32_LEAN_AND_MEAN",
        "#endif",
        "#include <windows.h>",
        "",
        '#include "config1.h" // mp:D29 -- the all-or-nothing fallback binder',
        "",
        'extern "C" {',
        "void *g_mh_harness_host_fn[MH_HARNESS_HOST_COUNT]   = {0};",
        "void *g_mh_harness_spine_fn[MH_HARNESS_SPINE_COUNT] = {0};",
        "int g_mh_harness_unbound_row  = -1;",
        "int g_mh_harness_unbound_side = -1;",
        "",
        "const char *const g_mh_harness_host_name[MH_HARNESS_HOST_COUNT] = {",
    ]
    for r in data["host"]:
        out.append('    "%s",' % r["export"])
    out += ["};", "", "const char *const g_mh_harness_spine_name[MH_HARNESS_SPINE_COUNT] = {"]
    for r in data["spine"]:
        out.append('    "%s",' % r["export"])
    out += [
        "};",
        "} // extern \"C\"",
        "",
        "// The trap the absent path jumps to. Named, three channels, then dead -- the same reach as",
        "// mh::config::detail::refuse, and for the same reason: a process that died silently is",
        "// indistinguishable from one that crashed, and the operator would look in the wrong place.",
        'extern "C" __declspec(noreturn) void __cdecl mh_harness_unbound_trap(void) {',
        "    const int  i    = g_mh_harness_unbound_row;",
        "    const int  s    = g_mh_harness_unbound_side;",
        "    const char *tbl = s == 0 ? \"mh.dll (host)\" : \"libmh.dll (spine)\";",
        "    const char *nm  = \"(row index out of range)\";",
        "    if (s == 0 && i >= 0 && i < MH_HARNESS_HOST_COUNT) nm = g_mh_harness_host_name[i];",
        "    if (s == 1 && i >= 0 && i < MH_HARNESS_SPINE_COUNT) nm = g_mh_harness_spine_name[i];",
        "    char line[1024];",
        "    wsprintfA(line,",
        '              "mh_harness.dll CALLED AN UNBOUND CONTRACT ROW: %s slot %d of %s.\\r\\n"',
        '              "The instrument armed with an incomplete table, which the bind is supposed to "',
        '              "make impossible -- it resolves every row before adopting any. (In configuration "',
        '              "(1) only the mp:D29 fallback rows are bound, so a spine row reached there is a "',
        '              "harness.cpp call site the D29 guards missed.) Every number this "',
        '              "run would have reported is therefore untrustworthy, so the process is being "',
        '              "terminated rather than left to write a well-formed log of nothing.\\r\\n",',
        "              nm, i, tbl);",
        "    OutputDebugStringA(line);",
        "    HANDLE e = GetStdHandle(STD_ERROR_HANDLE);",
        "    if (e != nullptr && e != INVALID_HANDLE_VALUE) {",
        "        DWORD wrote = 0;",
        "        WriteFile(e, line, lstrlenA(line), &wrote, nullptr);",
        "    }",
        "    char dir[MAX_PATH];",
        "    GetModuleFileNameA(nullptr, dir, MAX_PATH);",
        "    {",
        "        char *slash = nullptr;",
        "        for (char *p = dir; *p != '\\0'; ++p)",
        "            if (*p == '\\\\' || *p == '/') slash = p;",
        "        if (slash != nullptr) slash[1] = '\\0';",
        "    }",
        "    char path[MAX_PATH];",
        '    wsprintfA(path, "%smh_harness_refused.log", dir);',
        "    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,",
        "                           FILE_ATTRIBUTE_NORMAL, nullptr);",
        "    if (h != INVALID_HANDLE_VALUE) {",
        "        DWORD wrote = 0;",
        "        WriteFile(h, line, lstrlenA(line), &wrote, nullptr);",
        "        CloseHandle(h);",
        "    }",
        "    TerminateProcess(GetCurrentProcess(), 4);",
        "    for (;;) {}",
        "}",
        "",
        "// ---- the binds -------------------------------------------------------------------------------",
        "//",
        "// GetModuleHandle, NEVER LoadLibrary. Both modules are already in the process by construction:",
        "// mh.dll is holding our handle, and libmh.dll was bound one call earlier by mh.dll's own",
        "// DllMain (which is why MH_HarnessModule_Init is handed `spine_bound` rather than guessing).",
        "// A lookup of an already-loaded module is the primitive seams/video.cpp:969 states the rule",
        "// for, and docs/dll-split.md pre-ruled it for exactly this edge.",
        'extern "C" int mh_harness_bind_host(void) {',
        '    HMODULE h = GetModuleHandleA("mh.dll");',
        "    if (h == nullptr) return 0;",
        "    void *t[MH_HARNESS_HOST_COUNT];",
        "    int   got = 0;",
        "    for (int i = 0; i < MH_HARNESS_HOST_COUNT; ++i) {",
        "        t[i] = (void *)GetProcAddress(h, g_mh_harness_host_name[i]);",
        "        if (t[i] != nullptr) ++got;",
        "    }",
        "    if (got != MH_HARNESS_HOST_COUNT) return got; // adopt the table only when it is WHOLE",
        "    for (int i = 0; i < MH_HARNESS_HOST_COUNT; ++i) g_mh_harness_host_fn[i] = t[i];",
        "    return got;",
        "}",
        "",
        'extern "C" int mh_harness_bind_spine(void) {',
        '    HMODULE h = GetModuleHandleA("libmh.dll");',
        "    if (h == nullptr) return 0;",
        "    void *t[MH_HARNESS_SPINE_COUNT];",
        "    int   got = 0;",
        "    for (int i = 0; i < MH_HARNESS_SPINE_COUNT; ++i) {",
        "        t[i] = (void *)GetProcAddress(h, g_mh_harness_spine_name[i]);",
        "        if (t[i] != nullptr) ++got;",
        "    }",
        "    if (got != MH_HARNESS_SPINE_COUNT) return got;",
        "    for (int i = 0; i < MH_HARNESS_SPINE_COUNT; ++i) g_mh_harness_spine_fn[i] = t[i];",
        "    return got;",
        "}",
        "",
        "// mp:D29 -- CONFIGURATION (1): the fallback rows, out of mh.dll, into their SPINE slots. Called",
        "// only when libmh.dll is absent. The binder is config1.h's, so the selftest drives the same",
        "// code with a planted resolver. mh.dll's export IS the function its own readers call, so the",
        "// harness reads mh.dll's REAL table -- never a copy of it.",
        'extern "C" int mh_harness_bind_config1(void) {',
        '    HMODULE h = GetModuleHandleA("mh.dll");',
        "    if (h == nullptr) return 0;",
        "    return mh::harness_cfg1::bind_fallback(",
        "        g_mh_harness_spine_fn, MH_HARNESS_SPINE_COUNT, mh_harness_config1_fallback_slot,",
        "        mh_harness_config1_fallback_name, MH_HARNESS_CONFIG1_FALLBACK_COUNT,",
        "        [h](const char *n) { return (void *)GetProcAddress(h, n); });",
        "}",
        "",
        'extern "C" {',
        "extern int g_mh_harness_unbound_row;",
        "extern int g_mh_harness_unbound_side;",
        "__declspec(noreturn) void __cdecl mh_harness_unbound_trap(void);",
        "}",
    ]
    for side, sideid in (("host", 0), ("spine", 1)):
        out.append("")
        out.append(
            "// ---- the %s rows (%d) --------------------------------------------------------"
            % (side.upper(), len(data[side]))
        )
        for i, r in enumerate(data[side]):
            out.append(
                THUNK
                % {
                    "pretty": r["pretty"] or r["mangled"],
                    "side": side,
                    "sideid": sideid,
                    "i": i,
                    "off": i * 4,
                    "mangled": r["mangled"],
                }
            )
    return "\n".join(out) + "\n"


def render_harness_def(data):
    # THE COUNT IS DERIVED, NOT TYPED. This banner said "the twelve" and "a thirteenth cannot arrive
    # unnoticed" -- and when the thirteenth did arrive (F5J's MH_Harness_StepFence) the generator
    # happily re-emitted both words over a list of 13 names. A hand-written number in a GENERATED
    # file is the one kind that nobody re-measures, because the file is not hand-maintained.
    n = len(data["exports"])
    out = [
        "; mh_harness.def -- mh_harness.dll's EXPORT LIST (fork F4E).",
        ";",
        "; GENERATED by tools/gen_harness_contract.py; do not hand-edit. The rows are the %d" % n,
        "; MH_Harness_* entry points mh.dll's objects leave UNDEFINED, i.e. exactly the surface the",
        "; instrument has to present -- derived, so a %dth cannot arrive unnoticed, and" % (n + 1),
        "; cross-checked against mh_harness_module.h's hand-written absent-value list.",
        ";",
        "; THE IMPORT LIBRARY THE LINKER DROPS BESIDE THIS IS NEVER LINKED, BY ANYONE. A single",
        "; mh_harness.lib on a future link line would put mh_harness.dll in that module's IMPORT TABLE,",
        "; and a missing file would then fail PROCESS LOAD before one instruction of ours ran (measured",
        "; at F4A: 0xC0000135, zero log directories). An uninstrumented run is the NORMAL case for every",
        "; player and almost every gate lane, so that is precisely the configuration that must not need",
        "; the file. mh.dll reaches every name below with GetProcAddress instead.",
        "LIBRARY mh_harness",
        "EXPORTS",
    ]
    for r in data["exports"]:
        out.append("    %s" % r["name"])
    out += [
        "",
        "; The module-level entries (mh/include/mh_harness_module.h). Not part of the instrument's API:",
        "; the ABI handshake plus the spine verdict, and the loader-order probe -- called directly",
        "; rather than through the table, the same arrangement mh_net.def and libmh.def have.",
        "    MH_HarnessModule_Init",
        "    MH_HarnessModule_Probe",
        "",
    ]
    return "\n".join(out) + "\n"


def render_mh_def(data):
    out = [
        "; mh.def -- the part of mh.dll's export list that mh_harness.dll imports (fork F4E).",
        ";",
        "; GENERATED by tools/gen_harness_contract.py from the two images' objects; do not hand-edit.",
        "; Every name is the symbol GetProcAddress is given, verbatim: a C++ row is the DECORATED name,",
        "; an extern \"C\" one is the decorated name without its leading underscore (see export_name()).",
        ";",
        "; MH.DLL'S THREE ORIGINAL EXPORTS ARE NOT HERE, DELIBERATELY. DecompressLZWData,",
        "; GetSightAreaFromRadius and MH_HostedPoolBase are __declspec(dllexport) in mh.c; MSVC unions",
        "; the two mechanisms and naming a symbol in both is LNK4197. MH_HostedPoolBase in particular",
        "; is the FORCE-LOAD ANCHOR every deployed patched exe imports BY NAME -- renaming or dropping",
        "; it would unload mh.dll from every already-patched exe, the rig VMs' included.",
        ";",
        "; A .def does not make mh.dll depend on anything. These are exports, not imports: the harness",
        "; resolves them with GetProcAddress against the handle it already has, so a run with no",
        "; mh_harness.dll costs this list nothing.",
        "EXPORTS",
    ]
    for r in data["host"]:
        out.append("    %s" % r["export"])
    fb = list(data.get("config1_fallback", []))
    if fb:
        out += [
            "",
            "; mp:D29 -- the CONFIGURATION (1) FALLBACK rows. These are SPINE rows (libmh.dll answers them",
            "; when it is present, and the harness binds them there), exported here so that with NO",
            "; libmh.dll the harness can bind mh.dll's own answer -- its region table seeded from REGIONS[]",
            "; and its empty owner table (seams/libmh_bind.cpp) -- and hash spine-free.",
        ]
        for n in fb:
            out.append("    %s" % export_name(n))
    out.append("")
    return "\n".join(out) + "\n"


def render_all(data):
    return {
        GEN_H_PATH: render_h(data),
        GEN_CPP_PATH: render_cpp(data),
        HARNESS_DEF_PATH: render_harness_def(data),
        MH_DEF_PATH: render_mh_def(data),
    }


def write_if_changed(path, text):
    old = io.open(path, encoding="utf-8", newline="").read() if os.path.isfile(path) else None
    if old == text:
        return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    io.open(path, "w", encoding="utf-8", newline="\n").write(text)
    return True


def load_committed():
    if not os.path.isfile(JSON_PATH):
        raise Refusal("%s does not exist -- run the generator once with a Debug build" % JSON_PATH)
    with io.open(JSON_PATH, encoding="utf-8") as fh:
        return json.load(fh)


def strip_meta(d):
    return {
        k: d[k] for k in ("host", "spine", "exports", "unresolved", "config1_fallback") if k in d
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="fail on drift instead of writing")
    ap.add_argument("--rederive", action="store_true", help="re-measure from the objects")
    ap.add_argument(
        "--selftest", action="store_true", help="planted inputs; every negative goes red"
    )
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    try:
        if args.rederive or not args.check:
            dumpbin = find_dumpbin()
            if dumpbin is None:
                raise Refusal("no dumpbin.exe found (machine_config VS_INSTALL_ROOT)")
            data = derive(dumpbin, find_undname())
            errs = validate(data)
            if errs:
                for e in errs:
                    print("gen_harness_contract: %s" % e)
                return 1
            if args.check:
                committed = strip_meta(load_committed())
                if strip_meta(data) != committed:
                    print(
                        "gen_harness_contract --rederive --check: THE OBJECTS DISAGREE WITH THE "
                        "COMMITTED CONTRACT."
                    )
                    for side in ("host", "spine", "exports"):
                        now = {r.get("mangled", r.get("name")) for r in data[side]}
                        was = {r.get("mangled", r.get("name")) for r in committed.get(side, [])}
                        if now != was:
                            print("  %-8s added: %s" % (side, sorted(now - was)))
                            print("  %-8s gone : %s" % (side, sorted(was - now)))
                    print("  Re-run `python tools/gen_harness_contract.py` and commit the result.")
                    return 1
                print(
                    "gen_harness_contract --rederive --check: contract CURRENT "
                    "(%d host + %d spine rows, %d exports)"
                    % (len(data["host"]), len(data["spine"]), len(data["exports"]))
                )
                return 0
        else:
            data = strip_meta(load_committed())
            errs = validate(data)
            if errs:
                for e in errs:
                    print("gen_harness_contract: %s" % e)
                return 1

        rendered = render_all(data)
        if args.check:
            # Newline-INSENSITIVE on purpose (2026-09-16): the private repo's CI run at 6ed4a1e0
            # reported both .def files STALE on the GitHub runner while the same tree passed here.
            # Git for Windows ships core.autocrlf=true in its SYSTEM gitconfig, .gitattributes had
            # no rule for *.def, so the runner checked the LF blobs out as CRLF and a raw compare
            # failed on line endings alone. .gitattributes now pins the four hand-derived .def files
            # to eol=lf as well; this normalisation is the belt to that brace -- a drift gate must
            # answer the same on every checkout config, or a red is a property of the machine.
            stale = [
                p
                for p, t in rendered.items()
                if (
                    io.open(p, encoding="utf-8", newline=None).read() if os.path.isfile(p) else None
                )
                != t.replace("\r\n", "\n")
            ]
            if stale:
                print(
                    "gen_harness_contract --check: STALE -- %s do not match the committed contract."
                    % ", ".join(os.path.relpath(p, REPO).replace("\\", "/") for p in stale)
                )
                print("  Re-run `python tools/gen_harness_contract.py`.")
                return 1
            print(
                "gen_harness_contract --check: the %d emitted files re-render identically "
                "(%d host + %d spine rows, %d exports)"
                % (len(rendered), len(data["host"]), len(data["spine"]), len(data["exports"]))
            )
            return 0

        changed = [p for p, t in rendered.items() if write_if_changed(p, t)]
        os.makedirs(os.path.dirname(JSON_PATH), exist_ok=True)
        io.open(JSON_PATH, "w", encoding="utf-8", newline="\n").write(
            json.dumps(strip_meta(data), indent=1, sort_keys=True) + "\n"
        )
        print(
            "gen_harness_contract: %d host + %d spine rows, %d exports; wrote %d file(s)"
            % (len(data["host"]), len(data["spine"]), len(data["exports"]), len(changed) + 1)
        )
        for p in changed:
            print("   %s" % os.path.relpath(p, REPO).replace("\\", "/"))
        return 0
    except Refusal as e:
        print("gen_harness_contract: REFUSED -- %s" % e)
        return 2


# ---------------------------------------------------------------------------------------------
# selftest -- every negative must go red
# ---------------------------------------------------------------------------------------------
def roster_selftest():
    """THE PLANTED STALE OBJECT (fork F5K's rider). A real build tree cannot be asked to hold one, so
    the arm builds a fake project + object directory and plants it there. The assertion is not that
    the run merely goes red -- a refusal saying "something is wrong" would leave the reader diffing
    two directory listings by hand -- but that the offending object is NAMED, on BOTH policies."""
    import tempfile

    bad = 0
    rows = []
    with tempfile.TemporaryDirectory() as td:
        proj = os.path.join(td, "fake.vcxproj")
        io.open(proj, "w", encoding="utf-8", newline="\n").write(
            "<Project><ItemGroup>\n"
            '  <ClCompile Include="..\\mh\\a.cpp" />\n'
            '  <ClCompile Include="b.cpp" />\n'
            '  <ClCompile Include="mh_calls.gen.cpp" />\n'
            "</ItemGroup></Project>\n"
        )
        objdir = os.path.join(td, "Debug")
        os.makedirs(objdir)

        def plant(*names):
            for f in os.listdir(objdir):
                os.remove(os.path.join(objdir, f))
            for n in names:
                io.open(os.path.join(objdir, n), "w").write("")

        def run(strict=True, p=None):
            try:
                return live_objs(objdir, p or proj, "the fake", strict=strict), None
            except Refusal as e:
                return None, str(e)

        # POSITIVE CONTROL FIRST: a refusal arm proves nothing if the function refuses everything.
        # mh_calls.gen is here on purpose -- the double extension is the shape a "simplified"
        # basename match gets wrong.
        plant("a.obj", "b.obj", "mh_calls.gen.obj")
        objs, err = run()
        rows.append(
            ("the exact roster passes, all 3 objects kept", err is None and len(objs) == 3, err)
        )

        # THE RIDER'S NEGATIVE, on the strict policy the two newly-rostered sets use.
        plant("a.obj", "b.obj", "mh_calls.gen.obj", "zz_deleted_tu.obj")
        objs, err = run()
        rows.append(
            (
                "strict: a planted STALE object is refused BY NAME",
                err is not None and "zz_deleted_tu.obj" in err,
                err,
            )
        )

        # AND ON THE TOLERANT POLICY the two pre-existing sets keep: it proceeds, but the object is
        # dropped from the measurement and NAMED on stdout -- silence there was the old behaviour.
        import contextlib

        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            objs, err = run(strict=False)
        said = buf.getvalue()
        rows.append(
            (
                "tolerant: a stale object is dropped AND named",
                err is None and len(objs) == 3 and "zz_deleted_tu.obj" in said,
                err or said.strip() or len(objs or []),
            )
        )

        # THE OTHER DIRECTION: a declared TU with no object must not measure a shorter contract.
        plant("a.obj", "mh_calls.gen.obj")
        objs, err = run()
        rows.append(
            (
                "a declared TU with no object is refused by name",
                err is not None and "b" in err and "build Debug" in err,
                err,
            )
        )

        # A ROSTER THAT READ AS EMPTY would classify every object as stale; refuse instead.
        empty = os.path.join(td, "empty.vcxproj")
        io.open(empty, "w", encoding="utf-8", newline="\n").write("<Project/>\n")
        plant("a.obj")
        objs, err = run(p=empty)
        rows.append(
            (
                "an empty/unreadable roster refuses rather than flagging all",
                err is not None and "declares no <ClCompile>" in err,
                err,
            )
        )

    for label, ok, detail in rows:
        print("  %-46s %s" % (label, "ok" if ok else "FAIL (got %r)" % (detail,)))
        bad += not ok
    return bad


def selftest():
    import copy

    base = None
    try:
        base = strip_meta(load_committed())
    except Refusal as e:
        print("gen_harness_contract --selftest: REFUSED -- %s" % e)
        return 2
    bad = 0

    def expect(name, data, want_err_substr):
        nonlocal bad
        errs = validate(data)
        hit = any(want_err_substr in e for e in errs)
        print("  %-46s %s" % (name, "RED (ok)" if hit else "GREEN -- THE CHECK IS BLIND"))
        if not hit:
            bad += 1

    def expect_clean(name, data):
        nonlocal bad
        errs = validate(data)
        print("  %-46s %s" % (name, "clean (ok)" if not errs else "RED: %s" % errs[:1]))
        if errs:
            bad += 1

    print("gen_harness_contract --selftest")
    expect_clean("the committed contract validates", copy.deepcopy(base))

    d = copy.deepcopy(base)
    d["host"] = [r for r in d["host"] if r["mangled"] != HOST_ANCHORS[0][0]]
    expect("host anchor removed", d, "positive control MISSING from the host")

    d = copy.deepcopy(base)
    d["spine"] = [r for r in d["spine"] if r["mangled"] != SPINE_ANCHORS[0][0]]
    expect("spine anchor removed", d, "positive control MISSING from the spine")

    d = copy.deepcopy(base)
    d["exports"] = [r for r in d["exports"] if r["name"] != "MH_Harness_Init"]
    expect("export anchor removed", d, "positive control MISSING from the export list")

    d = copy.deepcopy(base)
    d["exports"] = d["exports"] + [{"name": "MH_Harness_Invented"}]
    expect("a 13th export the header does not declare", d, "does not match the derived export set")

    d = copy.deepcopy(base)
    d["unresolved"] = ["?nowhere@mh@@YAXXZ"]
    expect("a symbol that resolves in neither image", d, "resolve in NEITHER image")

    d = copy.deepcopy(base)
    if d["host"]:
        d["host"] = d["host"] + [dict(d["host"][0])]
        expect("a duplicated host slot name", d, "is used by 2 rows")

    d = copy.deepcopy(base)
    if d["spine"]:
        d["spine"][0] = dict(d["spine"][0])
        d["spine"][0]["pretty"] = "int __stdcall mh::sim::whatever(void)"
        d["spine"][0]["mangled"] = "?whatever@sim@mh@@YGHXZ"
        d["spine"][0]["slot"] = "mh_sim_whatever_stdcall"
        expect("a non-__cdecl row", d, "is not __cdecl")

    # mp:D29 -- the configuration (1) fallback class.
    d = copy.deepcopy(base)
    d["config1_fallback"] = [n for n in d["config1_fallback"] if "live@state" not in n]
    expect("a fallback row mh.dll stopped defining", d, "configuration (1) fallback list")

    d = copy.deepcopy(base)
    d["spine"] = [r for r in d["spine"] if "owner_table@state" not in r["mangled"]]
    expect("a fallback row that is not a spine row", d, "is not a SPINE row")

    d = copy.deepcopy(base)
    d["config1_fallback"] = d["config1_fallback"] + ["?rng_trace_count@sim@mh@@YAHXZ"]
    expect("an extra fallback row (no spine-free answer)", d, "configuration (1) fallback list")

    rendered = render_all(base)
    fb_exports = [export_name(n) for n, _why in CONFIG1_FALLBACK]
    ok = all(("    %s\n" % e) in rendered[MH_DEF_PATH] for e in fb_exports)
    print(
        "  %-46s %s"
        % ("mh.def exports every fallback row", "ok" if ok else "FAIL -- mh.def lacks one")
    )
    bad += 0 if ok else 1
    ok = "mh_harness_bind_config1" in rendered[GEN_CPP_PATH] and (
        "MH_HARNESS_CONFIG1_FALLBACK_COUNT %d" % len(CONFIG1_FALLBACK) in rendered[GEN_H_PATH]
    )
    print("  %-46s %s" % ("the binder + slot table are emitted", "ok" if ok else "FAIL"))
    bad += 0 if ok else 1

    bad += roster_selftest()

    print("gen_harness_contract --selftest: %s" % ("PASS" if bad == 0 else "FAIL (%d blind)" % bad))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
