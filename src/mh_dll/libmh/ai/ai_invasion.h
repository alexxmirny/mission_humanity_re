//
// ai/ai_invasion.h -- llm_strat_ai_invasion_launch_attack_group (RI-AI / AI1C layer 2).
//
// llm_strat_ai_invasion_launch_attack_group @0x004e8a1f (0x17e bytes)
//
// Forms an attack group from the whole of ai_groups[0] and sends it at the weakest surviving
// opponent, for an INVASION-FORCE-only AI player (ai_invasion_force != 0) once its reinforcement
// budget (ai_invasion_points) has run out -- llm_strat_ai_invasion_spawn_reinforcements is the
// function that counts that budget down; this is what takes over when it reaches zero.
//
// THREE GUARDS, all plain early returns to the shared epilogue at 0x004e792e:
//   1. ai_invasion_points != 0 (+0x20)         -- still reinforcing, not yet time to attack.
//   2. ai_groups[0].member_count == 0          -- nothing staged to send.
//   3. any EXISTING group already has goal 3 or 0xb (a census over 0..ai_group_count, UNSIGNED
//      bound) -- an attack is already under way.
//
// THEN PICK THE WEAKEST OPPONENT, unconditionally over every OTHER active player id (no hostility
// filter here, unlike the sibling llm_strat_ai_army_milestone_advance_or_attack -- this scan has no
// `ai_player_relation` check in the bytes at all): best starts at -1, and the first candidate seen
// is taken unconditionally; after that a candidate replaces the current best only when its
// total_unit_power is UNSIGNED-less than the best's (CMP/JNC @0x004e8ae6, matching the identical
// scan already documented in ai_army_milestone.cpp). best can be left at -1 if no OTHER active
// player exists (active_player_count <= 1) -- there is no early return for that case, unlike
// army_milestone: the function proceeds regardless and stamps target_player_id = -1.
//
// group_create; A REAL EARLY RETURN ON -1 (CMP/JZ @0x004e8b18) -- the only guard past the census
// that can still bail. Then goal = 0xb (word @0x004e8b27) and target_player_id = best (DWORD
// @0x004e8b30, can be -1). Then DRAIN ai_groups[0] completely: while member_count != 0, move its
// head_unit into the new group (group_member_move re-reads head_unit every iteration, since the
// move itself mutates the source list). Then TWO task_enqueue calls, same group, same
// pending_param (0x183): task_code 0x18 (attack_random -- the jump-table name from
// ai_group_task_machine.cpp) and task_code 0 (the task_activate no-op/park code, NOT a typo -- see
// that file's jump-table comment).
//
// THE TWO ENQUEUES SHARE A PHYSICAL TAIL WITH llm_strat_ai_group_expansion_form_or_repurpose: the
// second call is a `JMP 0x004e7925` into that function's body, the same shared-tail idiom the
// ai_group_form.cpp header documents for its own trio. Not observable in the translation (the
// arguments are already assembled), but worth knowing when reading the two .asm files side by side.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// WHAT A CALL ACTUALLY DID. The overwhelming majority of calls return at one of the three guards
// having written nothing -- identical in the compared region to a clean call that never ran.
struct invasion_launch_report {
    bool    invasion_points_pending = false; // guard 1: still reinforcing
    bool    home_group_empty        = false; // guard 2: ai_groups[0] has no members to send
    bool    attack_already_underway = false; // guard 3: census found an existing goal 3 or 0xb
    int32_t census_scanned          = 0;
    bool    create_failed           = false; // group_create returned -1
    bool    launched                = false; // the group was created, stamped, drained and enqueued
    int32_t group_index             = -1;
    int32_t target_player_id        = -1; // the weakest opponent found, or -1 if none
    int32_t units_moved             = 0;
    int32_t tasks_enqueued          = 0;
};

// llm_strat_ai_invasion_launch_attack_group @0x004e8a1f.
invasion_launch_report invasion_launch_attack_group(const ai_view &v, const ai_store &own,
                                                    const ai_calls &gc, uint32_t player_id);

} // namespace detail

void invasion_launch_attack_group(uint32_t player_id);

} // namespace mh::ai
