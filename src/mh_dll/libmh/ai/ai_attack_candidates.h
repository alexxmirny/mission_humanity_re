//
// ai/ai_attack_candidates.h -- the AI's per-tick attack-candidate scratch writer (RI-AI / AI1A,
// batch A layer 2).
//
// One function: the sole producer of _G_LLM_STRAT_AI_ATTACK_CANDIDATES /
// _G_LLM_STRAT_AI_ATTACK_CANDIDATE_COUNT (mh_llm_strat_ai_attack_candidate, ai_state.h's
// `attack_candidate` / ATTACK_SCRATCH_CAP). Same reset-then-fill transient discipline as the engage
// scratch (ai_state.h comment on ATTACK_SCRATCH_CAP): callers reset the count, this appends with no
// cap check at all, and llm_strat_ai_active_unit_tick is the consumer that fills in `score` /
// `dist_sq` itself.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game and no rig. The wrapper below is this applied to state(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_attack_candidate_add @0x004ec7d9.
//
// Appends one record to the attack-candidate scratch at the current count (NO cap check -- see
// ATTACK_SCRATCH_CAP), then increments the count last:
//   unit_index      = (uint16_t)unit_index
//   weapon_range_sq = gc.unit_max_weapon_range(player, unit_index), SQUARED. The original stores
//                     the raw range into this same slot first and immediately overwrites it with
//                     the square (mh_structs.gen.h's field comment) -- the raw-range store is dead;
//                     only the square is observable, so only the square is written here.
//   x, y            = units[player][unit_index].x / .y (map::object::unit, byte fields),
//                     zero-extended to the record's uint16_t columns.
// `score` and `dist_sq` are DELIBERATELY left untouched -- llm_strat_ai_active_unit_tick, the sole
// caller, initialises them itself; writing either here would be a divergence (brief rule 11).
//
// unit_max_weapon_range's first argument is `player`, not a unit reference, despite the ai_calls
// parameter being named `unit_ref` -- read straight off the push order (EAX still holds this
// function's own `player` argument at the call site) and consistent with the identical pattern in
// ai_engage_scan.cpp's unit_scan_engage_candidates_in_range.
void attack_candidate_add(const ai_view &v, const ai_store &own, const ai_calls &gc, int32_t player,
                          int32_t unit_index);

} // namespace detail

void attack_candidate_add(int32_t player, int32_t unit_index);

} // namespace mh::ai
