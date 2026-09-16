#!/usr/bin/env python3
# gen_libmh_std_contract.py -- fork F4G: the STANDALONE libmh.dll <-> host EXPORT CONTRACT.
#
# THE PROBLEM, and why it is a different one from gen_libmh_contract.py's.
#
# F4D took the spine out of mh.dll and into `libmh.dll`. That file is the HOSTED arm -- built
# WITHOUT MH_LIBMH_BUILD, so crt/crt_select.h's MH_CRT() resolves to `mh::call::*`, the ORIGINAL
# BINARY's own Watcom CRT reached at fixed VAs. It cannot run in a process that has no mh.exe
# mapped: the first allocation is a call into unmapped memory. So the fork plan's configuration
# (3) -- "libmh + a host, no game binary" -- needs its own DLL, built from the STANDALONE arm, and
# `libmh_std.vcxproj` is it (Release\standalone\libmh.dll; see that generated project's header for
# why it links the archive with /WHOLEARCHIVE rather than recompiling the roster).
#
# A DLL exports what its .def says and NOTHING else, so the standalone arm needs an export list for
# its host exactly as the hosted arm needed one for mh.dll. This tool derives it the same way, from
# the same kind of object-level fact, for the same reason (docs/dll-split.md: a hand-maintained
# list is the G106 shape -- it looks complete, is checked by nobody, and its first forgotten row is
# a link failure at best):
#
#     { external symbols UNDEFINED in libref_host's objects }
#   & { external symbols DEFINED   in libmh.lib's objects }
#
# Both object sets are read through live_objs(), which refuses an object no longer declared by its
# project (fork F5K) -- a directory listing is a record of every build that ever ran there, and a
# left-behind object from a deleted .cpp would otherwise contribute an EXPORT that no source line
# produces. Read that function for why the roster is the .vcxproj and not the archive.
#
# ---- WHAT IS DIFFERENT FROM THE HOSTED CONTRACT -------------------------------------------------
#
# There are no thunks, no slot table and no absent values, and the absence of all three is the
# point rather than a simplification:
#
#   * the host IMPORTS this DLL statically (the import library IS linked here -- the deliberate
#     inverse of libmh.def's "never linked, by anyone"), because a standalone host has no
#     configuration (1) to degrade into. libmh IS the program; a missing file killing process load
#     is the correct answer, not a degradation to be engineered;
#   * therefore there is no "the module is absent" path to answer, so no `xor eax,eax` stub and no
#     decision about what zero means for each row;
#   * therefore a missing row is a LINK ERROR NAMING THE SYMBOL, not a silent runtime fallback.
#     That is why this tool's --rederive --check runs in run_gate beside the other two: a stale list
#     is caught by the build itself, and this check is what tells you the committed list drifted
#     rather than leaving you to read a linker diagnostic.
#
# ---- OUTPUTS ------------------------------------------------------------------------------------
#
#   tools/data/libmh_std_contract.json     the committed row list
#   src/mh_dll/libmh_std/libmh_std.def     the export list, rendered from it
#
# ---- MODES --------------------------------------------------------------------------------------
#
#   python tools/gen_libmh_std_contract.py               re-derive from objects and write both
#   python tools/gen_libmh_std_contract.py --check       SOURCE-ONLY: re-render the .def from the
#                                                        committed json and fail on a diff. Needs no
#                                                        build -- this is the lint_repo row.
#   python tools/gen_libmh_std_contract.py --rederive --check
#                                                        re-measure from the objects and fail if the
#                                                        committed json disagrees. Needs the Release
#                                                        build; run_gate runs it after the build.
#   python tools/gen_libmh_std_contract.py --selftest    planted inputs; every negative goes red.
#
# Exit 0 = current. 1 = stale. 2 = the tool cannot be trusted (objects missing, no dumpbin, a
# positive control gone). NEVER read a 2 as "the contract is empty".

from __future__ import annotations

import argparse
import io
import json
import os
import re
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src", "mh_dll")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Release rather than Debug, and unlike gen_libmh_contract.py that is not a free choice: neither of
# these projects has a Debug configuration at all (both map Debug|x86 -> Release|Win32 in mh.sln),
# and neither is built with /GL, so the objects are readable ones.
HOST_OBJDIR = os.path.join(SRC, "libref_host", "Release")
LIB_OBJDIR = os.path.join(SRC, "libmh", "Release")

# THE ROSTER EACH OBJECT SET IS CHECKED AGAINST (fork F5K). See live_objs for why these two files
# are the honest subject and the ARCHIVE is not, even though the module under derivation is
# libmh.lib.
HOST_VCXPROJ = os.path.join(SRC, "libref_host", "libref_host.vcxproj")
LIB_VCXPROJ = os.path.join(SRC, "libmh", "libmh.vcxproj")

JSON_PATH = os.path.join(REPO, "tools", "data", "libmh_std_contract.json")
DEF_PATH = os.path.join(SRC, "libmh_std", "libmh_std.def")

# A PROJECT symbol, derived from the decorated name rather than listed -- the same test
# gen_libmh_contract.py and check_libmh_outbound.py use, and for the same reason: a new CRT helper
# must not read as a new contract row.
PROJECT_RE = re.compile(r"@mh@@|^_?MH_[A-Za-z]|^_?libmh_[a-z]")

# Ruling Q5's per-image marshalling thunks (addr/mh_calls.gen.cpp). BOTH images compile that TU --
# the host has always carried it, and libmh_std.vcxproj carries it because /WHOLEARCHIVE would
# otherwise leave the archive's few references to those shapes unresolved. So they are defined on
# each side and are never a contract row. Excluded EXPLICITLY, so a reader does not have to work out
# why a symbol eligible by every other test is absent from the list.
SHIM_RE = re.compile(r"@detail@call@mh@@")

# POSITIVE CONTROLS. A derivation that reports a plausible list has to prove it can still see the
# shapes it is supposed to see. RE-PICK RULE (F3C/F3D, restated in check_libmh_outbound.py): if an
# item genuinely removes one of these rows, DELETE the anchor in the same commit -- do not widen the
# tool to tolerate its absence.
ANCHORS = [
    ("_libmh_set_host_api", "the C facade's host-callback bind: the first call the host makes"),
    ("_libmh_import_world", "the LIB-WORLD blob import -- config (3) has no other way to a world"),
    ("?sim_step@sim@mh@@YAXXZ", "the sim step itself; without it the host has nothing to replay"),
]

DEF_BANNER = """; libmh_std.def -- the STANDALONE libmh.dll's EXPORT LIST (fork F4G).
;
; GENERATED by tools/gen_libmh_std_contract.py from two object sets; do not hand-edit.
; Every name is the DECORATED symbol as the host's import library must match it: a C++ symbol
; verbatim, an extern "C" __cdecl one without its leading underscore (the linker does that
; matching). Sorted by decorated name, so a row added in the middle is one diff.
;
; THE IMPORT LIBRARY BESIDE THIS ONE *IS* LINKED, and that is the deliberate inverse of
; libmh_dll/libmh.def's rule. There, a static import would put libmh.dll in mh.dll's import
; table and a missing file would kill mh.exe at load -- and absence is a SHIPPED configuration
; (config (1)). Here there is no configuration to degrade into: libmh IS the program, the host
; is nothing without it, and a missing file failing process load is the loudest correct answer.
;
; THE SUBJECT IS THE STANDALONE ARM. This module is libmh.lib -- the roster compiled WITH
; MH_LIBMH_BUILD, so MH_CRT() picks the vendored CRT and no original-binary VA is on any live
; path -- linked whole. Configuration (2)'s libmh.dll is a different binary with the same file
; name and a different export list; a swapped pair is refused by name at mh.dll's bind.
LIBRARY libmh
EXPORTS
"""


class Refusal(Exception):
    """Exit 2: this run cannot be trusted."""


def find_dumpbin():
    from check_module_bind import find_dumpbin as f

    return f()


def dump_symbols(dumpbin, objs):
    from check_libmh_outbound import dump_symbols as d

    return d(dumpbin, objs)


def objs_in(objdir, what):
    if not os.path.isdir(objdir):
        raise Refusal(
            "%s does not exist. Build Release|Win32 first -- a derivation over a missing %s object "
            "set measures an empty contract, which reads exactly like a closed one."
            % (objdir, what)
        )
    out = sorted(os.path.join(objdir, f) for f in os.listdir(objdir) if f.lower().endswith(".obj"))
    if not out:
        raise Refusal("%s holds no .obj files (%s)" % (objdir, what))
    return out


def roster(vcxproj):
    """The TU basenames a generated .vcxproj still compiles. Basenames because that is what the
    compiler names the object after, and it is the only thing an object on disk can be matched by."""
    text = io.open(vcxproj, encoding="utf-8-sig").read()
    return {
        os.path.splitext(os.path.basename(x))[0]
        for x in re.findall(r'<ClCompile Include="([^"]+)"', text)
    }


def live_objs(objdir, vcxproj, what):
    """objs_in, CROSS-CHECKED AGAINST THE PROJECT'S DECLARED TU ROSTER (fork F5K, from F4H's
    residue).

    THE HOLE THIS CLOSES. objs_in globs a build directory, and a build directory is a record of
    every build that ever ran there -- not of the tree that exists now. Delete a .cpp and its object
    stays, keeps defining its symbols, and walks straight into this derivation: a host reference it
    satisfies becomes a row of libmh_std.def, i.e. an EXPORT of the shipped standalone spine that no
    source line in the repo produces. It was latent rather than theoretical -- the sibling generator
    measured the same shape biting for real (F4D R5: eight F2-era objects in mh/Debug put two phantom
    rows in F4D-PRE's count) -- and there are zero stale objects in either of these two trees today,
    which is exactly when a check is cheap to add and impossible to argue about.

    THE SUBJECT, AND WHY IT IS NOT THE ARCHIVE. This tool's own header calls the right-hand set
    "libmh.lib's objects", and run_gate says it "intersects libref_host's objects with the libmh
    ARCHIVE's" -- so the obvious roster for the libmh side is the archive's member list
    (`dumpbin /ARCHIVEMEMBERS`), and it is the wrong one. The archive is a BUILD OUTPUT of the same
    project, produced by handing the librarian exactly the roster below; it is downstream of the
    thing we want to be authoritative. Delete a .cpp, regenerate the project, and until someone
    rebuilds, the archive still carries the dead member -- an archive-checked run would pass the one
    case this function exists to catch, and would pass it by agreeing with the stale object. The
    .vcxproj's ClCompile list is the DECLARATION (generated by tools/gen_libmh_vcxproj.py from the
    source tree, so it tracks a deleted file at the moment the project is regenerated, not at the
    next build). libref_host has no archive at all, so its project was always the only candidate;
    using the projects for both sides also means the two halves are checked by one rule.

    WHY IT REFUSES WHERE gen_libmh_contract.py ONLY NOTES. That tool ignores stale objects with a
    capped note because mh/Debug legitimately holds hundreds of pre-spine leftovers that no clean
    build will ever remove. These two trees are single-configuration, freshly introduced at F4G, and
    currently exact (628/628 and 3/3), so the strict answer is available -- and the artifact here is
    an EXPORT LIST, where a phantom row is shipped surface rather than a stale measurement. A stale
    object is named, not counted away."""
    want = roster(vcxproj)
    if not want:
        raise Refusal(
            "%s declares no <ClCompile> TUs -- an empty roster would silently accept every object "
            "as stale-or-live and is a broken read of the project, not an empty project"
            % os.path.basename(vcxproj)
        )
    keep, stale = [], []
    for p in objs_in(objdir, what):
        (keep if os.path.splitext(os.path.basename(p))[0] in want else stale).append(p)
    if stale:
        named = ", ".join(os.path.basename(p) for p in stale[:12])
        raise Refusal(
            "%d STALE object(s) in %s: %s%s. No TU of %s compiles them any more, so every symbol "
            "they carry would enter this export contract from a source file that no longer exists. "
            "Delete them, or rebuild into a clean directory, and rerun."
            % (
                len(stale),
                objdir,
                named,
                ", ..." if len(stale) > 12 else "",
                os.path.basename(vcxproj),
            )
        )
    have = {os.path.splitext(os.path.basename(p))[0] for p in keep}
    missing = sorted(want - have)
    if missing:
        raise Refusal(
            "%d of %s's %d TUs have no object in %s: %s%s -- build Release|Win32 first. A short "
            "object set measures a short contract, which reads exactly like a closed one."
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


def export_name(mangled):
    """The name the .def must carry -- see gen_libmh_contract.export_name for the measured lesson
    (an export list that used the decorated name for the C rows too made the first real bind report
    `resolved 87 of 101`). A C++ symbol is exported decorated; an extern "C" __cdecl one is
    decorated with a leading underscore in the object and exported without it."""
    return mangled if mangled.startswith("?") else mangled.lstrip("_")


def derive(host_objs, lib_objs, dumpbin):
    _hdef, hundef = dump_symbols(dumpbin, host_objs)
    ldef, _lundef = dump_symbols(dumpbin, lib_objs)
    names = set()
    for name in hundef:
        if name in ldef and PROJECT_RE.search(name) and not SHIM_RE.search(name):
            names.add(name)
    rows = [{"mangled": n, "export": export_name(n)} for n in sorted(names)]

    seen = {r["export"] for r in rows}
    if len(seen) != len(rows):
        raise Refusal("two rows render to the same export name -- the .def would be ambiguous")
    for mangled, why in ANCHORS:
        if mangled not in names:
            raise Refusal(
                "POSITIVE CONTROL MISSING: %s (%s) is not a contract row. Either the derivation is "
                "broken or the tree genuinely changed -- if the latter, delete the anchor in the "
                "same commit rather than widening this tool." % (mangled, why)
            )
    return rows


def render_def(rows):
    return DEF_BANNER + "".join("    %s\n" % r["export"] for r in rows)


def load_committed():
    if not os.path.isfile(JSON_PATH):
        return None
    return json.load(io.open(JSON_PATH, encoding="utf-8"))


def write_outputs(rows):
    io.open(JSON_PATH, "w", encoding="utf-8", newline="\n").write(
        json.dumps({"rows": rows}, indent=1) + "\n"
    )
    os.makedirs(os.path.dirname(DEF_PATH), exist_ok=True)
    io.open(DEF_PATH, "w", encoding="utf-8", newline="\r\n").write(render_def(rows))


def roster_selftest():
    """THE PLANTED STALE OBJECT (fork F5K). A real build tree cannot be asked to hold one, so the
    arm builds a fake project + object directory and plants it there. The assertion is not merely
    that the run goes red -- a refusal that says "something is wrong" would leave the reader to
    diff two directory listings by hand -- but that the offending object is NAMED in the message."""
    bad = 0
    with tempfile.TemporaryDirectory() as td:
        proj = os.path.join(td, "fake.vcxproj")
        io.open(proj, "w", encoding="utf-8", newline="\n").write(
            "<Project><ItemGroup>\n"
            '  <ClCompile Include="..\\mh\\a.cpp" />\n'
            '  <ClCompile Include="b.cpp" />\n'
            '  <ClCompile Include="mh_calls.gen.cpp" />\n'
            "</ItemGroup></Project>\n"
        )
        objdir = os.path.join(td, "Release")
        os.makedirs(objdir)

        def plant(*names):
            for f in os.listdir(objdir):
                os.remove(os.path.join(objdir, f))
            for n in names:
                io.open(os.path.join(objdir, n), "w").write("")

        def run():
            try:
                return live_objs(objdir, proj, "the fake"), None
            except Refusal as e:
                return None, str(e)

        # POSITIVE CONTROL FIRST: the exact roster passes, and passes whole. A refusal arm proves
        # nothing if the function refuses everything. Note mh_calls.gen -- the double extension is
        # the shape splitext() gets wrong if anyone "simplifies" the basename match.
        plant("a.obj", "b.obj", "mh_calls.gen.obj")
        objs, err = run()
        ok = err is None and len(objs or []) == 3
        cases = [("  %-4s the exact roster passes, all 3 objects kept", ok, err or len(objs or []))]

        # THE ITEM'S NEGATIVE: an object from a .cpp the project no longer compiles.
        plant("a.obj", "b.obj", "mh_calls.gen.obj", "zz_deleted_tu.obj")
        objs, err = run()
        ok = err is not None and "zz_deleted_tu.obj" in err
        cases.append(("  %-4s a planted STALE object is refused BY NAME", ok, err))

        # THE OTHER DIRECTION: a declared TU whose object is absent (a half-built tree) must not be
        # measured as a smaller contract.
        plant("a.obj", "mh_calls.gen.obj")
        objs, err = run()
        ok = err is not None and "b" in err and "build Release" in err
        cases.append(("  %-4s a declared TU with no object is refused by name", ok, err))

        # A ROSTER THAT READ AS EMPTY would classify every object as stale; refuse instead.
        empty = os.path.join(td, "empty.vcxproj")
        io.open(empty, "w", encoding="utf-8", newline="\n").write("<Project/>\n")
        plant("a.obj")
        try:
            live_objs(objdir, empty, "the fake")
            ok, detail = False, "accepted"
        except Refusal as e:
            ok, detail = "declares no <ClCompile>" in str(e), str(e)
        cases.append(
            ("  %-4s an empty/unreadable roster refuses rather than flagging all", ok, detail)
        )

    for fmt, ok, detail in cases:
        print((fmt % ("ok" if ok else "FAIL")) + ("" if ok else "  (got %r)" % (detail,)))
        bad += not ok
    return bad


def selftest():
    """Planted inputs: every negative must go red. Deliberately small -- this tool has one
    derivation and three rules, and each case is one of them."""
    bad = 0

    class FakeDump:
        def __init__(self, hundef, ldef):
            self.h, self.l = hundef, ldef

        def __call__(self, _db, objs):
            return (set(), set(self.h)) if objs == ["host"] else (set(self.l), set())

    real = globals()["dump_symbols"]
    cases = [
        (
            "a symbol the host needs and libmh defines is a ROW",
            ["?sim_step@sim@mh@@YAXXZ", "_libmh_set_host_api", "_libmh_import_world"],
            ["?sim_step@sim@mh@@YAXXZ", "_libmh_set_host_api", "_libmh_import_world"],
            None,
            3,
        ),
        (
            "a CRT symbol is NOT a row even when both sides have it",
            ["?sim_step@sim@mh@@YAXXZ", "_libmh_set_host_api", "_libmh_import_world", "_malloc"],
            ["?sim_step@sim@mh@@YAXXZ", "_libmh_set_host_api", "_libmh_import_world", "_malloc"],
            None,
            3,
        ),
        (
            "a per-image marshalling shim is NOT a row",
            [
                "?sim_step@sim@mh@@YAXXZ",
                "_libmh_set_host_api",
                "_libmh_import_world",
                "?s_u32_B68@detail@call@mh@@YAIIPBX@Z",
            ],
            [
                "?sim_step@sim@mh@@YAXXZ",
                "_libmh_set_host_api",
                "_libmh_import_world",
                "?s_u32_B68@detail@call@mh@@YAIIPBX@Z",
            ],
            None,
            3,
        ),
        (
            "a symbol the host wants but libmh does NOT define is not a row (it is a link error)",
            [
                "?sim_step@sim@mh@@YAXXZ",
                "_libmh_set_host_api",
                "_libmh_import_world",
                "?x@mh@@YAXXZ",
            ],
            ["?sim_step@sim@mh@@YAXXZ", "_libmh_set_host_api", "_libmh_import_world"],
            None,
            3,
        ),
        (
            "a MISSING POSITIVE CONTROL refuses rather than reporting a short list",
            ["?sim_step@sim@mh@@YAXXZ", "_libmh_set_host_api"],
            ["?sim_step@sim@mh@@YAXXZ", "_libmh_set_host_api"],
            "refuse",
            0,
        ),
    ]
    for label, hundef, ldef, expect, want in cases:
        globals()["dump_symbols"] = FakeDump(hundef, ldef)
        try:
            rows = derive(["host"], ["lib"], None)
            got, err = len(rows), None
        except Refusal as e:
            got, err = 0, str(e)
        globals()["dump_symbols"] = real
        if expect == "refuse":
            ok = err is not None
        else:
            ok = err is None and got == want
        print(
            "  %-4s %s%s"
            % ("ok" if ok else "FAIL", label, "" if ok else "  (got %r)" % (err or got))
        )
        bad += not ok

    # The export-name rule, which is the one that cost a real boot one image over.
    for mangled, want in (
        ("?sim_step@sim@mh@@YAXXZ", "?sim_step@sim@mh@@YAXXZ"),
        ("_libmh_set_host_api", "libmh_set_host_api"),
    ):
        ok = export_name(mangled) == want
        print(
            "  %-4s export_name(%s) -> %s" % ("ok" if ok else "FAIL", mangled, export_name(mangled))
        )
        bad += not ok

    bad += roster_selftest()
    print("[gen_libmh_std_contract] selftest: %s" % ("PASS" if not bad else "%d FAILED" % bad))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(
        description="fork F4G: the standalone libmh.dll <-> host export contract, derived and gated"
    )
    ap.add_argument("--check", action="store_true", help="fail on a stale committed artifact")
    ap.add_argument("--rederive", action="store_true", help="re-measure from the objects")
    ap.add_argument("--selftest", action="store_true", help="planted inputs")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    try:
        if args.check and not args.rederive:
            committed = load_committed()
            if committed is None:
                print(
                    "[gen_libmh_std_contract] FAIL: %s is missing -- run the generator" % JSON_PATH
                )
                return 1
            want = render_def(committed["rows"])
            got = io.open(DEF_PATH, encoding="utf-8").read() if os.path.isfile(DEF_PATH) else ""
            if got != want:
                print(
                    "[gen_libmh_std_contract] FAIL: libmh_std.def does not re-render from the "
                    "committed list -- rerun tools/gen_libmh_std_contract.py"
                )
                return 1
            print(
                "[gen_libmh_std_contract] OK: %d row(s), libmh_std.def re-renders"
                % len(committed["rows"])
            )
            return 0

        dumpbin = find_dumpbin()
        if not dumpbin:
            raise Refusal("no dumpbin.exe under machine_config.VS_INSTALL_ROOT")
        rows = derive(
            live_objs(HOST_OBJDIR, HOST_VCXPROJ, "libref_host"),
            live_objs(LIB_OBJDIR, LIB_VCXPROJ, "the libmh archive"),
            dumpbin,
        )
        if args.check:
            committed = load_committed()
            if committed is None or committed["rows"] != rows:
                old = {r["mangled"] for r in (committed or {"rows": []})["rows"]}
                new = {r["mangled"] for r in rows}
                print(
                    "[gen_libmh_std_contract] FAIL: the committed list disagrees with the objects "
                    "(%d row(s) measured, %d committed)" % (len(rows), len(old))
                )
                for n in sorted(new - old):
                    print("    + %s" % n)
                for n in sorted(old - new):
                    print("    - %s" % n)
                print("    rerun tools/gen_libmh_std_contract.py")
                return 1
            print(
                "[gen_libmh_std_contract] OK: %d row(s), committed list matches the objects"
                % len(rows)
            )
            return 0
        write_outputs(rows)
        print(
            "wrote %s and %s (%d row(s))"
            % (os.path.relpath(JSON_PATH, REPO), os.path.relpath(DEF_PATH, REPO), len(rows))
        )
        return 0
    except Refusal as e:
        print("[gen_libmh_std_contract] REFUSED (exit 2): %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())
