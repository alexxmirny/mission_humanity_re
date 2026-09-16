//
// sim_group_move_order_commit_selftest.cpp -- `simtest` cases for
// llm_strat_group_move_order_commit @0x0041c86c (sim/sim_group_move_order_commit.h/.cpp, SIM1-G1).
//
// SCOPE:
//   T1  -- the entry guard (0x0041c894 CMP member_count,0 / 0x0041c898 JZ; 0x0041c89e CMP
//          player_id,-1 / 0x0041c8a2 JZ): EITHER condition returns with NO state touched at all --
//          both sides, verified against every region the function otherwise writes.
//   T2  -- the unconditional route/wrap-mask prefix (0x0041c8a8-0x0041c8bc): route_step[0] seeded to
//          {0,0}; path_wrap_mask = map_width-1. Neighbour route_step[1] untouched.
//   T3  -- the goal/anchor copy (0x0041c8c1-0x0041c8dc): BOTH goal_{x,y} and anchor_{x,y} take
//          (goal_x, goal_y) verbatim -- not a goal/anchor split, not an axis swap.
//   T4  -- the member-count global write (0x0041c8fe-0x0041c903).
//   T5  -- the per-member loop copy (0x0041c90a-0x0041c9b6), normal multi-member case: unit_handle =
//          scratch.unit_idx (full dword), cur_col/cur_row = low byte of scratch.tile_col/tile_row;
//          a roster slot the loop never reaches (member_count < its index) stays untouched.
//   T6  -- the bad-member ("unit_idx<1") counter (0x0041c93a-0x0041c948): the LOOP still completes and
//          commits every member's row (partial-commit-then-bail), but the post-loop gate
//          (0x0041c9b6-0x0041c9ba) then skips owner/pathfind entirely.
//   T7  -- the impassable-start-tile abort (0x0041c98a-0x0041c9ac): the member whose OWN tile is
//          impassable is copied in full (its row IS committed) before the abort; the NEXT member is
//          never reached; stack_capacity_guard_0x20() fires exactly once and the whole commit bails.
//   T8  -- (a) target_class normalization (0x0041c8e1-0x0041c8f7: 1 stays GROUND, anything else becomes
//          AIR) and the mode/mask CALL ARGS to group_move_order_pathfind (0x0041c9c8-0x0041c9ce,
//          mode=is_plain_move_flag passed VERBATIM, not normalized); (b) the member_count>1 &&
//          is_plain_move_flag!=0 short-circuit gate (0x0041c9d3 CMP+JLE / 0x0041c9dc CMP+JNZ) on the
//          scratch tile_col/tile_row writeback loop (0x0041c9e4-0x0041ca2e) -- fires only when BOTH
//          hold, and touches ONLY .tile_col/.tile_row, not the row's other fields.
//   T9  -- dedicated non-corruption sweep on a full success path: neighbouring route_step, group_member
//          and group_move_scratch rows this call never addresses read back exactly as seeded.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM
// tmp/decomp_sim/llm_strat_group_move_order_commit_0041c86c.asm -- every assertion below cites the
// instruction address(es) it pins. NOT read off the .cpp body.
//
// move_group_id (param 5) is loaded into a local at entry and never read again anywhere in the
// listing -- truly dead, per the function's own plate comment and the .cpp's own note at its call
// site -- so every Seed below feeds it a distinct sentinel and no case asserts on it.
//
#include "sim/sim_group_move_order_commit.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: proves CALL ORDER -- and, since the two callees sit on MUTUALLY EXCLUSIVE
// terminal paths (guard aborts before pathfind is ever reached, and vice versa), the trace equality
// checks below also double as the "exact call order" proof the brief asks for. ------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- per-callee recorders (2, one per group_move_order_commit_calls member) -----------------------
struct PathfindCall {
    int32_t mode;
    uint8_t target_mask;
};
std::vector<PathfindCall> g_pathfind_calls;
void                      rec_pathfind(int32_t mode, uint8_t target_mask) {
    tr("pathfind");
    g_pathfind_calls.push_back({mode, target_mask});
}

int  g_guard_calls = 0;
void rec_guard() {
    tr("guard");
    ++g_guard_calls;
}

const group_move_order_commit_calls g_calls = {
    &rec_pathfind,
    &rec_guard,
};

void reset_observations() {
    g_trace.clear();
    g_pathfind_calls.clear();
    g_guard_calls = 0;
}

// ---- sentinels ---------------------------------------------------------------------------------
// Distinct, non-zero, non-default values for every region the function writes, so a run that leaves a
// region untouched is provably distinguishable from one that zeroed or default-initialized it.
constexpr uint32_t SENT_WRAP_MASK   = 0xdeadbeefu;
constexpr int32_t  SENT_GOAL_X      = -111;
constexpr int32_t  SENT_GOAL_Y      = -222;
constexpr int32_t  SENT_ANCHOR_X    = -333;
constexpr int32_t  SENT_ANCHOR_Y    = -444;
constexpr int32_t  SENT_OWNER       = 0x7777;
constexpr int32_t  SENT_MEMBERCOUNT = static_cast<int32_t>(0x88888888u);
constexpr uint8_t  SENT_ROUTE0_DIR  = 0xab;
constexpr uint8_t  SENT_ROUTE0_RUN  = 0xcd;
constexpr uint8_t  SENT_ROUTE1_DIR  = 0x11; // route_step[1] -- neighbour, never touched by this fn
constexpr uint8_t  SENT_ROUTE1_RUN  = 0x22;

constexpr uint32_t SENT_MEMBER_HANDLE = 0xfeedfaceu;
constexpr uint8_t  SENT_MEMBER_COL    = 0xee;
constexpr uint8_t  SENT_MEMBER_ROW    = 0xff;
constexpr int32_t  GUARD_MEMBER_IDX   = 70; // well past every test's member_count (<=3)

constexpr int32_t GUARD_SCRATCH_IDX = 90; // well past every test's member_count
constexpr int32_t SENT_SCRATCH_COL  = 201;
constexpr int32_t SENT_SCRATCH_ROW  = 202;
constexpr int32_t SENT_SCRATCH_WR   = 203;
constexpr int32_t SENT_SCRATCH_UI   = 204;
constexpr int32_t SENT_SCRATCH_SP   = 205;

void seed_prefix_sentinels(sim_fixture &fx) {
    fx.group_route_steps[0].dir_code   = SENT_ROUTE0_DIR;
    fx.group_route_steps[0].run_length = SENT_ROUTE0_RUN;
    fx.group_route_steps[1].dir_code   = SENT_ROUTE1_DIR;
    fx.group_route_steps[1].run_length = SENT_ROUTE1_RUN;
    fx.path_wrap_mask                  = SENT_WRAP_MASK;
    fx.group_order_goal_x              = SENT_GOAL_X;
    fx.group_order_goal_y              = SENT_GOAL_Y;
    fx.group_anchor_x                  = SENT_ANCHOR_X;
    fx.group_anchor_y                  = SENT_ANCHOR_Y;
    fx.group_order_owner               = SENT_OWNER;
    fx.group_member_count              = SENT_MEMBERCOUNT;
}

void seed_members_sentinel(sim_fixture &fx) {
    for (auto &m : fx.group_members) {
        m.unit_handle = SENT_MEMBER_HANDLE;
        m.cur_col     = SENT_MEMBER_COL;
        m.cur_row     = SENT_MEMBER_ROW;
    }
}

void seed_scratch_guard(sim_fixture &fx) {
    group_scratch_member &g = fx.group_move_scratch[GUARD_SCRATCH_IDX];
    g.tile_col              = SENT_SCRATCH_COL;
    g.tile_row              = SENT_SCRATCH_ROW;
    g.wave_rank             = SENT_SCRATCH_WR;
    g.unit_idx              = SENT_SCRATCH_UI;
    g.saved_passable        = SENT_SCRATCH_SP;
}

bool member_untouched(const sim_fixture &fx, int32_t idx) {
    const group_member &m = fx.group_members[(size_t)idx];
    return m.unit_handle == SENT_MEMBER_HANDLE && m.cur_col == SENT_MEMBER_COL &&
           m.cur_row == SENT_MEMBER_ROW;
}

bool scratch_guard_untouched(const sim_fixture &fx) {
    const group_scratch_member &g = fx.group_move_scratch[(size_t)GUARD_SCRATCH_IDX];
    return g.tile_col == SENT_SCRATCH_COL && g.tile_row == SENT_SCRATCH_ROW &&
           g.wave_rank == SENT_SCRATCH_WR && g.unit_idx == SENT_SCRATCH_UI &&
           g.saved_passable == SENT_SCRATCH_SP;
}

// ---- fixture seeding -----------------------------------------------------------------------------
struct MemberIn {
    int32_t tile_col;
    int32_t tile_row;
    int32_t unit_idx;
    uint8_t passable_val; // 1 == passable, 0 == impassable
};

struct Seed {
    int32_t  goal_x             = 1;
    int32_t  goal_y             = 2;
    int32_t  player_id          = 3;
    int32_t  member_count       = 1;
    int32_t  move_group_id      = 0x5eed; // dead param -- distinct sentinel, never asserted on
    uint32_t is_plain_move_flag = 0;
    uint32_t target_class       = 1;
    int32_t  map_width          = 77; // -> wrap mask expected 76

    std::vector<MemberIn>                    members;       // one entry per scratch row [0, member_count)
    std::vector<std::pair<uint8_t, uint8_t>> planner_tiles; // v.group_member_tile[i*2 + 0/1]
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();
    seed_prefix_sentinels(fx);
    seed_members_sentinel(fx);
    seed_scratch_guard(fx);

    fx.map_width = s.map_width;
    std::fill(fx.passable.begin(), fx.passable.end(), (uint8_t)1); // default passable everywhere

    for (size_t i = 0; i < s.members.size(); ++i) {
        group_scratch_member &sc = fx.group_move_scratch[i];
        sc.tile_col              = s.members[i].tile_col;
        sc.tile_row              = s.members[i].tile_row;
        sc.wave_rank             = 42; // don't-care, distinct nonzero
        sc.unit_idx              = s.members[i].unit_idx;
        sc.saved_passable        = 9; // don't-care, distinct nonzero
        const uint32_t idx       = ((uint32_t)s.members[i].tile_col << 8) | (uint32_t)s.members[i].tile_row;
        fx.passable[idx]         = s.members[i].passable_val;
    }

    for (size_t i = 0; i < s.planner_tiles.size(); ++i) {
        fx.group_member_tile[i * 2 + 0] = s.planner_tiles[i].first;
        fx.group_member_tile[i * 2 + 1] = s.planner_tiles[i].second;
    }

    reset_observations();

    sim_store own = fx.store();
    detail::group_move_order_commit(fx.view(), own, g_calls, s.goal_x, s.goal_y, s.player_id,
                                    s.member_count, s.move_group_id, s.is_plain_move_flag,
                                    s.target_class);
}

} // namespace

void run_group_move_order_commit_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- entry guard (0x0041c894 CMP member_count,0 / 0x0041c898 JZ; 0x0041c89e CMP player_id,-1 /
    // 0x0041c8a2 JZ): EITHER condition alone returns before touching ANY state, incl. the fields the
    // rest of this file proves the function otherwise writes unconditionally once past the guard.
    // =================================================================================================
    {
        Seed s;
        s.player_id    = 3; // valid
        s.member_count = 0; // GUARD: member_count==0
        s.members.clear();
        seed_and_run(fx, s);
        ck(fx.group_route_steps[0].dir_code == SENT_ROUTE0_DIR &&
               fx.group_route_steps[0].run_length == SENT_ROUTE0_RUN,
           "T1a: member_count==0 -- route_step[0] untouched (0x0041c898 JZ taken)");
        ck_eq(fx.path_wrap_mask, SENT_WRAP_MASK, "T1a: path_wrap_mask untouched");
        ck_eq((uint32_t)fx.group_order_goal_x, (uint32_t)SENT_GOAL_X, "T1a: group_order_goal_x untouched");
        ck_eq((uint32_t)fx.group_order_goal_y, (uint32_t)SENT_GOAL_Y, "T1a: group_order_goal_y untouched");
        ck_eq((uint32_t)fx.group_anchor_x, (uint32_t)SENT_ANCHOR_X, "T1a: group_anchor_x untouched");
        ck_eq((uint32_t)fx.group_anchor_y, (uint32_t)SENT_ANCHOR_Y, "T1a: group_anchor_y untouched");
        ck_eq((uint32_t)fx.group_order_owner, (uint32_t)SENT_OWNER, "T1a: group_order_owner untouched");
        ck_eq((uint32_t)fx.group_member_count, (uint32_t)SENT_MEMBERCOUNT,
              "T1a: group_member_count untouched");
        ck(member_untouched(fx, 0), "T1a: group_members[0] untouched");
        ck(g_trace.empty(), "T1a: no outward call fires");

        s.player_id    = -1;                               // GUARD: player_id==-1
        s.member_count = 2;                                // otherwise-valid member_count
        s.members      = {{10, 20, 1, 1}, {30, 40, 1, 1}}; // otherwise-valid members
        seed_and_run(fx, s);
        ck(fx.group_route_steps[0].dir_code == SENT_ROUTE0_DIR &&
               fx.group_route_steps[0].run_length == SENT_ROUTE0_RUN,
           "T1b: player_id==-1 -- route_step[0] untouched (0x0041c8a2 JZ taken)");
        ck_eq(fx.path_wrap_mask, SENT_WRAP_MASK, "T1b: path_wrap_mask untouched");
        ck_eq((uint32_t)fx.group_order_goal_x, (uint32_t)SENT_GOAL_X, "T1b: group_order_goal_x untouched");
        ck_eq((uint32_t)fx.group_order_owner, (uint32_t)SENT_OWNER, "T1b: group_order_owner untouched");
        ck_eq((uint32_t)fx.group_member_count, (uint32_t)SENT_MEMBERCOUNT,
              "T1b: group_member_count untouched (even though 2 otherwise-valid members were provided)");
        ck(member_untouched(fx, 0) && member_untouched(fx, 1),
           "T1b: group_members[0]/[1] untouched despite otherwise-valid member rows");
        ck(g_trace.empty(), "T1b: no outward call fires");
    }

    // =================================================================================================
    // T2 -- the unconditional route/wrap-mask prefix (0x0041c8a8-0x0041c8bc): route_step[0] seeded to
    // the terminal sentinel {dir_code=0, run_length=0}; path_wrap_mask = map_width-1. Neighbour
    // route_step[1] is never touched.
    // =================================================================================================
    {
        Seed s;
        s.player_id    = 5;
        s.member_count = 1;
        s.members      = {{10, 20, 1, 1}};
        s.map_width    = 77;
        seed_and_run(fx, s);
        ck_eq(fx.group_route_steps[0].dir_code, 0u,
              "T2: route_step[0].dir_code == 0 (0x0041c8a8 MOV byte [ROUTE_STEPS],0)");
        ck_eq(fx.group_route_steps[0].run_length, 0u,
              "T2: route_step[0].run_length == 0 (0x0041c8af MOV byte [ROUTE_STEPS+1],0)");
        ck(fx.group_route_steps[1].dir_code == SENT_ROUTE1_DIR &&
               fx.group_route_steps[1].run_length == SENT_ROUTE1_RUN,
           "T2: route_step[1] (neighbour) untouched");
        ck_eq(fx.path_wrap_mask, 76u,
              "T2: path_wrap_mask == map_width(77)-1 == 76 (0x0041c8b6-0x0041c8bc)");
    }

    // =================================================================================================
    // T3 -- goal/anchor copy (0x0041c8c1-0x0041c8dc): BOTH goal_{x,y} and anchor_{x,y} take
    // (goal_x, goal_y) verbatim -- distinct, non-symmetric values so a goal/anchor mixup or an axis
    // swap disagrees with this case.
    // =================================================================================================
    {
        Seed s;
        s.goal_x       = 555;
        s.goal_y       = -777;
        s.player_id    = 9;
        s.member_count = 1;
        s.members      = {{10, 20, 1, 1}};
        seed_and_run(fx, s);
        ck_eq((uint32_t)fx.group_order_goal_x, (uint32_t)555,
              "T3: group_order_goal_x == goal_x (0x0041c8c1-0x0041c8c4)");
        ck_eq((uint32_t)fx.group_order_goal_y, (uint32_t)-777,
              "T3: group_order_goal_y == goal_y (0x0041c8c9-0x0041c8cc)");
        ck_eq((uint32_t)fx.group_anchor_x, (uint32_t)555,
              "T3: group_anchor_x == goal_x, NOT goal_y (0x0041c8d1-0x0041c8d4)");
        ck_eq((uint32_t)fx.group_anchor_y, (uint32_t)-777,
              "T3: group_anchor_y == goal_y, NOT goal_x (0x0041c8d9-0x0041c8dc)");
    }

    // =================================================================================================
    // T4 -- group_member_count global write (0x0041c8fe-0x0041c903).
    // =================================================================================================
    {
        Seed s;
        s.player_id    = 1;
        s.member_count = 3;
        s.members      = {{10, 20, 1, 1}, {30, 40, 1, 1}, {50, 60, 1, 1}};
        seed_and_run(fx, s);
        ck_eq((uint32_t)fx.group_member_count, (uint32_t)3,
              "T4: group_member_count == member_count (0x0041c8fe-0x0041c903)");
    }

    // =================================================================================================
    // T5 -- per-member loop copy (0x0041c90a-0x0041c9b6), normal multi-member case: unit_handle =
    // scratch.unit_idx (full dword), cur_col/cur_row = low byte of scratch.tile_col/tile_row. A roster
    // slot the loop never reaches (past member_count) stays untouched. All members valid + passable,
    // so the run falls through to owner/pathfind (0x0041c9c0-0x0041c9ce).
    // =================================================================================================
    {
        Seed s;
        s.player_id          = 41;
        s.member_count       = 3;
        s.is_plain_move_flag = 0; // step 8 gate closed (member_count>1 but flag==0) -- isolates T5
        s.target_class       = 1;
        s.members            = {
            {10, 20, 5, 1},
            {30, 40, 6, 1},
            {50, 60, 7, 1},
        };
        seed_and_run(fx, s);
        ck_eq(fx.group_members[0].unit_handle, 5u, "T5: group_members[0].unit_handle == scratch[0].unit_idx");
        ck_eq(fx.group_members[0].cur_col, 10u, "T5: group_members[0].cur_col == scratch[0].tile_col");
        ck_eq(fx.group_members[0].cur_row, 20u, "T5: group_members[0].cur_row == scratch[0].tile_row");
        ck_eq(fx.group_members[1].unit_handle, 6u, "T5: group_members[1].unit_handle == scratch[1].unit_idx");
        ck_eq(fx.group_members[1].cur_col, 30u, "T5: group_members[1].cur_col == scratch[1].tile_col");
        ck_eq(fx.group_members[1].cur_row, 40u, "T5: group_members[1].cur_row == scratch[1].tile_row");
        ck_eq(fx.group_members[2].unit_handle, 7u, "T5: group_members[2].unit_handle == scratch[2].unit_idx");
        ck_eq(fx.group_members[2].cur_col, 50u, "T5: group_members[2].cur_col == scratch[2].tile_col");
        ck_eq(fx.group_members[2].cur_row, 60u, "T5: group_members[2].cur_row == scratch[2].tile_row");
        ck(member_untouched(fx, GUARD_MEMBER_IDX), "T5: group_members[70] (past member_count) untouched");
        ck_eq((uint32_t)fx.group_order_owner, (uint32_t)41,
              "T5: all members valid+passable -- group_order_owner IS committed (0x0041c9c0-0x0041c9c3)");
        ck(trace_eq({"pathfind"}), "T5: exactly one pathfind call, no guard call");
    }

    // =================================================================================================
    // T6 -- bad-member counter (0x0041c93a-0x0041c948, unit_idx<1): the LOOP still completes and
    // commits every member's row (partial-commit-then-bail), but the post-loop gate
    // (0x0041c9b6-0x0041c9ba) then skips owner/pathfind entirely. All tiles passable, so this isolates
    // the bad-member path from the impassable-abort path (T7).
    // =================================================================================================
    {
        Seed s;
        s.player_id    = 9;
        s.member_count = 2;
        s.members      = {
            {11, 22, 1, 1}, // valid (unit_idx>=1)
            {33, 44, 0, 1}, // BAD: unit_idx<1, but still passable
        };
        seed_and_run(fx, s);
        ck_eq((uint32_t)fx.group_member_count, (uint32_t)2, "T6: group_member_count still committed (prefix write)");
        ck_eq(fx.group_members[0].unit_handle, 1u, "T6: group_members[0] fully committed despite the later bail");
        ck_eq(fx.group_members[0].cur_col, 11u, "T6: group_members[0].cur_col committed");
        ck_eq(fx.group_members[0].cur_row, 22u, "T6: group_members[0].cur_row committed");
        ck_eq(fx.group_members[1].unit_handle, 0u,
              "T6: group_members[1] ALSO fully committed (bad_member_count is counted, loop does not abort here)");
        ck_eq(fx.group_members[1].cur_col, 33u, "T6: group_members[1].cur_col committed");
        ck_eq(fx.group_members[1].cur_row, 44u, "T6: group_members[1].cur_row committed");
        ck_eq((uint32_t)fx.group_order_owner, (uint32_t)SENT_OWNER,
              "T6: group_order_owner NOT written -- post-loop bad-member gate bails (0x0041c9b6-0x0041c9ba)");
        ck(g_trace.empty(), "T6: no outward call fires (bails before pathfind, no impassable member to trigger guard)");
    }

    // =================================================================================================
    // T7 -- impassable-start-tile abort (0x0041c98a-0x0041c9ac): the member whose OWN tile is
    // impassable is copied IN FULL (unit_handle/cur_col/cur_row all land) before the abort -- the copy
    // precedes the passable check in program order. The NEXT member is never reached at all. The whole
    // commit aborts via stack_capacity_guard_0x20(), exactly once.
    // =================================================================================================
    {
        Seed s;
        s.player_id    = 13;
        s.member_count = 3;
        s.members      = {
            {12, 13, 1, 1}, // passable -- copied normally
            {14, 15, 1, 0}, // IMPASSABLE -- copied in full, then aborts
            {16, 17, 1, 1}, // never reached
        };
        seed_and_run(fx, s);
        ck_eq(fx.group_members[0].unit_handle, 1u, "T7: group_members[0] committed (before the impassable member)");
        ck_eq(fx.group_members[0].cur_col, 12u, "T7: group_members[0].cur_col committed");
        ck_eq(fx.group_members[0].cur_row, 13u, "T7: group_members[0].cur_row committed");
        ck_eq(fx.group_members[1].unit_handle, 1u,
              "T7: group_members[1] (the impassable member itself) IS fully copied before the abort "
              "(0x0041c92a-0x0041c96b precede the 0x0041c98a passable CMP)");
        ck_eq(fx.group_members[1].cur_col, 14u, "T7: group_members[1].cur_col committed");
        ck_eq(fx.group_members[1].cur_row, 15u, "T7: group_members[1].cur_row committed");
        ck(member_untouched(fx, 2),
           "T7: group_members[2] NEVER reached -- the abort at member 1 skips the rest of the loop entirely");
        ck_eq((uint32_t)g_guard_calls, (uint32_t)1,
              "T7: stack_capacity_guard_0x20() fires exactly once (0x0041c9a7 CALL)");
        ck(trace_eq({"guard"}), "T7: guard fires, pathfind never reached -- the two callees are mutually exclusive");
        ck_eq((uint32_t)fx.group_order_owner, (uint32_t)SENT_OWNER,
              "T7: group_order_owner NOT written -- the abort returns before 0x0041c9c0");
        ck_eq((uint32_t)fx.group_member_count, (uint32_t)3, "T7: group_member_count still committed (prefix write)");
    }

    // =================================================================================================
    // T8 -- target_class normalization (0x0041c8e1-0x0041c8f7: ==1 stays GROUND, anything else becomes
    // AIR) + mode/mask CALL ARGS to group_move_order_pathfind (0x0041c9c8-0x0041c9ce: mode =
    // is_plain_move_flag passed VERBATIM -- NOT normalized, unlike target_class) + the step 8
    // member_count>1 && is_plain_move_flag!=0 short-circuit gate (0x0041c9d3/0x0041c9dc) on the
    // scratch tile_col/tile_row writeback (0x0041c9e4-0x0041ca2e), which touches ONLY those two
    // fields.
    // =================================================================================================
    {
        // T8a -- member_count==1 (<=1): step 8 skipped regardless of is_plain_move_flag. target_class
        // == 1 stays GROUND(1). mode is passed through as-is (3, not clamped to a bool 0/1).
        Seed s;
        s.player_id          = 21;
        s.member_count       = 1;
        s.is_plain_move_flag = 3; // nonzero, but member_count<=1 must still skip step 8
        s.target_class       = 1;
        s.members            = {{10, 20, 1, 1}};
        // no planner_tiles seeded -- if step 8 fired anyway it would zero scratch[0]'s tile, which is
        // distinguishable from the original (10,20).
        seed_and_run(fx, s);
        ck(trace_eq({"pathfind"}), "T8a: pathfind fires (all members valid+passable)");
        if (!g_pathfind_calls.empty()) {
            ck_eq((uint32_t)g_pathfind_calls[0].mode, (uint32_t)3,
                  "T8a: pathfind mode == is_plain_move_flag(3) passed verbatim (0x0041c9cb MOV "
                  "EAX,is_plain_move_flag)");
            ck_eq((uint32_t)g_pathfind_calls[0].target_mask, (uint32_t)WEAPON_TARGET_GROUND,
                  "T8a: target_class==1 normalizes to WEAPON_TARGET_GROUND (0x0041c8e5 JNZ not taken)");
        }
        ck_eq((uint32_t)fx.group_move_scratch[0].tile_col, (uint32_t)10,
              "T8a: member_count<=1 -- step 8 skipped, scratch[0].tile_col unchanged (0x0041c9da JLE taken)");
        ck_eq((uint32_t)fx.group_move_scratch[0].tile_row, (uint32_t)20,
              "T8a: member_count<=1 -- step 8 skipped, scratch[0].tile_row unchanged");

        // T8b -- member_count==2 (>1) but is_plain_move_flag==0: step 8 still skipped. target_class
        // != 1 normalizes to AIR(2).
        Seed s2;
        s2.player_id          = 22;
        s2.member_count       = 2;
        s2.is_plain_move_flag = 0; // flag closed
        s2.target_class       = 7; // != 1
        s2.members            = {{10, 20, 1, 1}, {30, 40, 1, 1}};
        seed_and_run(fx, s2);
        ck(trace_eq({"pathfind"}), "T8b: pathfind fires (all members valid+passable)");
        if (!g_pathfind_calls.empty()) {
            ck_eq((uint32_t)g_pathfind_calls[0].mode, (uint32_t)0, "T8b: pathfind mode == is_plain_move_flag(0)");
            ck_eq((uint32_t)g_pathfind_calls[0].target_mask, (uint32_t)WEAPON_TARGET_AIR,
                  "T8b: target_class==7 (!=1) normalizes to WEAPON_TARGET_AIR (0x0041c8e5 JNZ taken, "
                  "0x0041c8f0 sets 2)");
        }
        ck_eq((uint32_t)fx.group_move_scratch[0].tile_col, (uint32_t)10,
              "T8b: member_count>1 but flag==0 -- step 8 skipped (0x0041c9e0 JNZ not taken)");
        ck_eq((uint32_t)fx.group_move_scratch[1].tile_col, (uint32_t)30, "T8b: scratch[1].tile_col unchanged too");

        // T8c -- member_count==2 (>1) AND is_plain_move_flag!=0 (3): step 8 FIRES, overwriting
        // scratch[i].tile_col/tile_row from v.group_member_tile[i*2+0]/[i*2+1] -- and ONLY those two
        // fields (wave_rank/unit_idx/saved_passable are untouched).
        Seed s3;
        s3.player_id          = 23;
        s3.member_count       = 2;
        s3.is_plain_move_flag = 3; // flag open
        s3.target_class       = 1;
        s3.members            = {{10, 20, 1, 1}, {30, 40, 1, 1}};
        s3.planner_tiles      = {{99, 88}, {77, 66}}; // distinct from the members' own tiles above
        seed_and_run(fx, s3);
        ck(trace_eq({"pathfind"}), "T8c: pathfind fires (all members valid+passable)");
        if (!g_pathfind_calls.empty())
            ck_eq((uint32_t)g_pathfind_calls[0].mode, (uint32_t)3, "T8c: pathfind mode == is_plain_move_flag(3)");
        ck_eq((uint32_t)fx.group_move_scratch[0].tile_col, (uint32_t)99,
              "T8c: step 8 fires -- scratch[0].tile_col <- group_member_tile[0] (0x0041ca00-0x0041ca10)");
        ck_eq((uint32_t)fx.group_move_scratch[0].tile_row, (uint32_t)88,
              "T8c: scratch[0].tile_row <- group_member_tile[1] (0x0041ca16-0x0041ca26)");
        ck_eq((uint32_t)fx.group_move_scratch[1].tile_col, (uint32_t)77, "T8c: scratch[1].tile_col <- group_member_tile[2]");
        ck_eq((uint32_t)fx.group_move_scratch[1].tile_row, (uint32_t)66, "T8c: scratch[1].tile_row <- group_member_tile[3]");
        ck_eq((uint32_t)fx.group_move_scratch[0].wave_rank, (uint32_t)42,
              "T8c: scratch[0].wave_rank untouched by the writeback (only tile_col/tile_row are written)");
        ck_eq((uint32_t)fx.group_move_scratch[0].unit_idx, (uint32_t)1,
              "T8c: scratch[0].unit_idx untouched by the writeback");
        ck_eq((uint32_t)fx.group_move_scratch[0].saved_passable, (uint32_t)9,
              "T8c: scratch[0].saved_passable untouched by the writeback");
        ck(scratch_guard_untouched(fx), "T8c: group_move_scratch[90] (past member_count) untouched by the writeback loop");
    }

    // =================================================================================================
    // T9 -- dedicated non-corruption sweep on a full success path (both gates of step 8 open, so the
    // widest possible write footprint is exercised): every region a NEIGHBOURING index that this call
    // never addresses reads back exactly as seeded.
    // =================================================================================================
    {
        Seed s;
        s.goal_x             = 71;
        s.goal_y             = 72;
        s.player_id          = 31;
        s.member_count       = 2;
        s.is_plain_move_flag = 5;
        s.target_class       = 1;
        s.map_width          = 200;
        s.members            = {{1, 2, 1, 1}, {3, 4, 1, 1}};
        s.planner_tiles      = {{5, 6}, {7, 8}};
        seed_and_run(fx, s);

        ck(fx.group_route_steps[1].dir_code == SENT_ROUTE1_DIR &&
               fx.group_route_steps[1].run_length == SENT_ROUTE1_RUN,
           "T9: route_step[1] (neighbour of the written route_step[0]) untouched");
        ck(member_untouched(fx, GUARD_MEMBER_IDX), "T9: group_members[70] (past member_count) untouched");
        ck(scratch_guard_untouched(fx), "T9: group_move_scratch[90] (past member_count) untouched");
    }
}

} // namespace mh::sim::test
