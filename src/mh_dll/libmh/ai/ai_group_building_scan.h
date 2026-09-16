//
// ai/ai_group_building_scan.h -- two building-roster scans by up-to-4 requested types
// (RI-AI batch C layer 3, 2026-08-06).
//
// Both functions share the exact COUNT-DRIVEN roster-walk shape documented in
// ai_turret_threat.cpp / ai_nearest_flagged.h: `remaining` starts at
// buildings[player][0].index (read as the ushort bit pattern, zero-extended), the walk runs
// building_index from 1 while remaining != 0, an EMPTY slot (building_id == 0) advances the index
// without consuming the budget, and a live-but-non-matching slot DOES consume it. Neither has a
// capacity check of its own against the table it feeds -- see the "no cap" notes below, the same
// discipline as every other AI scratch producer in this module (ai_state.h's ENGAGE_SCRATCH_CAP /
// ATTACK_SCRATCH_CAP comments).
//
// Both were previously ai_calls members (`gc.group_find_nearest_building_of_types` /
// `gc.group_collect_buildings_of_types`, still declared in ai_state.h for any caller not yet
// updated to call these detail:: bodies directly) called from llm_strat_ai_group_task_attack_
// random_target's closure -- see ai_group_task_attack.cpp/.h, which is presumed-pure/"REAL in the
// shadow arm" for both. This translation is what makes that presumption checkable.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game and no rig. The wrappers below are this applied to state(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_group_find_nearest_building_of_types @0x004e98a7 (279 B).
//
// Scans `player_id`'s building roster for the alive building whose building_id equals one of the
// up to 4 requested types, nearest to (query_x, query_y) by toroidal distance, and returns its
// roster index -- 0 if none match (the accumulator's own init value, never overwritten on an empty
// result; not a sentinel the original tests for separately).
//
// The nearest-so-far test is STRICT (`new_dist < best_dist`, unsigned) -- a tie keeps the FIRST
// candidate found, matching find_nearest_flagged_building's tie rule. `best_dist` seeds at
// UINT32_MAX so the first alive+matching candidate always wins the first comparison.
//
// toroidal_dist_sq's argument order is (building.x, building.y, query_x, query_y) -- read off the
// four PUSHes at 0x004e9959-0x004e9994 in program order (query_y, query_x, building.y,
// building.x), which for __cdecl's right-to-left push puts the LAST push (building.x) in the
// FIRST argument slot.
int32_t group_find_nearest_building_of_types(const ai_view &v, const ai_calls &gc,
                                             int32_t player_id, int32_t query_x, int32_t query_y,
                                             uint32_t building_type_1, uint32_t building_type_2,
                                             uint32_t building_type_3, uint32_t building_type_4);

// llm_strat_ai_group_collect_buildings_of_types @0x004e99be (181 B).
//
// Same roster walk as its sibling above, but instead of tracking a nearest match it APPENDS every
// alive building matching one of the 4 requested types to
// _G_LLM_STRAT_AI_BUILDING_CANDIDATE_SCRATCH_LIST[_G_LLM_STRAT_AI_BUILDING_CANDIDATE_SCRATCH_COUNT++]
// (0x004e9a54-0x004e9a62). RESET-THEN-FILL discipline like every other AI scratch here -- the
// caller (llm_strat_ai_group_task_attack_random_target) zeroes the count before its four collect
// calls, this function only ever increments it. NO CAPACITY CHECK against the list's declared
// [256] extent (ai_state.h) -- reproduced, not fixed, per the brief's rule 14.
//
// DECLARED NEED: `own.building_candidate_scratch_list` does not exist yet. ai_store currently
// exposes only the writable COUNT (`building_candidate_scratch_count`); the LIST itself
// (`ai_view::building_candidate_scratch_list`) is const-only, so this function cannot write an
// entry into it through the state view as it stands. See the .cpp for the exact call and this
// translation's declared_needs.
void group_collect_buildings_of_types(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                      int32_t player_id, uint32_t unused_edx_slot,
                                      uint32_t unused_ebx_slot, uint32_t building_type_1,
                                      uint32_t building_type_2, uint32_t building_type_3,
                                      uint32_t building_type_4);

} // namespace detail

int32_t group_find_nearest_building_of_types(int32_t player_id, int32_t query_x, int32_t query_y,
                                             uint32_t building_type_1, uint32_t building_type_2,
                                             uint32_t building_type_3, uint32_t building_type_4);

void group_collect_buildings_of_types(int32_t player_id, uint32_t unused_edx_slot,
                                      uint32_t unused_ebx_slot, uint32_t building_type_1,
                                      uint32_t building_type_2, uint32_t building_type_3,
                                      uint32_t building_type_4);

} // namespace mh::ai
