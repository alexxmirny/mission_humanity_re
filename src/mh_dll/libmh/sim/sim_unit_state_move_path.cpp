//
// sim/sim_unit_state_move_path.cpp -- see sim_unit_state_move_path.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_unit_state_move_path_00480b53.asm) -- see the header banner for the
// parameter-order derivation, the cfg-type-enum note, the fine_to_tile convention choice, and the
// unbacked-order/state-literal derivation.
//
#include "sim/sim_unit_state_move_path.h"

#include "addr/mh_calls.gen.h"     // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"           // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_order_enqueue.h" // UNIT_STATE_IDLE_SCATTER, UNIT_STATE_HOVER_ENGAGE,
#include "addr/mh_rebind.gen.h"    // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
// UNIT_TYPE_A_PLANE, UNIT_TYPE_H_PLANE (shared there)

namespace mh::sim {

const unit_state_move_path_calls &live_unit_state_move_path_calls() {
    static const unit_state_move_path_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_chase_check),
        MH_LIBMH_BIND(llm_strat_path_free_slot),
        MH_LIBMH_BIND(llm_strat_unit_notify_status),
        MH_LIBMH_BIND(llm_strat_unit_set_state),
        MH_LIBMH_BIND(llm_strat_unit_set_state_order),
        MH_LIBMH_BIND(llm_strat_target_class),
        MH_LIBMH_BIND(llm_strat_unit_in_weapon_range),
        MH_LIBMH_BIND(llm_strat_storage_get_approach_tile),
        MH_LIBMH_BIND(llm_strat_unit_unlink_tile),
        MH_LIBMH_BIND(llm_strat_fow_remove_sight),
        MH_LIBMH_BIND(map_unit_PutOnMap),
        MH_LIBMH_BIND(map_fow_UpdateFoWPlus),
    };
    return c;
}

namespace detail {

// ---- llm_strat_unit_state (order/state) literals -- NOT backed by a real Ghidra enum (see the
// header banner). Own local constants (MOVE_PATH_ prefix), matching every sibling TU's identical
// convention for this field. IDLE_SCATTER/HOVER_ENGAGE/UNIT_TYPE_A_PLANE/UNIT_TYPE_H_PLANE come from
// sim_order_enqueue.h instead (already shared there).
inline constexpr uint16_t MOVE_PATH_ORDER_DEPLOY_APPROACH    = 0x18; // 0x00480df3
inline constexpr uint16_t MOVE_PATH_ORDER_ATTACK_UNIT        = 0x1a; // 0x00480c4e/0x00480ee2 range test
inline constexpr uint16_t MOVE_PATH_ORDER_ATTACK_UNIT_RETURN = 0x1b; // 0x00480c5a
inline constexpr uint16_t MOVE_PATH_ORDER_ATTACK_BUILDING    = 0x1c; // 0x00480c68, also the [0x1a,0x1c] range top
inline constexpr uint16_t MOVE_PATH_ORDER_LANDING_REQUEST    = 0x29; // 0x00480daa
inline constexpr uint16_t MOVE_PATH_STATE_GROUP_STEP         = 0x0b; // 0x00480e57 (set_state_order's state arg)
inline constexpr uint16_t MOVE_PATH_STATE_MOVE_PATH_PLANE    = 0x2c; // 0x00480e07/0x00480ee7
inline constexpr uint16_t MOVE_PATH_STATE_PLOT_TURN_PATH     = 0x2d; // 0x00480efa (this batch's sibling state)
inline constexpr uint16_t MOVE_PATH_ORDER_ASCEND_TO_ORBIT    = 0x31; // 0x00480fe8

// llm_strat_unit_notify_status's status-code literals this function passes -- also unbacked (no
// Ghidra enum), named locally per the same convention as sim_unit_state_move_walker.cpp's own set.
inline constexpr uint32_t MOVE_PATH_NOTIFY_GOAL_REACHED = 0x65; // path exhausted, path slot released
inline constexpr uint32_t MOVE_PATH_NOTIFY_REPLAN       = 1;    // move_op_arg / not-in-range fallback

// fine->tile: SIMPLIFIED to plain truncating `/ 32`, per this codebase's GENERAL convention (the
// SIM1-G2 batch context carries no shift-form override for this function, unlike move_walker's) --
// see the header banner for the cross-TU equivalence precedent this relies on.
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }

void unit_state_move_path(const sim_view &v, sim_store &own, const unit_state_move_path_calls &c) {
    unit          &u          = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced.
    const uint16_t player     = *v.cur_player;
    const int32_t  unit_index = static_cast<int32_t>(*v.cur_index);

    // 0x00480b6b-0x00480b9a: a queued order is about to replace this state -- do nothing this tick.
    if (u.move_microstep == 0x1f && u.order_queued != 0) {
        own.tick_budget() = 0.0;
        return;
    }

    // 0x00480b9f-0x00480c02: step_cost = cfg step_speed[proto][player], scaled by move_heading. NOT a
    // call to llm_strat_dir_step_factor -- this function inlines its OWN two-bucket scale directly.
    // DECLARED NEED: move_path_heading_scale_far/_mid have no sim_view binding yet (see header banner).
    double step_cost = v.cfg_units[u.unit_proto_id].step_speed[player];
    if (u.move_heading > 3) {
        if (u.move_heading <= 7) {
            step_cost *= *v.move_path_heading_scale_mid; // DAT_00501420, 0x00480bf6-0x00480bff
        } else {
            step_cost *= *v.move_path_heading_scale_far; // DAT_00501418, 0x00480be8-0x00480bf1
        }
    }

    // 0x00480c02-0x00480c35: the standard tick-budget carryover gate.
    if (own.tick_budget() < step_cost) {
        u.activity_clock -= own.tick_budget();
        own.tick_budget() = 0.0;
        return;
    }
    own.tick_budget() -= step_cost; // 0x00480c3a-0x00480c49

    // 0x00480c49-0x00480c79: cache llm_strat_unit_chase_check()'s return, gated on order being one of
    // the three ATTACK_* values. See the header banner's ARM-SAFETY note -- this call is why this
    // function's shadow closure is not arm-safe. DECLARED NEED: own.unit_chase_result_mut() (see
    // header banner) -- not otherwise read anywhere in THIS function.
    if (u.order == MOVE_PATH_ORDER_ATTACK_UNIT || u.order == MOVE_PATH_ORDER_ATTACK_UNIT_RETURN ||
        u.order == MOVE_PATH_ORDER_ATTACK_BUILDING) {
        own.unit_chase_result_mut() = c.unit_chase_check();
    }

    // Shared table lookup: _G_LLM_STRAT_MOVE_MICROSTEPS[move_heading][move_microstep].facing, applied
    // to BOTH facing_target and facing_current -- used at 0x00480c92-0x00480ce9 (mid-animation) and
    // again at 0x00481117-0x0048116f (just after a real tile step, with move_microstep freshly 0).
    auto sync_facing_from_microstep_table = [&]() {
        const uint8_t facing =
            v.move_microsteps[static_cast<uint32_t>(u.move_heading) * MICROSTEPS_PER_HEADING +
                              static_cast<uint32_t>(u.move_microstep)]
                .facing;
        u.facing_target  = facing;
        u.facing_current = facing;
    };

    if (u.move_microstep < 0x1f) {
        // 0x00480c87-0x00480cec: mid sub-tile animation -- advance and re-sync facing, always return
        // (no settle-at-0x1f special case here, unlike move_walker).
        u.move_microstep += 1;
        sync_facing_from_microstep_table();
        return;
    }

    // ---- 0x00480cf1-...: settled at a tile boundary (move_microstep >= 0x1f) ---------------------
    const path_waypoint &wp =
        v.path_buffers[player * PATH_WAYPOINTS_PER_PLAYER + u.path_slot_id * PATH_WAYPOINTS_PER_SLOT +
                       u.path_cursor]; // UNADVANCED cursor, 0x00480cf1-0x00480d28
    const uint32_t heading = wp.heading;

    if (heading < 0x18) {
        // ---- 0x00481012-...: a real next step is queued -- take it ---------------------------------
        u.path_cursor += 1; // 0x00481017 (the OLD cursor's heading, captured above, drives this step)
        const uint32_t old_x = u.x;
        const uint32_t old_y = u.y;
        const int32_t  dx    = v.dir_step_offsets[heading].dx; // 0x0048103b-0x00481047
        const int32_t  dy    = v.dir_step_offsets[heading].dy; // 0x0048104a-0x00481056

        c.unit_unlink_tile(player, static_cast<uint16_t>(unit_index)); // 0x00481059-0x0048106c
        c.fow_remove_sight(player, static_cast<int32_t>(old_x), static_cast<int32_t>(old_y),
                           v.cfg_units[u.unit_proto_id].sight); // 0x0048106c-0x00481094

        const uint32_t new_x = map_width_mask(v) & (old_x + static_cast<uint32_t>(dx));  // 0x00481094-0x004810a2
        const uint32_t new_y = map_height_mask(v) & (old_y + static_cast<uint32_t>(dy)); // 0x004810a5-0x004810b3

        c.map_unit_PutOnMap(player, static_cast<uint16_t>(unit_index), static_cast<uint8_t>(new_x),
                            static_cast<uint8_t>(new_y)); // 0x004810b6-0x004810cc
        c.map_fow_UpdateFoWPlus(player, new_x, new_y,
                                v.cfg_units[u.unit_proto_id].sight); // 0x004810d1-0x004810f4

        u.move_microstep = 0;                             // 0x004810f9-0x00481108
        u.move_heading   = static_cast<uint8_t>(heading); // 0x00481108-0x00481117
        sync_facing_from_microstep_table();               // 0x00481117-0x0048116f, move_microstep now 0
        return;                                           // NO budget refund -- a real tile transition happened this tick.
    }

    // ---- 0x00480d35-...: heading >= 0x18 -- terminal/no-next-step sentinel -----------------------
    if (u.path_slot_id != 0xff) {
        c.path_free_slot(player, unit_index); // 0x00480d35-0x00480d56
    }
    c.unit_notify_status(player, unit_index, MOVE_PATH_NOTIFY_GOAL_REACHED); // 0x00480d56-0x00480d6e

    // Named `settle_order` rather than `order` -- `mh::sim::order` (the order-QUEUE record alias, see
    // sim_state.h) is a distinct name in this same namespace; shadowing it with a plain uint16_t local
    // would compile but reads badly next to that type.
    const uint16_t settle_order = u.order; // 0x00480d6e-0x00480d7a

    if (settle_order == MOVE_PATH_ORDER_DEPLOY_APPROACH) {
        // 0x00480daa-0x00480df8 (ord < 0x1a bucket, only 0x18 special).
        c.unit_set_state(MOVE_PATH_ORDER_DEPLOY_APPROACH);
    } else if (settle_order >= MOVE_PATH_ORDER_ATTACK_UNIT &&
               settle_order <= MOVE_PATH_ORDER_ATTACK_BUILDING) {
        // 0x00480e7e-0x00480f32: the unit's OWN cfg type (not the target's).
        const uint32_t proto_type = v.cfg_units[u.unit_proto_id].type;
        if (proto_type == UNIT_TYPE_A_PLANE || proto_type == UNIT_TYPE_H_PLANE) {
            // 0x00480ee2-0x00480f2d.
            if (u.state == MOVE_PATH_STATE_MOVE_PATH_PLANE) {
                c.unit_set_state(static_cast<uint16_t>(v.cfg_units[u.unit_proto_id].move_op_arg));
                c.unit_notify_status(player, unit_index, MOVE_PATH_NOTIFY_REPLAN);
            } else {
                c.unit_set_state(MOVE_PATH_STATE_PLOT_TURN_PATH);
            }
        } else {
            // 0x00480f32-0x00480fe6: weapon-range check against the primary target.
            const int32_t target_class =
                c.target_class(static_cast<uint32_t>(static_cast<uint16_t>(u.target_ref)),
                               static_cast<int32_t>(static_cast<uint16_t>(u.target_index)));
            const int32_t in_range =
                c.unit_in_weapon_range(player, unit_index, fine_to_tile(u.target_fine_x),
                                       fine_to_tile(u.target_fine_y), target_class);
            if (in_range == 0) {
                c.unit_set_state(static_cast<uint16_t>(v.cfg_units[u.unit_proto_id].move_op_arg));
                c.unit_notify_status(player, unit_index, MOVE_PATH_NOTIFY_REPLAN);
            } else {
                c.unit_set_state(UNIT_STATE_HOVER_ENGAGE);
            }
        }
    } else if (settle_order == MOVE_PATH_ORDER_LANDING_REQUEST) {
        // 0x00480e02-0x00480e79.
        if (u.state == MOVE_PATH_STATE_MOVE_PATH_PLANE) {
            if (u.home_storage_slot == 0) {
                c.unit_set_state(UNIT_STATE_IDLE_SCATTER);
            } else {
                uint32_t out_x = 0, out_y = 0;
                c.storage_get_approach_tile(player, static_cast<uint16_t>(unit_index), &out_x, &out_y, 0);
                u.goal_x = static_cast<uint8_t>(out_x);
                u.goal_y = static_cast<uint8_t>(out_y);
                // VERIFIED (state, order) parameter order -- see header banner. Writes
                // state=GROUP_STEP, order=LANDING_REQUEST (order re-written to its own current value).
                c.unit_set_state_order(MOVE_PATH_STATE_GROUP_STEP, MOVE_PATH_ORDER_LANDING_REQUEST);
            }
        } else {
            c.unit_set_state(MOVE_PATH_ORDER_LANDING_REQUEST); // writes STATE=0x29 here, not order.
        }
    } else if (settle_order == UNIT_STATE_HOVER_ENGAGE) {
        // 0x00480fe8's sibling default at LAB_00480de4 -- ord == HOVER_ENGAGE(0x2e) exactly.
        c.unit_set_state(UNIT_STATE_HOVER_ENGAGE);
    } else if (settle_order == MOVE_PATH_ORDER_ASCEND_TO_ORBIT) {
        // 0x00480fe8-0x00480ff2.
        c.unit_set_state(MOVE_PATH_ORDER_ASCEND_TO_ORBIT);
    } else {
        // 0x00480ff4-0x00480ffe: the default/fallback -- every other order value.
        c.unit_set_state(UNIT_STATE_IDLE_SCATTER);
    }

    // 0x00480ffe-0x0048100d: common tail for every arm of the order dispatch above -- refund step_cost
    // (no real tile movement happened this tick, unlike the heading<0x18 branch above).
    own.tick_budget() += step_cost;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_state_move_path() {
    sim_state st = state();
    detail::unit_state_move_path(st.read, st.own, live_unit_state_move_path_calls());
}


} // namespace mh::sim
