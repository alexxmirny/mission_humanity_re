//
// ai/ai_mine_plan.cpp -- see ai_mine_plan.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_plan_mine_construction_004e52c4.asm).
//
#include "ai/ai_mine_plan.h"

#include "fp/x87_shapes.h" // CRT-X87: the hoisted x87 blocks (the asm moved, it did not change)

namespace mh::ai {
namespace detail {

int32_t x87_yield_below_spend_rate(uint32_t spend_total, const float *ai_clock, uint32_t yield) {
    return ::mh::fp::x87_yield_below_spend_rate(spend_total, ai_clock, yield);
}

void plan_mine_construction(const ai_view &v, const ai_store &own, const ai_calls &gc,
                            int32_t player) {
    const player_data &pd = v.players[player];

    // ---- 1. type selection, 0x004e52f4-0x004e533f ----
    int32_t       type = -1;
    const int32_t t1   = pd.ai_mine_candidate_tier1;
    if (t1 != -1 && pd.ai_building_type_available[t1] == 1) type = t1;
    const int32_t t2 = pd.ai_mine_candidate_tier2;
    if (t2 != -1 && pd.ai_building_type_available[t2] == 1) type = t2;
    if (type == -1) return; // JZ 0x004e792e (the shared Watcom epilogue) @0x004e5342

    // ---- 2. the scratch global, 0x004e535e-0x004e5396 ----
    const uint32_t base = (uint32_t)pd.ai_score_cat_0x10 + (uint32_t)pd.ai_score_cat_0x11 +
                          (uint32_t)pd.ai_score_cat_0x12 + (uint32_t)pd.ai_score_cat_0x13;
    *own.mine_rebalance_min_count = (int32_t)base;          // @0x004e537a -- overwritten below
    *own.mine_rebalance_min_count = (int32_t)(base + base); // @0x004e5386 -- also overwritten
    ++*own.mine_rebalance_min_count;                        // @0x004e5396 -> 2*base + 1
    // The stack local takes `base` @0x004e5380 and then `base*4 - base` @0x004e538e-0x004e5391;
    // only the second value is ever read (@0x004e54a7).
    const uint32_t queue_ceiling = base * 3u;

    // ---- 3. the yield refresh, 0x004e539c-0x004e53a9 ----
    // LEA EDX,[EAX + 0x10054] over a base of player_data + 0x10 -- i.e. the address of
    // player_data[player].ai_mine_yield_by_resource[0].
    gc.mine_portfolio_rebalance((uint32_t)player, &own.players[player].ai_mine_yield_by_resource[0]);

    // 0x004e53ae re-tests `type == -1` here. Nothing writes EDI between the test at 0x004e533f that
    // already returned and this one, so it can never be taken -- see the header.

    // ---- 4. the already-queued gate, 0x004e53bb ----
    if (gc.bldg_type_already_queued(player, (uint32_t)type) != 0) return;

    // ---- 5. per-resource shortage flags, 0x004e53c8-0x004e544d ----
    // Five ints at [EBP-0x40]: slot 0 is the accumulator (seeded from the zero already_queued
    // returned, @0x004e53c8), slots 1..4 are the per-resource flags.
    int32_t slot[RESOURCE_ID_LAST + 1] = {0};
    for (int32_t d = RESOURCE_ID_FIRST; d <= RESOURCE_ID_LAST; ++d) { // CMP EBX,4 + JBE @0x004e5464
        slot[d] = x87_yield_below_spend_rate((uint32_t)pd.resource_spend_total[d], &pd.ai_clock,
                                             (uint32_t)pd.ai_mine_yield_by_resource[d]);
        // @0x004e5434: a resource with a ZERO yield estimate is short regardless of the rate. This
        // re-reads the array the rebalance call above just filled, which is why `pd` is a reference
        // into live memory and not a copy.
        if (pd.ai_mine_yield_by_resource[d] == 0) slot[d] = 1;
        slot[0] += slot[d]; // ADD [EBP-0x40],EAX @0x004e544a
    }

    // ---- 6. the two gates, 0x004e546d-0x004e54aa. Both UNSIGNED. ----
    // CMP EAX,[0x01013040] + JA @0x004e5479 -- read BACK through the view, as the original does,
    // rather than reusing the value written in step 2.
    if (!((uint32_t)pd.ai_score_bldg_type_b > (uint32_t)*v.mine_rebalance_min_count)) slot[0] = 1;
    if (slot[0] == 0) return;                                       // @0x004e5486
    if ((uint32_t)pd.ai_score_bldg_type_b >= queue_ceiling) return; // CMP + JNC @0x004e54aa

    // ---- 7. queue it and pull it to the front, 0x004e54bb-0x004e54c2 ----
    gc.bldg_queue_construction(player, type, -1, 0);
    gc.queue_rotate_newest_to_front(player);
}

} // namespace detail

void plan_mine_construction(int32_t player) {
    const ai_state st = state();
    detail::plan_mine_construction(st.read, st.own, live_calls(), player);
}


} // namespace mh::ai
