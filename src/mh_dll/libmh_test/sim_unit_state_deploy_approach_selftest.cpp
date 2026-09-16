//
// sim_unit_state_deploy_approach_selftest.cpp -- `simtest` oracle for llm_strat_unit_state_deploy_approach
// @0x0048117c (sim/sim_unit_state_deploy.h/.cpp, RI-SIM / SIM1-G3).
//
// arm_ready:false -- shadow_region_closure.py's closure reaches 780 functions / 400 undeclared regions
// (the standard escape this batch's remaining rows share), so a per-call shadow arm cannot evidence this
// site. This offline oracle is its evidence.
//
// EXPECTED BEHAVIOUR from the header banner's own derivation (sim_unit_state_deploy.h), in particular the
// CORRECTION section resolving which of the TWO (col,row) out-pointer pairs feeds which call/write:
//   neighbor = tile_neighbor_in_dir(u.x, u.y, u.move_heading)         -- pair A
//   corner   = calc_placement_corner_from_center(u.unit_proto_id, neighbor.col, neighbor.row) -- pair B,
//     computed AROUND THE NEIGHBOR TILE (pair A), not the unit's own position.
//   footprint_is_clear(corner.col, corner.row, equivalent, player) uses pair B (the corner), NOT pair A.
//   clear==0: unit_set_state_order(IDLE_SCATTER, IDLE_SCATTER). No further calls until the shared tail.
//   clear!=0: footprint_clear_passable(pair B), unit_unlink_tile, fow_remove_sight(OLD x/y) -- THEN
//     u.x/u.y are set from pair A (the neighbor tile), NOT pair B (the footprint corner) -- the
//     load-bearing correction this file exists to pin down. map_unit_put_on_map/map_fow_update_fow_plus
//     use the NEW (post-write) u.x/u.y.
//     player==player_side: ctrl_group_contains_unit(unit_index, ctrl_groups[0].count, 0)!=0 ->
//       unit_ctrlgroup_remove_member + game_set_event(INFO_REFRESH) (an EXTRA one, on top of the tail).
//     else if click_select_target_flags==(player|0x80) && click_select_target_id==unit_index ->
//       click_select_target_id=0 + game_set_event(INFO_REFRESH) (also extra).
//     move_heading = dir_remap_table[OLD move_heading].step_primary; move_microstep=0;
//     unit_set_state(DEPLOY_TO_BUILDING=0x17).
//   EVERY path: game_set_event(INFO_REFRESH=6) once at the shared tail.
//
#include "sim/sim_unit_state_deploy.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct neighbor_call {
    int32_t x, y, dir;
};
std::vector<neighbor_call> g_neighbor_calls;
int32_t                    g_neighbor_col_out = 11, g_neighbor_row_out = 22;
void                       rec_tile_neighbor_in_dir(int32_t x, int32_t y, int32_t dir, int32_t *out_col, int32_t *out_row) {
    g_neighbor_calls.push_back({x, y, dir});
    *out_col = g_neighbor_col_out;
    *out_row = g_neighbor_row_out;
}

struct corner_call {
    uint16_t proto_id;
    int32_t  center_x, center_y;
};
std::vector<corner_call> g_corner_calls;
int32_t                  g_corner_col_out = 33, g_corner_row_out = 44;
void                     rec_calc_placement_corner_from_center(uint16_t unit_index, int32_t center_x, int32_t center_y,
                                                               uint32_t *out_col, uint32_t *out_row) {
    g_corner_calls.push_back({unit_index, center_x, center_y});
    *out_col = (uint32_t)g_corner_col_out;
    *out_row = (uint32_t)g_corner_row_out;
}

struct footprint_clear_call {
    int32_t  x, y, building_type;
    uint32_t viewer;
};
std::vector<footprint_clear_call> g_footprint_is_clear_calls;
int32_t                           g_footprint_is_clear_result = 1;
int32_t                           rec_footprint_is_clear(int32_t x, int32_t y, int32_t building_type, uint32_t viewer) {
    g_footprint_is_clear_calls.push_back({x, y, building_type, viewer});
    return g_footprint_is_clear_result;
}

struct footprint_clear_passable_call {
    int32_t origin_x, origin_y, building_idx;
};
std::vector<footprint_clear_passable_call> g_footprint_clear_passable_calls;
void                                       rec_footprint_clear_passable(int32_t origin_x, int32_t origin_y, int32_t building_idx) {
    g_footprint_clear_passable_calls.push_back({origin_x, origin_y, building_idx});
}

struct unlink_call {
    uint32_t player;
    uint16_t unit_index;
};
std::vector<unlink_call> g_unlink_calls;
void                     rec_unit_unlink_tile(uint32_t player, uint16_t unit_index) {
    g_unlink_calls.push_back({player, unit_index});
}

struct fow_call {
    uint32_t player;
    int32_t  x, y;
    uint8_t  radius;
};
std::vector<fow_call> g_fow_calls;
void                  rec_fow_remove_sight(uint32_t player, int32_t x, int32_t y, uint8_t radius) {
    g_fow_calls.push_back({player, x, y, radius});
}

struct put_on_map_call {
    uint16_t player, unit_index;
    uint8_t  x, y;
};
std::vector<put_on_map_call> g_put_on_map_calls;
void                         rec_map_unit_put_on_map(uint16_t player, uint16_t unit_index, uint8_t x, uint8_t y) {
    g_put_on_map_calls.push_back({player, unit_index, x, y});
}

struct fow_plus_call {
    uint32_t player, x, y;
    uint8_t  sight;
};
std::vector<fow_plus_call> g_fow_plus_calls;
void                       rec_map_fow_update_fow_plus(uint32_t player, uint32_t x, uint32_t y, uint8_t sight) {
    g_fow_plus_calls.push_back({player, x, y, sight});
}

struct ctrl_contains_call {
    uint32_t unit_id;
    int32_t  count, group_index;
};
std::vector<ctrl_contains_call> g_ctrl_contains_calls;
int32_t                         g_ctrl_contains_result = 0;
int32_t                         rec_ctrl_group_contains_unit(uint32_t unit_id, int32_t count, int32_t group_index) {
    g_ctrl_contains_calls.push_back({unit_id, count, group_index});
    return g_ctrl_contains_result;
}

struct ctrl_remove_call {
    uint32_t unit_idx;
    int32_t  group_idx;
};
std::vector<ctrl_remove_call> g_ctrl_remove_calls;
void                          rec_unit_ctrlgroup_remove_member(uint32_t unit_idx, int32_t                          */*count_ptr*/, int32_t group_idx) {
    g_ctrl_remove_calls.push_back({unit_idx, group_idx});
}

std::vector<uint32_t> g_set_event_calls;
uint32_t              rec_game_set_event(uint32_t type) {
    g_set_event_calls.push_back(type);
    return 0;
}

std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) { g_set_state_calls.push_back(new_state); }

struct set_state_order_call {
    uint16_t new_order, new_state;
};
std::vector<set_state_order_call> g_set_state_order_calls;
void                              rec_unit_set_state_order(uint16_t new_order, uint16_t new_state) {
    g_set_state_order_calls.push_back({new_order, new_state});
}

constexpr uint16_t PLAYER     = 2;
constexpr int32_t  UNIT_INDEX = 6;
constexpr uint16_t PROTO_ID   = 8;
constexpr int32_t  EQUIVALENT = 15;

const unit_state_deploy_approach_calls g_calls = {
    &rec_tile_neighbor_in_dir,
    &rec_calc_placement_corner_from_center,
    &rec_footprint_is_clear,
    &rec_footprint_clear_passable,
    &rec_unit_unlink_tile,
    &rec_fow_remove_sight,
    &rec_map_unit_put_on_map,
    &rec_map_fow_update_fow_plus,
    &rec_ctrl_group_contains_unit,
    &rec_unit_ctrlgroup_remove_member,
    &rec_game_set_event,
    &rec_unit_set_state,
    &rec_unit_set_state_order,
};

void reset_recorders() {
    g_neighbor_calls.clear();
    g_neighbor_col_out = 11;
    g_neighbor_row_out = 22;
    g_corner_calls.clear();
    g_corner_col_out = 33;
    g_corner_row_out = 44;
    g_footprint_is_clear_calls.clear();
    g_footprint_is_clear_result = 1;
    g_footprint_clear_passable_calls.clear();
    g_unlink_calls.clear();
    g_fow_calls.clear();
    g_put_on_map_calls.clear();
    g_fow_plus_calls.clear();
    g_ctrl_contains_calls.clear();
    g_ctrl_contains_result = 0;
    g_ctrl_remove_calls.clear();
    g_set_event_calls.clear();
    g_set_state_calls.clear();
    g_set_state_order_calls.clear();
}

unit &make_unit(sim_fixture &fx) {
    unit &u         = fx.u(PLAYER, UNIT_INDEX);
    u.x             = 1;
    u.y             = 2;
    u.move_heading  = 3;
    u.unit_proto_id = PROTO_ID;

    cfg_unit &cu  = fx.cfg_units[PROTO_ID];
    cu.sight      = 5;
    cu.equivalent = EQUIVALENT;

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = PLAYER;
    fx.view_cur_index  = UNIT_INDEX;
    return u;
}

} // namespace

void run_unit_state_deploy_approach_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- footprint_is_clear returns 0: scatter, and NOTHING past that point runs (no
    // footprint_clear_passable/unlink/fow/put_on_map/fow_plus/ctrl-group/unit_set_state); the shared
    // tail set_event still fires exactly once. Also pins the CORRECTION: calc_placement_corner_from_center
    // is called with the NEIGHBOR pair (pair A), and footprint_is_clear with the CORNER pair (pair B).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                     = make_unit(fx);
        g_footprint_is_clear_result = 0;

        std::vector<dir_remap_row> local_remap(25);
        sim_view                   v = fx.view();
        v.dir_remap_table            = local_remap.data();
        sim_store own                = fx.store();
        detail::unit_state_deploy_approach(v, own, g_calls);

        ck(g_neighbor_calls.size() == 1 && g_neighbor_calls[0].x == u.x && g_neighbor_calls[0].y == u.y &&
               g_neighbor_calls[0].dir == u.move_heading,
           "T1: tile_neighbor_in_dir(u.x, u.y, u.move_heading) called once, 0x0048119b");
        ck(g_corner_calls.size() == 1 && g_corner_calls[0].proto_id == PROTO_ID &&
               g_corner_calls[0].center_x == g_neighbor_col_out && g_corner_calls[0].center_y == g_neighbor_row_out,
           "T1: calc_placement_corner_from_center(proto_id, NEIGHBOR.col, NEIGHBOR.row) -- uses pair A, "
           "0x004811cb-0x004811ce (the CORRECTION)");
        ck(g_footprint_is_clear_calls.size() == 1 &&
               g_footprint_is_clear_calls[0].x == g_corner_col_out &&
               g_footprint_is_clear_calls[0].y == g_corner_row_out &&
               g_footprint_is_clear_calls[0].building_type == EQUIVALENT &&
               g_footprint_is_clear_calls[0].viewer == PLAYER,
           "T1: footprint_is_clear(CORNER.col, CORNER.row, equivalent, player) -- uses pair B, "
           "0x004811df-0x004811fe (the CORRECTION)");
        ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].new_order == 0x13 &&
               g_set_state_order_calls[0].new_state == 0x13,
           "T1: unit_set_state_order(IDLE_SCATTER, IDLE_SCATTER) on the blocked path, 0x004813c1-0x004813cb");
        ck(g_footprint_clear_passable_calls.empty(), "T1: footprint_clear_passable NOT called on the blocked path");
        ck(g_unlink_calls.empty(), "T1: unit_unlink_tile NOT called on the blocked path");
        ck(g_put_on_map_calls.empty(), "T1: map_unit_put_on_map NOT called on the blocked path");
        ck(g_set_state_calls.empty(), "T1: unit_set_state NOT called on the blocked path");
        ck_eq((int32_t)u.x, 1, "T1: u.x untouched on the blocked path");
        ck(g_set_event_calls.size() == 1 && g_set_event_calls[0] == 6,
           "T1: shared tail set_event(INFO_REFRESH) fires exactly once, 0x004813dd");
    }

    // =================================================================================================
    // T2 -- clear!=0, player==player_side, ctrl_group_contains_unit!=0: the unit moves onto the
    // NEIGHBOR tile (pair A), NOT the footprint corner (pair B) -- the load-bearing part of the
    // CORRECTION. ctrl-group removal fires + an EXTRA set_event on top of the shared tail one (TWO
    // total). move_heading remapped via dir_remap_table, microstep reset, DEPLOY_TO_BUILDING entered.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                     = make_unit(fx);
        g_footprint_is_clear_result = 1;
        g_ctrl_contains_result      = 1;
        fx.player_side              = static_cast<int16_t>(PLAYER);

        std::vector<dir_remap_row> local_remap(25);
        local_remap[3].step_primary = 19; // u.move_heading starts at 3 -- must remap to 19

        sim_view v                 = fx.view();
        v.dir_remap_table          = local_remap.data();
        sim_store own              = fx.store();
        own.ctrl_group_at(0).count = 7;

        detail::unit_state_deploy_approach(v, own, g_calls);

        ck(g_footprint_clear_passable_calls.size() == 1 &&
               g_footprint_clear_passable_calls[0].origin_x == g_corner_col_out &&
               g_footprint_clear_passable_calls[0].origin_y == g_corner_row_out &&
               g_footprint_clear_passable_calls[0].building_idx == EQUIVALENT,
           "T2: footprint_clear_passable(CORNER.col, CORNER.row, equivalent), 0x0048120e-0x00481223");
        ck(g_fow_calls.size() == 1 && g_fow_calls[0].x == 1 && g_fow_calls[0].y == 2,
           "T2: fow_remove_sight fires at the OLD position (1,2) before the move, 0x0048125f");
        ck_eq((int32_t)u.x, g_neighbor_col_out, "T2: u.x set from the NEIGHBOR pair (A), not the corner (B)");
        ck_eq((int32_t)u.y, g_neighbor_row_out, "T2: u.y set from the NEIGHBOR pair (A), not the corner (B)");
        ck(g_put_on_map_calls.size() == 1 && g_put_on_map_calls[0].x == (uint8_t)g_neighbor_col_out &&
               g_put_on_map_calls[0].y == (uint8_t)g_neighbor_row_out,
           "T2: map_unit_put_on_map uses the NEW (post-move) x/y, 0x004812c1");
        ck(g_fow_plus_calls.size() == 1 && g_fow_plus_calls[0].x == (uint32_t)g_neighbor_col_out,
           "T2: map_fow_update_fow_plus fires at the NEW position, 0x004812e0");
        ck(g_ctrl_contains_calls.size() == 1 && g_ctrl_contains_calls[0].unit_id == UNIT_INDEX &&
               g_ctrl_contains_calls[0].count == 7,
           "T2: ctrl_group_contains_unit(unit_index, ctrl_groups[0].count, 0) called, 0x0048130d");
        ck(g_ctrl_remove_calls.size() == 1 && g_ctrl_remove_calls[0].unit_idx == UNIT_INDEX,
           "T2: unit_ctrlgroup_remove_member(unit_index, ...) called, 0x0048131e");
        ck_eq((int)g_set_event_calls.size(), 2,
              "T2: set_event fires TWICE -- once in the ctrl-group branch, once at the shared tail");
        ck_eq((int32_t)u.move_heading, 19, "T2: move_heading remapped via dir_remap_table[old].step_primary");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x17,
           "T2: unit_set_state(DEPLOY_TO_BUILDING=0x17), 0x004813b5");
    }

    // =================================================================================================
    // T3 -- clear!=0, player==player_side, ctrl_group_contains_unit==0: NO removal, set_event fires
    // only ONCE (the shared tail) -- isolates the "extra event" from being unconditional.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_unit(fx);
        g_footprint_is_clear_result = 1;
        g_ctrl_contains_result      = 0;
        fx.player_side              = static_cast<int16_t>(PLAYER);

        std::vector<dir_remap_row> local_remap(25);
        sim_view                   v = fx.view();
        v.dir_remap_table            = local_remap.data();
        sim_store own                = fx.store();
        detail::unit_state_deploy_approach(v, own, g_calls);

        ck(g_ctrl_remove_calls.empty(), "T3: ctrl_group_contains_unit==0 -- remove_member NOT called");
        ck_eq((int)g_set_event_calls.size(), 1, "T3: set_event fires only once (the shared tail)");
    }

    // =================================================================================================
    // T4 -- clear!=0, player!=player_side, click-select target matches this (player, unit_index):
    // click_select_target_id cleared, extra set_event (TWO total), the ctrl-group branch never runs.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_unit(fx);
        g_footprint_is_clear_result  = 1;
        fx.player_side               = static_cast<int16_t>(PLAYER + 1); // != PLAYER
        fx.click_select_target_flags = static_cast<uint16_t>(PLAYER | 0x80u);
        fx.click_select_target_id    = UNIT_INDEX;

        std::vector<dir_remap_row> local_remap(25);
        sim_view                   v = fx.view();
        v.dir_remap_table            = local_remap.data();
        sim_store own                = fx.store();
        detail::unit_state_deploy_approach(v, own, g_calls);

        ck(g_ctrl_contains_calls.empty(), "T4: player != player_side -- ctrl_group_contains_unit NOT called");
        ck_eq((int)fx.click_select_target_id, 0, "T4: click_select_target_id cleared, 0x0048134b-0x00481380");
        ck_eq((int)g_set_event_calls.size(), 2, "T4: set_event fires TWICE (branch + shared tail)");
    }

    // =================================================================================================
    // T5 -- clear!=0, player!=player_side, click-select target does NOT match: neither UI branch runs,
    // set_event fires only once.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_unit(fx);
        g_footprint_is_clear_result  = 1;
        fx.player_side               = static_cast<int16_t>(PLAYER + 1);
        fx.click_select_target_flags = static_cast<uint16_t>((PLAYER + 5) | 0x80u); // mismatched player
        fx.click_select_target_id    = UNIT_INDEX;

        std::vector<dir_remap_row> local_remap(25);
        sim_view                   v = fx.view();
        v.dir_remap_table            = local_remap.data();
        sim_store own                = fx.store();
        detail::unit_state_deploy_approach(v, own, g_calls);

        ck_eq((int)fx.click_select_target_id, UNIT_INDEX, "T5: click_select_target_id untouched on a mismatch");
        ck_eq((int)g_set_event_calls.size(), 1, "T5: set_event fires only once (the shared tail)");
    }
}

} // namespace mh::sim::test
