//
// ai/ai_group_expansion.cpp -- see ai_group_expansion.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_group_expansion_form_or_repurpose_004e769e.asm), not from Ghidra's C:
// the decompile mishandles the shared-tail jump (renders it as a `goto LAB_004e7925` with a
// synthetic `a2` variable standing in for the task_code that is really just whatever EBX already
// holds at the jump) and never resolves the register-provenance of the two output-pointer calls'
// argument order, which this file gets from the raw LEA/MOV sequence instead (see the .h).
//
#include "ai/ai_group_expansion.h"


namespace mh::ai {
namespace detail {

group_expansion_report group_expansion_form_or_repurpose(const ai_view &v, const ai_store &own,
                                                         const ai_calls &gc, uint32_t player_id) {
    group_expansion_report rep{};
    player_data           &pd = own.players[player_id];

    // Scan existing groups for one already on the expansion mission (goal 3). A non-matching goal
    // just advances to the next group (0x004e76c2 JNZ / 0x004e7780 INC) -- there is no early break
    // on "found", only on an ACTIONABLE shortage state once one is found.
    for (uint32_t g = 0; g < (uint32_t)pd.ai_group_count; ++g) {
        ++rep.groups_scanned;
        if (pd.ai_groups[g].goal != 3) continue;

        if (pd.ai_resource_shortage_state == 2) {
            // 0x004e76d1/0x004e76d8: still critically short -- leave the existing group alone,
            // do nothing at all.
            rep.repurpose_blocked = true;
            return rep;
        }
        if (pd.ai_resource_shortage_state == 3) {
            // 0x004e76de/0x004e76e5 fall through to LAB_004e76eb: repurpose this group.
            rep.repurposed       = true;
            rep.repurposed_group = (int32_t)g;

            // task_queue_count is RE-READ every iteration (the original recomputes the whole
            // player-record base each time too, at LAB_004e76eb; re-reading through `pd` here is
            // the same observable effect without reproducing that redundant arithmetic).
            while (pd.ai_groups[g].task_queue_count != 0) {
                gc.group_task_dequeue((int32_t)player_id, (int32_t)g);
                ++rep.repurpose_dequeues;
            }
            // rally_formup (2), all zero.
            gc.group_task_enqueue((int32_t)player_id, (int32_t)g, 2, 0x183, 0, 0, 0, 0, 0);
            ++rep.tasks_enqueued;
            // advance_to_anchor (0xc), args = the player's own home tile.
            gc.group_task_enqueue((int32_t)player_id, (int32_t)g, 0xc, 0x183,
                                  (uint32_t)pd.ai_home_tile_x, (uint32_t)pd.ai_home_tile_y, 0, 0, 0);
            ++rep.tasks_enqueued;
            // disband (0x14) -- the SHARED TAIL at 0x004e7925, also used by this function's form
            // path below (task 8) and by llm_strat_ai_invasion_launch_attack_group (task 0). Same
            // physical CALL, different task_code already loaded before the jump; see the .h.
            gc.group_task_enqueue((int32_t)player_id, (int32_t)g, AI_GROUP_TASK_DISBAND, 0x183, 0, 0,
                                  0, 0, 0);
            ++rep.tasks_enqueued;
            return rep;
        }
        // shortage_state is 0 or 1: this match triggers nothing; keep scanning (0x004e76de JNZ).
    }

    // No repurposable group. The FORM path is gated on the SAME field, but this time only 2 (not 3)
    // proceeds -- 0x004e77a3/0x004e77aa.
    if (pd.ai_resource_shortage_state != 2) {
        rep.no_existing_match_wrong_state = true;
        return rep;
    }

    // Weakest HOSTILE opponent: ai_player_relation <= -1 (the original tests `> -1` and skips;
    // equivalent since the field is only ever initialised to +1 or -1 -- see the struct comment).
    // First qualifying candidate is taken unconditionally; thereafter only a STRICTLY weaker one
    // (UNSIGNED compare) replaces it -- the same idiom as ai_invasion.cpp / ai_army_milestone.cpp.
    int32_t weakest = -1;
    for (uint32_t q = 0; q < (uint32_t)*v.active_player_count; ++q) {
        if (q == player_id) continue;
        if (pd.ai_player_relation[q] > -1) continue;
        ++rep.opponents_scanned;
        if (weakest == -1 || (uint32_t)pd.ai_opponent_assessments[q].total_unit_power <
                                 (uint32_t)pd.ai_opponent_assessments[weakest].total_unit_power) {
            weakest = (int32_t)q;
        }
    }
    rep.weakest_opponent = weakest;
    if (weakest == -1) {
        rep.no_opponent_found = true;
        return rep;
    }

    // 80% of the combined headcount of SEED groups 0 and 2 (ai_group_form.h). Both fields are read
    // MOVZX in the original even though member_count is a signed int16_t -- reproduced with an
    // explicit zero-extending cast rather than the field's own signed type.
    const int32_t sum = (int32_t)(uint16_t)pd.ai_groups[0].member_count +
                        (int32_t)(uint16_t)pd.ai_groups[2].member_count;
    const int32_t target_member_count = (sum * 80) / 100;
    rep.target_member_count           = target_member_count;
    if (target_member_count == 0) {
        rep.zero_member_target = true;
        return rep;
    }

    // Two OUTPUT-POINTER calls. Argument order read off the raw LEA/MOV sequence (Watcom register
    // order EAX,EDX,EBX,ECX for params 1..4): both write unconditionally, so the locals need no
    // defensive value beyond satisfying the compiler.
    uint32_t mother_x = 0, mother_y = 0; // committed llm_strat_bldg_find_mother_position_indexed out-params
    gc.bldg_find_mother_position_indexed(weakest, &mother_x, &mother_y);
    uint32_t home_x = 0, home_y = 0; // committed llm_strat_ai_pick_owned_tile_or_home out-params
    gc.pick_owned_tile_or_home((int32_t)player_id, &home_x, &home_y);

    const int32_t group_index = gc.group_create((int32_t)player_id);
    rep.group_index           = group_index;
    if (group_index == -1) { // 0x004e787f -- a real early return, unlike the two form.h siblings
                             // that use group_create's result unchecked.
        rep.create_failed = true;
        return rep;
    }

    pd.ai_groups[group_index].goal                = 3;
    pd.ai_groups[group_index].target_player_id    = weakest; // FULL DWORD -- see the field comment.
    pd.ai_groups[group_index].active_member_count = (uint16_t)target_member_count;

    // muster_from_pool (0x11): args = the player's own tile (home_x, home_y), p9 = the target
    // headcount, truncated to uint16_t by group_task_enqueue's own committed prototype.
    gc.group_task_enqueue((int32_t)player_id, group_index, 0x11, 0x183, home_x,
                          home_y, 0, 0, (uint16_t)target_member_count);
    ++rep.tasks_enqueued;
    // DISCARDED result -- pure side effect, advances the AI PRNG (channel 2). Do NOT drop this
    // call: the original consumes exactly one draw here regardless of the value (EAX is
    // overwritten four instructions later, at 0x004e78eb), and dropping it would consume one
    // fewer draw than the original and desync the sim this translation is measured inside.
    (void)gc.rand_below_ai(4);
    // advance_to_anchor (3): args = the mother-building tile of the WEAKEST opponent (not the
    // player's own).
    gc.group_task_enqueue((int32_t)player_id, group_index, 3, 0x19b, (uint32_t)mother_x,
                          (uint32_t)mother_y, 0, 0, 0);
    ++rep.tasks_enqueued;
    // nudge_stragglers (6): same anchor, p9 = 15 (a literal, not read from anywhere).
    gc.group_task_enqueue((int32_t)player_id, group_index, 6, 0x19b, (uint32_t)mother_x,
                          (uint32_t)mother_y, 0, 0, 15);
    ++rep.tasks_enqueued;
    // recall_home (8) -- the SHARED TAIL at 0x004e7925 (see the repurpose branch above and the
    // .h): all zero.
    gc.group_task_enqueue((int32_t)player_id, group_index, 8, 0x183, 0, 0, 0, 0, 0);
    ++rep.tasks_enqueued;

    rep.formed = true;
    return rep;
}

} // namespace detail

void group_expansion_form_or_repurpose(uint32_t player_id) {
    const ai_state st = state();
    (void)detail::group_expansion_form_or_repurpose(st.read, st.own, live_calls(), player_id);
}


} // namespace mh::ai
