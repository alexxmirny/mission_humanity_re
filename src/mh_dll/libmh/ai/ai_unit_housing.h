//
// ai/ai_unit_housing.h -- the per-tick unit-housing top-up (RI-AI / AI1B, antichain layer 3).
//
// One function: for each of the AI's four unit classes (soldiers/vehicles/planes/helis), queues one
// more housing building whenever the class's live population has outgrown the built+queued housing
// score by the "one more per 50 units" heuristic. Called from llm_strat_ai_plan_construction right
// after llm_strat_ai_react_resource_shortage (per the Ghidra plate).
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game and no rig. The wrapper below is this applied to state(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_maintain_unit_housing @0x004e58d1.
//
// Reads _G_LLM_STRAT_UNIT_HOUSING_STATS[player].used_{soldiers,vehicles,planes,helis} and, for each
// class, computes `needed = used / 50 + 1` (unsigned division) and compares it UNSIGNED against the
// matching player_data::ai_score_cat_0x2{0,1,2,3} counter -- queueing one housing building only when
// `needed > score`. The four (used, score, candidate) triples are, in the order the original reads
// them:
//   used_soldiers  vs ai_score_cat_0x20  -> players[player+1].ai_housing_candidate_soldier
//   used_vehicles  vs ai_score_cat_0x21  -> players[player+1].ai_housing_candidate_vehicle
//   used_planes    vs ai_score_cat_0x23  -> players[player+1].ai_housing_candidate_plane
//   used_helis     vs ai_score_cat_0x22  -> players[player+1].ai_housing_candidate_heli
// THE CANDIDATE IDS LIVE IN THE NEXT PLAYER'S RECORD, DELIBERATELY -- the same documented +1
// aliasing llm_strat_ai_init_build_candidate_priorities writes (see ai_build.h/.cpp); this function
// is one of the seven readers that precedent already accounts for.
//
// The four gates differ and are NOT symmetric:
//   soldiers -- count_by_id(player, ai_resource_shortage_candidates[0]) != 0
//   vehicles -- (ai_score_cat_0x11 || _0x12 || _0x13) && ai_score_cat_0x20 != 0
//   planes   -- side_has_aircraft_producer(player) != 0 && ai_score_cat_0x21 != 0
//   helis    -- bldg_has_heli_unit(player) != 0 && ai_score_cat_0x21 != 0 && ai_score_cat_0x23 != 0
// and each block additionally skips if bldg_type_already_queued(player, candidate) is true.
//
// NO LATENT PLAYER-ID BUG (withdrawn 2026-08-02). The original's tail on the heli branch is
// `JMP llm_strat_bldg_queue_construction_thunk`, whose first instruction is `MOV EAX,ESI`, and ESI
// holds this function's own `player` (latched at entry, never rewritten, callee-saved across every
// intervening call) -- NOT a hardcoded 0. Translated inline below as an ordinary
// `queue_construction(player, heli_candidate, -1, 0); return;` rather than a call through the
// (uncallable, shared-epilogue) thunk.
void maintain_unit_housing(const ai_view &v, const ai_store &own, const ai_calls &gc, int32_t player);

// llm_strat_ai_bldg_has_heli_unit @0x004d8a9d (RI-AI batch B, 2026-08-07). The gate the heli branch
// above reads. Walks the player's ACTIVE buildings (roster index starting at 1, per the
// disassembly's `MOV ESI,0x1`; the count sentinel is buildings[player][0].index, NOT decremented on
// a zero-type slot, hence the do/while(true) shape with the count test at the top) and, for every
// building whose type id is nonzero, scans EVERY cfg unit type 1..cfg_unit_sec->total (inclusive,
// unsigned JBE) for one whose cfg Building.unit_quant[unit_type] > 0.0 AND whose
// cfg_units[unit_type].ai_unit == 6 (HELI, confirmed via the disassembly's own `CMP dword ...,0x6`
// @0x004d8b5d, not inferred from the enum name). Returns 1 the instant a match is found anywhere;
// 0 if the roster runs out first.
int32_t bldg_has_heli_unit(const ai_view &v, int32_t player);

} // namespace detail

void    maintain_unit_housing(int32_t player);
int32_t bldg_has_heli_unit(int32_t player);

} // namespace mh::ai
