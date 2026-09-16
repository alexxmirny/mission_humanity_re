//
// ai/ai_group_form.h -- the three AI unit-group FORMATION functions (RI-AI / AI1C layer 2).
//
// llm_strat_ai_group_form_standby_from_pool3 @0x004e722a  (0xc5 bytes)
// llm_strat_ai_group_form_surplus_from_pool4 @0x004e72ef  (0xc4 bytes)
// llm_strat_ai_group_form_patrol             @0x004e7ad2  (0xe3 bytes)
//
// ONE SHAPE, THREE INSTANCES, which is why they share a translation unit: each censuses the
// player's existing groups by `goal`, refuses to act if the census says there is already one (or
// enough), checks a POOL group's headcount against a threshold, then creates a group, stamps its
// goal + active_member_count, and enqueues exactly two tasks onto it. The differences are the goal
// value, the pool group index, the threshold and the task pair -- so writing them side by side is
// what makes an accidental transposition visible.
//
//   fn                        goal  census rule              pool   threshold                tasks
//   form_standby_from_pool3   0x0a  ANY group with goal 0xa  g[3]   member_count != 0         0x12, 0x17
//   form_surplus_from_pool4   0x08  ANY group with goal 0x8  g[4]   member_count >= MIN_POOL4 0x13, 0x15
//   form_patrol               0x05  COUNT of goal 5 >= MAX   g[2]   member_count > 3 * SIZE   0x10, 0x0e
//
// THE POOL GROUPS ARE SEED GROUPS. All three read `member_count` of a group with a FIXED index in
// 0..4, i.e. one of llm_strat_spawn_ai_base's five seed groups, which is why nothing bounds-checks
// the index and why llm_strat_ai_unit_group_tick's reaper has a floor of 5. Derived from the
// absolute operands, not assumed: 0xe8035e / 0xe80dc4 / 0xe7f8f8 minus player_data's base 0xe6dec0
// minus ai_groups' offset 0x10568 give 3 * 0xa66 + 4, 4 * 0xa66 + 4 and 2 * 0xa66 + 4 exactly.
//
// THE CENSUS BOUND IS ai_group_count AND IT IS RE-READ EVERY ITERATION (the loop header at
// 0x004e7255 / 0x004e731a / 0x004e7b01 recomputes the whole player base and reloads the count), so
// the loops are written that way here rather than hoisting it. The first two short-circuit out of
// the loop the moment they see a match; the third has no early exit because it is counting.
//
// TWO THINGS THAT LOOK LIKE MISTAKES AND ARE NOT.
//   * form_patrol is the ONLY one of the three that checks group_create's -1 (CMP EAX,-1 / JZ at
//     0x004e7b50). The other two use the returned index unchecked, and a -1 there computes
//     player_data[p].ai_groups[-1] -- an address 0xa66 bytes BEFORE ai_groups, still inside
//     player_data. Preserved; see the per-function comment in the .cpp.
//   * form_patrol's supply gate is `pool.member_count > 3 * PATROL_GROUP_SIZE` (JLE returns), i.e.
//     it wants three times the group it is about to form, not one. Preserved as written.
//
// THE TWO TAILS ARE PHYSICALLY SHARED IN THE IMAGE and that is worth knowing before reading the
// listings: form_surplus_from_pool4 ends with `JMP 0x004e72e1`, which is inside
// form_standby_from_pool3, and form_patrol does the same. All three therefore make their SECOND
// group_task_enqueue call through one instruction pair. Nothing about that is observable -- the
// arguments are already in registers and on the stack -- but a reader of the disassembly who does
// not notice will think two of the three functions have no second enqueue.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// WHAT A CALL ACTUALLY DID. All three functions return void and most calls return at the census or
// the pool gate having written nothing, which is indistinguishable in the compared region from a
// call that never happened. These fields are the anti-vacuity evidence for the shadow arm.
struct group_form_report {
    int32_t census_scanned = 0;     // groups examined by the census loop
    int32_t census_matches = 0;     // groups whose goal matched (patrol counts; the others stop at 1)
    bool    census_blocked = false; // returned because the census said no
    bool    pool_blocked   = false; // returned because the pool headcount was below the threshold
    bool    create_failed  = false; // group_create returned -1 (form_patrol only -- see the header)
    bool    formed         = false; // a group was created and stamped
    int32_t group_index    = -1;    // what group_create returned, when it was called
    int32_t tasks_enqueued = 0;     // group_task_enqueue calls
};

// llm_strat_ai_group_form_standby_from_pool3 @0x004e722a.
group_form_report form_standby_from_pool3(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                          int32_t player_id);
// llm_strat_ai_group_form_surplus_from_pool4 @0x004e72ef.
group_form_report form_surplus_from_pool4(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                          int32_t player_id);
// llm_strat_ai_group_form_patrol @0x004e7ad2.
group_form_report form_patrol(const ai_view &v, const ai_store &own, const ai_calls &gc,
                              int32_t player_id);

} // namespace detail

void form_standby_from_pool3(int32_t player_id);
void form_surplus_from_pool4(int32_t player_id);
void form_patrol(int32_t player_id);

} // namespace mh::ai
