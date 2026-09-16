//
// ai/ai_spend_rate.cpp -- see ai_spend_rate.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_resource_spend_rate_update_004e7bb5.asm), not from Ghidra's C.
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h, with the player stride 166140 = 0x288fc
// built four separate times by SHL 2 / ADD / SHL 7 / SUB / SHL 2 / SHL 6 / ADD:
//   0xe7df64 = player_data + 0x100a4 = ai_spend_ring_cursor
//   0xe7df68 = player_data + 0x100a8 = ai_spend_rate_denom_ring[0]
//   0xe7e168 = player_data + 0x102a8 = ai_spend_rate_numer_ring[0]
//   0xe7e368 = player_data + 0x104a8 = ai_spend_rate_numer
//   0xe7e36c = player_data + 0x104ac = ai_spend_rate_denom
//   0xe7e370 = player_data + 0x104b0 = ai_spend_rate            (a double: two dword stores at
//              0x004e7c6f/0x004e7c79 on the zero-denominator path, one FSTP at 0x004e7caf otherwise)
//   0x66f438 = _G_LLM_STRAT_AI_RESOURCE_SPEND_WEIGHTS, int[4], read through the region registry
// so no byte offset and no literal VA appears below (Law 1).
//
// WHICH RING FEEDS WHICH ACCUMULATOR is the one thing here that is easy to get backwards, so it is
// read off the pairs rather than from the names: 0x004e7c24 multiplies the weight by
// [EBX + 0xe7e168] (the NUMER ring) and 0x004e7c2b adds it into [EAX + 0xe7e368] (ai_spend_rate_
// numer); 0x004e7c37 multiplies by [EBX + 0xe7df68] (the DENOM ring) and 0x004e7c3e adds into
// [EAX + 0xe7e36c]. Both terms use the SAME weight, loaded twice from the same slot
// (0x004e7c1e and 0x004e7c31).
//
// THE ACCUMULATION IS 32-BIT AND WRAPS. `IMUL ESI,[..]` / `ADD [..],ESI` are plain 32-bit signed
// ops with no saturation, so a large history overflows exactly as the original does -- which is
// also why the FILD path below has to treat the result as UNSIGNED (see the header).
//
// THE TAIL IS A SHARED WATCOM EPILOGUE: `ADD ESP,0x8 / JMP 0x004e2d4b` @0x004e7ce4 is `return`,
// not a call.
//
#include "ai/ai_spend_rate.h"

#include "fp/x87_shapes.h" // CRT-X87: the hoisted x87 blocks (the asm moved, it did not change)

namespace mh::ai {
namespace detail {

double x87_ratio(uint32_t numer, uint32_t denom) {
    return ::mh::fp::x87_ratio(numer, denom);
}

spend_rate_report resource_spend_rate_update(const ai_view &v, const ai_store &own, int32_t player) {
    spend_rate_report rep{};

    const player_data &pd = v.players[player];
    player_data       &wp = own.players[player];

    // 0x004e7bdc / 0x004e7be7 -- both accumulators cleared before the sum.
    wp.ai_spend_rate_numer = 0;
    wp.ai_spend_rate_denom = 0;

    for (int32_t slot = 0; slot < SPEND_RING_SLOTS; ++slot) {
        for (int32_t r = 0; r < SPEND_RING_RESOURCES; ++r) {
            const int32_t weight = v.spend_weights[r];
            const int32_t cell   = slot * SPEND_RING_RESOURCES + r;
            const int32_t num_in = pd.ai_spend_rate_numer_ring[cell];
            const int32_t den_in = pd.ai_spend_rate_denom_ring[cell];
            wp.ai_spend_rate_numer += weight * num_in;
            wp.ai_spend_rate_denom += weight * den_in;
            if (num_in != 0 || den_in != 0) ++rep.nonzero_terms;
        }
    }
    rep.numer = pd.ai_spend_rate_numer;
    rep.denom = pd.ai_spend_rate_denom;

    // 0x004e7c66: `CMP dword ptr [..],0x0 / JNZ` on the DENOMINATOR only.
    if (pd.ai_spend_rate_denom == 0) {
        rep.denom_is_zero = true;
        wp.ai_spend_rate  = SPEND_RATE_NO_DENOM;
    } else {
        wp.ai_spend_rate = x87_ratio((uint32_t)pd.ai_spend_rate_numer,
                                     (uint32_t)pd.ai_spend_rate_denom);
    }

    // 0x004e7ccb-0x004e7cde: INC, then `CMP ..,0x20 / JC` -- an UNSIGNED compare, so the reset also
    // fires on a cursor that somehow held a negative value.
    ++wp.ai_spend_ring_cursor;
    if ((uint32_t)pd.ai_spend_ring_cursor >= (uint32_t)SPEND_RING_SLOTS) {
        wp.ai_spend_ring_cursor = 0;
        rep.wrapped             = true;
    }
    rep.cursor = pd.ai_spend_ring_cursor;
    return rep;
}

} // namespace detail

void resource_spend_rate_update(int32_t player) {
    const ai_state st = state();
    (void)detail::resource_spend_rate_update(st.read, st.own, player);
}

// ---- the differential-oracle arm ----------------------------------------------------------------
//
// Nothing is stubbed and nothing extra is declared: the body makes no outward call and every store
// is inside player_data, the measured region. The weights array is read-only.
//
// WHAT A VACUOUS GREEN LOOKS LIKE HERE, and it is the DEFAULT state early in a match: both rings
// are zero, so the sum is zero, the denominator is zero and every call takes the sentinel branch --
// which writes a compile-time constant and would agree with almost any wrong translation. The
// division, the unsigned widening and the weighting are then all unexercised. So the arm counts
// `divided` (calls that reached the x87 quotient) and `terms_max` (the most non-zero ring cells any
// call summed), and BOTH must be non-zero before this site is read as covering anything but the
// sentinel path. Wrap coverage is `wrapped`; it fires once every 32 calls by construction.

} // namespace mh::ai
