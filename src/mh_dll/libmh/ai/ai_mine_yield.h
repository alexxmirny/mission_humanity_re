//
// ai/ai_mine_yield.h -- how much would a mine of this type, on this tile, extract? (RI-AI / AI1B
// layer 5).
//
// llm_strat_ai_calc_mine_yield_estimate @0x004e30e9. An OUT-POINTER helper with five parameters
// (EAX/EDX/EBX/ECX + one stack slot, `RET 0x4`): a cfg BUILDING TYPE id, a FINE tile coordinate
// pair, an `int *out_yield` and a `llm_strat_ai_mine_quality *out_quality`. Its sole caller is
// llm_strat_ai_mine_portfolio_rebalance, which aims the two pointers at row `i` of
// _G_LLM_STRAT_AI_MINE_YIELD_ESTIMATE and _G_LLM_STRAT_AI_MINE_QUALITY_HIST.
//
// WHAT IT DOES. For each resource id the building TYPE can extract, it walks a fixed 25-cell 5x5
// kernel over the COARSE resource plane (map::resources[64][64], quarter resolution) centred on the
// mine's own coarse cell, NEAREST FIRST, and stops at the first cell holding that resource. The
// estimate is that cell's kernel weight times the type's extract_val for the resource; the quality
// histogram counts how many resources were found at ring 0 / ring 1 / ring 2.
//
// FIVE THINGS A TRANSLATOR GETS WRONG WITHOUT BEING TOLD, all re-derived from the listing:
//
//  1. out_yield[0] IS NOT ZEROED. The init loop at 0x004e3162 runs r = 1..4 (`MOV ESI,1` @0x004e3157
//     then `CMP ESI,4 / JBE`), and the total pass at 0x004e329f only ADDs into it. Slot 0 belongs to
//     the CALLER -- the rebalance resets and re-sums it itself. Zeroing it here diverges on the very
//     first call.
//  2. THE EXTRACT TABLE IS A TERMINATED LIST, not a fixed four. The walk at 0x004e317d breaks out to
//     0x004e31b0 on the first zero extract_id (`CMP dword ptr [..+0xd9f21d],0 / JZ` @0x004e318a), so
//     a type with {2, 0, 3, 0} extracts resource 2 and nothing else.
//  3. THE FIRST HIT WINS AND ENDS THAT RESOURCE'S SCAN. Both arms after the store at 0x004e323b jump
//     to 0x004e3263, which is the OUTER loop's increment -- so `best_weight` can never actually
//     accumulate a maximum and is always still 0.0f when the FCOMP reads it.
//  4. THE FCOMP AT 0x004e3242 IS `JNC` ON C0, so an UNORDERED compare STORES. Written below as
//     `if (!(best >= weight)) best = weight;`, which gives the same answer for NaN. It is dead as
//     written (see 3) and is emitted because it is the instruction, not because anything reaches it.
//  5. THE THREE BUCKET TESTS COMPARE FLOAT BIT PATTERNS AS SIGNED INTS (`CMP dword ptr [..],
//     0x3f7d70a4 / JL` @0x004e32a1, and 0x3eff7cee / 0x3dcac083 at 0x004e32b6 / 0x004e32cc). For the
//     non-negative values this array can hold that is identical to a float compare, and it is
//     reproduced literally so the equivalence never has to be re-argued.
//
// THE ARITHMETIC IS x87 AND THE EQUIVALENCE IS DATA-DEPENDENT. The product is built as
// `FILD qword` (a ZERO-extended 32-bit extract_val -- the high dword is an explicit 0 store at
// 0x004e3218) * `FMUL double` (the kernel weight), truncated toward zero by utils_math_trunc
// @0x004d0596, then `FISTP qword` of which only the LOW dword is kept. With the shipped weights
// {1.0, 0.5, 0.1} no integer extract_val separates 80-bit from 64-bit intermediate precision, so an
// SSE2 `(int64)trunc((double)a * w)` would agree TODAY -- but that is a property of the data, not of
// the code, so the __asm form below reproduces the instructions instead (same choice, same reason,
// as ai_mine_plan.cpp's x87_yield_below_spend_rate).
//
// utils_math_trunc, dumped 2026-08-05: FSTCW, set the CONTROL WORD's HIGH BYTE to 0x1f (RC = 11,
// round toward zero; PC = 11, extended), FRNDINT, restore. The FISTP that follows therefore runs
// under the ORIGINAL rounding mode on an already-integral value.
//
// TWO UNCHECKED THINGS, BOTH THE ORIGINAL'S:
//   * the coarse wrap moduli are `map_width / 4` and `map_height / 4` (SIGNED /4 spelled
//     SAR/SHL/SBB/SAR at 0x004e3107-0x004e312f) and are used as UNSIGNED DIVISORS at 0x004e31e0 /
//     0x004e31f4. A map narrower than 4 tiles divides by zero. No shipped map is.
//   * `extract_present[extract_id]` / `extract_val[extract_id]` at 0x004e3199 / 0x004e31a6 index two
//     eight-dword STACK arrays with a raw cfg value. An id above 7 writes into the function's own
//     adjacent locals. That cannot be expressed in C++ and is not attempted: the sibling structure
//     map::resources caps a resource id at 7 by its own `short value[8]`, so an id past that is a
//     corrupt cfg. The write is skipped and counted in the report instead of being faked.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The three ring thresholds, as the RAW FLOAT BIT PATTERNS the original compares against (see 5).
// Numerically 0.99, 0.4989845 and 0.099 -- the shipped kernel weights 1.0 / 0.5 / 0.1 minus an
// epsilon, so each test is "was the nearest hit on this ring or closer".
inline constexpr int32_t MINE_RING0_BITS = 0x3f7d70a4;
inline constexpr int32_t MINE_RING1_BITS = 0x3eff7cee;
inline constexpr int32_t MINE_RING2_BITS = 0x3dcac083;

// The extract tables' index domain. The two stack arrays are eight dwords wide
// ([ESP+0x00..0x1f] and [ESP+0x20..0x3f]); only 1..4 are ever initialised or read.
inline constexpr int32_t MINE_EXTRACT_SLOTS = 8;

namespace detail {

struct mine_yield_report {
    int32_t extract_slots  = 0; // cfg extract entries walked before the terminating zero id (0..4)
    int32_t resources_hit  = 0; // resource ids whose kernel scan found a deposit
    int32_t bad_extract_id = 0; // cfg extract ids outside 0..7 -- the un-expressible overrun above
};

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. Takes no call set: apart from the inert Watcom stack probe the body's only
// CALL is the x87 truncation helper, which is reproduced inline.
mine_yield_report calc_mine_yield_estimate(const ai_view &v, int32_t building_id, uint32_t tile_x,
                                           uint32_t tile_y, int32_t *out_yield,
                                           mine_quality *out_quality);

// `trunc(extract_val * weight)` exactly as 0x004e3220-0x004e322f builds it, returning the low dword
// of the FISTP qword. Exposed for the offline test, which pins the {1.0, 0.5, 0.1} cases.
int32_t x87_scale_and_trunc(uint32_t extract_val, const double *weight);

} // namespace detail

void calc_mine_yield_estimate(int32_t building_id, uint32_t tile_x, uint32_t tile_y,
                              int32_t *out_yield, int32_t *out_quality);

} // namespace mh::ai
