//
// ai/ai_construction_plan.h -- the AI's per-tick build-order decision (RI-AI, batch C layer 5).
//
// llm_strat_ai_plan_construction @0x004e5ae0, `void __watcall f(uint player)`, player in EAX. Called
// from llm_strat_ai_player_tick when player_data::ai_phase_flags bit 0x1 is set. It is the top-level
// orchestrator this whole planner family (turret/mine/shortage/housing/site-scan) hangs off of, and
// unlike its callees it never touches player_data through a raw offset ANYWHERE -- every field it
// reads or writes already has a name and a type (ai_build_plan_len_and_flag, ai_build_plan_cursor,
// ai_build_plan[], ai_bldg_queue_count, ai_build_candidate_{primary,secondary,shortage},
// ai_resource_shortage_state, ai_resource_need_{score,threshold}, ai_building_type_available[]).
//
// THE SHAPE, three parts in the original's own order:
//
//   1. REBUILD ai_building_type_available[0..cfg_building_sec->total] from scratch (0 = not
//      researched), then walk the cfg INVENTION table (rows 1..cfg_progress_sec->total) and, for
//      every row whose `.type == BUILDING` that this player has `available`, stamp
//      ai_building_type_available[row.index] to 1 (researched, not yet built) or 2 (researched AND
//      already built, per-player `.f3`). BOTH LOOP BOUNDS ARE INCLUSIVE (`JBE`, not `JC`) and the
//      invention scan starts at row 1, not 0 -- reproduced verbatim, not "fixed" to exclusive/half-open.
//
//   2. gc.rebalance_building_workers(player) and gc.plan_turret_upgrade(player) ALWAYS run, regardless
//      of which branch follows. The former's return is a DECISION ("housing is short"), stashed and
//      read again in part 3.
//
//   3. ONE GATE PICKS BETWEEN TWO ENTIRELY DIFFERENT BODIES, and it is the SAME condition Ghidra
//      renders both ways depending on which side you read it from:
//        (ai_build_plan_len_and_flag & 0x7fffffff) <= ai_build_plan_cursor   [UNSIGNED, JBE]
//      TRUE  (the scripted opening plan has been fully consumed) -> LIVE PRIORITY LOGIC: try the
//            secondary/primary/shortage build candidates, react to a resource shortage, top up unit
//            housing, then scan for expansion sites (one full-map call in category 4, or -- when the
//            plan's own high bit 0x80000000 is set -- a category-0 probe and, only if THAT placed
//            nothing, categories 1-3 followed by clearing the high bit). Returns unconditionally at
//            the end of this arm.
//      FALSE (still mid-plan) -> ADVANCE THE PRECOMPUTED PLAN: bail if the AI's own build queue is
//            non-empty, bail if the next plan entry's type is not yet marked available (== 1, not
//            just non-zero -- an already-built type, marked 2, is skipped exactly like an
//            unresearched one), otherwise queue it and advance the cursor by one entry per call.
//
// See the .cpp for the per-line assembly citations; this header carries only the shape and the two
// declared needs the translation surfaced (an unbound view member and eight uncalled ai_calls slots).
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// llm_strat_ai_plan_construction @0x004e5ae0.
void plan_construction(const ai_view &v, const ai_store &own, const ai_calls &gc, uint32_t player);

// llm_strat_ai_calc_power_supply_ratio @0x004e3926 (RI-AI batch B, antichain layer 3, 2026-08-06).
// Sole caller is plan_construction above (the `power_ratio < *v.energy_ratio` gate on the secondary
// build candidate). Returns the player's POWER SUPPLY / DEMAND ratio as a double: (sum of
// Building[].electric_power over the player's owned buildings whose cfg type is A_PLANT/H_PLANT/
// A_MOTHER/H_MOTHER -- the four generator types, tested UNCONDITIONALLY, not gated by is_alien_race)
// divided by (the same sum over every other owned building). This is the POWER economy (generated
// resource: plants produce, buildings consume), NOT the ENERGY/HP stat -- see the .cpp for the
// evidence chain (cfg field name, the four-type gate, the AI.SCR notes) that settled the
// ENERGY-vs-POWER trap (docs/conventions.md#energy-is-not-power) for this function's own held rename, back on 2026-08-06 (AI-READY).
//
// Edge cases, in order: both sums zero -> 0.0; consumers zero but generators nonzero -> 1.1 (a fixed
// sentinel meaning "plenty", not a computed value); otherwise the true quotient.
double calc_power_supply_ratio(const ai_view &v, int32_t player);

} // namespace detail

void   plan_construction(uint32_t player);
double calc_power_supply_ratio(int32_t player);

} // namespace mh::ai
