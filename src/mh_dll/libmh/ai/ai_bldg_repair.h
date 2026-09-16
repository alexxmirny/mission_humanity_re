//
// ai/ai_bldg_repair.h -- the per-player repair/upgrade scanner (RI-AI / AI1A, antichain layer 2).
//
// One function: walks a single player's building roster looking for ONE candidate per call to
// either (re)queue a repair or queue an upgrade. Called right after llm_strat_ai_plan_construction
// from llm_strat_ai_player_tick (per the Ghidra plate comment on the original).
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game and no rig. The wrapper below is this applied to state(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_scan_bldg_repair_upgrade @0x004e5e04.
//
// For every occupied slot in buildings[player][1 .. ), COUNT-DRIVEN (see the roster-walk note in
// the .cpp): first decides whether the building needs repair (energy below repair_ratio * its
// config max, OR strictly below max AND it is the player's mother building), and if so -- when the
// building is alive and not in a busy state (construction/charge-step/dismantling/upgrading) --
// either refreshes an existing pending repair queue entry (removing it if the building died) or
// queues a new one. If repair was NOT queued this call (not damaged, not alive, or busy), instead
// checks upgrade eligibility (Building[].upgrade_index != 0 AND the corresponding tech flag in the
// player's untyped upgrade-researched byte array is set) and, symmetrically, queues or refreshes an
// upgrade entry. At most one queue mutation happens per occupied building per call.
void scan_bldg_repair_upgrade(const ai_view &v, const ai_store &own, const ai_calls &gc,
                              int32_t player);

} // namespace detail

void scan_bldg_repair_upgrade(int32_t player);

} // namespace mh::ai
