//
// ai/ai_group_centroid.h -- the AI unit group's toroidal-wrapped centroid (RI-AI / AI1A layer 5).
//
// One function: player_data[player].ai_groups[group_index]'s member centroid, walked from head_unit
// along the ai_group_next chain (unit record +0xd4). Nine callers across the AI group task machine
// use it to find "where is this group right now" before issuing a move/attack/patrol order.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrapper below is this applied to state()/live_calls(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_group_compute_centroid @0x004d5c17.
//
// EMPTY GROUP (member_count == 0): does NOT compute anything -- writes the player's own
// ai_home_tile_x/y through the outputs instead. A reimplementation that returns (0,0) or leaves the
// outputs untouched changes AI behaviour on every empty group.
//
// Otherwise: takes the HEAD unit's (x, y) as the origin, walks every member from head_unit along
// ai_group_next (0 = tail). For each member NOT `llm_strat_unit_is_idle_or_parked`, computes its
// (dx, dy) relative to the head, wraps each axis independently into +/-(width/2), +/-(height/2) via
// a two-sided compare-and-adjust (NOT a modulo -- reproduce the shape), and adds it to a running sum.
//
// TWO DELIBERATE ORIGINAL QUIRKS, preserved rather than "fixed":
//   (1) Idle/parked members are skipped from the delta SUM, but member_count is NOT decremented for
//       the divisor -- the divide still uses the ORIGINAL member_count. With idle members present the
//       centroid is biased toward the head unit. Not a rounding artifact.
//   (2) The divide (member_count - 1, only when member_count > 1; skipped -- raw sum used as-is --
//       when member_count == 1) happens on the SUM, not per-delta. The final result
//       (head + sum[/divisor]) is then masked with width_m/height_m via bitwise AND rather than
//       reduced modulo -- correct only because map dimensions are powers of two.
void group_compute_centroid(const ai_view &v, const ai_calls &gc, int32_t player, int32_t group_index,
                            uint32_t *out_x, uint32_t *out_y);

} // namespace detail

// Public wrapper. Signature matches the committed export/call signature
// (sig_llm_strat_ai_group_compute_centroid): the original returns via two out-pointers rather than a
// struct, and nine already-translated callers (ai_group_redistribute.cpp,
// ai_group_relocation.cpp, ai_group_task_formation.cpp, ai_group_task_attack.cpp,
// ai_group_task_movement.cpp, ai_group_task_predicates.cpp, ai_group_task_workers.cpp) already call
// this exact shape through `gc.group_compute_centroid(player, group_index, &x, &y)`.
void group_compute_centroid(int32_t player, int32_t group_index, uint32_t *out_x, uint32_t *out_y);

} // namespace mh::ai
