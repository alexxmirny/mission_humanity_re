//
// ai/ai_shortage_state.h -- resource-shortage state recompute (RI-AI batch B, 2026-08-06).
//
// One function: llm_strat_ai_player_tick's step right after the opponent-relations update.
// Refreshes `player_idx`'s own ai_resource_need_score, then drains+tests a per-opponent
// pending-event flag array to decide an immediate escalation to state 3; failing that, it lazily
// refreshes every currently-non-AI (ai_enabled == 0) active player's own score cache, picks the
// strongest-assessed opponent (by total_unit_power) among the other active players, and derives
// state 2 (strong enemy threat vs the player's own economy/military cache), state 1 (need score at
// or above threshold), or 0. The result feeds llm_strat_ai_plan_construction /
// _react_resource_shortage, which only react to a shortage when the state is 0.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game and no rig. The wrapper below is this applied to state(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_recompute_shortage_state @0x004e853f.
//
// THREE DISTINCT ARRAYS OF player_idx's OWN ROW ARE IN PLAY, and none of them is any other
// player's row:
//   - ai_intel_flags[0..7]      -- per-OTHER-PLAYER pending-event bits, drained (zeroed) here;
//   - ai_opponent_assessments[0..7] -- per-OTHER-PLAYER military/economic cache, indexed by the
//     OTHER player's id for the "who is the strongest opponent" scan;
//   - ai_opponent_assessments[player_idx] -- THE SAME ARRAY, but read at player_idx's OWN index,
//     i.e. player_idx used as both the row selector and the assessment-slot index. This is a
//     cached "my own economy/military strength" scratch, NOT an actual opponent's data (confirmed
//     instruction-by-instruction at 0x004e86ac-0x004e86e3 -- the self-slot base is the normal
//     per-player stride PLUS a separate player_idx*sizeof(assessment) added in, landing back on
//     this same array with the outer index reused as the inner one).
void recompute_shortage_state(const ai_view &v, const ai_store &own, const ai_calls &gc,
                              uint32_t player_idx);

} // namespace detail

void recompute_shortage_state(uint32_t player_idx);

} // namespace mh::ai
