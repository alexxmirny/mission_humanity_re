#!/usr/bin/env python3
"""
gen_dll_exports.py -- generate src/mh_dll/mh/addr/mh_export.gen.h from
tools/data/dll_call_protos.json: the INVERSE of gen_dll_calls.py.

gen_dll_calls.py lets our C++ CALL a game function. This lets a plain C++ function BE one: for
every function with a committed calling contract it emits a macro that defines a naked ENTRY thunk
which unmarshals the Watcom register/stack arguments into a cdecl call of your C++ body, then
returns with the callee stack cleanup that function's own callers already expect.

Shares the shape/slot logic with gen_dll_calls.py by IMPORTING it, so the two directions cannot
drift apart. Ghidra-FREE; `--check` is the drift gate in tools/lint_repo.py.

USING IT (in a seam .cpp):

    static int32_t my_body(uint32_t a, int32_t b) { ... }      // a plain C++ function

    MH_EXPORT_REPLACE(llm_some_game_fn, my_body)               // defines the entry thunk

    // at arm time:
    if (!mh_export_install_llm_some_gamefn()) { /* already logged; stay disarmed */ }

WHAT THE MACRO GIVES YOU
  * A TYPE-CHECKED binding. The macro stores your function in a pointer of the game function's exact
    generated signature, so a wrong parameter list is a COMPILE error rather than a stack that comes
    apart at runtime. The thunk then calls through that pointer, which also sidesteps C++ name
    mangling inside __asm.
  * The right cleanup. `ret N` when the function's callers expect callee cleanup (__watcall,
    __stdcall, ...) and a plain `ret` when they do not (__cdecl, __custumocall) -- taken from the
    committed convention, never guessed. Unlike the CALL direction, this one cannot be finessed: the
    thunk IS the callee, and its callers are already-compiled game code.
  * The arm guard. The installer checks the Watcom prologue before writing, and a refusal is logged
    through hook/export.h rather than silently leaving the original body in place.

By-value struct parameters are passed to your C++ body as a `const void *` pointing at the caller's
own copy on the stack -- the same shape gen_dll_calls.py uses in the other direction.

Usage:
  python tools/gen_dll_exports.py          # regenerate the header in place
  python tools/gen_dll_exports.py --check  # fail if the header differs from a fresh regen
"""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_dll_calls import PROTOS, REASON, param_names, shape_of, slot_values  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
HEADER = REPO / "src" / "mh_dll" / "mh" / "addr" / "mh_export.gen.h"
SELFTEST = REPO / "src" / "mh_dll" / "mh_nettest" / "mh_export_selftest.gen.cpp"

# return class -> the C type the generated signature uses when the game function returns a value.
# The C++ body leaves its result exactly where the game expects it (EAX for integers, ST0 for
# doubles are BOTH what MSVC's own cdecl return does), so no return marshalling is needed.
RET_OK = ("void", "u32", "u16", "u8", "f64")


def callee_ptype(p):
    """The C type a CALLEE-side body sees for one parameter.

    A by-value blob is the callee's OWN copy -- the caller pushed it and discards it on return -- so
    an exported body may legitimately mutate it, and several originals do (masking fields in place
    before storing). `const` is right for the CALL direction, where the thunk copies the caller's
    buffer, and wrong here. Shared with gen_dll_shadow.py so the export signature and the shadow
    dispatch cannot disagree about it (they did, once, and it was a compile error rather than
    anything worse only because the binding is type-checked).
    """
    c = p["ctype"]
    return c[len("const ") :] if p["slot"]["kind"] == "blob" and c.startswith("const ") else c


def export_shape(fn):
    """Shape id for the ENTRY direction. Unlike the call direction, cleanup IS part of the key:
    the thunk is the callee, so `ret N` vs `ret` is baked into it and cannot be made discipline-
    agnostic the way the call-side epilogue is."""
    sid, slots = shape_of(fn)
    return "e_%s__%s" % (sid[2:], fn["cleanup"]), slots


def emit_thunk_macro(shape_id, slots, cleanup):
    """The naked entry thunk, as a line-continued macro so it can be instantiated per replacement."""
    # incoming: [esp]=return address, game stack arg at declared offset O. After `push ebp;
    # mov ebp,esp`: [ebp]=saved ebp, [ebp+4]=return address, so arg O sits at [ebp + 4 + O].
    has_blob = any(s["kind"] == "blob" for s in slots)
    cdecl_bytes = sum(8 if (s["kind"] == "stack" and s["size"] == 8) else 4 for s in slots)
    game_stack = max((s["off"] + s["size"] for s in slots if s["kind"] != "reg"), default=4) - 4

    a = ["push ebp", "mov  ebp, esp"]
    # ECX AND EDX MUST BE PRESERVED ACROSS THE CALL INTO OUR C++.
    #
    # A Watcom callee preserves every register that is neither one of its arguments nor its return:
    # llm_strat_ai_scan_target_list_for_engage_candidates opens `push ebx / push ecx / push esi`, and
    # llm_strat_ai_engage_partition_turret_candidates (no args at all) pushes ebx, ecx, edx, esi, edi
    # and ebp. Callers rely on it -- llm_strat_unit_passive_engage_tick keeps the PLAYER INDEX in ECX
    # across four consecutive calls and then writes players[ECX].ai_target_list_count.
    #
    # MSVC's __cdecl, which is what the IMPL dispatcher is, preserves EBX/ESI/EDI/EBP but treats ECX
    # and EDX as VOLATILE. So without these two saves our replacement silently destroys a live value
    # in the game's caller. Measured 2026-08-01: arming the AI batch-A shadow sites crashed the game
    # at 0x004ee741 (`MOV [ECX + EAX + 0xe930e8], 0`) with a wild ECX -- a fault the rig reports as a
    # STALL, not as a crash, because the render loop keeps running.
    #
    # Restoring EDX is safe even where the original clobbers it (it is an argument register there, so
    # no caller may rely on it) and REQUIRED where the original preserves it. Neither register is ever
    # a return register in the emitted shapes -- returns are EAX, AL, or ST0.
    a += ["push ecx", "push edx"]
    if has_blob:
        # ESI is the scratch for taking the address of a by-value blob. It must NOT be one of the
        # argument registers -- and it never is -- and it is callee-saved for the game, hence the
        # save/restore. EAX would be the obvious scratch but it is usually the FIRST argument, which
        # is pushed LAST, so a blob (a later argument, pushed earlier) would destroy it first.
        a.append("push esi")
    # cdecl: push arguments in REVERSE declaration order
    for s in reversed(slots):
        if s["kind"] == "reg":
            a.append("push %s" % s["reg"].lower())
        elif s["kind"] == "blob":
            a.append("lea  esi, [ebp + %d]" % (4 + s["off"]))
            a.append("push esi")
        elif s["size"] == 8:
            a.append("push dword ptr [ebp + %d]" % (4 + s["off"] + 4))
            a.append("push dword ptr [ebp + %d]" % (4 + s["off"]))
        else:
            a.append("push dword ptr [ebp + %d]" % (4 + s["off"]))
    a.append("call dword ptr [IMPL]")
    if cdecl_bytes:
        a.append("add  esp, %d" % cdecl_bytes)
    if has_blob:
        a.append("pop  esi")
    a += ["pop  edx", "pop  ecx"]
    a.append("pop  ebp")
    a.append("ret %d" % game_stack if (cleanup == "callee" and game_stack) else "ret")

    lines = ["#define MH_EXPORT_THUNK_%s(THUNK, IMPL) \\" % shape_id]
    lines.append("    __declspec(naked) void THUNK() { \\")
    lines.append("        __asm { \\")
    for ln in a:
        lines.append("            __asm %s \\" % ln)
    lines.append("        } \\")
    lines.append("    }")
    return "\n".join(lines)


def slot_ctype(slot):
    if slot["kind"] == "blob":
        return "const void *"
    if slot["kind"] == "stack" and slot["size"] == 8:
        return "uint64_t"
    return "uint32_t"


def emit_selftest(shapes):
    """Exhaustive oracle for the entry thunks.

    A Watcom-style CALLER is generated per shape: it lays the arguments out exactly as compiled game
    code would (registers set, stack frame written at the declared offsets), calls the entry thunk,
    and records the ESP delta across the call.

    That last part is the whole point of hand-rolling the caller. Driving an entry thunk with the
    verified CALL-side thunk from mh_calls.gen.cpp would be easier -- but that thunk's epilogue
    restores ESP from EBP, which would absorb and hide a wrong `ret N`. Cleanup discipline is
    precisely what an entry thunk can get wrong and what a real game caller would NOT forgive, so the
    test caller deliberately does not self-heal: it asserts the exact ESP a caller of that
    convention expects.
    """
    o = [
        "// GENERATED by tools/gen_dll_exports.py -- DO NOT EDIT.",
        "//",
        "// Correctness oracle for the ENTRY thunks in addr/mh_export.gen.h (P0-EXPORT). For every",
        "// entry-thunk shape: a C++ body that records what it was handed, the generated thunk bound",
        "// to it, and a synthetic Watcom-style CALLER that hands it arguments the way compiled game",
        "// code would and then checks the stack discipline the callee is supposed to honour.",
        "//",
        "// The ESP assertion is the reason this caller is hand-rolled rather than reusing the",
        "// verified call-side thunk: that one restores ESP from EBP and would silently absorb a",
        "// wrong `ret N`. Here a callee-cleanup shape MUST have popped its stack arguments and a",
        "// caller-cleanup shape MUST NOT have.",
        '#include "addr/mh_export.gen.h"',
        "",
        "#include <cstdint>",
        "#include <cstdio>",
        "#include <cstring>",
        "",
        "static uint32_t g_seen[64];",
        "static uint32_t g_esp_pre, g_esp_post, g_ret_u32;",
        "static uint32_t g_ecx_post, g_edx_post;  // register-preservation probes",
        "static double   g_ret_f64;",
        "static int      g_checks, g_fails;",
        "",
        "static void echk(const char *shape, const char *what, bool ok) {",
        "    ++g_checks;",
        "    if (!ok) {",
        "        ++g_fails;",
        '        printf("  FAIL %-58s %s\\n", shape, what);',
        "    }",
        "}",
        "",
    ]

    tests = []
    for sid in sorted(shapes):
        slots, cleanup = shapes[sid]
        ret_cls = sid.split("__")[0][2:].split("_")[0]
        vals = slot_values(slots)
        rett = {
            "void": "void",
            "u32": "uint32_t",
            "u16": "uint32_t",
            "u8": "uint32_t",
            "f64": "double",
        }[ret_cls]

        # ---- the C++ body the game will end up calling ----
        params = ", ".join("%s a%d" % (slot_ctype(s), i) for i, s in enumerate(slots))
        b = ["static %s __cdecl exp_body_%s(%s) {" % (rett, sid, params or "void")]
        k = 0
        rec = []
        for i, s in enumerate(slots):
            if s["kind"] == "blob":
                n = s["size"] // 4
                b.append("    { uint32_t sum = 0; const uint32_t *p = (const uint32_t *)a%d;" % i)
                b.append("      for (int j = 0; j < %d; ++j) sum += p[j];" % n)
                b.append("      g_seen[%d] = sum; }" % k)
                rec.append((i, k, "blob"))
                k += 1
            elif s["kind"] == "stack" and s["size"] == 8:
                b.append("    g_seen[%d] = (uint32_t)a%d;" % (k, i))
                b.append("    g_seen[%d] = (uint32_t)(a%d >> 32);" % (k + 1, i))
                rec.append((i, k, "u64"))
                k += 2
            else:
                b.append("    g_seen[%d] = a%d;" % (k, i))
                rec.append((i, k, "plain"))
                k += 1
        if ret_cls == "f64":
            b.append("    return 98765.4321;")
        elif ret_cls != "void":
            b.append("    return 0xFEEDFACEu;")
        b.append("}")
        o.append("\n".join(b))
        o.append(
            "static %s(__cdecl *exp_impl_%s)(%s) = exp_body_%s;"
            % (rett, sid, ", ".join(slot_ctype(s) for s in slots) or "void", sid)
        )
        o.append("MH_EXPORT_THUNK_%s(exp_thunk_%s, exp_impl_%s)" % (sid, sid, sid))
        o.append("")

        # ---- a synthetic Watcom-style caller ----
        game_stack = max((s["off"] + s["size"] for s in slots if s["kind"] != "reg"), default=4) - 4
        blobs = [(i, s) for i, s in enumerate(slots) if s["kind"] == "blob"]
        c = ["// clang-format off"]
        for i, s in blobs:
            c.append(
                "static const uint32_t exp_blob_%s_%d[] = {%s};"
                % (sid, i, ", ".join("0x%08xu" % v for v in vals[i]))
            )
        c.append("__declspec(naked) void exp_caller_%s() {" % sid)
        c.append("    __asm {")
        for ln in ["push ebp", "mov  ebp, esp", "push ebx", "push esi", "push edi"]:
            c.append("        " + ln)
        if game_stack:
            c.append("        sub  esp, %d" % game_stack)
        for i, s in enumerate(slots):
            if s["kind"] == "blob":
                c.append("        cld")
                c.append("        lea  edi, [esp + %d]" % (s["off"] - 4))
                c.append("        mov  esi, offset exp_blob_%s_%d" % (sid, i))
                c.append("        mov  ecx, %d" % (s["size"] // 4))
                c.append("        rep  movsd")
            elif s["kind"] == "stack" and s["size"] == 8:
                c.append(
                    "        mov  dword ptr [esp + %d], 0%08xh"
                    % (s["off"] - 4, vals[i] & 0xFFFFFFFF)
                )
                c.append(
                    "        mov  dword ptr [esp + %d], 0%08xh"
                    % (s["off"], (vals[i] >> 32) & 0xFFFFFFFF)
                )
            elif s["kind"] == "stack":
                c.append("        mov  dword ptr [esp + %d], 0%08xh" % (s["off"] - 4, vals[i]))
        for i, s in enumerate(slots):
            if s["kind"] == "reg":
                c.append("        mov  %s, 0%08xh" % (s["reg"].lower(), vals[i]))
        # REGISTER PRESERVATION. A Watcom callee preserves every register that is not one of its
        # arguments, and real callers depend on it across the call. Our replacement reaches MSVC
        # __cdecl code, which treats ECX and EDX as volatile -- so the thunk has to save them, and
        # this is the check that says whether it did. Only meaningful for a register the shape does
        # NOT pass an argument in; where ECX/EDX carry an argument the original may clobber them.
        argregs = {s["reg"].lower() for s in slots if s["kind"] == "reg"}
        probes = [(r, v) for r, v in (("ecx", 0xC1C1C1C1), ("edx", 0xD2D2D2D2)) if r not in argregs]
        for r, v in probes:
            c.append("        mov  %s, 0%08xh" % (r, v))
        c.append("        mov  dword ptr [g_esp_pre], esp")
        c.append("        call exp_thunk_%s" % sid)
        c.append("        mov  dword ptr [g_esp_post], esp")
        for r, _v in probes:
            c.append("        mov  dword ptr [g_%s_post], %s" % (r, r))
        if ret_cls == "f64":
            c.append("        fstp qword ptr [g_ret_f64]")
        elif ret_cls != "void":
            c.append("        mov  dword ptr [g_ret_u32], eax")
        for ln in ["lea  esp, [ebp - 12]", "pop  edi", "pop  esi", "pop  ebx", "pop  ebp", "ret"]:
            c.append("        " + ln)
        c.append("    }")
        c.append("}")
        c.append("// clang-format on")
        o.append("\n".join(c))
        o.append("")
        tests.append((sid, ret_cls, slots, vals, rec, cleanup, game_stack, probes))

    for sid, ret_cls, slots, vals, rec, cleanup, game_stack, probes in tests:
        label = "%s [%s-pops]" % (sid, cleanup)
        t = ["static void exp_test_%s() {" % sid]
        t.append("    std::memset(g_seen, 0, sizeof g_seen);")
        t.append("    g_ret_u32 = 0; g_ret_f64 = 0;")
        t.append("    g_ecx_post = 0; g_edx_post = 0;")
        t.append("    exp_caller_%s();" % sid)
        for r, v in probes:
            t.append(
                '    echk("%s", "%s preserved across the call", g_%s_post == 0x%08xu);'
                % (label, r.upper(), r, v)
            )
        expect = game_stack if cleanup == "callee" else 0
        t.append(
            '    echk("%s", "callee %s pop its %d stack byte(s)", g_esp_post - g_esp_pre == %du);'
            % (label, "must" if cleanup == "callee" else "must NOT", game_stack, expect)
        )
        for i, k, kind in rec:
            if kind == "blob":
                t.append(
                    '    echk("%s", "arg%d blob bytes", g_seen[%d] == 0x%08xu);'
                    % (label, i, k, sum(vals[i]) & 0xFFFFFFFF)
                )
            elif kind == "u64":
                t.append(
                    '    echk("%s", "arg%d lo", g_seen[%d] == 0x%08xu);'
                    % (label, i, k, vals[i] & 0xFFFFFFFF)
                )
                t.append(
                    '    echk("%s", "arg%d hi", g_seen[%d] == 0x%08xu);'
                    % (label, i, k + 1, (vals[i] >> 32) & 0xFFFFFFFF)
                )
            else:
                where = (
                    slots[i]["reg"] if slots[i]["kind"] == "reg" else "stack+%d" % slots[i]["off"]
                )
                t.append(
                    '    echk("%s", "arg%d from %s", g_seen[%d] == 0x%08xu);'
                    % (label, i, where, k, vals[i])
                )
        if ret_cls == "f64":
            t.append('    echk("%s", "ST0 return", g_ret_f64 == 98765.4321);' % label)
        elif ret_cls != "void":
            t.append('    echk("%s", "return value", g_ret_u32 == 0xFEEDFACEu);' % label)
        t.append("}")
        o.append("\n".join(t))
        o.append("")

    o.append("int run_exportstest() {")
    o.append(
        '    printf("=== exportstest (%d generated entry thunks, real Watcom-style callers) ===\\n");'
        % len(shapes)
    )
    for sid, *_ in tests:
        o.append("    exp_test_%s();" % sid)
    o.append('    printf("%d checks, %d failures\\n", g_checks, g_fails);')
    o.append("    return g_fails == 0 ? 0 : 1;")
    o.append("}")
    o.append("")
    return "\n".join(o)


def emit(protos):
    fns = protos["functions"]
    # Installing a detour REWRITES 8 BYTES at the entry, so a function shorter than that cannot host
    # one -- the write would run past its end and corrupt whatever function follows. (The call
    # direction has no such limit, which is why this filter lives here and not in the dumper.)
    # A VARARGS SHAPE IS CALL-DIRECTION ONLY. the Ghidra-side call-proto dump synthesizes one `ok` row per
    # measured fixed-arity shape of a variadic callee so the DLL can CALL it; those rows all carry
    # the base function's `va`. Detouring is per-FUNCTION, not per-shape -- emitting one here would
    # produce four `addr_w_sprintf__*` detour targets for a single address, and a fixed-arity body
    # cannot stand in for a variadic one anyway. Excluded by construction rather than by remembering.
    ok = [
        f
        for f in fns
        if f["status"] == "ok"
        and not f.get("from_varargs_shape")
        and f["ret"].get("storage", "void") in RET_OK
        and f.get("size", 0) >= 8
        and f.get("entry8")
    ]

    shapes = {}
    for f in ok:
        sid, slots = export_shape(f)
        shapes.setdefault(sid, (slots, f["cleanup"]))

    o = [
        "// GENERATED by tools/gen_dll_exports.py from tools/data/dll_call_protos.json -- DO NOT EDIT.",
        "// Regenerate after re-dumping the JSON (mh_dump_call_protos.py against /eng/mh.exe);",
        "// `gen_dll_exports.py --check` is the drift gate.",
        "//",
        "// Install a plain C++ function AS a game function. The inverse of addr/mh_calls.gen.h.",
        "//",
        "//     static int32_t my_body(uint32_t a, int32_t b) { ... }",
        "//     MH_EXPORT_REPLACE(llm_some_game_fn, my_body)",
        "//     ... at arm time:  if (!mh_export_install_llm_some_game_fn()) { /* logged; stay off */ }",
        "//",
        "// The macro binds your function through a pointer of the game function's exact signature, so",
        "// a wrong parameter list is a COMPILE error, not a stack that comes apart at runtime. The",
        "// thunk applies the cleanup that function's already-compiled callers expect (`ret N` for",
        "// callee-cleanup conventions, plain `ret` otherwise) -- unlike the call direction, this",
        "// cannot be made discipline-agnostic, because here WE are the callee.",
        "//",
        "// By-value struct parameters arrive as `const void *` aimed at the caller's own stack copy.",
        "//",
        "// ---- WHERE THE INSTALL GOES (fork F4D-PRE) -----------------------------------------------",
        "//",
        "// The installer calls ::mh::hosthook::install_export_ok -- the HOST's hook table",
        "// (libmh/state/hook_api.h), not mh::hook:: directly. 22 of the 627 libmh-roster TUs expand this",
        "// macro, and until F4D-PRE that made install_export_ok libmh's single largest outbound edge --",
        "// one no source scan could see, because not one of those 22 TUs NAMES it. Routing it through the",
        "// table is what lets tools/check_libmh_outbound.py assert the edge is closed.",
        "//",
        "// ---- THE STANDALONE ARM (LIB-REF-SPLIT) --------------------------------------------------",
        "//",
        "// A promote seam PATCHES an original entry, so it needs that entry's address by definition.",
        "// The standalone artifact has no original to patch, and under MH_LIBMH_BUILD every seam",
        "// degrades to an installer that refuses -- measured 2026-09-11, this macro accounted for 481",
        "// distinct original VAs / 695 immediates in libmh.lib, ai_promote.obj alone for 166.",
        "//",
        "// THIS IS ALSO WHERE THE ASM LEAVES BY CONFIGURATION (the lib-ref plan's sec 0.3). The 108",
        "// MH_EXPORT_THUNK_* shapes are __declspec(naked) __asm blocks whose only purpose is to receive",
        "// a call made by already-compiled ORIGINAL code in the game's register discipline. Nothing in a",
        "// standalone host calls them, so they are not compiled rather than being justified.",
        "//",
        "// WHAT STAYS IN BOTH ARMS: addr_/entry_/sig_ per function. They are declaration-only --",
        "// `inline constexpr` that nothing odr-uses emits no bytes -- and state/rebind_verify.gen.cpp's",
        "// one static_assert per rebind row compares a target's real type against sig_<fn>, which is the",
        "// check that lets R2 derive most binder targets without a human reading each row. Removing them",
        "// standalone would disarm that in exactly the configuration the binder is for.",
        "#pragma once",
        "#include <cstdint>",
        "",
        '#include "addr/mh_structs.gen.h"',
        "",
        "#ifndef MH_LIBMH_BUILD",
        '#include "state/hook_api.h"',
        "",
        "// ---- entry-thunk shapes (one per storage shape x cleanup discipline) ----",
        "// clang-format off",
    ]
    for sid in sorted(shapes):
        slots, cleanup = shapes[sid]
        o.append(emit_thunk_macro(sid, slots, cleanup))
        o.append("")
    o.append("// clang-format on")
    o.append("#endif // !MH_LIBMH_BUILD")
    o.append("")
    o.append("namespace mh::exp {")
    o.append("")
    o.append(
        "// ---- per-function: the address to patch, and the signature your C++ body must have ----"
    )
    for f in sorted(ok, key=lambda x: x["va"]):
        names = param_names(f)
        params = ", ".join(
            "%s%s%s" % (callee_ptype(p), "" if callee_ptype(p).endswith("*") else " ", n)
            for p, n in zip(f["params"], names)
        )
        o.append("inline constexpr uintptr_t addr_%s = %su;" % (f["cident"], f["va"]))
        o.append(
            "inline constexpr uint64_t  entry_%s = %sull;  // arm guard: the 8 bytes we generated against"
            % (f["cident"], f["entry8"])
        )
        o.append(
            "using sig_%s = %s(__cdecl *)(%s);  // %s %s"
            % (f["cident"], f["ret"]["ctype"], params or "void", f["cc"], f["cleanup"])
        )
    o.append("")
    o.append("} // namespace mh::exp")
    o.append("")
    o.append("// ---- MH_EXPORT_REPLACE(<game function>, <your C++ function>) ----")
    o.append(
        "// Expands at file scope in your TU: a type-checked binding, the naked entry thunk, and"
    )
    o.append("// a `mh_export_install_<fn>()` you call at arm time (false = refused AND logged).")
    o.append("#define MH_EXPORT_REPLACE(FN, IMPL) MH_EXPORT_REPLACE_##FN(IMPL)")
    o.append("")
    o.append("#ifndef MH_LIBMH_BUILD")
    for f in sorted(ok, key=lambda x: x["va"]):
        sid, _slots = export_shape(f)
        c = f["cident"]
        o.append("#define MH_EXPORT_REPLACE_%s(IMPL) \\" % c)
        o.append("    static ::mh::exp::sig_%s mh_export_impl_%s = IMPL; \\" % (c, c))
        o.append("    MH_EXPORT_THUNK_%s(mh_export_thunk_%s, mh_export_impl_%s) \\" % (sid, c, c))
        o.append("    static bool mh_export_install_%s() { \\" % c)
        o.append(
            "        return ::mh::hosthook::install_export_ok(::mh::exp::addr_%s, (void *)mh_export_thunk_%s, \"%s\", ::mh::exp::entry_%s); \\"
            % (c, c, c, c)
        )
        o.append("    }")
    o.append("")
    o.append("#else // MH_LIBMH_BUILD -- the standalone arm; see the header comment")
    o.append("")
    for f in sorted(ok, key=lambda x: x["va"]):
        c = f["cident"]
        o.append("#define MH_EXPORT_REPLACE_%s(IMPL) \\" % c)
        o.append("    static ::mh::exp::sig_%s mh_export_impl_%s = IMPL; \\" % (c, c))
        o.append("    static bool mh_export_install_%s() { \\" % c)
        o.append("        (void)mh_export_impl_%s; return false; \\" % c)
        o.append("    }")
    o.append("")
    o.append("#endif // MH_LIBMH_BUILD")
    o.append("")
    o.append(
        "// ---- NOT exportable: same reasons as mh_calls.gen.h, plus a non-EAX/ST0 return ----"
    )
    exportable = {f["cident"] for f in ok}
    for f in sorted(fns, key=lambda x: x["va"]):
        if f["cident"] in exportable:
            continue
        why = REASON.get(f["status"], "MH_UNAVAILABLE__parameter_storage_not_marshallable")
        if f["status"] == "ok":
            why = (
                "MH_UNAVAILABLE__function_too_small_to_host_a_detour"
                if f.get("size", 0) < 8
                else "MH_UNAVAILABLE__non_standard_return_register"
            )
        o.append(
            '#define MH_EXPORT_REPLACE_%s(IMPL) static_assert(false, "%s: %s");'
            % (f["cident"], f["cident"], why)
        )
    o.append("")
    return "\n".join(o), emit_selftest(shapes), len(ok), len(fns) - len(ok), len(shapes)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="drift gate: fail if header differs")
    args = ap.parse_args()

    protos = json.loads(PROTOS.read_text(encoding="utf-8-sig"))
    text, ttext, n_ok, n_bad, n_shapes = emit(protos)

    if args.check:
        bad = False
        for path, want in ((HEADER, text), (SELFTEST, ttext)):
            on_disk = path.read_text(encoding="utf-8") if path.exists() else ""
            if on_disk != want:
                print(
                    f"DRIFT: {path} differs from a fresh regen -- run gen_dll_exports.py and commit."
                )
                bad = True
        if bad:
            return 1
        print(
            f"ok: mh_export.gen.* match dll_call_protos.json ({n_ok} exportable, {n_shapes} shapes)"
        )
        return 0

    HEADER.parent.mkdir(parents=True, exist_ok=True)
    HEADER.write_text(text, encoding="utf-8", newline="\n")
    SELFTEST.write_text(ttext, encoding="utf-8", newline="\n")
    print(f"wrote {HEADER} ({n_ok} exportable, {n_bad} refused, {n_shapes} entry-thunk shapes)")
    print(f"wrote {SELFTEST} (oracle for all {n_shapes} shapes; `net_selftest.exe exportstest`)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
