//
// ai/ai_group_member_count.h -- the AI unit-group ACTIVE-member-count adjuster (RI-AI AI1E).
//
// One function, two modes selected by its `mode` argument:
//
//   mode == 0  -- "this unit just went operationally inactive" (boarded/stored). Decrements the
//                 unit's CURRENT ai_group's active_member_count by one -- a plain 16-bit wrapping
//                 decrement, no floor check, matching the original's bare `DEC word ptr`.
//   mode != 0  -- "this unit just became operationally active again" (exited storage, landed,
//                 took off). Classifies the unit by combat role (ground/plane/heli/soldier) and
//                 routes it through group_member_move into the matching SEED group slot
//                 (AI_SEED_GROUP_COUNT == 5, so slots 2/3/4 are real, always-present groups, not
//                 synthesized indices) -- unless the unit is none of the four roles, in which case
//                 nothing happens at all.
//
// active_member_count is a DIFFERENT counter from unit_group::member_count: member_count (and the
// intrusive head/tail list) is owned by group_member_unlink/group_member_link (see
// ai_group_membership.h); this function is the field's own comment's SOLE named adjuster
// ("adjusted by llm_strat_ai_group_member_count_adjust", mh_structs.gen.h).
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with no
// game and no rig. The wrapper below is this applied to state(); the split costs one inlined call.
namespace detail {

// llm_strat_ai_group_member_count_adjust @0x004db499.
//
// `player` is masked to its low 4 bits before use (`player & 0xf`, unconditionally -- no range
// check against MAX_PLAYERS either here or in the original), giving the player_data slot both the
// active-gate read and every other access in this body index from.
//
// `group_or_type` (the THIRD parameter, arriving in EBX) IS NEVER READ: the assembly overwrites EBX
// with its own player_data-offset scratch computation before the register is used for anything, so
// whatever the caller passes there is discarded unread. Kept in the signature only because it is
// part of the original's fixed ABI (the shadow-replace binding needs an exact match) -- do not wire
// it to anything.
//
// Early-out: if player_data[player & 0xf].ai_enabled == 0, return immediately (no group lookup, no
// callee at all).
//
// Otherwise looks up the unit's CURRENT ai_group index (unit_get_ai_group_index) and:
//   - mode == 0: decrements that group's active_member_count by 1 (in place, 16-bit, wrapping same
//     as the original's `DEC word ptr`) and returns -- no classification, no group_member_move call.
//   - mode != 0: classifies the unit via the shared ground/plane/heli/soldier predicates, in that
//     TESTING ORDER (ground first, then plane, then heli, then soldier), and calls group_member_move
//     to move it from its current group into the matching seed slot: ground OR soldier -> 2, plane
//     -> 4, heli -> 3. If none of the four predicates fire, returns without calling anything.
void group_member_count_adjust(const ai_view &v, const ai_store &own, const ai_calls &gc,
                               uint32_t player, uint32_t unit_index, uint32_t group_or_type,
                               uint32_t mode);

} // namespace detail

void group_member_count_adjust(uint32_t player, uint32_t unit_index, uint32_t group_or_type,
                               uint32_t mode);

} // namespace mh::ai
