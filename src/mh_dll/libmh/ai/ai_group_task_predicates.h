//
// ai/ai_group_task_predicates.h -- the four PROGRESS PREDICATES llm_strat_ai_group_task_step polls
// (RI-AI / AI1C layer 3). Translated from the DISASSEMBLY:
//
//   llm_strat_ai_group_check_arrival_status   @0x004e943d (0x167 bytes)  STEP_ARRIVAL
//   llm_strat_ai_group_no_member_near_centroid @0x004d66df (0x93 bytes)  STEP_NEAR_CENTROID
//   llm_strat_ai_group_area_scan_hostile      @0x004d69ce (0x1b6 bytes)  STEP_SCAN_HOSTILE_40/_C0
//   llm_strat_ai_group_all_units_settled      @0x004d6b84 (0x5f bytes)   STEP_SETTLED
//
// All four are called from ai_group_task_machine.cpp's group_task_step (see its STEP_ARMS switch)
// through the SAME ai_calls slots this file now provides real bodies for: group_check_arrival_status,
// group_check_unit_near_centroid (bound to the RENAMED llm_strat_ai_group_no_member_near_centroid --
// see the polarity note below), group_area_scan_hostile and group_all_units_settled. That caller is
// NOT retranslated here; this file only supplies what its `gc.` calls now resolve to for real.
//
// ALL FOUR WALK THE SAME INTRUSIVE MEMBER LIST: unit_group::head_unit (+0xa) is the first member's
// roster index (0 = empty group), and unit::ai_group_next (+0xd4, on the UNIT record) is the forward
// link (0 = end of chain). None of the four ever reads tail_unit or ai_group_prev -- the walk is
// singly-linked in this direction. None of the four WRITES anything (own is unused in all four); they
// are pure predicates over player_data/units/the map planes.
//
// TWO NAMING TRAPS SPELLED OUT ONCE HERE RATHER THAN FOUR TIMES.
//
//  1. group_no_member_near_centroid'S RETURN POLARITY IS THE NEGATION OF ITS NAME (and of the ai_calls
//     slot it fills, `group_check_unit_near_centroid`): it returns 1 when NO member is within
//     dist_sq < 0x64 of the group centroid, and 0 as soon as one member is found near it. The function
//     was renamed 2026-08-06 (was llm_strat_ai_group_check_unit_near_centroid) for exactly this reason
//     -- the old name read as a positive predicate. See its own comment below.
//
//  2. group_no_member_near_centroid's "empty group" return-1 exit is `MOV EAX,0x1` at 0x004d66d1,
//     which is EIGHT BYTES BEFORE this function's own entry point (0x004d66df) -- it lives inside the
//     PRECEDING function's shared Watcom epilogue, not inside a call. It is still this function's own
//     control flow (the chain-exhausted fallthrough jumps there), not a bug in the .asm export; see the
//     function's own comment for the exact address trace.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// llm_strat_ai_group_check_arrival_status @0x004e943d.
//
// Walks `player`'s ai_groups[group_index] member chain and returns a readiness code: 0 = not ready,
// 1 = arrival threshold met (or the group is empty), 2/3 = partial (a straggler of order_notify_status
// 5/4 was seen and nothing since reset readiness to 0).
//
// Per member (SKIPPED entirely -- no classification, straight to the next-unit fetch -- when the unit
// is an AI PLANE **and** its order state is already settled; that is the one condition under which the
// switch below does not run at all):
//   order_notify_status 0        : no-op (falls straight through)
//   order_notify_status 1,2,3    : toroidal_dist_sq(unit.x, unit.y, group.active_param_a,
//                                   active_param_b) < 100 -> tally a near-hit; else readiness = 0
//                                   (UNSIGNED compare, CMP EAX,0x64 / JNC)
//   order_notify_status 4        : if readiness != 0, readiness = 3 (no near-hit tally)
//   order_notify_status 5        : if readiness != 0, readiness = 2, THEN FALLS THROUGH to 6's tally
//   order_notify_status 6        : tally a near-hit
//   order_notify_status > 6      : rejected before the switch (CMP AL,0x6 / JA), same as skipped
//
// After the chain is exhausted: if readiness is still 0 (i.e. nothing already forced it to 2/3, and at
// least one straggler drove it to 0), a second look re-admits it to 1 when
// `member_count * 0x46 <= near_hit_count * 100` (member_count read UNSIGNED, ~70% threshold). If
// readiness is already non-zero (1, 2 or 3) that second look is skipped and the value returns as-is.
// An EMPTY group (head_unit == 0) never enters the loop, so readiness stays its initial 1.
int32_t group_check_arrival_status(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                   uint32_t player, int32_t group_index);

// llm_strat_ai_group_no_member_near_centroid @0x004d66df (was
// llm_strat_ai_group_check_unit_near_centroid; renamed 2026-08-06 for the inverted polarity -- see the
// header banner). Fills a centroid via gc.group_compute_centroid, then walks the member chain: as soon
// as any member's toroidal_dist_sq to the centroid is < 100, returns 0 ("found one near it") on the
// spot. If the chain runs out (including an empty group) without a near hit, returns 1 ("no member is
// near the centroid"). Both exits set the full dword.
int32_t group_no_member_near_centroid(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                      int32_t player, int32_t group_index);

// llm_strat_ai_group_area_scan_hostile @0x004d69ce.
//
// Scans `player`'s ai_groups[group_index] designated tile area for a BUILDING matching `owner_mask`
// (tested against the tile's class_owner high nibble, `(owner_mask & (class_owner & 0xf0)) != 0`)
// whose owner (class_owner low nibble) is hostile to `player`
// (`player_data[player].ai_player_relation[owner] < 0`, a SIGNED compare -- the original spells the
// spiral branch's version of this test as `CMP dword,-1 / JG`, i.e. "skip unless > -1", which is the
// exact same condition as `< 0` and is written that way here for clarity). Returns 0 the instant such a
// building is found, 1 if the whole area is clear.
//
// TWO DIFFERENT AREA SHAPES, selected by group.active_sub_code:
//   == 0  : a RECTANGULAR sweep, Y from active_param_b to active_param_d (exclusive) outer, X from
//           active_param_a to active_param_c (exclusive) inner, each axis wrapping via
//           `(coord + 1) & *map_*_mask` on increment. THESE ARE `!=` LOOPS, not `<` loops -- read
//           verbatim from 0x004d6aca-0x004d6b77. An empty rectangle (start == end on either axis)
//           degenerates to zero iterations on that axis rather than a full wrap.
//   != 0  : a SPIRAL disc scan out to `spiral_ring_cell_counts[(uint16_t)active_sub_code]` cells
//           (active_sub_code read UNSIGNED as the table index), each cell
//           `(active_param_a + spiral_offsets[i].dx) & *map_width_mask`,
//           `(active_param_b + spiral_offsets[i].dy) & *map_height_mask`.
// Both shapes index the SAME tile plane the same way: tile_at(v, x, y).
//
// Calls nothing (pure over the view); `own` and `gc` are unused.
int32_t group_area_scan_hostile(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                uint32_t player, int32_t group_index, uint8_t owner_mask);

// llm_strat_ai_group_all_units_settled @0x004d6b84. Walks the member chain calling
// gc.unit_order_state_is_settled(player, unit_index) per member; returns 0 (and stops) on the first
// unsettled member, 1 once the chain is exhausted (including an empty group) with none unsettled.
int32_t group_all_units_settled(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                int32_t player_id, int32_t group_index);

} // namespace detail

int32_t group_check_arrival_status(uint32_t player, int32_t group_index);
int32_t group_no_member_near_centroid(int32_t player, int32_t group_index);
int32_t group_area_scan_hostile(uint32_t player, int32_t group_index, uint8_t owner_mask);
int32_t group_all_units_settled(int32_t player_id, int32_t group_index);

} // namespace mh::ai
