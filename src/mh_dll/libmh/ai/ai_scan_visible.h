//
// ai/ai_scan_visible.h -- the AI's visible-enemy target scan + its scan-target-list sort
// (RI-AI / AI1A layer 2).
//
// Two functions, unrelated except that they share the SAME scratch array
// (_G_LLM_STRAT_AI_SCAN_TARGETS / _G_LLM_STRAT_AI_SCAN_TARGET_COUNT):
//   - target_list_scan_visible_enemies walks every OTHER active (hostile) player's unit roster
//     and appends every visible, active, non-idle enemy unit to that scratch via
//     scan_target_list_add;
//   - scan_target_list_sort is a two-line wrapper that hands that same scratch to the game's own
//     qsort with a fixed element width and the game's own comparator.
//
// NOT `player_data::ai_target_list` and NOT `llm_strat_ai_target_list_add` (0x004d6d67, already
// translated) -- see the CONTEXT.md note this batch was briefed with. `llm_strat_ai_scan_target_
// list_add` (0x004ec2d0, callee, stays original) is the ONLY writer of the scratch this pair
// touches.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game and no rig. The wrappers below are this applied to state(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_target_list_scan_visible_enemies @0x004ec5ea.
//
// For every OTHER active player (unsigned compare against *v.active_player_count) that `player`
// considers HOSTILE (player_data[player].ai_player_relation[other] == -1, tested as "> -1 ->
// skip" so a stray non-+-1/-1 value also skips, exactly as the original), walks that player's
// unit roster COUNT-DRIVEN (remaining seeded from units[other][0]'s first two bytes reinterpreted
// as a ushort, decremented only on an occupied slot, unit_index incremented every iteration --
// see ai_turret_threat.cpp for the same idiom over buildings). For every occupied unit with
// energy > 0 that is NOT idle-or-parked and whose tile in `player`'s OWN ai_tile_flags_grid reads
// in [1,4], packs a target ref (other's index in the low bits | 0x80 if the unit's cfg TYPE is
// < 0xf, else | 0x20) and appends it via scan_target_list_add. Returns the count added.
//
// NEEDS `v.cfg_units` (see ai_scan_visible.cpp top-of-file note) -- not yet in ai_view.
int32_t target_list_scan_visible_enemies(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                         uint32_t player);

// llm_strat_ai_scan_target_list_sort @0x004ec7bb.
//
// Thin wrapper: `gc.qsort(base, count, 0x16, gc.scan_target_sort_cmp)`. The comparator returns 0
// on a tie, so the permutation of equal keys is the GAME's qsort, not ours -- see ai_calls::qsort.
// Takes (v, own) only for shape consistency with every other detail:: function in this cluster --
// this body reads and writes NO AI state at all, they are unused.
void scan_target_list_sort(const ai_view &v, const ai_store &own, const ai_calls &gc, void *base,
                           uint32_t count);

} // namespace detail

int32_t target_list_scan_visible_enemies(uint32_t player);
void    scan_target_list_sort(void *base, uint32_t count);

} // namespace mh::ai
