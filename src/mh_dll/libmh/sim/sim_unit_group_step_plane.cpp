//
// sim/sim_unit_group_step_plane.cpp -- see sim_unit_group_step_plane.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_unit_group_step_plane_00484b4a.asm) -- see the header banner
// for the pathfinder_air_mode_flag hazard (writes 1/0, NOT 2), the fine_to_tile form (established `/
// 32`, not the shift form), and the unbacked-enum literal derivation.
//
#include "sim/sim_unit_group_step_plane.h"

#include "addr/mh_calls.gen.h"     // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"           // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_order_enqueue.h" // UNIT_STATE_STOP_TO_DEFAULT, UNIT_STATE_IDLE_SCATTER (shared there)
#include "addr/mh_rebind.gen.h"    // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_group_step_plane_calls &live_unit_group_step_plane_calls() {
    static const unit_group_step_plane_calls c = {
        MH_LIBMH_BIND(llm_strat_path_find_free_slot),
        MH_LIBMH_BIND(llm_strat_path_free_slot),
        MH_LIBMH_BIND(llm_strat_target_release_ref),
        MH_LIBMH_BIND(llm_strat_unit_get_coords),
        MH_LIBMH_BIND(llm_strat_unit_set_state_order),
        MH_LIBMH_BIND(llm_strat_unit_set_state),
        MH_LIBMH_BIND(llm_strat_unit_get_ready_home_building),
        MH_LIBMH_BIND(llm_strat_trace_greedy_path),
        MH_LIBMH_BIND(llm_strat_pathtrace_remove_loops),
        MH_LIBMH_BIND(llm_strat_path_write_from_solver),
        MH_LIBMH_BIND(llm_strat_unit_notify_status),
    };
    return c;
}

namespace detail {

// ---- llm_strat_unit_state (order) literals -- NOT backed by a real Ghidra enum (see the header
// banner). Own local constants (GROUP_STEP_PLANE_ORDER_ prefix), matching every sibling TU's identical
// convention for this field. Values agree with sim_unit_state_move_walker.cpp's own
// MOVE_WALKER_STATE_ATTACK_UNIT/_ATTACK_UNIT_RETURN -- deliberately NOT shared between the two TUs,
// per that file's own stated reasoning for the identical situation.
inline constexpr uint16_t GROUP_STEP_PLANE_ORDER_ATTACK_UNIT        = 0x1a; // 0x00484bba
inline constexpr uint16_t GROUP_STEP_PLANE_ORDER_ATTACK_UNIT_RETURN = 0x1b; // 0x00484bc1
inline constexpr uint16_t GROUP_STEP_PLANE_ORDER_LANDING_REQUEST    = 0x29; // 0x00484d31

// llm_strat_unit_notify_status's status-code literal this function passes -- also unbacked (no Ghidra
// enum). Distinct from move_walker's 0x65/0x66/1 trio; this is a THIRD code, named locally per the
// same convention.
inline constexpr uint32_t GROUP_STEP_PLANE_NOTIFY_PATH_COMPUTED = 100; // 0x64, 0x00484dee

// fine->tile: the ESTABLISHED `/ 32` form (see the header banner on why this function does NOT use
// move_walker's shift-form override). Provably identical to the SAR/SHL/SBB/SAR sequence this
// function's own .asm emits at 0x00484c6b-0x00484c9c, per sim_order_enqueue.cpp's own precedent.
inline int32_t fine_to_tile(int32_t fine) { return fine / 32; }

void unit_group_step_plane(const sim_view &v, sim_store &own, const unit_group_step_plane_calls &c) {
    unit          &u          = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced.
    const uint16_t player     = *v.cur_player;
    const int32_t  unit_index = static_cast<int32_t>(*v.cur_index);

    uint8_t facing_override = 0xff; // 0x00484b62: local_1c = 0xff -- no facing override by default.

    // 0x00484b66: this function's own entry write is 1, NOT 2 -- see the header hazard note. Also NOT
    // matching group_step_ground's own write (0, not 1) -- both facts read directly off each
    // function's .asm, not inferred from the pre-existing (wrong) sim_state.h prose.
    own.pathfinder_air_mode_flag() = 1;

    const int32_t free_slot = c.path_find_free_slot(static_cast<int32_t>(player));
    if (free_slot == -1) {
        // 0x00484b85-0x00484b8f: no path slot available -- give up and idle.
        c.unit_set_state(UNIT_STATE_IDLE_SCATTER);
        return;
    }

    // 0x00484b94-0x00484bb5: the unit already held a path slot -- free it, it is about to be
    // reassigned to `free_slot` below.
    if (u.path_slot_id != 0xff) {
        c.path_free_slot(player, unit_index);
    }

    // 0x00484bb5-0x00484cf5: ATTACK_UNIT / ATTACK_UNIT_RETURN target re-validation.
    if (u.order == GROUP_STEP_PLANE_ORDER_ATTACK_UNIT || u.order == GROUP_STEP_PLANE_ORDER_ATTACK_UNIT_RETURN) {
        const uint32_t target_owner = ref_owner(static_cast<uint32_t>(u.target_ref));
        const uint16_t target_slot  = static_cast<uint16_t>(u.target_index);
        if (v.units[target_owner * v.caps.units + target_slot].energy <= 0.0) {
            // 0x00484cad-0x00484cf0: target is dead. UNCONDITIONAL release (no `target_ref != 0`
            // guard here, unlike move_walker's analogous cascade -- transcribed literally per the
            // .asm) then STOP_TO_DEFAULT and return; no notify_status call on this path.
            c.target_release_ref(player, unit_index, 1u);
            u.target_ref   = 0;
            u.target_index = 0;
            c.unit_set_state_order(UNIT_STATE_STOP_TO_DEFAULT, UNIT_STATE_STOP_TO_DEFAULT);
            return;
        }
        // 0x00484c18-0x00484cab: target still alive -- re-fetch its live fine position and recompute
        // goal_x/goal_y from it.
        c.unit_get_coords(static_cast<uint16_t>(target_owner), static_cast<int32_t>(target_slot),
                          &u.target_fine_x, &u.target_fine_y);
        u.goal_x = static_cast<uint8_t>(fine_to_tile(u.target_fine_x));
        u.goal_y = static_cast<uint8_t>(fine_to_tile(u.target_fine_y));
    }

    // 0x00484cf5-0x00484d31: snapshot x/y/goal_x/goal_y (post-target-revalidation values).
    const uint8_t x      = u.x;
    const uint8_t y      = u.y;
    const uint8_t goal_x = u.goal_x;
    const uint8_t goal_y = u.goal_y;

    // 0x00484d31-0x00484d81: LANDING_REQUEST with a ready home building -- constrain the path solve to
    // the building's facing, and drop out of air mode.
    if (u.order == GROUP_STEP_PLANE_ORDER_LANDING_REQUEST) {
        const int32_t home_building_idx = c.unit_get_ready_home_building();
        if (home_building_idx != 0) {
            const uint16_t building_id =
                v.buildings[player * v.caps.buildings + home_building_idx].building_id;
            facing_override                = v.cfg_buildings[building_id].door_approach_route[0];
            own.pathfinder_air_mode_flag() = 0; // 0x00484d77 -- the one conditional reset in this function.
        }
    }

    // 0x00484d81-0x00484e06: solve and commit the path.
    c.trace_greedy_path(x, y, static_cast<int32_t>(u.move_heading), goal_x, goal_y, facing_override);
    while (c.pathtrace_remove_loops() != 0) {
        // 0x00484dab-0x00484db2: drain until the trace has no more loops to remove.
    }
    c.path_write_from_solver(player, unit_index, x, y, free_slot);
    c.unit_set_state(static_cast<uint16_t>(v.cfg_units[u.unit_proto_id].move_op_code));
    c.unit_notify_status(player, unit_index, GROUP_STEP_PLANE_NOTIFY_PATH_COMPUTED);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_group_step_plane() {
    sim_state st = state();
    detail::unit_group_step_plane(st.read, st.own, live_unit_group_step_plane_calls());
}


} // namespace mh::sim
