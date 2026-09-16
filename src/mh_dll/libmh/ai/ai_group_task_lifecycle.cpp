//
// ai/ai_group_task_lifecycle.cpp -- see ai_group_task_lifecycle.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_group_task_{disband_004eaaa4,hold_004eab33,recall_home_004eab3e,
// rally_formup_004eaea5,scatter_random_004eaeb4}.asm), not from Ghidra's .c: task_disband's draft
// mislabels the two fields it writes (see the header for the offset derivation that corrects it).
//
#include "ai/ai_group_task_lifecycle.h"


namespace mh::ai {
namespace detail {

namespace {

// See the header: a value `goal` has never carried anywhere else in this cluster.
constexpr int16_t GOAL_DISBANDING = 9;
// task_code 0xc, the code recall_home enqueues its first (move) task under. Verified against
// ai_group_task_machine.cpp's ACTIVATE_ARMS table: slot 0xc is ARM_ADVANCE_TO_ANCHOR (shared with
// slot 0x3), i.e. llm_strat_ai_group_task_advance_to_anchor. AI_GROUP_TASK_RECRUIT_FROM_STORAGE (the
// second enqueue's code, 9) is already declared in ai_state.h.
constexpr uint16_t TASK_ADVANCE_TO_ANCHOR  = 0x0c;
constexpr uint32_t SCATTER_CLASS_OR_RADIUS = 0x0d; // == this task's own code; see the header

} // namespace

// ---------------------------------------------------------------------------------------------
// llm_strat_ai_group_task_disband @0x004eaaa4. Task code 0x14.
// ---------------------------------------------------------------------------------------------
group_task_lifecycle_report group_task_disband(const ai_view &v, const ai_store &own,
                                               const ai_calls &gc, uint32_t player_id,
                                               int32_t group_index) {
    (void)v;
    group_task_lifecycle_report rep{};
    unit_group                 &grp = own.players[player_id].ai_groups[group_index];

    // The two stores at 0x004eaad0/0x004eaada -- see the header for why these are reinforce_pending
    // and goal, not the task_queue_count/task_code the exported .c's plate claims.
    grp.reinforce_pending = 0;
    grp.goal              = GOAL_DISBANDING;

    while (grp.member_count != 0) {
        // Reload head_unit for the order call...
        const uint32_t unit_for_order = grp.head_unit;
        gc.unit_issue_default_order((uint16_t)player_id, (int32_t)unit_for_order);
        // ...and reload it AGAIN for the move, exactly as the assembly does (a second MOVZX from the
        // same field at 0x004eab1a) rather than reusing the value above -- see the header.
        const uint32_t unit_for_move = grp.head_unit;
        gc.group_member_move(player_id, group_index, 0, (int32_t)unit_for_move);
        ++rep.members_disbanded;
    }
    return rep;
}

// ---------------------------------------------------------------------------------------------
// llm_strat_ai_group_task_hold @0x004eab33. Task code 0xb. No parameters, no state touched -- the
// whole body is the inert stack-capacity probe (rule 6 of the translator brief). The hold behaviour
// itself runs elsewhere (llm_strat_ai_group_task_step, via llm_strat_ai_group_all_units_settled).
// ---------------------------------------------------------------------------------------------
void group_task_hold(const ai_view &v, const ai_store &own, const ai_calls &gc) {
    (void)v;
    (void)own;
    (void)gc;
}

// ---------------------------------------------------------------------------------------------
// llm_strat_ai_group_task_recall_home @0x004eab3e. Task code 8.
// ---------------------------------------------------------------------------------------------
group_task_lifecycle_report group_task_recall_home(const ai_view &v, const ai_store &own,
                                                   const ai_calls &gc, int32_t player_id,
                                                   int32_t group_index) {
    (void)v;
    group_task_lifecycle_report rep{};
    unit_group                 &grp = own.players[player_id].ai_groups[group_index];

    // Drain any already-queued sub-tasks first (task_queue_count, offset 0x12 -- the field the .c
    // draft's plate correctly names here, unlike disband's). Re-read through `grp` every iteration
    // rather than caching the count, matching the assembly's own reload at LAB_004eab56: the ORIGINAL
    // llm_strat_ai_group_task_dequeue is what shrinks it, and nothing here may assume it is stable
    // across that call.
    while (grp.task_queue_count != 0) {
        gc.group_task_dequeue(player_id, group_index);
        ++rep.queue_drained;
    }

    uint32_t home_x = 0, home_y = 0; // committed llm_strat_ai_pick_owned_tile_or_home out-params
    gc.pick_owned_tile_or_home(player_id, &home_x, &home_y);

    // Move (task_code 0xc = advance_to_anchor, sub_code 0x183) to the resolved tile, then enqueue
    // AI_GROUP_TASK_RECRUIT_FROM_STORAGE (task_code 9, all-zero params) -- NOT a generic "settle"
    // task, which is what the exported .c's plate calls it: ACTIVATE_ARMS[9] is
    // ARM_RECRUIT_FROM_STORAGE (ai_group_task_machine.cpp), the same constant ai_state.h already
    // declares for the disband-pass gate. The original's tail `JMP 0x004ea069` after this second
    // enqueue is a shared Watcom epilogue, not a call -- it means return (same pattern as
    // ai_turret_threat.cpp's tail jump).
    gc.group_task_enqueue(player_id, group_index, TASK_ADVANCE_TO_ANCHOR, 0x183, home_x,
                          home_y, 0, 0, 0);
    gc.group_task_enqueue(player_id, group_index, AI_GROUP_TASK_RECRUIT_FROM_STORAGE, 0, 0, 0, 0, 0,
                          0);
    return rep;
}

// ---------------------------------------------------------------------------------------------
// llm_strat_ai_group_task_rally_formup @0x004eaea5. Task code 2 (ACTIVATE_ARMS[2] ==
// ARM_RALLY_FORMUP, ai_group_task_machine.cpp). Tail-call thunk -- see the header.
// ---------------------------------------------------------------------------------------------
void group_task_rally_formup(const ai_view &v, const ai_store &own, const ai_calls &gc,
                             int32_t player_id, int32_t group_index) {
    (void)v;
    (void)own;
    gc.group_rally_formup_worker(player_id, group_index);
}

// ---------------------------------------------------------------------------------------------
// llm_strat_ai_group_task_scatter_random @0x004eaeb4. Task code 0xd. Forwarding thunk -- see the
// header. The third argument (0xd) is fixed, matching this task's own code.
// ---------------------------------------------------------------------------------------------
void group_task_scatter_random(const ai_view &v, const ai_store &own, const ai_calls &gc,
                               int32_t player_id, int32_t group_index) {
    (void)v;
    (void)own;
    gc.group_scatter_random_worker(player_id, group_index, SCATTER_CLASS_OR_RADIUS);
}

} // namespace detail

void group_task_disband(uint32_t player_id, int32_t group_index) {
    const ai_state st = state();
    (void)detail::group_task_disband(st.read, st.own, live_calls(), player_id, group_index);
}
void group_task_hold() {
    const ai_state st = state();
    detail::group_task_hold(st.read, st.own, live_calls());
}
void group_task_recall_home(int32_t player_id, int32_t group_index) {
    const ai_state st = state();
    (void)detail::group_task_recall_home(st.read, st.own, live_calls(), player_id, group_index);
}
void group_task_rally_formup(int32_t player_id, int32_t group_index) {
    const ai_state st = state();
    detail::group_task_rally_formup(st.read, st.own, live_calls(), player_id, group_index);
}
void group_task_scatter_random(int32_t player_id, int32_t group_index) {
    const ai_state st = state();
    detail::group_task_scatter_random(st.read, st.own, live_calls(), player_id, group_index);
}

// ---- the differential-oracle arms -------------------------------------------------------------

} // namespace mh::ai
