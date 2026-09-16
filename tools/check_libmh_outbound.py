#!/usr/bin/env python3
# check_libmh_outbound.py -- fork item F4D-PRE: libmh's OUTBOUND edge, measured and ruled.
#
# THE CLAIM UNDER TEST. `src/mh_dll/libmh` is the 627-TU spine mh.dll is about to stop containing
# (F4D, Q10 ratified). A spine that calls back OUT into its host is a spine that cannot be shipped
# without it, so F4D-PRE's done_when demands that edge be "0 or a named, gated registration
# surface". It is now the second of those: five hook services (entry ownership, the two entry
# installers, the two determinism-harness questions) reach libmh through ONE table it is handed at
# bind time (libmh/include/libmh_hook.h, libmh/state/hook_api.h, mh/seams/libmh_hook_host.cpp).
#
# This tool is what stops the edge regrowing. It measures the same thing TWICE, because neither
# mechanism can see what the other can:
#
#   SRC  -- an INCLUDE-CLOSURE walk from the 627 roster TUs. Resolves every `#include "..."` against
#           the real include path and fails if the closure reaches a harness header (mh/hook,
#           mh/seams, mh/effects, mh/shadow, or an mh/include/mh_*.h export/bind header), or if any
#           reachable file names `mh::hook::` / `mh::seams::` / `MH_Harness_*` outside a comment.
#           NEVER STALE, needs no build -- so this is the lint row.
#           DELIBERATELY IGNORES `#ifdef MH_LIBMH_BUILD`. Until F4D-PRE the rule was the weaker
#           "guarded is fine" (tools/lint_libmh_layering.py) and eight roster TUs each carried their
#           own guarded harness include plus a hand-written `#else` stub. A guard makes the edge
#           invisible to a link-level reader while leaving it in the hosted build, which is the
#           configuration F4D actually ships. So a guarded harness include is a FAILURE here, and
#           lint_libmh_layering stays as the weaker-but-finer-grained sibling it always was.
#           Cannot see: a reference introduced by a MACRO. That is not hypothetical -- see below.
#
#   OBJ  -- dumpbin -symbols over the HOSTED build of the same 627 TUs (src/mh_dll/libmh_dll/Debug
#           since fork F4D, i.e. libmh.dll's own compilation of them WITHOUT MH_LIBMH_BUILD; before
#           the split it was mh.vcxproj's, which is where mh.dll compiled the roster). The
#           configuration is what matters, not the project: this is the build that ships, and it is
#           the one whose edges decide the split. Unresolved externals = defined
#           by no roster object = what the host must supply. This is the LOAD-BEARING mechanism and
#           the reason the tool exists at all: `mh::hook::install_export_ok` was libmh's largest
#           outbound row, reached from 22 of the 627 TUs, and NOT ONE OF THEM NAMES IT -- it arrives
#           through MH_EXPORT_REPLACE (addr/mh_export.gen.h). A source scan measured zero there and
#           would have called the edge closed while it was open.
#           Cannot see: header-inline/constexpr coupling (never undefined), anything the optimiser
#           folded, and anything a STALE object predates -- so mtimes are compared and reported.
#           Release objects are /GL (ANONYMOUS OBJECT) and unreadable by dumpbin; Debug is the set.
#
# WHY THE LIBMH.LIB ITSELF IS THE WRONG SUBJECT, stated because reading it is the obvious first
# instinct and it answers a different question. That archive is built WITH MH_LIBMH_BUILD -- the
# LIB-REF standalone arm -- in which every one of these five rows is compiled out by construction.
# Measured 2026-09-13: five unresolved project externals in the hosted objects, ZERO in the archive.
# The archive is the artifact for a host that has no game image; F4D's libmh.dll is a HOSTED build
# of the same roster, and that is the build whose edge decides whether the split is possible.
#
# BUCKETS. Every unresolved external is classified, and an unclassified PROJECT symbol fails:
#   toolchain -- not a project symbol at all (CRT, compiler runtime, a __imp_ system import). Derived
#                from the decorated name, not from a list: a project symbol is one in namespace `mh`
#                or carrying an MH_/libmh_ C prefix. Counted, never ruled, never a failure.
#   shim      -- `mh::call::detail::s_*`, the generated naked marshalling thunks
#                (addr/mh_calls.gen.cpp). RULED BY Q5: that TU left libmh's roster at F4D-PRE and is
#                compiled PER-IMAGE instead (mh.vcxproj, mh_nettest.vcxproj, libref_host.vcxproj --
#                the last one's /WHOLEARCHIVE is what makes a forgotten row a link error rather than
#                a silent gap). It is generated source in this tree, so any image that compiles the
#                roster can compile it too; it is not something a host must SUPPLY.
#   common    -- mh_common's buffer codecs (mh::lzw, mh::lzss). A static library both images link,
#                the same per-image arrangement F4B set for mh_net_proto's session_info.cpp.
#   host      -- anything else in namespace mh / MH_ / libmh_. THE RESIDUE. Non-empty = the edge has
#                regrown, and the run fails naming the symbol and the objects that want it.
#
# Usage:
#   python tools/check_libmh_outbound.py                  # both mechanisms, full report
#   python tools/check_libmh_outbound.py --src-only       # the lint row (no build needed)
#   python tools/check_libmh_outbound.py --objdir DIR     # read hosted objects from DIR
#   python tools/check_libmh_outbound.py --json
#   python tools/check_libmh_outbound.py --selftest       # planted inputs: every negative goes RED
#
# Exit 0 = the edge is closed (host bucket empty) for the mechanisms that ran.
# Exit 1 = residue: a harness reach in source, or an unruled project symbol in the objects.
# Exit 2 = the TOOL is not to be trusted: the roster is not where it looks, objects are missing or
#          stale, dumpbin is absent, or a positive control went missing. NEVER read a 2 as "no refs".

from __future__ import annotations

import argparse
import collections
import glob
import io
import json
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src", "mh_dll")
MH = os.path.join(SRC, "mh")
# F5O: the roster left mh/ for libmh/. gen_libmh_vcxproj.sources() is relative to THAT tree.
LIBMH = os.path.join(SRC, "libmh")

# WHERE THE HOSTED ROSTER'S OBJECTS LIVE, and this MOVED at fork F4D. The OBJ mechanism's whole
# argument is that it reads the build WITHOUT MH_LIBMH_BUILD -- the configuration that actually
# ships -- rather than the archive, whose standalone arm compiles every one of these rows out. Until
# F4D that build was mh.vcxproj's (mh.dll contained the spine); now it is libmh_dll.vcxproj's, and
# mh/Debug holds no roster object at all. Pointing this at the old directory would not have failed
# loudly: scan_obj() refuses on a MISSING roster object, so the tool would have exited 2 rather than
# reporting a false zero -- but a permanent exit 2 is a gate that has stopped running, which is the
# same outcome by a slower route.
HOSTED_OBJDIR = os.path.join(SRC, "libmh_dll", "Debug")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _dllsrc  # noqa: E402

# The include path the libmh roster is compiled with (libmh.vcxproj's
# AdditionalIncludeDirectories), in order. The closure walk resolves against exactly this plus the
# includer's own directory, so a header it cannot resolve is REPORTED rather than skipped.
INCLUDE_DIRS = [
    LIBMH,
    MH,
    os.path.join(MH, "include"),
    os.path.join(SRC, "mh_common"),
    os.path.join(SRC, "mh_common", "include"),
    os.path.join(REPO, "src", "mh_net_proto", "include"),
]

# Harness DIRECTORIES: the injection layer, which libmh does not compile. A closure that reaches one
# is a module TU reaching the harness, guarded or not.
HARNESS_DIRS = ("hook", "seams", "effects", "shadow")

# Harness HEADERS inside mh/include, which is a mixed directory (it also holds shared module headers
# -- ctrl_emit.h, packet_buffer.h, projectile.h, lobby_session.h). DERIVED from the naming
# convention every one of them already follows rather than hand-listed: an `mh_*.h` in mh/include
# declares an mh.dll export or binder surface.
HARNESS_INCLUDE_RE = re.compile(r"^mh_.*\.h$")

# Tokens that name the harness from module code. `mh::hosthook::` is deliberately absent -- that IS
# the registration surface, defined inside the roster.
TOKEN_RE = re.compile(r"\bmh::(?:hook|seams)::\w*|\bMH_Harness_\w+")

INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*"([^"]+)"', re.M)

# ---------------------------------------------------------------------------------------------
# OBJ classification
# ---------------------------------------------------------------------------------------------
# A PROJECT symbol, derived from the decorated name: MSVC mangles a C++ name in namespace `mh` with
# an `@mh@@` segment, and the project's C surface is MH_* / libmh_*. Everything else is the
# toolchain's. Deriving this rather than listing CRT names is what keeps a new CRT helper from
# reading as a new outbound edge.
PROJECT_RE = re.compile(r"@mh@@|^_?MH_[A-Za-z]|^_?libmh_[a-z]")
SHIM_RE = re.compile(r"@detail@call@mh@@")
COMMON_RE = re.compile(r"@(?:lzw|lzss)@mh@@")

# POSITIVE CONTROLS for the OBJ pass. A scan that reports an empty edge has to prove it can still
# see one, and these are facts of the tree rather than of this item: every roster object references
# the CRT (a 627-TU C++ build cannot not), and the roster still calls the marshalling shims and
# mh_common's codecs. If one goes missing the tool exits 2 NAMING IT, because the alternative is a
# silent zero -- the exact shape a negative-claim gate exists to refuse.
#
# RE-PICK RULE, learned from F3C/F3D: an anchor that dies with the change it watches turns a correct
# result into a misleading exit 2. If a future item genuinely removes the last `mh::call::` site in
# the roster (LIB-ABI's direction of travel) or moves the save codecs into libmh, DELETE that anchor
# in the same commit -- do not widen the tool to tolerate its absence.
OBJ_ANCHORS = [
    ("__chkstk", "toolchain", "the CRT stack probe -- present in any 627-TU x86 build"),
    ("?s_void@detail@call@mh@@YAXI@Z", "shim", "the zero-argument marshalling shape, Q5's bucket"),
    ("?decompress_block@lzss@mh@@YAXPBXPAX@Z", "common", "mh_common's save codec"),
]


class Refusal(Exception):
    """Exit 2: the tool cannot be trusted for this run."""


def strip_comments_and_strings(text):
    """Blank out //, /* */, "..." and '...' while preserving line structure (same helper shape as
    check_net_lockstep_refs -- kept local so neither tool depends on the other)."""
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
                if i + 1 < n:
                    out[i + 1] = " "
                i += 2
        elif c in '"\'':
            quote = c
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


# ---------------------------------------------------------------------------------------------
# SRC mechanism -- the include closure
# ---------------------------------------------------------------------------------------------
def roster_sources():
    """The 627 TUs, from the SAME generator that writes libmh.vcxproj -- never a hand list."""
    import gen_libmh_vcxproj

    return [
        os.path.join(LIBMH, s.replace("\\", os.sep)) for s in gen_libmh_vcxproj.sources()
    ], gen_libmh_vcxproj.MODULES


def classify_path(path):
    """-> 'harness' / 'module' / 'other', for a file inside the tree."""
    rel = _dllsrc.module_rel(path)
    if rel is None:
        return "other"
    head = rel.split("/")[0]
    if head in HARNESS_DIRS:
        return "harness"
    if head == "include" and HARNESS_INCLUDE_RE.match(os.path.basename(rel)):
        return "harness"
    return "module"


def resolve_include(spec, includer):
    for base in [os.path.dirname(includer)] + INCLUDE_DIRS:
        cand = os.path.normpath(os.path.join(base, spec.replace("/", os.sep)))
        if os.path.isfile(cand):
            return cand
    return None


def scan_src():
    """-> (violations, stats). A violation is a dict naming file:line and what it reached."""
    tus, _modules = roster_sources()
    missing = [p for p in tus if not os.path.isfile(p)]
    if not tus:
        raise Refusal("the libmh roster is EMPTY -- gen_libmh_vcxproj.sources() returned nothing")
    if missing:
        raise Refusal(
            "%d roster TU(s) do not exist on disk (first: %s). The roster and the tree disagree; "
            "this run cannot say anything about the edge." % (len(missing), missing[0])
        )

    violations = []
    unresolved = []
    seen = set()
    queue = list(tus)
    while queue:
        path = queue.pop()
        key = os.path.normcase(os.path.abspath(path))
        if key in seen:
            continue
        seen.add(key)
        raw = io.open(path, encoding="utf-8", errors="replace").read()
        code = strip_comments_and_strings(raw)
        rel_self = os.path.relpath(path, REPO).replace("\\", "/")
        # INCLUDES ARE READ OFF THE RAW TEXT. strip_comments_and_strings blanks the quoted spec, so
        # a stripped scan sees `#include` with nothing after it -- which is why the first draft
        # resolved 0 headers and reported the whole tree as "unresolved". Same reason
        # check_net_lockstep_refs scans its own include edges raw. INCLUDE_RE anchors at line start
        # with only whitespace before the `#`, so `// #include "hook/x.h"` still does not match.
        for m in INCLUDE_RE.finditer(raw):
            spec = m.group(1)
            target = resolve_include(spec, path)
            line = raw[: m.start()].count("\n") + 1
            if target is None:
                unresolved.append((rel_self, line, spec))
                continue
            kind = classify_path(target)
            if kind == "harness":
                violations.append(
                    dict(
                        kind="include",
                        file=rel_self,
                        line=line,
                        detail=spec,
                        reached=os.path.relpath(target, REPO).replace("\\", "/"),
                    )
                )
                continue  # do NOT walk into the harness; one report per edge, not per subtree
            if kind == "module":
                queue.append(target)
        for m in TOKEN_RE.finditer(code):
            violations.append(
                dict(
                    kind="token",
                    file=rel_self,
                    line=code[: m.start()].count("\n") + 1,
                    detail=m.group(0),
                    reached="",
                )
            )
    return violations, dict(tus=len(tus), files=len(seen), unresolved=unresolved)


# ---------------------------------------------------------------------------------------------
# OBJ mechanism
# ---------------------------------------------------------------------------------------------
SYM_RE = re.compile(r"^[0-9A-F]{3,} ([0-9A-F]{8}) (\S+)\s+\S+\s+(?:\(\)\s+)?(\S+)\s+\|\s+(\S+)")


def find_dumpbin():
    from check_module_bind import find_dumpbin as f

    return f()


def dump_symbols(dumpbin, objs):
    """-> (defined{name: [obj]}, undef{name: [obj]}) over a list of .obj paths."""
    defined = collections.defaultdict(set)
    undef = collections.defaultdict(set)
    for i in range(0, len(objs), 50):
        out = subprocess.run(
            [dumpbin, "-symbols"] + objs[i : i + 50], capture_output=True, text=True
        )
        if out.returncode != 0:
            raise Refusal("dumpbin failed: %s" % (out.stdout + out.stderr)[-400:])
        cur = None
        for line in out.stdout.splitlines():
            m = re.match(r"^Dump of file (.*)$", line)
            if m:
                cur = os.path.basename(m.group(1))
                continue
            m = SYM_RE.match(line)
            if not m:
                continue
            sect, cls, name = m.group(2), m.group(3), m.group(4)
            if cls != "External":
                continue
            (undef if sect == "UNDEF" else defined)[name].add(cur)
    return defined, undef


def bucket_of(name):
    if not PROJECT_RE.search(name):
        return "toolchain"
    if SHIM_RE.search(name):
        return "shim"
    if COMMON_RE.search(name):
        return "common"
    return "host"


def classify(defined, undef):
    """The pure half, so the selftest can drive it with synthetic tables.
    -> {bucket: {symbol: [objs]}}"""
    out = {b: {} for b in ("toolchain", "shim", "common", "host")}
    for name, objs in undef.items():
        if name in defined:
            continue
        out[bucket_of(name)][name] = sorted(objs)
    return out


def scan_obj(objdir):
    tus, _ = roster_sources()
    bases = [os.path.splitext(os.path.basename(p))[0] for p in tus]
    objs, absent = [], []
    for b in sorted(set(bases)):
        p = os.path.join(objdir, b + ".obj")
        (objs if os.path.isfile(p) else absent).append(p if os.path.isfile(p) else b)
    if absent:
        raise Refusal(
            "%d of %d roster objects are not in %s (first: %s.obj). Build Debug|Win32 first -- an "
            "OBJ pass over a partial roster measures a smaller edge than the tree has."
            % (len(absent), len(bases), objdir, absent[0])
        )
    dumpbin = find_dumpbin()
    if not dumpbin:
        raise Refusal("no dumpbin.exe found (machine_config VS_INSTALL_ROOT)")

    # Staleness is REPORTED, not guessed at: an object older than its source predates the edit the
    # run is being asked about.
    stale = []
    for p in tus:
        o = os.path.join(objdir, os.path.splitext(os.path.basename(p))[0] + ".obj")
        if os.path.getmtime(o) < os.path.getmtime(p):
            stale.append(os.path.relpath(p, REPO).replace("\\", "/"))

    defined, undef = dump_symbols(dumpbin, objs)
    buckets = classify(defined, undef)

    # The positive controls.
    seen = {}
    for b, rows in buckets.items():
        for name in rows:
            seen[name] = b
    for name, want, why in OBJ_ANCHORS:
        if name not in seen:
            raise Refusal(
                "positive control MISSING: %s (%s) was expected in bucket '%s' and the scan did not "
                "see it. Either the scan is broken or the anchor died with a change -- re-pick it in "
                "the SAME commit rather than widening this check." % (name, why, want)
            )
        if seen[name] != want:
            raise Refusal(
                "positive control MISCLASSIFIED: %s landed in '%s', expected '%s'"
                % (name, seen[name], want)
            )
    return buckets, dict(objs=len(objs), defined=len(defined), stale=stale, objdir=objdir)


# ---------------------------------------------------------------------------------------------
# selftest
# ---------------------------------------------------------------------------------------------
def selftest():
    import tempfile

    fails = []

    def ck(what, ok):
        if not ok:
            fails.append(what)
        print("  %-72s %s" % (what, "ok" if ok else "FAIL"))

    # ---- the OBJ classifier, on synthetic symbol tables -------------------------------------
    # Planted rather than built: the classification is the part that can be silently wrong, and a
    # real build cannot be made to contain a counterexample on demand.
    defined = {"?body@sim@mh@@YAXXZ": ["sim_step.obj"]}
    undef = {
        "_memset": ["a.obj"],
        "__imp__WriteFile@20": ["a.obj"],
        "?s_void@detail@call@mh@@YAXI@Z": ["b.obj"],
        "?decompress_block@lzss@mh@@YAXPBXPAX@Z": ["c.obj"],
        "?install_export_ok@hook@mh@@YA_NIPAXPBD_K@Z": ["d.obj"],
        "_MH_Harness_WantsWallclockPin": ["e.obj"],
        "?body@sim@mh@@YAXXZ": ["f.obj"],  # defined in the roster -> not an edge at all
    }
    b = classify(defined, undef)
    ck(
        "a CRT symbol is toolchain, never residue",
        list(b["toolchain"]) and "_memset" in b["toolchain"],
    )
    ck("a system import is toolchain", "__imp__WriteFile@20" in b["toolchain"])
    ck(
        "a marshalling shape is the ruled `shim` bucket",
        "?s_void@detail@call@mh@@YAXI@Z" in b["shim"],
    )
    ck(
        "an mh_common codec is the ruled `common` bucket",
        "?decompress_block@lzss@mh@@YAXPBXPAX@Z" in b["common"],
    )
    ck(
        "an mh::hook:: symbol is RESIDUE -- the edge this item closed",
        "?install_export_ok@hook@mh@@YA_NIPAXPBD_K@Z" in b["host"],
    )
    ck("an MH_Harness_ C symbol is RESIDUE too", "_MH_Harness_WantsWallclockPin" in b["host"])
    ck("a symbol the roster DEFINES is not an edge", "?body@sim@mh@@YAXXZ" not in b["host"])
    ck("the residue bucket is exactly those two", len(b["host"]) == 2)
    empty = classify({}, {})
    ck(
        "an empty symbol table yields an empty residue (the caller refuses on the anchors)",
        not empty["host"],
    )

    # ---- the SRC rule, on planted trees ------------------------------------------------------
    # Each case is a file the closure walk is pointed at; the reds are the point. The GUARDED case
    # is the one that distinguishes this tool from lint_libmh_layering, which passes it.
    cases = [
        ("clean module TU", '#include "state/hook_api.h"\nvoid f(){ mh::hosthook::bound(); }\n', 0),
        ("unguarded harness include goes red", '#include "hook/promoted.h"\n', 1),
        (
            "GUARDED harness include goes red TOO (the strengthening)",
            '#ifndef MH_LIBMH_BUILD\n#include "hook/promoted.h"\n#endif\n',
            1,
        ),
        ("a seams/ include goes red", '#include "seams/net_internal.h"\n', 1),
        ("an mh/include export header goes red", '#include "include/mh_harness_export.h"\n', 1),
        ("a shared mh/include header does NOT", '#include "include/packet_buffer.h"\n', 0),
        ("an mh::hook:: token goes red", "void f(){ mh::hook::entry_owner_of(0); }\n", 1),
        ("an MH_Harness_ token goes red", "int f(){ return MH_Harness_WantsWallclockPin(); }\n", 1),
        (
            "the same token in a COMMENT does not",
            "// MH_Harness_WantsWallclockPin is the old edge\n",
            0,
        ),
        ("...nor in a string literal", 'const char *s = "mh::hook::entry_owner_of";\n', 0),
    ]
    for what, body, want in cases:
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "planted.cpp")
            io.open(p, "w", encoding="utf-8").write(body)
            code = strip_comments_and_strings(body)
            n = len(TOKEN_RE.findall(code))
            for m in INCLUDE_RE.finditer(body):
                t = resolve_include(m.group(1), os.path.join(LIBMH, "sim", "planted.cpp"))
                if t is not None and classify_path(t) == "harness":
                    n += 1
            ck(what, n == want)

    # ---- the roster is DERIVED, and the refusal arms ----------------------------------------
    tus, _ = roster_sources()
    ck("the roster comes from gen_libmh_vcxproj (>= 500 TUs)", len(tus) >= 500)
    ck(
        "a harness TU is NOT in the roster (seams/ is excluded by construction)",
        not any(os.sep + "seams" + os.sep in p for p in tus),
    )
    ck("the registration surface IS in the roster", any(p.endswith("hook_api.cpp") for p in tus))

    print()
    print("%d check(s), %d failure(s)" % (10 + len(cases) + 3, len(fails)))
    return 0 if not fails else 2


# ---------------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description="libmh's OUTBOUND edge, measured and ruled (fork F4D-PRE)"
    )
    ap.add_argument("--src-only", action="store_true", help="skip the OBJ pass (no build needed)")
    ap.add_argument(
        "--objdir",
        default=HOSTED_OBJDIR,
        help="hosted objects (default: %(default)s -- libmh_dll.vcxproj's Debug build of the "
        "roster, i.e. the artifact that actually ships)",
    )
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--selftest", action="store_true", help="planted inputs: every negative RED")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    try:
        violations, sstats = scan_src()
        buckets, ostats = (None, None)
        if not args.src_only:
            buckets, ostats = scan_obj(args.objdir)
    except Refusal as e:
        print("[libmh_outbound] REFUSED (exit 2): %s" % e)
        return 2

    if args.json:
        print(
            json.dumps(
                dict(
                    src=dict(violations=violations, **{k: v for k, v in sstats.items()}),
                    obj=None if buckets is None else dict(buckets=buckets, **ostats),
                ),
                indent=1,
                default=str,
            )
        )
    else:
        print("libmh OUTBOUND EDGE -- the 627-TU roster's reach into its host")
        print()
        print(
            "  SRC: %d roster TU(s), %d file(s) in the include closure, %d unresolved include(s)"
            % (sstats["tus"], sstats["files"], len(sstats["unresolved"]))
        )
        for f, ln, spec in sstats["unresolved"][:10]:
            print("       [unresolved] %s:%d  %s" % (f, ln, spec))
        if violations:
            print("       HARNESS REACH (%d):" % len(violations))
            for v in violations:
                print(
                    "         %-6s %s:%d  %s%s"
                    % (
                        v["kind"],
                        v["file"],
                        v["line"],
                        v["detail"],
                        "  -> " + v["reached"] if v["reached"] else "",
                    )
                )
        else:
            print("       no harness include and no harness token, guarded or not")
        print()
        if buckets is None:
            print("  OBJ: SKIPPED (--src-only). The macro-introduced edge is invisible to SRC.")
        else:
            print(
                "  OBJ: %d roster object(s) in %s, %d defined external(s)"
                % (ostats["objs"], ostats["objdir"], ostats["defined"])
            )
            if ostats["stale"]:
                print(
                    "       STALE (%d object(s) older than their source; first: %s)"
                    % (len(ostats["stale"]), ostats["stale"][0])
                )
            for b in ("toolchain", "shim", "common", "host"):
                print("       %-9s %3d symbol(s)" % (b, len(buckets[b])))
            for name in sorted(buckets["shim"])[:3]:
                print("         [shim]   %s" % name)
            if len(buckets["shim"]) > 3:
                print("         [shim]   ... and %d more" % (len(buckets["shim"]) - 3))
            for name in sorted(buckets["common"]):
                print("         [common] %s" % name)
            for name in sorted(buckets["host"]):
                print(
                    "         [host]   %s   <-- RESIDUE: %s"
                    % (name, ", ".join(buckets["host"][name]))
                )
            print()
            print("  THE NUMBER: libmh -> host outbound symbols = %d" % len(buckets["host"]))
        print()

    bad = len(violations) + (0 if buckets is None else len(buckets["host"]))
    if bad:
        print(
            "[libmh_outbound] FAIL: the outbound edge is not closed -- %d source reach(es) + %d "
            "unruled project symbol(s). Route it through libmh/include/libmh_hook.h (add a row) or "
            "remove the dependency; do not re-introduce a guarded harness include."
            % (len(violations), 0 if buckets is None else len(buckets["host"]))
        )
        return 1
    print(
        "[libmh_outbound] OK: the 627-TU roster reaches its host only through the registration "
        "surface%s" % ("" if buckets is not None else " (source arm only)")
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
