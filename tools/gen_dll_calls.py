#!/usr/bin/env python3
"""
gen_dll_calls.py -- generate typed C++ callables for the original game's functions from
tools/data/dll_call_protos.json (itself dumped from /eng/mh.exe by the Ghidra-side call-proto dump).

Emits TWO files:
  src/mh_dll/mh/addr/mh_calls.gen.h    -- one typed inline wrapper per callable game function
  src/mh_dll/mh/addr/mh_calls.gen.cpp  -- one naked-asm marshalling thunk per distinct SHAPE

The call-side member of the generated-header family (mh_addrs.gen.h = addresses, mh_structs.gen.h
= layouts). mh.exe is Watcom-built, so most functions take their first args in EAX/EDX/EBX/ECX and
spill the rest to the stack; MSVC cannot express that, so every call from injected/reimplemented
code into original code was hand-written register marshalling. This replaces it.

Ghidra-FREE: reads the committed JSON only. `--check` is the drift gate (tools/lint_repo.py).

HOW IT WORKS
  A function's marshalling SHAPE is (return class, ordered arg slots). Slots are read from Ghidra's
  real per-parameter storage, so nothing here assumes the __watcall register order -- and it had
  better not, because 5 functions genuinely pass EAX/EBX/EDX in that order. 1258 callable functions
  collapse onto ~83 shapes, so the asm that has to be right is small and hand-checkable.

  Narrow args (AX:2, BL:1, ...) are collapsed onto their 32-bit parent register: the wrapper's C
  cast produces the correctly sign- or zero-extended 32-bit value, which is what a Watcom caller
  would have in that register anyway, and the callee reads only the low part its prototype declares.

  Stack args are written at their DECLARED offsets into an explicitly-sized outgoing frame rather
  than pushed in sequence, so a gap in the offsets can't silently shift every later argument.

  The epilogue restores ESP from EBP instead of popping a known amount. That makes each thunk
  correct under EITHER cleanup discipline -- __watcall/__stdcall callees pop their own stack args,
  __cdecl/__custumocall ones don't (Ghidra extrapop==stackshift) -- so the single nastiest way to
  get this wrong (an unbalanced ESP at every call site) is designed out rather than tracked.

WHAT IS DELIBERATELY NOT CALLABLE
  A function whose storage Ghidra never committed gets a declaration returning an INCOMPLETE type
  named for the reason. Declaring it is legal; calling it is a compile error whose text says why
  ("use of undefined type MH_UNAVAILABLE__return_type_never_determined"). Absence is loud, and
  prototyping stays demand-driven: the reimplementation's call-tree closure decides which of the
  1173 un-prototyped functions is worth an RE pass, instead of a 1173-function campaign up front.

Usage:
  python tools/gen_dll_calls.py          # regenerate both files in place
  python tools/gen_dll_calls.py --check  # fail if either differs from a fresh regen
"""

import argparse
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PROTOS = REPO / "tools" / "data" / "dll_call_protos.json"
HEADER = REPO / "src" / "mh_dll" / "mh" / "addr" / "mh_calls.gen.h"
SOURCE = REPO / "src" / "mh_dll" / "mh" / "addr" / "mh_calls.gen.cpp"
SELFTEST = REPO / "src" / "mh_dll" / "mh_nettest" / "mh_calls_selftest.gen.cpp"

# return class -> (C type of the shape thunk, asm fixup applied right after the call)
RET_C = {"void": "void", "u32": "uint32_t", "u16": "uint32_t", "u8": "uint32_t", "f64": "double"}
RET_FIX = {"u16": "movzx eax, ax", "u8": "movzx eax, al"}

# Why a function is not callable -> the incomplete type whose name shows up in the compile error.
REASON = {
    "no_prototype": "MH_UNAVAILABLE__no_prototype_committed_in_Ghidra",
    "unassigned_return": "MH_UNAVAILABLE__return_type_never_determined",
    "stack_purge_mismatch": "MH_UNAVAILABLE__prototype_contradicts_the_functions_own_RET",
    "varargs": "MH_UNAVAILABLE__varargs_cannot_be_marshalled",
    "unsupported_return_storage": "MH_UNAVAILABLE__non_standard_return_register",
    "unsupported_param_storage": "MH_UNAVAILABLE__parameter_storage_not_marshallable",
    "unsupported_param_type": "MH_UNAVAILABLE__parameter_type_not_marshallable",
    "unassigned_param": "MH_UNAVAILABLE__parameter_storage_not_marshallable",
}

CPP_KEYWORDS = {
    "alignas",
    "alignof",
    "and",
    "asm",
    "auto",
    "bool",
    "break",
    "case",
    "catch",
    "char",
    "class",
    "const",
    "constexpr",
    "continue",
    "default",
    "delete",
    "do",
    "double",
    "else",
    "enum",
    "explicit",
    "export",
    "extern",
    "false",
    "float",
    "for",
    "friend",
    "goto",
    "if",
    "inline",
    "int",
    "long",
    "mutable",
    "namespace",
    "new",
    "not",
    "operator",
    "or",
    "private",
    "protected",
    "public",
    "register",
    "return",
    "short",
    "signed",
    "sizeof",
    "static",
    "struct",
    "switch",
    "template",
    "this",
    "throw",
    "true",
    "try",
    "typedef",
    "typename",
    "union",
    "unsigned",
    "using",
    "virtual",
    "void",
    "volatile",
    "while",
    "xor",
}


def slot_tag(slot):
    """A slot's contribution to the shape id: the parent register, or S<size>/B<size> on the stack."""
    if slot["kind"] == "reg":
        return slot["reg"]
    if slot["kind"] == "blob":
        return "B%d" % slot["size"]
    return "S%d" % slot["size"]


def shape_of(fn):
    """(shape_id, slots) -- cleanup discipline is deliberately NOT part of the key; see the header."""
    slots = [p["slot"] for p in fn["params"]]
    tags = [slot_tag(s) for s in slots]
    return "s_" + "_".join([fn["ret"].get("storage", "void")] + tags), slots


def thunk_param_ctype(slot):
    if slot["kind"] == "blob":
        return "const void *"
    if slot["kind"] == "stack" and slot["size"] == 8:
        return "uint64_t"
    return "uint32_t"


def emit_thunk(shape_id, ret_cls, slots):
    """One naked __cdecl marshalling thunk for a shape. Returns (declaration, definition)."""
    rc = RET_C[ret_cls]
    params = ["uintptr_t fn"] + ["%s a%d" % (thunk_param_ctype(s), i) for i, s in enumerate(slots)]
    sig = "%s __cdecl %s(%s)" % (rc, shape_id, ", ".join(params))

    # incoming cdecl frame: [ebp+8]=fn, then each arg in declaration order
    off, arg_at = 0x0C, []
    for s in slots:
        arg_at.append(off)
        off += 8 if (s["kind"] == "stack" and s["size"] == 8) else 4

    stack_slots = [(i, s) for i, s in enumerate(slots) if s["kind"] in ("stack", "blob")]
    total = max((s["off"] + s["size"] for _i, s in stack_slots), default=4) - 4
    has_blob = any(s["kind"] == "blob" for _i, s in stack_slots)

    a = []
    a.append("push ebp")
    a.append("mov  ebp, esp")
    a.append("push ebx")
    a.append("push esi")
    a.append("push edi")
    if has_blob:
        a.append("cld")
    if stack_slots:
        # outgoing frame written at DECLARED offsets (off 4 == the first arg, at [esp] pre-call)
        a.append("sub  esp, %d" % total)
        for i, s in stack_slots:
            dst = s["off"] - 4
            src = arg_at[i]
            if s["kind"] == "blob":
                a.append("lea  edi, [esp + %d]" % dst)
                a.append("mov  esi, [ebp + %d]" % src)
                a.append("mov  ecx, %d" % (s["size"] // 4))
                a.append("rep  movsd")
            elif s["size"] == 8:
                a.append("mov  eax, [ebp + %d]" % src)
                a.append("mov  [esp + %d], eax" % dst)
                a.append("mov  eax, [ebp + %d]" % (src + 4))
                a.append("mov  [esp + %d], eax" % (dst + 4))
            else:
                a.append("mov  eax, [ebp + %d]" % src)
                a.append("mov  [esp + %d], eax" % dst)
    # register args last, EAX last of all (it is the scratch used above)
    regs = [(i, s) for i, s in enumerate(slots) if s["kind"] == "reg"]
    for i, s in sorted(regs, key=lambda t: t[1]["reg"] == "EAX"):
        a.append("mov  %s, [ebp + %d]" % (s["reg"].lower(), arg_at[i]))
    a.append("call dword ptr [ebp + 8]")
    if ret_cls in RET_FIX:
        a.append(RET_FIX[ret_cls])
    a.append("lea  esp, [ebp - 12]")  # discipline-agnostic: undo whatever the callee did to ESP
    a.append("pop  edi")
    a.append("pop  esi")
    a.append("pop  ebx")
    a.append("pop  ebp")
    a.append("ret")

    body = ["__declspec(naked) " + sig + " {", "    __asm {"]
    body += ["        " + line for line in a]
    body += ["    }", "}"]
    return sig + ";", "\n".join(body)


def cast_to_slot(ctype, slot):
    """C expression fragment turning a wrapper arg into the thunk's raw slot type."""
    if slot["kind"] == "blob":
        return "(const void *)%s"
    if ctype.endswith("*"):
        return "(uint32_t)(uintptr_t)%s"
    if slot["kind"] == "stack" and slot["size"] == 8:
        # Fully qualified on purpose: gen_dll_shadow.py reuses this to build call expressions that
        # land at GLOBAL scope (inside MH_SHADOW_REPLACE), where a bare `detail::` does not resolve.
        return "::mh::call::detail::bits(%s)" if ctype == "double" else "(uint64_t)%s"
    return "(uint32_t)%s"


def param_names(fn):
    out, seen = [], {}
    for i, p in enumerate(fn["params"]):
        n = p["name"] or ("a%d" % i)
        if n in CPP_KEYWORDS:
            n = n + "_"
        if n in seen:
            n = "%s_%d" % (n, i)
        seen[n] = True
        out.append(n)
    return out


def slot_values(slots):
    """Deterministic distinctive test values, one per slot (blobs get a dword-filled buffer)."""
    vals = []
    for i, s in enumerate(slots):
        if s["kind"] == "blob":
            vals.append([0xB10B0000 + (i << 8) + j for j in range(s["size"] // 4)])
        elif s["kind"] == "stack" and s["size"] == 8:
            vals.append(0xF00D0000_C0DE0000 + (i << 8))
        else:
            vals.append(0xC0DE0000 + (i << 8) + 0x11)
    return vals


def emit_stub(shape_id, ret_cls, slots, pops):
    """A fake Watcom CALLEE for one shape: records what it actually received, then returns a
    recognisable value. `pops` picks the cleanup discipline it emulates (callee-pops `ret N` vs
    caller-pops plain `ret`) -- the thunk must be right for both."""
    name = "stub_%s__%s" % (shape_id, "callee" if pops else "caller")
    stack_bytes = max((s["off"] + s["size"] for s in slots if s["kind"] != "reg"), default=4) - 4

    a, k = [], 0
    rec = []  # (g_seen index, kind) in slot order, for the checker
    # registers FIRST: the blob-sum loop below clobbers EAX/ECX/ESI
    for i, s in enumerate(slots):
        if s["kind"] == "reg":
            a.append("mov  dword ptr [g_seen + %d], %s" % (k * 4, s["reg"].lower()))
            rec.append((i, k, "reg"))
            k += 1
    for i, s in enumerate(slots):
        if s["kind"] == "stack":
            a.append("mov  eax, [esp + %d]" % s["off"])
            a.append("mov  dword ptr [g_seen + %d], eax" % (k * 4))
            k += 1
            if s["size"] == 8:
                a.append("mov  eax, [esp + %d]" % (s["off"] + 4))
                a.append("mov  dword ptr [g_seen + %d], eax" % (k * 4))
                k += 1
            rec.append((i, k - (2 if s["size"] == 8 else 1), "stack"))
        elif s["kind"] == "blob":
            # sum the blob's dwords: proves every byte arrived at the right offset
            a.append("xor  eax, eax")
            a.append("lea  esi, [esp + %d]" % s["off"])
            a.append("mov  ecx, %d" % (s["size"] // 4))
            a.append("blobsum%d:" % i)
            a.append("add  eax, [esi]")
            a.append("add  esi, 4")
            a.append("dec  ecx")
            a.append("jnz  blobsum%d" % i)
            a.append("mov  dword ptr [g_seen + %d], eax" % (k * 4))
            rec.append((i, k, "blob"))
            k += 1

    if ret_cls == "f64":
        a.append("fld  qword ptr [g_stub_f64]")
    elif ret_cls == "u32":
        a.append("mov  eax, 0DEADBEEFh")
    elif ret_cls in ("u16", "u8"):
        # garbage in the high bits ON PURPOSE: only AL/AX carries the result, so the thunk must
        # mask the rest off rather than hand it back
        a.append("mov  eax, 0FFFFFF5Ah" if ret_cls == "u8" else "mov  eax, 0FFFFBEEFh")
    a.append("ret %d" % stack_bytes if (pops and stack_bytes) else "ret")

    body = ["__declspec(naked) void %s() {" % name, "    __asm {"]
    body += ["        " + ln for ln in a]
    body += ["    }", "}"]
    return name, rec, "\n".join(body)


def emit_selftest(shapes):
    """A no-game, seconds-long exhaustive check of every generated marshalling thunk."""
    o = [
        "// GENERATED by tools/gen_dll_calls.py -- DO NOT EDIT.",
        "//",
        "// Exhaustive correctness oracle for the naked marshalling thunks in mh_calls.gen.cpp.",
        "// For every distinct storage shape it stands up a fake Watcom CALLEE that records what it",
        "// actually received in each register and at each stack offset, calls it through the",
        "// generated thunk with distinctive values, and checks all of: every argument landed in the",
        "// right place, the return value came back correctly (including narrow returns whose high",
        "// bits the stub deliberately fills with garbage, and ST0 doubles), and ESP is balanced.",
        "//",
        "// Each shape that carries stack arguments is tested against BOTH cleanup disciplines --",
        "// a callee that pops its own args (__watcall/__stdcall) and one that does not (__cdecl/",
        "// __custumocall) -- because the thunks claim to be correct for either.",
        "//",
        "// No game process and no Ghidra: this runs in the DLL gate next to the transport selftests.",
        '#include "addr/mh_calls.gen.h"',
        "",
        "#include <cstdint>",
        "#include <cstdio>",
        "#include <cstring>",
        "",
        "static uint32_t g_seen[64];",
        "static double   g_stub_f64 = 12345.678;",
        "static int      g_checks;",
        "static int      g_fails;",
        "",
        "static void check(const char *shape, const char *what, bool ok) {",
        "    ++g_checks;",
        "    if (!ok) {",
        "        ++g_fails;",
        '        printf("  FAIL %-56s %s\\n", shape, what);',
        "    }",
        "}",
        "",
        "// clang-format off",
    ]

    tests = []
    for sid in sorted(shapes):
        ret_cls, slots = shapes[sid]
        has_stack = any(s["kind"] != "reg" for s in slots)
        vals = slot_values(slots)
        for pops in [True, False] if has_stack else [True]:
            stub, rec, code = emit_stub(sid, ret_cls, slots, pops)
            o.append(code)
            o.append("")
            tests.append((sid, ret_cls, slots, vals, stub, rec, pops))

    o.append("// clang-format on")
    o.append("")

    for sid, ret_cls, slots, vals, stub, rec, pops in tests:
        label = "%s [%s-pops]" % (sid, "callee" if pops else "caller")
        f = ["static void test_%s() {" % stub[5:]]
        f.append("    std::memset(g_seen, 0, sizeof g_seen);")
        args = ["(uintptr_t)&%s" % stub]
        for i, s in enumerate(slots):
            if s["kind"] == "blob":
                f.append(
                    "    static const uint32_t blob%d[] = {%s};"
                    % (i, ", ".join("0x%08xu" % v for v in vals[i]))
                )
                args.append("blob%d" % i)
            elif s["kind"] == "stack" and s["size"] == 8:
                args.append("0x%016xull" % vals[i])
            else:
                args.append("0x%08xu" % vals[i])
        call = "mh::call::detail::%s(%s)" % (sid, ", ".join(args))
        f.append("    volatile uint32_t canary = 0xC0FFEEu;")
        f.append("    uint32_t esp0 = 0, esp1 = 0;")
        f.append("    __asm mov esp0, esp")
        if ret_cls == "void":
            f.append("    %s;" % call)
        elif ret_cls == "f64":
            f.append("    double r = %s;" % call)
        else:
            f.append("    uint32_t r = %s;" % call)
        f.append("    __asm mov esp1, esp")
        f.append('    check("%s", "ESP unbalanced", esp0 == esp1);' % label)
        f.append('    check("%s", "stack canary clobbered", canary == 0xC0FFEEu);' % label)
        for i, k, kind in rec:
            if kind == "blob":
                total = sum(vals[i]) & 0xFFFFFFFF
                f.append(
                    '    check("%s", "arg%d blob bytes", g_seen[%d] == 0x%08xu);'
                    % (label, i, k, total)
                )
            elif kind == "stack" and slots[i]["size"] == 8:
                lo, hi = vals[i] & 0xFFFFFFFF, (vals[i] >> 32) & 0xFFFFFFFF
                f.append('    check("%s", "arg%d lo", g_seen[%d] == 0x%08xu);' % (label, i, k, lo))
                f.append(
                    '    check("%s", "arg%d hi", g_seen[%d] == 0x%08xu);' % (label, i, k + 1, hi)
                )
            else:
                where = slots[i]["reg"] if kind == "reg" else "stack+%d" % slots[i]["off"]
                f.append(
                    '    check("%s", "arg%d in %s", g_seen[%d] == 0x%08xu);'
                    % (label, i, where, k, vals[i])
                )
        if ret_cls == "u32":
            f.append('    check("%s", "return value", r == 0xDEADBEEFu);' % label)
        elif ret_cls == "u8":
            f.append('    check("%s", "narrow return not masked", r == 0x5Au);' % label)
        elif ret_cls == "u16":
            f.append('    check("%s", "narrow return not masked", r == 0xBEEFu);' % label)
        elif ret_cls == "f64":
            f.append('    check("%s", "ST0 return value", r == 12345.678);' % label)
        f.append("}")
        o.append("\n".join(f))
        o.append("")

    o.append("int run_callstest() {")
    o.append(
        '    printf("=== callstest (%d generated marshalling thunks, both cleanup disciplines) ===\\n");'
        % len(shapes)
    )
    for _sid, _r, _s, _v, stub, _rec, _p in tests:
        o.append("    test_%s();" % stub[5:])
    o.append('    printf("%d checks, %d failures\\n", g_checks, g_fails);')
    o.append("    return g_fails == 0 ? 0 : 1;")
    o.append("}")
    o.append("")
    return "\n".join(o)


def emit(protos):
    fns = protos["functions"]
    ok = [f for f in fns if f["status"] == "ok"]

    shapes = {}
    for f in ok:
        sid, slots = shape_of(f)
        shapes.setdefault(sid, (f["ret"].get("storage", "void"), slots))

    banner = [
        "// GENERATED by tools/gen_dll_calls.py from tools/data/dll_call_protos.json -- DO NOT EDIT.",
        "// Regenerate after re-dumping the JSON (mh_dump_call_protos.py against /eng/mh.exe);",
        "// `gen_dll_calls.py --check` is the drift gate.",
    ]

    # ---------------- header ----------------
    h = list(banner)
    h += [
        "//",
        "// Typed C++ callables for the original game's functions. EN build (/eng/mh.exe), image",
        "// base 0x00400000, no ASLR -- the DLL is EN-only and refuses to arm on anything else.",
        "//",
        "// A function appears here in one of two forms:",
        "//   * CALLABLE   -- an inline wrapper with real parameter types, marshalling through the",
        "//                   naked shape thunk in mh_calls.gen.cpp that matches its storage.",
        "//   * UNAVAILABLE-- a declaration returning an INCOMPLETE type named for the reason. It",
        "//                   costs nothing to declare; CALLING it is a compile error that says why.",
        "//                   Fix by committing the prototype in Ghidra and re-dumping -- never by",
        "//                   hand-editing this file.",
        "#pragma once",
        "#include <cstdint>",
        "#include <cstring>",
        '#include "addr/mh_structs.gen.h"',
        "",
        "namespace mh::call {",
        "",
        "// ---- reasons a function is not callable (incomplete on purpose: the name IS the error) ----",
    ]
    for tag in sorted(set(REASON.values())):
        h.append("struct %s;" % tag)
    h += [
        "",
        "namespace detail {",
        "",
        "// bit pattern of a double for an 8-byte stack slot (the callee expects raw IEEE bytes)",
        "inline uint64_t bits(double d) {",
        "    uint64_t u;",
        "    std::memcpy(&u, &d, sizeof(u));",
        "    return u;",
        "}",
        "",
        "// ---- marshalling thunks, one per distinct storage shape (defined in mh_calls.gen.cpp) ----",
    ]
    for sid in sorted(shapes):
        ret_cls, slots = shapes[sid]
        decl, _ = emit_thunk(sid, ret_cls, slots)
        h.append(decl)
    h += ["", "} // namespace detail", "", "// ---- callable game functions ----"]

    for f in sorted(ok, key=lambda x: x["va"]):
        sid, slots = shape_of(f)
        names = param_names(f)
        rct = f["ret"]["ctype"]
        rcls = f["ret"].get("storage", "void")
        sig_params = ", ".join(
            "%s%s%s" % (p["ctype"], "" if p["ctype"].endswith("*") else " ", n)
            for p, n in zip(f["params"], names)
        )
        args = ", ".join(
            ["%su" % f["va"]]
            + [cast_to_slot(p["ctype"], p["slot"]) % n for p, n in zip(f["params"], names)]
        )
        call = "detail::%s(%s)" % (sid, args)
        if rcls == "void" or rct == "void":
            body = "%s;" % call
        elif rct.endswith("*"):
            body = "return (%s)(uintptr_t)%s;" % (rct, call)
        elif rcls == "f64":
            body = "return (%s)%s;" % (rct, call)
        else:
            body = "return (%s)%s;" % (rct, call)
        h.append(
            "inline %s%s%s(%s) { %s }  // %s %s"
            % (
                rct,
                "" if rct.endswith("*") else " ",
                f["cident"],
                sig_params,
                body,
                f["cc"],
                f["va"],
            )
        )

    h += ["", "// ---- NOT callable: commit the prototype in Ghidra, then re-dump ----"]
    for f in sorted((x for x in fns if x["status"] != "ok"), key=lambda x: x["va"]):
        tag = REASON.get(f["status"], "MH_UNAVAILABLE__parameter_storage_not_marshallable")
        h.append("%s %s(...);  // %s %s" % (tag, f["cident"], f["va"], f["cc"]))

    h += ["", "} // namespace mh::call", ""]

    # ---------------- source ----------------
    c = list(banner)
    c += [
        "//",
        "// Naked marshalling thunks. Each takes the target address plus the shape's raw slot values",
        "// under __cdecl, places them where the Watcom callee expects, calls, and normalises the",
        "// result. The epilogue restores ESP from EBP rather than popping a fixed amount, so a thunk",
        "// is correct whether the callee cleans its own stack args (__watcall/__stdcall) or not",
        "// (__cdecl/__custumocall).",
        "//",
        "// EBX/ESI/EDI are saved: EBX and ECX are __watcall ARGUMENT registers (so the callee may",
        "// clobber them) while EBX/ESI/EDI are callee-saved under the MSVC convention we return to.",
        '#include "addr/mh_calls.gen.h"',
        "",
        "namespace mh::call::detail {",
        "",
        "// clang-format off  (one instruction per line; clang-format would fold the __asm block)",
    ]
    for sid in sorted(shapes):
        ret_cls, slots = shapes[sid]
        _, defn = emit_thunk(sid, ret_cls, slots)
        c.append(defn)
        c.append("")
    c += ["// clang-format on", "", "} // namespace mh::call::detail", ""]

    return (
        "\n".join(h),
        "\n".join(c),
        emit_selftest(shapes),
        len(ok),
        len(fns) - len(ok),
        len(shapes),
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--check", action="store_true", help="drift gate: fail if either file differs from regen"
    )
    args = ap.parse_args()

    protos = json.loads(PROTOS.read_text(encoding="utf-8-sig"))
    htext, ctext, ttext, n_ok, n_bad, n_shapes = emit(protos)

    if args.check:
        bad = False
        for path, text in ((HEADER, htext), (SOURCE, ctext), (SELFTEST, ttext)):
            on_disk = path.read_text(encoding="utf-8") if path.exists() else ""
            if on_disk != text:
                print(
                    f"DRIFT: {path} differs from a fresh regen -- run gen_dll_calls.py and commit."
                )
                bad = True
        if bad:
            return 1
        print(f"ok: mh_calls.gen.* match dll_call_protos.json ({n_ok} callable, {n_shapes} shapes)")
        return 0

    HEADER.parent.mkdir(parents=True, exist_ok=True)
    HEADER.write_text(htext, encoding="utf-8", newline="\n")
    SOURCE.write_text(ctext, encoding="utf-8", newline="\n")
    SELFTEST.write_text(ttext, encoding="utf-8", newline="\n")
    print(f"wrote {HEADER} ({n_ok} callable, {n_bad} unavailable)")
    print(f"wrote {SOURCE} ({n_shapes} shape thunks)")
    print(f"wrote {SELFTEST} (oracle for all {n_shapes} shapes; `net_selftest.exe callstest`)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
