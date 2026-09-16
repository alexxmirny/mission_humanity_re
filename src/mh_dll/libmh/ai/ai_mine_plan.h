//
// ai/ai_mine_plan.h -- the AI's mine-construction planner (RI-AI / AI1B, batch B layer 3).
//
// One of the planners llm_strat_ai_plan_construction runs in sequence. It picks a mine building
// TYPE from two cached candidate slots, refreshes the per-resource yield estimate, and decides
// whether to put one more mine in the build queue.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// The x87 half of the per-resource shortage test, kept as the original's own instructions for the
// reason ai_shortage_gate.h states at length: the quotient is never rounded to a double in the
// original (FDIV @0x004e53eb feeds FCOMPP @0x004e5405 straight out of the register), so computing
// it in SSE2 rounds once more than the game does.
//
// Returns 1 when yield is BELOW the spend rate -- the original's `JNC` @0x004e540a skips the
// `MOV EAX,1` when C0 is clear, so the shortage arm is exactly "C0 set", and unordered (C0 set)
// counts as short.
int32_t x87_yield_below_spend_rate(uint32_t spend_total, const float *ai_clock, uint32_t yield);

// llm_strat_ai_plan_mine_construction @0x004e52c4. Seven steps, in the original's order:
//
// 1. TYPE SELECTION (0x004e52f4-0x004e533f). tier1 then tier2, each accepted only if it is != -1
//    AND player_data::ai_building_type_available[it] == 1. tier2 is tested SECOND and overwrites,
//    so it wins when both are available -- which matches its field comment ("takes priority").
//    Still -1 -> return.
// 2. THE SCRATCH GLOBAL (0x004e535e-0x004e5396). base is the sum of the four ai_score_cat_0x1i
//    BUILT+QUEUED CATEGORY COUNTS (not stockpiles -- the old plate said stockpiles).
//    _G_LLM_STRAT_AI_MINE_REBALANCE_MIN_COUNT ends at 2*base+1 and a stack local at 3*base.
//    THE ORDER IS LOAD-BEARING: llm_strat_ai_mine_portfolio_rebalance reads that global at
//    0x004e35ab, so the store must happen before the call in step 3, not after.
// 3. llm_strat_ai_mine_portfolio_rebalance(player, &ai_mine_yield_by_resource[0]) -- an OUT-POINTER
//    into player_data, which is why no instruction in the image appears to write that array.
// 4. llm_strat_ai_bldg_type_already_queued(player, type) != 0 -> return. (The old plate called this
//    an affordability check against "FUN_004d3593"; there is no such call and 0x004d3593 is not a
//    function entry -- ghidra_findings 2026-08-02-2250-3.)
// 5. PER-RESOURCE SHORTAGE FLAGS for d in 1..4 (0x004e53d5-0x004e544d), summed into slot 0 of the
//    same five-int stack array. A flag is set when yield[d] < spend_total[d] / ai_clock, and FORCED
//    to 1 when yield[d] == 0 -- so a resource with no mine at all always counts as short, whatever
//    the rate says.
// 6. TWO GATES, both UNSIGNED (0x004e546d-0x004e54aa):
//      ai_score_bldg_type_b <= 2*base+1  ->  force the accumulator to 1
//      accumulator == 0                  ->  return
//      ai_score_bldg_type_b >= 3*base    ->  return
//    The old plate had only the first. ai_score_bldg_type_b is the player's owned-MINE count
//    (count_by_type + queue-pending), written by llm_strat_ai_score_build_categories.
// 7. llm_strat_bldg_queue_construction(player, type, -1, 0) then
//    llm_strat_ai_queue_rotate_newest_to_front(player).
//
// THE TWO DEAD STORES IN STEP 2 ARE REPRODUCED, NOT ELIDED. The global is written base, then
// 2*base, then incremented; the first two values are never read by anything, because the only
// reader between them is nothing at all. They are kept because the cost is two stores and the
// alternative is a translation that quietly disagrees with the listing an auditor is holding.
//
// AND ONE TEST IS GENUINELY UNREACHABLE. 0x004e53ae re-tests `type == -1` after the rebalance call,
// but nothing writes the register holding it between there and the identical test at 0x004e533f
// that already returned. It is noted at its site rather than translated, because writing it would
// suggest rebalance can change the answer, which it cannot.
void plan_mine_construction(const ai_view &v, const ai_store &own, const ai_calls &gc,
                            int32_t player);

} // namespace detail

void plan_mine_construction(int32_t player);

} // namespace mh::ai
