//
// fp/x87_shapes.h -- GENERATED-ONCE, then owned by hand: every remaining inline x87 block, in one place.
//
// CRT-X87's goal, in the user's words, is "as less asm as possible in translated code". This file is
// how that is met WITHOUT betting on an equivalence proof: the assembly MOVES here and does not
// change, so the hoist is behaviour-preserving by construction. Whether any of these later becomes
// C++ is a separate, per-helper decision -- see x87.h's step-2 banner for the one that cleared, the
// one that was refused, and the two shapes measured NOT to be replaceable.
//
// SHAPE, NOT DOMAIN. Each helper is named for what it computes (`trunc_mul_div`), because five pairs
// of these turned out to be the same instruction text under different local names in different
// files. The merge rule is the first pass's and it is strict: identical text INCLUDING OPERAND
// SIZES, with only local names normalised -- and, added here, IDENTICAL SIGNATURES. A shared
// instruction sequence behind a different parameter list is a semantic claim, not a mechanical one;
// the single case of that (tact_calc_dir24's atan_rad_to_int_deg, which binds two of
// trunc_mul_div's three operands to its own constants) forwards explicitly at its own call site
// instead of being folded in here.
//
// The callers keep their reviewed local names and signatures and now just forward, so NO call site
// anywhere moved -- the same edit the first pass made for trunc_i32 / trunc_i32_mul.
//
#pragma once
#include <cmath> // std::floor / std::sqrt -- helpers that round once to a double, then call the CRT
#include <cstdint>

// CRT-X87-CPP step 3: the two PROVEN SINKS the converted helpers end in. `trunc_i32` is
// `fld`+the control-word dance+`fistp dword`; `trunc_i64_low32` is the same dance with
// `fistp qword` and a low-dword take. They are NOT interchangeable -- they differ out of int32
// range -- and each has its own fptest case pinning it to the assembly.
#include "fp/x87.h"

namespace mh::fp {

// Lifted from sim/sim_facing24_from_points.cpp:sector_index_from_angle. The divisor was a file-local
// constant there (SECTOR_DEG = 15.0, _G_LLM_STRAT_FACING24_SECTOR_DEG @0x00501534); it stays a
// PARAMETER so the constant remains documented at its own site and the helper is the plain shape it
// actually is -- trunc(a / b + 1).
//
// C++ (CRT-X87-CPP step 3), PROVEN -- fptest case T1 holds the verbatim assembly.
// **CORRECTNESS DEPENDS ON PC=53**, which libmh GUARANTEES (libmh.h's contract prose +
// libmh_install_fp_precision on the standalone path): measured, 0 of 256 swept inputs differ at
// PC=53 and 4 differ at PC=64, because the original keeps `a / b` at 64 significand bits where a C++
// double does not. The exponent axis is clear: `+ 1.0` cannot pull an out-of-range quotient back, so
// anything leaving double range reaches the truncating sink and saturates identically in both arms.
// FP-CPP-PROVEN: fptest T1 -- 0 of 256 at PC=53; PC=53-DEPENDENT (4 at PC=64).
inline int32_t trunc_div_plus1(double angle_deg, double sector_deg) {
    return trunc_i32(angle_deg / sector_deg + 1.0);
}

// Lifted verbatim from sim/sim_unit_apply_damage.cpp:soldier_strip_should_continue.
//
// STILL ASSEMBLY, and the reason is MEASURED (CRT-X87-CPP step 2, fptest case C1). The FLAG half is
// settled -- SETNC reads CF = C0, whose negation is exactly C++'s `threshold >= cur_energy`, false on
// unordered like the hardware -- but the ARITHMETIC half refuses at BOTH precision settings:
//   * PC=64: 44 of 1584 swept inputs differ, e.g. soldier_count = INT_MIN, energy_max = 1e-300,
//     cur_energy = 1e-300. `energy_max - energy_max/soldier_count` is kept at 64 significand bits in
//     the register; a C++ double expression is not, and the two land on opposite sides of the compare.
//   * PC=53: 16 still differ, and PC is not what causes them -- the x87 register has a 15-bit
//     EXPONENT with no control-word field that narrows it. energy_max = DBL_MAX with a negative
//     soldier_count makes the subtraction exceed double range: finite in the register, +inf as a
//     double, and `>= inf` then answers differently. No precision setting and no C++ spelling can
//     reproduce that, which is why this one is not merely PC-conditional -- it is refused.
// fptest case C1 pins the refusal by requiring the C++ spelling to keep diverging, so it is not
// re-litigated from a narrower sweep. CONSIDERED AND DECLINED: the call site's operands are unit
// energies and a soldier count, so a C++ body under a documented domain precondition would probably
// hold -- but the precondition would need a reachability proof of its own, and keeping the assembly
// costs nothing.
// FP-ASM-KEEP: fptest C1 -- REFUSED. 16 of 1584 differ even at PC=53 (the subtraction leaves double
//   range, which the x87 exponent keeps and no PC field narrows), 44 at PC=64.
inline bool soldier_strip_should_continue(int32_t soldier_count, double energy_max, double cur_energy) {
    uint8_t result = 0;
    // clang-format off
    __asm {
        fild    dword ptr [soldier_count]  ; ST(0) = (double)soldier_count         (0x0047e587)
        fdivr   qword ptr [energy_max]     ; ST(0) = energy_max / soldier_count    (0x0047e59c)
        fsubr   qword ptr [energy_max]     ; ST(0) = energy_max - ST(0)            (0x0047e5b1)
        fcomp   qword ptr [cur_energy]     ; compare ST(0) (threshold) vs cur_energy, pop (0x0047e5bc)
        fnstsw  ax
        sahf
        setnc   result                     ; CF=0 (threshold >= cur_energy) -> continue
    }
    // clang-format on
    return result != 0;
}

// STILL ASSEMBLY, REFUSED, and the reason is MEASURED (CRT-X87-CPP step 3, fptest case W, row target_pip_count).
// This is a MULTI-OP chain, and that is what decides it: 192 of 2816 swept inputs differ **at PC=53**
// (and 250 at PC=64), so the PC=53 guarantee does NOT rescue it. That row DECLARES its REFUSE class at the sweep
// call site and fptest fails if the measurement ever stops agreeing -- in either direction. What differs is the x87
// register's 15-bit EXPONENT, which no control-word field narrows.
// The measured counter-example: emax=1e300, ecur=0, pip_slot_count=INT_MIN, scale=0.3 -- `(emax-ecur) * (pip*scale)`
// overflows a double to -inf where the register keeps it finite, and the FINAL `/ emax` then
// divides it BACK into int32 range, so the sink sees -6.4e8 through the register and a
// saturated indefinite through the double.
// THE GENERAL RULE THIS HELPER ESTABLISHES, and it corrected a wrong prediction: a truncating
// sink SATURATES everything outside int32 range to the same integer indefinite, so an exponent
// excursion is invisible when it reaches the sink DIRECTLY -- but a later operation that
// RESCALES it back into range (a divide-down, or a multiply by zero) makes it visible again.
// Single-op truncation helpers therefore clear the exponent axis; chains that can come back
// do not.
// Lifted verbatim from sim/sim_bldg_update_charge_pips.cpp:target_pip_count.
// FP-ASM-KEEP: fptest W -- REFUSED, row target_pip_count: 192 of 2816 differ AT PC=53, so the
//   guarantee does not rescue it. A multi-op chain whose overflow a later divide rescales back into
//   range.
inline int32_t target_pip_count(double energy_max, double energy_cur, int32_t pip_slot_count, double scale) {
    int32_t  r       = 0;
    uint16_t cw_save = 0, cw_trunc = 0;
    // clang-format off  (one instruction per line; clang-format would fold the __asm block)
    __asm {
        fld     qword ptr [energy_max]      ; ST(0) = cfg_buildings[bid].energy         (0x00478f04)
        fsub    qword ptr [energy_cur]      ; ST(0) -= buildings[...].energy            (0x00478f0a)
        fild    dword ptr [pip_slot_count]  ; ST(0) = pip_slot_count (int->float10, push)(0x00478f30)
        fmul    qword ptr [scale]           ; ST(0) *= DAT_00501382                     (0x00478f36)
        fmulp   st(1), st(0)                ; ST(1) *= ST(0); pop -> ST(0) = product    (0x00478f3c)
        fdiv    qword ptr [energy_max]      ; ST(0) /= cfg_buildings[bid].energy        (0x00478f5e)
        ; --- utils_math_trunc @0x004d0596, inlined: round toward zero -------------------------
        fstcw   cw_save             ;                                          (0x004d0597)
        mov     ax, cw_save
        mov     ah, 0x1f            ; RC = 11 truncate, PC = 11 extended       (0x004d059f)
        mov     cw_trunc, ax
        fldcw   cw_trunc            ;                                          (0x004d05a4)
        frndint                     ;                                          (0x004d05a7)
        fldcw   cw_save             ; restore BEFORE the store                 (0x004d05a9)
        ; -----------------------------------------------------------------------------------------
        fistp   dword ptr [r]       ;                                          (0x00478f69)
    }
    // clang-format on
    return r;
}

// Lifted from tact/tact_fx_update_projectile.cpp:trunc_add.
// C++ (CRT-X87-CPP step 3), PROVEN -- fptest case T2 holds the verbatim assembly: 0 of 256 swept
// inputs differ at PC=53 AND at PC=64, so this one is NOT PC-dependent. One operation whose result
// goes straight to a truncating sink: a sum that leaves double range saturates to the same integer
// indefinite through both arms, and no swept 53-vs-64-bit sum straddles an integer boundary.
// FP-CPP-PROVEN: fptest T2 -- 0 of 256 at BOTH settings.
inline int32_t trunc_add(double a, double b) { return trunc_i32(a + b); }

// Lifted from ai/ai_group_muster_pick.cpp:weapon_power_add_and_trunc.
// C++ (CRT-X87-CPP step 3), PROVEN -- fptest case T7 holds the verbatim assembly: 0 of 160 swept
// inputs differ at BOTH precision settings, so not PC-dependent.
// TWO THINGS THE SPELLING HAS TO GET RIGHT, both read off the disassembly rather than off the shape:
//   * 0x004d6d0b-0x004d6d0e builds the operand as a QWORD with an EXPLICIT ZERO high dword and FILDs
//     that, so the value is ZERO-extended -- a uint32 -> double conversion, not a sign-extended one.
//   * 0x004d6d28 reads back only `dword ptr [ESP]`, the LOW dword of the 64-bit store, hence
//     trunc_i64_low32 rather than trunc_i32.
// FP-CPP-PROVEN: fptest T7 -- 0 of 160 at BOTH settings.
inline int32_t trunc_add_u32_double_low(uint32_t running_total, const double *addend) {
    return trunc_i64_low32(static_cast<double>(running_total) + *addend);
}

// Lifted from sim/sim_dir_headings.cpp:angle_div_sector_trunc.
// C++ (CRT-X87-CPP step 3), PROVEN -- fptest case T5 holds the verbatim assembly.
// **CORRECTNESS DEPENDS ON PC=53**, which libmh GUARANTEES: 0 of 4254 swept inputs differ at PC=53,
// 4 differ at PC=64.
// FP-CPP-PROVEN: fptest T5 -- 0 of 4254 at PC=53; PC=53-DEPENDENT (4 at PC=64).
inline int32_t trunc_div(double angle_deg, double sector_deg) {
    return trunc_i32(angle_deg / sector_deg);
}

// Lifted from ai/ai_opponent_relations.cpp:trunc_float_to_int32.
// C++ (CRT-X87-CPP step 3), PROVEN -- fptest case T6 holds the verbatim assembly: 0 divergences at
// BOTH precision settings, so not PC-dependent -- and structurally it could not be, because there is
// no arithmetic here at all. A float widens to double exactly; the only content is the sink, and
// this one is `fistp qword` + a LOW-DWORD take, which is why it goes through trunc_i64_low32 and NOT
// through trunc_i32 (they differ out of range -- see x87.h and fptest case Q).
// FP-CPP-PROVEN: fptest T6 -- 0 of 17156 at BOTH settings.
inline int32_t trunc_float_to_int32(float value) {
    return trunc_i64_low32(static_cast<double>(value));
}

// STILL ASSEMBLY, REFUSED, and the reason is MEASURED (CRT-X87-CPP step 3, fptest case W, row trunc_hp_loss_level).
// This is a MULTI-OP chain, and that is what decides it: 88 of 4096 swept inputs differ **at PC=53**
// (and 102 at PC=64), so the PC=53 guarantee does NOT rescue it. That row DECLARES its REFUSE class at the sweep
// call site and fptest fails if the measurement ever stops agreeing -- in either direction. What differs is the x87
// register's 15-bit EXPONENT, which no control-word field narrows.
// The measured counter-example: max=0.1, cur=DBL_MAX, scale=0 -- the `/ max` overflows a double to inf, then `* 0` gives NaN
// where the register gives 0.
// THE GENERAL RULE THIS HELPER ESTABLISHES, and it corrected a wrong prediction: a truncating
// sink SATURATES everything outside int32 range to the same integer indefinite, so an exponent
// excursion is invisible when it reaches the sink DIRECTLY -- but a later operation that
// RESCALES it back into range (a divide-down, or a multiply by zero) makes it visible again.
// Single-op truncation helpers therefore clear the exponent axis; chains that can come back
// do not.
// Lifted verbatim from sim/sim_unit_update_damage_smoke.cpp:trunc_hp_loss_level.
// FP-ASM-KEEP: fptest W -- REFUSED, row trunc_hp_loss_level: 88 of 4096 differ AT PC=53.
inline int32_t trunc_hp_loss_level(double max_energy, double cur_energy, double scale) {
    int32_t  r       = 0;
    uint16_t cw_save = 0, cw_trunc = 0;
    // clang-format off  (one instruction per line; clang-format would fold the __asm block)
    __asm {
        fld     qword ptr [max_energy]   ; ST0 = max_energy                        (0x00487155)
        fsub    qword ptr [cur_energy]   ; ST0 = max_energy - cur_energy           (0x0048715b)
        fdiv    qword ptr [max_energy]   ; ST0 = ST0 / max_energy                  (0x00487181)
        fmul    qword ptr [scale]        ; ST0 = ST0 * scale (DAT_0050147a)        (0x00487187)
        ; --- utils_math_trunc @0x004d0596, inlined: round toward zero -------------------------
        fstcw   cw_save             ;                                          (0x004d0597)
        mov     ax, cw_save
        mov     ah, 0x1f            ; RC = 11 truncate, PC = 11 extended        (0x004d059f)
        mov     cw_trunc, ax
        fldcw   cw_trunc            ;                                          (0x004d05a4)
        frndint                     ;                                          (0x004d05a7)
        fldcw   cw_save             ; restore BEFORE the store                 (0x004d05a9)
        ; -----------------------------------------------------------------------------------------
        fistp   dword ptr [r]       ;                                          (0x00487192)
    }
    // clang-format on
    return r;
}

// Lifted from sim/sim_weapon_projectile_spawn.cpp:trunc_mul.
// C++ (CRT-X87-CPP step 3), PROVEN -- fptest case T4 holds the verbatim assembly.
// **CORRECTNESS DEPENDS ON PC=53**, which libmh GUARANTEES. The measurement is emphatic and it is
// the same one that refused x87.h's trunc_i32_mul: 0 of 4254 swept inputs differ at PC=53, and
// **1743** differ at PC=64 -- the identical count, from the identical k * (1/k) family, which lands
// exactly halfway below 1.0 so a 53-bit product rounds up to 1.0 and truncates to 1 while a 64-bit
// one stays below and truncates to 0. Read that number as the warning it is: without the PC=53
// guarantee this helper is wrong on hundreds of ordinary inputs.
// FP-CPP-PROVEN: fptest T4 -- 0 of 4254 at PC=53; PC=53-DEPENDENT (1743 at PC=64).
inline int32_t trunc_mul(double a, double b) { return trunc_i32(a * b); }

// STILL ASSEMBLY, REFUSED, and the reason is MEASURED (CRT-X87-CPP step 3, fptest case W, row trunc_mul_div).
// This is a MULTI-OP chain, and that is what decides it: 58 of 8094 swept inputs differ **at PC=53**
// (and 1819 at PC=64), so the PC=53 guarantee does NOT rescue it. That row DECLARES its REFUSE class at the sweep
// call site and fptest fails if the measurement ever stops agreeing -- in either direction. What differs is the x87
// register's 15-bit EXPONENT, which no control-word field narrows.
// The measured counter-example: a=3, b=DBL_MAX, c=1e300 -- `a*b` overflows to inf as a double and stays finite in the
// register, and the following `/ c` brings it back into range.
// THE GENERAL RULE THIS HELPER ESTABLISHES, and it corrected a wrong prediction: a truncating
// sink SATURATES everything outside int32 range to the same integer indefinite, so an exponent
// excursion is invisible when it reaches the sink DIRECTLY -- but a later operation that
// RESCALES it back into range (a divide-down, or a multiply by zero) makes it visible again.
// Single-op truncation helpers therefore clear the exponent axis; chains that can come back
// do not.
// Lifted verbatim from sim/sim_projectile_tick.cpp:trunc_mul_div.
// FP-ASM-KEEP: fptest W -- REFUSED, row trunc_mul_div: 58 of 8094 differ AT PC=53. The product
//   overflows a double where the register keeps it finite, and the following divide brings it back.
inline int32_t trunc_mul_div(double a, double b, double c) {
    int32_t  r       = 0;
    uint16_t cw_save = 0, cw_trunc = 0;
    // clang-format off
    __asm {
        fld     qword ptr [a]
        fmul    qword ptr [b]
        fdiv    qword ptr [c]
        fstcw   cw_save
        mov     ax, cw_save
        mov     ah, 0x1f
        mov     cw_trunc, ax
        fldcw   cw_trunc
        frndint
        fldcw   cw_save
        fistp   dword ptr [r]
    }
    // clang-format on
    return r;
}

// STILL ASSEMBLY, REFUSED, and the reason is MEASURED (CRT-X87-CPP step 3, fptest case W, row trunc_mul_mul).
// This is a MULTI-OP chain, and that is what decides it: 58 of 8094 swept inputs differ **at PC=53**
// (and 1801 at PC=64), so the PC=53 guarantee does NOT rescue it. That row DECLARES its REFUSE class at the sweep
// call site and fptest fails if the measurement ever stops agreeing -- in either direction. What differs is the x87
// register's 15-bit EXPONENT, which no control-word field narrows.
// The measured counter-example: a=3, b=DBL_MAX, c=0 -- `a*b` overflows to inf as a double, and `inf * 0` is NaN (which
// truncates to the integer indefinite) where `finite * 0` is 0.
// THE GENERAL RULE THIS HELPER ESTABLISHES, and it corrected a wrong prediction: a truncating
// sink SATURATES everything outside int32 range to the same integer indefinite, so an exponent
// excursion is invisible when it reaches the sink DIRECTLY -- but a later operation that
// RESCALES it back into range (a divide-down, or a multiply by zero) makes it visible again.
// Single-op truncation helpers therefore clear the exponent axis; chains that can come back
// do not.
// Lifted verbatim from sim/sim_weapon_projectile_spawn.cpp:trunc_mul_mul.
// FP-ASM-KEEP: fptest W -- REFUSED, row trunc_mul_mul: 58 of 8094 differ AT PC=53. Same shape,
//   rescued by a multiply-by-zero (inf * 0 is NaN; finite * 0 is 0).
inline int32_t trunc_mul_mul(double a, double b, double c) {
    int32_t  r       = 0;
    uint16_t cw_save = 0, cw_trunc = 0;
    // clang-format off
    __asm {
        fld     qword ptr [a]
        fmul    qword ptr [b]
        fmul    qword ptr [c]
        fstcw   cw_save
        mov     ax, cw_save
        mov     ah, 0x1f
        mov     cw_trunc, ax
        fldcw   cw_trunc
        frndint
        fldcw   cw_save
        fistp   dword ptr [r]
    }
    // clang-format on
    return r;
}

// Lifted from sim/sim_weapon_damage_calc.cpp:trunc_qword_low.
//
// C++ (CRT-X87-CPP step 3), PROVEN -- fptest case T12 holds the verbatim assembly: 0 divergences at
// BOTH precision settings, not PC-dependent.
//
// THIS HELPER'S EARLIER REFUSAL WAS WRONG, and the correction is worth recording. It was filed under
// "the `fild qword` / `fistp qword` width is PC-dependent above 2^53" -- true of that shape in
// general, and NOT true here: the PC-dependent half is the `fild qword` INPUT conversion of an int64
// larger than 2^53, and this helper has no `fild` at all. It takes a `double` and its only content
// is the `fistp qword` + LOW-DWORD sink, which fptest case Q pins exactly. The refusal was inherited
// from the shape rather than read off the body; the measurement corrected it.
//
// The original never round-trips this value through memory between llm_math_scale_pct's ST0 return
// and utils_math_trunc's FRNDINT (0x004d3417-0x004d341c has no FLD at all); our `c.scale_pct(...)`
// returns through a `double` -- already the interop layer's precision floor -- so the value reaching
// here is the same 64-bit one either way.
// FP-CPP-PROVEN: fptest T12 -- 0 of 17161 at BOTH settings.
inline int32_t trunc_qword_low(double x) { return trunc_i64_low32(x); }

// STILL ASSEMBLY, REFUSED, and the reason is MEASURED (CRT-X87-CPP step 3, fptest case W, row trunc_scaled_int).
// This is a MULTI-OP chain, and that is what decides it: 17 of 6814 swept inputs differ **at PC=53**
// (and 986 at PC=64), so the PC=53 guarantee does NOT rescue it. That row DECLARES its REFUSE class at the sweep
// call site and fptest fails if the measurement ever stops agreeing -- in either direction. What differs is the x87
// register's 15-bit EXPONENT, which no control-word field narrows.
// The measured counter-example: value_int=0, ratio_num=1, ratio_den=denorm_min -- the divide overflows a double to inf, then
// the `* 0` gives NaN where the register gives 0.
// THE GENERAL RULE THIS HELPER ESTABLISHES, and it corrected a wrong prediction: a truncating
// sink SATURATES everything outside int32 range to the same integer indefinite, so an exponent
// excursion is invisible when it reaches the sink DIRECTLY -- but a later operation that
// RESCALES it back into range (a divide-down, or a multiply by zero) makes it visible again.
// Single-op truncation helpers therefore clear the exponent axis; chains that can come back
// do not.
// Lifted verbatim from sim/sim_projectile_tick.cpp:trunc_scaled_int.
// FP-ASM-KEEP: fptest W -- REFUSED, row trunc_scaled_int: 17 of 6814 differ AT PC=53.
inline int32_t trunc_scaled_int(int32_t value_int, double ratio_num, double ratio_den) {
    int32_t  r       = 0;
    uint16_t cw_save = 0, cw_trunc = 0;
    // clang-format off  (one instruction per line; clang-format would fold the __asm block)
    __asm {
        fld     qword ptr [ratio_num]  ; ST0 = ratio_num                        (0x00440e83)
        fdiv    qword ptr [ratio_den]  ; ST0 /= ratio_den                       (0x00440e86)
        fild    dword ptr [value_int]  ; ST0(new) = (double)value_int           (0x00440e8e)
        fmulp   st(1), st(0)           ; ST(1) = ST(1)*ST(0); pop               (0x00440e91)
        ; --- utils_math_trunc @0x004d0596, inlined: round toward zero -------------------------
        fstcw   cw_save             ;                                          (0x004d0597)
        mov     ax, cw_save
        mov     ah, 0x1f            ; RC = 11 truncate, PC = 11 extended        (0x004d059f)
        mov     cw_trunc, ax
        fldcw   cw_trunc            ;                                          (0x004d05a4)
        frndint                     ;                                          (0x004d05a7)
        fldcw   cw_save             ; restore BEFORE the store                 (0x004d05a9)
        ; -----------------------------------------------------------------------------------------
        fistp   dword ptr [r]       ;                                          (0x00440e98/0x00440eb5)
    }
    // clang-format on
    return r;
}

// Lifted from tact/tact_fx_update_projectile.cpp:trunc_sub.
// C++ (CRT-X87-CPP step 3), PROVEN -- fptest case T3 holds the verbatim assembly: 0 of 256 swept
// inputs differ at PC=53 AND at PC=64. Not PC-dependent, same single-op-into-a-truncating-sink
// reasoning as trunc_add.
// FP-CPP-PROVEN: fptest T3 -- 0 of 256 at BOTH settings.
inline int32_t trunc_sub(double a, double b) { return trunc_i32(a - b); }

// Lifted verbatim from ai/ai_shortage_gate.cpp:x87_capacity_short.
//
// STILL ASSEMBLY, and the reason is MEASURED (CRT-X87-CPP step 2, fptest case C2). The FLAG half is
// settled: SAHF + JBE tests CF|ZF = C0|C3, so the 1 verdict is a STRICT, ORDERED `threshold > ratio`
// -- unordered sets both bits, so a NaN takes the JBE and returns 0, which is what C++'s `>` does
// too. The DIVISION is what refuses, at both precision settings, and one of the two is mundane:
//   * PC=64: 42 of 1584 swept inputs differ, and the cheapest is capacity=1, holdings=0.1,
//     threshold=10.0f. 1/0.1 kept at 64 bits is just BELOW 10; rounded to a double it is exactly
//     10.0. `10 > ratio` is therefore 1 through the register and 0 through the C++ double. Nothing
//     exotic about those numbers -- this is not an edge case, it is the ordinary behaviour.
//   * PC=53: 36 still differ, from EXPONENT RANGE rather than precision -- the x87 register's 15-bit
//     exponent has no control-word field that narrows it, so `capacity / holdings` with a denormal
//     divisor stays finite in the register where a double overflows to +inf.
// Since `holdings` is a DOUBLE (unlike the int32/int32 siblings below that did clear), both the
// significand and the exponent can leave what C++ keeps. fptest case C2 pins the refusal.
// CONSIDERED AND DECLINED: ai_shortage_gate.cpp passes an integer-valued, non-zero `holdings` (it
// flushes 0.0 to 1.0 right before the call), so a C++ body under a documented domain precondition
// would probably hold -- but the precondition would need a reachability proof of its own, and
// keeping the assembly costs nothing.
// FP-ASM-KEEP: fptest C2 -- REFUSED. 36 of 1584 differ at PC=53 (exponent range), 42 at PC=64 --
//   the cheapest being the entirely ordinary capacity=1, holdings=0.1, threshold=10.
inline int32_t x87_capacity_short(int32_t capacity, double holdings, const float *threshold) {
    int32_t  cap = capacity;
    uint16_t sw  = 0;
    // clang-format off  (one instruction per line; clang-format would fold the __asm block)
    __asm {
        mov     eax, threshold
        fild    dword ptr [cap]         ; ST(0) = (double)(int)capacity   (0x004e38ca)
        fdiv    qword ptr [holdings]    ; ST(0) = capacity / holdings     (0x004e38d1)
        fld     dword ptr [eax]         ; ST(0) = fSiloRatio, ST(1) = ratio (0x004e38d4)
        fcompp                          ; compare ST(0) with ST(1), pop both (0x004e38da)
        fnstsw  sw                      ;                                 (0x004e38dc)
    }
    // clang-format on
    return ((sw & 0x4100u) == 0) ? 1 : 0;
}

// Lifted from ai/ai_worker_rebalance.cpp:x87_floor_diff_times_ratio.
// C++ (CRT-X87-CPP step 3), PROVEN -- fptest case T9 holds the verbatim assembly: 0 divergences at
// BOTH precision settings, not PC-dependent. The original rounds ONCE, to a double, at its own
// `fstp qword` before the CRT call -- and a C++ `(double)diff * (double)*ratio` compiled for x87
// rounds at exactly the same point, so there is no 80-bit value left for the two arms to disagree
// about. That is also why this row carried no width probe in the classification table: the probe
// would have been the same instruction sequence as the subject.
// `floor` @0x004daadb is immediately followed by `trunc` @0x004d0596 at both original call sites;
// `floor` already yields an integral double, so the `trunc` is a no-op on its result and the pair
// means exactly `(int)floor(x)`.
// FP-CPP-PROVEN: fptest T9 -- 0 of 110 at BOTH settings.
inline int32_t x87_floor_diff_times_ratio(int32_t diff, const float *ratio) {
    return static_cast<int32_t>(std::floor(static_cast<double>(diff) * static_cast<double>(*ratio)));
}

// Lifted from ai/ai_worker_rebalance.cpp:x87_floor_worker_count_times_util.
// C++ (CRT-X87-CPP step 3), PROVEN -- fptest case T10 holds the verbatim assembly: 0 divergences at
// BOTH precision settings, not PC-dependent. Same single-op-then-`fstp qword` shape as
// x87_floor_diff_times_ratio above, and the same reason the width axis is vacuous here.
// FP-CPP-PROVEN: fptest T10 -- 0 of 176 at BOTH settings.
inline int32_t x87_floor_worker_count_times_util(int32_t worker_count, double util) {
    return static_cast<int32_t>(std::floor(static_cast<double>(worker_count) * util));
}

// Lifted from ai/ai_worker_rebalance.cpp:x87_housing_short.
//
// C++ (CRT-X87-CPP step 2), PROVEN -- `fptest` case C3 holds the verbatim assembly and re-derives
// this equality on every run: 0 of 1089 swept inputs differ at PC=53 AND at PC=64, over every
// int32 edge (INT_MIN / 0 / INT_MAX), every NaN and infinity placement, and a float threshold
// planted AT the exact quotient and at both its neighbours. NOT PC-dependent, and the reason is
// operand widths rather than luck: both operands are int32, so the quotient can never leave double
// range, and for a 53-vs-64-bit intermediate to flip the verdict a FLOAT threshold would have to
// sit within ~2^-64 relative of the exact quotient, where the smallest nonzero gap available is
// ~2^-61. fptest's width arm asserts that probe stays at 0 in BOTH directions, so if the bound is
// ever wrong the suite says so instead of passing quietly.
//
// THE FLAG: JA @0x004e40a5 is taken (the "short" verdict) iff CF=0 AND ZF=0, i.e. extra_space is
// STRICTLY greater than the ratio and the comparison is ORDERED -- CF is status-word bit 8 (0x0100),
// ZF is bit 14 (0x4000). C++'s `>` is that predicate exactly, including false on unordered; fptest's
// negative arm keeps a plausible-but-wrong `!(x <= y)` spelling and requires it to diverge.
//
// `noinline` IS NOT DECORATION AND MUST NOT BE REMOVED: with it inlined, MSVC 19.44.35228's
// link-time code generator ICEs -- `fatal error C1001 ... link!InvokeCompilerPass` naming this
// helper's forwarder at ai_worker_rebalance.cpp:88. Measured, twice, both directions. The same
// applies to x87_yield_below_spend_rate below; x87_labor_utilization (which returns a double rather
// than materialising the compare as an integer) does not need it.
// FP-CPP-PROVEN: fptest C3 -- 0 of 1089 at BOTH settings.
__declspec(noinline) inline bool x87_housing_short(int32_t housing_prev, int32_t pop_total, const float *extra_space) {
    const double ratio = static_cast<double>(housing_prev) / static_cast<double>(pop_total);
    const double extra = static_cast<double>(*extra_space);
    return extra > ratio;
}

// Lifted from ai/ai_worker_rebalance.cpp:x87_labor_utilization.
//
// C++ (CRT-X87-CPP step 2), PROVEN -- `fptest` case C4 holds the verbatim assembly: 0 of 1089 swept
// inputs differ at PC=53 AND at PC=64, compared BY BITS (a -0.0 or an unflushed NaN must not pass as
// equal). NOT PC-dependent, and here that is structural rather than a bound: the ORIGINAL itself
// stores the quotient to a double before either compare, so it double-rounds exactly as the C++
// expression does and there is no 80-bit value left to disagree about.
//
// THIS ONE HOLDS TWO of the five compares, and they are not the same predicate:
//   * JBE @0x004e3d95 skips the flush unless CF=0 AND ZF=0 -- i.e. the clamp fires on a STRICT,
//     ORDERED `max_fuck_ratio > util`, which is C++'s `>` exactly (false on unordered).
//   * JNC @0x004e3dcc skips the flush unless CF=1 -- i.e. the clamp fires on RAW C0, which is
//     `util > 1.0` OR UNORDERED. C++'s `util > 1.0` is wrong there (false on a NaN); the faithful
//     spelling is `!(1.0 >= util)`, and the assembly really does flush a NaN util to 1.0.
// The original rounds the quotient to a double via `fstp qword [util]` BEFORE either compare, so
// both comparisons are double-against-double in the original too.
// FP-CPP-PROVEN: fptest C4 -- 0 of 1089 at BOTH settings, compared BY BITS.
inline double x87_labor_utilization(int32_t avail, int32_t total, const float *max_fuck_ratio) {
    double       util = static_cast<double>(avail) / static_cast<double>(total);
    const double cap  = static_cast<double>(*max_fuck_ratio);
    if (cap > util) util = 0.0;
    if ((1.0 >= util) == false) util = 1.0; // raw C0: `1.0 < util` OR unordered
    return util;
}

// Lifted from ai/ai_spend_rate.cpp:x87_ratio.
// C++ (CRT-X87-CPP step 3), PROVEN -- fptest case T11 holds the verbatim assembly, compared BY BITS
// (this one returns a double, so a sign-of-zero or an unflushed NaN must not pass as equal): 0
// divergences at BOTH precision settings, not PC-dependent. One divide, then the ORIGINAL's own
// `fstp qword` -- both arms round once, at the same point.
// Both operands are built at 0x004e7c8b-0x004e7caa as QWORDs with an explicit 0 high dword and
// FILDed, i.e. ZERO-extended, which is what the uint32 -> double conversions reproduce.
// FP-CPP-PROVEN: fptest T11 -- 0 of 100 at BOTH settings, compared BY BITS.
inline double x87_ratio(uint32_t numer, uint32_t denom) {
    return static_cast<double>(numer) / static_cast<double>(denom);
}

// Lifted from ai/ai_mine_yield.cpp:x87_scale_and_trunc.
//
// C++ (CRT-X87-CPP step 3), PROVEN -- fptest case T13 holds the verbatim assembly.
// **CORRECTNESS DEPENDS ON PC=53**, which libmh GUARANTEES: 0 divergences at PC=53, and the PC=64
// column is printed by that case rather than assumed.
//
// THIS HELPER'S EARLIER REFUSAL WAS ALSO WRONG, same correction as trunc_qword_low above. It was
// filed under the "`fild qword` above 2^53" rule, but the operand here is a **uint32** zero-extended
// into a qword (the high dword is an EXPLICIT zero store at 0x004e3218) -- so the FILD is exact, can
// never exceed 2^32, and the >2^53 hazard is unreachable at this signature. What actually decides it
// is the single multiply, which is a precision question and therefore answered by the guarantee.
// The zero extension is reproduced, not narrowed: a "negative" extract_val is read as a large
// positive, exactly as the original does.
// 0x004e3233 takes the LOW dword of the 64-bit store, hence trunc_i64_low32 rather than trunc_i32.
// FP-CPP-PROVEN: fptest T13 -- 0 of 4158 at PC=53; PC=53-DEPENDENT (1743 at PC=64).
inline int32_t x87_scale_and_trunc(uint32_t extract_val, const double *weight) {
    return trunc_i64_low32(static_cast<double>(extract_val) * *weight);
}

// Lifted from ai/ai_player_tick.cpp:x87_sqrt_and_trunc.
// C++ (CRT-X87-CPP step 3), PROVEN -- fptest case T8 holds the verbatim assembly: 0 of 57153 swept
// inputs differ at BOTH precision settings, so not PC-dependent.
// The high dword is an EXPLICIT zero store (`MOV [EBP-0x1c],0` @0x004e8cb2), so the FILD is
// zero-extended -- a uint32 -> double conversion -- and 0x004e8cc9 takes the LOW dword of the 64-bit
// store, hence trunc_i64_low32.
// THE ERROR ARM IS STILL A COMMENT, NOT A BRANCH: the original calls CRT_004da9f0, whose full body
// is FTST/FSTSW/SAHF/JNC -> FSQRT with the carry (negative-operand) arm popping ST0, calling the
// math error handler at 0x004e91d0 and returning AL=1 -- an ERROR FLAG, not a value. That arm is
// provably unreachable for THIS caller (a squared distance, zero-extended into a 64-bit int), which
// is why `std::sqrt` of a non-negative double is the whole of it.
// FP-CPP-PROVEN: fptest T8 -- 0 of 57153 at BOTH settings.
inline int32_t x87_sqrt_and_trunc(uint32_t radius_sq) {
    return trunc_i64_low32(std::sqrt(static_cast<double>(radius_sq)));
}

// Lifted from ai/ai_mine_plan.cpp:x87_yield_below_spend_rate.
//
// C++ (CRT-X87-CPP step 2), PROVEN -- `fptest` case C5 holds the verbatim assembly: 0 of 900 swept
// inputs differ at PC=53 AND at PC=64, over the uint32 edges (0 / 0x7fffffff / 0x80000000 /
// 0xffffffff), every NaN and infinity placement, and a float clock planted AT the exact quotient and
// at both its neighbours. NOT PC-dependent, on the same operand-width ground as x87_housing_short:
// `yield` is a uint32, i.e. an EXACT integer below 2^32, and the rate is uint32/float -- a 32-bit
// significand over a 24-bit one -- so a nonzero distance between the rate and that integer is at
// least ~2^-56 relative, never inside the ~2^-64 band where 53 and 64 bits disagree. fptest's width
// arm asserts that probe stays at 0 in both directions.
//
// THE FLAG: SAHF + JNC @0x004e540a jumps to `XOR EAX,EAX` when CF (= C0) is clear, so the 1 verdict
// is RAW C0 -- "yield < rate OR UNORDERED". C++'s `yield < rate` is wrong there (false on a NaN rate
// where the original returns 1); the faithful spelling is the negation of `>=`. fptest's negative
// arm keeps the wrong `<` spelling and requires it to diverge.
//
// 0x004e53de-0x004e5402 builds each operand as a QWORD whose high dword is an explicit 0 and FILDs
// that, so both are ZERO-extended -- not sign-extended, which is what a `FILD dword` would have
// given and what the sibling in ai_shortage_gate.cpp actually does. Both are uint32 here, so the
// C++ uint32->double conversions reproduce that exactly.
//
// `noinline` IS NOT DECORATION -- see x87_housing_short: inlined, MSVC 19.44.35228's LTCG ICEs
// (C1001 at this helper's forwarder, ai_mine_plan.cpp:15).
// FP-CPP-PROVEN: fptest C5 -- 0 of 900 at BOTH settings.
__declspec(noinline) inline int32_t x87_yield_below_spend_rate(uint32_t spend_total, const float *ai_clock, uint32_t yield) {
    const double rate = static_cast<double>(spend_total) / static_cast<double>(*ai_clock);
    const double y    = static_cast<double>(yield);
    return (y >= rate) ? 0 : 1; // raw C0: `<` OR unordered, i.e. the NEGATION of `>=`
}

} // namespace mh::fp
