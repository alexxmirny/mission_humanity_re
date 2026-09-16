//
// ai/ai_group_task_recruit.cpp -- see ai_group_task_recruit.h. Translated from the DISASSEMBLY, not
// from the exported .c drafts (both were cross-checked field by field against the .asm's literal
// offsets rather than trusted wholesale -- see the header's per-function notes).
//
#include "ai/ai_group_task_recruit.h"


namespace mh::ai {
namespace detail {

void group_task_muster_from_pool(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                 uint32_t player_id, int32_t group_index) {
    unit_group &grp = own.players[player_id].ai_groups[group_index];

    // Fold reinforce_pending into active_sub_code and clear it (0x004ea49e-0x004ea4ac): the 16-bit
    // ADD's result -- the POST-fold active_sub_code -- is what gates the whole body (JZ tests it, not
    // the pre-fold value).
    grp.active_sub_code    = (int16_t)(grp.active_sub_code + grp.reinforce_pending);
    const int16_t sub_code = grp.active_sub_code;
    grp.reinforce_pending  = 0;
    if (sub_code == 0) return; // 0x004ea4b5 JZ 0x004ea069

    uint32_t unit_index = 0;
    int32_t  src_group  = 0;
    if (v.players[player_id].ai_groups[2].member_count == 0) {
        // Pool 2 is empty -- fall back to pool 0. Give up entirely if that is empty too.
        if (v.players[player_id].ai_groups[0].member_count == 0) {
            grp.active_sub_code = 0; // LAB_004ea5cb
            return;
        }
        if (grp.task_code == 0x10)
            unit_index = gc.group_find_slowest_unit(player_id, 0);
        else if (grp.task_code == 0x11)
            unit_index = gc.group_pick_best_weapon_unit(player_id, 0);
        else
            unit_index = v.players[player_id].ai_groups[0].head_unit;
        src_group = 0;
        // NOTE: unlike the pool-2 arm below, this fallback issues NO move order -- the original jumps
        // straight from LAB_004ea5c2 into the shared group_member_move call at 0x004ea554, skipping the
        // active_param_a check and both order calls entirely. Not a "should be symmetric" bug to fix.
    } else {
        if (grp.task_code == 0x10)
            unit_index = gc.group_find_slowest_unit(player_id, 2);
        else if (grp.task_code == 0x11)
            unit_index = gc.group_pick_best_weapon_unit(player_id, 2);
        else
            unit_index = v.players[player_id].ai_groups[2].head_unit;
        if (grp.active_param_a == -1) {
            gc.unit_issue_default_order((uint16_t)player_id, unit_index);
        } else {
            gc.unit_flag_and_move(player_id, unit_index, (uint32_t)grp.active_param_a,
                                  (uint32_t)grp.active_param_b);
        }
        src_group = 2;
    }
    gc.group_member_move(player_id, src_group, group_index, unit_index);
    --grp.active_sub_code;
}

void group_task_recruit_from_pool3(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                   uint32_t player_id, int32_t group_index) {
    unit_group &grp = own.players[player_id].ai_groups[group_index];

    grp.active_sub_code    = (int16_t)(grp.active_sub_code + grp.reinforce_pending);
    const int16_t sub_code = grp.active_sub_code;
    grp.reinforce_pending  = 0;

    // Both short-circuited to the SAME reset target in the original (0x004ea627 / 0x004ea631 ->
    // LAB_004ea670) -- reproduced as `||`, sub_code checked first.
    if (sub_code == 0 || v.players[player_id].ai_groups[3].member_count == 0) {
        grp.active_sub_code = 0;
        return;
    }
    const uint32_t unit_id = v.players[player_id].ai_groups[3].head_unit;
    gc.unit_launch_from_storage_enqueue((uint8_t)player_id, unit_id, (uint32_t)grp.active_param_a,
                                        (uint32_t)grp.active_param_b);
    gc.group_member_move(player_id, 3, group_index, unit_id);
    --grp.active_sub_code;
}

void group_task_recruit_from_pool4(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                   uint32_t player_id, int32_t group_index) {
    unit_group &grp = own.players[player_id].ai_groups[group_index];

    grp.active_sub_code    = (int16_t)(grp.active_sub_code + grp.reinforce_pending);
    const int16_t sub_code = grp.active_sub_code;
    grp.reinforce_pending  = 0;

    if (sub_code == 0 || v.players[player_id].ai_groups[4].member_count == 0) {
        grp.active_sub_code = 0;
        return;
    }
    const uint32_t unit_id = v.players[player_id].ai_groups[4].head_unit;
    gc.unit_launch_from_storage_enqueue((uint8_t)player_id, unit_id, (uint32_t)grp.active_param_a,
                                        (uint32_t)grp.active_param_b);
    gc.group_member_move(player_id, 4, group_index, unit_id);
    --grp.active_sub_code;
}

void group_task_recruit_from_storage(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                     uint32_t player_id, int32_t group_index) {
    unit_group &grp = own.players[player_id].ai_groups[group_index];

    // local_1c: zero-extended (MOVZX, not sign-extended) even though member_count is int16_t.
    int32_t remaining     = (int32_t)(uint16_t)grp.member_count;
    grp.reinforce_pending = 0;
    grp.goal              = 9;

    // aiStackY_84[0x19]: purely local per-call scratch (docked reservations made THIS call), matching
    // the original's own stack array -- nothing in ai_view/ai_store models it.
    int32_t reserved_this_call[UNIT_STORAGE_SLOTS_PER_PLAYER] = {};

    while (remaining != 0) {
        int32_t matched_slot = -1; // local_20

        for (int32_t slot = 1; slot < UNIT_STORAGE_SLOTS_PER_PLAYER; ++slot) {
            const unit_storage_slot &bay =
                v.unit_storage[player_id * UNIT_STORAGE_SLOTS_PER_PLAYER + slot];
            if (bay.b_index == 0) continue;

            // UNSIGNED compare here (CMP/JNC @0x004ea7be/0x004ea7c1) -- see the header note on why
            // this differs from the sibling route_unit_to_home_storage's signed JGE on the same field.
            const uint32_t projected = (uint32_t)bay.docked_count + (uint32_t)reserved_this_call[slot];
            if (projected >= 0x32u) continue;

            const uint16_t bldg_type = building_of(v, player_id, bay.b_index).building_id;
            const uint16_t head_unit = grp.head_unit;
            bool           matched   = false;
            if (bldg_type == v.players[player_id + 1].ai_housing_candidate_vehicle) {
                matched = gc.unit_is_ai_ground((uint16_t)player_id, head_unit) != 0;
            }
            if (!matched && bldg_type == v.players[player_id + 1].ai_housing_candidate_heli) {
                matched = gc.unit_is_ai_heli(player_id, head_unit) != 0;
            }
            if (!matched && bldg_type == v.players[player_id + 1].ai_housing_candidate_plane) {
                matched = gc.unit_is_ai_plane(player_id, head_unit) != 0;
            }
            if (!matched && bldg_type == v.players[player_id + 1].ai_housing_candidate_soldier) {
                matched = gc.unit_is_ai_soldier(player_id, head_unit) != 0;
            }
            if (matched) {
                matched_slot = slot;
                break;
            }
        }

        if (matched_slot == -1) {
            const uint16_t head_unit = grp.head_unit;
            // ground-or-soldier -> dst 2 (ground checked first here, unlike the soldier-first order in
            // route_unit_to_home_storage -- confirmed independently off 0x004ea9de/0x004ea9f0).
            if (gc.unit_is_ai_ground((uint16_t)player_id, head_unit) != 0 ||
                gc.unit_is_ai_soldier(player_id, head_unit) != 0) {
                gc.group_member_move(player_id, group_index, 2, head_unit);
            } else if (gc.unit_is_ai_plane(player_id, head_unit) != 0) {
                gc.group_member_move(player_id, group_index, 4, head_unit);
            } else if (gc.unit_is_ai_heli(player_id, head_unit) != 0) {
                gc.group_member_move(player_id, group_index, 3, head_unit);
            }
            // else: head_unit matches none of the four classifiers -- NO move at all (0x004eaa54 JZ
            // straight to the loop tail). Reproduced, not "fixed".
        } else {
            ++reserved_this_call[matched_slot];
            gc.unit_group_assign_by_type(player_id, group_index, grp.head_unit,
                                         (uint32_t)matched_slot);
        }
        --remaining;
    }
}

} // namespace detail

void group_task_muster_from_pool(uint32_t player_id, int32_t group_index) {
    const ai_state st = state();
    detail::group_task_muster_from_pool(st.read, st.own, live_calls(), player_id, group_index);
}
void group_task_recruit_from_pool3(uint32_t player_id, int32_t group_index) {
    const ai_state st = state();
    detail::group_task_recruit_from_pool3(st.read, st.own, live_calls(), player_id, group_index);
}
void group_task_recruit_from_pool4(uint32_t player_id, int32_t group_index) {
    const ai_state st = state();
    detail::group_task_recruit_from_pool4(st.read, st.own, live_calls(), player_id, group_index);
}
void group_task_recruit_from_storage(uint32_t player_id, int32_t group_index) {
    const ai_state st = state();
    detail::group_task_recruit_from_storage(st.read, st.own, live_calls(), player_id, group_index);
}


} // namespace mh::ai
