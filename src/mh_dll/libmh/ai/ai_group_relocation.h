#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// llm_strat_ai_route_unit_to_home_storage @0x004d590e.
//
// Classifies `unit_id` into one of four AI roles (IsAiSoldier -> 1, else IsAiGround -> 2, else
// IsAiPlane -> 3, else IsAiHeli -> 4, else 0 -- checked in that exact order, first match wins), then
// scans unit_storage[player][1..24] (slot 0 is never read) for a slot that is OCCUPIED (b_index != 0)
// and not overcrowded (docked_count <= 0x31=49) whose parked building's building_id matches the
// role's designated home-storage building id -- player_data[player+1].ai_housing_candidate_{vehicle,
// heli,plane,soldier} for role {2,4,3,1} respectively (the SAME "+1" aliasing documented on those
// fields in mh_structs.gen.h; NOT a translation error). The scan takes the FIRST matching slot and
// stops.
//
// Regardless of whether a slot was found, for role 1..4 the unit is moved into a fixed per-role AI
// sub-group via group_member_move(player, src=unit's own ai_group_index, dst, unit_id):
//   role 1 (soldier) -> dst group 2
//   role 2 (ground)  -> dst group 2   -- SAME destination as role 1. Read directly off the bytes:
//                                        the switch's case-1 and case-2 arms are two different jump
//                                        targets that both land on the identical shared tail
//                                        (0x004d5b99) which hardcodes EBX=2 -- not a transcription
//                                        slip on this side, and not something a plausible "tidier"
//                                        one-role-per-group mapping would predict. Preserved exactly.
//   role 3 (plane)   -> dst group 4
//   role 4 (heli)    -> dst group 3
// role 0 (unclassified) skips the move entirely (falls straight to the storage-exit check below --
// the original's EAX=ECX-1 / CMP 3 / JA-default dispatch, where ECX=0 makes EAX wrap to 0xffffffff,
// unsigned-greater than 3).
//
// Finally, ONLY if a storage slot was found, enqueues an exit-storage order for it:
// unit_order_exit_storage_enqueue(player, unit_id, matched_slot, 0, 0).
void route_unit_to_home_storage(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                uint32_t player, int32_t unit_id);

// llm_strat_ai_group_reposition_members @0x004d61d3.
//
// FOUR PHASES, all keyed off player_data[player].ai_groups[group_idx]:
//
// Phase 1 (bail-out scan): walks the group's WHOLE member list (via head_unit / unit::ai_group_next,
// no role filter). If ANY member has an order pending (llm_strat_ai_unit_is_order_pending) or a
// secondary target lock (unit::target2_ref != 0), sets
// player_data[player].ai_reposition_cached_member_count = -1 and returns immediately -- no further
// phase runs.
//
// Phase 2 (eligibility count + memo check): re-walks the SAME list counting members that are
// IsAiSoldier OR IsAiGround (the same ordered classifier chain as route_unit_to_home_storage, but
// only two of the four roles are eligible here). If the count is 0, OR the group's current
// (uint16)member_count already equals the memoized ai_reposition_cached_member_count, returns without
// doing anything else (the original checks these as two separate branches -- local_24==0 first, then
// the memo compare after re-deriving the group's base address -- but both are pure reads with nothing
// between them, so testing them as one `||` is behaviourally identical).
//
// Phase 3 (branch on eligible count vs the AI.SCR threshold at 0x0066939c, UNSIGNED compare -- see
// the N5 note in the header banner for the two new scratch regions this phase touches):
//
//   SMALL branch (eligible_count < threshold): picks one anchor tile
//   (llm_strat_ai_pick_owned_tile_or_home), rebuilds the group-unit scratch list from the group's
//   CURRENT (freshly re-read) head_unit -- appending EVERY member, no role filter this time, no cap
//   check -- then calls group_scatter_to_passable_tile(player, anchor_x, anchor_y) once. That callee
//   is original, untranslated code; whatever it does with the freshly-filled scratch list (and
//   whatever OTHER regions it touches, e.g. the turret-threat-adjacent ENCLOSURE_SCRATCH grid per the
//   header banner's N4 note) happens for real.
//
//   LARGE branch (eligible_count >= threshold): counts PASSABLE cells (player's own
//   ai_tile_flags_grid[(x<<8)|y] == 1, the raw byte, not masked against the documented 0x40/0x1f
//   sub-fields -- this is a THIRD use of that grid ai_state.h's own field comment does not yet
//   classify) over the whole player_data[player].ai_tile_flags_grid, width x height (the real map
//   extents, not the physical 256x256). Derives a per-cell "budget" step = (passable_count << 8) /
//   eligible_count (unsigned division), then re-walks the grid a SECOND time with a Bresenham-style
//   fractional accumulator (reset every call, never persisted) to pick roughly eligible_count evenly-
//   spaced passable tiles into the spread-point scratch table (N5), each stamped `used = 0`. Exact
//   accumulator shape, transcribed rather than simplified:
//     if (acc < 0x100) { acc += step; append_point(x, y); ++spread_count; }
//     acc -= 0x100;                                                  // ALWAYS, both branches
//   Then walks the group's member list a THIRD time (fresh head_unit read), and for each IsAiSoldier-
//   or-IsAiGround member: finds the NEAREST spread point with `used == 0` (toroidal_dist_sq,
//   exhaustive scan over 0..spread_count); if none is free, BREAKS THE WHOLE WALK (not just this
//   member -- confirmed at 0x004d64f1: JZ jumps past the loop's own back-edge test straight to the
//   common tail). Otherwise claims it (`used = 1`) and conditionally re-routes the unit:
//     - if unit is in storage-transit: never move.
//     - else if unit is in transit AND (goal_x == target_x OR goal_y == target_y): don't move
//       (already close enough on some axis).
//     - else if unit is in transit (and neither axis matches): DO move.
//     - else (not in transit): move UNLESS (unit.x == target_x OR unit.y == target_y).
//   The move is unit_flag_and_move(player, unit, target_x, target_y). This exact shape (not the
//   superficially different nesting the exported .c reconstructs it as) was read off the raw jump
//   targets at 0x004d6511-0x004d6589; both readings agree on outcome for every input, but the .c's
//   nesting is easy to mistranslate by inspection alone -- see `uncertainties[]`.
//
// Phase 4 (common tail, both branches): player_data[player].ai_reposition_cached_member_count =
// (uint16)ai_groups[group_idx].member_count (freshly re-read).
void group_reposition_members(const ai_view &v, const ai_store &own, const ai_calls &gc,
                              uint32_t player, int32_t group_idx);

// llm_strat_ai_group_split_excess_members @0x004e69ed.
//
// Straightforward, no declared needs: computes the group's centroid (group_compute_centroid),
// spins off `player_data[player].ai_groups[3].member_count` (the shared excess/pool-3 headcount,
// (uint16) zero-extended) members from `group_idx` at that centroid
// (group_split_off_create(player, centroid_x, centroid_y, group_idx, member_count)), then -- unless
// the group's LIVE task_code (ai_groups[group_idx].task_code, NOT the static `goal` field) is already
// 0xc or 8 -- preempts it with a fixed hold/reassess task:
// group_task_preempt(player, group_idx, /*task_code*/ 8, /*param_4*/ 3, 0, 0, 0, 0, 0).
void group_split_excess_members(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                uint32_t player, int32_t group_idx);

} // namespace detail

void route_unit_to_home_storage(uint32_t player, int32_t unit_id);
void group_reposition_members(uint32_t player, int32_t group_idx);
void group_split_excess_members(uint32_t player, int32_t group_idx);

} // namespace mh::ai
