//
// sim_path_step_check_and_request_detour_selftest.cpp -- `simtest` offline oracle for
// llm_strat_path_step_check_and_request_detour (sim/sim_path_step_check_and_request_detour.h/.cpp,
// RI-SIM / SIM1-G2).
//
// 0 DIRECT write cells -- it writes only through its callee llm_strat_unit_path_detour (the
// frontier original, called via mh::call::, exercised HERE only as a mocked callback) -- so this
// offline oracle is its only evidence.
//
#include "sim/sim_path_step_check_and_request_detour.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct log_t {
    int      facing24_calls    = 0;
    uint32_t last_facing24_arg = 0;
    int32_t  facing24_out_dx   = 0;
    int32_t  facing24_out_dy   = 0;

    int     detour_calls       = 0;
    int32_t last_detour_player = -1;
    int32_t last_detour_unit   = -1;
    int32_t last_detour_alt    = -1;
    int32_t detour_ret         = 1;

    void reset() { *this = log_t{}; }
};
log_t g_log;

const path_step_check_and_request_detour_calls &recording_calls() {
    static const path_step_check_and_request_detour_calls c = {
        [](uint32_t facing24, int32_t *out_dx, int32_t *out_dy) -> void {
            ++g_log.facing24_calls;
            g_log.last_facing24_arg = facing24;
            *out_dx                 = g_log.facing24_out_dx;
            *out_dy                 = g_log.facing24_out_dy;
        },
        [](int32_t player, int32_t unit_idx, int32_t alt_unit_idx) -> int32_t {
            ++g_log.detour_calls;
            g_log.last_detour_player = player;
            g_log.last_detour_unit   = unit_idx;
            g_log.last_detour_alt    = alt_unit_idx;
            return g_log.detour_ret;
        },
    };
    return c;
}

} // namespace

void run_path_step_check_and_request_detour_tests() {
    constexpr uint32_t SRC_X = 10, SRC_Y = 20;
    constexpr uint32_t DST_X = 11, DST_Y = 20;
    constexpr uint32_t SRC_PLAYER = 1, SRC_UNIT = 5;
    constexpr uint32_t DST_PLAYER = 2, DST_UNIT = 7;

    auto setup_src = [&](sim_fixture &fx) {
        tile_object &st = fx.tile_objects[(size_t)((SRC_X << 8) | SRC_Y)];
        st.building     = SRC_UNIT;
        st.class_owner  = (uint8_t)SRC_PLAYER; // low nibble = player; high nibble class doesn't gate the src read
    };

    // ---- (a) dst PASSABLE and building==0: "no detour needed" (0), no callee calls at all. --------
    {
        sim_fixture fx;
        fx.reset();
        g_log.reset();
        setup_src(fx);
        fx.passable[(size_t)((DST_X << 8) | DST_Y)]              = 1;
        fx.tile_objects[(size_t)((DST_X << 8) | DST_Y)].building = 0;
        sim_view v                                               = fx.view();

        const int32_t r =
            detail::path_step_check_and_request_detour(v, recording_calls(), SRC_X, SRC_Y, DST_X, DST_Y);
        ck_eq(r, 0, "path_step_check_and_request_detour: dst passable+empty -> 0 (clear-tile fast path)");
        ck_eq(g_log.facing24_calls, 0, "clear-tile path calls nothing");
        ck_eq(g_log.detour_calls, 0, "clear-tile path calls nothing");
    }

    // ---- (b) dst NOT unit-class (class_owner high nibble != 0x80), impassable or occupied by a
    // building -- blocked (1), no detour call (nothing to route around). ----------------------------
    {
        sim_fixture fx;
        fx.reset();
        g_log.reset();
        setup_src(fx);
        fx.passable[(size_t)((DST_X << 8) | DST_Y)] = 0; // not passable
        tile_object &dt                             = fx.tile_objects[(size_t)((DST_X << 8) | DST_Y)];
        dt.building                                 = 42;       // occupied
        dt.class_owner                              = 0x40 | 3; // BUILDING class (0x40), not unit (0x80)
        sim_view v                                  = fx.view();

        const int32_t r =
            detail::path_step_check_and_request_detour(v, recording_calls(), SRC_X, SRC_Y, DST_X, DST_Y);
        ck_eq(r, 1, "path_step_check_and_request_detour: dst occupied by a non-unit -> blocked, no detour call");
        ck_eq(g_log.detour_calls, 0, "non-unit obstruction never requests a detour");
    }

    // Shared dst-is-a-unit setup for the remaining cases.
    auto setup_dst_unit = [&](sim_fixture &fx, uint32_t occ_state, uint32_t move_op_arg,
                              uint32_t move_op_code) -> unit & {
        fx.passable[(size_t)((DST_X << 8) | DST_Y)] = 0;
        tile_object &dt                             = fx.tile_objects[(size_t)((DST_X << 8) | DST_Y)];
        dt.building                                 = DST_UNIT;
        dt.class_owner                              = (uint8_t)(0x80 | DST_PLAYER); // unit class, owner = DST_PLAYER

        unit &occ                    = fx.units[DST_PLAYER * UNITS_PER_PLAYER + DST_UNIT];
        occ.unit_proto_id            = 0;
        occ.state                    = (uint16_t)occ_state;
        fx.cfg_units[0].move_op_arg  = (uint8_t)move_op_arg;
        fx.cfg_units[0].move_op_code = (uint8_t)move_op_code;
        return occ;
    };

    // ---- (c) occupant.state == move_op_arg -> free (0), no facing/detour calls. --------------------
    {
        sim_fixture fx;
        fx.reset();
        g_log.reset();
        setup_src(fx);
        setup_dst_unit(fx, /*state*/ 0xa, /*move_op_arg*/ 0xa, /*move_op_code*/ 0xf);
        sim_view v = fx.view();

        const int32_t r =
            detail::path_step_check_and_request_detour(v, recording_calls(), SRC_X, SRC_Y, DST_X, DST_Y);
        ck_eq(r, 0, "path_step_check_and_request_detour: occupant.state == move_op_arg -> free");
        ck_eq(g_log.facing24_calls, 0, "move_op_arg short-circuit never predicts a next tile");
        ck_eq(g_log.detour_calls, 0, "move_op_arg short-circuit never requests a detour");
    }

    // ---- (d) occupant.state == literal 0xc -> free (0), same short-circuit as (c). -----------------
    {
        sim_fixture fx;
        fx.reset();
        g_log.reset();
        setup_src(fx);
        setup_dst_unit(fx, /*state*/ 0xc, /*move_op_arg*/ 0xa, /*move_op_code*/ 0xf);
        sim_view v = fx.view();

        const int32_t r =
            detail::path_step_check_and_request_detour(v, recording_calls(), SRC_X, SRC_Y, DST_X, DST_Y);
        ck_eq(r, 0, "path_step_check_and_request_detour: occupant.state == literal 0xc -> free");
        ck_eq(g_log.detour_calls, 0, "literal-0xc short-circuit never requests a detour");
    }

    // ---- (e) occupant.state == move_op_code, predicted next tile != our src -> free (0), the ---------
    // predicted-tile check runs (facing24_to_delta called once) but no detour is requested.
    {
        sim_fixture fx;
        fx.reset();
        g_log.reset();
        setup_src(fx);
        unit &occ        = setup_dst_unit(fx, /*state*/ 0xf, /*move_op_arg*/ 0xa, /*move_op_code*/ 0xf);
        occ.path_slot_id = 3;
        occ.path_cursor  = 1;
        const uint32_t path_index =
            DST_PLAYER * PATH_WAYPOINTS_PER_PLAYER + 3u * PATH_WAYPOINTS_PER_SLOT + 1u;
        fx.path_buffers[path_index].heading = 9; // arbitrary, resolved by the mocked callback below
        g_log.facing24_out_dx               = 5; // predicted next tile = (DST_X+5, DST_Y+0) != SRC
        g_log.facing24_out_dy               = 0;
        sim_view v                          = fx.view();

        const int32_t r =
            detail::path_step_check_and_request_detour(v, recording_calls(), SRC_X, SRC_Y, DST_X, DST_Y);
        ck_eq(r, 0, "path_step_check_and_request_detour: occupant vacating elsewhere -> free");
        ck_eq(g_log.facing24_calls, 1, "move_op_code arm predicts the occupant's next tile");
        ck_eq(g_log.last_facing24_arg, 9u, "facing24_to_delta called with the occupant's path heading");
        ck_eq(g_log.detour_calls, 0, "occupant vacating our tile on its own -> no detour requested");
    }

    // ---- (f) occupant.state == move_op_code, predicted next tile == our src -> a detour IS
    // requested: (src_player, src_unit_idx, dst_unit_idx), return 1 regardless of the callee's own
    // return value (this function only ever returns 1 once it decides to call the callee). ----------
    {
        sim_fixture fx;
        fx.reset();
        g_log.reset();
        setup_src(fx);
        unit &occ        = setup_dst_unit(fx, /*state*/ 0xf, /*move_op_arg*/ 0xa, /*move_op_code*/ 0xf);
        occ.path_slot_id = 3;
        occ.path_cursor  = 1;
        const uint32_t path_index =
            DST_PLAYER * PATH_WAYPOINTS_PER_PLAYER + 3u * PATH_WAYPOINTS_PER_SLOT + 1u;
        fx.path_buffers[path_index].heading = 9;
        g_log.facing24_out_dx               = (int32_t)SRC_X - (int32_t)DST_X; // predicted next tile == SRC exactly
        g_log.facing24_out_dy               = (int32_t)SRC_Y - (int32_t)DST_Y;
        g_log.detour_ret                    = 0; // the callee's own return is irrelevant to this function's result
        sim_view v                          = fx.view();

        const int32_t r =
            detail::path_step_check_and_request_detour(v, recording_calls(), SRC_X, SRC_Y, DST_X, DST_Y);
        ck_eq(r, 1, "path_step_check_and_request_detour: occupant would step onto our tile -> detour "
                    "requested, returns 1 regardless of the callee's own return");
        ck_eq(g_log.detour_calls, 1, "exactly one detour request");
        ck_eq(g_log.last_detour_player, (int32_t)SRC_PLAYER, "detour requested for the SRC unit's player");
        ck_eq(g_log.last_detour_unit, (int32_t)SRC_UNIT, "detour requested for the SRC unit's index");
        ck_eq(g_log.last_detour_alt, (int32_t)DST_UNIT, "the dst occupant is offered as the alt candidate");
    }

    // ---- (g) occupant.state matches NONE of {move_op_arg, 0xc, move_op_code} -- skips the
    // move-prediction branch entirely and goes straight to requesting a detour. ---------------------
    {
        sim_fixture fx;
        fx.reset();
        g_log.reset();
        setup_src(fx);
        setup_dst_unit(fx, /*state*/ 0x99, /*move_op_arg*/ 0xa, /*move_op_code*/ 0xf);
        sim_view v = fx.view();

        const int32_t r =
            detail::path_step_check_and_request_detour(v, recording_calls(), SRC_X, SRC_Y, DST_X, DST_Y);
        ck_eq(r, 1, "path_step_check_and_request_detour: occupant.state matches nothing -> detour requested");
        ck_eq(g_log.facing24_calls, 0, "no move-state match -> the prediction branch never runs");
        ck_eq(g_log.detour_calls, 1, "falls straight through to the detour request");
    }
}

} // namespace mh::sim::test
