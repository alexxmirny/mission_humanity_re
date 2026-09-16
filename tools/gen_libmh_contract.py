#!/usr/bin/env python3
# gen_libmh_contract.py -- fork F4D: the mh.dll <-> libmh.dll EXPORT CONTRACT, derived and gated.
#
# THE PROBLEM. F4D takes the 627-TU spine out of mh.dll and into libmh.dll. mh.dll's remaining 39
# TUs still name ~100 of the spine's symbols; after the split every one of them has to be resolved
# at RUNTIME (config (1) is a shipped configuration in which libmh.dll is simply not there), which
# means a LoadLibrary + one GetProcAddress per symbol and, on mh.dll's side, a definition of each
# symbol that forwards through the resulting table.
#
# docs/dll-split.md says that list "should be BOUND BY A GENERATOR, not by hand" and names the
# failure it is avoiding: G106, the hand-maintained list that looks complete, is checked by nobody,
# and whose first forgotten row is a call through a null pointer. mh_net.dll's 23 rows were small
# enough to carry as an X-macro a human reads; ~100 C++ symbols with mangled names are not.
#
# ---- WHERE THE LIST COMES FROM ------------------------------------------------------------------
#
# NOT from a scan of source, and not from a hand list. It is the INTERSECTION of two object-level
# facts, measured with dumpbin over the two images' Debug objects:
#
#     { external symbols UNDEFINED in mh.dll's objects }
#   & { external symbols DEFINED   in libmh.dll's objects }
#
# That is exactly "what mh.dll needs and libmh has", which is what an export contract IS. It cannot
# drift away from either side, and it sees what no source scan can: F4D-PRE measured the reverse
# edge and found its largest row -- mh::hook::install_export_ok, wanted by 22 TUs -- named by NOT
# ONE of them, because it arrives through the MH_EXPORT_REPLACE macro. The same blindness applies
# in this direction.
#
# The forwarding shims mh.dll defines do NOT disturb the measurement, and that is a property of the
# mechanism rather than luck: a shim is a naked stub with a C name, attached to the mangled symbol
# by a `/alternatename` linker directive. The mangled symbol stays UNDEFINED in the object and is
# substituted at LINK time, so re-deriving the contract after the split measures the same set.
#
# ---- HOW A SYMBOL WITH NO SIGNATURE GETS A DEFINITION -------------------------------------------
#
# The obvious shape -- write 97 C++ forwarders -- needs 97 exact signatures, and a signature typed
# from a demangled string is a transcription with 97 chances to be subtly wrong. So the shims carry
# no types at all:
#
#     extern "C" __declspec(naked) void mh_libmh_thunk_N(void) {
#         __asm { mov eax, [g_libmh_fn + N*4]   ; test eax,eax ; jz absent
#                 inc [g_libmh_crossings]       ; jmp eax
#           absent: inc [g_libmh_absent_calls]  ; xor eax,eax ; ret }
#     }
#     #pragma comment(linker, "/alternatename:?mangled@...=_mh_libmh_thunk_N")
#
# A `jmp` through the slot is a perfect __cdecl tail call: arguments are already on the caller's
# stack, the caller cleans them, and the return value comes back in EAX/EDX/ST0 untouched. The
# thunk never needs to know the arity, the types, or the return class. Measured working under
# mh.dll's own Release flags (/O2 /GL + /LTCG) before this generator was written.
#
# TWO COUNTERS, AND THEY ARE THE ITEM'S STANDING ARM. `crossings` is incremented on the bound path
# and `absent_calls` on the other, so "did this run actually go through the DLL boundary" stops
# being a thing a reader infers from a bind line and becomes a number. The recorded miss F4D exists
# to fix is that LIB-SPINE-API proved the crossing live ONCE and never made it a gate; a lane that
# binds and then never crosses now reds instead of reading green.
#
# ---- THE ROWS THAT CANNOT BE A NAKED THUNK ------------------------------------------------------
#
# `xor eax,eax; ret` is the right absent answer for a row that returns void, an integer, a bool or
# a pointer -- and for this contract that is not a convenience, it is the DEFINITION of config (1):
# no promotion installs, no observer fires, nothing is instrumented, so the original binary's own
# bodies run. Two classes of row cannot use it, and the generator REFUSES rather than guesses:
#
#   MECHANICAL  the demangled return type is a REFERENCE or a by-value struct. EAX=0 for a
#               reference is a null the caller dereferences; a by-value struct returns through a
#               hidden pointer the stub would leave unfilled. DERIVED from the demangled signature,
#               never listed -- a new row of this shape fails the generator by name.
#   SEMANTIC    zero is a legal value of the right type but the WRONG answer. These are listed in
#               SEMANTIC_HAND below, each with its reason, and each one is a written decision about
#               what config (1) means for that row.
#
# Both classes are defined by hand in mh/seams/libmh_bind.cpp, which includes the real headers, so
# the compiler checks the signature instead of a human. They still get a table slot, a .def row and
# a name: the only thing they skip is the generated stub.
#
# ---- OUTPUTS ------------------------------------------------------------------------------------
#
#   tools/data/libmh_contract.json          the committed list (the done_when's "export list
#                                           committed and drift-gated")
#   src/mh_dll/libmh_dll/libmh.def          libmh.dll's exports, one per row (see export_name():
#                                           a C++ symbol is exported decorated, an extern "C" one
#                                           without its leading underscore, and the bind's
#                                           GetProcAddress string is the SAME name)
#   src/mh_dll/mh/seams/libmh_contract.gen.h    the count, the slot enum, the table declarations
#   src/mh_dll/mh/seams/libmh_contract.gen.cpp  the table, the names, the thunks, the pragmas
#
# ---- MODES --------------------------------------------------------------------------------------
#
#   python tools/gen_libmh_contract.py              re-derive from objects and write everything
#   python tools/gen_libmh_contract.py --check      SOURCE-ONLY: re-render the three emitted files
#                                                   from the committed json and fail on a diff.
#                                                   Needs no build -- this is the lint_repo row.
#   python tools/gen_libmh_contract.py --rederive --check
#                                                   re-measure from the objects and fail if the
#                                                   committed json disagrees. Needs both Debug
#                                                   builds; run_gate runs it after the build, the
#                                                   same place --subset runs.
#   python tools/gen_libmh_contract.py --selftest   planted inputs; every negative goes red.
#
# Exit 0 = current. 1 = stale/residue. 2 = the tool cannot be trusted (objects missing, no dumpbin,
# a positive control gone). NEVER read a 2 as "the contract is empty".

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

MH_OBJDIR = os.path.join(SRC, "mh", "Debug")
# PINNED by libmh_dll.vcxproj (<IntDir>$(Configuration)\</IntDir>) so this constant cannot be
# invalidated by a toolset changing its mind about $(Platform) subdirectories -- see the note in
# the generated project. Debug rather than Release because a /GL Release object is an ANONYMOUS
# OBJECT that dumpbin cannot read; this project does not use /GL, but Debug is the set
# check_libmh_outbound.py already standardised on and keeping one answer costs nothing.
LIB_OBJDIR = os.path.join(SRC, "libmh_dll", "Debug")

JSON_PATH = os.path.join(REPO, "tools", "data", "libmh_contract.json")
DEF_PATH = os.path.join(SRC, "libmh_dll", "libmh.def")
GEN_H_PATH = os.path.join(SRC, "mh", "seams", "libmh_contract.gen.h")
GEN_CPP_PATH = os.path.join(SRC, "mh", "seams", "libmh_contract.gen.cpp")

# A PROJECT symbol, derived from the decorated name rather than listed -- the same test
# check_libmh_outbound.py uses, and for the same reason: a new CRT helper must not read as a new
# contract row. MSVC mangles a C++ name in namespace `mh` with an `@mh@@` segment; the project's C
# surface is MH_* / libmh_*.
PROJECT_RE = re.compile(r"@mh@@|^_?MH_[A-Za-z]|^_?libmh_[a-z]")

# The generated marshalling thunks (Q5's per-image shim, addr/mh_calls.gen.cpp). BOTH images compile
# that TU, so its `mh::call::detail::s_*` shapes are defined on each side and are not a contract row
# -- the same arrangement F4B set for mh_net_proto's session_info.cpp. Excluded explicitly so a
# future reader does not have to work out why they are absent from a list they are eligible for.
SHIM_RE = re.compile(r"@detail@call@mh@@")

# ---- the rows that are defined BY HAND in mh/seams/libmh_bind.cpp --------------------------------
#
# MECHANICAL hand-ness is derived below from the demangled return type. This list is the SEMANTIC
# half: rows where `xor eax,eax` would compile, run, and be wrong. Each entry is a decision about
# what config (1) means for that row, and the generator fails if one of them stops being a contract
# row (a stale entry reads exactly like a live decision -- F4D-PRE's own lesson).
SEMANTIC_HAND = {
    "?install_export_ok@hosthook@mh@@YA_NIPAXPBD_K@Z": (
        "EVERY MH_EXPORT_REPLACE site routes through this accessor, including mh.dll's own two "
        "(harness.cpp:2325 the wall-clock pin, reimpl_probe.cpp:96 utils_w_strlen). It lives in "
        "libmh and forwards to mh.dll's hook table, so with the module absent a zero answer would "
        "refuse mh.dll's OWN installs -- which have nothing to do with the spine. The absent "
        "answer calls mh::hook::install_export_ok directly, which is what the bound path reaches "
        "anyway after a round trip."
    ),
    "?live@state@mh@@YAAAUlive_table@12@XZ": (
        "The state region registry. ONE table per PROCESS: mh::ai::island_move() rebases 48 "
        "regions from inside libmh and mh.dll's seams read live_base() for dozens. Absent, "
        "mh.dll's own static IS the registry and nothing can rebase it."
    ),
    "?owner_table@state@mh@@YAPAUowner_slot@12@XZ": (
        "Region ownership, claimed from roster TUs (ai_state.cpp 48 regions, order_queue.cpp 2) "
        "and read by desync_watch.cpp / harness.cpp. A null table would fault; absent, mh.dll's "
        "own empty table is correct, because with no libmh nothing has claimed anything."
    ),
    "?owner_count@state@mh@@YAAAHXZ": (
        "The count beside owner_table(), and a reference return, so it is mechanically hand too. "
        "Listed here as well because the two must stay the same object."
    ),
}

# POSITIVE CONTROLS. A derivation that reports a plausible list has to prove it can still see the
# shapes it is supposed to see. Re-pick rule (F3C/F3D, restated in check_libmh_outbound.py): if an
# item genuinely removes one of these rows, DELETE the anchor in the same commit -- do not widen
# the tool to tolerate its absence.
ANCHORS = [
    (
        "?install_promotion_dispatch@sim@mh@@YAHH@Z",
        "the order-dispatch promotion installer -- the [promote] spine's single largest arm",
    ),
    (
        "_libmh_set_host_api",
        "the C facade's host-callback bind; if this is not a row, mh.dll is not reaching libmh",
    ),
    (
        "?install_export_ok@hosthook@mh@@YA_NIPAXPBD_K@Z",
        "F4D-PRE's macro-introduced row, invisible to any source scan",
    ),
]

RETURN_HAND_RE = re.compile(r"^(?:class|struct|union)\s[^(]*?(?<![*&])\s__cdecl\s|&\s*__cdecl\s")


def find_dumpbin():
    from check_module_bind import find_dumpbin as f

    return f()


def find_undname():
    import machine_config as mc

    root = mc.VS_INSTALL_ROOT
    base = os.path.join(root, "VC", "Tools", "MSVC")
    if not os.path.isdir(base):
        return None
    for ver in sorted(os.listdir(base), reverse=True):
        for host in ("Hostx64", "Hostx86"):
            p = os.path.join(base, ver, "bin", host, "x86", "undname.exe")
            if os.path.isfile(p):
                return p
    return None


class Refusal(Exception):
    """Exit 2: this run cannot be trusted."""


# ---------------------------------------------------------------------------------------------
# derivation
# ---------------------------------------------------------------------------------------------
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


def live_objs(objdir, vcxproj, what):
    """Objects belonging to TUs the project still compiles. An object left behind by a TU the
    project dropped is STALE, and a stale object is a measurement of a tree that no longer exists
    (fork F4D R5: eight F2-era objects in mh/Debug put two phantom rows in F4D-PRE's own count)."""
    text = io.open(vcxproj, encoding="utf-8").read()
    want = {
        os.path.splitext(os.path.basename(x))[0]
        for x in re.findall(r'<ClCompile Include="([^"]+)"', text)
    }
    keep, stale = [], []
    for p in objs_in(objdir, what):
        (keep if os.path.splitext(os.path.basename(p))[0] in want else stale).append(
            os.path.basename(p)
        )
    keep = [os.path.join(objdir, b) for b in keep]
    missing = len(want) - len(keep)
    if missing > 0:
        raise Refusal(
            "%d of %s's %d TUs have no object in %s -- build Debug|Win32 first"
            % (missing, os.path.basename(vcxproj), len(want), objdir)
        )
    return keep, sorted(stale)


def demangle(undname, names):
    out = {}
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
    """THE NAME GetProcAddress MUST BE GIVEN, which is not always the object's symbol.

    A C++ symbol is exported under its decorated name verbatim. An `extern "C"` __cdecl symbol is
    DECORATED with a leading underscore in the object file and EXPORTED without one -- the .def
    names it undecorated and the linker does the matching. So the two differ for exactly the 14 C
    rows of this contract, and the difference is not cosmetic: it is the string the bind passes to
    GetProcAddress.

    MEASURED, NOT REASONED. The first build of this generator used the decorated name for both, and
    the first real boot reported `LOADED BUT REFUSED -- resolved 87 of 101 contract symbols`,
    naming all fourteen `_libmh_*` rows. The bind refused the module whole and the game ran
    configuration (1) -- correctly, loudly, and with the boot continuing to 268k frames. Worth
    recording as the shape rather than the bug: an export list that is WRONG produces a named
    refusal here, not a crash and not a silent fallback, because the bind resolves every row before
    adopting any of them and the crossing arm then reports 0.
    """
    return mangled if mangled.startswith("?") else mangled.lstrip("_")


def slot_name(mangled, pretty):
    """A stable, readable C identifier per row. From the DEMANGLED qualified name where there is
    one (`mh::sim::install_promotion_batch_h` -> `mh_sim_install_promotion_batch_h`), else from the
    C name. Collisions are checked by the caller rather than assumed away."""
    m = re.search(r"([A-Za-z_][A-Za-z0-9_:]*)\s*\(", pretty or "")
    base = m.group(1) if m else mangled.lstrip("_?")
    return re.sub(r"[^A-Za-z0-9_]", "_", base.replace("::", "_"))


def classify_hand(mangled, pretty):
    """-> (is_hand, reason). MECHANICAL first (derived), then SEMANTIC (listed)."""
    if mangled in SEMANTIC_HAND:
        return True, "semantic"
    if pretty and RETURN_HAND_RE.search(pretty):
        return True, "return-type"
    return False, ""


def derive(mh_objs, lib_objs, dumpbin, undname):
    _mdef, mundef = dump_symbols(dumpbin, mh_objs)
    ldef, _lundef = dump_symbols(dumpbin, lib_objs)

    names = set()
    for name in mundef:
        if name in ldef and PROJECT_RE.search(name) and not SHIM_RE.search(name):
            names.add(name)
    # The hand-defined rows are DEFINED in mh.dll, so they are not undefined there and the
    # intersection above cannot see them. They are still contract rows: libmh must export them or
    # the hand shim has nothing to forward to.
    for name in SEMANTIC_HAND:
        if name in ldef:
            names.add(name)

    pretty = demangle(undname, sorted(names)) if undname else {}

    rows = []
    for name in sorted(names):
        p = pretty.get(name, "")
        hand, why = classify_hand(name, p)
        rows.append(
            {
                "mangled": name,
                "export": export_name(name),
                "pretty": p,
                "slot": slot_name(name, p),
                "hand": hand,
                "hand_reason": why,
                "defined_in": sorted(ldef[name]),
                "used_by": sorted(mundef.get(name, [])),
            }
        )
    return rows


def validate(rows):
    """Everything that must be true of a contract before it is committed."""
    errs = []
    seen = collections.Counter(r["slot"] for r in rows)
    for slot, n in seen.items():
        if n > 1:
            errs.append("slot name %s is used by %d rows -- disambiguate slot_name()" % (slot, n))
    for r in rows:
        if r["pretty"] and "__cdecl" not in r["pretty"] and r["mangled"].startswith("?"):
            errs.append(
                "%s is not __cdecl (%s). The naked forwarding thunk is a __cdecl tail call and "
                "cannot serve another convention." % (r["mangled"], r["pretty"])
            )
        if r["hand"] and r["hand_reason"] == "semantic" and r["mangled"] not in SEMANTIC_HAND:
            errs.append("%s claims a semantic hand rule that is not listed" % r["mangled"])
    live = {r["mangled"] for r in rows}
    for name in SEMANTIC_HAND:
        if name not in live:
            errs.append(
                "SEMANTIC_HAND names %s, which is no longer a contract row. A stale adjudication "
                "reads exactly like a live decision -- delete the entry in the same commit that "
                "removed the row." % name
            )
    for name, why in ANCHORS:
        if name not in live:
            errs.append(
                "positive control MISSING: %s (%s). Either the derivation is broken or the anchor "
                "died with a change -- re-pick it in the SAME commit rather than dropping it."
                % (name, why)
            )
    return errs


# ---------------------------------------------------------------------------------------------
# emission
# ---------------------------------------------------------------------------------------------
BANNER = "// GENERATED by tools/gen_libmh_contract.py -- do not hand-edit; rerun the generator."


HARNESS_JSON = os.path.join(REPO, "tools", "data", "harness_contract.json")


def harness_spine_exports():
    """The spine rows mh_harness.dll resolves out of libmh.dll, which libmh.def must ALSO export.

    FORK F4E, AND IT IS A MEASURED CORRECTION RATHER THAN A REFINEMENT. This contract's derivation
    asks what MH.DLL needs, which was the whole story while mh.dll was the only image reaching the
    spine. It is not any more: mh_harness.dll reads state regions, the RNG trace and the promotion
    predicates straight out of libmh, and only 10 of its 30 rows overlap mh.dll's. Emitting the
    intersection alone produced exactly the failure the instrument's bind is built to make loud --
    the first armed boot after the split printed `Resolved 10 of 30 libmh.dll spine symbols` and
    refused to instrument the run. So the EXPORT LIST is the union; the SLOT TABLE is not, because
    mh.dll has neither a thunk nor a caller for the twenty rows that are only the instrument's.

    Read from the committed json rather than re-derived, because this file is emitted by `--check`
    too and that is a lint row, which may not build anything."""
    if not os.path.isfile(HARNESS_JSON):
        return []
    with io.open(HARNESS_JSON, encoding="utf-8") as fh:
        data = json.load(fh)
    return sorted(r["export"] for r in data.get("spine", []))


def render_def(rows):
    out = [
        "; libmh.def -- libmh.dll's EXPORT LIST (fork F4D).",
        ";",
        "; GENERATED by tools/gen_libmh_contract.py from the two images' objects; do not hand-edit.",
        "; Every name is the DECORATED symbol, verbatim -- mh.dll resolves each with GetProcAddress",
        "; using exactly this string, so an alias would be one more mapping to get wrong. The order",
        "; is the committed order (sorted by decorated name) and it is the same order as the slot",
        "; enum in mh/seams/libmh_contract.gen.h, so a row added in the middle is one diff.",
        ";",
        "; THE IMPORT LIBRARY THE LINKER DROPS BESIDE THIS IS NEVER LINKED, BY ANYONE. A single",
        "; libmh_dll.lib on a future link line would put libmh.dll in that module's IMPORT TABLE, and",
        "; a missing file would then fail PROCESS LOAD before one instruction of ours ran (measured",
        "; at F4A: 0xC0000135, zero log directories). Config (1) is precisely the configuration where",
        "; the file is absent. mh.dll reaches every name below with GetProcAddress instead, and",
        "; tools/check_module_bind.py --subset reads mh.dll's real import table, where libmh.dll",
        "; appearing would be the break. (mh_net.dll has the identical arrangement.)",
        "LIBRARY libmh",
        "EXPORTS",
    ]
    for r in rows:
        out.append("    %s" % r["export"])
    mine = {r["export"] for r in rows}
    extra = [e for e in harness_spine_exports() if e not in mine]
    if extra:
        out += [
            "",
            "; ---- THE INSTRUMENT'S OWN ROWS (fork F4E) ----------------------------------------",
            ";",
            "; mh_harness.dll reads the sim's state regions, the RNG trace and the promotion",
            "; predicates straight out of this module, through its own GetModuleHandle +",
            "; GetProcAddress table (tools/gen_harness_contract.py). They are NOT part of the",
            "; mh.dll contract above -- mh.dll has neither a caller nor a thunk for them -- but they",
            "; have to be EXPORTED or the instrument cannot resolve them. Measured the hard way: the",
            "; first armed boot after the split printed `Resolved 10 of 30 libmh.dll spine symbols`",
            "; and refused to instrument the run, which is the refusal working and this list being",
            "; wrong.",
        ]
        for e in extra:
            out.append("    %s" % e)
    out += [
        "",
        "; The module-level entries (libmh/include/libmh_module.h). Not part of the spine contract:",
        "; the ABI handshake and the loader-order probe, which mh.dll calls directly rather than",
        "; through the table -- the same arrangement mh_net.def has for MH_NetModule_Init/Probe.",
        "    libmh_module_init",
        "    libmh_module_probe_read",
        "",
    ]
    return "\n".join(out)


def render_h(rows):
    n = len(rows)
    out = [
        "#pragma once",
        BANNER,
        "//",
        "// The mh.dll side of the F4D export contract: one table slot per libmh symbol mh.dll",
        "// needs, plus the two crossing counters that make 'did this run go through the DLL",
        "// boundary' a number instead of an inference. mh/seams/libmh_bind.cpp fills the table;",
        "// libmh_contract.gen.cpp defines it and the forwarding thunks over it.",
        "",
        "#define MH_LIBMH_CONTRACT_COUNT %d" % n,
        "",
        "// Slot indices, in the committed order (= libmh.def's order). Only the hand-defined rows",
        "// name theirs in source; the rest are reached by the generated thunks.",
        "enum mh_libmh_slot {",
    ]
    for i, r in enumerate(rows):
        out.append("    MH_LIBMH_SLOT_%s = %d," % (r["slot"], i))
    out += [
        "};",
        "",
        'extern "C" {',
        "// The bound addresses, one per row, in slot order. A null slot means 'libmh did not",
        "// export this row' -- which the bind treats as a refusal of the whole module, so at",
        "// runtime the table is either entirely filled or entirely null.",
        "extern void *g_libmh_fn[MH_LIBMH_CONTRACT_COUNT];",
        "extern const char *const g_libmh_fn_name[MH_LIBMH_CONTRACT_COUNT];",
        "// THE STANDING ARM (F4D done_when). Incremented inside the thunks, so they count real",
        "// calls rather than intentions: crossings on the bound path, absent_calls on the other.",
        "// A brokered lane with crossings == 0 has not exercised the spine boundary, whatever its",
        "// bind line says, and tools/check_module_bind.py --libmh reds on it.",
        "extern unsigned g_libmh_crossings;",
        "extern unsigned g_libmh_absent_calls;",
        "}",
        "",
    ]
    return "\n".join(out)


def render_cpp(rows):
    out = [
        BANNER,
        "//",
        "// THE FORWARDING SHIMS. Each non-hand row gets a signature-free naked stub that tail-jumps",
        "// through its table slot, attached to the mangled C++ symbol by a /alternatename linker",
        "// directive. See tools/gen_libmh_contract.py's header for why the shims carry no types and",
        "// why `xor eax,eax; ret` is the correct absent answer for every row that reaches it.",
        "//",
        "// Rows defined BY HAND in mh/seams/libmh_bind.cpp are listed at the bottom of this file as",
        "// comments, so the two halves of the contract are readable in one place.",
        '#include "seams/libmh_contract.gen.h"',
        "",
        'extern "C" {',
        "void *g_libmh_fn[MH_LIBMH_CONTRACT_COUNT] = {0};",
        "unsigned g_libmh_crossings    = 0;",
        "unsigned g_libmh_absent_calls = 0;",
        "const char *const g_libmh_fn_name[MH_LIBMH_CONTRACT_COUNT] = {",
    ]
    for r in rows:
        out.append('    "%s",' % r["export"])
    out += ["};", "}", ""]

    for i, r in enumerate(rows):
        if r["hand"]:
            continue
        out += [
            "// %s" % (r["pretty"] or r["mangled"]),
            'extern "C" __declspec(naked) void mh_libmh_thunk_%d(void) {' % i,
            "    __asm {",
            "        mov eax, dword ptr [g_libmh_fn + %d]" % (i * 4),
            "        test eax, eax",
            "        jz absent",
            "        inc dword ptr [g_libmh_crossings]",
            "        jmp eax",
            "    absent:",
            "        inc dword ptr [g_libmh_absent_calls]",
            "        xor eax, eax",
            "        ret",
            "    }",
            "}",
            '#pragma comment(linker, "/alternatename:%s=_mh_libmh_thunk_%d")' % (r["mangled"], i),
            "",
        ]

    hand = [r for r in rows if r["hand"]]
    out.append(
        "// ---- defined by hand in mh/seams/libmh_bind.cpp (%d rows) -----------" % len(hand)
    )
    for r in hand:
        out.append(
            "//   [%d] %s   (%s)" % (rows.index(r), r["pretty"] or r["mangled"], r["hand_reason"])
        )
    out.append("")
    return "\n".join(out)


def write_if_changed(path, text):
    old = io.open(path, encoding="utf-8").read() if os.path.exists(path) else None
    if old == text:
        return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    io.open(path, "w", encoding="utf-8", newline="\n").write(text)
    return True


EMITTED = ((DEF_PATH, render_def), (GEN_H_PATH, render_h), (GEN_CPP_PATH, render_cpp))


def load_rows():
    if not os.path.exists(JSON_PATH):
        raise Refusal("%s does not exist -- run the generator with a built tree" % JSON_PATH)
    return json.load(io.open(JSON_PATH, encoding="utf-8"))["rows"]


# ---------------------------------------------------------------------------------------------
def selftest():
    fails = []

    def ck(what, ok):
        if not ok:
            fails.append(what)
        print("  %-74s %s" % (what, "ok" if ok else "FAIL"))

    ck(
        "a reference return is mechanically hand",
        classify_hand("?x@y@mh@@", "struct mh::lockstep::reimpl_fixes const & __cdecl mh::x(void)")[
            0
        ],
    )
    ck(
        "a by-value struct return is mechanically hand",
        classify_hand("?s@t@mh@@", "struct mh::tact::tact_state __cdecl mh::tact::state(void)")[0],
    )
    ck(
        "a pointer return is NOT hand",
        not classify_hand("?p@q@mh@@", "char const * __cdecl mh::save::last_save_path(void)")[0],
    )
    ck(
        "a void return is NOT hand",
        not classify_hand("?v@w@mh@@", "void __cdecl mh::ai::ai_say(char const *,...)")[0],
    )
    ck(
        "an int return is NOT hand",
        not classify_hand("?i@j@mh@@", "int __cdecl mh::sim::install_promotion_batch_h(int)")[0],
    )
    ck(
        "a listed semantic row is hand whatever its return type",
        classify_hand(
            "?install_export_ok@hosthook@mh@@YA_NIPAXPBD_K@Z",
            "bool __cdecl mh::hosthook::install_export_ok(unsigned int,void *,char const *,"
            "unsigned __int64)",
        )
        == (True, "semantic"),
    )
    ck(
        "a CRT symbol is not a project symbol",
        not PROJECT_RE.search("_memset") and not PROJECT_RE.search("__imp__WriteFile@20"),
    )
    ck("a mh:: symbol is a project symbol", bool(PROJECT_RE.search("?f@sim@mh@@YAHXZ")))
    ck("a libmh_ C symbol is a project symbol", bool(PROJECT_RE.search("_libmh_set_host_api")))
    ck("a Q5 marshalling shape is excluded", bool(SHIM_RE.search("?s_void@detail@call@mh@@YAXI@Z")))

    # validate() must refuse each failure mode by name
    base = [
        {
            "mangled": m,
            "pretty": "",
            "slot": "s%d" % i,
            "hand": m in SEMANTIC_HAND,
            "hand_reason": "semantic" if m in SEMANTIC_HAND else "",
            "defined_in": [],
            "used_by": [],
        }
        for i, (m, _w) in enumerate(ANCHORS)
    ]
    base += [
        {
            "mangled": m,
            "pretty": "",
            "slot": "h%d" % i,
            "hand": True,
            "hand_reason": "semantic",
            "defined_in": [],
            "used_by": [],
        }
        for i, m in enumerate(SEMANTIC_HAND)
        if m not in dict(ANCHORS)
    ]
    ck("a complete planted table validates", validate(base) == [])
    ck(
        "a missing positive control fails by name",
        any("positive control MISSING" in e for e in validate(base[1:])),
    )
    dup = [dict(r) for r in base]
    dup[1]["slot"] = dup[0]["slot"]
    ck("a duplicate slot name fails", any("slot name" in e for e in validate(dup)))
    stale = [r for r in base if r["mangled"] != next(iter(SEMANTIC_HAND))]
    ck(
        "a SEMANTIC_HAND entry that is no longer a row fails",
        any("no longer a contract row" in e for e in validate(stale)),
    )
    bad = [dict(r) for r in base]
    bad[0] = dict(bad[0])
    bad[0]["pretty"] = "int __stdcall mh::sim::f(int)"
    ck("a non-cdecl row fails", any("not __cdecl" in e for e in validate(bad)))

    print("\n%s (%d checks, %d failed)" % ("FAIL" if fails else "PASS", 15, len(fails)))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description="the mh.dll <-> libmh.dll export contract")
    ap.add_argument("--check", action="store_true", help="fail if an emitted file is stale")
    ap.add_argument(
        "--rederive",
        action="store_true",
        help="re-measure the contract from the objects (needs both Debug builds)",
    )
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--mh-objdir", default=MH_OBJDIR)
    ap.add_argument("--lib-objdir", default=LIB_OBJDIR)
    ap.add_argument("--json", action="store_true", help="print the derived rows as json")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    try:
        if args.rederive or not args.check:
            dumpbin = find_dumpbin()
            if not dumpbin:
                raise Refusal("no dumpbin.exe found (machine_config VS_INSTALL_ROOT)")
            undname = find_undname()
            if not undname:
                raise Refusal("no undname.exe found -- the return-type rule needs it")
            mh_objs, mh_stale = live_objs(
                args.mh_objdir, os.path.join(SRC, "mh", "mh.vcxproj"), "mh.dll"
            )
            lib_objs, lib_stale = live_objs(
                args.lib_objdir,
                os.path.join(SRC, "libmh_dll", "libmh_dll.vcxproj"),
                "libmh.dll",
            )
            for what, stale in (("mh", mh_stale), ("libmh", lib_stale)):
                if stale:
                    # Named but CAPPED. R5's eight F2-era objects were worth naming; the 635 the
                    # spine left behind in mh/Debug are a build-tree artefact, and printing them
                    # all buries the result this tool exists to report.
                    head = ", ".join(stale[:8]) + (", ..." if len(stale) > 8 else "")
                    print(
                        "[gen_libmh_contract] note: %d stale object(s) in the %s tree IGNORED "
                        "(not TUs of the project any more): %s" % (len(stale), what, head)
                    )
            rows = derive(mh_objs, lib_objs, dumpbin, undname)
            errs = validate(rows)
            if errs:
                for e in errs:
                    print("[gen_libmh_contract] FAIL: %s" % e)
                return 1
        else:
            rows = load_rows()
    except Refusal as e:
        print("[gen_libmh_contract] REFUSED: %s" % e)
        return 2

    if args.json:
        print(json.dumps(rows, indent=2))
        return 0

    payload = json.dumps(
        {
            "note": "GENERATED by tools/gen_libmh_contract.py -- the mh.dll <-> libmh.dll contract",
            "count": len(rows),
            "hand": sum(1 for r in rows if r["hand"]),
            "rows": rows,
        },
        indent=2,
    )

    if args.check:
        rc = 0
        if args.rederive:
            committed = load_rows()
            if [r["mangled"] for r in committed] != [r["mangled"] for r in rows]:
                only_new = sorted({r["mangled"] for r in rows} - {r["mangled"] for r in committed})
                only_old = sorted({r["mangled"] for r in committed} - {r["mangled"] for r in rows})
                print(
                    "[gen_libmh_contract] FAIL: the committed contract disagrees with the objects."
                )
                for n in only_new:
                    print("    + %s (the tree wants it; the committed list does not have it)" % n)
                for n in only_old:
                    print("    - %s (committed but nothing needs it any more)" % n)
                print("    rerun tools/gen_libmh_contract.py")
                rc = 1
        for path, render in EMITTED:
            text = render(rows)
            if not os.path.exists(path):
                print("[gen_libmh_contract] FAIL: %s does not exist" % path)
                rc = 1
                continue
            if io.open(path, encoding="utf-8").read() != text:
                print(
                    "[gen_libmh_contract] FAIL: %s is stale -- rerun tools/gen_libmh_contract.py"
                    % os.path.relpath(path, REPO).replace("\\", "/")
                )
                rc = 1
        if rc == 0:
            print(
                "[gen_libmh_contract] OK: %d rows (%d hand-defined), three emitted files current"
                % (len(rows), sum(1 for r in rows if r["hand"]))
            )
        return rc

    changed = []
    if write_if_changed(JSON_PATH, payload + "\n"):
        changed.append(JSON_PATH)
    for path, render in EMITTED:
        if write_if_changed(path, render(rows)):
            changed.append(path)
    print(
        "wrote %d row(s), %d hand-defined; %d file(s) changed"
        % (len(rows), sum(1 for r in rows if r["hand"]), len(changed))
    )
    for p in changed:
        print("    %s" % os.path.relpath(p, REPO).replace("\\", "/"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
