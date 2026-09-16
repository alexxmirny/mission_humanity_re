//
// ai/ai_mine_rebalance.h -- recompute the AI's mine portfolio, and retire at most one mine
// (RI-AI / AI1B layer 4).
//
// llm_strat_ai_mine_portfolio_rebalance @0x004e3418, `void __watcall (uint player, int *out_yield)`.
// Its sole caller is llm_strat_ai_plan_mine_construction, which passes
// `&player_data[player].ai_mine_yield_by_resource[0]` (LEA [EAX + 0x10054] over a player_data + 0x10
// base, @0x004e53a9). That is why an instruction sweep of the image finds NO writer for that array
// -- it is filled through a register-held pointer -- and it is a known trap. The array's
// consumer is llm_strat_ai_resource_site_meets_threshold's veto, already reimplemented and T1, so
// this is the producer half of that pair.
//
// THREE PASSES, and the second and third are mutually exclusive.
//
// PASS 1 -- COLLECT (0x004e347e-0x004e3574). Walks the buildings roster from slot 1 while a BUDGET
// counter, seeded from `buildings[player][0].index` (MOVZX word @0x004e346e), is still positive. An
// EMPTY slot (building_id == 0) does NOT consume the budget; every non-empty one does. For each
// slot whose cfg Building type is 2 or 0x16 -- the alien and human mine, BLDG_TYPE_A_MINE /
// BLDG_TYPE_H_MINE -- it records the roster index, calls calc_mine_yield_estimate on the building's
// own tile into row `n` of the two scratch tables, and adds that row's [1..4] into out_yield[1..4].
// out_yield[0..4] were ALL zeroed on entry (0x004e3433, r = 0..4 -- unlike the estimator's, this
// init loop does include slot 0).
//
// If nothing was collected the function returns having written only those five zeros (0x004e357e).
//
// PASS 2 -- RETIRE THE UNDERPERFORMERS (0x004e35fe-0x004e372b). Two gates first:
//   * the LIVE mine count must be >= _G_LLM_STRAT_AI_MINE_REBALANCE_MIN_COUNT, UNSIGNED
//     (CMP + JC @0x004e35ab). That global is an ARGUMENT CHANNEL, not config: the caller stores
//     2*(the four ai_score_cat counters)+1 into it immediately before calling. So a shadow arm of
//     THIS function reads whatever the caller left, and the caller's own site is coupled to it.
//   * no collected mine may already be in building state 0x6b (CMP word + JZ @0x004e35e9). Note the
//     numeric coincidence with the ORDER kind that bldg_order_restart_construction_enqueue issues,
//     also 0x6b; suggestive of "a restart is already in flight", but nothing in this listing proves
//     that correspondence and it is not relied on.
// Then, per collected mine, five KEEP tests -- and the mine is retired only if ALL FIVE fail:
//     1. summed percentage share >= _G_LLM_STRAT_AI_MINE_LOW_SHARE_PCT_THRESHOLD (image 10)
//     2. 10*near + 5*mid + far >= 15   (the quality histogram)
//     3..5. 2 * this mine's estimate >= the portfolio total, for EACH of resources 1..4
// Retiring means: enqueue the restart-construction order, subtract the mine's [1..4] back out of
// out_yield, flush the pending unit-training entries, and clear the roster-index slot.
//
// PASS 3 -- RETIRE ONE MINE THAT MAKES NOTHING SCARCE (0x004e3755-0x004e384d). Runs ONLY if pass 2
// retired nothing. Re-weights the four resource totals by 12/4, 12/3, 12/3, 12/1, and for the first
// resource whose weighted total is under about an eighth of the weighted grand total
// (`w[b] * 8 < w[0] + 1`, UNSIGNED) retires the SMALLEST-total surviving mine that produces NONE of
// it -- then breaks out entirely. At most one mine is retired per call by either pass.
//
// EVERY EXIT PATH re-sums out_yield[0] = out_yield[1..4] (0x004e3853), including the two early ones.
//
// FOUR THINGS THAT ARE THE ORIGINAL'S AND ARE REPRODUCED RATHER THAN FIXED:
//   * NOTHING BOUNDS the collect counter against MINE_SCRATCH_ROWS (32). The walk is driven by the
//     roster count, which is 100, so a player with more than 32 mines writes past all three tables
//     and into whatever follows -- the three are contiguous (128 + 384 + 1024 bytes ending exactly
//     at _G_LLM_STRAT_AI_EXPAND_SITE_COUNT @0x0101370c). The translation reproduces that verbatim by
//     indexing the live pointers rather than a bounded array, because a clamp would diverge on
//     exactly the state that triggers it; the report COUNTS the rows past 32 so a run that reaches
//     it is visible instead of silent. Untested territory either way: no scenario in the save index
//     has a player with 33 mines.
//   * THE LOOP AT 0x004e359e-0x004e35a6 HAS AN EMPTY BODY (`XOR EBX,EBX` / `CMP EBX,4` / `JBE` onto
//     its own `INC EBX`). It is dead and is not translated as anything.
//   * The state-0x6b gate walks 0 .. LIVE-1 (CMP ESI,[EBP-0x20] @0x004e35f9) while the retire pass
//     walks 0 .. COLLECTED-1 (@0x004e3728). The two counters are equal at that point because nothing
//     has been retired yet, so the difference is invisible -- but it is what the listing says.
//   * `MOVZX EAX, word ptr [EBP-0x54]` at 0x004e36e1 / 0x004e3804 truncates the player to 16 bits
//     for the enqueue call, while the flush call two instructions later takes the full dword.
//   * THE COLLECT WALK HAS NO SLOT BOUND, only the budget. If the roster's slot-0 `index` is nonzero
//     while every slot is empty the walk never decrements and runs off the end of the roster row
//     forever. The original hangs identically -- the loop head at 0x004e3570 tests only the budget --
//     so this is faithful rather than a new hazard, but a shadow arm reaches it FIRST.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The quality-histogram score gate, `CMP EDX,0xf / JNC` @0x004e3694. UNSIGNED.
inline constexpr uint32_t MINE_QUALITY_SCORE_KEEP_MIN = 15;
// The histogram's ring multipliers, 0x004e3665-0x004e3687: score = 10*near + 5*mid + 1*far.
inline constexpr int32_t MINE_QUALITY_NEAR_MUL = 10;
inline constexpr int32_t MINE_QUALITY_MID_MUL  = 5;
// map::object::building.state that the gate at 0x004e35e9 refuses to rebalance around.
inline constexpr uint16_t BLDG_STATE_RESTART_PENDING = 0x6b;
// Pass 3's per-resource re-weighting, `x * 12 / d`: 0x004e3755-0x004e377e. Resource 1 divides by 4
// with a SHR (unsigned), resources 2 and 3 by 3 with an unsigned DIV, resource 4 not at all.
inline constexpr uint32_t MINE_REWEIGHT_MUL   = 12;
inline constexpr uint32_t MINE_REWEIGHT_DIV[] = {0, 4, 3, 3, 1};
// "under about an eighth of the weighted total": `w[b] * 8 < w[0] + 1`, UNSIGNED (0x004e37a0).
inline constexpr uint32_t MINE_SCARCE_SHIFT = 3;
// The victim search's initial best-total sentinel, `MOV ECX,0x5f5e100` @0x004e37af.
inline constexpr uint32_t MINE_VICTIM_TOTAL_SENTINEL = 100000000u;

namespace detail {

struct mine_rebalance_report {
    int32_t collected       = 0; // mines the collect pass recorded
    int32_t live            = 0; // still-owned count as the retire pass started
    int32_t min_count       = 0; // the argument-channel threshold, as read
    int32_t retired_pass2   = 0;
    int32_t retired_pass3   = 0;
    int32_t overrun_rows    = 0; // collect slots past MINE_SCRATCH_ROWS -- see the header
    int32_t scarce_res      = 0; // pass 3's under-weighted resource id, 0 = none was under threshold
    uint8_t hit_state_gate  = 0; // a collected mine was already in state 0x6b -> early out
    uint8_t below_min_count = 0; // live < min_count -> early out
    uint8_t reached_pass3   = 0;
};

// The logic over an EXPLICIT state and an INJECTED call set, so `net_selftest.exe aitest` can drive
// it over heap buffers with no game and no rig.
mine_rebalance_report mine_portfolio_rebalance(const ai_view &v, const ai_store &own,
                                               const ai_calls &gc, uint32_t player,
                                               int32_t *out_yield);

} // namespace detail

void mine_portfolio_rebalance(uint32_t player, int32_t *out_yield);

} // namespace mh::ai
