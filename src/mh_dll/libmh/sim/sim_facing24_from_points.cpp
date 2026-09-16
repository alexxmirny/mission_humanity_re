//
// sim/sim_facing24_from_points.cpp -- see sim_facing24_from_points.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_facing24_from_points_004946d2.asm), not from Ghidra's .c: the
// draft's overall shape and its `float10`/`ROUND` framing of the tail (Ghidra's rendering of the
// inlined utils_math_trunc + the caller's own trailing FISTP) read correctly as a description, but
// every operand, every jcc polarity, and the constant-pool values below were independently re-walked
// against the raw FLD/FADD/FMUL/FDIV/FCOMP/JBE/JC targets per the translator brief.
//
#include "sim/sim_facing24_from_points.h"

#include "addr/mh_calls.gen.h" // MH_CRT(llm_math_atan) -- the one outward call this TU binds live
#include "fp/x87_shapes.h"     // CRT-X87: the hoisted x87 blocks (the asm moved, it did not change)
#include "crt/crt_select.h"    // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const facing24_from_points_calls &live_facing24_from_points_calls() {
    static const facing24_from_points_calls c = {
        MH_CRT(llm_math_atan),
    };
    return c;
}

namespace {

// The eight-double constant pool at 0x005014fc-0x00501534 -- see the header banner for why these are
// plain constexpr doubles rather than a sim_view member, and for the PI-exactness caveat carried into
// uncertainties[].
inline constexpr double RAD2DEG_NUM = 180.0; // _G_LLM_STRAT_FACING24_RAD2DEG_NUM   (0x005014fc)
// CONDUCTOR-CONFIRMED (2026-08-11) via ReVA read-memory: bytes `E0 86 44 54 FB 21 09 40` (LE) at
// 0x00501504 decode to 3.1415926536 EXACTLY -- a truncated 11-significant-digit approximation of pi,
// NOT bit-identical to the standard double pi (3.14159265358979323846, which differs by ~1.02e-11).
// The translator's uncertainty about this was well-founded; using textbook pi here would be a real,
// if usually-invisible, divergence from the original at sector-boundary inputs. Written as the exact
// decimal literal the game's own constant pool holds (verified bit-identical when parsed as a
// double), not as a "close enough" approximation.
inline constexpr double RAD2DEG_DEN    = 3.1415926536; // _..._RAD2DEG_DEN (0x00501504) -- game's own truncated pi, NOT std pi
inline constexpr double HALF_TURN_DEG  = 180.0;        // _G_LLM_STRAT_FACING24_HALF_TURN_DEG (0x0050150c)
inline constexpr double BIAS_DEG       = 7.0;          // _G_LLM_STRAT_FACING24_BIAS_DEG      (0x00501514)
inline constexpr double WRAP_ADD_DEG   = 360.0;        // _G_LLM_STRAT_FACING24_WRAP_ADD_DEG  (0x0050151c)
inline constexpr double WRAP_LIMIT_DEG = 360.0;        // _G_LLM_STRAT_FACING24_WRAP_LIMIT_DEG (0x00501524)
inline constexpr double WRAP_SUB_DEG   = -360.0;       // _G_LLM_STRAT_FACING24_WRAP_SUB_DEG  (0x0050152c)
inline constexpr double SECTOR_DEG     = 15.0;         // _G_LLM_STRAT_FACING24_SECTOR_DEG    (0x00501534)

// 0x00494791-0x004947a6: (angle_deg / SECTOR_DEG) + 1.0, truncated toward zero via the same
// utils_math_trunc control-word swap every other translated site inlines (see the header banner),
// then stored with a 32-bit FISTP whose caller-restored (default) control word is a no-op here
// because FRNDINT already left an exact integer value in ST0.
//
// UNLIKE the sibling precedents (refund_amount/x87_scale_and_trunc/weapon_power_add_and_trunc), the
// value handed to trunc is not a fresh FILD/FMUL/FADD of two operands -- it is angle_deg divided by a
// constant and incremented by 1.0, computed ENTIRELY in the x87 register with NO intermediate 64-bit
// store between the division and the trunc call (no FSTP between FADDP at 0x0049479c and CALL at
// 0x0049479e). Written as one hand-written __asm block, matching that instruction sequence exactly,
// rather than as C++ `angle_deg / SECTOR_DEG + 1.0` (which /fp:precise x87 codegen would likely also
// keep in-register, but "likely" is not the same guarantee an explicit transcription gives -- see
// uncertainties[]).
int32_t sector_index_from_angle(double angle_deg) {
    return ::mh::fp::trunc_div_plus1(angle_deg, SECTOR_DEG);
}

} // namespace

namespace detail {

uint8_t facing24_from_points(char from_x, char from_y, char to_x, char to_y,
                             const facing24_from_points_calls &c) {
    // 0x004946f3-0x00494716: both deltas as SIGNED int subtraction (MOVSX byte->int), each then
    // converted to double via an explicit FILD/FSTP round-trip (a real store-to-64-bit-double step,
    // matching a plain C++ `(double)` cast of an exact int value -- no precision loss possible for
    // values this small).
    const double dx = (double)((int)from_x - (int)to_x); // 0x004946f3-0x00494703
    const double dy = (double)((int)to_y - (int)from_y); // 0x00494706-0x00494716

    // 0x00494719-0x0049472a: atan(dx / dy), called through the indirection (see header) rather than
    // MH_CRT(llm_math_atan) directly, so net_selftest.exe simtest can drive this offline. dx/dy is
    // UNGUARDED for dy == 0.0 here exactly as the original leaves it (IEEE-754 division yields
    // +-infinity or NaN, which atan/the rest of the chain propagate rather than trap on) -- not
    // fixed.
    double angle_deg = c.atan(dx / dy);

    // 0x0049472d-0x0049473c: radians -> degrees as ONE combined expression (matches the original's
    // FLD/FMUL/FDIV/FSTP with no intermediate store between the multiply and the divide).
    angle_deg = (angle_deg * RAD2DEG_NUM) / RAD2DEG_DEN;

    // 0x0049473f-0x00494755: FCOMP 0.0 vs dy; JBE (0.0<=dy) skips -- so the correction applies when
    // dy < 0.0, matching atan's [-90,90] domain needing a half-turn flip for the other two quadrants.
    if (dy < 0.0)                              // 0x00494747
        angle_deg = angle_deg + HALF_TURN_DEG; // 0x00494749-0x00494752

    // 0x00494755-0x0049475e: unconditional pre-quantization bias.
    angle_deg = angle_deg + BIAS_DEG;

    // 0x00494761-0x00494777: FCOMP 0.0 vs angle_deg; JBE (0.0<=angle_deg) skips -- so the wrap-up
    // applies when angle_deg < 0.0.
    if (angle_deg < 0.0)                      // 0x00494769
        angle_deg = angle_deg + WRAP_ADD_DEG; // 0x0049476b-0x00494774

    // 0x00494777-0x00494791: FCOMP angle_deg vs WRAP_LIMIT_DEG; JC (angle_deg<limit) skips -- so the
    // wrap-down (WRAP_SUB_DEG is -360.0, i.e. a subtraction) applies when angle_deg >= WRAP_LIMIT_DEG.
    if (angle_deg >= WRAP_LIMIT_DEG)          // 0x00494783
        angle_deg = angle_deg + WRAP_SUB_DEG; // 0x00494785-0x0049478e

    // 0x00494791-0x004947a6: see sector_index_from_angle -- 1..24 for any angle already wrapped into
    // [0, 360).
    const int32_t sector = sector_index_from_angle(angle_deg);

    // 0x004947a6-0x004947b8: four redundant byte-to-byte MOV round trips through separate stack
    // temporaries (a Ghidra/-Od-style artifact of re-materialising the same byte, not additional
    // logic) -- transcribed as one narrowing cast.
    return (uint8_t)sector;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

uint8_t facing24_from_points(char from_x, char from_y, char to_x, char to_y) {
    return detail::facing24_from_points(from_x, from_y, to_x, to_y, live_facing24_from_points_calls());
}


} // namespace mh::sim
