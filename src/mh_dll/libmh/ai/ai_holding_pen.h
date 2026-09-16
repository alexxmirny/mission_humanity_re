//
// ai/ai_holding_pen.h -- the AI holding-pen target scanner (RI-AI / AI1A batch A layer 2).
//
// llm_strat_ai_holding_pen_scan_targets @0x004ec35f walks `player_data[player].ai_groups[pen_index]`'s
// member chain (starting at `head_unit`, then each member unit's OWN `ai_group_next` link -- the same
// intrusive list `llm_strat_ai_group_member_move` maintains) and, for every member unit, spiral-scans
// the tiles around THAT unit's own (x, y) out to a radius of max(sight, weapon range): a tile's
// BUILDING is offered when the tile is occupied (`class_owner & 0xc0`) and its owner is hostile to
// `player`; every UNIT stacked on the tile is offered the same way, walked via the tile's packed
// `unit` chain and each unit's OWN `unit_above` link -- exactly the OFFER 2 walk in
// ai_spiral_scan.cpp's scan_spiral_ring_for_engage_candidates, reused here over a different anchor and
// with no target_mask / fog-of-war gate (this function has neither -- see the .cpp). Every qualifying
// target is appended via gc.scan_target_list_add (the 0x004ec2d0 SCAN-side entry point into
// `_G_LLM_STRAT_AI_SCAN_TARGETS`, NOT player_data's `ai_target_list`). Returns the count of targets
// added.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrapper below is this applied to state(); the split costs one inlined call.
namespace detail {

// llm_strat_ai_holding_pen_scan_targets @0x004ec35f. No ai_store parameter: this function writes
// nothing of its own (every effect is the gc.scan_target_list_add call).
int32_t holding_pen_scan_targets(const ai_view &v, const ai_calls &gc, int32_t player,
                                 uint32_t pen_index);

} // namespace detail

int32_t holding_pen_scan_targets(int32_t player, uint32_t pen_index);

} // namespace mh::ai
