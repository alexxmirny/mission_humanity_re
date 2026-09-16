//
// crt/crt_math.h -- the vendored floating-point CRT (LIB-CRT, the VENDOR-EQUIV class).
//
// WHAT THIS REPLACES. Three callees, 14 sites, all of them x87 leaves in the game's statically
// linked Watcom CRT:
//
//     llm_sqrt        @0x004da9c0  10 sites  a signed-zero-aware guard around FSQRT
//     llm_math_atan   @0x004daa52   3 sites  a tail call into IF@ATAN (FLD1 / FPATAN)
//     floor           @0x004daadb   1 site   modf, then -1 when the fraction is negative
//
// ---- WHY THESE ARE ASSEMBLY AND NOT <cmath> -----------------------------------------------------
//
// Because CRT-X87 already ran this experiment and it is recorded in mh/fp/x87.h: an x87 sequence
// replaced by its "obviously equivalent" C++ is a claim, not a fact, and one of the two helpers that
// experiment tested REFUSED the replacement after an offline probe said it would clear
// (an offline probe can agree where the real header does not). The cheap, decidable move for a body that is literally one x87
// instruction is to emit that instruction. `fsqrt` and `fpatan` are then bit-equal to the original by
// construction rather than by a sweep -- and the sweep in `crttest` still runs, because "by
// construction" is what everybody says right before a compiler reassociates something.
//
// The concrete hazard `std::sqrt` would introduce is not hypothetical: MSVC compiles it to SSE2
// `sqrtsd`, and the surrounding sim arithmetic is x87. Under the harness's pinned PC=53
// (harness.cpp `pin_fpu`) the two agree for every input; at the bare x87 default PC=64 they need not,
// and both settings are reachable -- exactly the trap that made `trunc_i32_mul` stay assembly.
//
// PRECISION AT THE BOUNDARY, and why this file does not make it worse. The original returns in ST(0)
// with `RET 8`, so a caller can consume 80 bits without a store -- and llm_strat_slot_dist_to_ref
// (0x004cbdf7) does exactly that, feeding ST(0) straight into utils_math_trunc. Our HOSTED arm
// already loses that: `mh::call::llm_sqrt` is declared `double(double)` and the thunk stores ST(0)
// to a qword. This file keeps the same signature, so the standalone arm rounds where the hosted arm
// already rounds and the two agree. That rounding was accepted when the call sites were translated
// (see sim_path_slot_dist.cpp's header note); it is NOT introduced here.
//
// The offline oracle is `net_selftest crttest` (mh_nettest/crt_vendor_selftest.cpp).
//
#pragma once

#include <cstdint>
#include <cstring>

namespace mh::crt {

namespace detail {

// @0x004da9c0-0x004da9d4: the guard is on the RAW BITS, not on `x < 0.0`, and the difference is
// -0.0. `TEST byte ptr [ESP+0xb],0x80` reads the sign bit; the two dwords are then OR'd against
// 0x7fffffff to ask "is the magnitude zero". Negative zero therefore takes the FSQRT path and yields
// -0.0, which is what IEEE-754 requires and what `x < 0.0` would ALSO get right -- but `x <= 0.0`,
// the spelling a hurried translation reaches for, would not. Pinned as its own predicate so
// `crttest` can sweep it against the assembly independently of the arithmetic.
inline bool sqrt_is_domain_error(double x) {
    uint64_t bits;
    std::memcpy(&bits, &x, sizeof(bits));
    const bool negative     = (bits & 0x8000000000000000ull) != 0ull;
    const bool magnitude_nz = (bits & 0x7fffffffffffffffull) != 0ull;
    return negative && magnitude_nz;
}

// The value the original's domain-error path produces, and the derivation, because nothing about it
// is guessable from llm_sqrt alone:
//
//   llm_sqrt        pushes 3 and calls __math87_err @0x004e91d0
//   __math87_err    code 3 is in [1,3], so @0x004e91ed builds EAX = 3 | 0x40 in AL, then OR AH,0x20
//                   -> EAX = 0x2043, EDX = &x, and calls __math1err -> __math2err @0x004f2517
//   __math2err      CL & 0x40 -> exc.type = 1 (DOMAIN); EAX & 0x1f = 3 selects the function-name
//                   string; CH & 0x20 is SET, so @0x004f25a1 zeroes both dwords of exc.retval
//                   -- the return value is preset to 0.0 before the handler ever runs
//   _matherr        @0x004f3910. The user-override hook @0x0066f67c is the hardcoded
//                   `XOR EAX,EAX / RET` stub (0x004f453c) -- the game links no matherr -- so the
//                   default handler runs unconditionally and returns exc.retval, which is still the
//                   0.0 __math2err preset. Verified 2026-09-08 by walking the whole callee set.
//
// So a negative argument yields exactly +0.0 (not -0.0, not a NaN). This is a DEAD PATH from libmh --
// all ten call sites pass a sum of squares -- but "unreachable" is a claim about integer overflow in
// the callers, not a property of this function, so it is implemented rather than trapped.
//
// TWO SIDE EFFECTS ARE DELIBERATELY NOT REPRODUCED, and both are the host's business, not libmh's:
// the default handler prints "Domain error in sqrt\n" to stderr (__rterrmsg @0x004f38c8) and sets
// errno to 0xd/EDOM (CRT_004dfade). libmh owns no errno and must not write to a host's stderr; the
// VALUE is the whole of the observable contract, and it is what this constant is.
inline constexpr double SQRT_DOMAIN_ERROR_RESULT = 0.0;

} // namespace detail

// ---- llm_sqrt @0x004da9c0 ------------------------------------------------------------------------
inline double llm_sqrt(double x) {
    if (detail::sqrt_is_domain_error(x)) return detail::SQRT_DOMAIN_ERROR_RESULT;
    double r = 0.0;
    // clang-format off
    __asm {
        fld     qword ptr [x]       ; 0x004da9e7  FLD  double ptr [ESP+4]
        fsqrt                       ; 0x004da9eb  FSQRT
        fstp    qword ptr [r]       ; the RET 8 boundary: the thunk stores ST(0) to a qword too
    }
    // clang-format on
    return r;
}

// ---- llm_math_atan @0x004daa52 -------------------------------------------------------------------
//
// The original is a two-instruction tail call into IF@ATAN @0x004daa28, which is
// `FLD1 / FPATAN` -- FPATAN computes atan(ST(1)/ST(0)), and with ST(0) = 1.0 that is atan(x).
// IF@ATAN guards on bit 0 of the byte @0x0066f45c and diverts to __fpatan_wrap when it is set; that
// byte reads 0 in the image (checked 2026-09-08), which is the "real x87 present" case, so the
// hardware instruction is the live path and the emulator arm is unreachable on any machine that can
// run a 2001 Win32 game at all.
inline double llm_math_atan(double x) {
    double r = 0.0;
    // clang-format off
    __asm {
        fld     qword ptr [x]       ; 0x004daa52  FLD double ptr [ESP+4]
        fld1                        ; 0x004daa28  FLD1
        fpatan                      ; 0x004daa33  FPATAN   -> atan(ST(1)/ST(0)) = atan(x)
        fstp    qword ptr [r]
    }
    // clang-format on
    return r;
}

// ---- floor @0x004daadb ---------------------------------------------------------------------------
//
// NOT `std::floor`, for the reason in the banner, and the structure is worth keeping visible anyway:
// the original does not have a floor instruction, it has trunc-plus-a-correction.
//
//   modf @0x004e9280   -> utils_math_trunc @0x004d0596 (the FSTCW / mov ah,0x1f / FLDCW / FRNDINT /
//                         FLDCW control-word dance -- RC=11 truncate, PC=11 extended, restore BEFORE
//                         the store, exactly the sequence fp/x87.h's banner documents), then
//                         frac = x - trunc(x)
//   @0x004daaf5-0x004daafc  FLDZ / FCOMPP / FNSTSW / SAHF / JBE -- SKIP the correction when the
//                         branch is taken. `JBE` is `CF | ZF`, and after FNSTSW/SAHF that is
//                         `C0 | C3`: taken when 0.0 < frac (C0), when 0.0 == frac (C3), AND WHEN THE
//                         COMPARE IS UNORDERED, since a NaN sets C3,C2,C0 = 1,1,1. So the correction
//                         is applied on exactly the ORDERED-LESS case, and the faithful C++ is
//                         `frac < 0.0`.
//
//                         THIS LINE SAID THE OPPOSITE UNTIL crttest CAUGHT IT (2026-09-08). It
//                         reasoned from fp/x87.h's banner -- "the game tests C0, so unordered reads
//                         as less-than and the correct spelling is `!(x >= y)`" -- and applied that
//                         rule mechanically. The rule does not transfer here for TWO independent
//                         reasons, and either one alone flips the answer: this compare tests
//                         `C0 | C3`, not C0 alone, and the branch SKIPS the work rather than doing
//                         it. `!(0.0 <= frac)` is true on a NaN, so it APPLIED a correction the
//                         assembly does not. Unobservable by value -- NaN and both infinities absorb
//                         `+ (-1.0)` bit for bit, which is why the sweep was green either way and
//                         why crttest pins the VALUE at those inputs rather than the predicate --
//                         but a predicate that disagrees with the assembly is a live bug waiting for
//                         a caller that can tell. Recorded because the general lesson is that an
//                         x87-compare rule is about ONE flag test and ONE branch polarity; re-derive
//                         it, do not recall it.
//   @0x004dab01        FADD of DAT_00506d54, read out of the image as exactly -1.0.
inline double floor(double x) {
    double   ipart    = 0.0;
    double   fpart    = 0.0;
    uint16_t cw_save  = 0;
    uint16_t cw_trunc = 0;
    // The original juggles ESP to hold the two control words (@0x004d0596 `PUSH EAX` / `PUSH dword
    // ptr [ESP]` / `LEA ESP,[ESP+8]`). Locals are used here instead -- MSVC may address `x`, `ipart`
    // and `fpart` off ESP, so moving ESP inside the block is not safe even when it balances. The
    // INSTRUCTIONS that matter (the control-word values, FRNDINT, the restore-before-store ordering)
    // are unchanged, and this is the same spelling mh/fp/x87.h uses for the same dance.
    // clang-format off
    __asm {
        fld     qword ptr [x]
        fld     st(0)                       ; modf 0x004e9285: ST0 = x, ST1 = x
        ; utils_math_trunc 0x004d0596, inlined
        fstcw   cw_save
        mov     ax, cw_save
        mov     ah, 0x1f                    ; RC = 11 (truncate), PC = 11 (extended)
        mov     cw_trunc, ax
        fldcw   cw_trunc
        frndint                             ; 0x004d05a7  ST0 = trunc(x)
        fldcw   cw_save                     ; 0x004d05a9  restore BEFORE the store
        ; modf's tail 0x004e928c: ST(1) = x - trunc(x)
        fsub    st(1), st(0)
        fstp    qword ptr [ipart]           ; 0x004e9292  *out = trunc(x)
        fstp    qword ptr [fpart]           ; modf's return value, ST(0) at 0x004daaf5
    }
    // clang-format on

    // @0x004daaf5-0x004dab07. `fpart < 0.0`, NOT `!(0.0 <= fpart)`: the two differ on a NaN fraction,
    // and the assembly's JBE (CF|ZF, i.e. C0|C3) is TAKEN when unordered, so it skips. See the
    // banner -- this was wrong the other way round until crttest's reference arm refused to diverge.
    if (fpart < 0.0) ipart += -1.0;
    return ipart;
}

// ---- llm_math_cos_impl @0x004dabbc / llm_math_fsin_reduce_loop @0x004dabc6 (LIB-REF-SPLIT) ------
//
// The two ST0-ARGUMENT leaves. They are the last original VAs in the migrated modules that are
// reached as a CALL rather than as data, and they were invisible to the outward-call census for the
// whole of its existence: their call sites pass a literal VA to a naked thunk (mh/fp/st0_call.h), so
// there is no `mh::call::` token to find. That thunk exists only to call the ORIGINAL; standalone
// there is nothing at either address, so its two callers had to route somewhere and this is where.
//
// ---- WHAT THE ORIGINAL DOES, read off the disassembly rather than assumed ------------------------
//
//   004dabbc  FCOS                    004dabc6  FSIN
//   004dabbe  CALL 0x004dabd0         004dabc8  CALL 0x004dabd0
//   004dabc3  JNC  0x004dabbc         004dabcd  JNC  0x004dabc6
//   004dabc5  RET                     004dabcf  RET
//
//   004dabd0  PUSH EBP / MOV EBP,ESP / PUSH EAX
//   004dabd4  FSTSW [EBP-2] / MOV AH,[EBP-1] / OR AH,1 / SAHF / JNP 0x004dabf8
//   004dabe1  FLD tbyte [0x00669d70] / FXCH
//   004dabe9  FPREM / FSTSW [EBP-2] / MOV AH,[EBP-1] / SAHF / JP 0x004dabe9
//   004dabf5  FSTP ST1 / CLC
//   004dabf8  POP EAX / POP EBP / RET
//
// FSIN/FCOS set C2 when the argument is too large to reduce internally, leaving the operand on the
// stack untouched. The helper reads C2 (status bit 10, which SAHF puts in PF) and hands the answer
// back IN THE CARRY FLAG: `OR AH,1` forces CF=1 before SAHF, so the in-range path returns CF=1 and
// the caller's JNC falls through to RET; the out-of-range path reduces mod 2*pi with FPREM until C2
// clears, drops the divisor, and CLCs so the caller's JNC loops back and retries the transcendental.
//
// THE FLAG PLUMBING IS COLLAPSED HERE AND THE PREDICATE IS NOT. Inlined, there is no CALL whose
// return value needs a flag, so `OR AH,1 / SAHF / JNP` becomes `test ah,4 / jz` -- the same single
// bit (AH bit 2 IS status bit 10 IS C2) and the same branch polarity. Nothing else is collapsed.
//
// THE 2*PI CONSTANT IS EMBEDDED AS ITS TEN BYTES, and the first attempt did not -- which the hosted
// differential caught, so it is worth recording why the clever version is wrong. The tbyte at
// 0x00669d70 reads 35 c2 68 21 a2 da 0f c9 01 40: mantissa 0xc90fdaa22168c235, exponent 0x4001, i.e.
// exactly FLDPI's mantissa one binade up. So `fldpi; fadd st(0),st(0)` looks like it rebuilds the
// constant with nothing copied out of the image -- and it does, at PC=64.
//
// FLD IS NOT PRECISION-CONTROL-AFFECTED; FADD IS. Under the harness's pinned PC=53 (harness.cpp
// pin_fpu) that FADD rounds to a 53-bit mantissa and drops the low 11 bits the original's FLD keeps,
// so the FPREM divisor differs and every input large enough to actually reduce comes back different.
// Measured, not reasoned: the first sweep was 21 DISAGREEMENTS out of 96, and all 21 were band B
// (the C2/FPREM cases), with bands A, C and D bit-identical at 75/75. An in-range answer never
// touches the divisor -- which is exactly why the cheap half agreed, and why a sweep without the
// large magnitudes would have shipped this green.
//
// So the divisor is LOADED, as the original loads it. The ten bytes are a numeric constant read out
// of the image, not an address: nothing resolves through them and the VA scan does not see them.
//
// PRECISION-CONTROL NEUTRAL, recorded for CRT-X87-CPP (plan sec 3.9): these are the same
// instructions, so whatever the live control word says -- PC=53 under the harness's pin_fpu, PC=64
// at the bare x87 default -- both arms inherit it identically. This body makes no PC assumption and
// therefore cannot be invalidated by the PC=53 decision either way.
//
// ASM RATHER THAN C++, per the lib-ref plan sec 0.3's allowance, with the measured reason: there is
// no C++ spelling of "FSIN, and if C2 then reduce and retry". `std::sin` is a different algorithm
// (and on MSVC a different unit entirely), and the values reach hashed AI group state through
// ai_group_task_movement.cpp -- whose own comment says "never substitute std::sin/std::cos".
// TWO BODIES, NOT ONE TEMPLATE. A single parameterised version was written first and does not
// compile: MSVC's __asm labels are scoped to one __asm block, so the `retry:` the transcendental
// must jump back to cannot live in a different block from the FSIN/FCOS that an `if constexpr`
// selects. The duplication is nine instructions and mirrors the original, which also has two entries
// sharing one helper.

// sin, as llm_math_fsin_reduce_loop @0x004dabc6 computes it.
inline double llm_math_fsin_reduce_loop(double x) {
    // The image's own tbyte at 0x00669d70, byte for byte. See the constant note above for
    // why this is LOADED rather than rebuilt from FLDPI.
    static const uint8_t k2pi[10] = {0x35, 0xc2, 0x68, 0x21, 0xa2, 0xda, 0x0f, 0xc9, 0x01, 0x40};
    double               r        = 0.0;
    // clang-format off
    __asm {
        fld     qword ptr [x]
    retry:
        fsin                        ; 0x004dabc6
        fstsw   ax                  ; 0x004dabd4 (the helper, inlined)
        test    ah, 4               ; C2 = status bit 10 = AH bit 2
        jz      done                ; in range -> the original's CF=1 -> caller's JNC falls through
        fld     tbyte ptr [k2pi]    ; 0x004dabe1  FLD tbyte [0x00669d70] -- PC-independent
        fxch                        ; 0x004dabe7
    prem:
        fprem                       ; 0x004dabe9: PARTIAL remainder -- loops while C2 stays set
        fstsw   ax
        test    ah, 4
        jnz     prem                ; 0x004dabf3
        fstp    st(1)               ; 0x004dabf5: drop the divisor
        jmp     retry               ; 0x004dabf7 CLC -> caller's JNC taken -> retry
    done:
        fstp    qword ptr [r]
    }
    // clang-format on
    return r;
}

// cos, as llm_math_cos_impl @0x004dabbc computes it. Identical but for the one instruction.
inline double llm_math_cos_impl(double x) {
    // The image's own tbyte at 0x00669d70, byte for byte. See the constant note above for
    // why this is LOADED rather than rebuilt from FLDPI.
    static const uint8_t k2pi[10] = {0x35, 0xc2, 0x68, 0x21, 0xa2, 0xda, 0x0f, 0xc9, 0x01, 0x40};
    double               r        = 0.0;
    // clang-format off
    __asm {
        fld     qword ptr [x]
    retry:
        fcos                        ; 0x004dabbc
        fstsw   ax
        test    ah, 4
        jz      done
        fld     tbyte ptr [k2pi]    ; 0x004dabe1  FLD tbyte [0x00669d70] -- PC-independent
        fxch
    prem:
        fprem
        fstsw   ax
        test    ah, 4
        jnz     prem
        fstp    st(1)
        jmp     retry
    done:
        fstp    qword ptr [r]
    }
    // clang-format on
    return r;
}

} // namespace mh::crt
