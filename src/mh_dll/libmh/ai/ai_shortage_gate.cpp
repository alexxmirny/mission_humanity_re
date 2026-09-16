//
// ai/ai_shortage_gate.cpp -- see ai_shortage_gate.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_storage_capacity_short_and_cap_check_004e3872.asm). Ghidra's .c for this
// body renders the divisor as `*(int *)(&player_resources + uVar1 * 4 + player * 0x28)` and the
// zero-flush as a `(ulonglong)local_18 & 0x7fffffff00000000` mask over a variable it has already
// typed `double` -- readable, but it never names the numerator's array at all, which is the one
// thing the old plate got wrong.
//
#include "ai/ai_shortage_gate.h"

#include "fp/x87_shapes.h" // CRT-X87: the hoisted x87 blocks (the asm moved, it did not change)

namespace mh::ai {
namespace detail {

int32_t x87_capacity_short(int32_t capacity, double holdings, const float *threshold) {
    return ::mh::fp::x87_capacity_short(capacity, holdings, threshold);
}

int32_t storage_capacity_short_and_cap_check(const ai_view &v, const ai_calls &gc, int32_t player) {
    const player_data &pd = v.players[player];

    // MOV EDX,1 @0x004e3887 / CMP EDX,5 + JC @0x004e3913 -- ids 1..4 inclusive, id 0 skipped.
    for (int32_t d = RESOURCE_ID_FIRST; d <= RESOURCE_ID_LAST; ++d) {
        // FILD dword @0x004e389e is a SIGNED 32-bit load (contrast plan_mine_construction, which
        // uses FILD *qword* over a zero-extended dword). FSTP double @0x004e38a5 then puts it in the
        // stack temporary the divide reads.
        double holdings =
            (double)v.player_resources[player * RESOURCE_SLOTS_PER_PLAYER + d];
        // 0x004e38a8-0x004e38be: the bit test is (high & 0x7fffffff) == 0 && low == 0, i.e. "the
        // double is +0.0 or -0.0". `== 0.0` is true for exactly that pair, and FILD of a 32-bit
        // integer produces -0.0 for no input at all, so the two are equivalent here twice over.
        if (holdings == 0.0) holdings = 1.0;

        if (x87_capacity_short(v.storage[player].cap_prev[d], holdings, v.silo_ratio)) {
            // 0x004e38e1-0x004e3910. UNSIGNED (CMP + SETC), and it RETURNS -- no later resource is
            // examined.
            const uint32_t built =
                (uint32_t)gc.bldg_count_by_id(player, pd.ai_build_candidate_shortage);
            return built < (uint32_t)pd.ai_score_bldg_type_b ? 1 : 0;
        }
    }
    // XOR EAX,EAX @0x004e391c.
    return 0;
}

} // namespace detail

int32_t storage_capacity_short_and_cap_check(int32_t player) {
    const ai_state st = state();
    return detail::storage_capacity_short_and_cap_check(st.read, live_calls(), player);
}

// ---- the differential-oracle arm ----------------------------------------------------------------
//
// THIS SITE'S VERDICT IS THE RETURN VALUE AND NOTHING ELSE, which is unusual enough to state rather
// than leave a reader to infer from an empty region list. The function writes no tracked state
// (tmp/state_matrix.json, regenerated at EN v205, gives it four cells and all four are READS:
// player_resources, _G_LLM_STRAT_STORAGE_STATS, player_data and _G_LLM_STRAT_AI_SILO_RATIO), and
// its one callee llm_strat_bldg_count_by_id writes nothing either. So the generated site declares
// zero regions, snapshots zero bytes, and the whole comparison rides on `r_ours == r_orig`.
//
// That is a REAL oracle, not a vacuous one -- the return is a 0/1 that encodes both the x87 verdict
// and the unsigned count compare -- but it is a narrower one than a region-comparing site, and the
// anti-vacuity rule for it is different: `N call(s), 0 divergence(s)` here means "N returns agreed",
// which is only interesting once at least one call took the SHORTAGE arm. The trace line prints the
// per-resource capacity/holdings pair for exactly that reason; a run in which every call fell out of
// the loop has compared four subtractions' worth of nothing.

} // namespace mh::ai
