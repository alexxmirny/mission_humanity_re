//
// ai/ai_group_task_formation.h -- the LAST four functions of RI-AI / AI1C antichain layer 3
// (2026-08-06, closing the layer's real count of 28):
//
//   llm_strat_ai_group_task_advance_to_anchor   @0x004eaf72 (0xbc bytes)  task_code 0x03, 0x0c
//   llm_strat_ai_group_task_disperse_passable   @0x004eb02e (0x9d bytes)  task_code 0x04
//   llm_strat_ai_group_task_wait                @0x004eaf67 (0x0b bytes)  task_code 0x07
//   llm_strat_ai_group1_drain_to_group0         @0x004eb2a0 (0x4c bytes)  (not a task arm)
//
// WHY THESE FOUR SHARE A FILE. The first three are group-task-machine arms reached from
// llm_strat_ai_group_task_activate's nineteen-arm dispatch (ai_group_task_machine.cpp's
// ACTIVATE_ARMS table), and the first two are the same function twice over: identical member-list
// harvest into the shared _G_LLM_STRAT_AI_GROUP_UNIT_SCRATCH_LIST, differing only in which mover
// they hand the filled list to. `wait` rides along because it is the third arm of the same dispatch.
// group1_drain_to_group0 is not a task arm at all -- it is llm_strat_ai_group_home_guard_replenish's
// first unconditional step -- but it is the fourth and last unclaimed layer-3 entry in the migration
// set, and it is a nine-line loop; giving it its own translation unit would be ceremony.
//
// THE HARVEST LOOP IS THE FILE'S ONE REAL IDEA, and it is NOT the same as the member walks in
// ai_group_task_movement.cpp / ai_group_home_guard.cpp. Those iterate to act on each member. These
// two iterate to PUBLISH the membership: reset _G_LLM_STRAT_AI_GROUP_UNIT_SCRATCH_COUNT to zero,
// append every member id to _G_LLM_STRAT_AI_GROUP_UNIT_SCRATCH_LIST while following
// unit::ai_group_next, and then make ONE call whose callee reads that list back. The list is the
// argument. ai_group_relocation.cpp's SMALL branch does the identical thing before its own
// gc.group_scatter_to_passable_tile, which is what confirms the protocol rather than merely
// suggesting it.
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h's offsetof asserts by resolving the
// (player*0x288fc + group*0xa66) base each body computes and subtracting: ai_groups[] sits at
// player_data+0xe7e428 in these listings, giving member_count +0x4, head_unit +0xa,
// active_param_a +0x2f, active_param_b +0x33 -- all four match the asserts exactly. unit::
// ai_group_next lands at +0xd4 off units (0xdd8d1c - 0xdd8c48), likewise matching.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrappers below are this applied to state().
namespace detail {

// llm_strat_ai_group_task_advance_to_anchor @0x004eaf72, task_codes 0x03 and 0x0c.
//
// Harvests the group's member list into the shared scratch list (see the banner), then computes the
// group's live centroid and hands BOTH to gc.group_move_formation_rotating: the centroid as the
// formation's current reference point (the callee's `unused_param2`/`unused_param3` slots -- named
// "unused" by the committed prototype's own derivation, carried through verbatim rather than
// renamed on a guess) and the group's stored active_param_a/active_param_b as the destination tile.
//
// IT DOES NOT DEQUEUE. Its final `JMP 0x004ea069` lands on the shared Watcom epilogue that
// llm_strat_ai_group_task_drain_reserve_attack's body happens to own -- PAST the
// llm_strat_ai_group_task_dequeue call at 0x004ea064 that its siblings patrol_shuttle (0x004ea062)
// and loiter_wander (0x004ea064) jump INTO. Three arms, three different landing addresses in the
// same tail, two of which dequeue and this one does not. Do not "restore symmetry" here.
void group_task_advance_to_anchor(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                  int32_t player_id, int32_t group_index);

// llm_strat_ai_group_task_disperse_passable @0x004eb02e, task_code 0x04.
//
// Byte-for-byte the same harvest as advance_to_anchor above, then a single
// gc.group_scatter_to_passable_tile(player, active_param_a, active_param_b) -- no centroid call, no
// formation mover, and an ordinary RET rather than a jump into anyone's shared tail.
void group_task_disperse_passable(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                  uint32_t player_id, int32_t group_index);

void group_task_wait();

// llm_strat_ai_group1_drain_to_group0 @0x004eb2a0.
//
// Empties group 1 into group 0, one member at a time, by calling gc.group_member_move(player, 1, 0,
// head_unit) until group 1's member_count reaches zero. Both the count and the head are RE-READ from
// the view every iteration (the original recomputes its player base at the top of the loop, at
// LAB_004eb2b0) because group_member_move mutates exactly those two fields -- caching either would
// spin forever or walk a stale head. Same re-read discipline as ai_group_home_guard.cpp's step 4.
//
// Termination rests on group_member_move actually decrementing group 1's count, which is the
// original's assumption too -- there is no iteration guard in the disassembly.
void group1_drain_to_group0(const ai_view &v, const ai_store &own, const ai_calls &gc,
                            uint32_t player_id);

} // namespace detail

void group_task_advance_to_anchor(int32_t player_id, int32_t group_index);
void group_task_disperse_passable(uint32_t player_id, int32_t group_index);
void group_task_wait();
void group1_drain_to_group0(uint32_t player_id);

} // namespace mh::ai
