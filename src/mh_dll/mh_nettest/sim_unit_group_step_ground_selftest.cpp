//
// sim_unit_group_step_ground_selftest.cpp -- `simtest` oracle for llm_strat_unit_group_step_ground
// @0x00483011 (sim/sim_unit_group_step_ground.h/.cpp, RI-SIM / SIM1-G1, THE LARGEST
// BODY IN THE BATCH). THIS IS THE ONLY EVIDENCE for this function: it writes a live malloc'd
// pathfinder workbuf and calls an outward unit-notification per committed group member, so it
// cannot be shadow-armed (see the header's ARM-SAFETY note) -- treat every assertion below as if it
// is the sole thing standing between this function and being marked `verified`.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM
// tmp/decomp_sim/llm_strat_unit_group_step_ground_00483011.asm -- every assertion cites the
// instruction address(es) it pins. NOT read off the .cpp body.
//
// SCOPE (T1..T12, matching sim/sim_unit_group_step_ground.h's own derivation sections):
//   T1  the air-mode-flag write (0x00483034) -- unconditional, BEFORE the ATTACK_* order check,
//       proven via an early-return path that reaches it.
//   T2  target re-validation: target_ref&0x80 selecting unit- vs building-roster (both directions,
//       via decoy energies on the opposite roster), the energy<=0.0 dead-target boundary, and the
//       alive-target unit_get_coords + fine_to_tile(/32, truncating) goal recompute.
//   T3  the plane/heli standoff-skip vs the ground calc_range_approach_point call, its 0-return
//       give-up arm (target_release_ref + default_op_code commit) and its nonzero-return arm, both
//       the already-on-tile (HOVER_ENGAGE) and not-on-tile (fall-through) sides.
//   T4  group assembly: every roster-scan filter, one broken at a time plus a positive control, the
//       LANDING_REQUEST self-exclusion, the same-attack-order self-exclusion, and the group_count==100
//       cap-and-stop (member that HITS the cap is still added; later candidates are never scanned).
//   T5  the centroid call overwriting start_col/start_row before planning begins.
//   T6  the LANDING_REQUEST facing_override arm, both home_building_idx==0 and !=0.
//   T7  pass 1 in isolation: the planning candidate walk + tie-break, the commit non-relocate branch,
//       and the commit relocate branch (unlink/fow/PutOnMap/FoW/heading-commit/state/activity_clock/
//       path_write_from_solver-re-reads-post-relocation-tile/notify_status), including the
//       path_slot_id-free-then-reassign sub-branch, in ONE 2-member pass so call order is pinned too.
//   T8  the free_slot==-1 bail-out, both a plain pass-1 case and a pass-2 case that proves
//       move_heading is left at the RE-CLASSED synthetic value, NOT restored.
//   T9  pass 2's three-arm shape-class remap table + its extra direct `state` word write.
//   T10 pass 3's (DIFFERENT) remap table, with pass 3 actually reached and its commit exercised.
//   T11 the final move_heading restore: firing when pass 3 completes, firing when pass 3 is SKIPPED
//       by the processed_count>=group_count check, and NOT firing on a pass-3 bail-out.
//   T12 non-corruption: a guard unit and a guard building this function never addresses.
//
#include "sim/sim_unit_group_step_ground.h"

#include "sim/sim_order_enqueue.h" // UNIT_STATE_IDLE_SCATTER/_HOVER_ENGAGE, UNIT_TYPE_A_PLANE/_H_PLANE (shared there)

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- this TU's own local copies of the .cpp's TU-local literals (same "own local copy,
// value-identical, cross-checked against the .cpp" convention every SIM1-G1 sibling selftest uses --
// the .cpp's own copies have internal linkage and are not reachable from this file).
inline constexpr uint16_t ORDER_ATTACK_UNIT        = 0x1a; // 0x00483055
inline constexpr uint16_t ORDER_ATTACK_UNIT_RETURN = 0x1b; // 0x00483061
inline constexpr uint16_t ORDER_ATTACK_BUILDING    = 0x1c; // 0x0048306f
inline constexpr uint16_t ORDER_LANDING_REQUEST    = 0x29; // 0x00483461 / 0x004835bf
inline constexpr uint16_t STATE_GROUP_STEP         = 0x0b; // 0x00483416
inline constexpr int16_t  STATE_FORMATION_PLACED   = 0x37; // 0x00483b36 / 0x00484208 / 0x004848fa
inline constexpr uint32_t NOTIFY_PATH_COMPUTED     = 100;  // 0x64, 0x00483c0f / 0x00484332 / 0x004849d3

// ---- shared trace: proves CALL ORDER across all 21 callees -----------------------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- per-callee recorders (21, one per unit_group_step_ground_calls member) -----------------------

int32_t g_unit_coords_x = 0, g_unit_coords_y = 0;
struct GetCoordsCall {
    uint16_t player;
    int32_t  unit_index;
};
std::vector<GetCoordsCall> g_get_coords_calls;
void                       rec_unit_get_coords(uint16_t player, int32_t unit_index, int32_t *out_fine_x, int32_t *out_fine_y) {
    tr("unit_get_coords");
    g_get_coords_calls.push_back({player, unit_index});
    *out_fine_x = g_unit_coords_x;
    *out_fine_y = g_unit_coords_y;
}

int32_t g_bldg_coords_x = 0, g_bldg_coords_y = 0;
struct BldgGetCoordsCall {
    uint16_t player;
    int32_t  building_index;
};
std::vector<BldgGetCoordsCall> g_bldg_coords_calls;
void                           rec_bldg_get_coords(uint16_t player, int32_t building_index, int32_t *out_fine_x,
                                                   int32_t *out_fine_y) {
    tr("bldg_get_coords");
    g_bldg_coords_calls.push_back({player, building_index});
    *out_fine_x = g_bldg_coords_x;
    *out_fine_y = g_bldg_coords_y;
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

struct SetStateOfCall {
    int32_t player;
    int32_t unit_index;
    int16_t new_state;
};
std::vector<SetStateOfCall> g_set_state_of_calls;
void                        rec_unit_set_state_of(int32_t player, int32_t unit_index, int16_t new_state) {
    tr("unit_set_state_of");
    g_set_state_of_calls.push_back({player, unit_index, new_state});
}

int32_t g_approach_ret = 0;
int32_t g_approach_col = 0, g_approach_row = 0;
int32_t g_approach_calls = 0;
int32_t rec_unit_calc_range_approach_point(uint32_t *io_tile_x, uint32_t *io_tile_y) {
    tr("unit_calc_range_approach_point");
    ++g_approach_calls;
    if (g_approach_ret != 0) {
        *io_tile_x = (uint32_t)g_approach_col;
        *io_tile_y = (uint32_t)g_approach_row;
    }
    return g_approach_ret;
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

// Simulates the callee's real registration side effect (writing the member into the scratch array
// at the caller-owned count) so the COMMIT passes downstream have real, addressable roster indices
// to work with -- see the header's own step-3/step-4 derivation. g_scratch_add_increment defaults to
// the real callee's +1; T4's cap sub-case overrides it as a TEST-ONLY device (see that case).
group_scratch_member *g_scratch_ptr = nullptr;
// Per-CALL increment queue (same pattern as g_free_slot_queue/g_find_slot_queue below) -- defaults
// to the real callee's +1 for every call not explicitly queued. This matters because self's own
// unconditional step-3 registration (0x004833dd) goes through this SAME recorder as every later-scan
// match, so a blanket override (a single shared increment) would bump self's call too and desync any
// test that wants the cap check's exact-100 arithmetic (T4-cap) to add up.
std::vector<int32_t> g_scratch_add_increment_queue;
size_t               g_scratch_add_increment_pos = 0;
struct ScratchAddCall {
    int32_t player;
    int32_t unit_index;
};
std::vector<ScratchAddCall> g_scratch_add_calls;
void                        rec_group_scratch_add_unit_and_normalize_heading(int32_t player, int32_t unit_index, int32_t *io_count) {
    tr("group_scratch_add_unit_and_normalize_heading");
    g_scratch_add_calls.push_back({player, unit_index});
    if (g_scratch_ptr != nullptr && *io_count >= 0 && *io_count < 100) {
        g_scratch_ptr[*io_count].unit_idx = unit_index;
    }
    const int32_t inc = (g_scratch_add_increment_pos < g_scratch_add_increment_queue.size())
                                                   ? g_scratch_add_increment_queue[g_scratch_add_increment_pos++]
                                                   : 1;
    *io_count += inc;
}

int32_t g_centroid_x = 0, g_centroid_y = 0;
struct CentroidCall {
    int32_t player;
    int32_t member_count;
};
std::vector<CentroidCall> g_centroid_calls;
void                      rec_group_scratch_compute_centroid(int32_t player, int32_t member_count, int32_t *out_x,
                                                             int32_t *out_y) {
    tr("group_scratch_compute_centroid");
    g_centroid_calls.push_back({player, member_count});
    *out_x = g_centroid_x;
    *out_y = g_centroid_y;
}

int32_t g_home_building_ret   = 0;
int32_t g_home_building_calls = 0;
int32_t rec_unit_get_ready_home_building() {
    tr("unit_get_ready_home_building");
    ++g_home_building_calls;
    return g_home_building_ret;
}

// Writes the caller-supplied length into the fixture's OWN pathtrace_len field (captured by run()),
// simulating the real callee's side effect on _G_LLM_STRAT_PATHTRACE_LEN, so pass_plan's post-drain
// `*v.pathtrace_len` reads observe a TEST-CONTROLLED value per call.
uint32_t             *g_pathtrace_len_ptr = nullptr;
std::vector<uint32_t> g_pathtrace_len_queue;
size_t                g_pathtrace_len_pos = 0;
struct TraceGreedyPathCall {
    int32_t start_col, start_row, mode, goal_col, goal_row, heading;
};
std::vector<TraceGreedyPathCall> g_trace_greedy_path_calls;
uint8_t                         *rec_trace_greedy_path(int32_t start_col, int32_t start_row, int32_t mode, int32_t goal_col,
                                                       int32_t goal_row, int32_t heading) { // committed return uint8_t * (TACT1-P C6, 2026-09-04)
    tr("trace_greedy_path");
    g_trace_greedy_path_calls.push_back({start_col, start_row, mode, goal_col, goal_row, heading});
    if (g_pathtrace_len_ptr != nullptr) {
        *g_pathtrace_len_ptr =
            (g_pathtrace_len_pos < g_pathtrace_len_queue.size()) ? g_pathtrace_len_queue[g_pathtrace_len_pos++] : 0u;
    }
    return nullptr;
}

int32_t g_remove_loops_calls = 0;
int32_t rec_pathtrace_remove_loops() {
    tr("pathtrace_remove_loops");
    ++g_remove_loops_calls;
    return 0; // always drains in exactly one call -- the drain-loop ITERATION COUNT is already pinned
              // by this batch's plane sibling (sim_unit_group_step_plane_selftest.cpp T9); out of scope here.
}

std::vector<int32_t> g_free_slot_queue;
size_t               g_free_slot_pos = 0;
struct FreeSlotCall {
    int32_t player;
};
std::vector<FreeSlotCall> g_free_slot_calls;
int32_t                   rec_path_find_free_slot(int32_t player) {
    tr("path_find_free_slot");
    g_free_slot_calls.push_back({player});
    return (g_free_slot_pos < g_free_slot_queue.size()) ? g_free_slot_queue[g_free_slot_pos++] : -1;
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

struct WriteFromSolverCall {
    uint32_t player;
    int32_t  unit_index;
    uint32_t src_x, src_y;
    int32_t  free_slot;
};
std::vector<WriteFromSolverCall> g_write_from_solver_calls;
void                             rec_path_write_from_solver(uint32_t player, int32_t unit_index, uint32_t src_x,
                                                            uint32_t src_y, int32_t free_slot) {
    tr("path_write_from_solver");
    g_write_from_solver_calls.push_back({player, unit_index, src_x, src_y, free_slot});
}

std::vector<int32_t> g_find_slot_queue;
size_t               g_find_slot_pos = 0;
struct FindSlotCall {
    int32_t heading, turn_delta;
};
std::vector<FindSlotCall> g_find_slot_calls;
int32_t                   rec_heading_candidate_find_slot(int32_t heading, int32_t turn_delta) {
    tr("heading_candidate_find_slot");
    g_find_slot_calls.push_back({heading, turn_delta});
    return (g_find_slot_pos < g_find_slot_queue.size()) ? g_find_slot_queue[g_find_slot_pos++] : -1;
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

// Simulates the real callee's map-side effect (stamping the unit's own x/y) so downstream code that
// RE-READS the roster (path_write_from_solver) observes the POST-relocation tile, exactly the
// behaviour the .cpp/header call out by name.
unit *g_units_base = nullptr;
struct PutOnMapCall {
    uint16_t player, b_id;
    uint8_t  x, y;
};
std::vector<PutOnMapCall> g_put_on_map_calls;
void                      rec_map_unit_PutOnMap(uint16_t player, uint16_t b_id, uint8_t x, uint8_t y) {
    tr("map_unit_PutOnMap");
    g_put_on_map_calls.push_back({player, b_id, x, y});
    if (g_units_base != nullptr) {
        unit &target = g_units_base[(size_t)player * (size_t)UNITS_PER_PLAYER + (size_t)b_id];
        target.x     = x;
        target.y     = y;
    }
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

const unit_group_step_ground_calls g_calls = {
    &rec_unit_get_coords,
    &rec_bldg_get_coords,
    &rec_unit_set_state_order,
    &rec_unit_set_state,
    &rec_unit_set_state_of,
    &rec_unit_calc_range_approach_point,
    &rec_target_release_ref,
    &rec_group_scratch_add_unit_and_normalize_heading,
    &rec_group_scratch_compute_centroid,
    &rec_unit_get_ready_home_building,
    &rec_trace_greedy_path,
    &rec_pathtrace_remove_loops,
    &rec_path_find_free_slot,
    &rec_path_free_slot,
    &rec_path_write_from_solver,
    &rec_heading_candidate_find_slot,
    &rec_unit_unlink_tile,
    &rec_fow_remove_sight,
    &rec_map_unit_PutOnMap,
    &rec_map_fow_UpdateFoWPlus,
    &rec_unit_notify_status,
};

void reset_observations() {
    g_trace.clear();
    g_get_coords_calls.clear();
    g_bldg_coords_calls.clear();
    g_set_state_order_calls.clear();
    g_set_state_calls.clear();
    g_set_state_of_calls.clear();
    g_approach_calls = 0;
    g_approach_ret   = 0;
    g_approach_col   = 0;
    g_approach_row   = 0;
    g_release_calls.clear();
    g_scratch_add_calls.clear();
    g_scratch_add_increment_queue.clear();
    g_scratch_add_increment_pos = 0;
    g_centroid_calls.clear();
    g_centroid_x          = 0;
    g_centroid_y          = 0;
    g_home_building_calls = 0;
    g_home_building_ret   = 0;
    g_trace_greedy_path_calls.clear();
    g_remove_loops_calls = 0;
    g_free_slot_calls.clear();
    g_free_slot_queue.clear();
    g_free_slot_pos = 0;
    g_path_free_calls.clear();
    g_write_from_solver_calls.clear();
    g_find_slot_calls.clear();
    g_find_slot_queue.clear();
    g_find_slot_pos = 0;
    g_unlink_calls.clear();
    g_fow_remove_calls.clear();
    g_put_on_map_calls.clear();
    g_fow_update_calls.clear();
    g_notify_calls.clear();
    g_pathtrace_len_queue.clear();
    g_pathtrace_len_pos = 0;
    g_unit_coords_x     = 0;
    g_unit_coords_y     = 0;
    g_bldg_coords_x     = 0;
    g_bldg_coords_y     = 0;
}

// Fixed "guard" slot/building no test's own (player,index) ever touches -- seeded with sentinel
// nonzero data each run so a wrong-index write is observable, not just a plain zero (T12).
constexpr uint16_t GUARD_PLAYER     = 6;
constexpr int32_t  GUARD_INDEX      = 95;
constexpr int32_t  GUARD_BLDG_OWNER = 1;
constexpr int32_t  GUARD_BLDG_SLOT  = 77;

void seed_guard_slot(sim_fixture &fx) {
    unit &g          = fx.u(GUARD_PLAYER, GUARD_INDEX);
    g.order          = 0x44;
    g.state          = 0x55;
    g.move_heading   = 9;
    g.target_ref     = 0x37;
    g.target_index   = 0x28;
    g.x              = 11;
    g.y              = 22;
    g.goal_x         = 33;
    g.goal_y         = 44;
    g.path_slot_id   = 0x22;
    g.activity_clock = 7777.0;
    g.unit_proto_id  = 66;

    building &gb   = fx.b(GUARD_BLDG_OWNER, GUARD_BLDG_SLOT);
    gb.building_id = 88;
    gb.energy      = 999.0;
}

// heading_candidates[heading*3 + slot]'s two "shape" purposes are read from DIFFERENT fields of the
// SAME entry: cand_facing (slot 0 only) is the path-shape class used both to seed shape_class and to
// filter commit-pass members; turn_delta/start_col_delta/start_row_delta (any of the 3 slots) are the
// PLANNING candidate table. set_heading_shape sets slot 0's cand_facing (deactivating that heading's
// own candidate walk, matching seed_and_reset()'s -1-everywhere default); set_heading_cand activates
// one specific candidate slot for the planning walk.
void set_heading_shape(sim_fixture &fx, int32_t heading, int32_t cand_facing) {
    fx.heading_candidates[(size_t)(heading * 3)].cand_facing = cand_facing;
}
void set_heading_cand(sim_fixture &fx, int32_t heading, int32_t slot_idx, int32_t turn_delta, int32_t col_delta,
                      int32_t row_delta) {
    heading_slot &s   = fx.heading_candidates[(size_t)(heading * 3 + slot_idx)];
    s.turn_delta      = turn_delta;
    s.start_col_delta = col_delta;
    s.start_row_delta = row_delta;
}

// Resets the fixture, DEACTIVATES every heading-candidate slot (turn_delta = -1, the real sentinel --
// fx.reset() only ZEROES the table, and 0 is NOT the sentinel, so every default case would otherwise
// walk 3 phantom candidates), seeds the guard slot/building, and clears every recorder/control global.
// Call this FIRST in every test block, before setting up that block's own scenario and control queues.
void reset_and_seed(sim_fixture &fx) {
    fx.reset();
    for (auto &slot : fx.heading_candidates) {
        slot.cand_facing     = 0;
        slot.turn_delta      = -1;
        slot.start_col_delta = 0;
        slot.start_row_delta = 0;
    }
    seed_guard_slot(fx);
    reset_observations();
}

// Binds the recorder-side raw pointers this run's stubs need (the group-scratch write target, the
// pathtrace_len write target, and the roster base for map_unit_PutOnMap's simulated side effect),
// then invokes the function under test. Callers set up fx + the g_*_queue/g_*_ret control globals
// BEFORE calling this.
void run(sim_fixture &fx) {
    g_scratch_ptr       = fx.group_move_scratch.data();
    g_pathtrace_len_ptr = &fx.pathtrace_len;
    g_units_base        = fx.units.data();
    sim_store own       = fx.store();
    detail::unit_group_step_ground(fx.view(), own, g_calls);
}

// ---- the 3-member pass-1/2/3 scaffold shared by T10/T11a/T11c --------------------------------------
//
// self (cur_unit, index 3): move_heading=7, cand_facing[7*3]=1 -- ALWAYS processed in pass 1 (shape_class
// is DEFINED from self's own heading at pass-1 start, so self trivially matches). Non-relocate (heading
// 7's own candidate slots stay deactivated, so pass 1's `chosen` == 7 == self's own heading).
// member_A (index 10): move_heading=0, cand_facing[0*3]=2 -- mismatches pass 1's shape(1), so pass 1
// skips it; MATCHES pass 2's re-classed shape (1 -> heading0/shape2/next3). Non-relocate (heading 0's
// own candidates deactivated -> pass 2's chosen == cur_unit's re-classed heading(0) == member_A's own).
// member_B (index 17): move_heading=1, cand_facing[1*3]=3 -- mismatches pass 1(1) and pass 2(2);
// MATCHES pass 3's shape (next_shape=3 from the shape-1 case) once pass 3 sets cur_unit's move_heading
// to 1 per ITS OWN table (shape3 -> heading1, 0x00484390 -- NOT the same mapping as pass 2's own
// shape1->heading4 arm, even though both tables mention heading4/heading1 elsewhere; do not confuse
// them). Non-relocate for the same reason as the other two.
constexpr uint16_t T10_PLAYER     = 2;
constexpr int32_t  T10_SELF_INDEX = 3;
constexpr int32_t  T10_A_INDEX    = 10;
constexpr int32_t  T10_B_INDEX    = 17;

void seed_three_pass_group(sim_fixture &fx) {
    unit &u               = fx.u(T10_PLAYER, T10_SELF_INDEX);
    u.order               = 0x05; // neutral -- not ATTACK_*/LANDING_REQUEST
    u.unit_proto_id       = 10;
    fx.cfg_units[10].type = 1; // ground
    u.x                   = 50;
    u.y                   = 60;
    u.goal_x              = 80;
    u.goal_y              = 90;
    u.move_heading        = 7;
    u.path_slot_id        = 0xff;
    fx.cur_unit_ptr       = &u;
    fx.view_cur_player    = T10_PLAYER;
    fx.view_cur_index     = (uint16_t)T10_SELF_INDEX;

    unit &a         = fx.u(T10_PLAYER, T10_A_INDEX);
    a.state         = STATE_GROUP_STEP;
    a.order         = u.order;
    a.unit_proto_id = 10;
    a.goal_x        = 80;
    a.goal_y        = 90;
    a.move_heading  = 0;
    a.path_slot_id  = 0xff;

    unit &b         = fx.u(T10_PLAYER, T10_B_INDEX);
    b.state         = STATE_GROUP_STEP;
    b.order         = u.order;
    b.unit_proto_id = 10;
    b.goal_x        = 80;
    b.goal_y        = 90;
    b.move_heading  = 1;
    b.path_slot_id  = 0xff;

    set_heading_shape(fx, 7, 1); // pass 1's starting shape_class
    set_heading_shape(fx, 0, 2); // pass 2's new shape (shape1 -> heading0/shape2/next3)
    set_heading_shape(fx, 1, 3); // pass 3's shape (next_shape=3 -> heading1, per pass 3's OWN table)
}

} // namespace

void run_unit_group_step_ground_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- the air-mode-flag write (0x00483034): MOV dword [0x0066a9a4],0x0 -- unconditional, value
    // ZERO, and it happens BEFORE the ATTACK_* order check at 0x00483050. Proven via the BUILDING-branch
    // dead-target early return (order=ATTACK_BUILDING), which returns at 0x00484a0a having touched
    // nothing else global -- if the flag write happened AFTER the check (or not at all on this path),
    // the sentinel we seed would survive.
    // =================================================================================================
    {
        reset_and_seed(fx);
        const uint16_t player = 2;
        const int32_t  index  = 3;
        unit          &u      = fx.u(player, index);
        u.order               = ORDER_ATTACK_BUILDING;
        u.target_ref          = (int16_t)0x05; // bit 0x80 CLEAR -> BUILDING branch; owner = 5
        u.target_index        = 12;
        fx.cur_unit_ptr       = &u;
        fx.view_cur_player    = player;
        fx.view_cur_index     = (uint16_t)index;

        fx.b(5, 12).energy = 0.0;   // dead, exact boundary
        fx.u(5, 12).energy = 100.0; // decoy, alive -- the OPPOSITE arm, must NOT be consulted

        fx.pathfinder_air_mode_flag = 9; // sentinel, distinct from the write value 0
        run(fx);

        ck_eq((uint32_t)fx.pathfinder_air_mode_flag, 0u,
              "T1: pathfinder_air_mode_flag written to 0 unconditionally at entry (0x00483034), proven "
              "here on an early-return path that reaches this write and returns without any other code "
              "capable of resetting the sentinel");
        ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].new_order == UNIT_STATE_IDLE_SCATTER &&
               g_set_state_order_calls[0].new_state == UNIT_STATE_IDLE_SCATTER,
           "T1: target_ref&0x80 CLEAR -> BUILDING branch (0x004830a2 TEST/JZ 0x00483181); building "
           "energy==0.0 dead -- unit_set_state_order(IDLE_SCATTER,IDLE_SCATTER) (0x00483219-0x00483223)");
        ck(trace_eq({"unit_set_state_order"}), "T1: immediate return -- no other callee fires (0x00483228 JMP epilogue)");
    }

    // =================================================================================================
    // T2 -- the target re-validation gate (0x00483050-0x00483217): T2a pins the UNIT sub-arm's dead
    // boundary (target_ref&0x80 SET, ATTACK_UNIT), T2b the BUILDING sub-arm's dead boundary (bit
    // CLEAR, ATTACK_UNIT_RETURN -- proving the OTHER order code also enters the block), and T2c the
    // UNIT sub-arm's ALIVE path (unit_get_coords + the /32 truncating fine_to_tile recompute), using
    // the unit's own cfg type == A_PLANE to isolate it from the (separately tested, T3) standoff block.
    // =================================================================================================
    {
        reset_and_seed(fx);
        const uint16_t player = 2;
        const int32_t  index  = 3;
        unit          &u      = fx.u(player, index);
        u.order               = ORDER_ATTACK_UNIT;
        u.target_ref          = (int16_t)0x85; // bit 0x80 SET -> UNIT branch; owner = 5
        u.target_index        = 12;
        fx.cur_unit_ptr       = &u;
        fx.view_cur_player    = player;
        fx.view_cur_index     = (uint16_t)index;

        fx.u(5, 12).energy = 0.0;   // dead, exact boundary
        fx.b(5, 12).energy = 100.0; // decoy, alive -- opposite arm, must NOT be consulted
        run(fx);

        ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].new_order == UNIT_STATE_IDLE_SCATTER,
           "T2a: target_ref&0x80 SET -> UNIT branch (0x004830a2 TEST/JZ, fall-through); unit energy==0.0 "
           "dead -- unit_set_state_order(IDLE_SCATTER,IDLE_SCATTER) (0x00483168-0x00483172)");
        ck(trace_eq({"unit_set_state_order"}), "T2a: immediate return (0x00483177 JMP epilogue)");
    }
    {
        reset_and_seed(fx);
        const uint16_t player = 2;
        const int32_t  index  = 3;
        unit          &u      = fx.u(player, index);
        u.order               = ORDER_ATTACK_UNIT_RETURN; // the OTHER order code that reaches this block
        u.target_ref          = (int16_t)0x05;            // bit 0x80 CLEAR -> BUILDING branch; owner = 5
        u.target_index        = 12;
        fx.cur_unit_ptr       = &u;
        fx.view_cur_player    = player;
        fx.view_cur_index     = (uint16_t)index;

        fx.b(5, 12).energy = 0.0;   // dead, exact boundary
        fx.u(5, 12).energy = 100.0; // decoy -- opposite arm, must NOT be consulted
        run(fx);

        ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].new_order == UNIT_STATE_IDLE_SCATTER,
           "T2b: order==ATTACK_UNIT_RETURN also reaches the block (0x0048305c/0x00483066); "
           "target_ref&0x80 CLEAR -> BUILDING branch; building energy==0.0 dead -- IDLE_SCATTER "
           "(0x00483219-0x00483223)");
        ck(trace_eq({"unit_set_state_order"}), "T2b: immediate return (0x00483228 JMP epilogue)");
    }
    {
        reset_and_seed(fx);
        const uint16_t player  = 2;
        const int32_t  index   = 3;
        const uint16_t cfg_row = 10;
        unit          &u       = fx.u(player, index);
        u.order                = ORDER_ATTACK_UNIT;
        u.target_ref           = (int16_t)0x83; // bit 0x80 SET -> UNIT branch; owner = 3
        u.target_index         = 7;
        u.unit_proto_id        = cfg_row;
        u.move_heading         = 5;
        fx.cur_unit_ptr        = &u;
        fx.view_cur_player     = player;
        fx.view_cur_index      = (uint16_t)index;

        fx.u(3, 7).energy          = 50.0;              // alive
        fx.cfg_units[cfg_row].type = UNIT_TYPE_A_PLANE; // isolates this case from the standoff block (T3)
        g_unit_coords_x            = -65;               // -65/32 = -2 (truncating toward zero)
        g_unit_coords_y            = 97;                //  97/32 =  3
        g_free_slot_queue          = {-1};              // bail right after group assembly -- keeps this case short
        run(fx);

        ck(g_release_calls.empty() && g_set_state_order_calls.empty(), "T2c: energy(50.0) > 0.0 -- no dead-target arm");
        ck(g_get_coords_calls.size() == 1 && g_get_coords_calls[0].player == (uint16_t)3 &&
               g_get_coords_calls[0].unit_index == 7,
           "T2c: llm_strat_unit_get_coords(target_owner=3, target_slot=7) (0x0048310f)");
        ck_eq((uint32_t)u.target_fine_x, (uint32_t)(int32_t)-65, "T2c: target_fine_x = -65 (out-param write)");
        ck_eq((uint32_t)u.target_fine_y, 97u, "T2c: target_fine_y = 97 (out-param write)");
        ck_eq((uint32_t)u.goal_x, 254u,
              "T2c: goal_x = (uint8_t)ground_fine_to_tile(-65) = (uint8_t)(-2) = 254 -- TRUNCATING "
              "divide toward zero, not a floor shift (0x00483114-0x00483137)");
        ck_eq((uint32_t)u.goal_y, 3u, "T2c: goal_y = ground_fine_to_tile(97) = 3 (0x00483143-0x00483160)");
        ck_eq((uint32_t)g_approach_calls, 0u,
              "T2c: cfg type==A_PLANE -- unit_calc_range_approach_point is NEVER called (0x0048325d/"
              "0x0048328f route straight to 0x00483390)");
    }

    // =================================================================================================
    // T3 -- the standoff block (0x0048322d-0x0048338b): the plane/heli skip (both A_PLANE and H_PLANE),
    // the GROUND calc_range_approach_point 0-return give-up arm, and its nonzero-return arm's two
    // exits (already-on-tile HOVER_ENGAGE vs fall-through to group assembly).
    // =================================================================================================
    for (uint8_t plane_type : {UNIT_TYPE_A_PLANE, UNIT_TYPE_H_PLANE}) {
        reset_and_seed(fx);
        const uint16_t player  = 2;
        const int32_t  index   = 3;
        const uint16_t cfg_row = 10;
        unit          &u       = fx.u(player, index);
        u.order                = ORDER_ATTACK_UNIT;
        u.target_ref           = (int16_t)0x83;
        u.target_index         = 7;
        u.unit_proto_id        = cfg_row;
        fx.cur_unit_ptr        = &u;
        fx.view_cur_player     = player;
        fx.view_cur_index      = (uint16_t)index;

        fx.u(3, 7).energy          = 50.0;
        fx.cfg_units[cfg_row].type = plane_type;
        g_free_slot_queue          = {-1};
        run(fx);
        ck_eq((uint32_t)g_approach_calls, 0u,
              "T3a: cfg type==0x11/0x12 (A_PLANE/H_PLANE) -- unit_calc_range_approach_point never "
              "called (0x0048325d JZ / 0x0048328f JNZ, both routing to 0x00483390)");
    }
    {
        reset_and_seed(fx);
        const uint16_t player  = 2;
        const int32_t  index   = 3;
        const uint16_t cfg_row = 10;
        unit          &u       = fx.u(player, index);
        u.order                = ORDER_ATTACK_UNIT;
        u.target_ref           = (int16_t)0x83;
        u.target_index         = 7;
        u.unit_proto_id        = cfg_row;
        fx.cur_unit_ptr        = &u;
        fx.view_cur_player     = player;
        fx.view_cur_index      = (uint16_t)index;

        fx.u(3, 7).energy          = 50.0;
        fx.cfg_units[cfg_row].type = 1; // ground -- NOT plane/heli
        g_free_slot_queue          = {-1};
        run(fx);
        ck_eq((uint32_t)g_approach_calls, 1u, "T3a: ground cfg type -- unit_calc_range_approach_point IS called (0x004832ba)");
    }
    {
        // T3b -- approach_ret==0 (no reachable stand-off tile): give up.
        reset_and_seed(fx);
        const uint16_t player  = 2;
        const int32_t  index   = 3;
        const uint16_t cfg_row = 10;
        unit          &u       = fx.u(player, index);
        u.order                = ORDER_ATTACK_UNIT;
        u.target_ref           = (int16_t)0x83;
        u.target_index         = 7;
        u.unit_proto_id        = cfg_row;
        fx.cur_unit_ptr        = &u;
        fx.view_cur_player     = player;
        fx.view_cur_index      = (uint16_t)index;

        fx.u(3, 7).energy                     = 50.0;
        fx.cfg_units[cfg_row].type            = 1;
        fx.cfg_units[cfg_row].default_op_code = 0x66; // sentinel, distinct from every literal here
        g_approach_ret                        = 0;
        run(fx);

        ck(g_release_calls.size() == 1 && g_release_calls[0].player_idx == player &&
               g_release_calls[0].unit_idx == index && g_release_calls[0].mode == 1u,
           "T3b: approach_ret==0 -- target_release_ref(cur_player, cur_index, mode=1) (0x00483326-0x0048333b)");
        ck_eq((uint32_t)(uint16_t)u.target_ref, 0u, "T3b: target_ref cleared (0x00483343-0x0048334c)");
        ck_eq((uint32_t)(uint16_t)u.target_index, 0u, "T3b: target_index cleared (0x00483351-0x0048335a)");
        ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].new_order == 0x66 &&
               g_set_state_order_calls[0].new_state == 0x66,
           "T3b: unit_set_state_order(default_op_code, default_op_code) -- the SAME value passed TWICE "
           "(0x00483363-0x00483386)");
        ck(trace_eq({"unit_get_coords", "unit_calc_range_approach_point", "target_release_ref", "unit_set_state_order"}),
           "T3b: exact call order, immediate return (0x0048338b JMP epilogue) -- no group-assembly calls");
    }
    {
        // T3c -- approach_ret!=0, already standing on the approach tile: HOVER_ENGAGE, immediate return.
        reset_and_seed(fx);
        const uint16_t player  = 2;
        const int32_t  index   = 3;
        const uint16_t cfg_row = 10;
        unit          &u       = fx.u(player, index);
        u.order                = ORDER_ATTACK_UNIT;
        u.target_ref           = (int16_t)0x83;
        u.target_index         = 7;
        u.unit_proto_id        = cfg_row;
        u.x                    = 40;
        u.y                    = 50;
        fx.cur_unit_ptr        = &u;
        fx.view_cur_player     = player;
        fx.view_cur_index      = (uint16_t)index;

        fx.u(3, 7).energy          = 50.0;
        fx.cfg_units[cfg_row].type = 1;
        g_approach_ret             = 1;
        g_approach_col             = 40; // SAME as u.x/u.y -- already on the tile
        g_approach_row             = 50;
        run(fx);

        ck_eq((uint32_t)u.goal_x, 40u, "T3c: goal_x committed to the approach tile (0x004832cc)");
        ck_eq((uint32_t)u.goal_y, 50u, "T3c: goal_y committed to the approach tile (0x004832db)");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_HOVER_ENGAGE,
           "T3c: already on the approach tile -- unit_set_state(HOVER_ENGAGE) (0x0048331a)");
        ck(trace_eq({"unit_get_coords", "unit_calc_range_approach_point", "unit_set_state"}),
           "T3c: immediate return (0x0048331f JMP epilogue) -- no group-assembly calls");
    }
    {
        // T3d -- approach_ret!=0, NOT on the approach tile: falls through into group assembly.
        reset_and_seed(fx);
        const uint16_t player  = 2;
        const int32_t  index   = 3;
        const uint16_t cfg_row = 10;
        unit          &u       = fx.u(player, index);
        u.order                = ORDER_ATTACK_UNIT;
        u.target_ref           = (int16_t)0x83;
        u.target_index         = 7;
        u.unit_proto_id        = cfg_row;
        u.x                    = 40;
        u.y                    = 50;
        fx.cur_unit_ptr        = &u;
        fx.view_cur_player     = player;
        fx.view_cur_index      = (uint16_t)index;

        fx.u(3, 7).energy          = 50.0;
        fx.cfg_units[cfg_row].type = 1;
        g_approach_ret             = 1;
        g_approach_col             = 41; // DIFFERENT from u.x/u.y -- not on the tile
        g_approach_row             = 60;
        g_free_slot_queue          = {-1}; // bail right after assembly registers cur_unit
        run(fx);

        ck_eq((uint32_t)u.goal_x, 41u, "T3d: goal_x committed even though the unit is not yet there (0x004832cc)");
        ck_eq((uint32_t)u.goal_y, 60u, "T3d: goal_y committed (0x004832db)");
        ck(g_set_state_calls.empty(), "T3d: NOT already on tile -- unit_set_state(HOVER_ENGAGE) does not fire (0x004832f8/0x00483311)");
        ck(!g_scratch_add_calls.empty() && g_scratch_add_calls[0].unit_index == index,
           "T3d: falls through past the standoff block into group assembly -- cur_unit registered via "
           "group_scratch_add_unit_and_normalize_heading (0x004833dd)");
    }

    // =================================================================================================
    // T4 -- group assembly (0x004833ed-0x00483593): every roster-scan filter broken one at a time plus
    // a positive control, the LANDING_REQUEST self-exclusion, the same-attack-order self-exclusion, and
    // the group_count==100 cap-and-stop.
    // =================================================================================================
    {
        reset_and_seed(fx);
        const uint16_t player      = 2;
        const int32_t  index       = 3;
        const uint16_t cfg_row     = 10;
        unit          &u           = fx.u(player, index);
        u.order                    = 0x05; // neutral
        u.unit_proto_id            = cfg_row;
        fx.cfg_units[cfg_row].type = 1;
        u.goal_x                   = 80;
        u.goal_y                   = 90;
        fx.cur_unit_ptr            = &u;
        fx.view_cur_player         = player;
        fx.view_cur_index          = (uint16_t)index;

        auto other = [&](int32_t i) -> unit & { return fx.u(player, i); };

        // index+1: wrong state -> excluded (0x00483416).
        other(index + 1).state         = 0x00;
        other(index + 1).order         = u.order;
        other(index + 1).unit_proto_id = cfg_row;
        other(index + 1).goal_x        = 80;
        other(index + 1).goal_y        = 90;

        // index+2: right state, wrong order -> excluded (0x00483443).
        other(index + 2).state         = STATE_GROUP_STEP;
        other(index + 2).order         = (uint16_t)(u.order + 1);
        other(index + 2).unit_proto_id = cfg_row;
        other(index + 2).goal_x        = 80;
        other(index + 2).goal_y        = 90;

        // index+3: right state/order, wrong type (plane) -> excluded (0x00483490/0x004834bc).
        other(index + 3).state         = STATE_GROUP_STEP;
        other(index + 3).order         = u.order;
        other(index + 3).unit_proto_id = 20;
        fx.cfg_units[20].type          = UNIT_TYPE_A_PLANE;
        other(index + 3).goal_x        = 80;
        other(index + 3).goal_y        = 90;

        // index+4: right so far, goal_x mismatch -> excluded (0x00483547).
        other(index + 4).state         = STATE_GROUP_STEP;
        other(index + 4).order         = u.order;
        other(index + 4).unit_proto_id = cfg_row;
        other(index + 4).goal_x        = 81;
        other(index + 4).goal_y        = 90;

        // index+5: right so far, goal_y mismatch -> excluded (0x0048356d).
        other(index + 5).state         = STATE_GROUP_STEP;
        other(index + 5).order         = u.order;
        other(index + 5).unit_proto_id = cfg_row;
        other(index + 5).goal_x        = 80;
        other(index + 5).goal_y        = 91;

        // index+6: POSITIVE CONTROL -- passes every filter.
        other(index + 6).state         = STATE_GROUP_STEP;
        other(index + 6).order         = u.order;
        other(index + 6).unit_proto_id = cfg_row;
        other(index + 6).goal_x        = 80;
        other(index + 6).goal_y        = 90;

        g_free_slot_queue = {-1}; // bail on self's own commit -- isolates this test to assembly
        run(fx);

        ck_eq((uint32_t)g_scratch_add_calls.size(), 2u,
              "T4: exactly TWO registrations -- self (unconditional, step 3, 0x004833dd) plus ONLY the "
              "positive control (0x0048358a); the five deliberately-broken candidates are all excluded");
        ck(g_scratch_add_calls[0].unit_index == index, "T4: self registered FIRST (0x004833dd)");
        ck(g_scratch_add_calls[1].unit_index == index + 6,
           "T4: the positive control (index+6) is the only later unit registered -- state==GROUP_STEP && "
           "order matches && order!=LANDING_REQUEST && type not plane/heli && order not ATTACK_BUILDING/"
           "ATTACK_UNIT/ATTACK_UNIT_RETURN && goal_x/goal_y both match");
    }
    {
        // T4-landing: cur_unit.order==LANDING_REQUEST composes with filter #2 (order match) so any
        // matching later candidate ALSO has order==0x29, which filter #3 then unconditionally excludes.
        reset_and_seed(fx);
        const uint16_t player      = 2;
        const int32_t  index       = 3;
        const uint16_t cfg_row     = 10;
        unit          &u           = fx.u(player, index);
        u.order                    = ORDER_LANDING_REQUEST;
        u.unit_proto_id            = cfg_row;
        fx.cfg_units[cfg_row].type = 1;
        u.goal_x                   = 80;
        u.goal_y                   = 90;
        fx.cur_unit_ptr            = &u;
        fx.view_cur_player         = player;
        fx.view_cur_index          = (uint16_t)index;

        unit &other         = fx.u(player, index + 1);
        other.state         = STATE_GROUP_STEP;
        other.order         = ORDER_LANDING_REQUEST; // == cur_unit.order (filter #2 passes)
        other.unit_proto_id = cfg_row;
        other.goal_x        = 80;
        other.goal_y        = 90;

        g_free_slot_queue = {-1};
        run(fx);
        ck_eq((uint32_t)g_scratch_add_calls.size(), 1u,
              "T4-landing: filter #3 (order != LANDING_REQUEST, 0x00483461/0x00483469) excludes the "
              "candidate even though filter #2 matched -- a LANDING_REQUEST-order driver unit can never "
              "gather a later group member via this scan; only self is registered");
    }
    {
        // T4-attack: cur_unit.order==ATTACK_UNIT composes with filter #2 the same way -- a later unit
        // sharing that same attack order is excluded by filter #5.
        reset_and_seed(fx);
        const uint16_t player      = 2;
        const int32_t  index       = 3;
        const uint16_t cfg_row     = 10;
        unit          &u           = fx.u(player, index);
        u.order                    = ORDER_ATTACK_UNIT;
        u.target_ref               = (int16_t)0x83;
        u.target_index             = 7;
        u.unit_proto_id            = cfg_row;
        fx.cfg_units[cfg_row].type = UNIT_TYPE_A_PLANE; // skip the standoff block cleanly
        fx.u(3, 7).energy          = 50.0;
        g_unit_coords_x            = 80 * 32; // exact, so goal recompute lands on (80,90)
        g_unit_coords_y            = 90 * 32;
        fx.cur_unit_ptr            = &u;
        fx.view_cur_player         = player;
        fx.view_cur_index          = (uint16_t)index;

        unit &other           = fx.u(player, index + 1);
        other.state           = STATE_GROUP_STEP;
        other.order           = ORDER_ATTACK_UNIT; // == cur_unit.order
        other.unit_proto_id   = 20;
        fx.cfg_units[20].type = 1; // ground -- passes the type filter
        other.goal_x          = 80;
        other.goal_y          = 90;

        g_free_slot_queue = {-1};
        run(fx);
        ck_eq((uint32_t)g_scratch_add_calls.size(), 1u,
              "T4-attack: filter #5 (order not one of ATTACK_BUILDING/ATTACK_UNIT/ATTACK_UNIT_RETURN, "
              "0x004834e0/0x00483500) excludes a later unit sharing cur_unit's OWN attack order; only "
              "self is registered");
    }
    {
        // T4-cap: the group_count==100 EQUALITY check (0x0048358f).
        reset_and_seed(fx);
        const uint16_t player      = 2;
        const int32_t  index       = 0;
        const uint16_t cfg_row     = 10;
        unit          &u           = fx.u(player, index);
        u.order                    = 0x05;
        u.unit_proto_id            = cfg_row;
        fx.cfg_units[cfg_row].type = 1;
        u.goal_x                   = 80;
        u.goal_y                   = 90;
        fx.cur_unit_ptr            = &u;
        fx.view_cur_player         = player;
        fx.view_cur_index          = (uint16_t)index;

        for (int32_t k = 1; k <= 5; ++k) {
            unit &o         = fx.u(player, k);
            o.state         = STATE_GROUP_STEP;
            o.order         = u.order;
            o.unit_proto_id = cfg_row;
            o.goal_x        = 80;
            o.goal_y        = 90;
        }
        // TEST-ONLY device: simulate a callee that bumps group_count by 33 per LATER-match
        // registration (real llm_strat_group_scratch_add_unit_and_normalize_heading always bumps by
        // exactly 1) so the count reaches EXACTLY 100 after the 3rd later match (1 self + 33*3 = 100)
        // with a small, hand-checkable roster instead of needing 99 real later units -- exercises the
        // `==0x64` EQUALITY check (0x0048358f), not a claim about the real callee's increment. The
        // queue is PER CALL: self's own unconditional step-3 registration (0x004833dd) is the FIRST
        // call into the recorder and keeps the real +1 (the "1" half of "1+33*3"); only the THREE
        // later-scan registrations that follow it bump by 33.
        g_scratch_add_increment_queue = {1, 33, 33, 33};
        g_free_slot_queue             = {-1};
        run(fx);

        ck_eq((uint32_t)g_scratch_add_calls.size(), 4u,
              "T4-cap: self + exactly 3 later matches -- group_count hits 100 exactly on the 3rd "
              "(1+33*3=100), and the scan STOPS there (0x0048358f CMP ...,0x64 / JNZ) even though 2 "
              "more valid candidates (k=4,5) were seeded and never reached");
        ck(g_scratch_add_calls[3].unit_index == 3,
           "T4-cap: the member whose insert PUSHES the count to exactly 100 is STILL registered -- the "
           "check is on the callee's OWN already-bumped result, not a rejection of that insert");
    }

    // =================================================================================================
    // T5 -- the centroid call (0x00483599-0x004835b7) OVERWRITES start_col/start_row before planning.
    // =================================================================================================
    {
        reset_and_seed(fx);
        const uint16_t player      = 2;
        const int32_t  index       = 3;
        const uint16_t cfg_row     = 10;
        unit          &u           = fx.u(player, index);
        u.order                    = 0x05;
        u.unit_proto_id            = cfg_row;
        fx.cfg_units[cfg_row].type = 1;
        u.x                        = 15;
        u.y                        = 25;
        u.goal_x                   = 80;
        u.goal_y                   = 90;
        u.move_heading             = 6;
        fx.cur_unit_ptr            = &u;
        fx.view_cur_player         = player;
        fx.view_cur_index          = (uint16_t)index;

        g_centroid_x      = 200; // DISTINCT from u.x(15)
        g_centroid_y      = 210; // DISTINCT from u.y(25)
        g_free_slot_queue = {-1};
        run(fx);

        ck(g_centroid_calls.size() == 1 && g_centroid_calls[0].player == player &&
               g_centroid_calls[0].member_count == 1,
           "T5: group_scratch_compute_centroid(cur_player, group_count=1) (0x004835a9)");
        ck(!g_trace_greedy_path_calls.empty() && g_trace_greedy_path_calls[0].start_col == 200 &&
               g_trace_greedy_path_calls[0].start_row == 210,
           "T5: pass 1's FIRST trace_greedy_path call starts from the CENTROID (200,210, 0x004835ae/"
           "0x004835b7), NOT cur_unit's own tile (15,25) -- the centroid OVERWRITES start_col/start_row "
           "before planning begins");
    }

    // =================================================================================================
    // T6 -- the LANDING_REQUEST facing_override arm (0x004835ba-0x004835fd), both sides.
    // =================================================================================================
    {
        reset_and_seed(fx);
        const uint16_t player      = 2;
        const int32_t  index       = 3;
        const uint16_t cfg_row     = 10;
        unit          &u           = fx.u(player, index);
        u.order                    = ORDER_LANDING_REQUEST;
        u.unit_proto_id            = cfg_row;
        fx.cfg_units[cfg_row].type = 1;
        u.goal_x                   = 80;
        u.goal_y                   = 90;
        fx.cur_unit_ptr            = &u;
        fx.view_cur_player         = player;
        fx.view_cur_index          = (uint16_t)index;

        g_home_building_ret = 0;
        g_free_slot_queue   = {-1};
        run(fx);

        ck_eq((uint32_t)g_home_building_calls, 1u, "T6a: unit_get_ready_home_building() called (0x004835c6)");
        ck(!g_trace_greedy_path_calls.empty() && g_trace_greedy_path_calls[0].heading == 0xff,
           "T6a: home_building_idx==0 -- facing_override stays at its 0xff default (0x004835d2 JZ taken)");
    }
    {
        reset_and_seed(fx);
        const uint16_t player      = 2;
        const int32_t  index       = 3;
        const uint16_t cfg_row     = 10;
        unit          &u           = fx.u(player, index);
        u.order                    = ORDER_LANDING_REQUEST;
        u.unit_proto_id            = cfg_row;
        fx.cfg_units[cfg_row].type = 1;
        u.goal_x                   = 80;
        u.goal_y                   = 90;
        fx.cur_unit_ptr            = &u;
        fx.view_cur_player         = player;
        fx.view_cur_index          = (uint16_t)index;

        g_home_building_ret                         = 4;
        fx.b(player, 4).building_id                 = 55;
        fx.cfg_buildings[55].door_approach_route[0] = 19; // distinct sentinel
        g_free_slot_queue                           = {-1};
        run(fx);

        ck(!g_trace_greedy_path_calls.empty() && g_trace_greedy_path_calls[0].heading == 19,
           "T6b: facing_override = cfg_buildings[buildings[player][4].building_id=55].facing = 19 "
           "(0x004835d4-0x004835fd)");
    }

    // =================================================================================================
    // T7 -- pass 1 in isolation: T7a the planning candidate walk + tie-break; T7b the commit
    // NON-relocate branch; T7c the commit RELOCATE branch (full sequence + activity_clock cross-check
    // + path_slot_id reassignment), both members in ONE pass so call order is pinned too.
    // =================================================================================================
    {
        // T7a -- candidate selection: shortest wins, ties keep the EARLIER candidate.
        reset_and_seed(fx);
        const uint16_t player      = 2;
        const int32_t  index       = 3;
        const uint16_t cfg_row     = 10;
        unit          &u           = fx.u(player, index);
        u.order                    = 0x05;
        u.unit_proto_id            = cfg_row;
        fx.cfg_units[cfg_row].type = 1;
        u.x                        = 50;
        u.y                        = 60;
        u.goal_x                   = 80;
        u.goal_y                   = 90;
        u.move_heading             = 7;
        fx.cur_unit_ptr            = &u;
        fx.view_cur_player         = player;
        fx.view_cur_index          = (uint16_t)index;

        // The group here is self ALONE, so a real group_scratch_compute_centroid call would return
        // self's own tile -- but T5 already proves that call's out-params OVERWRITE start_col/
        // start_row before planning begins (0x004835ae/0x004835b7), so the mock must be told to
        // return (50,60) explicitly; leaving it at reset's default (0,0) would silently start every
        // trace from the wrong tile (that was this test's own bug, not a production one).
        g_centroid_x = 50;
        g_centroid_y = 60;

        set_heading_shape(fx, 7, 1);
        set_heading_cand(fx, 7, 0, /*turn_delta=*/11, /*col=*/1, /*row=*/0);
        set_heading_cand(fx, 7, 1, /*turn_delta=*/22, /*col=*/0, /*row=*/1);
        set_heading_cand(fx, 7, 2, /*turn_delta=*/33, /*col=*/-1, /*row=*/-1);

        g_pathtrace_len_queue = {100, 100, 50, 50}; // straight, cand0(no improve), cand1(wins), cand2(tie)
        g_free_slot_queue     = {-1};               // bail right after planning -- isolates this case to pass_plan
        run(fx);

        ck_eq((uint32_t)g_trace_greedy_path_calls.size(), 5u,
              "T7a: straight + 3 candidates + the re-run of the winner == 5 calls total");
        const auto &straight = g_trace_greedy_path_calls[0];
        ck(straight.start_col == 50 && straight.start_row == 60 && straight.mode == 7 &&
               straight.goal_col == 80 && straight.goal_row == 90,
           "T7a: the straight-ahead reference trace (start=own tile, mode=own heading) (0x00483648)");
        ck(g_trace_greedy_path_calls[1].mode == 11 && g_trace_greedy_path_calls[2].mode == 22 &&
               g_trace_greedy_path_calls[3].mode == 33,
           "T7a: the 3 candidate traces use each slot's OWN turn_delta as `mode`, VERBATIM (0x0048370f)");
        const auto &rerun = g_trace_greedy_path_calls[4];
        ck(rerun.start_col == 50 && rerun.start_row == 61 && rerun.mode == 22,
           "T7a: candidate 1 (len 50) wins over candidate 0 (len 100, no improvement) -- the WINNER is "
           "RE-RUN with its own delta-adjusted start tile (50+0, 60+1) and its own turn_delta as mode "
           "(0x00483744-0x00483867); candidate 2's EQUAL length (50) does NOT overwrite it (the compare "
           "is unsigned STRICT `>`, 0x00483720-0x00483736 JBE) -- ties keep the EARLIER candidate");
    }
    {
        // T7b -- commit, NON-relocate branch (heading already == chosen).
        reset_and_seed(fx);
        const uint16_t player              = 2;
        const int32_t  index               = 3;
        const uint16_t cfg_row             = 10;
        unit          &u                   = fx.u(player, index);
        u.order                            = 0x05;
        u.unit_proto_id                    = cfg_row;
        fx.cfg_units[cfg_row].type         = 1;
        fx.cfg_units[cfg_row].move_op_code = 0x44; // sentinel
        u.x                                = 50;
        u.y                                = 60;
        u.goal_x                           = 80;
        u.goal_y                           = 90;
        u.move_heading                     = 7;
        u.path_slot_id                     = 0xff; // "no path assigned" -- path_free_slot must NOT fire
        fx.cur_unit_ptr                    = &u;
        fx.view_cur_player                 = player;
        fx.view_cur_index                  = (uint16_t)index;

        // heading 7's candidates stay deactivated (reset_and_seed's default), so pass_plan's
        // best_cand stays -1 and chosen == u.move_heading(7) trivially -- the member (same record)
        // therefore has heading == chosen, forcing the non-relocate branch.
        g_free_slot_queue = {5}; // succeeds -- no bailout
        run(fx);

        ck(g_find_slot_calls.empty(),
           "T7b: heading already == chosen -- heading_candidate_find_slot is never even CALLED "
           "(the `!=` guard short-circuits before it, 0x00483999/0x0048399c)");
        ck(g_unlink_calls.empty() && g_fow_remove_calls.empty() && g_put_on_map_calls.empty() &&
               g_fow_update_calls.empty(),
           "T7b: non-relocate branch -- NONE of the relocation callees fire");
        ck(g_set_state_of_calls.size() == 1 && g_set_state_of_calls[0].player == player &&
               g_set_state_of_calls[0].unit_index == index && g_set_state_of_calls[0].new_state == 0x44,
           "T7b: unit_set_state_of(player, member, cfg_units[proto].move_op_code=0x44) (0x00483b7a-"
           "0x00483bb4)");
        ck_eq((uint32_t)u.move_heading, 7u, "T7b: move_heading UNCHANGED (non-relocate never writes it)");
        ck(g_write_from_solver_calls.size() == 1 && g_write_from_solver_calls[0].player == player &&
               g_write_from_solver_calls[0].unit_index == index && g_write_from_solver_calls[0].src_x == 50 &&
               g_write_from_solver_calls[0].src_y == 60 && g_write_from_solver_calls[0].free_slot == 5,
           "T7b: path_write_from_solver reads x/y BACK FROM THE ROSTER (50,60, unchanged since no "
           "relocation) (0x00483bb9-0x00483c0a)");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].status_code == NOTIFY_PATH_COMPUTED,
           "T7b: unit_notify_status(player, member, 100) (0x00483c0f-0x00483c21)");
    }
    {
        // T7c -- commit, RELOCATE branch (member index10) alongside a non-relocate self (member index3),
        // in ONE pass-1 run, so exact call order + the activity_clock cross-stamp + the path_slot_id
        // free-then-reassign sub-branch are all pinned together.
        reset_and_seed(fx);
        const uint16_t player      = 2;
        const int32_t  self_index  = 3;
        const int32_t  other_index = 10;
        const uint16_t cfg_row     = 10;

        unit &u                     = fx.u(player, self_index);
        u.order                     = 0x05;
        u.unit_proto_id             = cfg_row;
        fx.cfg_units[cfg_row].type  = 1;
        fx.cfg_units[cfg_row].sight = 4;
        u.x                         = 50;
        u.y                         = 60;
        u.goal_x                    = 80;
        u.goal_y                    = 90;
        u.move_heading              = 7;
        u.activity_clock            = 321.25; // the source of `saved_activity_clock`
        u.path_slot_id              = 0xff;   // self stays non-relocate (fully proven in isolation by T7b)
        fx.cur_unit_ptr             = &u;
        fx.view_cur_player          = player;
        fx.view_cur_index           = (uint16_t)self_index;

        unit &o          = fx.u(player, other_index);
        o.state          = STATE_GROUP_STEP;
        o.order          = u.order;
        o.unit_proto_id  = cfg_row;
        o.goal_x         = 80; // matches -- registered by the later-unit scan
        o.goal_y         = 90;
        o.x              = 70;
        o.y              = 80;
        o.move_heading   = 12;    // DIFFERENT from chosen(7) -- forces relocate
        o.path_slot_id   = 9;     // != 0xff -- path_free_slot must fire first
        o.activity_clock = 555.0; // distinct from cur_unit's 321.25, to prove the CROSS-unit re-stamp

        set_heading_shape(fx, 7, 1);                                           // pass 1 shape_class
        set_heading_shape(fx, 12, 1);                                          // "other" must match the same shape_class
        set_heading_cand(fx, 12, 0, /*turn_delta=*/99, /*col=*/3, /*row=*/-2); // relocation delta source
        // heading 7's own candidates stay deactivated -> chosen == u.move_heading(7) trivially.

        g_find_slot_queue = {0};    // heading_candidate_find_slot returns candidate slot 0
        g_free_slot_queue = {5, 6}; // self, then other -- both succeed, no bailout

        run(fx);

        ck_eq_d(u.activity_clock, 321.25, "T7c: self's own activity_clock untouched by its own non-relocate commit");

        ck(g_find_slot_calls.size() == 1 && g_find_slot_calls[0].heading == 12 && g_find_slot_calls[0].turn_delta == 7,
           "T7c: heading_candidate_find_slot(member's ORIGINAL move_heading=12, chosen=7)");
        ck(g_path_free_calls.size() == 1 && g_path_free_calls[0].player == player &&
               g_path_free_calls[0].unit_index == other_index,
           "T7c: path_slot_id(9) != 0xff -- path_free_slot(player, member) fires BEFORE relocation "
           "(0x004838ff-0x00483930)");
        ck(g_unlink_calls.size() == 1 && g_unlink_calls[0].unit_index == (uint16_t)other_index,
           "T7c: unit_unlink_tile(player, member) (0x004839e2)");
        ck(g_fow_remove_calls.size() == 1 && g_fow_remove_calls[0].x == 70 && g_fow_remove_calls[0].y == 80 &&
               g_fow_remove_calls[0].radius == 4,
           "T7c: fow_remove_sight uses the OLD tile (70,80 = member's OWN x/y before relocation) and "
           "cfg_units[proto].sight=4 as radius (0x00483a21)");
        ck(g_put_on_map_calls.size() == 1 && g_put_on_map_calls[0].x == 73 && g_put_on_map_calls[0].y == 14,
           "T7c: map_unit_PutOnMap gets the delta-adjusted, MASK-WRAPPED new tile -- "
           "col=(70+3)&0xff=73, row=(80-2)&0x3f=14 (0x00483aca), TRUNCATED to bytes");
        ck(g_fow_update_calls.size() == 1 && g_fow_update_calls[0].x == 73 && g_fow_update_calls[0].y == 14 &&
               g_fow_update_calls[0].sight == 4,
           "T7c: map_fow_UpdateFoWPlus gets the SAME new tile as FULL dwords, not truncated (0x00483b0f)");
        ck_eq((uint32_t)o.move_heading, 7u, "T7c: member's move_heading committed to `chosen`(7) AFTER PutOnMap/FoW (0x00483b30)");
        ck(g_set_state_of_calls.size() == 2 && g_set_state_of_calls[1].unit_index == other_index &&
               g_set_state_of_calls[1].new_state == STATE_FORMATION_PLACED,
           "T7c: unit_set_state_of(player, member, FORMATION_PLACED=0x37) (0x00483b48)");
        ck_eq_d(o.activity_clock, 321.25,
                "T7c: member's activity_clock re-stamped to the DRIVER's captured snapshot (321.25), NOT "
                "its own original value (555.0) -- the CROSS-unit re-stamp (0x00483b4d-0x00483b78)");
        ck(g_write_from_solver_calls.size() == 2 && g_write_from_solver_calls[1].src_x == 73 &&
               g_write_from_solver_calls[1].src_y == 14,
           "T7c: path_write_from_solver's src_x/src_y are RE-READ FROM THE ROSTER post-relocation "
           "(73,14), NOT the pre-relocation start_col/start_row locals (70,80) (0x00483bb9-0x00483c0a)");
        ck(g_notify_calls.size() == 2 && g_notify_calls[1].unit_index == other_index,
           "T7c: unit_notify_status(player, member, 100) fires for the relocated member too (0x00483c21)");

        // Pass 1's plan is zero-candidate for self (heading 7's own slots stay deactivated -- only
        // heading 12's slot 0 is set, and that's the COMMIT-phase relocation-delta source, not a
        // planning candidate for heading 7). So, exactly as in T8a, the plan contributes TWO
        // trace_greedy_path/pathtrace_remove_loops pairs (straight + the unconditional re-run at
        // 0x0048377b), not one.
        ck(trace_eq({"group_scratch_add_unit_and_normalize_heading", "group_scratch_add_unit_and_normalize_heading",
                     "group_scratch_compute_centroid", "trace_greedy_path", "pathtrace_remove_loops",
                     "trace_greedy_path", "pathtrace_remove_loops",
                     "path_find_free_slot", "unit_set_state_of", "path_write_from_solver", "unit_notify_status",
                     "path_find_free_slot", "path_free_slot", "heading_candidate_find_slot", "unit_unlink_tile",
                     "fow_remove_sight", "map_unit_PutOnMap", "map_fow_UpdateFoWPlus", "unit_set_state_of",
                     "path_write_from_solver", "unit_notify_status"}),
           "T7c: exact call order across BOTH members' commits, and processed_count(2)==group_count(2) "
           "after pass 1 -- the function returns at 0x00483c31-0x00483c37 WITHOUT pass 2/3 ever starting");
    }

    // =================================================================================================
    // T8 -- the free_slot==-1 bail-out: T8a a plain pass-1 case (nothing after fires); T8b a pass-2
    // case, proving move_heading is left at the RE-CLASSED synthetic value, NOT restored.
    // =================================================================================================
    {
        reset_and_seed(fx);
        const uint16_t player      = 2;
        const int32_t  index       = 3;
        const uint16_t cfg_row     = 10;
        unit          &u           = fx.u(player, index);
        u.order                    = 0x05;
        u.unit_proto_id            = cfg_row;
        fx.cfg_units[cfg_row].type = 1;
        u.x                        = 50;
        u.y                        = 60;
        u.goal_x                   = 80;
        u.goal_y                   = 90;
        u.move_heading             = 7;
        fx.cur_unit_ptr            = &u;
        fx.view_cur_player         = player;
        fx.view_cur_index          = (uint16_t)index;

        g_free_slot_queue = {-1};
        run(fx);

        ck(g_set_state_of_calls.size() == 1 && g_set_state_of_calls[0].player == player &&
               g_set_state_of_calls[0].unit_index == index &&
               g_set_state_of_calls[0].new_state == (int16_t)UNIT_STATE_IDLE_SCATTER,
           "T8a: free_slot==-1 -- unit_set_state_of(player, member, IDLE_SCATTER) (0x004838e3-0x004838f5)");
        // Pass 1's plan is zero-candidate (heading 7's slots stay deactivated), but the "re-run the
        // winner" step ALWAYS issues a second trace_greedy_path/pathtrace_remove_loops pair even when
        // best_cand==-1 -- 0x00483744-0x0048378c: JNZ 0x00483748 (not taken here) falls through into
        // its OWN CALL 0x0066a9ac at 0x0048377b, distinct from the straight-ahead call at 0x00483648.
        ck(trace_eq({"group_scratch_add_unit_and_normalize_heading", "group_scratch_compute_centroid",
                     "trace_greedy_path", "pathtrace_remove_loops", "trace_greedy_path", "pathtrace_remove_loops",
                     "path_find_free_slot", "unit_set_state_of"}),
           "T8a: the WHOLE function returns immediately -- no path_write_from_solver/unit_notify_status, "
           "no pass 2/3 (0x004838fa JMP 0x00484a0a)");
    }
    {
        reset_and_seed(fx);
        const uint16_t player      = 2;
        const int32_t  self_index  = 3;
        const int32_t  other_index = 10;
        const uint16_t cfg_row     = 10;
        unit          &u           = fx.u(player, self_index);
        u.order                    = 0x05;
        u.unit_proto_id            = cfg_row;
        fx.cfg_units[cfg_row].type = 1;
        u.x                        = 50;
        u.y                        = 60;
        u.goal_x                   = 80;
        u.goal_y                   = 90;
        u.move_heading             = 7; // ORIGINAL (saved_heading)
        u.path_slot_id             = 0xff;
        fx.cur_unit_ptr            = &u;
        fx.view_cur_player         = player;
        fx.view_cur_index          = (uint16_t)self_index;

        unit &o         = fx.u(player, other_index);
        o.state         = STATE_GROUP_STEP;
        o.order         = u.order;
        o.unit_proto_id = cfg_row;
        o.goal_x        = 80;
        o.goal_y        = 90;
        o.move_heading  = 15; // does NOT match pass 1's shape(1) -- skipped in pass 1
        o.path_slot_id  = 0xff;

        set_heading_shape(fx, 7, 1);  // pass 1 shape_class
        set_heading_shape(fx, 0, 2);  // pass 2's NEW shape (1 -> heading0/shape2/next3)
        set_heading_shape(fx, 15, 2); // mismatches pass 1(1), MATCHES pass 2(2)
        // heading 0's own candidates stay deactivated -> pass 2's chosen == cur_unit.move_heading(now
        // 0) == 0; "other"'s own heading(15) != chosen(0), so it WOULD relocate -- but the bail-out
        // below fires before relocation ever starts.

        g_free_slot_queue = {5, -1}; // self succeeds in pass 1; "other" FAILS in pass 2
        run(fx);

        ck_eq((uint32_t)u.move_heading, 0u,
              "T8b: pass 2's re-class fires (shape 1 -> move_heading=0, 0x00483c66) and the bail-out "
              "inside pass 2's commit (free_slot==-1, 0x00483fcc JMP 0x00484a0a) skips the FINAL "
              "restore at 0x004849f5 -- the unit is left holding the SYNTHETIC heading (0), NOT the "
              "original saved_heading (7)");
        ck(g_set_state_of_calls.size() == 2 && g_set_state_of_calls[0].unit_index == self_index &&
               g_set_state_of_calls[1].unit_index == other_index &&
               g_set_state_of_calls[1].new_state == (int16_t)UNIT_STATE_IDLE_SCATTER,
           "T8b: self committed normally in pass 1, then \"other\" gets IDLE_SCATTER in pass 2's "
           "bail-out (0x00483fb5-0x00483fc7)");
        ck_eq((uint32_t)g_free_slot_calls.size(), 2u,
              "T8b: exactly 2 path_find_free_slot calls -- pass 3 never starts (a 3rd call would "
              "appear if it did)");
        ck_eq((uint32_t)g_notify_calls.size(), 1u,
              "T8b: only self's pass-1 commit reaches unit_notify_status -- other's pass-2 bail "
              "returns before it (0x00484a0a epilogue)");
    }

    // =================================================================================================
    // T9 -- pass 2's shape-class remap table (0x00483c4c-0x00483cb8), all three arms, plus the extra
    // direct `state` word write in its non-relocate commit sub-arm (0x00484275-0x00484296).
    // =================================================================================================
    {
        // T9a -- shape 1 -> heading0/shape2/next3, and the extra direct state write.
        reset_and_seed(fx);
        const uint16_t player              = 2;
        const int32_t  self_index          = 3;
        const int32_t  other_index         = 10;
        const uint16_t cfg_row             = 10;
        unit          &u                   = fx.u(player, self_index);
        u.order                            = 0x05;
        u.unit_proto_id                    = cfg_row;
        fx.cfg_units[cfg_row].type         = 1;
        fx.cfg_units[cfg_row].move_op_code = 0x22;
        u.x                                = 50;
        u.y                                = 60;
        u.goal_x                           = 80;
        u.goal_y                           = 90;
        u.move_heading                     = 7;
        u.path_slot_id                     = 0xff;
        fx.cur_unit_ptr                    = &u;
        fx.view_cur_player                 = player;
        fx.view_cur_index                  = (uint16_t)self_index;

        unit &o         = fx.u(player, other_index);
        o.state         = STATE_GROUP_STEP;
        o.order         = u.order;
        o.unit_proto_id = cfg_row;
        o.goal_x        = 80;
        o.goal_y        = 90;
        o.move_heading  = 0; // == pass 2's chosen(0) -- non-relocate, to test the extra direct write
        o.path_slot_id  = 0xff;

        set_heading_shape(fx, 7, 1); // pass 1 shape
        set_heading_shape(fx, 0, 2); // pass 2's new shape
        // heading 0's own candidates stay deactivated -> pass 2's chosen == 0 == "other"'s own heading.

        g_free_slot_queue = {5, 6}; // both succeed
        run(fx);

        // Each zero-candidate pass contributes 2 trace_greedy_path calls, not 1 -- the "re-run the
        // winner" step at 0x00483744-0x0048378c fires unconditionally, even for best_cand==-1 (its
        // own CALL 0x0066a9ac at 0x0048377b, distinct from the straight-ahead call at 0x00483648;
        // see T8a/T7c). So pass 1 occupies indices [0,1] (both mode=7) and pass 2 occupies [2,3].
        ck_eq((uint32_t)g_trace_greedy_path_calls.size(), 4u,
              "T9a: pass 1 (straight+rerun, no candidates) + pass 2 (straight+rerun) == 4 calls; "
              "pass 3 never starts since processed_count(2)==group_count(2) after pass 2");
        ck_eq((uint32_t)g_trace_greedy_path_calls[0].mode, 7u, "T9a: pass 1 plans with the ORIGINAL move_heading (7)");
        ck_eq((uint32_t)g_trace_greedy_path_calls[2].mode, 0u,
              "T9a: pass 2 plans with the RE-CLASSED move_heading -- shape 1 -> move_heading=0 "
              "(0x00483c66-0x00483c72) -- index 2 is pass 2's OWN straight trace (index 0/1 are pass "
              "1's straight+rerun, both mode=7)");
        ck_eq((uint32_t)o.state, 0x22u,
              "T9a: pass 2's EXTRA direct write -- cfg_units[proto].move_op_code stored straight into "
              "units[p][member].state BEFORE the unit_set_state_of call (0x00484296)");
        ck(g_set_state_of_calls.size() == 2 && g_set_state_of_calls[1].unit_index == other_index &&
               g_set_state_of_calls[1].new_state == 0x22,
           "T9a: AND the redundant unit_set_state_of(player, member, 0x22) call fires too -- BOTH "
           "effects, not just one (0x004842d7)");
        ck_eq((uint32_t)fx.u(player, self_index).state, 0u,
              "T9a: CONTRAST -- pass 1's own non-relocate commit (self) does NOT do the direct write; "
              "self's own .state stays at the reset default (0), proving the direct write is PASS-2-ONLY");
        ck_eq((uint32_t)u.move_heading, 7u,
              "T9a: processed_count(2)==group_count(2) after pass 2 -- pass 3 is skipped by the COUNT "
              "check (not a bail-out), so the final restore at 0x004849f5 still fires: move_heading is "
              "back to the original saved_heading(7)");
    }
    {
        // T9b -- shape 2 -> heading4/shape1/next3.
        reset_and_seed(fx);
        const uint16_t player      = 2;
        const int32_t  self_index  = 3;
        const int32_t  other_index = 10;
        const uint16_t cfg_row     = 10;
        unit          &u           = fx.u(player, self_index);
        u.order                    = 0x05;
        u.unit_proto_id            = cfg_row;
        fx.cfg_units[cfg_row].type = 1;
        u.x                        = 50;
        u.y                        = 60;
        u.goal_x                   = 80;
        u.goal_y                   = 90;
        u.move_heading             = 11;
        u.path_slot_id             = 0xff;
        fx.cur_unit_ptr            = &u;
        fx.view_cur_player         = player;
        fx.view_cur_index          = (uint16_t)self_index;

        unit &o         = fx.u(player, other_index);
        o.state         = STATE_GROUP_STEP;
        o.order         = u.order;
        o.unit_proto_id = cfg_row;
        o.goal_x        = 80;
        o.goal_y        = 90;
        o.move_heading  = 20; // never matches any shape_class used in this case -- MUST stay inside
                              // the real 24-heading domain [0,23] (heading_candidates is a 72-entry
                              // fixture, 24 headings * 3 slots). A prior draft used 99 here: 99*3=297
                              // indexes 225 entries past the array's end, an out-of-bounds heap
                              // write (via set_heading_shape below) AND read (the production commit
                              // pass itself reads heading_candidates[member's own heading * 3] to
                              // test shape_class) -- there is an identical
                              // prior trap in this same suite (a bad seed value silently corrupting
                              // heap memory past a fixture array).
        o.path_slot_id = 0xff;

        set_heading_shape(fx, 11, 2); // pass 1 shape
        set_heading_shape(fx, 20, 0); // never matches (redundant with the reset default of 0 --
                                      // kept explicit for clarity/symmetry with the other cases)
        g_free_slot_queue = {5};
        run(fx);

        // Each pass's plan ALWAYS issues a second trace_greedy_path call to "re-run the winner" --
        // even in the best_cand==-1 (zero active candidates) case, 0x00483744-0x0048378c: the
        // JNZ 0x00483748 falls through into its OWN CALL 0x0066a9ac at 0x0048377b, distinct from the
        // straight-ahead call at 0x00483648. So a zero-candidate pass contributes TWO calls (straight
        // + rerun), not one -- self (heading11), the re-classed heading4, and the re-classed heading1
        // are all zero-candidate here, so 3 passes * 2 calls = 6.
        ck_eq((uint32_t)g_trace_greedy_path_calls.size(), 6u,
              "T9b: pass 1 + pass 2 + pass 3 planning ALL run (processed_count stays 1, \"other\" never "
              "matches any shape), even though pass 3's commit finds no one to process either -- 2 "
              "calls per pass (straight + the unconditional re-run, 0x0048377b) since every pass here "
              "is zero-candidate");
        ck_eq((uint32_t)g_trace_greedy_path_calls[2].mode, 4u,
              "T9b: shape_class==2 -> move_heading=4, shape=1, next=3 (0x00483c82-0x00483c8e) -- index "
              "2 is pass 2's OWN straight trace (index 0/1 are pass 1's straight+rerun, both mode=11)");
        ck_eq((uint32_t)g_trace_greedy_path_calls[4].mode, 1u,
              "T9b: pass 3's OWN table, shape=next_shape(3) -> move_heading=1 (0x0048439e-0x004843aa) "
              "-- a preview of T10's full table, confirmed here via the planning `mode` alone -- index "
              "4 is pass 3's straight trace (index 2/3 are pass 2's straight+rerun, both mode=4)");
    }
    {
        // T9c -- shape 3 -> heading4/shape1/next2 -- SAME target heading as T9b but via a DIFFERENT
        // origin shape; the two arms are not folded together in the .asm.
        reset_and_seed(fx);
        const uint16_t player      = 2;
        const int32_t  self_index  = 3;
        const int32_t  other_index = 10;
        const uint16_t cfg_row     = 10;
        unit          &u           = fx.u(player, self_index);
        u.order                    = 0x05;
        u.unit_proto_id            = cfg_row;
        fx.cfg_units[cfg_row].type = 1;
        u.x                        = 50;
        u.y                        = 60;
        u.goal_x                   = 80;
        u.goal_y                   = 90;
        u.move_heading             = 13;
        u.path_slot_id             = 0xff;
        fx.cur_unit_ptr            = &u;
        fx.view_cur_player         = player;
        fx.view_cur_index          = (uint16_t)self_index;

        unit &o         = fx.u(player, other_index);
        o.state         = STATE_GROUP_STEP;
        o.order         = u.order;
        o.unit_proto_id = cfg_row;
        o.goal_x        = 80;
        o.goal_y        = 90;
        o.move_heading  = 20; // see T9b's comment -- MUST stay inside [0,23]; 99 is an out-of-bounds
                              // heading_candidates index (G36-shaped heap corruption), not a valid
                              // "never matches" sentinel.
        o.path_slot_id = 0xff;

        set_heading_shape(fx, 13, 3); // pass 1 shape
        set_heading_shape(fx, 20, 0);
        g_free_slot_queue = {5};
        run(fx);

        // See T9b: every zero-candidate pass contributes 2 trace_greedy_path calls (straight +
        // the unconditional re-run at 0x0048377b), not 1.
        ck_eq((uint32_t)g_trace_greedy_path_calls.size(), 6u,
              "T9c: pass 1 + pass 2 + pass 3 planning all run -- 2 calls per pass (straight + rerun) "
              "since every pass here is zero-candidate");
        ck_eq((uint32_t)g_trace_greedy_path_calls[2].mode, 4u,
              "T9c: shape_class==3 -> move_heading=4, shape=1, next=2 (0x00483c9e-0x00483caa) -- index "
              "2 is pass 2's OWN straight trace (index 0/1 are pass 1's straight+rerun, both mode=13)");
        ck_eq((uint32_t)g_trace_greedy_path_calls[4].mode, 0u,
              "T9c: pass 3's table, shape=next_shape(2) -> move_heading=0 (0x00484382-0x0048438e) -- "
              "index 4 is pass 3's straight trace (index 2/3 are pass 2's straight+rerun, both mode=4)");
    }

    // =================================================================================================
    // T10/T11 -- pass 3, actually REACHED, with real members and a full commit; the final move_heading
    // restore firing on completion (T11a) and on a count-based SKIP (T9a above), and NOT firing on a
    // pass-3 bail-out (T11c).
    // =================================================================================================
    {
        // T10 / T11a -- all three members succeed; pass 3 runs to completion.
        reset_and_seed(fx);
        seed_three_pass_group(fx);
        g_free_slot_queue = {5, 6, 7}; // self(pass1), member_A(pass2), member_B(pass3) -- all succeed
        run(fx);

        // seed_three_pass_group only sets each heading's slot-0 cand_facing (via set_heading_shape),
        // never any actual candidate slot (set_heading_cand) -- so headings 7/0/1 (self's own,
        // pass 2's re-classed, pass 3's re-classed) are all zero-candidate, and each pass's plan
        // contributes 2 trace_greedy_path calls (straight + the unconditional re-run at 0x0048377b),
        // not 1 -- see T8a/T7c/T9a. So pass1=[0,1], pass2=[2,3], pass3=[4,5].
        ck_eq((uint32_t)g_trace_greedy_path_calls.size(), 6u,
              "T10: pass 1 + pass 2 + pass 3 planning -- 6 calls (straight+rerun per pass), pass 3 "
              "actually RUNS (processed_count(2) < group_count(3) after pass 2)");
        ck_eq((uint32_t)g_trace_greedy_path_calls[4].mode, 1u,
              "T10: pass 3's OWN remap table (0x00484366-0x004843aa) -- shape=next_shape(3) -> "
              "move_heading=1 (0x00484390-0x0048439c) -- a DIFFERENT table from pass 2's (which maps "
              "shape 1 to heading 4, not shape 3) -- index 4 is pass 3's straight trace (index 2/3 are "
              "pass 2's straight+rerun, both mode=0)");
        ck(g_set_state_of_calls.size() == 3 && g_set_state_of_calls[2].unit_index == T10_B_INDEX,
           "T10: member_B (heading1, shape3) IS processed in pass 3's commit -- non-relocate branch "
           "(0x0048493e)");
        ck_eq((uint32_t)fx.u(T10_PLAYER, T10_SELF_INDEX).move_heading, 7u,
              "T11a: pass 3 runs to COMPLETION (no bail-out) -- the final restore at 0x004849f5 still "
              "fires: move_heading is back to the original saved_heading(7)");
        ck_eq((uint32_t)g_notify_calls.size(), 3u, "T10: all three members reach unit_notify_status");
    }
    {
        // T11c -- member_B's free_slot fails inside pass 3: the bail-out skips the restore.
        reset_and_seed(fx);
        seed_three_pass_group(fx);
        g_free_slot_queue = {5, 6, -1}; // self, member_A succeed; member_B FAILS in pass 3
        run(fx);

        ck_eq((uint32_t)fx.u(T10_PLAYER, T10_SELF_INDEX).move_heading, 1u,
              "T11c: pass 3's bail-out (free_slot==-1, 0x004846be JMP 0x00484a0a) skips the final "
              "restore -- move_heading is left at pass 3's OWN re-classed value (1), NOT the original "
              "saved_heading(7) and NOT pass 2's intermediate value(0) either");
        ck(g_set_state_of_calls.size() == 3 && g_set_state_of_calls[2].unit_index == T10_B_INDEX &&
               g_set_state_of_calls[2].new_state == (int16_t)UNIT_STATE_IDLE_SCATTER,
           "T11c: member_B gets IDLE_SCATTER on the bail (0x004846a7-0x004846b9)");
        ck_eq((uint32_t)g_notify_calls.size(), 2u,
              "T11c: only self and member_A reach unit_notify_status -- member_B's bail returns before it");
    }

    // =================================================================================================
    // T12 -- non-corruption: a guard unit and a guard building this function never addresses, read
    // back unchanged across T7c's relocate scenario (the most field-mutating single-pass run).
    // =================================================================================================
    {
        reset_and_seed(fx);
        const uint16_t player      = 2;
        const int32_t  self_index  = 3;
        const int32_t  other_index = 10;
        const uint16_t cfg_row     = 10;

        unit &u                     = fx.u(player, self_index);
        u.order                     = 0x05;
        u.unit_proto_id             = cfg_row;
        fx.cfg_units[cfg_row].type  = 1;
        fx.cfg_units[cfg_row].sight = 4;
        u.x                         = 50;
        u.y                         = 60;
        u.goal_x                    = 80;
        u.goal_y                    = 90;
        u.move_heading              = 7;
        u.path_slot_id              = 0xff;
        fx.cur_unit_ptr             = &u;
        fx.view_cur_player          = player;
        fx.view_cur_index           = (uint16_t)self_index;

        unit &o          = fx.u(player, other_index);
        o.state          = STATE_GROUP_STEP;
        o.order          = u.order;
        o.unit_proto_id  = cfg_row;
        o.goal_x         = 80;
        o.goal_y         = 90;
        o.x              = 70;
        o.y              = 80;
        o.move_heading   = 12;
        o.path_slot_id   = 9;
        o.activity_clock = 555.0;

        set_heading_shape(fx, 7, 1);
        set_heading_shape(fx, 12, 1);
        set_heading_cand(fx, 12, 0, /*turn_delta=*/99, /*col=*/3, /*row=*/-2);

        g_find_slot_queue = {0};
        g_free_slot_queue = {5, 6};
        run(fx);

        const unit &g = fx.u(GUARD_PLAYER, GUARD_INDEX);
        ck(g.order == 0x44 && g.state == 0x55 && g.move_heading == 9, "T12: guard unit's dispatch fields untouched");
        ck((uint32_t)(uint16_t)g.target_ref == 0x37u && (uint32_t)(uint16_t)g.target_index == 0x28u,
           "T12: guard unit's target_ref/target_index untouched");
        ck(g.x == 11 && g.y == 22 && g.goal_x == 33 && g.goal_y == 44, "T12: guard unit's x/y/goal_x/goal_y untouched");
        ck(g.path_slot_id == 0x22, "T12: guard unit's path_slot_id untouched");
        ck_eq_d(g.activity_clock, 7777.0, "T12: guard unit's activity_clock untouched");
        ck(g.unit_proto_id == 66, "T12: guard unit's unit_proto_id untouched");

        const building &gb = fx.b(GUARD_BLDG_OWNER, GUARD_BLDG_SLOT);
        ck(gb.building_id == 88, "T12: guard building's building_id untouched");
        ck_eq_d(gb.energy, 999.0, "T12: guard building's energy untouched");
    }
}

} // namespace mh::sim::test
