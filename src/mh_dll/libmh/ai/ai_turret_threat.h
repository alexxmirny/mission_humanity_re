//
// ai/ai_turret_threat.h -- the turret-threat grid rescan (RI-AI / AI1A layer 1).
//
// One function: the consumer for player_data::ai_turret_rescan_pending. The two notification hooks
// (llm_strat_ai_notify_bldg_constructed / _object_removed) set the latch whenever an enemy turret is
// built or destroyed near a player; this is what actually re-derives that player's
// ai_tile_flags_grid "an enemy turret can reach this tile" bitmap from scratch every time it fires.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game and no rig. The wrapper below is this applied to state(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_turret_threat_rescan @0x004d8526.
//
// Clears `player`'s own ai_turret_rescan_pending latch and wipes `player`'s own threat grid, then
// walks every OTHER active player's building roster looking for a live enemy turret with a mounted
// weapon and re-stamps a threat ring at that turret's range into `player`'s grid.
//
// THREE DIFFERENT PLAYER INDICES are in play and they are NOT all the same one:
//   - the grid wiped/re-stamped is `player`'s (the ticking player, the function argument);
//   - the roster scanned is the OTHER player's, the outer loop variable;
//   - the `range_max[...]` subscript is that SAME OTHER (scanned/turret-owning) player, not
//     `player` -- see the comment at the call site below. Wrong in a way that is invisible with
//     only two players in the game, since the scanned player is never `player` itself.
void turret_threat_rescan(const ai_view &v, const ai_store &own, const ai_calls &gc,
                          uint32_t player);

} // namespace detail

void turret_threat_rescan(uint32_t player);

} // namespace mh::ai
