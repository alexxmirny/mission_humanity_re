//
// sim_unit_state_takeoff_taxi_selftest.cpp -- `simtest` oracle for llm_strat_unit_state_takeoff_taxi
// @0x0047f69f (sim/sim_unit_state_taxi_dock.h/.cpp, RI-SIM / SIM1-G3).
//
// arm_ready:false -- shadow_region_closure.py's closure reaches 780 functions / 400 undeclared regions
// (the standard escape this batch's remaining rows share), so a per-call shadow arm cannot evidence this
// site. This offline oracle is its evidence.
//
// EXPECTED BEHAVIOUR from the header banner's own derivation (sim_unit_state_taxi_dock.h):
//   0x0047f6b7-0x0047f6fa: building.online_state != TAXI_OUT_READY(2) -> tick_budget=0.0, return. NO
//     step_cost computed, dir_step_factor NOT called.
//   0x0047f715-0x0047f753: step_cost = dir_step_factor(facing_target) * (step_speed[player] *
//     taxi_takeoff_step_speed_mult).
//   tick_budget < step_cost -> activity_clock -= tick_budget (the PRE-drain value); tick_budget=0.0;
//     return.
//   tick_budget -= step_cost (unconditionally past that gate).
//   move_microstep < 0x1f -> microstep+=1; facing = move_microsteps[move_heading*32+microstep].facing;
//     BOTH facing_target and facing_current set to it; return (no path-buffer read at all).
//   move_microstep == 0x1f -> read path_buffers[...path_cursor].heading (UNADVANCED cursor).
//     heading==0xff -> unit_set_state(TAKEOFF=0x15); unit_takeoff_finalize(player, unit_index).
//     heading!=0xff -> path_cursor+=1; x/y stepped via dir_step_offsets[heading] & width/height_mask;
//       move_microstep=0; move_heading=heading; facing resynced from move_microsteps[NEW heading][0].
//
#include "sim/sim_unit_state_taxi_dock.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

double  g_dir_step_factor_arg    = 0.0;
int32_t g_dir_step_factor_calls  = 0;
double  g_dir_step_factor_result = 1.0;
double  rec_dir_step_factor(int32_t dir) {
    ++g_dir_step_factor_calls;
    g_dir_step_factor_arg = dir;
    return g_dir_step_factor_result;
}

std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) { g_set_state_calls.push_back(new_state); }

struct finalize_call {
    uint32_t player;
    uint32_t unit_index;
};
std::vector<finalize_call> g_finalize_calls;
void                       rec_unit_takeoff_finalize(uint32_t player, uint32_t unit_index) {
    g_finalize_calls.push_back({player, unit_index});
}

const unit_state_takeoff_taxi_calls g_calls = {
    &rec_dir_step_factor,
    &rec_unit_set_state,
    &rec_unit_takeoff_finalize,
};

constexpr uint16_t PLAYER       = 3;
constexpr int32_t  UNIT_INDEX   = 9;
constexpr int32_t  STORAGE_SLOT = 4;
constexpr int32_t  B_INDEX      = 6;

void reset_recorders() {
    g_dir_step_factor_calls  = 0;
    g_dir_step_factor_result = 1.0;
    g_set_state_calls.clear();
    g_finalize_calls.clear();
}

unit &make_ready_unit(sim_fixture &fx) {
    unit &u             = fx.u(PLAYER, UNIT_INDEX);
    u.home_storage_slot = static_cast<uint8_t>(STORAGE_SLOT);

    unit_storage &st = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
    st.b_index       = B_INDEX;

    building &b    = fx.b(PLAYER, B_INDEX);
    b.online_state = 2; // TAXI_DOCK_BLDG_ONLINE_STATE_TAXI_OUT_READY

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = PLAYER;
    fx.view_cur_index  = UNIT_INDEX;
    return u;
}

} // namespace

void run_unit_state_takeoff_taxi_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- building not taxi-out-ready (online_state != 2): tick_budget drained to 0, dir_step_factor
    // NEVER called (no step_cost computed at all), nothing else touched.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                            = make_ready_unit(fx);
        fx.b(PLAYER, B_INDEX).online_state = 1; // not ready
        fx.tick_budget                     = 9.0;
        u.activity_clock                   = 5.0;

        sim_store own = fx.store();
        detail::unit_state_takeoff_taxi(fx.view(), own, g_calls);

        ck_eq(g_dir_step_factor_calls, 0, "T1: not-ready building -- dir_step_factor never called, 0x0047f6b7");
        ck_eq_d(fx.tick_budget, 0.0, "T1: tick_budget drained to 0.0, 0x0047f6fc-0x0047f710");
        ck_eq_d(u.activity_clock, 5.0, "T1: activity_clock untouched on the not-ready path");
    }

    // =================================================================================================
    // T2 -- ready, but tick_budget < step_cost: activity_clock -= the PRE-drain tick_budget, then
    // tick_budget zeroed.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                                          = make_ready_unit(fx);
        u.facing_target                                  = 4;
        fx.cfg_units[u.unit_proto_id].step_speed[PLAYER] = 3.0;
        g_dir_step_factor_result                         = 1.0; // step_cost = 1.0 * (3.0*2.0) = 6.0
        fx.tick_budget                                   = 2.0; // < 6.0
        u.activity_clock                                 = 10.0;

        sim_store own = fx.store();
        detail::unit_state_takeoff_taxi(fx.view(), own, g_calls);

        ck_eq(g_dir_step_factor_calls, 1, "T2: dir_step_factor(facing_target) called once, 0x0047f715");
        ck_eq_d(g_dir_step_factor_arg, 4.0, "T2: dir_step_factor called with u.facing_target");
        ck_eq_d(u.activity_clock, 10.0 - 2.0, "T2: activity_clock -= the pre-drain tick_budget, 0x0047f761");
        ck_eq_d(fx.tick_budget, 0.0, "T2: tick_budget zeroed on insufficient budget, 0x0047f786");
    }

    // =================================================================================================
    // T3 -- ready, sufficient budget, mid sub-tile animation (move_microstep < 0x1f): budget spent,
    // microstep incremented, facing_target AND facing_current both set from the SAME table lookup, NO
    // path-buffer read (path_cursor/x/y untouched).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                                                   = make_ready_unit(fx);
        u.facing_target                                           = 0;
        fx.cfg_units[u.unit_proto_id].step_speed[PLAYER]          = 1.0;
        g_dir_step_factor_result                                  = 1.0; // step_cost = 1.0*(1.0*2.0) = 2.0
        fx.tick_budget                                            = 10.0;
        u.move_heading                                            = 5;
        u.move_microstep                                          = 3;
        u.path_cursor                                             = 77;  // must stay untouched
        fx.move_microsteps[5 * MICROSTEPS_PER_HEADING + 4].facing = 200; // POST-increment index (3+1=4)

        sim_store own = fx.store();
        detail::unit_state_takeoff_taxi(fx.view(), own, g_calls);

        ck_eq_d(fx.tick_budget, 8.0, "T3: tick_budget -= step_cost (10.0 - 2.0 = 8.0), 0x0047f78b-0x0047f794");
        ck_eq((uint32_t)u.move_microstep, 4u, "T3: move_microstep incremented (3 -> 4), 0x0047f79a");
        ck_eq((uint32_t)u.facing_target, 200u, "T3: facing_target set from move_microsteps[heading][4]");
        ck_eq((uint32_t)u.facing_current, 200u, "T3: facing_current set from the SAME lookup, two writes");
        ck_eq((uint32_t)u.path_cursor, 77u, "T3: path_cursor untouched on the mid-microstep path");
    }

    // =================================================================================================
    // T4 -- settled (move_microstep==0x1f), path exhausted (heading==0xff at the UNADVANCED cursor):
    // unit_set_state(TAKEOFF=0x15) then unit_takeoff_finalize(player, unit_index). No x/y move.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                                                                                       = make_ready_unit(fx);
        fx.cfg_units[u.unit_proto_id].step_speed[PLAYER]                                              = 1.0;
        g_dir_step_factor_result                                                                      = 1.0;
        fx.tick_budget                                                                                = 10.0;
        u.move_microstep                                                                              = 0x1f;
        u.path_slot_id                                                                                = 2;
        u.path_cursor                                                                                 = 5;
        u.x                                                                                           = 50;
        u.y                                                                                           = 60;
        fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + 2 * PATH_WAYPOINTS_PER_SLOT + 5].heading = 0xff;

        sim_store own = fx.store();
        detail::unit_state_takeoff_taxi(fx.view(), own, g_calls);

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x15,
           "T4: unit_set_state(TAKEOFF=0x15) on path-exhausted, 0x0047f84c-0x0047f873");
        ck(g_finalize_calls.size() == 1 && g_finalize_calls[0].player == PLAYER &&
               g_finalize_calls[0].unit_index == UNIT_INDEX,
           "T4: unit_takeoff_finalize(player, unit_index) called once, 0x0047f96e");
        ck_eq((uint32_t)u.x, 50u, "T4: x untouched -- path exhausted, no step taken");
        ck_eq((uint32_t)u.path_cursor, 5u, "T4: path_cursor NOT advanced on the exhausted path");
    }

    // =================================================================================================
    // T5 -- settled, a real next waypoint (heading != 0xff): path_cursor advanced, x/y stepped via
    // dir_step_offsets[heading] (full 32-bit add-then-mask), move_microstep reset to 0, move_heading
    // updated to the NEW heading, facing resynced from move_microsteps[NEW heading][0].
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                                                                                       = make_ready_unit(fx);
        fx.cfg_units[u.unit_proto_id].step_speed[PLAYER]                                              = 1.0;
        g_dir_step_factor_result                                                                      = 1.0;
        fx.tick_budget                                                                                = 10.0;
        u.move_microstep                                                                              = 0x1f;
        u.path_slot_id                                                                                = 1;
        u.path_cursor                                                                                 = 9;
        u.x                                                                                           = 10;
        u.y                                                                                           = 20;
        fx.path_buffers[PLAYER * PATH_WAYPOINTS_PER_PLAYER + 1 * PATH_WAYPOINTS_PER_SLOT + 9].heading = 7;
        fx.dir_step_offsets[7].dx                                                                     = 3;
        fx.dir_step_offsets[7].dy                                                                     = -2;
        fx.move_microsteps[7 * MICROSTEPS_PER_HEADING + 0].facing                                     = 44;

        sim_store own = fx.store();
        detail::unit_state_takeoff_taxi(fx.view(), own, g_calls);

        ck_eq((uint32_t)u.path_cursor, 10u, "T5: path_cursor advanced (9 -> 10), 0x0047f859");
        ck_eq((uint32_t)u.x, 13u, "T5: x = (10 + 3) & width_mask, 0x0047f8c0");
        ck_eq((uint32_t)u.y, 18u, "T5: y = (20 - 2) & height_mask, 0x0047f8e0");
        ck_eq((uint32_t)u.move_microstep, 0u, "T5: move_microstep reset to 0 on a fresh tile");
        ck_eq((uint32_t)u.move_heading, 7u, "T5: move_heading updated to the waypoint's heading");
        ck_eq((uint32_t)u.facing_target, 44u, "T5: facing resynced from move_microsteps[NEW heading][0]");
        ck_eq((uint32_t)u.facing_current, 44u, "T5: facing_current resynced too, same lookup");
    }
}

} // namespace mh::sim::test
