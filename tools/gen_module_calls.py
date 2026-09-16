#!/usr/bin/env python3
"""
gen_module_calls.py -- emit a reimplementation module's `calls` table from the generated
mh::call:: declarations.

WHY THIS EXISTS. Every libmh/sim/ and libmh/ai/ translation unit routes its outward calls through an
injectable `struct calls { ... }` of function pointers rather than calling `mh::call::` directly, so
the body can be driven by `net_selftest` with recording stubs (see sim/sim_order_enqueue.h's header
note for the full argument). Binding that table in `live_calls()` is an AGGREGATE INITIALIZER, which
means each member's function-pointer type has to match the generated declaration EXACTLY -- a
`uint32_t` where the thunk says `int32_t`, or `void *` where it says `const wchar_t *`, does not
convert, it fails to compile. Hand-transcribing 76 signatures for one module is a pointless
opportunity to get one wrong.

So this reads the SAME header the binding must match -- src/mh_dll/mh/addr/mh_calls.gen.h -- and
emits both halves from it. That is deliberately not `dll_call_protos.json`: the json is the input to
gen_dll_calls.py, and reproducing its type mapping here would be a second derivation that could
disagree with the first. The header is the thing the compiler sees.

The output is a SNIPPET, not a file: paste the two blocks into the module's .h and .cpp. That is on
purpose -- the members want hand-written comments grouping them by role, and a generated header
would either lose those or have to own the whole module.

Usage:
  # names taken from every CALL in an exported .asm (the usual case: one function's frontier)
  python tools/gen_module_calls.py --from-asm tmp/decomp/llm_strat_order_queue_dispatch_00466892.asm

  # or an explicit list
  python tools/gen_module_calls.py llm_snd_play llm_strat_bldg_get_coords

  # --exclude drops callees the module does not route (a stack probe, or one it inlines)
  python tools/gen_module_calls.py --from-asm X.asm --exclude utils_assert_stack_capacity
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CALLS_HEADER = REPO / "src" / "mh_dll" / "mh" / "addr" / "mh_calls.gen.h"

# `inline <ret> <name>(<params>) { ... }` -- the callable form. Anything else in that header is
# either a detail:: shim or an MH_UNAVAILABLE stub, and both are handled explicitly below.
DECL = re.compile(r"^inline\s+(.+?)\s+([A-Za-z_]\w*)\((.*?)\)\s*\{", re.M)
UNAVAILABLE = re.compile(r"^MH_UNAVAILABLE\S*\s+([A-Za-z_]\w*)\(", re.M)

# A parameter as declared: `int32_t building_index`, `void *out_x`, `const wchar_t *format`. The
# member's type is everything but the trailing identifier; keep the pointer stars with the type.
PARAM_NAME = re.compile(r"([A-Za-z_]\w*)\s*$")


def param_types(params):
    """`int32_t a, void *b` -> `int32_t, void *`. Empty/void -> ''."""
    params = params.strip()
    if not params or params == "void":
        return ""
    out = []
    for p in params.split(","):
        p = p.strip()
        m = PARAM_NAME.search(p)
        # A bare type with no parameter name (rare, but legal) has nothing to strip.
        t = p[: m.start()].rstrip() if m and p[: m.start()].strip() else p
        out.append(t)
    return ", ".join(out)


def load_decls():
    src = CALLS_HEADER.read_text(encoding="utf-8")
    decls = {}
    for ret, name, params in DECL.findall(src):
        # A name can appear twice only if gen_dll_calls emitted a varargs SHAPE alongside the base;
        # shapes carry the `__<id>` suffix, so the names differ and this stays a plain dict.
        decls[name] = (ret.strip(), params)
    return decls, set(UNAVAILABLE.findall(src))


def names_from_asm(path):
    """Every distinct CALL target NAME in an exported .asm, in first-seen order.

    Order matters a little: it puts the members in the order the function reaches them, which reads
    better than alphabetical for a hand-grouped table. Reads the resolved-operand comment, which is
    the same source the mechanical translation lint uses.
    """
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    seen = []
    for m in re.finditer(r"CALL 0x[0-9a-f]+\s+;\s*(\S+)", text):
        if m.group(1) not in seen:
            seen.append(m.group(1))
    return seen


# ---- --recording: the offline oracle's half ------------------------------------------------------
#
# A module's `calls` table exists so `net_selftest simtest` can drive the body with stubs instead of
# the game image. Writing those stubs by hand is the same transcription problem the struct itself
# had, one member later and with more to get wrong: a stub that widens its argument differently from
# the member's declared type records a value the test then asserts on. So the recording table is
# generated from THE STRUCT ITSELF -- not from mh_calls.gen.h -- because the struct is what the
# aggregate initializer must match, member for member and IN ORDER.
#
# Emitting a whole file rather than a snippet (the mode above emits a snippet) is deliberate: there
# is nothing here a human would want to comment per member, and 70+ stubs is not a paste.

# `void (*game_SpendResource)(int32_t, int32_t, int32_t)` -- with the declaration already collapsed
# onto one line by struct_members. IT MUST NOT BE MATCHED LINE-BY-LINE: a member whose parameter list
# wraps (llm_strat_ai_notify_bldg_constructed takes six uint32_t and wraps at column 100 under the
# house clang-format) then fails to match and is SILENTLY DROPPED -- and a C++ aggregate initializer
# with too few initializers is legal, so the omission shifts every later stub onto the wrong member
# instead of failing. Caught on this generator's first run; the count check below is the guard.
MEMBER = re.compile(r"^(.+?)\s*\(\*(\w+)\)\((.*?)\)$")
COMMENT = re.compile(r"//[^\n]*")


def struct_members(header, struct):
    """[(ret, name, [param types])] in declaration order, for `struct <struct> { ... };`."""
    src = Path(header).read_text(encoding="utf-8")
    m = re.search(r"\bstruct\s+%s\s*\{(.*?)\n\}\s*;" % re.escape(struct), src, re.S)
    if not m:
        raise SystemExit("gen_module_calls: no `struct %s { ... };` in %s" % (struct, header))
    body = COMMENT.sub("", m.group(1))
    # One declaration per `;`, whitespace collapsed, so a wrapped parameter list reads as one line.
    decls = [" ".join(d.split()) for d in body.split(";")]
    decls = [d for d in decls if d]
    out = []
    for d in decls:
        mm = MEMBER.match(d)
        if not mm:
            # REFUSE rather than skip. Anything inside this struct that is not a function pointer is
            # something this generator does not understand, and guessing produces a table that is
            # quietly one member short.
            raise SystemExit(
                "gen_module_calls: cannot parse a member of `struct %s`:\n    %s\n"
                "Every member must be a plain function pointer." % (struct, d)
            )
        ret, name, params = mm.group(1).strip(), mm.group(2), mm.group(3).strip()
        ptypes = [] if not params or params == "void" else [p.strip() for p in params.split(",")]
        out.append((ret, name, ptypes))
    return out


def arg_class(t):
    """How a parameter of type `t` is recorded: 'p'ointer, 'f'loat, or 'i'ntegral."""
    if "*" in t:
        return "p"
    if "double" in t or "float" in t:
        return "f"
    return "i"


def emit_recording(header, struct, names_hint):
    ms = struct_members(header, struct)
    maxa = max((sum(1 for t in p if arg_class(t) != "f") for _, _, p in ms), default=0)
    maxd = max((sum(1 for t in p if arg_class(t) == "f") for _, _, p in ms), default=0)
    L = []
    w = L.append
    w("//")
    w("// %s -- GENERATED by tools/gen_module_calls.py --recording. Do not hand-edit." % names_hint)
    w("// Regenerate with the command in the module's own note; the STRUCT is the input, so this")
    w("// file cannot drift from it without the build breaking at the aggregate initializer.")
    w("//")
    w("// The offline oracle's stub half of `%s`. Every member records the call and its" % struct)
    w("// arguments into one log and returns a per-member configurable value, so a simtest case")
    w(
        "// asserts on WHICH callees ran, IN WHAT ORDER, and WITH WHAT ARGUMENTS -- the three things a"
    )
    w("// state-only comparison cannot see. Arguments are widened to long long (pointers via")
    w(
        "// intptr_t) and floating-point ones kept separately, so a test reads back exactly the value"
    )
    w("// the member's declared type carried.")
    w("//")
    w(
        "// To make one member behave specially (an out-parameter, a canned return that depends on the"
    )
    w("// arguments), copy the table and overwrite that member -- it is an aggregate:")
    w("//     %s c = rec::recording_calls();" % struct)
    w("//     c.some_member = &my_own_stub;")
    w("#pragma once")
    w("#include <cstdint>")
    w("#include <cstring>")
    w("#include <vector>")
    w("")
    w('#include "sim/sim_order_dispatch.h"')
    w("")
    w("namespace mh::sim::rec {")
    w("")
    w("enum dc_fn : int {")
    for _, n, _ in ms:
        w("    DC_%s," % n)
    w("    DC_COUNT,")
    w("};")
    w("")
    w("inline const char *dc_name(int fn) {")
    w("    static const char *const n[DC_COUNT] = {")
    for _, n, _ in ms:
        w('        "%s",' % n)
    w("    };")
    w('    return (fn >= 0 && fn < DC_COUNT) ? n[fn] : "?";')
    w("}")
    w("")
    w(
        "// One recorded call. `a` holds the integral and pointer arguments in declaration order, `d`"
    )
    w("// the floating-point ones in declaration order -- two sequences, because a double does not")
    w("// survive a long long and an int does not survive a double.")
    w("struct dc_event {")
    w("    int       fn;")
    w("    int       na, nd;")
    w("    long long a[%d];" % max(maxa, 1))
    w("    double    d[%d];" % max(maxd, 1))
    w("};")
    w("")
    w("struct dc_log {")
    w("    std::vector<dc_event> events;")
    w("    // What each stub returns. Zero by default, which is the quiet answer for every one of")
    w("    // these callees -- a test that needs a nonzero status sets ret[DC_x] itself.")
    w("    long long ret[DC_COUNT];")
    w("")
    w("    void reset() {")
    w("        events.clear();")
    w("        std::memset(ret, 0, sizeof(ret));")
    w("    }")
    w("    int count(int fn) const {")
    w("        int n = 0;")
    w("        for (const dc_event &e : events)")
    w("            if (e.fn == fn) ++n;")
    w("        return n;")
    w("    }")
    w("    // The k-th call of `fn` (0-based), or null. `last` is nth(fn, count(fn)-1).")
    w("    const dc_event *nth(int fn, int k) const {")
    w("        for (const dc_event &e : events)")
    w("            if (e.fn == fn && k-- == 0) return &e;")
    w("        return nullptr;")
    w("    }")
    w("    const dc_event *last(int fn) const { return nth(fn, count(fn) - 1); }")
    w(
        "    // Index of the first call of `fn` in the whole sequence, or -1. Order between DIFFERENT"
    )
    w("    // callees is the property a lockstep/network path most needs asserted.")
    w("    int first_index(int fn) const {")
    w("        for (int i = 0; i < (int)events.size(); ++i)")
    w("            if (events[i].fn == fn) return i;")
    w("        return -1;")
    w("    }")
    w("};")
    w("")
    w("// The single log the stubs below record into. One per process is enough: simtest is")
    w("// single-threaded and every case calls reset() first.")
    w("inline dc_log &dc() {")
    w("    static dc_log g;")
    w("    return g;")
    w("}")
    w("")
    for ret, n, ps in ms:
        args, body = [], []
        ia = fa = 0
        for i, t in enumerate(ps):
            args.append("%s a%d" % (t, i))
            k = arg_class(t)
            if k == "f":
                body.append("e.d[%d] = (double)a%d;" % (fa, i))
                fa += 1
            elif k == "p":
                body.append("e.a[%d] = (long long)(intptr_t)a%d;" % (ia, i))
                ia += 1
            else:
                body.append("e.a[%d] = (long long)a%d;" % (ia, i))
                ia += 1
        w("inline %s dc_stub_%s(%s) {" % (ret, n, ", ".join(args)))
        w("    dc_event e{};")
        w("    e.fn = DC_%s;" % n)
        w("    e.na = %d;" % ia)
        w("    e.nd = %d;" % fa)
        for b in body:
            w("    " + b)
        w("    dc().events.push_back(e);")
        if ret != "void":
            cast = "(%s)(intptr_t)" % ret if "*" in ret else "(%s)" % ret
            w("    return %sdc().ret[DC_%s];" % (cast, n))
        w("}")
    w("")
    w(
        "// Member for member, in declaration order -- an aggregate initializer does not convert, so a"
    )
    w("// stub whose signature drifted from its member is a compile error here rather than a wrong")
    w("// number in a test.")
    w("inline %s recording_calls() {" % struct)
    w("    %s c = {" % struct)
    for _, n, _ in ms:
        w("        dc_stub_%s," % n)
    w("    };")
    w("    return c;")
    w("}")
    w("")
    w("} // namespace mh::sim::rec")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="*", help="callee names")
    ap.add_argument("--from-asm", help="take the names from every CALL in this exported .asm")
    ap.add_argument("--exclude", action="append", default=[], help="drop a name (repeatable)")
    ap.add_argument("--sort", action="store_true", help="alphabetical instead of first-seen order")
    ap.add_argument(
        "--recording",
        nargs=2,
        metavar=("HEADER", "STRUCT"),
        help="emit the offline oracle's recording stubs for `struct STRUCT` in HEADER",
    )
    ap.add_argument("--out", help="write to this path instead of stdout (with --recording)")
    ap.add_argument(
        "--check",
        action="store_true",
        help="with --recording --out: exit 1 if the file on disk is not what we emit",
    )
    a = ap.parse_args()

    if a.recording:
        header, struct = a.recording
        text = emit_recording(header, struct, Path(a.out).name if a.out else struct)
        if not a.out:
            print(text, end="")
            return 0
        out = Path(a.out)
        if a.check:
            have = out.read_text(encoding="utf-8") if out.exists() else ""
            if have != text:
                print(
                    "gen_module_calls: %s is STALE -- re-run without --check" % out, file=sys.stderr
                )
                return 1
            return 0
        out.write_text(text, encoding="utf-8", newline="\n")
        print("wrote %s (%d members)" % (out, len(struct_members(header, struct))), file=sys.stderr)
        return 0

    names = list(a.names)
    if a.from_asm:
        names += [n for n in names_from_asm(a.from_asm) if n not in names]
    names = [n for n in names if n not in set(a.exclude)]
    if a.sort:
        names.sort()
    if not names:
        ap.error("no names: pass some, or --from-asm")

    decls, unavailable = load_decls()

    # REFUSE rather than emit a table with a hole. A missing or uncallable callee is a readiness
    # failure (migration_ready's R2), and the module's build error would point at the aggregate
    # initializer rather than at the callee -- so say which one, here.
    problems = []
    for n in names:
        if n in unavailable:
            problems.append("%s: MH_UNAVAILABLE (no committed signature -- R2 blocker)" % n)
        elif n not in decls:
            problems.append("%s: not in %s" % (n, CALLS_HEADER.name))
    if problems:
        print("gen_module_calls: REFUSING --", file=sys.stderr)
        for p in problems:
            print("  " + p, file=sys.stderr)
        return 1

    width = max(len(decls[n][0]) for n in names)
    print("// ---- struct calls members (paste into the module header) ----")
    for n in names:
        ret, params = decls[n]
        print("    %-*s (*%s)(%s);" % (width, ret, n, param_types(params)))
    print()
    print("// ---- live_calls() initializer (paste into the module .cpp) ----")
    for n in names:
        print("        mh::call::%s," % n)
    print()
    print("// %d callee(s)" % len(names), file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
