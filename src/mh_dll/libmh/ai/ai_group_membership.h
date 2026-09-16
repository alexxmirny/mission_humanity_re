//
// ai/ai_group_membership.h -- the AI unit-group create/move/split-link trio (RI-AI batch C layer 4,
// 2026-08-06). Three small, related pieces of the unit-group task-force machinery:
//
//   group_create               -- allocates a new task-force slot for `player_id`, or -1 when the
//                                  player's 32-group cap (ai_group_count == 0x20) is already reached.
//   group_member_move          -- the two-call splice that moves one unit from one group's intrusive
//                                  member list to another's: unlink, then link. Nothing else.
//   group_has_split_group_link -- group_split_off_create's own gate: scans the NON-SEED groups
//                                  (index >= AI_SEED_GROUP_COUNT) for an existing goal==7 link to the
//                                  given target group id.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with no
// game and no rig. The wrappers below are this applied to state(); the split costs one inlined call.
namespace detail {

// llm_strat_ai_group_has_split_group_link @0x004d3af2.
//
// Scans player_data[player_id].ai_groups[AI_SEED_GROUP_COUNT .. ai_group_count) -- the FIVE SEED
// groups at indices 0..4 are skipped, not scanned -- for a group whose `goal` is 7 (the "split link"
// mission tag) AND whose `link_target_group` equals `target_group_id`. Returns 1 on the first match,
// 0 once the scan runs out. Pure: writes nothing (checked against the disassembly -- no store
// instruction anywhere in the body).
int32_t group_has_split_group_link(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                   int32_t player_id, uint32_t target_group_id);

// llm_strat_ai_group_member_move @0x004d4af5.
//
// A thin two-call wrapper: unlink the unit from `src_group`'s intrusive member list, then link it
// into `dst_group`'s. Nothing else -- do not fuse or reorder the two calls, and do not read or write
// any group/unit field directly here; both calls own their own writes.
void group_member_move(const ai_view &v, const ai_store &own, const ai_calls &gc, uint32_t player,
                       int32_t src_group, int32_t dst_group, int32_t unit_id);

// llm_strat_ai_group_create @0x004d4d85.
//
// Allocates the next free ai_groups[] slot for `player_id`. Returns -1 IMMEDIATELY, without touching
// ai_group_count/next_group_serial and without calling group_task_enqueue, when ai_group_count has
// already reached the 0x20 cap -- preserve that early-out exactly. Otherwise: takes the current
// ai_group_count as the new group's index and bumps the count; stamps the new group's serial_id from
// the current next_group_serial and bumps that too; zero-initialises the rest of the group's header
// (member_count, reinforce_pending, active_member_count, head_unit, tail_unit, current_param,
// task_queue_count, goal, and the 2-byte reserved_0x16 slot -- exactly what the original zeroes, no
// more and no less); and unconditionally enqueues task code 10 (an instant-complete placeholder, not
// one of the two named task codes in ai_state.h) with all eight remaining task_enqueue arguments
// zero. Returns the new group's index.
int32_t group_create(const ai_view &v, const ai_store &own, const ai_calls &gc, int32_t player_id);

} // namespace detail

int32_t group_has_split_group_link(int32_t player_id, uint32_t target_group_id);
void    group_member_move(uint32_t player, int32_t src_group, int32_t dst_group, int32_t unit_id);
int32_t group_create(int32_t player_id);

} // namespace mh::ai
