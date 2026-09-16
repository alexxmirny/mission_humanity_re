//
// fp/x87.h -- the x87 sequences the translated bodies need, in ONE place (tracker CRT-X87).
//
// WHY THIS FILE EXISTS. The originals are Watcom x86 with x87 floating point, and a few of their
// idioms have no bit-faithful C++ spelling that can be asserted rather than assumed -- so the
// translations carry inline `__asm`. Measured 2026-09-08: 60 blocks across sim/ai/tact, but only 26
// DISTINCT instruction sequences, and ONE of those accounted for 26 blocks under SIX different local
// names (`trunc_to_int32`, `trunc_dword`, `trunc_only`, `trunc_toward_zero`, `trunc_toward_zero_i32`,
// `truncate_toward_zero`). Six names, one semantic, twenty-six copies of the same nine instructions.
//
// So this header is step 1 of two: HOIST every duplicated sequence to one named helper, which is
// behaviour-preserving by construction because the assembly MOVES rather than changes. Step 2 is to
// replace a helper's body with C++ wherever an offline fixture proves bit-equality over a swept
// domain -- see the note on each helper for whether that is expected to close, and CRT-X87 for why
// it may not (the prologue arithmetic runs at 80-bit extended precision inside the x87 stack, and
// FISTP on an out-of-range value stores the integer indefinite where a C++ cast is undefined).
//
// MERGING RULE, and it is the one that keeps this safe: two blocks collapse into one helper ONLY if
// their instruction text is IDENTICAL including operand sizes. A mnemonic-only comparison calls
// `fistp dword` and `fistp qword` the same instruction; they are not, and `trunc_qword_low` is a
// real function in this tree that differs from the 26 in exactly that way. The dedupe that produced
// this file compared full text with only local NAMES normalised away.
//
// THE CONTROL-WORD DANCE, since every helper here contains it. `FSTCW` saves, `mov ah,0x1f` sets
// RC=11 (round toward zero) and PC=11 (extended), `FLDCW` installs it, `FRNDINT` rounds under it,
// `FLDCW` restores the caller's word BEFORE the store, and `FISTP` stores. Round-toward-zero is
// exactly what a C++ cast to an integer type is defined to do, which is why step 2 is plausible at
// all -- and the restore-before-store ordering is load-bearing, so it is preserved verbatim.
//
// ---- STEP 2, 2026-09-08: ONE HELPER CLEARED AND ONE DID NOT ----------------------------------------
//
// The 80-bit question was measured, at BOTH x87 precision settings -- the DLL harness pins PC=53
// (harness.cpp `pin_fpu`, default on) while the bare x87 default is PC=64, so both are reachable and
// a helper correct at only one would look fine for months. The two helpers came out DIFFERENT, which
// is why they are not treated alike below.
//
// `trunc_i32` CLEARED. Zero divergences from the assembly over 51k inputs at PC=53 and at PC=64, and
// the out-of-range edge this file used to warn about agrees BY VALUE: INT_MAX+1, +/-1e30, +/-inf and
// NaN all give 0x80000000 through `fistp` AND through the cast. MSVC compiles the cast to a call to
// __ftol2_sse, not to an inline `fistp` -- two different code paths reaching the same result.
//
// `trunc_i32_mul` DID NOT CLEAR, and the way that was found is worth more than the result. An offline
// probe -- both arms as static functions in one TU, libmh's own flags -- reported ZERO divergences
// and MSVC kept the product in ST(0) (`fmul ST(1),ST(0)`). Compiled AS THIS HEADER, against the real
// call sites, MSVC instead spills the product to a 64-bit temp, and `fptest` reported 1743
// divergences at PC=64 on the first run. The probe's codegen was not representative of the header's.
// So the original note on this helper was RIGHT -- "C++ is not obliged to keep an intermediate there"
// -- and the assembly stays. `fptest` case M pins the refusal so nobody repeats the probe and
// concludes otherwise.
//
// TWO MORE SHAPES THAT DID NOT CLEAR, same experiment. Do not generalise from `trunc_i32` to the 29
// blocks still outside this file:
//   * THE QWORD WIDTH is PC-DEPENDENT. `fild qword` loads an int64 EXACTLY into the 80-bit register;
//     C++'s static_cast<double>(int64) must round to 53 bits. At PC=53 they agree; at PC=64 they
//     differ for every |n| > 2^53 (measured: n=9007199254740993 gives ...93 through the assembly and
//     ...92 through the cast). sim_weapon_damage_calc and ai_mine_yield use this shape.
//   * THE `fcompp`/`fnstsw` COMPARES ARE NOT `x < y`. On a NaN the x87 sets C3,C2,C0 = 1,1,1, so a C0
//     test reads UNORDERED AS LESS-THAN while C++'s `<` is false. The game tests C0, so that IS the
//     behaviour to keep, and the correct C++ spelling is `!(x >= y)` -- verified over 4001 ordered
//     pairs and all five NaN placements, 0 mismatches, where `x < y` is wrong on all five.
//
// ---- STEP 2 CONTINUED, 2026-09-11 (CRT-X87-CPP): THE FIVE COMPARES, 3 CLEARED AND 2 REFUSED -------
//
// The five `fcompp`/`fnstsw` helpers in x87_shapes.h were taken one at a time, because they do NOT
// share a flag: `setnc`/CF, JBE/CF|ZF, JA/!(CF|ZF), JNC/raw-C0 and a second raw-C0 read. The flag
// half came out as predicted above -- a raw C0 read is `!(x >= y)` and a JA/JBE arm is a plain
// strict, ordered `>` -- and it was NOT the deciding question. THE ARITHMETIC WAS.
//
//   CLEARED  x87_housing_short, x87_labor_utilization, x87_yield_below_spend_rate. 0 divergences at
//            PC=53 AND PC=64 (fptest C3/C4/C5), so these are NOT PC-conditional. What they have in
//            common is that no 80-bit intermediate can reach the comparison: two divide int32 by
//            int32 and compare against a FLOAT (whose 24-bit significand is far too coarse to sit in
//            the ~2^-64 band where 53 and 64 bits disagree), and the third rounds its quotient to a
//            double in the ORIGINAL, before either compare.
//   REFUSED  soldier_strip_should_continue, x87_capacity_short -- both at BOTH precision settings,
//            and the cheapest counter-example is ordinary: capacity=1, holdings=0.1, threshold=10.0f,
//            where 1/0.1 kept at 64 bits is just below 10 and rounded to a double is exactly 10.
//
// AND A THIRD AXIS THIS FILE HAD NOT NAMED: EXPONENT RANGE. PC sets the SIGNIFICAND; the x87 register
// keeps a 15-bit exponent at every setting and no control-word field narrows it. So an intermediate
// that leaves double range stays finite in the register where a C++ double overflows to +-inf -- the
// PC=53 half of both refusals above is exactly that, and NO precision guarantee and no C++ spelling
// can fix it. Any future candidate whose intermediate can exceed double range is refused on sight.
//
#pragma once
#include <cfloat> // _controlfp_s -- the PC=53 install + reader below
#include <cstdint>

namespace mh::fp {

// ---- THE PC=53 GUARANTEE (CRT-X87-CPP step 3; user's ABI ruling 2026-09-11) ----------------------
//
// Eleven helpers below and in x87_shapes.h are C++ because the x87 runs at PC=53, and THREE of them
// are wrong at PC=64 -- `trunc_mul` on 1743 of 4254 swept inputs. So PC=53 stopped being an
// observation and became an ABI guarantee, and a guarantee has to be INSTALLED rather than inherited.
//
// WHY INHERITING WAS NEVER SAFE, measured rather than supposed. The strategic sim was read at PC=53
// on every path -- with the harness pin on, with `pin_fpu=0`, and on both peers across 8000 steps --
// and a full disassembly of mh.exe explains why nothing contradicts it: the image contains NO
// instruction that installs a control word. Every FLDCW in it takes a stack-slot operand (save then
// restore), `_control87` has zero callers, and Watcom's `__init_80x87` -- the one routine that would
// FINIT the word to PC=64 -- has zero callers too, i.e. it is dead code in this build. The 53 is
// supplied by the HOST TOOLCHAIN (mh.dll's own CRT init / the Win32 thread default), which is exactly
// why libmh cannot rely on it: a standalone host built with a different toolchain, or on a different
// OS, supplies whatever it likes. Hence install, don't inherit.
//
// HOSTED IS UNTOUCHED, deliberately: inside mh.exe the process already runs at 53 (measured), and
// the hosted arm must not change behaviour for a guarantee it already satisfies.

// Install the guarantee. Sets ONLY the precision-control field, leaving rounding and the exception
// masks as the host left them -- libmh guarantees a precision, not a whole FP environment. Returns
// 1 on success, 0 if the control word could not be set.
// FP-CPP-PROVEN: fptest P -- starts the probe at PC=64 so only a working install can pass, then
//   checks idempotence and that the ROUNDING mode survives.
inline int install_pc53() {
    unsigned cur = 0;
    return _controlfp_s(&cur, _PC_53, _MCW_PC) == 0 ? 1 : 0;
}

// ---- THE RAW WORDS, for comparing one arm against another ---------------------------------------
//
// pc_in_force() below answers the GUARANTEE's question ("is precision 53?") through the CRT's
// abstracted view. These two answer a different and blunter one: what do the hardware registers
// literally hold. That matters when two arms of the same replay are being compared, because the
// guarantee only covers the precision FIELD -- rounding mode, the exception masks, and the whole of
// MXCSR are left as the host left them, and "left as the host left them" is not the same sentence in
// mh.exe as it is in a standalone console process. A double-rounding difference from a PC or RC
// mismatch is bit-identical for hundreds of steps and then flips one marginal comparison, so the
// only useful form of this check is the raw word printed from BOTH arms and diffed.
//
// MXCSR is included even though the original is an x87 binary: OUR translated code is MSVC x86,
// whose default /arch:SSE2 puts double arithmetic in SSE registers, so both control words are live
// in a promoted run and only one of them is covered by the PC=53 guarantee.
// FP-ASM-KEEP: NEVER -- there is no arithmetic here to prove equal to anything. This READS a control
// register; `fnstcw` is the only way to obtain the x87 control word and its value is the thing under
// examination, so there is no C++ spelling to converge on and no sweep that could ever convert it.
inline unsigned short raw_x87_cw() {
    unsigned short w = 0;
    __asm { fnstcw w }
    return w;
}

// FP-ASM-KEEP: NEVER -- same statement for MXCSR, and it is `stmxcsr` rather than the `_mm_getcsr`
// intrinsic DELIBERATELY. The intrinsic is a perfectly good C++ spelling, which is exactly the
// problem: as a non-asm body this helper would be required to carry FP-CPP-PROVEN naming an fptest
// case, and there is no case to name because there is no arithmetic to sweep. Marking it ASM-KEEP on
// a body that genuinely cannot be anything else is honest; inventing a proof case for a register read
// would be decorating the gate rather than satisfying it.
inline unsigned raw_mxcsr() {
    unsigned v = 0;
    __asm { stmxcsr v }
    return v;
}

// Read the precision control actually in force: 53, 64, 24, or 0 if unreadable. This is what makes
// the guarantee TESTABLE rather than asserted -- fptest case P sets PC=64, installs, and requires
// this to come back 53, so an install that silently did nothing fails the suite instead of shipping.
// FP-CPP-PROVEN: fptest P -- the reader case P uses to hold the install to its claim.
inline int pc_in_force() {
    unsigned cur = 0;
    if (_controlfp_s(&cur, 0, 0) != 0) return 0; // mask 0 is a read, it writes nothing
    switch (cur & _MCW_PC) {
        case _PC_53: return 53;
        case _PC_64: return 64;
        case _PC_24: return 24;
        default: return 0;
    }
}

// Truncate a double toward zero and store as int32. THE dominant idiom -- 26 call sites.
// Original shape: `fld qword / <cw dance> / fistp dword`, kept verbatim in fp_x87_selftest.cpp as
// that suite's reference arm, so this body cannot drift away from what it replaced.
// Round-toward-zero is what a C++ integral cast is defined to do; the out-of-range behaviour was
// MEASURED to agree rather than assumed -- see the step-2 note above.
// FP-CPP-PROVEN: fptest S1 -- 0 divergences over 51k inputs at both precision settings, and the
//   out-of-range / non-finite edge agrees BY VALUE.
inline int32_t trunc_i32(double value) { return static_cast<int32_t>(value); }

// THE SECOND PROVEN SINK (CRT-X87-CPP step 3, fptest case Q). Several helpers end `fistp qword` and
// then take only the LOW DWORD of the 64-bit result -- a different sink from `fistp dword`, and one
// a C++ `static_cast<int32_t>` does NOT reproduce: out of int32 range the 32-bit store gives the
// 32-bit integer indefinite 0x80000000, where the 64-bit store gives the 64-BIT indefinite
// 0x8000000000000000 whose low dword is 0. So the narrowing has to go through int64 first, and that
// is what this is. Out of int64 range MSVC's conversion yields the same 0x8000000000000000 the
// hardware stores, which is why the two agree rather than merely looking similar -- measured, not
// assumed: case Q sweeps it against a verbatim `fistp qword` at both precision settings, including
// every non-finite and both out-of-range directions.
// FP-CPP-PROVEN: fptest Q -- the `fistp qword` + low-dword sink, proven distinct from trunc_i32 out
//   of int32 range.
inline int32_t trunc_i64_low32(double value) {
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(static_cast<int64_t>(value))));
}

// (int32 * double) truncated toward zero -- 5 call sites (refund_amount, decay_amount,
// mine_extract_rate_trunc, trunc_axis_delta).
//
// STILL ASSEMBLY, and deliberately. `static_cast<int32_t>(a * b)` was tried and REFUSED: compiled as
// part of this header MSVC spills the product to a 64-bit temp, which at PC=64 changes the answer on
// 1743 of 44k swept inputs. The canonical case is k * (1/k), exactly halfway below 1.0 -- rounded to
// 53 bits it becomes 1.0 and truncates to 1; kept at 64 bits it stays below and truncates to 0.
// `fptest` case M holds that refusal so it is not re-litigated from a non-representative probe.
// FP-ASM-KEEP: fptest M -- REFUSED. Compiled as part of this header MSVC spills the product to a
//   64-bit temp, changing 1743 of 44k swept answers at PC=64. 80-bit retention is a per-function
//   INLINING decision, not a property of the source.
inline int32_t trunc_i32_mul(int32_t a, double b) {
    int32_t  r        = 0;
    uint16_t cw_save  = 0;
    uint16_t cw_trunc = 0;
    // clang-format off  (one instruction per line; clang-format would fold the __asm block)
    __asm {
        fild    dword ptr [a]
        fmul    qword ptr [b]
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

} // namespace mh::fp
