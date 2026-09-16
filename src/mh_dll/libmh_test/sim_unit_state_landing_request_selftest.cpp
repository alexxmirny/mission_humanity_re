//
// sim_unit_state_landing_request_selftest.cpp -- `simtest` oracle for llm_strat_unit_state_landing_request
// @0x004800a8 (sim/sim_unit_state_taxi_dock.h/.cpp, RI-SIM / SIM1-G3).
//
// arm_ready:false -- shadow_region_closure.py's closure reaches 785 functions / 402 undeclared regions
// (the standard gfx/input/snd DLL-lifecycle + AI build-candidate escape this batch's remaining functions
// share), so a per-call shadow arm cannot evidence this site. This offline oracle is its evidence.
//
// EXPECTED BEHAVIOUR from tmp/decomp_sim/llm_strat_unit_state_landing_request_004800a8.asm, cross-checked
// against the existing translation's own derivation comments in sim_unit_state_taxi_dock.h/.cpp (the
// out-pointer wiring resolution in particular):
//   0x004800cc-0x004800e4: unit_get_ready_home_building() == 0 -> unit_set_state_order(IDLE_SCATTER,
//     IDLE_SCATTER), return. No tick_budget gate anywhere in this function.
//   0x004800e9-0x00480134: tile_neighbor_in_dir(u.x, u.y, u.move_heading, &neighbor_col, &neighbor_row)
//     AND storage_get_approach_tile(player, unit_index, &approach_col, &approach_row, 0) -- BOTH called
//     UNCONDITIONALLY, regardless of what the built_flags gate below decides.
//   0x00480134-0x0048017b: built_flags != BUILT_FLAGS_OPERATIONAL(3) -> unit_set_state_order(IDLE_SCATTER,
//     IDLE_SCATTER), return -- but the two probes above have ALREADY run by this point.
//   0x00480180-0x004801c4: three short-circuited gates, in order -- storage_can_land(player, unit_index,
//     storage_slot); only if non-zero is path_find_free_slot(player) called (free_slot > -1, i.e. signed
//     >= 0, required); only if that passes are neighbor_col==approach_col and then neighbor_row==
//     approach_row checked. ANY failure -> unit_set_state(PLOT_TURN_PATH=0x2d), no door/accept call.
//   0x004801c6-0x00480202: all four gates passed -- unit_storage[player][storage_slot].door_mutex_unit =
//     unit_index, then storage_accept_landing(player, unit_index, storage_slot, free_slot).
//
#include "sim/sim_unit_state_taxi_dock.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

int32_t g_ready_home_building_result = 0;
int32_t g_ready_home_building_calls  = 0;
int32_t rec_unit_get_ready_home_building() {
    ++g_ready_home_building_calls;
    return g_ready_home_building_result;
}

struct set_state_order_call {
    uint16_t new_order;
    uint16_t new_state;
};
std::vector<set_state_order_call> g_set_state_order_calls;
void                              rec_unit_set_state_order(uint16_t new_order, uint16_t new_state) {
    g_set_state_order_calls.push_back({new_order, new_state});
}

std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) { g_set_state_calls.push_back(new_state); }

struct neighbor_call {
    int32_t x, y, dir;
};
std::vector<neighbor_call> g_neighbor_calls;
int32_t                    g_neighbor_col_out = 0, g_neighbor_row_out = 0;
void                       rec_tile_neighbor_in_dir(int32_t x, int32_t y, int32_t dir, int32_t *out_col, int32_t *out_row) {
    g_neighbor_calls.push_back({x, y, dir});
    *out_col = g_neighbor_col_out;
    *out_row = g_neighbor_row_out;
}

struct approach_call {
    uint16_t p1, p2;
    uint32_t p5;
};
std::vector<approach_call> g_approach_calls;
int32_t                    g_approach_col_out = 0, g_approach_row_out = 0;
void                       rec_storage_get_approach_tile(uint16_t param_1, uint16_t param_2, uint32_t *a2, uint32_t *param_4,
                                                         uint32_t param_5) {
    g_approach_calls.push_back({param_1, param_2, param_5});
    *a2      = (uint32_t)g_approach_col_out;
    *param_4 = (uint32_t)g_approach_row_out;
}

struct can_land_call {
    int32_t  player;
    uint32_t unit_index;
    int32_t  storage_slot;
};
std::vector<can_land_call> g_can_land_calls;
int32_t                    g_can_land_result = 0;
int32_t                    rec_storage_can_land(int32_t player, uint32_t unit_index, int32_t storage_slot) {
    g_can_land_calls.push_back({player, unit_index, storage_slot});
    return g_can_land_result;
}

int32_t g_free_slot_calls  = 0;
int32_t g_free_slot_result = 0;
int32_t rec_path_find_free_slot(int32_t /*player*/) {
    ++g_free_slot_calls;
    return g_free_slot_result;
}

struct accept_landing_call {
    int32_t player, unit_index, storage_slot, free_slot;
};
std::vector<accept_landing_call> g_accept_landing_calls;
void                             rec_storage_accept_landing(int32_t player, int32_t unit_index, int32_t storage_slot, int32_t free_slot) {
    g_accept_landing_calls.push_back({player, unit_index, storage_slot, free_slot});
}

constexpr uint16_t PLAYER       = 2;
constexpr int32_t  UNIT_INDEX   = 4;
constexpr int32_t  STORAGE_SLOT = 6;
constexpr int32_t  B_INDEX      = 8;

const unit_state_landing_request_calls g_calls = {
    &rec_unit_get_ready_home_building,
    &rec_unit_set_state_order,
    &rec_tile_neighbor_in_dir,
    &rec_storage_get_approach_tile,
    &rec_storage_can_land,
    &rec_path_find_free_slot,
    &rec_unit_set_state,
    &rec_storage_accept_landing,
};

void reset_recorders() {
    g_ready_home_building_result = 1; // ready by default -- tests override when they need the scatter arm
    g_ready_home_building_calls  = 0;
    g_set_state_order_calls.clear();
    g_set_state_calls.clear();
    g_neighbor_calls.clear();
    g_neighbor_col_out = 0;
    g_neighbor_row_out = 0;
    g_approach_calls.clear();
    g_approach_col_out = 0;
    g_approach_row_out = 0;
    g_can_land_calls.clear();
    g_can_land_result  = 1;
    g_free_slot_calls  = 0;
    g_free_slot_result = 0;
    g_accept_landing_calls.clear();
}

// Sets up a unit/building/storage that would reach the "all four gates passed" branch if left alone;
// each test overrides exactly the input that test targets.
unit &make_ready_unit(sim_fixture &fx) {
    unit &u             = fx.u(PLAYER, UNIT_INDEX);
    u.home_storage_slot = static_cast<uint8_t>(STORAGE_SLOT);
    u.x                 = 10;
    u.y                 = 20;
    u.move_heading      = 3;

    unit_storage &st = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
    st.b_index       = B_INDEX;

    building &b   = fx.b(PLAYER, B_INDEX);
    b.built_flags = 3; // BUILT_FLAGS_OPERATIONAL

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = PLAYER;
    fx.view_cur_index  = UNIT_INDEX;
    return u;
}

} // namespace

void run_unit_state_landing_request_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- no ready home building: unit_set_state_order(IDLE_SCATTER, IDLE_SCATTER) called once,
    // NOTHING else called at all (the two unconditional probes below this gate never run).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_ready_unit(fx);
        g_ready_home_building_result = 0;

        sim_store own = fx.store();
        detail::unit_state_landing_request(fx.view(), own, g_calls);

        ck_eq(g_ready_home_building_calls, 1, "T1: unit_get_ready_home_building() called once, 0x004800cc");
        ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].new_order == 0x13 &&
               g_set_state_order_calls[0].new_state == 0x13,
           "T1: unit_set_state_order(IDLE_SCATTER=0x13, IDLE_SCATTER) called once, 0x004800d1-0x004800e4");
        ck(g_neighbor_calls.empty(), "T1: tile_neighbor_in_dir NOT called -- gated behind the ready check");
        ck(g_approach_calls.empty(), "T1: storage_get_approach_tile NOT called -- gated behind the ready check");
        ck(g_can_land_calls.empty(), "T1: storage_can_land NOT called");
        ck(g_accept_landing_calls.empty(), "T1: storage_accept_landing NOT called");
    }

    // =================================================================================================
    // T2 -- ready home building exists, but built_flags != OPERATIONAL: unit_set_state_order(SCATTER)
    // called -- but the two probes ALREADY ran (UNCONDITIONAL, before the built_flags check).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                           = make_ready_unit(fx);
        fx.b(PLAYER, B_INDEX).built_flags = 1; // connected but not staffed -- fails the ==3 test

        sim_store own = fx.store();
        detail::unit_state_landing_request(fx.view(), own, g_calls);

        ck(g_neighbor_calls.size() == 1 && g_neighbor_calls[0].x == u.x && g_neighbor_calls[0].y == u.y &&
               g_neighbor_calls[0].dir == u.move_heading,
           "T2: tile_neighbor_in_dir(u.x, u.y, u.move_heading, ...) called once even on the scatter path, "
           "0x004800e9-0x00480105");
        ck(g_approach_calls.size() == 1 && g_approach_calls[0].p1 == PLAYER &&
               g_approach_calls[0].p2 == UNIT_INDEX && g_approach_calls[0].p5 == 0,
           "T2: storage_get_approach_tile(player, unit_index, ..., 0) called once even on the scatter "
           "path, 0x0048010a-0x00480134");
        ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].new_order == 0x13,
           "T2: unit_set_state_order(IDLE_SCATTER, IDLE_SCATTER) on the built_flags!=OPERATIONAL gate, "
           "0x00480134-0x0048017b");
        ck(g_can_land_calls.empty(), "T2: storage_can_land NOT called -- the scatter return precedes it");
        ck(g_accept_landing_calls.empty(), "T2: storage_accept_landing NOT called");
    }

    // =================================================================================================
    // T3 -- built_flags OK, storage_can_land returns 0: path_find_free_slot is NOT called at all (the
    // asm's TEST/JZ short-circuit, not just the logical AND) -> unit_set_state(PLOT_TURN_PATH=0x2d).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_ready_unit(fx);
        g_can_land_result = 0;

        sim_store own = fx.store();
        detail::unit_state_landing_request(fx.view(), own, g_calls);

        ck(g_can_land_calls.size() == 1 && g_can_land_calls[0].player == PLAYER &&
               g_can_land_calls[0].unit_index == static_cast<uint32_t>(UNIT_INDEX) &&
               g_can_land_calls[0].storage_slot == STORAGE_SLOT,
           "T3: storage_can_land(player, unit_index, storage_slot) called once, 0x00480180-0x0048018f");
        ck_eq(g_free_slot_calls, 0,
              "T3: path_find_free_slot NOT called -- storage_can_land already failed, 0x00480191 JZ");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x2d,
           "T3: unit_set_state(PLOT_TURN_PATH=0x2d) on storage_can_land==0, 0x004801c6");
        ck(g_accept_landing_calls.empty(), "T3: storage_accept_landing NOT called");
    }

    // =================================================================================================
    // T4a -- storage_can_land!=0, path_find_free_slot returns -1 (the JG-signed boundary: -1 fails)
    // -> unit_set_state(PLOT_TURN_PATH). neighbor/approach are never compared.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_ready_unit(fx);
        g_can_land_result  = 1;
        g_free_slot_result = -1;
        // Deliberately set neighbor==approach so a WRONG (short-circuit-skipping) implementation that
        // reached the comparison anyway would still accept -- proving the failure came from THIS gate.
        g_neighbor_col_out = g_approach_col_out = 5;
        g_neighbor_row_out = g_approach_row_out = 7;

        sim_store own = fx.store();
        detail::unit_state_landing_request(fx.view(), own, g_calls);

        ck_eq(g_free_slot_calls, 1, "T4a: path_find_free_slot(player) called once, 0x00480195-0x0048019c");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x2d,
           "T4a: free_slot==-1 fails the >-1 (JG, signed >=0) test, 0x004801a1-0x004801a6");
        ck(g_accept_landing_calls.empty(), "T4a: storage_accept_landing NOT called");
    }

    // =================================================================================================
    // T4b -- free_slot==0 (the OTHER side of the same boundary) DOES pass this gate; the function
    // proceeds to compare coordinates. Paired with matching neighbor/approach so the whole chain
    // succeeds, exercising the boundary AND the accept path together.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_ready_unit(fx);
        g_can_land_result  = 1;
        g_free_slot_result = 0;
        g_neighbor_col_out = g_approach_col_out = 11;
        g_neighbor_row_out = g_approach_row_out = 22;

        sim_store own = fx.store();
        detail::unit_state_landing_request(fx.view(), own, g_calls);

        ck(g_accept_landing_calls.size() == 1 && g_accept_landing_calls[0].player == PLAYER &&
               g_accept_landing_calls[0].unit_index == UNIT_INDEX &&
               g_accept_landing_calls[0].storage_slot == STORAGE_SLOT &&
               g_accept_landing_calls[0].free_slot == 0,
           "T4b: free_slot==0 passes the boundary; storage_accept_landing(player, unit_index, "
           "storage_slot, free_slot=0) called, 0x004801c6-0x00480202");
        ck_eq((int32_t)fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT].door_mutex_unit, UNIT_INDEX,
              "T4b: door_mutex_unit set to unit_index on the accept path, 0x004801be-0x004801c1");
        ck(g_set_state_calls.empty(), "T4b: unit_set_state(PLOT_TURN_PATH) NOT called on the accept path");
    }

    // =================================================================================================
    // T5 -- all prior gates pass, but neighbor_col != approach_col: unit_set_state(PLOT_TURN_PATH), no
    // door_mutex_unit write, no accept call. Also proves the ROW compare is never reached when the COL
    // compare already failed (the asm's own axis order: col first, then row).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_ready_unit(fx);
        g_can_land_result  = 1;
        g_free_slot_result = 3;
        g_neighbor_col_out = 1;
        g_approach_col_out = 2;                      // mismatch on X
        g_neighbor_row_out = g_approach_row_out = 9; // Y matches -- isolates the X gate

        fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT].door_mutex_unit = 0;

        sim_store own = fx.store();
        detail::unit_state_landing_request(fx.view(), own, g_calls);

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x2d,
           "T5: neighbor_col != approach_col fails, 0x004801a8-0x004801af");
        ck(g_accept_landing_calls.empty(), "T5: storage_accept_landing NOT called");
        ck_eq((int32_t)fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT].door_mutex_unit, 0,
              "T5: door_mutex_unit NOT written on the reject path");
    }

    // =================================================================================================
    // T6 -- col matches, row mismatches: same PLOT_TURN_PATH rejection, isolating the Y gate.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_ready_unit(fx);
        g_can_land_result  = 1;
        g_free_slot_result = 3;
        g_neighbor_col_out = g_approach_col_out = 4; // X matches
        g_neighbor_row_out                      = 1;
        g_approach_row_out                      = 2; // mismatch on Y

        sim_store own = fx.store();
        detail::unit_state_landing_request(fx.view(), own, g_calls);

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x2d,
           "T6: neighbor_row != approach_row fails, 0x004801b2-0x004801c4");
        ck(g_accept_landing_calls.empty(), "T6: storage_accept_landing NOT called");
    }
}

} // namespace mh::sim::test
