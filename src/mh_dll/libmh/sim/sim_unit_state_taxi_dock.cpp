//
// sim/sim_unit_state_taxi_dock.cpp -- see sim_unit_state_taxi_dock.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_unit_state_takeoff_taxi_0047f69f.asm,
// tmp/decomp_sim/llm_strat_unit_state_dock_taxi_in_0047f981.asm,
// tmp/decomp_sim/llm_strat_unit_state_landing_request_004800a8.asm) -- see the header banner for the
// takeoff_taxi-vs-dock_taxi_in divergence notes, the dock_taxi_in shuttle-slot preserved-behaviour
// derivation, and the landing_request out-pointer resolution.
//
#include "sim/sim_unit_state_taxi_dock.h"

#include "addr/mh_calls.gen.h"     // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"           // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_order_enqueue.h" // UNIT_STATE_PARKED, UNIT_STATE_IDLE_SCATTER, BUILDING_TYPE_A_PORT,
#include "addr/mh_rebind.gen.h"    // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
// BUILDING_TYPE_H_PORT, BUILT_FLAGS_OPERATIONAL (all shared there)

namespace mh::sim {

const unit_state_takeoff_taxi_calls &live_unit_state_takeoff_taxi_calls() {
    static const unit_state_takeoff_taxi_calls c = {
        MH_LIBMH_BIND(llm_strat_dir_step_factor),
        MH_LIBMH_BIND(llm_strat_unit_set_state),
        MH_LIBMH_BIND(llm_strat_unit_takeoff_finalize),
    };
    return c;
}

const unit_state_dock_taxi_in_calls &live_unit_state_dock_taxi_in_calls() {
    static const unit_state_dock_taxi_in_calls c = {
        MH_LIBMH_BIND(llm_strat_dir_step_factor),
        MH_LIBMH_BIND(llm_strat_unit_set_state),
        MH_LIBMH_BIND(llm_strat_target_release_ref),
        MH_LIBMH_BIND(llm_strat_path_free_slot),
        MH_LIBMH_BIND(llm_strat_bldg_flush_cargo_hold),
        MH_LIBMH_BIND(llm_strat_prod_unbind_planet),
        MH_LIBMH_BIND(llm_strat_storage_scrap_home_docked_units),
    };
    return c;
}

const unit_state_landing_request_calls &live_unit_state_landing_request_calls() {
    static const unit_state_landing_request_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_get_ready_home_building),
        MH_LIBMH_BIND(llm_strat_unit_set_state_order),
        MH_LIBMH_BIND(llm_strat_tile_neighbor_in_dir),
        MH_LIBMH_BIND(llm_strat_storage_get_approach_tile),
        MH_LIBMH_BIND(llm_strat_storage_can_land),
        MH_LIBMH_BIND(llm_strat_path_find_free_slot),
        MH_LIBMH_BIND(llm_strat_unit_set_state),
        MH_LIBMH_BIND(llm_strat_storage_accept_landing),
    };
    return c;
}

namespace detail {

void unit_state_takeoff_taxi(const sim_view &v, sim_store &own,
                             const unit_state_takeoff_taxi_calls &c) {
    unit          &u            = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced -- see sim_view::cur_unit's comment.
    const uint16_t player       = *v.cur_player;
    const uint16_t unit_index   = *v.cur_index;
    const int32_t  storage_slot = u.home_storage_slot;

    // 0x0047f6b7-0x0047f6fa: the storage slot's owning building must be in its taxi-out-ready phase
    // (online_state==2) or this tick does nothing at all -- budget is drained but never spent.
    const building &b = building_of(v, player, storage_of(v, player, storage_slot).b_index);
    if (b.online_state != TAXI_DOCK_BLDG_ONLINE_STATE_TAXI_OUT_READY) {
        own.tick_budget() = 0.0;
        return;
    }

    // 0x0047f715-0x0047f753: step_cost = dir_step_factor(facing_target) * (step_speed[player] *
    // DAT_005013f0). EXACT x87 evaluation order preserved (two FMULs, this order) -- see the header
    // banner's DECLARED NEED for the unconfirmed *v.taxi_takeoff_step_speed_mult constant.
    const double base      = v.cfg_units[u.unit_proto_id].step_speed[player] * *v.taxi_takeoff_step_speed_mult;
    const double step_cost = c.dir_step_factor(static_cast<int32_t>(u.facing_target)) * base;

    if (own.tick_budget() < step_cost) {
        // 0x0047f761-0x0047f786: insufficient budget -- carry the shortfall into activity_clock,
        // drain tick_budget, and stop (same idiom as every other tick-budget-gated sim TU).
        u.activity_clock -= own.tick_budget();
        own.tick_budget() = 0.0;
        return;
    }
    own.tick_budget() -= step_cost; // 0x0047f78b-0x0047f794

    if (u.move_microstep < 0x1f) {
        // 0x0047f79a-0x0047f80d: mid sub-tile animation -- advance one microstep and copy the SAME
        // facing byte into BOTH facing_target and facing_current (two separate writes, per the batch
        // context's move_microsteps idiom note -- not one write duplicated by this translation).
        u.move_microstep += 1;
        const uint8_t facing =
            v.move_microsteps[u.move_heading * MICROSTEPS_PER_HEADING + u.move_microstep].facing;
        u.facing_target  = facing;
        u.facing_current = facing;
        return;
    }

    // 0x0047f812-...: settled at a tile boundary -- consult the next path waypoint (read via the
    // UNADVANCED cursor).
    const uint8_t heading =
        v.path_buffers[player * PATH_WAYPOINTS_PER_PLAYER + u.path_slot_id * PATH_WAYPOINTS_PER_SLOT +
                       u.path_cursor]
            .heading;
    if (heading == 0xff) {
        // 0x0047f84c-0x0047f976: path exhausted (0xff sentinel, NOT move_walker's 0-sentinel for this
        // same field name) -- taxi-out is complete.
        c.unit_set_state(TAXI_DOCK_STATE_TAKEOFF);
        c.unit_takeoff_finalize(player, unit_index);
        return;
    }

    // 0x0047f859-0x0047f958: a real next step -- consumed via _G_LLM_STRAT_DIR_STEP_OFFSET_TABLE
    // looked up by the WAYPOINT HEADING just read (NOT llm_strat_facing24_to_delta, NOT
    // u.move_heading's old value). Full 32-bit add-then-mask, truncated to the byte x/y fields only on
    // the final store -- see the header banner on why this is NOT byte-width arithmetic despite the
    // exported .c draft's misleading `(byte)general.width_mask & ...` rendering.
    u.path_cursor += 1;
    const int32_t new_x = (static_cast<int32_t>(u.x) + v.dir_step_offsets[heading].dx) &
                          static_cast<int32_t>(map_width_mask(v));
    const int32_t new_y = (static_cast<int32_t>(u.y) + v.dir_step_offsets[heading].dy) &
                          static_cast<int32_t>(map_height_mask(v));
    u.x              = static_cast<uint8_t>(new_x);
    u.y              = static_cast<uint8_t>(new_y);
    u.move_microstep = 0;
    u.move_heading   = heading;
    const uint8_t facing =
        v.move_microsteps[u.move_heading * MICROSTEPS_PER_HEADING + u.move_microstep].facing;
    u.facing_target  = facing;
    u.facing_current = facing;
}

void unit_state_dock_taxi_in(const sim_view &v, sim_store &own,
                             const unit_state_dock_taxi_in_calls &c) {
    unit          &u            = own.cur_unit();
    const uint16_t player       = *v.cur_player;
    const uint16_t unit_index   = *v.cur_index;
    const int32_t  storage_slot = u.home_storage_slot;

    // 0x0047f9a5-0x0047f9e3: step_cost = dir_step_factor(facing_target) * (step_speed[player] *
    // DAT_005013f8). SAME shape/evaluation order as takeoff_taxi's step_cost, a DIFFERENT unconfirmed
    // constant (see the header banner's DECLARED NEED).
    const double base      = v.cfg_units[u.unit_proto_id].step_speed[player] * *v.taxi_dock_step_speed_mult;
    const double step_cost = c.dir_step_factor(static_cast<int32_t>(u.facing_target)) * base;

    if (own.tick_budget() < step_cost) {
        u.activity_clock -= own.tick_budget();
        own.tick_budget() = 0.0;
        return;
    }
    // 0x0047fa1b-0x0047fa2a: budget spent UNCONDITIONALLY here -- unlike takeoff_taxi, the building
    // gate below runs AFTER the spend, not before (see the header banner's divergence note #1).
    own.tick_budget() -= step_cost;

    // 0x0047fc74-0x0047fe5e: the settle tail, reached either from the A_PORT-ready branch below or
    // from the path-exhausted (0xff) branch further down. Modelled as a local lambda so this TU has no
    // goto -- the net control-flow shape matches the asm's own JMP into the same tail exactly.
    const building &b0     = building_of(v, player, storage_of(v, player, storage_slot).b_index);
    auto            settle = [&]() {
        // 1: unconditional state commit.
        c.unit_set_state(UNIT_STATE_PARKED);

        // 2: release a live secondary target, if any.
        if (u.target2_ref != 0) {
            c.target_release_ref(player, unit_index, 3u); // mode 3, immediate in the asm
            u.target2_ref = 0;
        }

        // 3: unconditional door-mutex release.
        own.storage_at(player, storage_slot).door_mutex_unit = 0;

        // 4: free the path slot if one was assigned.
        if (u.path_slot_id != 0xff) {
            c.path_free_slot(player, unit_index);
        }

        // 5: re-fetch building/type fresh (the asm recomputes the same address arithmetic a second
        // time from scratch; this is a pure read of values nothing above writes, so re-deriving via
        // the same helpers once is behaviourally identical to the asm's redundant recompute).
        const building &b1        = building_of(v, player, storage_of(v, player, storage_slot).b_index);
        const uint8_t   bldg_type = v.cfg_buildings[b1.building_id].type;
        if (bldg_type == BUILDING_TYPE_A_PORT || bldg_type == BUILDING_TYPE_H_PORT) {
            const int32_t building_index = storage_of(v, player, storage_slot).b_index;

            // 5b-c: save the building's own pre-swap shuttle_slot, then swap the unit's value in.
            const uint8_t saved                                  = own.building_at(player, building_index).shuttle_slot;
            own.building_at(player, building_index).shuttle_slot = u.shuttle_slot;

            // 5d: original callee -- whatever it does with the now-swapped-in value is its own effect.
            c.bldg_flush_cargo_hold(player, building_index);

            // 5e: restore the building's own pre-swap value.
            own.building_at(player, building_index).shuttle_slot = saved;

            // 5f: PRESERVED, NOT "FIXED" -- the unit's shuttle_slot ends up holding the SAME `saved`
            // (building's pre-swap) value too, not the unit's own prior value (which was already
            // overwritten at step 5c and never saved anywhere). See the header banner's raw-asm +
            // exported-.c-draft cross-check for the full derivation.
            u.shuttle_slot = saved;

            c.prod_unbind_planet(player, *v.planet_index);
            c.storage_scrap_home_docked_units(player, unit_index);
        }
    };

    if (v.cfg_buildings[b0.building_id].type == BUILDING_TYPE_A_PORT) {
        // 0x0047fa6f-0x0047faac: A_PORT gate -- wait for the building's own "ready to receive" phase
        // (online_state==1) instead of animating a taxi step at all.
        if (b0.online_state != TAXI_DOCK_BLDG_ONLINE_STATE_DOCK_READY) {
            return; // budget already spent this tick; nothing else happens.
        }
        settle();
        return;
    }

    if (u.move_microstep < 0x1f) {
        // Same mid-step animation core as takeoff_taxi -- see that function's identical block.
        u.move_microstep += 1;
        const uint8_t facing =
            v.move_microsteps[u.move_heading * MICROSTEPS_PER_HEADING + u.move_microstep].facing;
        u.facing_target  = facing;
        u.facing_current = facing;
        return;
    }

    const uint8_t heading =
        v.path_buffers[player * PATH_WAYPOINTS_PER_PLAYER + u.path_slot_id * PATH_WAYPOINTS_PER_SLOT +
                       u.path_cursor]
            .heading;
    if (heading == 0xff) {
        settle();
        return;
    }

    // Same next-step consumption as takeoff_taxi -- see that function's identical block.
    u.path_cursor += 1;
    const int32_t new_x = (static_cast<int32_t>(u.x) + v.dir_step_offsets[heading].dx) &
                          static_cast<int32_t>(map_width_mask(v));
    const int32_t new_y = (static_cast<int32_t>(u.y) + v.dir_step_offsets[heading].dy) &
                          static_cast<int32_t>(map_height_mask(v));
    u.x              = static_cast<uint8_t>(new_x);
    u.y              = static_cast<uint8_t>(new_y);
    u.move_microstep = 0;
    u.move_heading   = heading;
    const uint8_t facing2 =
        v.move_microsteps[u.move_heading * MICROSTEPS_PER_HEADING + u.move_microstep].facing;
    u.facing_target  = facing2;
    u.facing_current = facing2;
}

void unit_state_landing_request(const sim_view &v, sim_store &own,
                                const unit_state_landing_request_calls &c) {
    unit          &u            = own.cur_unit();
    const uint16_t player       = *v.cur_player;
    const uint16_t unit_index   = *v.cur_index;
    const int32_t  storage_slot = u.home_storage_slot;

    // 0x004800cc-0x004800e4: no ready home building -- scatter and retry later. No tick_budget gate
    // anywhere in this function (confirmed absent from the asm).
    if (c.unit_get_ready_home_building() == 0) {
        c.unit_set_state_order(UNIT_STATE_IDLE_SCATTER, UNIT_STATE_IDLE_SCATTER);
        return;
    }

    // 0x004800e9-0x00480134: two independent "where would we land" probes, UNCONDITIONAL (both run
    // regardless of what follows), compared for equality further below. See the header banner's
    // out-pointer resolution for why `approach_col`/`approach_row` -- not `approach_fine_x`/
    // `approach_fine_y` -- is the right reading of storage_get_approach_tile's two outputs here.
    int32_t neighbor_col = 0, neighbor_row = 0;
    c.tile_neighbor_in_dir(u.x, u.y, u.move_heading, &neighbor_col, &neighbor_row);

    // uint32_t: storage_get_approach_tile's committed out-params are uint32_t * (TACT1-P C6,
    // 2026-09-04); compared against the int32_t neighbor_col/row pair below via implicit conversion.
    uint32_t approach_col = 0, approach_row = 0;
    c.storage_get_approach_tile(player, unit_index, &approach_col, &approach_row, 0u);

    // 0x0047fa2a.. equivalent for this function: 0x00480134-0x0048017b -- built_flags must be fully
    // operational (connected AND staffed) or scatter and retry.
    const building &b = building_of(v, player, storage_of(v, player, storage_slot).b_index);
    if (b.built_flags != BUILT_FLAGS_OPERATIONAL) {
        c.unit_set_state_order(UNIT_STATE_IDLE_SCATTER, UNIT_STATE_IDLE_SCATTER);
        return;
    }

    // 0x00480180-0x004801c4: three short-circuited gates, in this exact order (path_find_free_slot is
    // NOT called at all if storage_can_land already failed -- matching the asm's own TEST/JZ before
    // the CALL, not just its logical effect).
    bool    ok        = c.storage_can_land(player, unit_index, storage_slot) != 0;
    int32_t free_slot = -1;
    if (ok) {
        free_slot = c.path_find_free_slot(player);
        ok        = free_slot > -1; // CMP -1; JG (signed >, i.e. free_slot>=0)
    }
    if (ok) ok = (neighbor_col == approach_col);
    if (ok) ok = (neighbor_row == approach_row);

    if (!ok) {
        c.unit_set_state(TAXI_DOCK_STATE_PLOT_TURN_PATH);
        return;
    }

    // 0x004801c6-0x00480202: all gates passed -- claim the door mutex and accept the landing.
    own.storage_at(player, storage_slot).door_mutex_unit = unit_index;
    c.storage_accept_landing(player, unit_index, storage_slot, free_slot);
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------

void unit_state_takeoff_taxi() {
    sim_state st = state();
    detail::unit_state_takeoff_taxi(st.read, st.own, live_unit_state_takeoff_taxi_calls());
}

void unit_state_dock_taxi_in() {
    sim_state st = state();
    detail::unit_state_dock_taxi_in(st.read, st.own, live_unit_state_dock_taxi_in_calls());
}

void unit_state_landing_request() {
    sim_state st = state();
    detail::unit_state_landing_request(st.read, st.own, live_unit_state_landing_request_calls());
}


} // namespace mh::sim
