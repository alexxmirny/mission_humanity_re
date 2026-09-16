//
// sim/sim_unit_state_exit.cpp -- see sim_unit_state_exit.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_unit_state_exit_{storage_begin,wait,cancel,walk_out}_*.asm) -- see the
// header banner for the field-offset derivation, the two CORRECTIONs to the batch hazard note (the
// "capacity field" misreading and the exit_walk_out ==2/else arm swap), and the declared-need
// boot-constant doubles.
//
#include "sim/sim_unit_state_exit.h"

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_state_exit_calls &live_unit_state_exit_calls() {
    static const unit_state_exit_calls c = {
        MH_LIBMH_BIND(llm_strat_storage_can_exit),
        MH_LIBMH_BIND(llm_strat_storage_place_exit_ground),
        MH_LIBMH_BIND(llm_strat_path_find_free_slot),
        MH_LIBMH_BIND(llm_strat_storage_exit_air),
        MH_LIBMH_BIND(llm_strat_unit_set_state),
        mh::state::evt::text_race_alert,
        MH_LIBMH_BIND(llm_strat_storage_type_accepts_unit),
        MH_LIBMH_BIND(llm_strat_dir_step_factor),
        MH_LIBMH_BIND(map_fow_UpdateFoWPlus),
        MH_LIBMH_BIND(llm_strat_ai_group_member_count_adjust),
        MH_LIBMH_BIND(llm_strat_unit_set_state_order),
    };
    return c;
}

namespace detail {

void unit_state_exit_storage_begin(const sim_view &v, sim_store &own, const unit_state_exit_calls &c) {
    unit          &u            = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced.
    const uint16_t player       = *v.cur_player;
    const int32_t  unit_index   = static_cast<int32_t>(*v.cur_index);
    const int32_t  storage_slot = static_cast<int32_t>(u.home_storage_slot);

    // 0x0047efee-0x0047f003.
    const int32_t can_exit = c.storage_can_exit(static_cast<int32_t>(player),
                                                static_cast<uint32_t>(unit_index), storage_slot);

    if (can_exit == 0) {
        // 0x0047f0b7-0x0047f266: still blocked -- optionally warn on a housing shortfall, then queue
        // at the door and transition to EXIT_WAIT.
        int32_t         reserved_passengers = 0;
        const building &b                   = building_of(v, player, storage_of(v, player, storage_slot).b_index);
        const uint8_t   bldg_type           = v.cfg_buildings[b.building_id].type;
        if (bldg_type == BUILDING_TYPE_H_SHUTTLE || bldg_type == BUILDING_TYPE_A_SHUTTLE) {
            // 0x0047f148-0x0047f199.
            reserved_passengers =
                v.prod_shuttle_slots[player * PROD_SHUTTLE_SLOTS_PER_PLAYER + b.shuttle_slot]
                    .passengers_reserved;
        }
        // 0x0047f19c-0x0047f23b: local-player housing-shortfall alert (no soldiers aboard to offset
        // the loss).
        if (player == static_cast<uint16_t>(*v.player_side) &&
            v.population[player].human + reserved_passengers < v.cfg_units[u.unit_proto_id].human &&
            v.cfg_units[u.unit_proto_id].soldier_count < 1) {
            c.race_alert_text_emit();
        }
        // 0x0047f240-0x0047f266.
        own.storage_at(player, storage_slot).door_waiter_count += 1;
        c.unit_set_state(UNIT_STATE_EXIT_WAIT);
        return;
    }

    // 0x0047f009-0x0047f0b2: can exit now -- claim the door and launch via the ground or air path.
    own.storage_at(player, storage_slot).door_mutex_unit = unit_index;

    // CORRECTION (see header banner): this is `cfg_units[unit_proto_id].type <= UNIT_TYPE_A_GROUND`,
    // not a "capacity" field.
    if (v.cfg_units[u.unit_proto_id].type <= UNIT_TYPE_A_GROUND) {
        // 0x0047f09c-0x0047f0b2.
        c.storage_place_exit_ground(player, unit_index, storage_slot);
        return;
    }

    // 0x0047f044-0x0047f09a: an aircraft-class unit -- needs a free air path slot.
    const int32_t free_slot = c.path_find_free_slot(static_cast<int32_t>(player));
    if (free_slot < 0) {
        // 0x0047f074-0x0047f09a: no slot -- queue at the door instead (same tail as the can_exit==0
        // path above).
        own.storage_at(player, storage_slot).door_waiter_count += 1;
        c.unit_set_state(UNIT_STATE_EXIT_WAIT);
        return;
    }
    c.storage_exit_air(player, unit_index, storage_slot, static_cast<uint32_t>(free_slot));
}

void unit_state_exit_wait(const sim_view &v, sim_store &own, const unit_state_exit_calls &c) {
    unit          &u            = own.cur_unit();
    const uint16_t player       = *v.cur_player;
    const int32_t  unit_index   = static_cast<int32_t>(*v.cur_index);
    const int32_t  storage_slot = static_cast<int32_t>(u.home_storage_slot);

    // 0x0047f298-0x0047f2ab.
    const int32_t can_exit = c.storage_can_exit(static_cast<int32_t>(player),
                                                static_cast<uint32_t>(unit_index), storage_slot);

    if (can_exit == 0) {
        // 0x0047f2d7-0x0047f347: still can't exit -- burn the tick as an activity-clock bump. This
        // state never carries a budget shortfall forward, same unconditional-entry-drain idiom
        // sim_unit_state_attack_building.cpp documents.
        own.tick_budget() = 0.0;
        const building &b = building_of(v, player, storage_of(v, player, storage_slot).b_index);
        if (b.built_flags == BUILT_FLAGS_OPERATIONAL) {
            // 0x0047f31a-0x0047f334: the storage building is fully operational (connected AND
            // staffed).
            u.activity_clock = u.activity_clock + *v.exit_wait_operational_activity_bump;
        } else {
            // 0x0047f336-0x0047f347.
            u.activity_clock = u.activity_clock + *v.exit_wait_default_activity_bump;
        }
        return;
    }

    // 0x0047f2af-0x0047f2d5: door is free now -- release the wait slot and hand back to
    // EXIT_STORAGE_BEGIN to try again.
    own.storage_at(player, storage_slot).door_waiter_count -= 1;
    c.unit_set_state(UNIT_STATE_EXIT_STORAGE_BEGIN);
}

void unit_state_exit_cancel(const sim_view &v, sim_store &own, const unit_state_exit_calls &c) {
    unit          &u            = own.cur_unit();
    const uint16_t player       = *v.cur_player;
    const int32_t  storage_slot = static_cast<int32_t>(u.home_storage_slot);

    // 0x0047f369-0x0047f392: the whole function -- give up the door-wait slot and park.
    own.storage_at(player, storage_slot).door_waiter_count -= 1;
    c.unit_set_state(UNIT_STATE_PARKED);
}

void unit_state_exit_walk_out(const sim_view &v, sim_store &own, const unit_state_exit_calls &c) {
    unit           &u            = own.cur_unit();
    const uint16_t  player       = *v.cur_player;
    const int32_t   unit_index   = static_cast<int32_t>(*v.cur_index);
    const int32_t   storage_slot = static_cast<int32_t>(u.home_storage_slot);
    const int32_t   b_index      = storage_of(v, player, storage_slot).b_index;
    const building &b            = building_of(v, player, b_index);

    // CORRECTION (see header banner): the ==2 arm below is the microstep-move arm, and the fallthrough
    // (below it) is the pending_damage accumulate -- SWAPPED from the batch hazard note's description,
    // verified against the JZ opcode's own rel32 target and independently against the Ghidra .c
    // draft's structuring (both agree with each other and disagree with the note).
    if (b.online_state == 2) {
        // 0x0047f4ee-0x0047f55e: step_cost, gated on cfg soldier_count.
        double step_cost = c.dir_step_factor(static_cast<int32_t>(u.facing_target)) *
                           v.cfg_units[u.unit_proto_id].step_speed[player];
        if (v.cfg_units[u.unit_proto_id].soldier_count > 0) {
            // 0x0047f549-0x0047f55b: DECLARED NEED boot constant (see header banner).
            step_cost = step_cost * *v.exit_walk_out_soldier_step_scale;
        }

        if (own.tick_budget() < step_cost) {
            // 0x0047f56c-0x0047f591: insufficient budget -- standard carryover idiom (this arm's own
            // live tick_budget gate; NOT preceded by a zeroing -- that only happens in the OTHER arm
            // below, see its own comment).
            u.activity_clock -= own.tick_budget();
            own.tick_budget() = 0.0;
            return;
        }
        own.tick_budget() -= step_cost; // 0x0047f596-0x0047f5a5

        if (u.move_microstep < 0x1f) {
            // 0x0047f5aa-0x0047f5be: still mid sub-tile animation.
            u.move_microstep += 1;
            return;
        }

        // 0x0047f5c3-0x0047f691: sub-tile animation already at max -- commit the exit tile.
        const int32_t col                        = u.x;
        const int32_t row                        = u.y;
        own.tile_object_at(col, row).building    = static_cast<uint16_t>(unit_index);
        own.tile_object_at(col, row).class_owner = static_cast<uint8_t>(player | 0x80u);
        u.path_blocked_retry_count               = 0;

        c.map_fow_UpdateFoWPlus(player, static_cast<uint32_t>(col), static_cast<uint32_t>(row),
                                v.cfg_units[u.unit_proto_id].sight);

        own.storage_at(player, storage_slot).door_mutex_unit = 0;

        // storage_slot is passed as the callee's third ("group_or_type") argument, literally per the
        // asm -- not modeled further, "callees stay original" (see header banner).
        c.ai_group_member_count_adjust(player, static_cast<uint32_t>(unit_index),
                                       static_cast<uint32_t>(storage_slot), 0u);
        c.unit_set_state_order(UNIT_STATE_STOP_TO_DEFAULT, UNIT_STATE_STOP_TO_DEFAULT);
        return;
    }

    // 0x0047f402-0x0047f4e9: not phase 2 -- drain the tick budget (unused again in this arm) and gate
    // a pure float self-damage accumulate on whether the storage building still legitimately accepts
    // this unit.
    own.tick_budget() = 0.0;

    const int32_t accepts =
        c.storage_type_accepts_unit(static_cast<uint32_t>(b.building_id), u.unit_proto_id);
    if (accepts == 0 || b.building_id <= 0 || b.energy <= 0.0) {
        // 0x0047f4d5-0x0047f4e6: FLD energy; FADD pending_damage; FSTP -- reproduced in that literal
        // term order per the translator brief's FP-order rule (see header banner: preserve-bug-shaped,
        // do not editorialise).
        u.pending_damage = u.energy + u.pending_damage;
    }
    // else: accepted AND a real building AND that building is still alive -- no-op.
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

void unit_state_exit_storage_begin() {
    sim_state st = state();
    detail::unit_state_exit_storage_begin(st.read, st.own, live_unit_state_exit_calls());
}

void unit_state_exit_wait() {
    sim_state st = state();
    detail::unit_state_exit_wait(st.read, st.own, live_unit_state_exit_calls());
}

void unit_state_exit_cancel() {
    sim_state st = state();
    detail::unit_state_exit_cancel(st.read, st.own, live_unit_state_exit_calls());
}

void unit_state_exit_walk_out() {
    sim_state st = state();
    detail::unit_state_exit_walk_out(st.read, st.own, live_unit_state_exit_calls());
}


} // namespace mh::sim
