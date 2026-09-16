#include "sim/sim_unit_state_enter.h"

#include "sim/sim_order_enqueue.h" // UNIT_STATE_PARKED, BUILDING_TYPE_A_GARAGE/_A_SHUTTLE/_H_GARAGE

#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: proves CALL ORDER / branch identity (TURN vs STEP calls disjoint callees) -------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }
bool                      trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- return-value knobs -------------------------------------------------------------------------
int32_t g_dir_from_to_ret       = 0;
int32_t g_walk_step_allowed_ret = 0;
double  g_dir_step_factor_ret   = 1.0;

// ---- per-callee recorders -------------------------------------------------------------------------
struct DirFromToCall {
    int32_t x1, y1, x2, y2;
};
std::vector<DirFromToCall> g_dir_from_to_calls;
int32_t                    rec_dir_from_to(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    tr("dir_from_to");
    g_dir_from_to_calls.push_back({x1, y1, x2, y2});
    return g_dir_from_to_ret;
}

std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) {
    tr("unit_set_state");
    g_set_state_calls.push_back(new_state);
}

struct WalkAllowedCall {
    uint32_t player;
    int32_t  unit_index;
};
std::vector<WalkAllowedCall> g_walk_allowed_calls;
int32_t                      rec_unit_walk_step_allowed(uint32_t player, int32_t unit_index) {
    tr("unit_walk_step_allowed");
    g_walk_allowed_calls.push_back({player, unit_index});
    return g_walk_step_allowed_ret;
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

std::vector<int32_t> g_dir_step_factor_args;
double               rec_dir_step_factor(int32_t dir) {
    tr("dir_step_factor");
    g_dir_step_factor_args.push_back(dir);
    return g_dir_step_factor_ret;
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

struct PathFreeCall {
    uint16_t player;
    int32_t  unit_index;
};
std::vector<PathFreeCall> g_path_free_calls;
void                      rec_path_free_slot(uint16_t player, int32_t unit_index) {
    tr("path_free_slot");
    g_path_free_calls.push_back({player, unit_index});
}

const unit_state_enter_calls g_calls = {
    &rec_unit_set_state,
    nullptr, // storage_can_enter -- unreachable from this function
    nullptr, // storage_board_unit
    nullptr, // unit_queue_advance
    &rec_dir_from_to,
    &rec_unit_walk_step_allowed,
    &rec_unit_soldiers_set_heading,
    &rec_dir_step_factor,
    &rec_target_release_ref,
    &rec_path_free_slot,
};

constexpr uint16_t PLAYER     = 1;
constexpr int32_t  UNIT_INDEX = 6;
constexpr uint8_t  SLOT       = 3;
constexpr int32_t  B_INDEX    = 8;
constexpr uint16_t PROTO      = 12;

// Fixed storage/park tile pair shared by every case -- dir_from_to is mocked, so its own geometry is
// not under test; only that THIS function passes it exit_tile<<5 / park<<5 unchanged.
constexpr int32_t EXIT_TILE_X = 10, EXIT_TILE_Y = 20;
constexpr uint8_t PARK_X = 3, PARK_Y = 4;

void reset_recorders() {
    g_trace.clear();
    g_dir_from_to_calls.clear();
    g_set_state_calls.clear();
    g_walk_allowed_calls.clear();
    g_soldier_heading_calls.clear();
    g_dir_step_factor_args.clear();
    g_release_calls.clear();
    g_path_free_calls.clear();
    g_dir_from_to_ret       = 0;
    g_walk_step_allowed_ret = 0;
    g_dir_step_factor_ret   = 1.0;
}

// Common per-case seed: fresh fixture, storage/park tiles fixed, home building bound (B_INDEX, cfg
// type defaults to 0 = the "default two-group" arm unless a case overrides it).
unit &seed(sim_fixture &fx, uint16_t player, int32_t index, uint8_t slot) {
    fx.reset();
    reset_recorders();
    unit &u             = fx.u(player, index);
    u.unit_proto_id     = PROTO;
    u.home_storage_slot = slot;
    fx.cur_unit_ptr     = &u;
    fx.view_cur_player  = player;
    fx.view_cur_index   = (uint16_t)index;

    unit_storage &st = fx.storage[(size_t)player * STORAGE_PER_PLAYER + slot];
    st.b_index       = B_INDEX;
    st.exit_tile_x   = EXIT_TILE_X;
    st.exit_tile_y   = EXIT_TILE_Y;
    st.park_x        = PARK_X;
    st.park_y        = PARK_Y;

    building &b   = fx.b(player, B_INDEX);
    b.building_id = B_INDEX;

    return u;
}

void call(sim_fixture &fx) {
    sim_store own = fx.store();
    detail::unit_state_enter_walk_in(fx.view(), own, g_calls);
}

} // namespace

void run_unit_state_enter_walk_in_tests() {
    sim_fixture fx;

    // =================================================================================================
    // 1 -- dir_from_to is called UNCONDITIONALLY with the shifted exit/park tile pair, regardless of
    // which branch follows (0x004803d7-0x0048045f). Checked once here; every later case's dir_from_to
    // call args are the same by construction (not re-asserted per case).
    // =================================================================================================
    {
        unit &u                                = seed(fx, PLAYER, UNIT_INDEX, SLOT);
        u.facing_target                        = 7;
        g_dir_from_to_ret                      = 7; // facing already matches -- reaches STEP, but insufficient budget so it stops there
        fx.cfg_units[PROTO].step_speed[PLAYER] = 1.0;
        g_dir_step_factor_ret                  = 1.0;
        fx.tick_budget                         = 0.0;
        call(fx);
        (void)u;
        ck(g_dir_from_to_calls.size() == 1 &&
               g_dir_from_to_calls[0].x1 == (EXIT_TILE_X << 5) && g_dir_from_to_calls[0].y1 == (EXIT_TILE_Y << 5) &&
               g_dir_from_to_calls[0].x2 == (PARK_X << 5) && g_dir_from_to_calls[0].y2 == (PARK_Y << 5),
           "1: dir_from_to(exit_tile_x<<5, exit_tile_y<<5, park_x<<5, park_y<<5) (0x004803d7-0x0048045f)");
    }

    // =================================================================================================
    // 2 -- TURN, insufficient budget (0x00480490 JAE not taken): activity_clock -= tick_budget,
    // tick_budget zeroed, RETURN -- no facing write, no walk_step_allowed/soldier callee.
    // =================================================================================================
    {
        unit &u                                = seed(fx, PLAYER, UNIT_INDEX, SLOT);
        g_dir_from_to_ret                      = 5;
        u.facing_target                        = 20; // != 5 -- TURN branch
        u.facing_current                       = 18;
        u.activity_clock                       = 50.0;
        fx.cfg_units[PROTO].turn_speed[PLAYER] = 8.0;
        fx.tick_budget                         = 3.0; // < 8.0
        call(fx);
        ck(trace_eq({"dir_from_to"}), "2: TURN insufficient budget touches no facing/soldier callee");
        ck_eq_d(fx.u(PLAYER, UNIT_INDEX).activity_clock, 47.0,
                "2: activity_clock -= tick_budget (50-3), 0x004804a0-0x004804ac");
        ck_eq_d(fx.tick_budget, 0.0, "2: tick_budget zeroed, 0x004804b1-0x004804b9");
        ck_eq((uint32_t)fx.u(PLAYER, UNIT_INDEX).facing_target, 20u, "2: facing_target untouched");
        ck_eq((uint32_t)fx.u(PLAYER, UNIT_INDEX).facing_current, 18u, "2: facing_current untouched");
    }

    // =================================================================================================
    // 3 -- TURN, sufficient budget, INCREMENT via block 1 (diff_a>12 && facing_target>facing_needed),
    // WITH wrap on both facing_target and facing_current (24+1 -> 1), soldier_count==0 (no heading
    // call), walk_step_allowed!=0 (facing_current commits).
    // =================================================================================================
    {
        unit &u                                = seed(fx, PLAYER, UNIT_INDEX, SLOT);
        g_dir_from_to_ret                      = 5;
        u.facing_target                        = 24; // diff_a = 24-5=19>12, 24>5 -> increment
        u.facing_current                       = 24;
        fx.cfg_units[PROTO].turn_speed[PLAYER] = 4.0;
        fx.cfg_units[PROTO].soldier_count      = 0;
        fx.tick_budget                         = 10.0;
        g_walk_step_allowed_ret                = 1;
        call(fx);
        ck(trace_eq({"dir_from_to", "unit_walk_step_allowed"}),
           "3: TURN increment, soldier_count==0 -- no soldier-heading call (0x0048050b/0x00480516)");
        ck_eq_d(fx.tick_budget, 6.0, "3: tick_budget -= turn_cost (10-4), 0x004804bf-0x004804c8");
        ck_eq((uint32_t)fx.u(PLAYER, UNIT_INDEX).facing_target, 1u,
              "3: facing_target = 24+1, wraps to 1 (0x0048052d-0x00480539)");
        ck_eq((uint32_t)fx.u(PLAYER, UNIT_INDEX).facing_current, 1u,
              "3: facing_current = 24+1, wraps to 1, COMMITTED (walk_step_allowed!=0), 0x0048053d-0x0048059b");
        ck(g_walk_allowed_calls.size() == 1 && g_walk_allowed_calls[0].player == PLAYER &&
               g_walk_allowed_calls[0].unit_index == UNIT_INDEX,
           "3: unit_walk_step_allowed(player, index) (0x0048057b-0x00480590)");
    }

    // =================================================================================================
    // 4 -- TURN, sufficient budget, DECREMENT via block 2's diff_b<=-12, WITH wrap (1-1 -> 24 on
    // facing_target), soldier_count>0 (heading call fires, using the NEW post-commit facing_target),
    // walk_step_allowed==0 (facing_current does NOT commit).
    // =================================================================================================
    {
        unit &u                                = seed(fx, PLAYER, UNIT_INDEX, SLOT);
        g_dir_from_to_ret                      = 20;
        u.facing_target                        = 1; // diff_a=1-20=-19, not>12 -> block2: diff_b=-19<=-12 -> decrement
        u.facing_current                       = 1;
        fx.cfg_units[PROTO].turn_speed[PLAYER] = 2.0;
        fx.cfg_units[PROTO].soldier_count      = 3;
        fx.tick_budget                         = 5.0;
        g_walk_step_allowed_ret                = 0;
        call(fx);
        ck(trace_eq({"dir_from_to", "unit_walk_step_allowed", "unit_soldiers_set_heading"}),
           "4: TURN decrement, soldier_count>0 -- heading call AFTER walk_step_allowed (0x0048059e-0x004805e1)");
        ck_eq((uint32_t)fx.u(PLAYER, UNIT_INDEX).facing_target, 24u,
              "4: facing_target = 1-1, wraps to 24 (0x0048054f-0x0048055b)");
        ck_eq((uint32_t)fx.u(PLAYER, UNIT_INDEX).facing_current, 1u,
              "4: facing_current NOT committed (walk_step_allowed==0), stays 1 (0x0048057b JZ taken)");
        ck(g_soldier_heading_calls.size() == 1 && g_soldier_heading_calls[0].player == PLAYER &&
               g_soldier_heading_calls[0].unit_index == UNIT_INDEX && g_soldier_heading_calls[0].sprite_frame == 24,
           "4: unit_soldiers_set_heading(player, index, NEW facing_target=24) (0x004805d0-0x004805e1)");
    }

    // =================================================================================================
    // 5 -- TURN, sufficient budget, INCREMENT via block 2's second condition (facing_target<
    // facing_needed, small diff -- neither >12 nor <=-12).
    // =================================================================================================
    {
        unit &u                                = seed(fx, PLAYER, UNIT_INDEX, SLOT);
        g_dir_from_to_ret                      = 15;
        u.facing_target                        = 10; // diff_a=-5 (not>12); block2: diff_b=-5 (not<=-12); 10<15 -> increment
        u.facing_current                       = 10;
        fx.cfg_units[PROTO].turn_speed[PLAYER] = 1.0;
        fx.tick_budget                         = 5.0;
        call(fx);
        ck_eq((uint32_t)fx.u(PLAYER, UNIT_INDEX).facing_target, 11u,
              "5: block2 facing_target<facing_needed -- increment, 10+1=11 (0x00480523-0x00480530)");
    }

    // =================================================================================================
    // 6 -- TURN, sufficient budget, DECREMENT via block 2's fallthrough (facing_target>=facing_needed,
    // small diff -- proves the fallthrough arm independently of the diff_b<=-12 arm in case 4).
    // =================================================================================================
    {
        unit &u                                = seed(fx, PLAYER, UNIT_INDEX, SLOT);
        g_dir_from_to_ret                      = 10;
        u.facing_target                        = 15; // diff_a=5 (not>12); block2: diff_b=5 (not<=-12); 15<10 false -> fallthrough decrement
        u.facing_current                       = 15;
        fx.cfg_units[PROTO].turn_speed[PLAYER] = 1.0;
        fx.tick_budget                         = 5.0;
        call(fx);
        ck_eq((uint32_t)fx.u(PLAYER, UNIT_INDEX).facing_target, 14u,
              "6: block2 fallthrough -- decrement, 15-1=14 (0x0048052b fallthrough-0x00480552)");
    }

    // =================================================================================================
    // 7 -- STEP, insufficient budget: activity_clock -= tick_budget, tick_budget zeroed, RETURN -- no
    // move_microstep change, no set_state/door/target/path callee. dir_step_factor(facing_target) IS
    // called (proves the STEP branch, not TURN, was taken -- disjoint callee sets).
    // =================================================================================================
    {
        unit &u                                = seed(fx, PLAYER, UNIT_INDEX, SLOT);
        g_dir_from_to_ret                      = 7;
        u.facing_target                        = 7; // == facing_needed -> STEP
        u.move_microstep                       = 5;
        u.activity_clock                       = 10.0;
        fx.cfg_units[PROTO].step_speed[PLAYER] = 3.0;
        fx.cfg_units[PROTO].soldier_count      = 0;
        g_dir_step_factor_ret                  = 2.0; // step_cost = 2*3 = 6.0 (no soldier mult)
        fx.tick_budget                         = 4.0; // < 6.0
        call(fx);
        ck(trace_eq({"dir_from_to", "dir_step_factor"}), "7: STEP insufficient budget touches no further callee");
        ck_eq_d(fx.u(PLAYER, UNIT_INDEX).activity_clock, 6.0,
                "7: activity_clock -= tick_budget (10-4), 0x00480667-0x00480673");
        ck_eq_d(fx.tick_budget, 0.0, "7: tick_budget zeroed, 0x00480678-0x00480680");
        ck_eq((uint32_t)fx.u(PLAYER, UNIT_INDEX).move_microstep, 5u, "7: move_microstep untouched");
        ck(g_dir_step_factor_args.size() == 1 && g_dir_step_factor_args[0] == 7,
           "7: dir_step_factor(facing_target) (0x004805eb-0x004805f8)");
    }

    // =================================================================================================
    // 8 -- STEP, sufficient budget, soldier_count>0 applies the transport multiplier (0.5) to
    // step_cost, below the single-group microstep threshold (A_GARAGE, group_count=1, threshold=0x3f)
    // -- move_microstep INCREMENTS by 1, no finish-path callee fires.
    // =================================================================================================
    {
        unit &u                                = seed(fx, PLAYER, UNIT_INDEX, SLOT);
        g_dir_from_to_ret                      = 7;
        u.facing_target                        = 7;
        u.move_microstep                       = 0x10; // well below 0x3f
        fx.cfg_buildings[B_INDEX].type         = BUILDING_TYPE_A_GARAGE;
        fx.cfg_units[PROTO].step_speed[PLAYER] = 4.0;
        fx.cfg_units[PROTO].soldier_count      = 5;
        g_dir_step_factor_ret                  = 2.0; // step_cost = 2*4*0.5 = 4.0
        fx.tick_budget                         = 4.0; // exactly sufficient -- proves the *0.5 happened
                                                      // (without the multiplier step_cost would be 8.0
                                                      // and this budget would take the insufficient arm)
        call(fx);
        ck(trace_eq({"dir_from_to", "dir_step_factor"}), "8: STEP below-threshold increment touches no finish callee");
        ck_eq_d(fx.tick_budget, 0.0,
                "8: tick_budget -= step_cost*0.5 (4.0-4.0=0), proves the soldier-transport multiplier fired, "
                "0x0048061d-0x0048069c");
        ck_eq((uint32_t)fx.u(PLAYER, UNIT_INDEX).move_microstep, 0x11u,
              "8: move_microstep += 1 (0x1f threshold not yet reached), 0x0048073f-0x0048074a");
        ck(g_set_state_calls.empty(), "8: unit_set_state NOT called below threshold");
    }

    // =================================================================================================
    // 9 -- STEP, sufficient budget, move_microstep ALREADY AT the single-group threshold (A_GARAGE,
    // group_count=1, threshold=0x3f -- the check is `move_microstep < threshold`, so being AT the
    // threshold takes the finish arm, not the increment arm), target2_ref==0 and path_slot_id==0xff --
    // finish path fires door release + PARKED transition, but SKIPS both the target-release and
    // path-free calls.
    // =================================================================================================
    {
        unit &u                                = seed(fx, PLAYER, UNIT_INDEX, SLOT);
        g_dir_from_to_ret                      = 7;
        u.facing_target                        = 7;
        u.move_microstep                       = 0x3f; // AT the single-group threshold -- 0x00480737 JL not taken
        u.target2_ref                          = 0;
        u.path_slot_id                         = 0xff;
        fx.cfg_buildings[B_INDEX].type         = BUILDING_TYPE_A_GARAGE;
        fx.cfg_units[PROTO].step_speed[PLAYER] = 1.0;
        fx.cfg_units[PROTO].soldier_count      = 0;
        g_dir_step_factor_ret                  = 1.0;
        fx.tick_budget                         = 1.0;
        unit_storage &st                       = fx.storage[(size_t)PLAYER * STORAGE_PER_PLAYER + SLOT];
        st.door_mutex_unit                     = 77;
        call(fx);
        ck_eq((uint32_t)fx.u(PLAYER, UNIT_INDEX).move_microstep, 0x3fu,
              "9: move_microstep NOT incremented further once at/above threshold, stays 0x3f");
        ck_eq((uint32_t)st.door_mutex_unit, 0u, "9: door_mutex_unit released to 0, 0x0048074f-0x0048075d");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_PARKED,
           "9: unit_set_state(PARKED=0x1f), 0x0048075f-0x0048076a");
        ck(g_release_calls.empty(), "9: target_release_ref NOT called (target2_ref==0)");
        ck(g_path_free_calls.empty(), "9: path_free_slot NOT called (path_slot_id==0xff)");
    }

    // =================================================================================================
    // 10 -- STEP finish, DEFAULT two-group building (cfg type not A_GARAGE/A_SHUTTLE/H_GARAGE,
    // threshold=0x5f), target2_ref!=0 AND path_slot_id!=0xff -- BOTH cleanup calls fire, in order
    // (target_release_ref THEN target2_ref cleared THEN path_free_slot).
    // =================================================================================================
    {
        unit &u                                = seed(fx, PLAYER, UNIT_INDEX, SLOT);
        g_dir_from_to_ret                      = 7;
        u.facing_target                        = 7;
        u.move_microstep                       = 0x5f; // AT the two-group threshold
        u.target2_ref                          = (int16_t)0x99;
        u.path_slot_id                         = 12;   // != 0xff
        fx.cfg_buildings[B_INDEX].type         = 0x02; // deliberately NOT A_GARAGE(8)/A_SHUTTLE(0xd)/H_GARAGE(0x1c)
        fx.cfg_units[PROTO].step_speed[PLAYER] = 1.0;
        fx.cfg_units[PROTO].soldier_count      = 0;
        g_dir_step_factor_ret                  = 1.0;
        fx.tick_budget                         = 1.0;
        call(fx);
        ck(trace_eq({"dir_from_to", "dir_step_factor", "unit_set_state", "target_release_ref", "path_free_slot"}),
           "10: two-group finish -- exact call order, target_release_ref BEFORE path_free_slot "
           "(0x0048075f-0x004807cf)");
        ck(g_release_calls.size() == 1 && g_release_calls[0].player_idx == PLAYER &&
               g_release_calls[0].unit_idx == UNIT_INDEX && g_release_calls[0].mode == 3u,
           "10: target_release_ref(player, index, mode=3), 0x0048076e-0x0048078a");
        ck_eq((uint32_t)(uint16_t)fx.u(PLAYER, UNIT_INDEX).target2_ref, 0u,
              "10: target2_ref cleared AFTER the release call, 0x0048079e-0x004807ac");
        ck(g_path_free_calls.size() == 1 && g_path_free_calls[0].player == PLAYER &&
               g_path_free_calls[0].unit_index == UNIT_INDEX,
           "10: path_free_slot(player, index), 0x004807c1-0x004807cf");
    }

    // =================================================================================================
    // 11 -- STEP finish, two-group building, move_microstep starts one BELOW the two-group threshold
    // (0x5e) -- sanity that 0x5f (not 0x3f) really is this building's own threshold: the increment arm
    // fires here, not the finish arm.
    // =================================================================================================
    {
        unit &u                                = seed(fx, PLAYER, UNIT_INDEX, SLOT);
        g_dir_from_to_ret                      = 7;
        u.facing_target                        = 7;
        u.move_microstep                       = 0x5e; // one below the two-group threshold 0x5f
        fx.cfg_buildings[B_INDEX].type         = 0x02; // default two-group
        fx.cfg_units[PROTO].step_speed[PLAYER] = 1.0;
        g_dir_step_factor_ret                  = 1.0;
        fx.tick_budget                         = 1.0;
        call(fx);
        ck_eq((uint32_t)fx.u(PLAYER, UNIT_INDEX).move_microstep, 0x5fu,
              "11: two-group threshold is 0x5f (group_count=2 -> 2*0x20+0x1f), increments TO it "
              "(0x0048072c-0x00480734/0x0048073f-0x0048074a)");
        ck(g_set_state_calls.empty(), "11: unit_set_state NOT called -- still one tick short of finishing");
    }
}

} // namespace mh::sim::test
