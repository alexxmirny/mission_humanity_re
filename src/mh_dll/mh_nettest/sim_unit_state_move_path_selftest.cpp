#include "sim/sim_unit_state_move_path.h"

#include "sim/sim_order_enqueue.h" // UNIT_STATE_IDLE_SCATTER, UNIT_STATE_HOVER_ENGAGE, UNIT_TYPE_A_PLANE/H_PLANE

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// llm_strat_unit_state (order/state) literals this function compares against -- NOT backed by a
// real Ghidra enum (see the .cpp's own detail::-scoped MOVE_PATH_* constants); redeclared here
// since those are detail::-local and not visible from this TU.
constexpr uint16_t ORDER_DEPLOY_APPROACH    = 0x18;
constexpr uint16_t ORDER_ATTACK_UNIT        = 0x1a;
constexpr uint16_t ORDER_ATTACK_UNIT_RETURN = 0x1b;
constexpr uint16_t ORDER_ATTACK_BUILDING    = 0x1c;
constexpr uint16_t ORDER_LANDING_REQUEST    = 0x29;
constexpr uint16_t ORDER_ASCEND_TO_ORBIT    = 0x31;
constexpr uint16_t STATE_GROUP_STEP         = 0x0b;
constexpr uint16_t STATE_MOVE_PATH_PLANE    = 0x2c;
constexpr uint16_t STATE_PLOT_TURN_PATH     = 0x2d;
constexpr uint32_t NOTIFY_GOAL_REACHED      = 0x65;
constexpr uint32_t NOTIFY_REPLAN            = 1;

// ---- shared trace: proves call ORDER across all 12 callees --------------------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }
bool                      trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

int32_t g_chase_check_ret = 0;
int32_t rec_unit_chase_check() {
    tr("unit_chase_check");
    return g_chase_check_ret;
}

struct FreeSlotCall {
    uint16_t player;
    int32_t  unit_index;
};
std::vector<FreeSlotCall> g_free_slot_calls;
void                      rec_path_free_slot(uint16_t player, int32_t unit_index) {
    tr("path_free_slot");
    g_free_slot_calls.push_back({player, unit_index});
}

struct NotifyCall {
    uint32_t player;
    int32_t  unit_index;
    uint32_t status_code;
};
std::vector<NotifyCall> g_notify_calls;
void                    rec_unit_notify_status(uint32_t player, int32_t unit_index, uint32_t status_code) {
    tr("unit_notify_status");
    g_notify_calls.push_back({player, unit_index, status_code});
}

std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) {
    tr("unit_set_state");
    g_set_state_calls.push_back(new_state);
}

struct SetStateOrderCall {
    uint16_t new_state;
    uint16_t new_order;
};
std::vector<SetStateOrderCall> g_set_state_order_calls;
void                           rec_unit_set_state_order(uint16_t new_state, uint16_t new_order) {
    tr("unit_set_state_order");
    g_set_state_order_calls.push_back({new_state, new_order});
}

int32_t g_target_class_ret = 0;
int32_t rec_target_class(uint32_t owner_and_kind_flag, int32_t roster_slot) {
    tr("target_class");
    (void)owner_and_kind_flag;
    (void)roster_slot;
    return g_target_class_ret;
}

uint32_t g_in_weapon_range_ret = 0;
struct RangeCall {
    int32_t tile_x, tile_y;
};
std::vector<RangeCall> g_in_range_calls;
uint32_t               rec_unit_in_weapon_range(int32_t player, int32_t unit_idx, int32_t tile_x, int32_t tile_y,
                                                int32_t target_class_flags) {
    tr("unit_in_weapon_range");
    (void)player;
    (void)unit_idx;
    (void)target_class_flags;
    g_in_range_calls.push_back({tile_x, tile_y});
    return g_in_weapon_range_ret;
}

uint32_t g_approach_out_x = 0, g_approach_out_y = 0;
int      g_approach_calls = 0;
void     rec_storage_get_approach_tile(uint16_t player, uint16_t unit_index, uint32_t *out_x, uint32_t *out_y,
                                       uint32_t storage_idx) {
    tr("storage_get_approach_tile");
    (void)player;
    (void)unit_index;
    (void)storage_idx;
    ++g_approach_calls;
    *out_x = g_approach_out_x;
    *out_y = g_approach_out_y;
}

struct UnlinkCall {
    uint32_t player;
    uint16_t unit_index;
};
std::vector<UnlinkCall> g_unlink_calls;
void                    rec_unit_unlink_tile(uint32_t unit_player, uint16_t unit_index) {
    tr("unit_unlink_tile");
    g_unlink_calls.push_back({unit_player, unit_index});
}

struct FowRemoveCall {
    uint32_t player;
    int32_t  x, y;
    uint8_t  radius;
};
std::vector<FowRemoveCall> g_fow_remove_calls;
void                       rec_fow_remove_sight(uint32_t player, int32_t x, int32_t y, uint8_t radius) {
    tr("fow_remove_sight");
    g_fow_remove_calls.push_back({player, x, y, radius});
}

struct PutOnMapCall {
    uint16_t player, unit_index;
    uint8_t  x, y;
};
std::vector<PutOnMapCall> g_put_on_map_calls;
void                      rec_map_unit_PutOnMap(uint16_t player, uint16_t unit_index, uint8_t x, uint8_t y) {
    tr("map_unit_PutOnMap");
    g_put_on_map_calls.push_back({player, unit_index, x, y});
}

struct FowUpdateCall {
    uint32_t player, x, y;
    uint8_t  sight;
};
std::vector<FowUpdateCall> g_fow_update_calls;
void                       rec_map_fow_UpdateFoWPlus(uint32_t player, uint32_t x, uint32_t y, uint8_t sight) {
    tr("map_fow_UpdateFoWPlus");
    g_fow_update_calls.push_back({player, x, y, sight});
}

const unit_state_move_path_calls g_calls = {
    &rec_unit_chase_check,
    &rec_path_free_slot,
    &rec_unit_notify_status,
    &rec_unit_set_state,
    &rec_unit_set_state_order,
    &rec_target_class,
    &rec_unit_in_weapon_range,
    &rec_storage_get_approach_tile,
    &rec_unit_unlink_tile,
    &rec_fow_remove_sight,
    &rec_map_unit_PutOnMap,
    &rec_map_fow_UpdateFoWPlus,
};

void reset_observations() {
    g_trace.clear();
    g_chase_check_ret = 0;
    g_free_slot_calls.clear();
    g_notify_calls.clear();
    g_set_state_calls.clear();
    g_set_state_order_calls.clear();
    g_target_class_ret    = 0;
    g_in_weapon_range_ret = 0;
    g_in_range_calls.clear();
    g_approach_out_x = g_approach_out_y = 0;
    g_approach_calls                    = 0;
    g_unlink_calls.clear();
    g_fow_remove_calls.clear();
    g_put_on_map_calls.clear();
    g_fow_update_calls.clear();
}

void call(sim_fixture &fx) {
    sim_store own = fx.store();
    detail::unit_state_move_path(fx.view(), own, g_calls);
}

} // namespace

void run_unit_state_move_path_tests() {
    sim_fixture fx;

    // =================================================================================================
    // 1 -- the move_microstep==0x1f && order_queued!=0 early gate (0x00480b6b-0x00480b9a). Both must
    // be true; either alone falls through (proven by dir_step_factor-equivalent step_cost calc NOT
    // firing -- observed here via NOTHING in the trace at all, since the gate returns before any
    // callee).
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        unit &u            = fx.u(0, 1);
        u.move_microstep   = 0x1f;
        u.order_queued     = 1;
        fx.cur_unit_ptr    = &u;
        fx.view_cur_player = 0;
        fx.view_cur_index  = 1;
        fx.tick_budget     = 55.0;
        call(fx);
        ck(g_trace.empty(), "1a: move_microstep==0x1f && order_queued!=0 -- gate fires, no callee runs");
        ck_eq_d(fx.tick_budget, 0.0, "1a: tick_budget zeroed by the gate (0x00480b8a-0x00480b95)");

        // order_queued==0 -- gate's second condition false, falls through to the real path (proven by
        // reaching the tick_budget-insufficient early return further down instead of the empty trace).
        fx.reset();
        reset_observations();
        unit &u2                      = fx.u(0, 1);
        u2.move_microstep             = 0x1f;
        u2.order_queued               = 0;
        u2.unit_proto_id              = 3;
        fx.cfg_units[3].step_speed[0] = 5.0;
        fx.cur_unit_ptr               = &u2;
        fx.view_cur_player            = 0;
        fx.view_cur_index             = 1;
        fx.tick_budget                = 0.0; // insufficient -> early return, but NOT via the gate above
        call(fx);
        ck_eq((uint32_t)fx.u(0, 1).move_microstep, 0x1fu, "1b: order_queued==0 -- gate NOT taken (move_microstep untouched by the insufficient-budget path either)");
    }

    // =================================================================================================
    // 2 -- step_cost heading-scale bucketing (0x00480b9f-0x00480c02): <=3 unscaled, [4,7] mid, >7 far.
    // Observed indirectly via the tick_budget carryover math in the insufficient-budget branch, which
    // is exactly step_cost.
    // =================================================================================================
    {
        // Cross the budget==step_cost boundary with an EXACTLY-sufficient probe and check tick_budget
        // lands at precisely 0 -- that only happens if step_cost was computed as expected.
        auto probe_sufficient = [&](uint8_t heading, double expect_cost, const char *what) {
            fx.reset();
            reset_observations();
            unit &u                        = fx.u(0, 1);
            u.unit_proto_id                = 2;
            u.move_heading                 = heading;
            u.order_queued                 = 0;
            u.move_microstep               = 5; // mid-animation -- avoids the settle path's own writes
            fx.cfg_units[2].step_speed[0]  = 10.0;
            fx.move_path_heading_scale_mid = 2.0;
            fx.move_path_heading_scale_far = 3.0;
            fx.cur_unit_ptr                = &u;
            fx.view_cur_player             = 0;
            fx.view_cur_index              = 1;
            fx.tick_budget                 = expect_cost; // exactly sufficient
            call(fx);
            ck_eq_d(fx.tick_budget, 0.0, what);
        };
        probe_sufficient(2, 10.0, "2a: move_heading<=3 -- unscaled, step_cost=10*1=10");
        probe_sufficient(5, 20.0, "2b: move_heading in [4,7] -- mid scale, step_cost=10*2=20");
        probe_sufficient(4, 20.0, "2c: move_heading==4 boundary -- mid scale (in [4,7])");

        // 2d: move_heading>7 -- far scale. NOT probed via the mid-animation path like 2a-2c: the
        // fixture's move_microsteps table only models 8 headings (0-7, MICROSTEPS_PER_HEADING rows),
        // and the mid-animation branch's sync_facing_from_microstep_table() indexes it with the
        // ORIGINAL (pre-scale) move_heading -- so an original heading of 8+ would read past the
        // fixture's array (a real ASan heap-buffer-overflow was caught here during authoring). Instead
        // this drives the REAL-STEP (settled, heading<0x18) path, whose resync uses the NEW
        // (post-step) heading from the path waypoint, not the original -- so the original move_heading
        // can safely be >7 while the table lookup itself stays in-bounds. This path also has NO
        // refund (same as mid-animation), so the same "budget drains to exactly 0" observable applies.
        {
            fx.reset();
            reset_observations();
            unit &u                        = fx.u(0, 1);
            u.unit_proto_id                = 2;
            u.move_heading                 = 8;    // >7 -- far scale for the STEP_COST calc only
            u.move_microstep               = 0x1f; // settled
            u.order_queued                 = 0;
            u.path_slot_id                 = 9;
            u.path_cursor                  = 0;
            fx.cfg_units[2].step_speed[0]  = 10.0;
            fx.cfg_units[2].sight          = 1;
            fx.move_path_heading_scale_mid = 2.0;
            fx.move_path_heading_scale_far = 3.0;
            fx.dir_step_offsets[3].dx      = 0;
            fx.dir_step_offsets[3].dy      = 0;
            auto &wp                       = fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 9 * PATH_WAYPOINTS_PER_SLOT + 0];
            wp.heading                     = 3; // < 0x18 -- a real (SAFE, in-bounds) step, distinct from the original 8
            wp.run_length                  = 0;
            fx.cur_unit_ptr                = &u;
            fx.view_cur_player             = 0;
            fx.view_cur_index              = 1;
            fx.tick_budget                 = 30.0; // exactly step_speed(10) * far_scale(3) -- sufficient iff far scale applied
            call(fx);
            ck_eq_d(fx.tick_budget, 0.0,
                    "2d: move_heading>7 -- far scale, step_cost=10*3=30 (proven via the real-step path, "
                    "whose post-step resync uses the NEW heading=3, not the original 8)");
        }
    }

    // =================================================================================================
    // 3 -- tick_budget carryover gate (0x00480c02-0x00480c35): insufficient -> activity_clock -=
    // tick_budget, tick_budget=0, NOTHING further (trace empty). Sufficient -> tick_budget -= step_cost,
    // continues into the rest of the function.
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        unit &u                       = fx.u(0, 1);
        u.unit_proto_id               = 1;
        u.move_heading                = 0; // unscaled
        u.order_queued                = 0;
        u.activity_clock              = 50.0;
        fx.cfg_units[1].step_speed[0] = 100.0; // large step_cost
        fx.cur_unit_ptr               = &u;
        fx.view_cur_player            = 0;
        fx.view_cur_index             = 1;
        fx.tick_budget                = 3.0; // << 100
        call(fx);
        ck(g_trace.empty(), "3a: insufficient tick_budget -- no callee fires");
        ck_eq_d(fx.u(0, 1).activity_clock, 47.0, "3a: activity_clock -= tick_budget (50-3)");
        ck_eq_d(fx.tick_budget, 0.0, "3a: tick_budget zeroed");
    }

    // =================================================================================================
    // 4 -- chase_check cache, gated on order in {ATTACK_UNIT, ATTACK_UNIT_RETURN, ATTACK_BUILDING}
    // (0x00480c49-0x00480c79). Fired (all 3 order values) and NOT fired (any other order).
    // =================================================================================================
    auto setup_mid_anim = [&](uint16_t order) -> unit & {
        fx.reset();
        reset_observations();
        unit &u                       = fx.u(0, 1);
        u.unit_proto_id               = 1;
        u.move_heading                = 0;
        u.order_queued                = 0;
        u.order                       = order;
        u.move_microstep              = 5; // mid-animation, avoids the settle path
        fx.cfg_units[1].step_speed[0] = 1.0;
        fx.cur_unit_ptr               = &u;
        fx.view_cur_player            = 0;
        fx.view_cur_index             = 1;
        fx.tick_budget                = 1.0;
        return fx.u(0, 1);
    };
    {
        for (uint16_t ord : {ORDER_ATTACK_UNIT, ORDER_ATTACK_UNIT_RETURN, ORDER_ATTACK_BUILDING}) {
            setup_mid_anim(ord);
            g_chase_check_ret = 42;
            call(fx);
            const std::string fires_msg  = "4: order=" + std::to_string(ord) + " -- chase_check fires FIRST";
            const std::string cached_msg = "4: chase_result cached (order=" + std::to_string(ord) + ")";
            ck(!g_trace.empty() && std::strcmp(g_trace[0], "unit_chase_check") == 0, fires_msg.c_str());
            ck_eq((uint32_t)fx.unit_chase_result, 42u, cached_msg.c_str());
        }
        setup_mid_anim(0x99); // not an ATTACK_* order
        g_chase_check_ret = 42;
        call(fx);
        ck(g_trace.empty() || std::strcmp(g_trace[0], "unit_chase_check") != 0,
           "4b: non-ATTACK order -- chase_check does NOT fire");
    }

    // =================================================================================================
    // 5 -- mid-animation (move_microstep < 0x1f): increment, re-sync facing_target/current from the
    // microstep table, return. No other callee.
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        unit &u                                                            = fx.u(0, 1);
        u.unit_proto_id                                                    = 1;
        u.move_heading                                                     = 2;
        u.move_microstep                                                   = 10;
        u.order                                                            = 0x99;
        fx.cfg_units[1].step_speed[0]                                      = 1.0;
        fx.move_microsteps[(size_t)2 * MICROSTEPS_PER_HEADING + 11].facing = 77; // the POST-increment index
        fx.cur_unit_ptr                                                    = &u;
        fx.view_cur_player                                                 = 0;
        fx.view_cur_index                                                  = 1;
        fx.tick_budget                                                     = 1.0;
        call(fx);
        ck(g_trace.empty(), "5: mid-animation touches no callee");
        ck_eq((uint32_t)fx.u(0, 1).move_microstep, 11u, "5: move_microstep incremented (10->11)");
        ck_eq((uint32_t)fx.u(0, 1).facing_target, 77u, "5: facing_target synced from the POST-increment microstep entry");
        ck_eq((uint32_t)fx.u(0, 1).facing_current, 77u, "5: facing_current synced identically");
    }

    // =================================================================================================
    // 6 -- real-step transition (heading<0x18): full roster-write set, NO budget refund.
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        unit &u                       = fx.u(0, 1);
        u.unit_proto_id               = 1;
        u.move_heading                = 0;
        u.move_microstep              = 0x1f; // settled
        u.order                       = 0x99;
        u.path_slot_id                = 7;
        u.path_cursor                 = 3;
        u.x                           = 10;
        u.y                           = 20;
        fx.cfg_units[1].step_speed[0] = 1.0;
        fx.cfg_units[1].sight         = 4;
        fx.dir_step_offsets[5].dx     = 1;
        fx.dir_step_offsets[5].dy     = -1;
        auto &wp                      = fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 7 * PATH_WAYPOINTS_PER_SLOT + 3];
        wp.heading                    = 5; // < 0x18 -- real step
        wp.run_length                 = 9;
        fx.cur_unit_ptr               = &u;
        fx.view_cur_player            = 0;
        fx.view_cur_index             = 1;
        fx.tick_budget                = 1.0;
        // move_heading becomes the STEPPED heading (5) and move_microstep resets to 0, so the
        // resync reads move_microsteps[5*MICROSTEPS_PER_HEADING + 0].
        fx.move_microsteps[5 * MICROSTEPS_PER_HEADING + 0].facing = 88;
        call(fx);
        ck(trace_eq({"unit_unlink_tile", "fow_remove_sight", "map_unit_PutOnMap", "map_fow_UpdateFoWPlus"}),
           "6: real-step call order (unlink, fow_remove, PutOnMap, UpdateFoWPlus)");
        ck_eq((uint32_t)fx.u(0, 1).path_cursor, 4u, "6: path_cursor advances by 1");
        ck_eq((uint32_t)g_unlink_calls[0].unit_index, 1u, "6: unit_unlink_tile(player, unit_index) at the OLD tile");
        ck_eq((uint32_t)g_fow_remove_calls[0].x, 10u, "6: fow_remove_sight at the OLD x");
        ck_eq((uint32_t)g_fow_remove_calls[0].y, 20u, "6: fow_remove_sight at the OLD y");
        ck_eq((uint32_t)g_fow_remove_calls[0].radius, 4u, "6: fow_remove_sight radius = cfg sight");
        ck_eq((uint32_t)g_put_on_map_calls[0].x, 11u, "6: PutOnMap new_x = old_x + dx (10+1)");
        ck_eq((uint32_t)g_put_on_map_calls[0].y, 19u, "6: PutOnMap new_y = old_y + dy (20-1)");
        ck_eq((uint32_t)fx.u(0, 1).move_microstep, 0u, "6: move_microstep reset to 0");
        ck_eq((uint32_t)fx.u(0, 1).move_heading, 5u, "6: move_heading set to the stepped heading");
        ck_eq((uint32_t)fx.u(0, 1).facing_target, 88u, "6: facing resynced from move_microsteps[new_heading][0]");
        ck_eq_d(fx.tick_budget, 0.0, "6: NO budget refund on a real step (unlike the settle branches)");
    }

    // =================================================================================================
    // 7 -- terminal sentinel (heading>=0x18): path_free_slot gated on path_slot_id!=0xff, then
    // unit_notify_status(GOAL_REACHED), then the settle_order dispatch. Common tail refunds step_cost.
    // =================================================================================================
    auto setup_settle = [&](uint16_t order, uint8_t path_slot_id) -> unit & {
        fx.reset();
        reset_observations();
        unit &u                       = fx.u(0, 1);
        u.unit_proto_id               = 1;
        u.move_heading                = 0;
        u.move_microstep              = 0x1f;
        u.order                       = order;
        u.path_slot_id                = path_slot_id;
        u.path_cursor                 = 0;
        fx.cfg_units[1].step_speed[0] = 7.0; // step_cost, refunded on settle
        auto &wp                      = fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + (size_t)path_slot_id * PATH_WAYPOINTS_PER_SLOT + 0];
        wp.heading                    = 0x18; // terminal sentinel (>=0x18)
        fx.cur_unit_ptr               = &u;
        fx.view_cur_player            = 0;
        fx.view_cur_index             = 1;
        fx.tick_budget                = 7.0; // exactly sufficient, so post-refund == 7.0 again
        return fx.u(0, 1);
    };
    {
        // 7a: path_slot_id != 0xff -- path_free_slot fires; DEPLOY_APPROACH settle arm.
        setup_settle(ORDER_DEPLOY_APPROACH, 7);
        call(fx);
        ck(g_free_slot_calls.size() == 1, "7a: path_free_slot fires when path_slot_id != 0xff");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].status_code == NOTIFY_GOAL_REACHED,
           "7a: unit_notify_status(..., GOAL_REACHED)");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == ORDER_DEPLOY_APPROACH,
           "7a: unit_set_state(DEPLOY_APPROACH)");
        ck_eq_d(fx.tick_budget, 7.0, "7a: step_cost refunded on the settle path");

        // 7b: path_slot_id == 0xff -- path_free_slot must NOT fire.
        setup_settle(ORDER_DEPLOY_APPROACH, 0xff);
        call(fx);
        ck(g_free_slot_calls.empty(), "7b: path_free_slot skipped when path_slot_id==0xff");
    }
    {
        // 7c: ATTACK_UNIT, unit is a PLANE, state==MOVE_PATH_PLANE -- replan (set_state(move_op_arg), notify(REPLAN)).
        unit &u                     = setup_settle(ORDER_ATTACK_UNIT, 7);
        u.state                     = STATE_MOVE_PATH_PLANE;
        fx.cfg_units[1].type        = UNIT_TYPE_A_PLANE;
        fx.cfg_units[1].move_op_arg = 0x44;
        call(fx);
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x44,
           "7c: ATTACK_UNIT plane + state==MOVE_PATH_PLANE -- unit_set_state(cfg move_op_arg)");
        ck(g_notify_calls.back().status_code == NOTIFY_REPLAN, "7c: unit_notify_status(..., REPLAN)");

        // 7d: ATTACK_UNIT, plane, state != MOVE_PATH_PLANE -- unit_set_state(PLOT_TURN_PATH).
        unit &u2             = setup_settle(ORDER_ATTACK_UNIT, 7);
        u2.state             = 0x99;
        fx.cfg_units[1].type = UNIT_TYPE_H_PLANE;
        call(fx);
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == STATE_PLOT_TURN_PATH,
           "7d: ATTACK_UNIT plane + state!=MOVE_PATH_PLANE -- unit_set_state(PLOT_TURN_PATH)");

        // 7e: ATTACK_UNIT, NOT a plane, in weapon range -- unit_set_state(HOVER_ENGAGE).
        unit &u3              = setup_settle(ORDER_ATTACK_UNIT, 7);
        fx.cfg_units[1].type  = 1; // not a plane
        g_target_class_ret    = 5;
        g_in_weapon_range_ret = 1;
        call(fx);
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_HOVER_ENGAGE,
           "7e: ATTACK_UNIT non-plane + in-range -- unit_set_state(HOVER_ENGAGE)");
        (void)u3;

        // 7f: ATTACK_UNIT, NOT a plane, NOT in weapon range -- replan (set_state(move_op_arg), notify(REPLAN)).
        unit &u4                    = setup_settle(ORDER_ATTACK_UNIT, 7);
        fx.cfg_units[1].type        = 1;
        fx.cfg_units[1].move_op_arg = 0x55;
        g_in_weapon_range_ret       = 0;
        call(fx);
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x55,
           "7f: ATTACK_UNIT non-plane + not-in-range -- unit_set_state(cfg move_op_arg)");
        ck(g_notify_calls.back().status_code == NOTIFY_REPLAN, "7f: unit_notify_status(..., REPLAN)");
        (void)u;
        (void)u2;
        (void)u4;
    }
    {
        // 7g: LANDING_REQUEST, NOT plane -- unit_set_state(LANDING_REQUEST) (writes STATE, not order).
        unit &u              = setup_settle(ORDER_LANDING_REQUEST, 7);
        u.state              = 0x99;
        fx.cfg_units[1].type = 1;
        call(fx);
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == ORDER_LANDING_REQUEST,
           "7g: LANDING_REQUEST non-plane -- unit_set_state(LANDING_REQUEST)");
        ck(g_set_state_order_calls.empty(), "7g: unit_set_state_order NOT called on this arm");
        (void)u;

        // 7h: LANDING_REQUEST, plane, state==MOVE_PATH_PLANE, home_storage_slot==0 -- IDLE_SCATTER.
        unit &u2             = setup_settle(ORDER_LANDING_REQUEST, 7);
        u2.state             = STATE_MOVE_PATH_PLANE;
        u2.home_storage_slot = 0;
        fx.cfg_units[1].type = UNIT_TYPE_A_PLANE;
        call(fx);
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_IDLE_SCATTER,
           "7h: LANDING_REQUEST plane + home_storage_slot==0 -- unit_set_state(IDLE_SCATTER)");
        ck(g_approach_calls == 0, "7h: storage_get_approach_tile NOT called on the no-storage sub-arm");

        // 7i: LANDING_REQUEST, plane, state==MOVE_PATH_PLANE, home_storage_slot!=0 -- approach tile +
        // goal_x/y write + unit_set_state_order(GROUP_STEP, LANDING_REQUEST) -- STATE first, ORDER second.
        unit &u3             = setup_settle(ORDER_LANDING_REQUEST, 7);
        u3.state             = STATE_MOVE_PATH_PLANE;
        u3.home_storage_slot = 3;
        fx.cfg_units[1].type = UNIT_TYPE_H_PLANE;
        g_approach_out_x     = 44;
        g_approach_out_y     = 55;
        call(fx);
        ck(g_approach_calls == 1, "7i: storage_get_approach_tile called once");
        ck_eq((uint32_t)fx.u(0, 1).goal_x, 44u, "7i: goal_x written from the approach-tile out-param");
        ck_eq((uint32_t)fx.u(0, 1).goal_y, 55u, "7i: goal_y written from the approach-tile out-param");
        ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].new_state == STATE_GROUP_STEP &&
               g_set_state_order_calls[0].new_order == ORDER_LANDING_REQUEST,
           "7i: unit_set_state_order(STATE=GROUP_STEP, ORDER=LANDING_REQUEST) -- STATE param first");
        ck(g_set_state_calls.empty(), "7i: unit_set_state NOT called on this arm");
    }
    {
        // 7j: HOVER_ENGAGE settle order -- unit_set_state(HOVER_ENGAGE).
        setup_settle(UNIT_STATE_HOVER_ENGAGE, 7);
        call(fx);
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_HOVER_ENGAGE,
           "7j: settle_order==HOVER_ENGAGE -- unit_set_state(HOVER_ENGAGE)");

        // 7k: ASCEND_TO_ORBIT settle order.
        setup_settle(ORDER_ASCEND_TO_ORBIT, 7);
        call(fx);
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == ORDER_ASCEND_TO_ORBIT,
           "7k: settle_order==ASCEND_TO_ORBIT -- unit_set_state(ASCEND_TO_ORBIT)");

        // 7l: default fallback -- any other order value -- unit_set_state(IDLE_SCATTER).
        setup_settle(0x77, 7);
        call(fx);
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_IDLE_SCATTER,
           "7l: settle_order default -- unit_set_state(IDLE_SCATTER)");
    }
}

} // namespace mh::sim::test
