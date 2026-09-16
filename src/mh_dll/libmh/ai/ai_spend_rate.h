//
// ai/ai_spend_rate.h -- recompute the AI's weighted spend-rate ratio (RI-AI / AI1B layer 2).
//
// llm_strat_ai_resource_spend_rate_update @0x004e7bb5. Three parts, no outward call:
//
//   1. Zero ai_spend_rate_numer / ai_spend_rate_denom, then sum both rings over ALL 32 slots and
//      the 4 basic resources, each term weighted by _G_LLM_STRAT_AI_RESOURCE_SPEND_WEIGHTS[r].
//      The rings are int[128] typed as one array and used as int[32][4] -- 32 history slots, 4
//      resources -- which is why the index below is `slot * 4 + r` and the original's address
//      arithmetic is `slot << 4` plus `r << 2`.
//   2. ai_spend_rate = numer / denom as a DOUBLE, or the constant 1e13 when denom is zero.
//   3. Advance ai_spend_ring_cursor, wrapping at 32.
//
// NOTE WHAT PART 3 DOES NOT DO: it advances the write cursor but does NOT clear the slot it moves
// to. Whoever writes the rings (not this function -- nothing here stores into either ring) inherits
// the previous lap's values until it overwrites them, and part 1 sums all 32 slots regardless of
// how many have ever been written. So the ratio is a 32-lap moving aggregate, and on a fresh base
// it is a sum over mostly-zero history rather than over "the slots filled so far".
//
// THE ZERO-DENOMINATOR CONSTANT IS 1e13, and it is stored as raw dwords by the original
// (MOV .. ,0xe5400000 @0x004e7c6f and MOV .. ,0x42a2309c @0x004e7c79 -- the low and high halves of
// the IEEE-754 double 0x42a2309ce5400000). It is a "no spending yet, so the ratio is effectively
// infinite" sentinel, not a computed value; consumers compare against it rather than dividing by it.
//
// THE DIVISION IS x87, AND THIS MODULE REPRODUCES IT AS x87. The original does
// FILD qword / FILD qword / FDIVP / FSTP double: each operand is built as a 64-bit integer whose
// LOW dword is the (unsigned) accumulator and whose HIGH dword is an explicit zero
// (0x004e7c8b-0x004e7caa), so a negative int32 accumulator becomes a large POSITIVE int64 -- and
// then divided on the x87 stack and rounded once on the store. A plain C++ `(double)a / (double)b`
// is compiled to SSE2 by MSVC/x86 and rounds the quotient DIRECTLY to 53 bits, whereas the x87
// divide rounds to whatever the game's runtime precision-control setting is and the FSTP rounds
// again -- a double-rounding difference of one ULP, on inputs where the exact quotient sits near a
// tie. That would be a real byte difference in a region the shadow oracle compares, and it would be
// rare enough to look like a flake. Rather than measure the game's control word (which the rig
// cannot report and which a later build could change), the division below is the same three x87
// instructions in inline asm, so it is exact by construction whatever the control word says.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

inline constexpr int32_t SPEND_RING_SLOTS     = 32; // the ring depth, and the cursor's wrap point
inline constexpr int32_t SPEND_RING_RESOURCES = 4;  // resources per slot -- ids 1..4 in ring order
// The ratio written when the denominator sums to zero. Raw dwords in the original; see the header.
inline constexpr double SPEND_RATE_NO_DENOM = 1e13;

namespace detail {

struct spend_rate_report {
    int32_t numer         = 0; // ai_spend_rate_numer after the sum
    int32_t denom         = 0; // ai_spend_rate_denom after the sum
    bool    denom_is_zero = false;
    int32_t cursor        = 0; // ai_spend_ring_cursor after the advance
    bool    wrapped       = false;
    int32_t nonzero_terms = 0; // ring cells that contributed anything -- the anti-vacuity number
};

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game. Takes no call set: the body makes no outward call.
spend_rate_report resource_spend_rate_update(const ai_view &v, const ai_store &own, int32_t player);

// The original's exact quotient: both operands zero-extended to 64-bit, divided and stored on the
// x87 stack. Exposed so a unit test can pin it directly.
double x87_ratio(uint32_t numer, uint32_t denom);

} // namespace detail

void resource_spend_rate_update(int32_t player);

} // namespace mh::ai
