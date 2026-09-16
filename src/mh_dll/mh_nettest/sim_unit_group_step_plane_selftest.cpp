//
// sim_unit_group_step_plane_selftest.cpp -- `simtest` oracle for llm_strat_unit_group_step_plane
// @0x00484b4a (sim/sim_unit_group_step_plane.h/.cpp, RI-SIM / SIM1-G1).
//
// SCOPE (honest, not exhaustive): the entry write of _G_LLM_STRAT_PATHFINDER_AIR_MODE_FLAG to 1
// (0x00484b66 -- NOT 2, see the CRITICAL note below) and the free_slot==-1 early-return arm
// (0x00484b7f/0x00484b83/0x00484b85-0x00484b8f); the path_slot_id!=0xff gate that guards
// llm_strat_path_free_slot (0x00484b99/0x00484ba0/0x00484ba2-0x00484bb0), both sides; the
// ATTACK_UNIT/ATTACK_UNIT_RETURN target-revalidation branch (0x00484bba/0x00484bc1) -- the DEAD-target
// arm's UNCONDITIONAL release (no `target_ref != 0` guard, proven with target_ref==0, 0x00484c12 JNC
// taken / 0x00484cad-0x00484cf0) and the ALIVE-target arm's unit_get_coords call + the fine->tile
// truncating-divide conversion into goal_x/goal_y, exercised with a NEGATIVE fine value to separate
// truncation from floor-shift (0x00484c12 JNC not taken / 0x00484c18-0x00484ca5); the neutral-order
// path that skips the whole attack block untouched; the LANDING_REQUEST gate (0x00484d31/0x00484d3b)
// with home_building_idx==0 (facing_override/air-mode-flag both left alone, 0x00484d45/0x00484d49) vs
// !=0 (facing_override derived from cfg_buildings[buildings[player][idx].building_id].facing AND
// _G_LLM_STRAT_PATHFINDER_AIR_MODE_FLAG RESET TO 0, 0x00484d3d-0x00484d77 -- THE ONE CONDITIONAL RESET
// IN THIS FUNCTION); the final unconditional block's exact args + call order for trace_greedy_path /
// the pathtrace_remove_loops drain loop (zero and many iterations) / path_write_from_solver /
// unit_set_state(cfg_units[unit_proto_id].move_op_code) / unit_notify_status(..., 100)
// (0x00484d81-0x00484e01); and non-corruption of a neighbouring roster unit, a neighbouring building,
// and the ATTACK target unit's own fields (this function never writes them, only the CURRENT unit's
// target_fine_x/y cache).
//
// CRITICAL, RE-CONFIRMED FROM THIS .asm (do not trust older prose): the entry write at 0x00484b66 is
// `MOV dword ptr [0x0066a9a4],0x1` -- writes 1, not 2. The ONE conditional path that resets it writes
// `MOV dword ptr [0x0066a9a4],0x0` at 0x00484d77, inside the LANDING_REQUEST-with-ready-home-building
// arm. Two earlier claims in this tree -- "ground writes 1 / plane writes 2" and "1 for both" -- were
// BOTH wrong; T1 and T7 below pin the actual 1-then-0 values against these two exact addresses.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM
// tmp/decomp_sim/llm_strat_unit_group_step_plane_00484b4a.asm -- every assertion below cites the
// instruction address(es) it pins. NOT read off the .cpp body.
//
#include "sim/sim_unit_group_step_plane.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim/sim_order_enqueue.h" // UNIT_STATE_STOP_TO_DEFAULT / UNIT_STATE_IDLE_SCATTER -- shared public constants
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- this TU's own local copies of the TU-local (anonymous-namespace, not visible here) order/notify
// literals -- same "own local copy, value-identical, cross-checked against the .cpp" convention
// sim_unit_state_hover_engage_selftest.cpp already established (its own constants have internal
// linkage in the .cpp and are not reachable from this file).
inline constexpr uint16_t ORDER_ATTACK_UNIT        = 0x1a; // 0x00484bba
inline constexpr uint16_t ORDER_ATTACK_UNIT_RETURN = 0x1b; // 0x00484bc1
inline constexpr uint16_t ORDER_LANDING_REQUEST    = 0x29; // 0x00484d31
inline constexpr uint32_t NOTIFY_PATH_COMPUTED     = 100;  // 0x64, 0x00484dee

// ---- shared trace: one sequence proves CALL ORDER across all 11 callees --------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- per-callee recorders (11, one per unit_group_step_plane_calls member) ------------------------
int32_t g_free_slot_ret = 42;
struct FindFreeSlotCall {
    int32_t player;
};
std::vector<FindFreeSlotCall> g_find_free_slot_calls;
int32_t                       rec_path_find_free_slot(int32_t player) {
    tr("path_find_free_slot");
    g_find_free_slot_calls.push_back({player});
    return g_free_slot_ret;
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

struct ReleaseRefCall {
    uint32_t player_idx;
    int32_t  unit_idx;
    uint32_t mode;
};
std::vector<ReleaseRefCall> g_release_ref_calls;
void                        rec_target_release_ref(uint32_t player_idx, int32_t unit_idx, uint32_t mode) {
    tr("target_release_ref");
    g_release_ref_calls.push_back({player_idx, unit_idx, mode});
}

int32_t g_get_coords_fine_x = 0;
int32_t g_get_coords_fine_y = 0;
struct GetCoordsCall {
    uint16_t player;
    int32_t  unit_index;
};
std::vector<GetCoordsCall> g_get_coords_calls;
void                       rec_unit_get_coords(uint16_t player, int32_t unit_index, int32_t *out_fine_x, int32_t *out_fine_y) {
    tr("unit_get_coords");
    g_get_coords_calls.push_back({player, unit_index});
    if (out_fine_x != nullptr) *out_fine_x = g_get_coords_fine_x;
    if (out_fine_y != nullptr) *out_fine_y = g_get_coords_fine_y;
}

struct SetStateOrderCall {
    uint16_t new_order;
    uint16_t new_state;
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

int32_t g_home_building_ret   = 0;
int32_t g_home_building_calls = 0;
int32_t rec_unit_get_ready_home_building() {
    tr("unit_get_ready_home_building");
    ++g_home_building_calls;
    return g_home_building_ret;
}

struct TraceGreedyPathCall {
    int32_t start_col, start_row, mode, goal_col, goal_row, heading;
};
std::vector<TraceGreedyPathCall> g_trace_greedy_path_calls;
uint8_t                         *rec_trace_greedy_path(int32_t start_col, int32_t start_row, int32_t mode, int32_t goal_col,
                                                       int32_t goal_row, int32_t heading) { // committed return uint8_t * (TACT1-P C6, 2026-09-04)
    tr("trace_greedy_path");
    g_trace_greedy_path_calls.push_back({start_col, start_row, mode, goal_col, goal_row, heading});
    return nullptr;
}

// The drain loop: returns 1 for the first `g_remove_loops_remaining` calls, then 0.
int32_t g_remove_loops_remaining = 0;
int32_t g_remove_loops_calls     = 0;
int32_t rec_pathtrace_remove_loops() {
    tr("pathtrace_remove_loops");
    ++g_remove_loops_calls;
    if (g_remove_loops_remaining > 0) {
        --g_remove_loops_remaining;
        return 1;
    }
    return 0;
}

struct WriteFromSolverCall {
    uint32_t player;
    int32_t  unit_index;
    uint32_t src_x, src_y;
    int32_t  free_slot;
};
std::vector<WriteFromSolverCall> g_write_from_solver_calls;
void                             rec_path_write_from_solver(uint32_t player, int32_t unit_index, uint32_t src_x, uint32_t src_y,
                                                            int32_t free_slot) {
    tr("path_write_from_solver");
    g_write_from_solver_calls.push_back({player, unit_index, src_x, src_y, free_slot});
}

struct NotifyStatusCall {
    uint32_t player;
    int32_t  unit_index;
    uint32_t status_code;
};
std::vector<NotifyStatusCall> g_notify_status_calls;
void                          rec_unit_notify_status(uint32_t player, int32_t unit_index, uint32_t status_code) {
    tr("unit_notify_status");
    g_notify_status_calls.push_back({player, unit_index, status_code});
}

const unit_group_step_plane_calls g_calls = {
    &rec_path_find_free_slot,
    &rec_path_free_slot,
    &rec_target_release_ref,
    &rec_unit_get_coords,
    &rec_unit_set_state_order,
    &rec_unit_set_state,
    &rec_unit_get_ready_home_building,
    &rec_trace_greedy_path,
    &rec_pathtrace_remove_loops,
    &rec_path_write_from_solver,
    &rec_unit_notify_status,
};

void reset_observations() {
    g_trace.clear();
    g_find_free_slot_calls.clear();
    g_path_free_calls.clear();
    g_release_ref_calls.clear();
    g_get_coords_calls.clear();
    g_set_state_order_calls.clear();
    g_set_state_calls.clear();
    g_home_building_calls = 0;
    g_trace_greedy_path_calls.clear();
    g_remove_loops_calls = 0;
    g_write_from_solver_calls.clear();
    g_notify_status_calls.clear();
}

// Fixed "guard" slot no test's own (player,index) ever touches -- seeded with sentinel nonzero data
// each run so a wrong-index write is observable, not just a plain zero.
constexpr uint16_t GUARD_PLAYER = 6;
constexpr int32_t  GUARD_INDEX  = 9;

void seed_guard_slot(sim_fixture &fx) {
    unit &g         = fx.u(GUARD_PLAYER, GUARD_INDEX);
    g.unit_proto_id = 91;
    g.order         = 0x44;
    g.path_slot_id  = 0x22;
    g.target_ref    = 0x37;
    g.target_index  = 0x28;
    g.target_fine_x = 4001;
    g.target_fine_y = 4002;
    g.x             = 11;
    g.y             = 22;
    g.goal_x        = 33;
    g.goal_y        = 44;
    g.move_heading  = 5;

    building &gb   = fx.b(GUARD_PLAYER, 40);
    gb.building_id = 66;
}

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player  = 2;
    int32_t  index   = 4;
    uint16_t cfg_row = 10; // unit_proto_id

    uint8_t  path_slot_id = 0xff; // 0xff == none, default: skip path_free_slot
    uint16_t order        = 0x05; // neutral by default -- not ATTACK_*, not LANDING_REQUEST

    // ATTACK_UNIT / ATTACK_UNIT_RETURN target fields (only relevant when order is one of those two)
    int16_t target_ref    = 3;    // owner = target_ref & 0xf
    int16_t target_index  = 7;    // target slot
    double  target_energy = 50.0; // >0 == alive by default

    int32_t move_heading = 4;

    // LANDING_REQUEST fields
    int32_t  home_building_ret = 0;  // unit_get_ready_home_building() stub return -- 0 == no ready building
    uint16_t building_id       = 20; // buildings[player][home_building_ret].building_id
    uint8_t  building_facing   = 55; // cfg_buildings[building_id].facing

    int32_t free_slot_ret = 42;

    // unit_get_coords stub write-back (into cur_unit's own target_fine_x/y cache)
    int32_t get_coords_fine_x = -65; // NEGATIVE -- truncating-divide vs floor-shift separator
    int32_t get_coords_fine_y = 97;

    uint8_t move_op_code            = 0x77; // cfg_units[cfg_row].move_op_code
    int32_t remove_loops_extra_hits = 0;    // pathtrace_remove_loops returns 1 this many times, then 0
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();
    seed_guard_slot(fx);

    unit &u         = fx.u(s.player, s.index);
    u.unit_proto_id = s.cfg_row;
    u.path_slot_id  = s.path_slot_id;
    u.order         = s.order;
    u.target_ref    = s.target_ref;
    u.target_index  = s.target_index;
    u.move_heading  = (uint8_t)s.move_heading;
    // Distinct, non-symmetric x/y/goal_x/goal_y so a translation that swapped an axis or forgot to
    // re-snapshot after the attack block disagrees with the fixture.
    u.x      = 60;
    u.y      = 70;
    u.goal_x = 80;
    u.goal_y = 90;

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = s.player;
    fx.view_cur_index  = (uint16_t)s.index;

    // The ATTACK target unit -- a DIFFERENT roster slot from both the acting unit and the guard.
    const uint32_t target_owner                        = (uint32_t)(s.target_ref & 0xf);
    fx.u((int32_t)target_owner, s.target_index).energy = s.target_energy;

    fx.cfg_units[s.cfg_row].move_op_code = s.move_op_code;

    building &b                                            = fx.b(s.player, s.home_building_ret == 0 ? 1 : s.home_building_ret);
    b.building_id                                          = s.building_id;
    fx.cfg_buildings[s.building_id].door_approach_route[0] = s.building_facing;

    reset_observations();
    g_free_slot_ret          = s.free_slot_ret;
    g_home_building_ret      = s.home_building_ret;
    g_get_coords_fine_x      = s.get_coords_fine_x;
    g_get_coords_fine_y      = s.get_coords_fine_y;
    g_remove_loops_remaining = s.remove_loops_extra_hits;

    sim_store own = fx.store();
    detail::unit_group_step_plane(fx.view(), own, g_calls);
}

} // namespace

void run_unit_group_step_plane_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- the entry write of _G_LLM_STRAT_PATHFINDER_AIR_MODE_FLAG is 1, NOT 2 (0x00484b66), and the
    // free_slot==-1 early-return arm (0x00484b7f CMP / 0x00484b83 JNZ / 0x00484b85-0x00484b8f):
    // unit_set_state(IDLE_SCATTER=0x13) fires and the function returns immediately -- no further
    // calls, and the flag is left at 1 (the reset at 0x00484d77 is never reached on this path).
    // =================================================================================================
    {
        Seed s;
        s.free_slot_ret             = -1;
        fx.pathfinder_air_mode_flag = 9; // sentinel, distinct from both 0 and 1
        seed_and_run(fx, s);
        ck_eq((uint32_t)fx.pathfinder_air_mode_flag, 1u,
              "T1: pathfinder_air_mode_flag = 1 at entry (0x00484b66), NOT 2");
        ck(g_find_free_slot_calls.size() == 1 && g_find_free_slot_calls[0].player == s.player,
           "T1: path_find_free_slot(cur_player) called (0x00484b70-0x00484b77)");
        ck(trace_eq({"path_find_free_slot", "unit_set_state"}),
           "T1: free_slot==-1 -- ONLY unit_set_state fires, nothing else (0x00484b7f-0x00484b8f)");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_IDLE_SCATTER,
           "T1: unit_set_state(IDLE_SCATTER=0x13) (0x00484b85-0x00484b8a)");
    }

    // =================================================================================================
    // T2 -- the path_slot_id!=0xff gate (0x00484b99 CMP / 0x00484ba0 JZ) guarding
    // llm_strat_path_free_slot(cur_player, cur_index) -- both sides, exact args when it fires. Order
    // held neutral (not ATTACK_*/LANDING_REQUEST) so this case isolates the gate alone.
    // =================================================================================================
    {
        Seed s;
        s.path_slot_id = 0xff;
        seed_and_run(fx, s);
        ck(g_path_free_calls.empty(), "T2a: path_slot_id==0xff -- path_free_slot does NOT fire (0x00484ba0 JZ taken)");

        s.path_slot_id = 5;
        seed_and_run(fx, s);
        ck(g_path_free_calls.size() == 1 && g_path_free_calls[0].player == s.player &&
               g_path_free_calls[0].unit_index == s.index,
           "T2b: path_slot_id!=0xff -- path_free_slot(cur_player, cur_index) fires with the RIGHT args "
           "(0x00484ba2-0x00484bb0)");
    }

    // =================================================================================================
    // T3 -- ATTACK_UNIT/ATTACK_UNIT_RETURN target-revalidation, DEAD-target arm (energy<=0.0,
    // 0x00484c12 JNC taken -> 0x00484cad): UNCONDITIONAL target_release_ref (no `target_ref != 0`
    // guard, unlike move_walker's analogous cascade) -- T3a proves the release still fires with
    // target_ref==0 (the value a guard would have skipped on); T3b (the OTHER order value, 0x1b) uses
    // a NONZERO target_ref/target_index sentinel to prove they really get zeroed, plus the exact
    // unit_set_state_order args and the immediate return (no path computation, no notify_status).
    // Energy is exactly 0.0 -- the JNC boundary -- so a translation using `< 0.0` instead of `<= 0.0`
    // disagrees here.
    // =================================================================================================
    {
        Seed s;
        s.order         = ORDER_ATTACK_UNIT;
        s.target_ref    = 0; // no guard would have skipped on this
        s.target_index  = 5;
        s.target_energy = 0.0; // dead, exactly at the JNC boundary
        seed_and_run(fx, s);
        ck(g_release_ref_calls.size() == 1 && g_release_ref_calls[0].player_idx == s.player &&
               g_release_ref_calls[0].unit_idx == s.index && g_release_ref_calls[0].mode == 1u,
           "T3a: target_ref==0 -- target_release_ref(cur_player, cur_index, mode=1) STILL fires, no "
           "`target_ref != 0` guard (0x00484cad-0x00484cc5)");
        ck(g_get_coords_calls.empty(), "T3a: dead target -- unit_get_coords does NOT fire");
    }
    {
        Seed s;
        s.order = ORDER_ATTACK_UNIT_RETURN;
        // 0x45, NOT decimal 77. The owner is the LOW NIBBLE, and 77 == 0x4d has low nibble 0xd ==
        // 13 -- past MAX_PLAYERS (8), so seed_and_run's `fx.u(target_ref & 0xf, target_index)`
        // wrote a double ~68 KB past the end of the fixture's `units` vector. It corrupted the run
        // silently (see sim_test_support.h's bounds guard on u(), added because of this).
        s.target_ref    = 0x45; // low nibble 5 -- valid roster owner, nonzero sentinel to zero
        s.target_index  = 88;
        s.target_energy = 0.0; // dead
        seed_and_run(fx, s);
        ck(g_release_ref_calls.size() == 1 && g_release_ref_calls[0].mode == 1u,
           "T3b: target_release_ref(cur_player, cur_index, mode=1) fires (order=ATTACK_UNIT_RETURN too) "
           "(0x00484cc0-0x00484cc5)");
        const unit &u = fx.u(s.player, s.index);
        ck_eq((uint32_t)(uint16_t)u.target_ref, 0u,
              "T3b: cur_unit->target_ref zeroed (0x00484cca-0x00484cd3)");
        ck_eq((uint32_t)(uint16_t)u.target_index, 0u,
              "T3b: cur_unit->target_index zeroed (0x00484cd8-0x00484ce1)");
        ck(g_set_state_order_calls.size() == 1 &&
               g_set_state_order_calls[0].new_order == UNIT_STATE_STOP_TO_DEFAULT &&
               g_set_state_order_calls[0].new_state == UNIT_STATE_STOP_TO_DEFAULT,
           "T3b: unit_set_state_order(STOP_TO_DEFAULT, STOP_TO_DEFAULT) (0x00484ce1-0x00484ceb)");
        ck(trace_eq({"path_find_free_slot", "target_release_ref", "unit_set_state_order"}),
           "T3b: immediate return -- no trace_greedy_path/path_write_from_solver/unit_notify_status "
           "(0x00484cf0 JMP to the epilogue)");
    }

    // =================================================================================================
    // T4 -- ATTACK_UNIT target-revalidation, ALIVE-target arm (energy>0.0, 0x00484c12 JNC NOT taken):
    // unit_get_coords(target_owner, target_slot, &target_fine_x, &target_fine_y) with the RIGHT args
    // (0x00484c18-0x00484c54), then goal_x/goal_y = fine_to_tile(target_fine_x/y) via the asm's
    // SAR/SHL/SBB/SAR truncating-divide-by-32 sequence (0x00484c59-0x00484ca5). fine_x is NEGATIVE
    // (-65) to separate truncation-toward-zero (-65/32 = -2, the asm's actual behaviour, hand-verified
    // instruction by instruction) from a floor-shift mistranslation (-65>>5 = -3): a translation using
    // `>> 5` instead of `/ 32` disagrees with the pinned value here.
    // =================================================================================================
    {
        Seed s;
        s.order             = ORDER_ATTACK_UNIT;
        s.target_ref        = 3;
        s.target_index      = 7;
        s.target_energy     = 50.0; // alive
        s.get_coords_fine_x = -65;  // -65/32 = -2 (truncating) -> (uint8_t)-2 = 254
        s.get_coords_fine_y = 97;   //  97/32 =  3 -> 3
        seed_and_run(fx, s);
        ck(g_release_ref_calls.empty(), "T4: alive target -- target_release_ref does NOT fire");
        ck(g_get_coords_calls.size() == 1 && g_get_coords_calls[0].player == (uint16_t)3 &&
               g_get_coords_calls[0].unit_index == 7,
           "T4: unit_get_coords(target_owner=3, target_slot=7) (0x00484c18-0x00484c54)");
        const unit &u = fx.u(s.player, s.index);
        ck_eq((uint32_t)u.target_fine_x, (uint32_t)(int32_t)-65, "T4: cur_unit->target_fine_x = -65 (out-param write)");
        ck_eq((uint32_t)u.target_fine_y, 97u, "T4: cur_unit->target_fine_y = 97 (out-param write)");
        ck_eq((uint32_t)u.goal_x, 254u,
              "T4: goal_x = (uint8_t)fine_to_tile(-65) = (uint8_t)(-2) = 254 -- TRUNCATING divide, not "
              "a floor shift (0x00484c6b-0x00484c7c)");
        ck_eq((uint32_t)u.goal_y, 3u, "T4: goal_y = fine_to_tile(97) = 3 (0x00484c82-0x00484ca5)");
    }

    // =================================================================================================
    // T5 -- order is neither ATTACK_UNIT/_RETURN nor LANDING_REQUEST: the WHOLE attack-revalidation
    // block is skipped (0x00484bba/0x00484bc6 both JNZ taken -> straight to 0x00484cf5) -- x/y/goal_x/
    // goal_y snapshot the UNCHANGED seeded values, and none of the attack-only callees fire.
    // =================================================================================================
    {
        Seed s;
        s.order = 0x05; // neutral
        seed_and_run(fx, s);
        ck(g_release_ref_calls.empty() && g_get_coords_calls.empty(),
           "T5: neutral order -- neither dead-target nor alive-target arm runs at all");
        ck(g_trace_greedy_path_calls.size() == 1 && g_trace_greedy_path_calls[0].start_col == 60 &&
               g_trace_greedy_path_calls[0].start_row == 70 && g_trace_greedy_path_calls[0].goal_col == 80 &&
               g_trace_greedy_path_calls[0].goal_row == 90,
           "T5: x/y/goal_x/goal_y snapshot the seeded values UNCHANGED (60,70,80,90), attack block never touched them");
    }

    // =================================================================================================
    // T6 -- LANDING_REQUEST(0x29) with home_building_idx==0 (0x00484d3b JNZ not taken, then 0x00484d49
    // JZ taken): NEITHER facing_override NOR the air-mode flag are touched -- facing_override stays
    // 0xff (the function's default), and the flag stays 1 (the entry write from 0x00484b66).
    // =================================================================================================
    {
        Seed s;
        s.order             = ORDER_LANDING_REQUEST;
        s.home_building_ret = 0; // no ready home building
        seed_and_run(fx, s);
        ck(g_home_building_calls == 1, "T6: unit_get_ready_home_building() called (0x00484d3d)");
        ck_eq((uint32_t)fx.pathfinder_air_mode_flag, 1u,
              "T6: home_building_idx==0 -- pathfinder_air_mode_flag stays 1, NOT reset (0x00484d49 JZ taken)");
        ck(g_trace_greedy_path_calls.size() == 1 && g_trace_greedy_path_calls[0].heading == 0xff,
           "T6: facing_override (6th trace_greedy_path arg) stays at its 0xff default -- the "
           "cfg_buildings[...].facing overwrite at 0x00484d74 never runs (0x00484d49 JZ taken)");
    }

    // =================================================================================================
    // T7 -- LANDING_REQUEST(0x29) with home_building_idx!=0 (0x00484d49 JZ NOT taken):
    // facing_override = cfg_buildings[buildings[player][home_idx].building_id].facing (distinct
    // building_id/facing values so a swapped lookup disagrees), AND
    // _G_LLM_STRAT_PATHFINDER_AIR_MODE_FLAG is RESET TO 0 -- THE ONE CONDITIONAL RESET IN THIS
    // FUNCTION (0x00484d3d-0x00484d77). This is the fact two separate prose records both
    // pinned WRONG ("ground 1/plane 2", then "1 for both") -- both wrong; this
    // function writes 1 then conditionally 0, never 2.
    // =================================================================================================
    {
        Seed s;
        s.order                     = ORDER_LANDING_REQUEST;
        s.home_building_ret         = 3; // nonzero -- a ready home building
        s.building_id               = 41;
        s.building_facing           = 17; // distinct sentinel
        fx.pathfinder_air_mode_flag = 1;
        seed_and_run(fx, s);
        ck_eq((uint32_t)fx.pathfinder_air_mode_flag, 0u,
              "T7: home_building_idx!=0 -- pathfinder_air_mode_flag RESET TO 0 (0x00484d77), NOT 2");
        ck(g_trace_greedy_path_calls.size() == 1 && g_trace_greedy_path_calls[0].heading == 17,
           "T7: facing_override(6th trace_greedy_path arg) = cfg_buildings[building_id=41].facing = 17, "
           "derived via buildings[player][home_idx=3].building_id (0x00484d3d-0x00484d74)");
    }

    // =================================================================================================
    // T8 -- the final unconditional block (0x00484d81-0x00484e01): trace_greedy_path(x, y, move_heading,
    // goal_x, goal_y, facing_override) with the EXACT arg order (not a swapped pair), then
    // path_write_from_solver(player, unit_index, x, y, free_slot), then
    // unit_set_state(cfg_units[unit_proto_id].move_op_code), then unit_notify_status(player,
    // unit_index, 100) -- exact args and exact CALL ORDER (the asm has no branch that could reorder
    // these four).
    // =================================================================================================
    {
        Seed s;
        s.order         = 0x05; // neutral -- isolates the final block, x/y/goal_x/goal_y = 60/70/80/90
        s.move_heading  = 6;
        s.free_slot_ret = 42;
        s.move_op_code  = 0x33;
        seed_and_run(fx, s);
        ck(g_trace_greedy_path_calls.size() == 1, "T8: trace_greedy_path called exactly once");
        const auto &tgp = g_trace_greedy_path_calls[0];
        ck(tgp.start_col == 60 && tgp.start_row == 70 && tgp.mode == 6 && tgp.goal_col == 80 &&
               tgp.goal_row == 90 && tgp.heading == 0xff,
           "T8: trace_greedy_path(x=60, y=70, mode=move_heading=6, goal_x=80, goal_y=90, "
           "facing_override=0xff) (0x00484d81-0x00484da3)");
        ck(g_write_from_solver_calls.size() == 1 && g_write_from_solver_calls[0].player == s.player &&
               g_write_from_solver_calls[0].unit_index == s.index && g_write_from_solver_calls[0].src_x == 60 &&
               g_write_from_solver_calls[0].src_y == 70 && g_write_from_solver_calls[0].free_slot == 42,
           "T8: path_write_from_solver(cur_player, cur_index, x=60, y=70, free_slot=42) (0x00484db4-0x00484dce)");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x33,
           "T8: unit_set_state(cfg_units[unit_proto_id].move_op_code=0x33) (0x00484dd3-0x00484de9)");
        ck(g_notify_status_calls.size() == 1 && g_notify_status_calls[0].player == s.player &&
               g_notify_status_calls[0].unit_index == s.index &&
               g_notify_status_calls[0].status_code == NOTIFY_PATH_COMPUTED,
           "T8: unit_notify_status(cur_player, cur_index, 100) (0x00484dee-0x00484e01)");
        ck(trace_eq({"path_find_free_slot", "trace_greedy_path", "pathtrace_remove_loops",
                     "path_write_from_solver", "unit_set_state", "unit_notify_status"}),
           "T8: exact call order -- trace_greedy_path, THEN the (single, zero-iteration) drain, THEN "
           "path_write_from_solver, THEN unit_set_state, THEN unit_notify_status");
    }

    // =================================================================================================
    // T9 -- the pathtrace_remove_loops drain (0x00484dab-0x00484db2): a do/while that calls again as
    // long as the return is nonzero. T9a: zero extra hits (first call already returns 0) -> exactly
    // ONE call. T9b: three extra nonzero hits -> exactly FOUR calls -- proves the loop actually
    // iterates and does not just call once.
    // =================================================================================================
    {
        Seed s;
        s.order                   = 0x05;
        s.remove_loops_extra_hits = 0;
        seed_and_run(fx, s);
        ck_eq((uint32_t)g_remove_loops_calls, 1u, "T9a: zero extra hits -- pathtrace_remove_loops called exactly ONCE (0x00484dab, returns 0 immediately)");

        s.remove_loops_extra_hits = 3;
        seed_and_run(fx, s);
        ck_eq((uint32_t)g_remove_loops_calls, 4u,
              "T9b: three nonzero hits -- pathtrace_remove_loops called FOUR times total (3 nonzero + "
              "the terminating 0), proving the loop really drains rather than calling once (0x00484db0 "
              "TEST / 0x00484db2 JNZ)");
    }

    // =================================================================================================
    // T10 -- non-corruption: a guard unit and a guard building this function never addresses stay
    // exactly as seeded across the most field-mutating run (LANDING_REQUEST with a ready home
    // building, full success path); the ATTACK target unit's own fields (only its .energy is ever
    // READ by this function -- the writes go to the CURRENT unit's target_fine_x/y cache, not the
    // target's own record) are likewise untouched.
    // =================================================================================================
    {
        Seed s;
        s.order             = ORDER_LANDING_REQUEST;
        s.home_building_ret = 3;
        s.building_id       = 41;
        s.building_facing   = 17;
        seed_and_run(fx, s);

        const unit &g = fx.u(GUARD_PLAYER, GUARD_INDEX);
        ck(g.unit_proto_id == 91 && g.order == 0x44 && g.path_slot_id == 0x22, "T10: guard unit's dispatch fields untouched");
        ck((uint32_t)(uint16_t)g.target_ref == 0x37 && (uint32_t)(uint16_t)g.target_index == 0x28,
           "T10: guard unit's target_ref/target_index untouched");
        ck(g.target_fine_x == 4001 && g.target_fine_y == 4002, "T10: guard unit's target_fine_x/y untouched");
        ck(g.x == 11 && g.y == 22 && g.goal_x == 33 && g.goal_y == 44, "T10: guard unit's x/y/goal_x/goal_y untouched");
        ck(g.move_heading == 5, "T10: guard unit's move_heading untouched");

        const building &gb = fx.b(GUARD_PLAYER, 40);
        ck(gb.building_id == 66, "T10: guard building's building_id untouched (this function only reads buildings)");

        const uint32_t owner = (uint32_t)(s.target_ref & 0xf);
        const unit    &t     = fx.u((int32_t)owner, s.target_index);
        ck_eq_d(t.energy, s.target_energy, "T10: ATTACK target unit's own .energy untouched (only read, never written)");
        ck(t.x == 0 && t.y == 0 && t.goal_x == 0 && t.goal_y == 0,
           "T10: ATTACK target unit's own x/y/goal_x/goal_y untouched (this function writes the CURRENT "
           "unit's target_fine_x/y cache and goal_x/y, never the target's own record)");
    }
}

} // namespace mh::sim::test
