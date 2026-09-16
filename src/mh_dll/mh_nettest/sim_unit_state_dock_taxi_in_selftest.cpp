//
// sim_unit_state_dock_taxi_in_selftest.cpp -- `simtest` oracle for llm_strat_unit_state_dock_taxi_in
// @0x0047f981 (sim/sim_unit_state_taxi_dock.h/.cpp, RI-SIM / SIM1-G3).
//
// arm_ready:false -- shadow_region_closure.py's closure reaches 779 functions / 397 undeclared regions
// (the standard escape this batch's remaining rows share), so a per-call shadow arm cannot evidence this
// site. This offline oracle is its evidence.
//
// EXPECTED BEHAVIOUR from the header banner's own derivation (sim_unit_state_taxi_dock.h), in particular
// the three ways this diverges from takeoff_taxi (gate-vs-spend order, gate condition, and the settle
// tail) and the shuttle_slot PRESERVED-NOT-FIXED behaviour:
//   step_cost = dir_step_factor(facing_target) * (step_speed[player] * taxi_dock_step_speed_mult) --
//     computed and dir_step_factor CALLED regardless of what follows.
//   tick_budget < step_cost -> activity_clock -= tick_budget (pre-drain); tick_budget=0.0; return.
//   tick_budget -= step_cost UNCONDITIONALLY past that gate (spent BEFORE the building check, unlike
//     takeoff_taxi).
//   building type == A_PORT: online_state != DOCK_READY(1) -> return (budget already spent, nothing
//     else). online_state == DOCK_READY -> settle().
//   building type != A_PORT: no online_state check at all -- straight into the ordinary microstep/
//     path-waypoint core (move_microstep<0x1f: animate; ==0x1f: read path_buffers[cursor].heading,
//     0xff -> settle(), else -> real step, same idiom as takeoff_taxi).
//   settle(): unit_set_state(PARKED=0x1f); if target2_ref!=0: target_release_ref(player,unit_index,3)
//     + clear target2_ref; door_mutex_unit=0 UNCONDITIONALLY; if path_slot_id!=0xff:
//     path_free_slot(player,unit_index); re-fetch building/type FRESH -- if A_PORT or H_PORT: save
//     building.shuttle_slot, swap unit.shuttle_slot IN, bldg_flush_cargo_hold(player,building_index),
//     restore building.shuttle_slot to the saved value, then set unit.shuttle_slot to the SAME saved
//     (building's pre-swap) value too -- NOT the unit's own original value (PRESERVED BUG, not fixed).
//     prod_unbind_planet(player, *planet_index); storage_scrap_home_docked_units(player,unit_index).
//
#include "sim/sim_unit_state_taxi_dock.h"

#include <vector>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_PORT / BUILDING_TYPE_H_PORT
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

int32_t g_dir_step_factor_calls  = 0;
double  g_dir_step_factor_result = 1.0;
double  rec_dir_step_factor(int32_t /*dir*/) {
    ++g_dir_step_factor_calls;
    return g_dir_step_factor_result;
}

std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) { g_set_state_calls.push_back(new_state); }

struct release_call {
    uint32_t player;
    int32_t  unit_idx;
    uint32_t mode;
};
std::vector<release_call> g_release_calls;
void                      rec_target_release_ref(uint32_t player, int32_t unit_idx, uint32_t mode) {
    g_release_calls.push_back({player, unit_idx, mode});
}

struct path_free_call {
    uint16_t player;
    int32_t  unit_index;
};
std::vector<path_free_call> g_path_free_calls;
void                        rec_path_free_slot(uint16_t player, int32_t unit_index) {
    g_path_free_calls.push_back({player, unit_index});
}

struct flush_call {
    uint32_t player;
    int32_t  building_index;
};
std::vector<flush_call> g_flush_calls;
void                    rec_bldg_flush_cargo_hold(uint32_t player, int32_t building_index) {
    g_flush_calls.push_back({player, building_index});
}

struct unbind_call {
    int32_t player, planet_slot;
};
std::vector<unbind_call> g_unbind_calls;
void                     rec_prod_unbind_planet(int32_t player, int32_t planet_slot) {
    g_unbind_calls.push_back({player, planet_slot});
}

struct scrap_call {
    uint16_t player;
    int32_t  unit_index;
};
std::vector<scrap_call> g_scrap_calls;
void                    rec_storage_scrap_home_docked_units(uint16_t player, int32_t unit_index) {
    g_scrap_calls.push_back({player, unit_index});
}

const unit_state_dock_taxi_in_calls g_calls = {
    &rec_dir_step_factor,
    &rec_unit_set_state,
    &rec_target_release_ref,
    &rec_path_free_slot,
    &rec_bldg_flush_cargo_hold,
    &rec_prod_unbind_planet,
    &rec_storage_scrap_home_docked_units,
};

constexpr uint16_t PLAYER       = 4;
constexpr int32_t  UNIT_INDEX   = 7;
constexpr int32_t  STORAGE_SLOT = 2;
constexpr int32_t  B_INDEX      = 5;

void reset_recorders() {
    g_dir_step_factor_calls  = 0;
    g_dir_step_factor_result = 1.0;
    g_set_state_calls.clear();
    g_release_calls.clear();
    g_path_free_calls.clear();
    g_flush_calls.clear();
    g_unbind_calls.clear();
    g_scrap_calls.clear();
}

unit &make_unit(sim_fixture &fx, uint8_t building_type) {
    unit &u             = fx.u(PLAYER, UNIT_INDEX);
    u.home_storage_slot = static_cast<uint8_t>(STORAGE_SLOT);

    unit_storage &st = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
    st.b_index       = B_INDEX;

    building &b                    = fx.b(PLAYER, B_INDEX);
    b.building_id                  = B_INDEX;
    fx.cfg_buildings[B_INDEX].type = building_type;

    fx.cfg_units[u.unit_proto_id].step_speed[PLAYER] = 1.0;
    fx.tick_budget                                   = 100.0; // plenty of headroom by default

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = PLAYER;
    fx.view_cur_index  = UNIT_INDEX;
    return u;
}

} // namespace

void run_unit_state_dock_taxi_in_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- insufficient budget: dir_step_factor is called (step_cost is always computed), but the
    // building is never even fetched -- activity_clock -= the pre-drain budget, tick_budget zeroed.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u          = make_unit(fx, BUILDING_TYPE_A_PORT);
        fx.tick_budget   = 1.0; // step_cost = 1.0*(1.0*2.0) = 2.0 -- insufficient
        u.activity_clock = 9.0;

        sim_store own = fx.store();
        detail::unit_state_dock_taxi_in(fx.view(), own, g_calls);

        ck_eq(g_dir_step_factor_calls, 1, "T1: dir_step_factor called once (step_cost always computed)");
        ck_eq_d(u.activity_clock, 9.0 - 1.0, "T1: activity_clock -= the pre-drain tick_budget");
        ck_eq_d(fx.tick_budget, 0.0, "T1: tick_budget zeroed on insufficient budget");
        ck(g_set_state_calls.empty(), "T1: nothing past the budget gate runs");
    }

    // =================================================================================================
    // T2 -- sufficient budget, A_PORT, online_state != DOCK_READY(1): budget IS spent (unlike
    // takeoff_taxi's pre-spend gate), but the function returns with no settle and no further calls.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_unit(fx, BUILDING_TYPE_A_PORT);
        fx.b(PLAYER, B_INDEX).online_state = 2; // != DOCK_READY(1)
        fx.tick_budget                     = 10.0;

        sim_store own = fx.store();
        detail::unit_state_dock_taxi_in(fx.view(), own, g_calls);

        ck_eq_d(fx.tick_budget, 8.0, "T2: budget spent even though the building isn't ready (10.0 - 2.0)");
        ck(g_set_state_calls.empty(), "T2: settle() NOT reached -- unit_set_state not called");
        ck(g_flush_calls.empty(), "T2: no shuttle-slot swap -- settle never runs");
    }

    // =================================================================================================
    // T3 -- A_PORT ready: settle() runs. target2_ref==0 (no release call) and path_slot_id==0xff (no
    // path_free_slot call) isolate the unconditional effects: unit_set_state(PARKED), door_mutex_unit
    // cleared, and the shuttle-slot swap-then-restore-then-PRESERVE-BUG sequence for an A_PORT building.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                                                                = make_unit(fx, BUILDING_TYPE_A_PORT);
        fx.b(PLAYER, B_INDEX).online_state                                     = 1; // DOCK_READY
        fx.tick_budget                                                         = 10.0;
        u.target2_ref                                                          = 0;
        u.path_slot_id                                                         = 0xff;
        u.shuttle_slot                                                         = 9; // the unit's own value, must NOT survive
        fx.b(PLAYER, B_INDEX).shuttle_slot                                     = 3; // building's pre-swap value -- what SHOULD end up on both
        fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT].door_mutex_unit = 77;
        fx.planet_index                                                        = 6;

        sim_store own = fx.store();
        detail::unit_state_dock_taxi_in(fx.view(), own, g_calls);

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x1f,
           "T3: unit_set_state(PARKED=0x1f) called once, settle() step 1");
        ck(g_release_calls.empty(), "T3: target2_ref==0 -- target_release_ref NOT called");
        ck(g_path_free_calls.empty(), "T3: path_slot_id==0xff -- path_free_slot NOT called");
        ck_eq((int32_t)fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT].door_mutex_unit, 0,
              "T3: door_mutex_unit cleared unconditionally, settle() step 3");
        ck(g_flush_calls.size() == 1 && g_flush_calls[0].player == PLAYER &&
               g_flush_calls[0].building_index == B_INDEX,
           "T3: bldg_flush_cargo_hold(player, building_index) called once, A_PORT swap arm");
        ck_eq((int32_t)fx.b(PLAYER, B_INDEX).shuttle_slot, 3,
              "T3: building.shuttle_slot restored to its OWN pre-swap value (3), settle() step 5e");
        ck_eq((int32_t)u.shuttle_slot, 3,
              "T3: unit.shuttle_slot ends up with the BUILDING's saved value (3), NOT the unit's own "
              "original (9) -- the PRESERVED, not fixed, bug");
        ck(g_unbind_calls.size() == 1 && g_unbind_calls[0].player == PLAYER && g_unbind_calls[0].planet_slot == 6,
           "T3: prod_unbind_planet(player, *planet_index) called once");
        ck(g_scrap_calls.size() == 1 && g_scrap_calls[0].player == PLAYER &&
               g_scrap_calls[0].unit_index == UNIT_INDEX,
           "T3: storage_scrap_home_docked_units(player, unit_index) called once");
    }

    // =================================================================================================
    // T4 -- building type != A_PORT: no online_state check at all -- straight into the ordinary
    // mid-microstep animation core (same idiom as takeoff_taxi).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                                                   = make_unit(fx, /*building_type=*/0x01); // plain, not A_PORT/H_PORT
        fx.b(PLAYER, B_INDEX).online_state                        = 99;                                    // must be IGNORED entirely
        fx.tick_budget                                            = 10.0;
        u.move_heading                                            = 2;
        u.move_microstep                                          = 6;
        u.path_cursor                                             = 33; // must stay untouched
        fx.move_microsteps[2 * MICROSTEPS_PER_HEADING + 7].facing = 150;

        sim_store own = fx.store();
        detail::unit_state_dock_taxi_in(fx.view(), own, g_calls);

        ck(g_set_state_calls.empty(), "T4: non-A_PORT building never reaches settle() via the outer gate");
        ck_eq((uint32_t)u.move_microstep, 7u, "T4: move_microstep incremented (6 -> 7)");
        ck_eq((uint32_t)u.facing_target, 150u, "T4: facing_target set from move_microsteps[heading][7]");
        ck_eq((uint32_t)u.facing_current, 150u, "T4: facing_current set from the SAME lookup");
        ck_eq((uint32_t)u.path_cursor, 33u, "T4: path_cursor untouched on the mid-microstep path");
    }

    // =================================================================================================
    // T5 -- building type == H_PORT (the OTHER accepted swap type), reached via the path-exhausted
    // (heading==0xff) route rather than the A_PORT ready gate: settle() runs again, this time with
    // target2_ref!=0 (release called + cleared) and path_slot_id!=0xff (path_free_slot called) --
    // the positive complements to T3's negatives.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                                                                                       = make_unit(fx, BUILDING_TYPE_H_PORT);
        fx.tick_budget                                                                                = 10.0;
        u.move_microstep                                                                              = 0x1f;
        u.path_slot_id                                                                                = 4;
        u.path_cursor                                                                                 = 8;
        u.target2_ref                                                                                 = 55;
        fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + 4 * PATH_WAYPOINTS_PER_SLOT + 8].heading = 0xff;
        u.shuttle_slot                                                                                = 20;
        fx.b(PLAYER, B_INDEX).shuttle_slot                                                            = 12;

        sim_store own = fx.store();
        detail::unit_state_dock_taxi_in(fx.view(), own, g_calls);

        ck(g_release_calls.size() == 1 && g_release_calls[0].player == PLAYER &&
               g_release_calls[0].unit_idx == UNIT_INDEX && g_release_calls[0].mode == 3,
           "T5: target2_ref!=0 -- target_release_ref(player, unit_index, mode=3) called");
        ck_eq((int32_t)u.target2_ref, 0, "T5: target2_ref cleared after release");
        ck(g_path_free_calls.size() == 1 && g_path_free_calls[0].player == PLAYER &&
               g_path_free_calls[0].unit_index == UNIT_INDEX,
           "T5: path_slot_id!=0xff -- path_free_slot(player, unit_index) called");
        ck(g_flush_calls.size() == 1, "T5: H_PORT also triggers the shuttle-slot swap arm");
        ck_eq((int32_t)u.shuttle_slot, 12, "T5: H_PORT swap -- unit.shuttle_slot ends up with the building's saved value");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x1f,
           "T5: unit_set_state(PARKED) also fires via the path-exhausted settle route");
    }

    // =================================================================================================
    // T6 -- building type != A_PORT/H_PORT, settled with a real next waypoint: normal step-forward core,
    // no settle at all.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                                                                                       = make_unit(fx, /*building_type=*/0x02);
        fx.tick_budget                                                                                = 10.0;
        u.move_microstep                                                                              = 0x1f;
        u.path_slot_id                                                                                = 6;
        u.path_cursor                                                                                 = 1;
        u.x                                                                                           = 40;
        u.y                                                                                           = 50;
        fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + 6 * PATH_WAYPOINTS_PER_SLOT + 1].heading = 3;
        fx.dir_step_offsets[3].dx                                                                     = -5;
        fx.dir_step_offsets[3].dy                                                                     = 4;
        fx.move_microsteps[3 * MICROSTEPS_PER_HEADING + 0].facing                                     = 66;

        sim_store own = fx.store();
        detail::unit_state_dock_taxi_in(fx.view(), own, g_calls);

        ck_eq((uint32_t)u.path_cursor, 2u, "T6: path_cursor advanced (1 -> 2)");
        ck_eq((uint32_t)u.x, 35u, "T6: x = (40 - 5) & width_mask");
        ck_eq((uint32_t)u.y, 54u, "T6: y = (50 + 4) & height_mask");
        ck_eq((uint32_t)u.move_microstep, 0u, "T6: move_microstep reset to 0");
        ck_eq((uint32_t)u.move_heading, 3u, "T6: move_heading updated to the waypoint's heading");
        ck_eq((uint32_t)u.facing_target, 66u, "T6: facing resynced from move_microsteps[NEW heading][0]");
        ck(g_set_state_calls.empty(), "T6: no settle -- unit_set_state not called");
    }
}

} // namespace mh::sim::test
