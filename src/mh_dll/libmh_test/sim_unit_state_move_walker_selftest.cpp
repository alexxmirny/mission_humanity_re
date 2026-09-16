//
// sim_unit_state_move_walker_selftest.cpp -- `simtest` cases for llm_strat_unit_state_move_walker
// @0x0047c903 (sim/sim_unit_state_move_walker.h/.cpp). The biggest handler in the SIM1-G1 batch and a
// roster writer (path_buffers, soldiers, passable, tile_objects) -- this file is its PRIMARY
// verification (rig shadow sites deferred per the batch context).
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM tmp/decomp/llm_strat_unit_state_move_walker_0047c903.asm --
// every assertion below cites the instruction address(es) it pins. NOT read off the .cpp body (the
// .cpp's own header/body comments already carry extensive asm citations from the translator; this file
// re-derives them independently by reading the .asm directly, and agrees with them throughout).
//
// SCOPE (honest, not exhaustive): the order_queued/move_microstep==0x1f early gate (both sides, plus
// the "gate condition false because move_microstep!=0x1f" side); the mid-step budget spend (insufficient
// -> activity_clock carryover, sufficient -> spend, reaching vs not-yet-reaching 0x1f); the THREE
// combat-recheck cascades that fire the instant move_microstep reaches 0x1f (ATTACK_UNIT/_RETURN dead-
// target cleanup incl. the RETURN-home sub-branch, alive+in-range fire, alive+not-in-range with the
// every-4th-waypoint goal-recheck escape hatch both ways; ATTACK_BUILDING's dead/alive-in-range/alive-
// not-in-range trio, with its CALL-ORDER DIFFERENCE from ATTACK_UNIT's dead branch --
// set_state_order-then-notify vs release-notify-then-set_state_order); the settled/path-advance half:
// the spent-waypoint cursor skip (both sides), the not-should-settle GROUP_MARSHAL bailout, the
// should-settle arrival (at-goal alone, and the order-vs-state DISAMBIGUATION -- should_settle reads
// `order`, the in-range recheck + footprint reroll read `state` independently, proven with a case where
// they name DIFFERENT literals), the STEP path's full roster write set (tile_objects vacate+claim,
// passable restore+claim, path_buffer run_length decrement, move_step_speed_scale averaging filter,
// FoW calls, soldier idle-wander clear gated on BOTH soldier_count AND the flag) plus its budget gate
// and its blocked/detour path (INCLUDING the same-tick retry-count escalation subtlety: a detour
// request sets path_blocked_retry_count=0xc9 and the THEN-read old-count sees that same 0xc9, so a
// FIRST detour can bail out to GROUP_MARSHAL in the very tick it fires); the TURN path's shortest-arc
// direction choice (both the diff>12 and diff<=-12 boundary tests, plus the "small diff" fallthrough
// case), the mod-24 wrap both directions, the soldier-nudge gate, and the walk_step_allowed gate
// (including its BYTE-truncation nuance -- TEST AL,AL, not a full-int32 test). The arrival should-settle
// OR-chain (0x0047cf28-0x0047cf9e) is exercised via at-goal (case 11), order==SQUAD_MERGE (case 12), and
// order==PATROL_SWAP (case 13) -- it does NOT separately construct order==ATTACK_UNIT/ATTACK_BUILDING as
// THIS chain's own trigger (structurally identical OR-arms to the ones already exercised; those two
// literals ARE covered extensively as combat-recheck triggers in the mid-step region, cases 3-8, which is
// a different code path). Also does not vary the wrap-adjacent facing values beyond the ones picked.
// Both are honest gaps, not suspected bugs.
//
#include "sim/sim_unit_state_move_walker.h"

#include "sim/sim_order_enqueue.h" // UNIT_STATE_STOP_TO_DEFAULT, UNIT_STATE_PATROL_SWAP

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// llm_strat_unit_state (order/state) literals this function compares against -- NOT backed by a real
// Ghidra enum (see the .cpp's own header banner); these are the SAME numeric values the .cpp's
// detail::-scoped MOVE_WALKER_STATE_* constants hold (0x0047ca32/ca3e/cd47/cf92/cc43 respectively),
// redeclared here because those constants are `detail::`-local and not visible from this TU.
constexpr uint16_t ST_ATTACK_UNIT        = 0x1a;
constexpr uint16_t ST_ATTACK_UNIT_RETURN = 0x1b;
constexpr uint16_t ST_ATTACK_BUILDING    = 0x1c;
constexpr uint16_t ST_SQUAD_MERGE        = 0x33;
constexpr uint16_t ST_GROUP_MARSHAL      = 0x0a;
constexpr uint32_t NOTIFY_STILL_CHASING  = 0x66;
constexpr uint32_t NOTIFY_GOAL_REACHED   = 0x65;
constexpr uint32_t NOTIFY_REPLAN         = 1;

// ---- shared trace: one sequence proves CALL ORDER across all 19 callees ------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }
bool                      trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- return-value / behaviour knobs (settable per case BEFORE the call) ------------------------
double   g_dir_step_factor_ret      = 1.0;
int32_t  g_target_class_ret         = 0;
uint32_t g_in_weapon_range_ret      = 0;
int32_t  g_goal_in_weapon_range_ret = 0;
int32_t  g_detour_ret               = 0;
int32_t  g_walk_step_allowed_ret    = 0;
int32_t  g_facing24_dx = 0, g_facing24_dy = 0;
int32_t  g_get_coords_fine_x = 0, g_get_coords_fine_y = 0;
int32_t  g_bldg_get_coords_fine_x = 0, g_bldg_get_coords_fine_y = 0;
int32_t  g_bldg_footprint_fine_x = 0, g_bldg_footprint_fine_y = 0;

// ---- per-callee recorders (19, one per unit_state_move_walker_calls member) ---------------------
std::vector<int32_t> g_dir_step_factor_args;
double               rec_dir_step_factor(int32_t dir) {
    tr("dir_step_factor");
    g_dir_step_factor_args.push_back(dir);
    return g_dir_step_factor_ret;
}

struct TargetClassCall {
    uint32_t owner_and_kind_flag;
    int32_t  roster_slot;
};
std::vector<TargetClassCall> g_target_class_calls;
int32_t                      rec_target_class(uint32_t owner_and_kind_flag, int32_t roster_slot) {
    tr("target_class");
    g_target_class_calls.push_back({owner_and_kind_flag, roster_slot});
    return g_target_class_ret;
}

struct RangeCall {
    int32_t player, unit_idx, tile_x, tile_y, target_class_flags;
};
std::vector<RangeCall> g_in_range_calls;
uint32_t               rec_unit_in_weapon_range(int32_t player, int32_t unit_idx, int32_t tile_x, int32_t tile_y,
                                                int32_t target_class_flags) {
    tr("unit_in_weapon_range");
    g_in_range_calls.push_back({player, unit_idx, tile_x, tile_y, target_class_flags});
    return g_in_weapon_range_ret;
}
std::vector<RangeCall> g_goal_in_range_calls;
int32_t                rec_unit_goal_in_weapon_range(int32_t player, int32_t unit_idx, int32_t tile_x, int32_t tile_y,
                                                     int32_t target_class_flags) {
    tr("unit_goal_in_weapon_range");
    g_goal_in_range_calls.push_back({player, unit_idx, tile_x, tile_y, target_class_flags});
    return g_goal_in_weapon_range_ret;
}

int  g_fire_count = 0;
void rec_unit_fire_at_target_if_aimed() {
    tr("unit_fire_at_target_if_aimed");
    ++g_fire_count;
}

struct SetStateOrderCall {
    uint16_t new_order, new_state;
};
std::vector<SetStateOrderCall> g_set_state_order_calls;
void                           rec_unit_set_state_order(uint16_t new_order, uint16_t new_state) {
    tr("unit_set_state_order");
    g_set_state_order_calls.push_back({new_order, new_state});
}

std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) {
    tr("unit_set_state");
    g_set_state_calls.push_back(new_state);
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

struct ReleaseCall {
    uint32_t player_idx;
    int32_t  unit_idx;
    uint32_t mode;
};
std::vector<ReleaseCall> g_release_calls;
void                     rec_target_release_ref(uint32_t player_idx, int32_t unit_idx, uint32_t mode) {
    tr("target_release_ref");
    g_release_calls.push_back({player_idx, unit_idx, mode});
}

struct MoveAutoCall {
    uint16_t player;
    int32_t  unit_idx;
    uint32_t x, y;
};
std::vector<MoveAutoCall> g_move_auto_calls;
void                      rec_unit_order_move_auto(uint16_t player, int32_t unit_idx, uint32_t x, uint32_t y) {
    tr("unit_order_move_auto");
    g_move_auto_calls.push_back({player, unit_idx, x, y});
}

struct GetCoordsCall {
    uint16_t player;
    int32_t  unit_index;
};
std::vector<GetCoordsCall> g_get_coords_calls;
void                       rec_unit_get_coords(uint16_t player, int32_t unit_index, int32_t *out_fine_x, int32_t *out_fine_y) {
    tr("unit_get_coords");
    g_get_coords_calls.push_back({player, unit_index});
    *out_fine_x = g_get_coords_fine_x;
    *out_fine_y = g_get_coords_fine_y;
}

struct BldgGetCoordsCall {
    uint16_t player;
    int32_t  building_index;
};
std::vector<BldgGetCoordsCall> g_bldg_get_coords_calls;
void                           rec_bldg_get_coords(uint16_t player, int32_t building_index, int32_t *out_fine_x,
                                                   int32_t *out_fine_y) {
    tr("bldg_get_coords");
    g_bldg_get_coords_calls.push_back({player, building_index});
    *out_fine_x = g_bldg_get_coords_fine_x;
    *out_fine_y = g_bldg_get_coords_fine_y;
}

struct FootprintCall {
    uint32_t player, unit_idx;
    int32_t  target_owner, target_index;
};
std::vector<FootprintCall> g_footprint_calls;
void                       rec_bldg_footprint_random_offset(uint32_t player, uint32_t unit_idx, int32_t target_owner,
                                                            int32_t target_index, uint32_t *out_fine_x, uint32_t *out_fine_y) {
    tr("bldg_footprint_random_offset");
    g_footprint_calls.push_back({player, unit_idx, target_owner, target_index});
    *out_fine_x = (uint32_t)g_bldg_footprint_fine_x;
    *out_fine_y = (uint32_t)g_bldg_footprint_fine_y;
}

std::vector<uint32_t> g_facing24_args;
void                  rec_facing24_to_delta(uint32_t facing24, int32_t *out_dx, int32_t *out_dy) {
    tr("facing24_to_delta");
    g_facing24_args.push_back(facing24);
    *out_dx = g_facing24_dx;
    *out_dy = g_facing24_dy;
}

struct DetourCall {
    uint32_t src_x, src_y;
    int32_t  dst_x, dst_y;
};
std::vector<DetourCall> g_detour_calls;
int32_t                 rec_path_step_check_and_request_detour(uint32_t src_x, uint32_t src_y, int32_t dst_x,
                                                               int32_t dst_y) {
    tr("path_step_check_and_request_detour");
    g_detour_calls.push_back({src_x, src_y, dst_x, dst_y});
    return g_detour_ret;
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

struct FowUpdateCall {
    uint32_t player, x, y;
    uint8_t  sight;
};
std::vector<FowUpdateCall> g_fow_update_calls;
void                       rec_map_fow_UpdateFoWPlus(uint32_t player, uint32_t x, uint32_t y, uint8_t sight) {
    tr("map_fow_UpdateFoWPlus");
    g_fow_update_calls.push_back({player, x, y, sight});
}

struct SoldierHeadingCall {
    uint16_t player;
    int32_t  unit_index;
    uint8_t  sprite_frame;
};
std::vector<SoldierHeadingCall> g_soldier_heading_calls;
void                            rec_unit_soldiers_set_heading(uint16_t player, int32_t unit_index, uint8_t sprite_frame) {
    tr("unit_soldiers_set_heading");
    g_soldier_heading_calls.push_back({player, unit_index, sprite_frame});
}

int     g_walk_step_allowed_calls = 0;
int32_t rec_unit_walk_step_allowed(uint32_t player, int32_t unit_index) {
    tr("unit_walk_step_allowed");
    (void)player;
    (void)unit_index;
    ++g_walk_step_allowed_calls;
    return g_walk_step_allowed_ret;
}

const unit_state_move_walker_calls g_calls = {
    &rec_dir_step_factor,
    &rec_target_class,
    &rec_unit_in_weapon_range,
    &rec_unit_goal_in_weapon_range,
    &rec_unit_fire_at_target_if_aimed,
    &rec_unit_set_state_order,
    &rec_unit_set_state,
    &rec_unit_notify_status,
    &rec_target_release_ref,
    &rec_unit_order_move_auto,
    &rec_unit_get_coords,
    &rec_bldg_get_coords,
    &rec_bldg_footprint_random_offset,
    &rec_facing24_to_delta,
    &rec_path_step_check_and_request_detour,
    &rec_fow_remove_sight,
    &rec_map_fow_UpdateFoWPlus,
    &rec_unit_soldiers_set_heading,
    &rec_unit_walk_step_allowed,
};

void reset_observations() {
    g_trace.clear();
    g_dir_step_factor_args.clear();
    g_target_class_calls.clear();
    g_in_range_calls.clear();
    g_goal_in_range_calls.clear();
    g_fire_count = 0;
    g_set_state_order_calls.clear();
    g_set_state_calls.clear();
    g_notify_calls.clear();
    g_release_calls.clear();
    g_move_auto_calls.clear();
    g_get_coords_calls.clear();
    g_bldg_get_coords_calls.clear();
    g_footprint_calls.clear();
    g_facing24_args.clear();
    g_detour_calls.clear();
    g_fow_remove_calls.clear();
    g_fow_update_calls.clear();
    g_soldier_heading_calls.clear();
    g_walk_step_allowed_calls = 0;
}

// Common per-case reset: fresh fixture + fresh observations + benign default knobs (dir_step_factor
// returns 1.0 so an unset step_cost defaults to step_speed*scale rather than 0, which would mask a
// budget-gate bug by always looking "sufficient").
void reset_all(sim_fixture &fx) {
    fx.reset();
    reset_observations();
    g_dir_step_factor_ret      = 1.0;
    g_target_class_ret         = 0;
    g_in_weapon_range_ret      = 0;
    g_goal_in_weapon_range_ret = 0;
    g_detour_ret               = 0;
    g_walk_step_allowed_ret    = 0;
    g_facing24_dx              = 0;
    g_facing24_dy              = 0;
}

void call(sim_fixture &fx) {
    sim_store own = fx.store();
    detail::unit_state_move_walker(fx.view(), own, g_calls);
}

} // namespace

void run_unit_state_move_walker_tests() {
    sim_fixture fx;

    // =================================================================================================
    // 1 -- the order_queued/move_microstep==0x1f early gate (0x0047c91b-0x0047c94a). Both must be true
    // to short-circuit; either alone falls through to the normal path (proven here by whether
    // dir_step_factor -- called unconditionally at 0x0047c988 once the gate is NOT taken -- fires at
    // all: the gated path calls NOTHING).
    // =================================================================================================
    {
        reset_all(fx);
        unit &u            = fx.u(0, 1);
        u.order_queued     = 1;
        u.move_microstep   = 0x1f;
        u.activity_clock   = 123.0; // must stay untouched -- the gate does NOT touch it
        fx.cur_unit_ptr    = &u;
        fx.view_cur_player = 0;
        fx.view_cur_index  = 1;
        fx.tick_budget     = 77.0;
        call(fx);
        ck(g_trace.empty(), "1a: order_queued!=0 && move_microstep==0x1f -- the gate fires, NO callee runs (0x0047c936)");
        ck_eq_d(fx.tick_budget, 0.0, "1a: tick_budget zeroed by the gate (0x0047c936-0x0047c944)");
        ck_eq_d(fx.u(0, 1).activity_clock, 123.0, "1a: activity_clock untouched by the gate (distinguishes it from the mid-step insufficient-budget carryover)");

        // order_queued!=0 but move_microstep!=0x1f -- gate's SECOND condition false, falls through.
        reset_all(fx);
        unit &u2           = fx.u(0, 1);
        u2.order_queued    = 1;
        u2.move_microstep  = 0x10; // != 0x1f
        fx.cur_unit_ptr    = &u2;
        fx.view_cur_player = 0;
        fx.view_cur_index  = 1;
        fx.tick_budget     = 0.5; // small -- forces the insufficient-budget arm so nothing further happens
        call(fx);
        ck(g_dir_step_factor_args.size() == 1,
           "1b: order_queued!=0 but move_microstep(0x10)!=0x1f -- gate NOT taken, normal path runs (dir_step_factor called, 0x0047c988)");

        // order_queued==0 -- gate's FIRST condition false, falls through regardless of move_microstep.
        reset_all(fx);
        unit &u3           = fx.u(0, 1);
        u3.order_queued    = 0;
        u3.move_microstep  = 0x1f;
        fx.cur_unit_ptr    = &u3;
        fx.view_cur_player = 0;
        fx.view_cur_index  = 1;
        fx.tick_budget     = 0.0; // arrival path never touches budget, so a non-zero start would also work
        call(fx);
        ck(g_dir_step_factor_args.size() == 1,
           "1c: order_queued==0 -- gate NOT taken even though move_microstep==0x1f (0x0047c924 JZ taken)");
    }

    // =================================================================================================
    // 2 -- mid-step budget spend (move_microstep < 0x1f, 0x0047c99f JGE not taken).
    // =================================================================================================
    {
        // 2a: insufficient budget -- activity_clock -= tick_budget (FSUBR: mem-ST0, 0x0047c9be), NOT the
        // other way around; tick_budget -> 0; move_microstep UNCHANGED.
        reset_all(fx);
        unit &u                       = fx.u(0, 1);
        u.unit_proto_id               = 5;
        u.move_microstep              = 7;
        u.activity_clock              = 100.0;
        u.facing_target               = 3;
        u.move_step_speed_scale       = 3.0;
        fx.cfg_units[5].step_speed[0] = 2.0;
        g_dir_step_factor_ret         = 4.0; // step_cost = 2*3*4 = 24.0
        fx.cur_unit_ptr               = &u;
        fx.view_cur_player            = 0;
        fx.view_cur_index             = 1;
        fx.tick_budget                = 10.0; // < 24.0
        call(fx);
        ck(trace_eq({"dir_step_factor"}), "2a: insufficient-budget mid-step touches no other callee");
        ck_eq_d(fx.u(0, 1).activity_clock, 90.0, "2a: activity_clock = activity_clock - tick_budget = 100-10 (0x0047c9be-0x0047c9c1)");
        ck_eq_d(fx.tick_budget, 0.0, "2a: tick_budget zeroed (0x0047c9c4-0x0047c9d1)");
        ck_eq((uint32_t)fx.u(0, 1).move_microstep, 7u, "2a: move_microstep NOT incremented when budget is insufficient");

        // 2b: sufficient budget, does NOT reach 0x1f this tick -- spend, increment, return (nothing else).
        reset_all(fx);
        unit &u2                      = fx.u(0, 1);
        u2.unit_proto_id              = 5;
        u2.move_microstep             = 0x10;
        fx.cfg_units[5].step_speed[0] = 1.0;
        u2.move_step_speed_scale      = 1.0;
        g_dir_step_factor_ret         = 1.0; // step_cost = 1.0
        fx.cur_unit_ptr               = &u2;
        fx.view_cur_player            = 0;
        fx.view_cur_index             = 1;
        fx.tick_budget                = 10.0;
        call(fx);
        ck(trace_eq({"dir_step_factor"}), "2b: mid-step spend that doesn't reach 0x1f touches no other callee (0x0047ca03 JNZ taken)");
        ck_eq_d(fx.tick_budget, 9.0, "2b: tick_budget -= step_cost (0x0047c9dd-0x0047c9e6)");
        ck_eq((uint32_t)fx.u(0, 1).move_microstep, 0x11u, "2b: move_microstep += 1 (0x0047c9ec-0x0047c9f1)");

        // 2c: sufficient budget, REACHES 0x1f this tick -- settles: passable[x,y] cleared, order NOT one
        // of the combat states so the recheck cascade does nothing further.
        reset_all(fx);
        unit &u3                      = fx.u(0, 1);
        u3.unit_proto_id              = 5;
        u3.move_microstep             = 0x1e;
        u3.order                      = 0x99; // deliberately not ATTACK_UNIT/_RETURN/_BUILDING
        u3.x                          = 10;
        u3.y                          = 20;
        fx.cfg_units[5].step_speed[0] = 1.0;
        u3.move_step_speed_scale      = 1.0;
        g_dir_step_factor_ret         = 1.0;
        fx.passable[(10u << 8) | 20u] = 7; // must become 0
        fx.passable[(11u << 8) | 20u] = 9; // neighbouring tile -- must stay untouched
        fx.cur_unit_ptr               = &u3;
        fx.view_cur_player            = 0;
        fx.view_cur_index             = 1;
        fx.tick_budget                = 10.0;
        call(fx);
        ck(trace_eq({"dir_step_factor"}), "2c: reaching 0x1f with a non-combat order touches no callee beyond the step-cost calc");
        ck_eq((uint32_t)fx.u(0, 1).move_microstep, 0x1fu, "2c: move_microstep reaches exactly 0x1f");
        ck_eq((uint32_t)fx.passable[(10u << 8) | 20u], 0u, "2c: passable[x,y] cleared to 0 on settling (0x0047ca09-0x0047ca26)");
        ck_eq((uint32_t)fx.passable[(11u << 8) | 20u], 9u, "2c: neighbouring passable cell untouched");
    }

    // =================================================================================================
    // 3 -- ATTACK_UNIT / ATTACK_UNIT_RETURN dead-target cleanup (0x0047cc6a-0x0047cd42), all fired via
    // reaching 0x1f this tick (same mid-step-spend setup as case 2c, order set to the combat state).
    // =================================================================================================
    auto settle_into_combat_recheck = [&](uint16_t order, int32_t target_owner_from_ref, int16_t target_ref,
                                          int16_t target_index) -> unit & {
        reset_all(fx);
        unit &u                       = fx.u(0, 1);
        u.unit_proto_id               = 5;
        u.move_microstep              = 0x1e;
        u.order                       = order;
        u.target_ref                  = target_ref;
        u.target_index                = target_index;
        fx.cfg_units[5].step_speed[0] = 1.0;
        u.move_step_speed_scale       = 1.0;
        g_dir_step_factor_ret         = 1.0;
        (void)target_owner_from_ref;
        fx.cur_unit_ptr    = &u;
        fx.view_cur_player = 0;
        fx.view_cur_index  = 1;
        fx.tick_budget     = 10.0;
        return fx.u(0, 1);
    };
    {
        // 3a: ATTACK_UNIT, target dead, target_ref!=0 -- release+clear, notify, (not RETURN so no
        // move_auto), set_state_order(STOP,STOP).
        unit &u                                             = settle_into_combat_recheck(ST_ATTACK_UNIT, 1, 0x21, 3);
        fx.u(ref_owner((uint32_t)(uint16_t)0x21), 3).energy = -1.0; // dead
        (void)u;
        call(fx);
        ck(trace_eq({"dir_step_factor", "target_release_ref", "unit_notify_status", "unit_set_state_order"}),
           "3a: ATTACK_UNIT dead-target, target_ref!=0 -- exact call order (0x0047cc6a-0x0047cd3d)");
        ck(g_release_calls.size() == 1 && g_release_calls[0].player_idx == 0 && g_release_calls[0].unit_idx == 1 &&
               g_release_calls[0].mode == 1u,
           "3a: target_release_ref(cur_player, cur_index, mode=1) (0x0047cc79-0x0047cc8c)");
        ck_eq((uint32_t)(uint16_t)fx.u(0, 1).target_ref, 0u, "3a: target_ref cleared (0x0047cc96)");
        ck_eq((uint32_t)(uint16_t)fx.u(0, 1).target_index, 0u, "3a: target_index cleared (0x0047cca4)");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].status_code == NOTIFY_STILL_CHASING,
           "3a: unit_notify_status(..., 0x66) (0x0047ccc0)");
        ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].new_order == UNIT_STATE_STOP_TO_DEFAULT &&
               g_set_state_order_calls[0].new_state == UNIT_STATE_STOP_TO_DEFAULT,
           "3a: unit_set_state_order(STOP_TO_DEFAULT, STOP_TO_DEFAULT) (0x0047cd2e-0x0047cd3d)");

        // 3b: ATTACK_UNIT, target dead, target_ref==0 -- skip the release call entirely.
        settle_into_combat_recheck(ST_ATTACK_UNIT, 1, 0, 0);
        fx.u(0, 0).energy = -1.0; // target_owner=ref_owner(0)=0, target_slot=0 -- dead
        call(fx);
        ck(trace_eq({"dir_step_factor", "unit_notify_status", "unit_set_state_order"}),
           "3b: ATTACK_UNIT dead-target, target_ref==0 -- release call skipped (0x0047cc77 JZ taken)");

        // 3c: ATTACK_UNIT_RETURN, target dead, target_ref!=0, unit NOT at home -- extra move_auto call.
        unit &u3          = settle_into_combat_recheck(ST_ATTACK_UNIT_RETURN, 1, 0x21, 3);
        u3.x              = 50;
        u3.y              = 60;
        u3.home_x         = 51; // != x -- not home
        u3.home_y         = 60;
        fx.u(1, 3).energy = -1.0;
        call(fx);
        ck(trace_eq({"dir_step_factor", "target_release_ref", "unit_notify_status", "unit_order_move_auto",
                     "unit_set_state_order"}),
           "3c: ATTACK_UNIT_RETURN dead-target, not at home -- move_auto inserted before set_state_order (0x0047ccd1-0x0047cd29)");
        ck(g_move_auto_calls.size() == 1 && g_move_auto_calls[0].x == 51u && g_move_auto_calls[0].y == 60u,
           "3c: unit_order_move_auto(player, unit_idx, home_x, home_y) (0x0047cd03-0x0047cd29)");

        // 3d: ATTACK_UNIT_RETURN, target dead, unit ALREADY at home -- no move_auto call.
        unit &u4          = settle_into_combat_recheck(ST_ATTACK_UNIT_RETURN, 1, 0x21, 3);
        u4.x              = 50;
        u4.y              = 60;
        u4.home_x         = 50;
        u4.home_y         = 60;
        fx.u(1, 3).energy = -1.0;
        call(fx);
        ck(trace_eq({"dir_step_factor", "target_release_ref", "unit_notify_status", "unit_set_state_order"}),
           "3d: ATTACK_UNIT_RETURN dead-target, already home -- move_auto NOT called (0x0047cce2/0x0047ccfb both equal, JZ taken)");
    }

    // =================================================================================================
    // 4 -- ATTACK_UNIT, target ALIVE, in weapon range -- fire, keep order, STOP the state, notify.
    // =================================================================================================
    {
        unit &u               = settle_into_combat_recheck(ST_ATTACK_UNIT, 1, 0x21, 3);
        u.target_fine_x       = 1600; // -> tile 50
        u.target_fine_y       = 1760; // -> tile 55
        fx.u(1, 3).energy     = 5.0;  // alive
        g_target_class_ret    = 77;
        g_in_weapon_range_ret = 1; // in range
        call(fx);
        ck(trace_eq({"dir_step_factor", "target_class", "unit_in_weapon_range", "unit_fire_at_target_if_aimed",
                     "unit_set_state_order", "unit_notify_status"}),
           "4: ATTACK_UNIT alive+in-range exact call order (0x0047ca88-0x0047cb2d)");
        ck(g_in_range_calls.size() == 1 && g_in_range_calls[0].tile_x == 50 && g_in_range_calls[0].tile_y == 55 &&
               g_in_range_calls[0].target_class_flags == 77,
           "4: unit_in_weapon_range(player, unit_idx, fine_to_tile(target_fine_x)=50, fine_to_tile(target_fine_y)=55, "
           "target_class) (0x0047caa5-0x0047caf4)");
        ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].new_order == ST_ATTACK_UNIT &&
               g_set_state_order_calls[0].new_state == UNIT_STATE_STOP_TO_DEFAULT,
           "4: unit_set_state_order(u.order=ATTACK_UNIT, STOP_TO_DEFAULT) (0x0047cb02-0x0047cb10)");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].status_code == NOTIFY_STILL_CHASING,
           "4: unit_notify_status(..., 0x66) (0x0047cb15-0x0047cb28)");
    }

    // =================================================================================================
    // 5 -- ATTACK_UNIT, alive, NOT in range, NOT the every-4th waypoint -- nothing further happens.
    // =================================================================================================
    {
        unit &u               = settle_into_combat_recheck(ST_ATTACK_UNIT, 1, 0x21, 3);
        u.path_cursor         = 0; // (0+1)%4 = 1 != 0
        fx.u(1, 3).energy     = 5.0;
        g_in_weapon_range_ret = 0;
        call(fx);
        ck(trace_eq({"dir_step_factor", "target_class", "unit_in_weapon_range"}),
           "5: ATTACK_UNIT alive+not-in-range+not-every-4th -- no further callee (0x0047cb4d JNZ taken)");
    }

    // =================================================================================================
    // 6 -- ATTACK_UNIT, alive, NOT in range, IS the every-4th waypoint, but run_length!=0 -- the
    // secondary gate at 0x0047cb7f/0x0047cb86 still suppresses the re-fetch.
    // =================================================================================================
    {
        unit &u                                                                                     = settle_into_combat_recheck(ST_ATTACK_UNIT, 1, 0x21, 3);
        u.path_slot_id                                                                              = 4;
        u.path_cursor                                                                               = 3; // (3+1)%4 = 0
        fx.u(1, 3).energy                                                                           = 5.0;
        g_in_weapon_range_ret                                                                       = 0;
        fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 4 * PATH_WAYPOINTS_PER_SLOT + 3].run_length = 2; // != 0
        call(fx);
        ck(trace_eq({"dir_step_factor", "target_class", "unit_in_weapon_range"}),
           "6: every-4th waypoint but run_length!=0 -- re-fetch suppressed (0x0047cb7f CMP/0x0047cb86 JZ not taken)");
    }

    // =================================================================================================
    // 7 -- ATTACK_UNIT, alive, NOT in range, every-4th AND run_length==0 -- re-fetch the target's live
    // coords (mutating u.target_fine_x/_y in place), re-check with the GOAL variant, both outcomes.
    // =================================================================================================
    {
        // 7a: goal_in_range != 0 -- bail out WITHOUT the GROUP_MARSHAL/notify pair.
        unit &u                                                                                     = settle_into_combat_recheck(ST_ATTACK_UNIT, 1, 0x21, 3);
        u.path_slot_id                                                                              = 4;
        u.path_cursor                                                                               = 3;
        fx.u(1, 3).energy                                                                           = 5.0;
        g_in_weapon_range_ret                                                                       = 0;
        g_get_coords_fine_x                                                                         = 3200; // -> tile 100
        g_get_coords_fine_y                                                                         = 3392; // -> tile 106
        g_target_class_ret                                                                          = 88;
        g_goal_in_weapon_range_ret                                                                  = 1; // in range
        fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 4 * PATH_WAYPOINTS_PER_SLOT + 3].run_length = 0;
        call(fx);
        ck(trace_eq({"dir_step_factor", "target_class", "unit_in_weapon_range", "unit_get_coords", "target_class",
                     "unit_goal_in_weapon_range"}),
           "7a: every-4th + run_length==0 -- re-fetch + re-check fire, goal_in_range!=0 stops there (0x0047cb8d-0x0047cc41 JNZ taken)");
        ck(g_get_coords_calls.size() == 1 && g_get_coords_calls[0].player == 1 && g_get_coords_calls[0].unit_index == 3,
           "7a: unit_get_coords(target_owner=1, target_slot=3, &target_fine_x, &target_fine_y) (0x0047cb99-0x0047cbc5)");
        ck_eq((uint32_t)fx.u(0, 1).target_fine_x, 3200u, "7a: u.target_fine_x mutated to the re-fetched value");
        ck_eq((uint32_t)fx.u(0, 1).target_fine_y, 3392u, "7a: u.target_fine_y mutated to the re-fetched value");
        ck(g_goal_in_range_calls.size() == 1 && g_goal_in_range_calls[0].tile_x == 100 && g_goal_in_range_calls[0].tile_y == 106,
           "7a: unit_goal_in_weapon_range uses the FRESHLY re-fetched fine coords (0x0047cc0b-0x0047cc3a)");

        // 7b: goal_in_range == 0 -- GROUP_MARSHAL bailout + notify(STILL_CHASING).
        unit &u2                                                                                    = settle_into_combat_recheck(ST_ATTACK_UNIT, 1, 0x21, 3);
        u2.path_slot_id                                                                             = 4;
        u2.path_cursor                                                                              = 3;
        fx.u(1, 3).energy                                                                           = 5.0;
        g_in_weapon_range_ret                                                                       = 0;
        g_get_coords_fine_x                                                                         = 3200;
        g_get_coords_fine_y                                                                         = 3392;
        g_goal_in_weapon_range_ret                                                                  = 0; // not in range
        fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 4 * PATH_WAYPOINTS_PER_SLOT + 3].run_length = 0;
        call(fx);
        ck(trace_eq({"dir_step_factor", "target_class", "unit_in_weapon_range", "unit_get_coords", "target_class",
                     "unit_goal_in_weapon_range", "unit_set_state", "unit_notify_status"}),
           "7b: goal_in_range==0 -- GROUP_MARSHAL(0xa) + notify(STILL_CHASING) appended (0x0047cc43-0x0047cc65)");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == ST_GROUP_MARSHAL,
           "7b: unit_set_state(GROUP_MARSHAL=0xa) (0x0047cc43-0x0047cc48)");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].status_code == NOTIFY_STILL_CHASING,
           "7b: unit_notify_status(..., 0x66) even on the bailout (0x0047cc4d-0x0047cc60)");
    }

    // =================================================================================================
    // 8 -- ATTACK_BUILDING (0x0047cd42-0x0047ce9d). Note the CALL-ORDER DIFFERENCE from ATTACK_UNIT's
    // own dead-target branch: here set_state_order is called BEFORE notify (0x0047ce85/0x0047ce9d),
    // whereas ATTACK_UNIT's dead branch calls notify BEFORE set_state_order (case 3 above) -- a
    // plausible-but-wrong translation that reused one order for both would fail exactly one of these.
    // =================================================================================================
    {
        // 8a: dead building, target_ref!=0.
        unit &u                                             = settle_into_combat_recheck(ST_ATTACK_BUILDING, 1, 0x21, 3);
        fx.b(ref_owner((uint32_t)(uint16_t)0x21), 3).energy = -1.0;
        (void)u;
        call(fx);
        ck(trace_eq({"dir_step_factor", "target_release_ref", "unit_set_state_order", "unit_notify_status"}),
           "8a: ATTACK_BUILDING dead -- set_state_order BEFORE notify, opposite order from ATTACK_UNIT's dead branch (0x0047ce7b-0x0047ce9d)");
        ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].new_order == UNIT_STATE_STOP_TO_DEFAULT &&
               g_set_state_order_calls[0].new_state == UNIT_STATE_STOP_TO_DEFAULT,
           "8a: unit_set_state_order(STOP_TO_DEFAULT, STOP_TO_DEFAULT)");

        // 8b: alive, in range.
        unit &u2              = settle_into_combat_recheck(ST_ATTACK_BUILDING, 1, 0x21, 3);
        u2.target_fine_x      = 640; // -> tile 20
        u2.target_fine_y      = 800; // -> tile 25
        fx.b(1, 3).energy     = 5.0;
        g_target_class_ret    = 33;
        g_in_weapon_range_ret = 1;
        call(fx);
        ck(trace_eq({"dir_step_factor", "target_class", "unit_in_weapon_range", "unit_fire_at_target_if_aimed",
                     "unit_set_state_order", "unit_notify_status"}),
           "8b: ATTACK_BUILDING alive+in-range (0x0047cd91-0x0047ce31)");
        ck(g_in_range_calls.size() == 1 && g_in_range_calls[0].tile_x == 20 && g_in_range_calls[0].tile_y == 25,
           "8b: unit_in_weapon_range tile args from target_fine_x/y (0x0047cdae-0x0047cdfd)");
        ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].new_order == ST_ATTACK_BUILDING,
           "8b: unit_set_state_order(u.order=ATTACK_BUILDING, STOP_TO_DEFAULT) (0x0047ce0b-0x0047ce19)");

        // 8c: alive, NOT in range -- nothing further.
        unit &u3              = settle_into_combat_recheck(ST_ATTACK_BUILDING, 1, 0x21, 3);
        fx.b(1, 3).energy     = 5.0;
        g_in_weapon_range_ret = 0;
        call(fx);
        ck(trace_eq({"dir_step_factor", "target_class", "unit_in_weapon_range"}),
           "8c: ATTACK_BUILDING alive+not-in-range -- no fire/notify (0x0047ce04 JZ taken)");
    }

    // =================================================================================================
    // 9 -- settled path-advance: spent-waypoint cursor skip (0x0047cea7-0x0047ceeb), UNADVANCED cursor
    // read for run_length. move_microstep seeded AT 0x1f (already settled -- skips the combat recheck
    // entirely, unlike cases 3-8 which reached 0x1f THIS tick).
    // =================================================================================================
    auto seed_settled = [&](uint16_t order, uint16_t state) -> unit & {
        reset_all(fx);
        unit &u                       = fx.u(0, 1);
        u.unit_proto_id               = 5;
        u.move_microstep              = 0x1f; // already settled -- 0x0047c99f JGE taken immediately
        u.order                       = order;
        u.state                       = state;
        u.path_slot_id                = 7;
        fx.cfg_units[5].step_speed[0] = 1.0;
        u.move_step_speed_scale       = 1.0;
        g_dir_step_factor_ret         = 1.0;
        fx.cur_unit_ptr               = &u;
        fx.view_cur_player            = 0;
        fx.view_cur_index             = 1;
        fx.tick_budget                = 999.0; // arrival path never spends this -- must stay unchanged
        return fx.u(0, 1);
    };
    {
        // 9a: run_length==0 at the cursor -- path_cursor advances by 1, and the heading read afterwards
        // comes from the ADVANCED slot (proven with a decoy heading at the unadvanced slot: if the
        // advance did not happen, the decoy would route into the STEP/TURN branch instead of arrival).
        unit &u             = seed_settled(UNIT_STATE_STOP_TO_DEFAULT, UNIT_STATE_STOP_TO_DEFAULT);
        u.path_cursor       = 2;
        u.x                 = 9;
        u.y                 = 9;
        u.goal_x            = 9;
        u.goal_y            = 9; // at goal -- should_settle true regardless of order/state
        auto &wp_unadv      = fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 7 * PATH_WAYPOINTS_PER_SLOT + 2];
        wp_unadv.run_length = 0;
        wp_unadv.heading    = 13; // decoy -- if the advance didn't happen, this nonzero heading would
                                  // divert into the STEP/TURN branch instead of arrival
        auto &wp_adv      = fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 7 * PATH_WAYPOINTS_PER_SLOT + 3];
        wp_adv.heading    = 0; // terminal sentinel -- reached only if the cursor DID advance
        wp_adv.run_length = 0;
        call(fx);
        ck_eq((uint32_t)fx.u(0, 1).path_cursor, 3u, "9a: run_length==0 at the cursor -- path_cursor advances by 1 (0x0047cede JNZ not taken -> INC 0x0047cee5)");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].status_code == NOTIFY_GOAL_REACHED,
           "9a: arrival reached via the ADVANCED cursor's terminal heading, not diverted by the unadvanced slot's decoy");
        ck_eq_d(fx.tick_budget, 999.0, "9a: the arrival path never touches tick_budget");

        // 9b: run_length!=0 at the cursor -- cursor does NOT advance; a decoy terminal heading at the
        // WOULD-BE-advanced slot proves it, since reaching it would wrongly short-circuit to arrival.
        unit &u2             = seed_settled(UNIT_STATE_STOP_TO_DEFAULT, UNIT_STATE_STOP_TO_DEFAULT);
        u2.path_cursor       = 2;
        auto &wp2_unadv      = fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 7 * PATH_WAYPOINTS_PER_SLOT + 2];
        wp2_unadv.run_length = 5; // != 0 -- no advance
        wp2_unadv.heading    = 0; // terminal -- reached only if the cursor did NOT advance
        auto &wp2_adv        = fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 7 * PATH_WAYPOINTS_PER_SLOT + 3];
        wp2_adv.heading      = 13; // decoy: would divert to STEP/TURN if wrongly read
        wp2_adv.run_length   = 0;
        u2.x                 = 1;
        u2.y                 = 1;
        u2.goal_x            = 9;
        u2.goal_y            = 9; // NOT at goal, but order==STOP_TO_DEFAULT is not a settle trigger either --
                                  // exercise the not-should-settle bailout here too (kills two checks with one case).
        call(fx);
        ck_eq((uint32_t)fx.u(0, 1).path_cursor, 2u, "9b: run_length!=0 -- path_cursor stays put (0x0047cede JNZ taken)");
        ck(trace_eq({"dir_step_factor", "unit_set_state", "unit_notify_status"}),
           "9b: not at goal + non-settle order -- GROUP_MARSHAL bailout confirms the UNADVANCED cursor's terminal heading was read");
    }

    // =================================================================================================
    // 10 -- arrival, should_settle FALSE (not at goal, order not in the 5-way OR chain) -- GROUP_MARSHAL
    // re-plan bailout, NOT the "commit STOP_TO_DEFAULT" path (0x0047cfa0-0x0047cfc2).
    // =================================================================================================
    {
        unit &u                                                                          = seed_settled(0x77 /* arbitrary non-settle order */, 0x77);
        u.path_cursor                                                                    = 0;
        u.x                                                                              = 1;
        u.y                                                                              = 1;
        u.goal_x                                                                         = 9;
        u.goal_y                                                                         = 9;
        fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 7 * PATH_WAYPOINTS_PER_SLOT + 0] = {0, 5}; // heading=0, run_length=5(!=0, no advance)
        call(fx);
        ck(trace_eq({"dir_step_factor", "unit_set_state", "unit_notify_status"}),
           "10: not-should-settle -- GROUP_MARSHAL(0xa) + notify(REPLAN=1), no set_state_order at all (0x0047cfa0-0x0047cfc2)");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == ST_GROUP_MARSHAL, "10: unit_set_state(GROUP_MARSHAL)");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].status_code == NOTIFY_REPLAN, "10: unit_notify_status(..., REPLAN=1)");
        ck(g_set_state_order_calls.empty(), "10: unit_set_state_order NEVER called on the bailout path");
    }

    // =================================================================================================
    // 11 -- arrival, should_settle via AT-GOAL alone (order/state neither ATTACK_* nor PATROL_SWAP nor
    // SQUAD_MERGE) -- commit STOP_TO_DEFAULT, no in-range recheck, no footprint reroll, notify GOAL_REACHED.
    // =================================================================================================
    {
        unit &u                                                                          = seed_settled(0x77, 0x77);
        u.path_cursor                                                                    = 0;
        u.x                                                                              = 5;
        u.y                                                                              = 5;
        u.goal_x                                                                         = 5;
        u.goal_y                                                                         = 5; // at goal
        fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 7 * PATH_WAYPOINTS_PER_SLOT + 0] = {0, 0};
        call(fx);
        ck(trace_eq({"dir_step_factor", "unit_set_state_order", "unit_notify_status"}),
           "11: at-goal settle, non-ATTACK state -- no recheck/footprint calls (0x0047cfda-0x0047d182)");
        ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].new_order == 0x77 &&
               g_set_state_order_calls[0].new_state == UNIT_STATE_STOP_TO_DEFAULT,
           "11: unit_set_state_order(u.order, STOP_TO_DEFAULT) (0x0047cfcc-0x0047cfd5)");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].status_code == NOTIFY_GOAL_REACHED,
           "11: at goal -- notify(GOAL_REACHED=0x65) (0x0047d165-0x0047d178)");
    }

    // =================================================================================================
    // 12 -- arrival, should_settle via order==SQUAD_MERGE (NOT an ATTACK order) with state==
    // ATTACK_BUILDING -- proves should_settle reads `order` while the in-range-recheck/footprint block
    // reads `state` INDEPENDENTLY (0x0047cf92-0x0047cf9e for the settle trigger vs 0x0047cfdf-0x0047d000
    // for the recheck gate) -- a translation that used the SAME field for both would fail this.
    // =================================================================================================
    {
        // 12a: recheck in-range -- no re-issue set_state_order, footprint reroll still fires unconditionally
        // for state==ATTACK_BUILDING.
        unit &u                                                                          = seed_settled(ST_SQUAD_MERGE, ST_ATTACK_BUILDING);
        u.path_cursor                                                                    = 0;
        u.x                                                                              = 1;
        u.y                                                                              = 1;
        u.goal_x                                                                         = 9;
        u.goal_y                                                                         = 9;    // NOT at goal -- settle must come from order==SQUAD_MERGE alone
        u.target_ref                                                                     = 0x22; // owner 2
        u.target_index                                                                   = 4;
        fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 7 * PATH_WAYPOINTS_PER_SLOT + 0] = {0, 0};
        g_bldg_get_coords_fine_x                                                         = 960;  // -> tile 30
        g_bldg_get_coords_fine_y                                                         = 1120; // -> tile 35
        g_target_class_ret                                                               = 44;
        g_in_weapon_range_ret                                                            = 1; // in range
        g_bldg_footprint_fine_x                                                          = 2000;
        g_bldg_footprint_fine_y                                                          = 2100;
        call(fx);
        ck(trace_eq({"dir_step_factor", "unit_set_state_order", "bldg_get_coords", "target_class", "unit_in_weapon_range",
                     "bldg_footprint_random_offset", "unit_notify_status"}),
           "12a: order=SQUAD_MERGE settles, state=ATTACK_BUILDING recheck uses bldg_get_coords, in-range skips re-issue, "
           "footprint reroll always fires for state==ATTACK_BUILDING (0x0047cfc7-0x0047d17d)");
        ck(g_bldg_get_coords_calls.size() == 1 && g_bldg_get_coords_calls[0].building_index == 1,
           "12a: bldg_get_coords(player, unit_index AS building_index, ...) (0x0047d002-0x0047d016)");
        ck(g_in_range_calls.size() == 1 && g_in_range_calls[0].tile_x == 30 && g_in_range_calls[0].tile_y == 35,
           "12a: in-range check uses bldg_get_coords' fine_x/y (0x0047d057-0x0047d08d)");
        ck(g_footprint_calls.size() == 1 && g_footprint_calls[0].target_owner == 2 && g_footprint_calls[0].target_index == 4,
           "12a: bldg_footprint_random_offset(player, unit_idx, target_owner=2, target_index=4, &target_fine_x, &target_fine_y) "
           "(0x0047d0d0-0x0047d114) -- fires REGARDLESS of the in-range outcome");
        ck_eq((uint32_t)fx.u(0, 1).target_fine_x, 2000u, "12a: u.target_fine_x overwritten by the footprint reroll");
        ck_eq((uint32_t)fx.u(0, 1).target_fine_y, 2100u, "12a: u.target_fine_y overwritten by the footprint reroll");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].status_code == NOTIFY_STILL_CHASING,
           "12a: not at goal -- notify(STILL_CHASING) even though the settle came via order, not at_goal");

        // 12b: recheck NOT in-range -- extra set_state_order(move_op_arg, state) inserted BEFORE the
        // (still unconditional) footprint reroll.
        unit &u2                                                                         = seed_settled(ST_SQUAD_MERGE, ST_ATTACK_BUILDING);
        u2.path_cursor                                                                   = 0;
        u2.x                                                                             = 1;
        u2.y                                                                             = 1;
        u2.goal_x                                                                        = 9;
        u2.goal_y                                                                        = 9;
        u2.target_ref                                                                    = 0x22;
        u2.target_index                                                                  = 4;
        u2.unit_proto_id                                                                 = 5;
        fx.cfg_units[5].move_op_arg                                                      = 0xb; // air-mover sentinel, distinct from any state/order literal here
        fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 7 * PATH_WAYPOINTS_PER_SLOT + 0] = {0, 0};
        g_in_weapon_range_ret                                                            = 0; // not in range
        call(fx);
        ck(trace_eq({"dir_step_factor", "unit_set_state_order", "bldg_get_coords", "target_class", "unit_in_weapon_range",
                     "unit_set_state_order", "bldg_footprint_random_offset", "unit_notify_status"}),
           "12b: not-in-range -- a SECOND unit_set_state_order fires before the footprint reroll (0x0047d096-0x0047d0bf)");
        ck(g_set_state_order_calls.size() == 2 && g_set_state_order_calls[1].new_order == 0xb &&
               g_set_state_order_calls[1].new_state == ST_ATTACK_BUILDING,
           "12b: the second call is unit_set_state_order(cfg_units[proto].move_op_arg, u.state) (0x0047d0ae-0x0047d0bf)");
    }

    // =================================================================================================
    // 13 -- arrival, should_settle via order==PATROL_SWAP with state==ATTACK_UNIT: the OTHER recheck
    // sub-path (no bldg_get_coords, no footprint reroll -- uses target_fine_x/y directly).
    // =================================================================================================
    {
        unit &u                                                                          = seed_settled(UNIT_STATE_PATROL_SWAP, ST_ATTACK_UNIT);
        u.path_cursor                                                                    = 0;
        u.x                                                                              = 1;
        u.y                                                                              = 1;
        u.goal_x                                                                         = 9;
        u.goal_y                                                                         = 9;
        u.target_fine_x                                                                  = 480; // -> tile 15
        u.target_fine_y                                                                  = 640; // -> tile 20
        fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 7 * PATH_WAYPOINTS_PER_SLOT + 0] = {0, 0};
        g_target_class_ret                                                               = 12;
        g_in_weapon_range_ret                                                            = 1;
        call(fx);
        ck(trace_eq({"dir_step_factor", "unit_set_state_order", "target_class", "unit_in_weapon_range", "unit_notify_status"}),
           "13: state==ATTACK_UNIT -- no bldg_get_coords, no footprint reroll (0x0047cfdf JZ not taken, 0x0047d0c9 JNZ taken)");
        ck(g_in_range_calls.size() == 1 && g_in_range_calls[0].tile_x == 15 && g_in_range_calls[0].tile_y == 20,
           "13: in-range check uses u.target_fine_x/y directly, unlike the ATTACK_BUILDING sub-path (0x0047d01d-0x0047d036)");
        ck(g_bldg_get_coords_calls.empty() && g_footprint_calls.empty(), "13: neither bldg_get_coords nor the footprint reroll fires for state==ATTACK_UNIT");
    }

    // =================================================================================================
    // 14 -- STEP path (heading!=0, heading==facing_target), budget insufficient -- carryover, NO tile
    // writes at all. facing24_to_delta fires unconditionally before the budget check (0x0047d1dd, ahead
    // of the 0x0047d249 budget compare).
    // =================================================================================================
    auto seed_step_or_turn = [&](uint8_t heading, uint8_t facing_target) -> unit & {
        reset_all(fx);
        unit &u                                                                          = fx.u(0, 1);
        u.unit_proto_id                                                                  = 5;
        u.move_microstep                                                                 = 0x1f;
        u.order                                                                          = 0x77;
        u.state                                                                          = 0x77;
        u.path_slot_id                                                                   = 7;
        u.path_cursor                                                                    = 0;
        u.x                                                                              = 10;
        u.y                                                                              = 10;
        u.facing_target                                                                  = facing_target;
        fx.cfg_units[5].step_speed[0]                                                    = 1.0;
        u.move_step_speed_scale                                                          = 1.0;
        g_dir_step_factor_ret                                                            = 1.0; // step_cost = 1.0
        fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 7 * PATH_WAYPOINTS_PER_SLOT + 0] = {heading, 9};
        fx.cur_unit_ptr                                                                  = &u;
        fx.view_cur_player                                                               = 0;
        fx.view_cur_index                                                                = 1;
        return fx.u(0, 1);
    };
    {
        unit &u          = seed_step_or_turn(/*heading=*/5, /*facing_target=*/5); // heading==facing_target -> STEP
        g_facing24_dx    = 1;
        g_facing24_dy    = 0;
        fx.tick_budget   = 0.5; // < step_cost(1.0)
        u.activity_clock = 20.0;
        call(fx);
        ck(trace_eq({"dir_step_factor", "facing24_to_delta"}), "14: STEP budget-insufficient touches no tile/roster callee");
        ck_eq_d(fx.u(0, 1).activity_clock, 19.5, "14: activity_clock -= tick_budget = 20-0.5 (0x0047d262-0x0047d265)");
        ck_eq_d(fx.tick_budget, 0.0, "14: tick_budget zeroed (0x0047d268-0x0047d276)");
        ck_eq((uint32_t)fx.u(0, 1).x, 10u, "14: x untouched on the insufficient-budget STEP path");
    }

    // =================================================================================================
    // 15 -- STEP path, budget sufficient, destination BLOCKED -- detour request + retry escalation,
    // including the same-tick escalation subtlety (a first-ever detour that sets retry_count=0xc9 is
    // IMMEDIATELY read back as >=0xc9 and bails out in the SAME tick, 0x0047d511-0x0047d531).
    // =================================================================================================
    {
        // 15a: destination passable==0 -- blocked via the FIRST OR-term; detour requested -> retry_count
        // set to 0xc9, then read back as old_retry_count==0xc9 (not <0xc9) -> IMMEDIATE bailout.
        unit &u                       = seed_step_or_turn(5, 5);
        g_facing24_dx                 = 1;
        g_facing24_dy                 = 0; // new_x=11, new_y=10
        fx.tick_budget                = 10.0;
        fx.passable[(11u << 8) | 10u] = 0; // blocked
        u.path_blocked_retry_count    = 3; // pre-existing count, irrelevant once detour sets 0xc9
        g_detour_ret                  = 1; // detour requested
        call(fx);
        ck(trace_eq({"dir_step_factor", "facing24_to_delta", "path_step_check_and_request_detour", "unit_set_state",
                     "unit_notify_status"}),
           "15a: blocked + detour_requested!=0 -- SAME-TICK GROUP_MARSHAL bailout (0x0047d50f-0x0047d55c)");
        ck(g_detour_calls.size() == 1 && g_detour_calls[0].src_x == 10 && g_detour_calls[0].src_y == 10 &&
               g_detour_calls[0].dst_x == 11 && g_detour_calls[0].dst_y == 10,
           "15a: path_step_check_and_request_detour(old_x, old_y, new_x, new_y) (0x0047d4fc-0x0047d508)");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == ST_GROUP_MARSHAL, "15a: unit_set_state(GROUP_MARSHAL)");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].status_code == NOTIFY_REPLAN, "15a: unit_notify_status(..., REPLAN=1)");
        ck_eq((uint32_t)fx.u(0, 1).path_blocked_retry_count, 0u,
              "15a: path_blocked_retry_count reset to 0 on the bailout (0x0047d538-0x0047d53f), NOT left at 0xc9");
        ck_eq_d(fx.tick_budget, 0.0, "15a: tick_budget zeroed even on the bailout (0x0047d574-0x0047d582)");

        // 15b: destination tile_objects.building!=0 (passable!=0) -- blocked via the SECOND OR-term;
        // no detour requested; retry_count well under threshold -> activity_clock += 0.05, retry_count+=1.
        unit &u2                      = seed_step_or_turn(5, 5);
        g_facing24_dx                 = 1;
        g_facing24_dy                 = 0;
        fx.tick_budget                = 10.0;
        fx.passable[(11u << 8) | 10u] = 4;  // passable
        fx.t(11, 10).building         = 77; // but occupied by a building
        u2.path_blocked_retry_count   = 5;
        u2.activity_clock             = 1.0;
        g_detour_ret                  = 0; // no detour this time
        call(fx);
        ck(trace_eq({"dir_step_factor", "facing24_to_delta", "path_step_check_and_request_detour"}),
           "15b: blocked via tile_objects.building!=0, under retry threshold -- no state-machine call (0x0047d531 JBE taken)");
        ck_eq_d(fx.u(0, 1).activity_clock, 1.05, "15b: activity_clock += DAT_005013c8(0.05) (0x0047d568-0x0047d571)");
        ck_eq((uint32_t)fx.u(0, 1).path_blocked_retry_count, 6u, "15b: path_blocked_retry_count += 1 (0x0047d528)");
        ck_eq_d(fx.tick_budget, 0.0, "15b: tick_budget zeroed (0x0047d574-0x0047d582)");

        // 15c: blocked, no detour, retry_count AT the boundary (0xc8, the last value still under
        // threshold) -- takes the activity_clock bump, becomes 0xc9.
        unit &u3                      = seed_step_or_turn(5, 5);
        g_facing24_dx                 = 1;
        g_facing24_dy                 = 0;
        fx.tick_budget                = 10.0;
        fx.passable[(11u << 8) | 10u] = 0;
        u3.path_blocked_retry_count   = 0xc8;
        g_detour_ret                  = 0;
        call(fx);
        ck(trace_eq({"dir_step_factor", "facing24_to_delta", "path_step_check_and_request_detour"}),
           "15c: old_retry_count==0xc8 -- still the wait arm (0x0047d52e CMP DL,0xc8/0x0047d531 JBE taken)");
        ck_eq((uint32_t)fx.u(0, 1).path_blocked_retry_count, 0xc9u, "15c: retry_count becomes exactly 0xc9 (INC before the compare read DL)");

        // 15d: blocked, no detour, retry_count AT 0xc9 already (accumulated over prior ticks, not set
        // by this tick's detour) -- bails out, proving the boundary is old_retry_count<0xc9, not <=.
        unit &u4                      = seed_step_or_turn(5, 5);
        g_facing24_dx                 = 1;
        g_facing24_dy                 = 0;
        fx.tick_budget                = 10.0;
        fx.passable[(11u << 8) | 10u] = 0;
        u4.path_blocked_retry_count   = 0xc9;
        g_detour_ret                  = 0;
        call(fx);
        ck(trace_eq({"dir_step_factor", "facing24_to_delta", "path_step_check_and_request_detour", "unit_set_state",
                     "unit_notify_status"}),
           "15d: old_retry_count==0xc9 (accumulated, not this-tick-set) -- bailout too (0x0047d52e CMP/0x0047d531 JBE NOT taken)");
    }

    // =================================================================================================
    // 16 -- STEP path, budget sufficient, destination CLEAR -- the full roster write set. This is the
    // roster-writer's headline case: every field the STEP arm touches, asserted individually, plus a
    // neighbouring guard tile/soldier/path-slot proven untouched.
    // =================================================================================================
    {
        unit &u                                                                          = seed_step_or_turn(5, 5); // heading==facing_target -> STEP
        g_facing24_dx                                                                    = 1;
        g_facing24_dy                                                                    = 0; // old=(10,10) -> new=(11,10)
        fx.tick_budget                                                                   = 10.0;
        u.origin_tile_was_passable                                                       = 3;    // OLD snapshot -- restored onto passable[old] on vacate
        u.move_step_speed_scale                                                          = 2.0;  // pre-step scale, combined with the new tile's passable value
        u.move_microstep                                                                 = 0x1f; // sentinel -- must become 0
        u.path_blocked_retry_count                                                       = 7;    // sentinel -- must become 0
        u.unit_above[0]                                                                  = 12;   // soldier index (little-endian word, low byte)
        u.unit_above[1]                                                                  = 0;
        fx.cfg_units[5].sight                                                            = 4;
        fx.cfg_units[5].soldier_count                                                    = 2;    // >0 -- soldier-nudge gate open
        fx.passable[(10u << 8) | 10u]                                                    = 55;   // OLD tile's passable slot before restore (must become origin_tile_was_passable=3)
        fx.passable[(11u << 8) | 10u]                                                    = 9;    // NEW tile's pre-step passable value (feeds the averaging filter AND the snapshot)
        fx.t(10, 10).class_owner                                                         = 0x81; // OLD tile object -- must be vacated (class_owner=0, building=0)
        fx.t(10, 10).building                                                            = 55;
        fx.t(11, 10).class_owner                                                         = 0; // NEW tile object -- must be claimed
        fx.t(11, 10).building                                                            = 0;
        fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 7 * PATH_WAYPOINTS_PER_SLOT + 0] = {5, 6}; // run_length=6 -> must become 5
        soldier &head                                                                    = fx.soldiers[0 * SOLDIERS_PER_PLAYER + 12];
        head.idle_wander_flag                                                            = 9; // nonzero -- gate open, must be cleared to 0

        // Neighbouring/guard state that this call must NOT touch.
        fx.t(50, 50).class_owner                                                                    = 0x22;
        fx.t(50, 50).building                                                                       = 66;
        fx.passable[(50u << 8) | 50u]                                                               = 44;
        fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 7 * PATH_WAYPOINTS_PER_SLOT + 1].run_length = 88; // adjacent cursor slot
        soldier &guard_soldier                                                                      = fx.soldiers[0 * SOLDIERS_PER_PLAYER + 13];
        guard_soldier.idle_wander_flag                                                              = 5;

        call(fx);

        ck(trace_eq({"dir_step_factor", "facing24_to_delta", "fow_remove_sight", "map_fow_UpdateFoWPlus",
                     "unit_soldiers_set_heading"}),
           "16: full STEP call order (0x0047d1dd facing24_to_delta, 0x0047d329 fow_remove_sight, 0x0047d463 "
           "map_fow_UpdateFoWPlus, 0x0047d4d8 unit_soldiers_set_heading) -- no path_step_check_and_request_detour "
           "since the destination is clear");
        // step_cost here = step_speed[0](1.0) * move_step_speed_scale(2.0, overridden above) *
        // dir_step_factor_ret(1.0) = 2.0 -- NOT the seed_step_or_turn baseline of 1.0.
        ck_eq_d(fx.tick_budget, 8.0, "16: tick_budget -= step_cost = 10-2 (0x0047d2b0-0x0047d2b9)");

        ck_eq((uint32_t)fx.t(10, 10).class_owner, 0u, "16: OLD tile_object.class_owner cleared (0x0047d2cd-0x0047d2d4, BEFORE .building)");
        ck_eq((uint32_t)fx.t(10, 10).building, 0u, "16: OLD tile_object.building cleared (0x0047d2d4-0x0047d2eb)");
        ck_eq((uint32_t)fx.passable[(10u << 8) | 10u], 3u, "16: OLD passable restored to origin_tile_was_passable=3 (0x0047d2eb-0x0047d300)");

        ck(g_fow_remove_calls.size() == 1 && g_fow_remove_calls[0].x == 10 && g_fow_remove_calls[0].y == 10 &&
               g_fow_remove_calls[0].radius == 4,
           "16: fow_remove_sight(player, old_x=10, old_y=10, cfg.sight=4) (0x0047d306-0x0047d329)");

        ck_eq((uint32_t)fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 7 * PATH_WAYPOINTS_PER_SLOT + 0].run_length, 5u,
              "16: path_buffer_at(player, path_slot_id, path_cursor).run_length -= 1 (6->5) (0x0047d35e)");

        ck_eq((uint32_t)fx.t(11, 10).building, 1u, "16: NEW tile_object.building = unit_index=1 (0x0047d372-0x0047d379)");
        ck_eq((uint32_t)fx.t(11, 10).class_owner, 0x80u, "16: NEW tile_object.class_owner = player(0)|0x80 (0x0047d380-0x0047d397)");

        ck_eq((uint32_t)fx.u(0, 1).origin_tile_was_passable, 9u,
              "16: origin_tile_was_passable snapshot = passable[new] BEFORE it is overwritten to 5 (0x0047d3a6-0x0047d3b1)");
        ck_eq_d(fx.u(0, 1).move_step_speed_scale, (9.0 + 2.0) * 0.5,
                "16: move_step_speed_scale = (passable[new] + OLD scale) * 0.5, EXACT order per the asm (0x0047d3d1-0x0047d3f1)");
        ck_eq((uint32_t)fx.passable[(11u << 8) | 10u], 5u, "16: passable[new] set to the moving-unit sentinel 5 (0x0047d400-0x0047d407)");

        ck_eq((uint32_t)fx.u(0, 1).x, 11u, "16: u.x = new_x (0x0047d40d-0x0047d416)");
        ck_eq((uint32_t)fx.u(0, 1).y, 10u, "16: u.y = new_y (0x0047d416-0x0047d425)");
        ck_eq((uint32_t)fx.u(0, 1).move_microstep, 0u, "16: move_microstep reset to 0 (0x0047d425-0x0047d434)");
        ck_eq((uint32_t)fx.u(0, 1).path_blocked_retry_count, 0u, "16: path_blocked_retry_count reset to 0 (0x0047d434-0x0047d440)");

        ck(g_fow_update_calls.size() == 1 && g_fow_update_calls[0].x == 11 && g_fow_update_calls[0].y == 10 &&
               g_fow_update_calls[0].sight == 4,
           "16: map_fow_UpdateFoWPlus(player, new_x=11, new_y=10, cfg.sight=4) (0x0047d445-0x0047d463)");

        ck(g_soldier_heading_calls.size() == 1 && g_soldier_heading_calls[0].player == 0 &&
               g_soldier_heading_calls[0].unit_index == 1 && g_soldier_heading_calls[0].sprite_frame == 5,
           "16: unit_soldiers_set_heading(player, unit_index, facing_target=5) (0x0047d4c1-0x0047d4d8)");
        ck_eq((uint32_t)fx.soldiers[0 * SOLDIERS_PER_PLAYER + 12].idle_wander_flag, 0u,
              "16: head soldier's idle_wander_flag cleared (0x0047d4ee-0x0047d4f7)");

        // Neighbour non-corruption.
        ck_eq((uint32_t)fx.t(50, 50).class_owner, 0x22u, "16: guard tile's class_owner untouched");
        ck_eq((uint32_t)fx.t(50, 50).building, 66u, "16: guard tile's building untouched");
        ck_eq((uint32_t)fx.passable[(50u << 8) | 50u], 44u, "16: guard tile's passable untouched");
        ck_eq((uint32_t)fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 7 * PATH_WAYPOINTS_PER_SLOT + 1].run_length, 88u,
              "16: adjacent path-buffer slot untouched (array-index precision)");
        ck_eq((uint32_t)fx.soldiers[0 * SOLDIERS_PER_PLAYER + 13].idle_wander_flag, 5u, "16: guard soldier's idle_wander_flag untouched");
    }

    // =================================================================================================
    // 16b/16c -- the soldier idle-wander clear's AND gate, both ways to fail it (soldier_count<=0, and
    // soldier_count>0 but idle_wander_flag==0). Reuses the same clear-STEP shape as case 16 but only
    // asserts the gate's own outcome to keep this short.
    // =================================================================================================
    {
        unit &u                                                    = seed_step_or_turn(5, 5);
        g_facing24_dx                                              = 1;
        g_facing24_dy                                              = 0;
        fx.tick_budget                                             = 10.0;
        fx.passable[(11u << 8) | 10u]                              = 9;
        fx.cfg_units[5].soldier_count                              = 0; // gate closed via soldier_count
        u.unit_above[0]                                            = 20;
        fx.soldiers[0 * SOLDIERS_PER_PLAYER + 20].idle_wander_flag = 9; // would be nonzero if read
        call(fx);
        ck(trace_eq({"dir_step_factor", "facing24_to_delta", "fow_remove_sight", "map_fow_UpdateFoWPlus"}),
           "16b: soldier_count<=0 -- unit_soldiers_set_heading NOT called (0x0047d491 JLE taken)");
        ck_eq((uint32_t)fx.soldiers[0 * SOLDIERS_PER_PLAYER + 20].idle_wander_flag, 9u, "16b: idle_wander_flag left untouched when the gate never opens");

        unit &u2                                                   = seed_step_or_turn(5, 5);
        g_facing24_dx                                              = 1;
        g_facing24_dy                                              = 0;
        fx.tick_budget                                             = 10.0;
        fx.passable[(11u << 8) | 10u]                              = 9;
        fx.cfg_units[5].soldier_count                              = 2; // count open...
        u2.unit_above[0]                                           = 21;
        fx.soldiers[0 * SOLDIERS_PER_PLAYER + 21].idle_wander_flag = 0; // ...but flag closed
        call(fx);
        ck(trace_eq({"dir_step_factor", "facing24_to_delta", "fow_remove_sight", "map_fow_UpdateFoWPlus"}),
           "16c: soldier_count>0 but idle_wander_flag==0 -- unit_soldiers_set_heading NOT called (0x0047d4bf JZ taken)");
    }

    // =================================================================================================
    // 17 -- TURN path (heading!=0, heading!=facing_target).
    // =================================================================================================
    {
        // 17a: budget insufficient -- carryover, no facing change at all.
        unit &u                       = seed_step_or_turn(/*heading=*/10, /*facing_target=*/20); // heading != facing_target -> TURN
        fx.cfg_units[5].turn_speed[0] = 2.0;
        u.move_step_speed_scale       = 3.0; // turn_cost = 2*3 = 6.0
        fx.tick_budget                = 1.0; // < 6.0
        u.activity_clock              = 50.0;
        call(fx);
        ck(trace_eq({"dir_step_factor", "facing24_to_delta"}), "17a: TURN budget-insufficient touches no facing/soldier callee");
        ck_eq_d(fx.u(0, 1).activity_clock, 49.0, "17a: activity_clock -= tick_budget = 50-1 (0x0047d5d6-0x0047d5d9)");
        ck_eq_d(fx.tick_budget, 0.0, "17a: tick_budget zeroed (0x0047d5dc-0x0047d5ea)");
        ck_eq((uint32_t)fx.u(0, 1).facing_target, 20u, "17a: facing_target untouched when the turn budget is insufficient");

        // 17b: budget sufficient, INCREMENT direction (diff>12 AND facing_target>path_heading), WITH the
        // mod-24 wrap on both facing_target and facing_current (24+1 -> 1).
        unit &u2                      = seed_step_or_turn(/*heading=*/9, /*facing_target=*/24); // diff=24-9=15>12, 24>9 -> increment
        fx.cfg_units[5].turn_speed[0] = 1.0;
        u2.move_step_speed_scale      = 1.0; // turn_cost = 1.0
        fx.tick_budget                = 10.0;
        u2.facing_current             = 24; // also wraps
        fx.cfg_units[5].soldier_count = 1;
        g_walk_step_allowed_ret       = 1; // low byte nonzero -- facing_current commits
        call(fx);
        ck(trace_eq({"dir_step_factor", "facing24_to_delta", "unit_soldiers_set_heading", "unit_walk_step_allowed"}),
           "17b: TURN increment call order (0x0047d6c0-0x0047d737)");
        ck_eq_d(fx.tick_budget, 9.0, "17b: tick_budget -= turn_cost (0x0047d5f5-0x0047d5fe)");
        ck_eq((uint32_t)fx.u(0, 1).facing_target, 1u, "17b: facing_target = 24+1, wraps to 1 (0x0047d67e-0x0047d68e)");
        ck_eq((uint32_t)fx.u(0, 1).facing_current, 1u, "17b: facing_current = 24+1, wraps to 1 (0x0047d68e-0x0047d69e), COMMITTED (walk_step_allowed!=0)");
        ck(g_soldier_heading_calls.size() == 1 && g_soldier_heading_calls[0].sprite_frame == 1,
           "17b: unit_soldiers_set_heading uses the NEW (post-wrap) facing_target=1 (0x0047d6fe-0x0047d70f)");

        // 17c: DECREMENT via the large-diff boundary (diff<=-12), WITH the mod-24 wrap-under (1-1 -> 24).
        unit &u3                      = seed_step_or_turn(/*heading=*/13, /*facing_target=*/1); // diff=1-13=-12<=-12 -> decrement
        fx.cfg_units[5].turn_speed[0] = 1.0;
        u3.move_step_speed_scale      = 1.0;
        fx.tick_budget                = 10.0;
        u3.facing_current             = 1; // also wraps under
        fx.cfg_units[5].soldier_count = 0; // gate closed this time
        g_walk_step_allowed_ret       = 1;
        call(fx);
        ck(trace_eq({"dir_step_factor", "facing24_to_delta", "unit_walk_step_allowed"}),
           "17c: soldier_count<=0 -- unit_soldiers_set_heading skipped even on a real rotation (0x0047d6fc JLE taken)");
        ck_eq((uint32_t)fx.u(0, 1).facing_target, 24u, "17c: facing_target = 1-1, wraps to 24 (0x0047d6a0-0x0047d6ac)");
        ck_eq((uint32_t)fx.u(0, 1).facing_current, 24u, "17c: facing_current = 1-1, wraps to 24 (0x0047d6b0-0x0047d6bc)");

        // 17d: DECREMENT via the "small diff, facing_target >= path_heading" fallthrough (the shortest-
        // arc case that does NOT go all the way around) -- diff=5, no wrap.
        unit &u4                      = seed_step_or_turn(/*heading=*/5, /*facing_target=*/10); // diff=10-5=5 (not >12, not <=-12, facing_target>=path_heading)
        fx.cfg_units[5].turn_speed[0] = 1.0;
        u4.move_step_speed_scale      = 1.0;
        fx.tick_budget                = 10.0;
        u4.facing_current             = 10;
        g_walk_step_allowed_ret       = 0; // facing_current must NOT commit
        call(fx);
        ck_eq((uint32_t)fx.u(0, 1).facing_target, 9u, "17d: small-diff shortest-arc decrement: facing_target=10-1=9 (0x0047d669-0x0047d6a0)");
        ck_eq((uint32_t)fx.u(0, 1).facing_current, 10u, "17d: walk_step_allowed()==0 -- facing_current NOT committed, stays 10 (0x0047d729 JZ taken)");

        // 17e: the walk_step_allowed BYTE-truncation nuance -- TEST AL,AL, not a full-int32 test. A
        // return value whose low byte is 0 but whose full int32 is nonzero (0x100) must still be treated
        // as "not allowed" (facing_current does NOT commit), even though facing_target always does.
        unit &u5                      = seed_step_or_turn(/*heading=*/5, /*facing_target=*/10);
        fx.cfg_units[5].turn_speed[0] = 1.0;
        u5.move_step_speed_scale      = 1.0;
        fx.tick_budget                = 10.0;
        u5.facing_current             = 10;
        g_walk_step_allowed_ret       = 0x100; // low byte 0, full int32 nonzero
        call(fx);
        ck_eq((uint32_t)fx.u(0, 1).facing_target, 9u, "17e: facing_target ALWAYS commits regardless of walk_step_allowed (0x0047d6c6-0x0047d6c9)");
        ck_eq((uint32_t)fx.u(0, 1).facing_current, 10u,
              "17e: walk_step_allowed()==0x100 -- LOW BYTE is 0 (TEST AL,AL @0x0047d727-0x0047d729), so facing_current "
              "must NOT commit even though the full int32 return is nonzero -- a translation testing the whole int32 "
              "would wrongly commit here");
    }
}

} // namespace mh::sim::test
