#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// llm_strat_ai_group_task_muster_from_pool @0x004ea469.
//
// Folds `reinforce_pending` into `active_sub_code` and clears `reinforce_pending`
// (0x004ea49e-0x004ea4ac -- read active_sub_code, ADD reinforce_pending's OLD value into it, THEN
// zero reinforce_pending; the ADD's result, i.e. the POST-fold active_sub_code, is what the very next
// instruction's JZ tests). If that post-fold value is 0, return immediately -- no pool is drained.
//
// Otherwise pulls ONE unit per call from a pool group into this group:
//   - if player_data[player].ai_groups[2] (the general combat pool) has members, source it. task_code
//     selects the pick policy: 0x10 = group_find_slowest_unit(player, 2), 0x11 =
//     group_pick_best_weapon_unit(player, 2), else (0xf) = ai_groups[2].head_unit. Then, UNLIKE the
//     pool-0 fallback below, dispatches the unit's move order first: unit_issue_default_order(player,
//     unit) if active_param_a == -1, else unit_flag_and_move(player, unit, active_param_a,
//     active_param_b). Then group_member_move(player, /*src*/2, group_index, unit).
//   - else if ai_groups[0] (the seed/idle pool) has members, source it with the SAME task_code policy
//     (find_slowest_unit / pick_best_weapon_unit / head_unit, now against src_group 0) but issues NO
//     move order at all -- read directly off the branch targets: 0x004ea5c2 JMPs straight to 0x004ea554
//     (the shared group_member_move call), skipping the active_param_a check and both order calls
//     entirely. This is not a plausible "should be symmetric" slip to fix; it is what the bytes do.
//   - else (both pools empty) sets active_sub_code = 0 and returns without moving anything.
//
// On a successful pool draw (either branch), decrements active_sub_code by 1 after the move.
void group_task_muster_from_pool(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                 uint32_t player_id, int32_t group_index);

// llm_strat_ai_group_task_recruit_from_pool3 @0x004ea5d9.
//
// Same reinforce_pending -> active_sub_code fold as muster_from_pool. If the POST-fold value is 0, OR
// player_data[player].ai_groups[3].member_count is 0, sets active_sub_code = 0 and returns (the
// original tests these as two short-circuited branches to the SAME reset target, 0x004ea627 /
// 0x004ea631 -> LAB_004ea670 -- reproduced here as `||`). Otherwise takes ai_groups[3].head_unit,
// launches it from storage (unit_launch_from_storage_enqueue(player, unit, active_param_a,
// active_param_b) -- the ADAPTER at 0x004d58e7, not the real mover directly), moves it into this group
// (group_member_move(player, /*src*/3, group_index, unit)), and decrements active_sub_code by 1.
void group_task_recruit_from_pool3(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                   uint32_t player_id, int32_t group_index);

// llm_strat_ai_group_task_recruit_from_pool4 @0x004ea69c. Byte-identical shape to
// recruit_from_pool3 except it sources/targets ai_groups[4] instead of [3] -- confirmed by an
// independent field-offset derivation from its own literals (0xe80dc4 / 0xe80dca vs pool3's 0xe8035e /
// 0xe80364, each exactly 4*0xa66 vs 3*0xa66 past the group-0 base), not assumed from the plate text.
void group_task_recruit_from_pool4(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                   uint32_t player_id, int32_t group_index);

// llm_strat_ai_group_task_recruit_from_storage @0x004ea729.
//
// A SETUP handler, not a per-tick budget drain like the three above (it never touches active_sub_code
// or reinforce_pending's fold -- it only zeroes reinforce_pending outright and stamps goal = 9).
// Zeroes a 25-entry local scratch (per-storage-slot "units reserved this call" counter, purely local --
// nothing in ai_view/ai_store models it, matching the original's own stack array).
//
// Then, once per CURRENT group member (member_count iterations; the "current member" being processed
// is always ai_groups[group_index].head_unit, re-read fresh every iteration -- see below for why that
// still visits every member):
//
//   Scans unit_storage[player][1..24] (slot 0 never read) for the first OCCUPIED slot (b_index != 0)
//   that is not over its 50-docked cap counting this call's own reservations
//   (docked_count + reserved[slot] < 0x32, compared UNSIGNED -- CMP/JNC at 0x004ea7be/0x004ea7c1. NOTE:
//   the sibling function llm_strat_ai_route_unit_to_home_storage's OWN 50-cap check on the same field
//   uses a SIGNED JGE per ai_group_relocation.h's N1 note -- the two functions use different comparison
//   forms on the same field family, verified independently in each function's own bytes, not a copy-
//   paste inconsistency to "fix") whose parked building's building_id (buildings[player][b_index]
//   .building_id -- Ghidra could not resolve this one to a named field in the .c, re-derived from the
//   literal 0xc3d2a2 against the SAME player*0x6aa4+b_index*0x111 addressing every other buildings[]
//   access in this cluster uses. CONDUCTOR-CONFIRMED (2026-08-06, not left as the translator's
//   uncertainty): `buildings` base is 0x00c3d2a0 (mh_addrs.gen.h), stride player=0x6aa4/index=0x111,
//   and mh_map_object_building.building_id is offset 0x2 (static_assert'd in mh_structs.gen.h) --
//   0x00c3d2a0 + 0x2 == 0xc3d2a2 exactly, so this literal IS buildings[player][b_index].building_id,
//   byte-for-byte, not merely a plausible read) matches one of the four housing-candidate ids for
//   the group's head_unit's OWN class:
//     building_id == players[player+1].ai_housing_candidate_vehicle && unit_is_ai_ground(head_unit), OR
//     building_id == players[player+1].ai_housing_candidate_heli    && unit_is_ai_heli(head_unit),   OR
//     building_id == players[player+1].ai_housing_candidate_plane   && unit_is_ai_plane(head_unit),  OR
//     building_id == players[player+1].ai_housing_candidate_soldier && unit_is_ai_soldier(head_unit)
//   evaluated in that exact order with the ORIGINAL's short-circuit shape: a building_id mismatch AND a
//   classifier-false both fall through to the next check identically (0x004ea815/0x004ea88f/0x004ea909
//   all target the next check's own entry, same as a straight mismatch would).
//
//   MATCH: bumps this call's local reservation for that slot, then
//   unit_group_assign_by_type(player, group_index, head_unit, matched_slot).
//
//   NO MATCH across all 24 slots: classifies head_unit itself (ground -> dst 2, plane -> dst 4,
//   heli -> dst 3, checked in that order, ground/soldier BOTH map to dst 2 -- unit_is_ai_ground is
//   checked before unit_is_ai_soldier here, unlike route_unit_to_home_storage's soldier-first order,
//   confirmed independently off 0x004ea9de/0x004ea9f0) and calls group_member_move(player,
//   /*src*/group_index, dst, head_unit) -- EXCEPT when head_unit matches NONE of the four classifiers,
//   in which case NOTHING is called at all (0x004eaa54 JZ straight to the loop tail, no move, no
//   assign): read directly off the bytes, not inferred.
//
// Either way (matched, moved, or neither), the member-count loop decrements exactly once and re-reads
// head_unit fresh next pass -- so "for every current group member" holds because group_member_move /
// unit_group_assign_by_type are expected to remove/relink the head from the intrusive list on a
// successful action; an unclassifiable head_unit that is never moved would be re-examined identically
// on every remaining pass (reproduced, not "fixed" into a real iterator).
void group_task_recruit_from_storage(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                     uint32_t player_id, int32_t group_index);

} // namespace detail

void group_task_muster_from_pool(uint32_t player_id, int32_t group_index);
void group_task_recruit_from_pool3(uint32_t player_id, int32_t group_index);
void group_task_recruit_from_pool4(uint32_t player_id, int32_t group_index);
void group_task_recruit_from_storage(uint32_t player_id, int32_t group_index);

} // namespace mh::ai
