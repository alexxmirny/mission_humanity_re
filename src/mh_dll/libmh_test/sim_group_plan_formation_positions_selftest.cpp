#include "sim/sim_group_plan_formation_positions.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: proves CALL ORDER across all 5 callees -------------------------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// Own local copy of the .cpp's pack_xy, cross-checked against the .asm's CONCAT11(hi,lo) packing
// (hi = x in bits 8-15, lo = y in bits 0-7) independently here -- see 0x0041de1f-0x0041de2b /
// 0x0041dedd-0x0041dee2 (anchor/member packing) and 0x0041e604-0x0041e623 (member/found packing), all
// of which push a byte-truncated x into the high half and a byte-truncated y into the low half.
uint16_t pack_xy(int32_t x, int32_t y) {
    return static_cast<uint16_t>((static_cast<uint16_t>(static_cast<uint8_t>(x)) << 8) |
                                 static_cast<uint8_t>(y));
}

// ---- per-callee recorders (5, one per group_plan_formation_positions_calls member) -------------------

struct TileDistCall {
    int32_t x1, y1, x2, y2;
};
std::vector<TileDistCall> g_tile_dist_calls;
int32_t                   rec_tile_dist_wrapped(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    tr("tile_dist_wrapped");
    g_tile_dist_calls.push_back({x1, y1, x2, y2});
    return 0; // return is discarded by the caller regardless -- see the .h's own derivation
}

struct BuildStepsCall {
    int32_t  start_x, start_y, target_range;
    uint32_t unused_reserved;
    int32_t  path_slot_index;
};
std::vector<BuildStepsCall> g_build_steps_calls;
std::vector<int32_t>        g_build_steps_ret_queue;
size_t                      g_build_steps_ret_pos = 0;
int32_t                     rec_pathfind_build_steps(int32_t start_x, int32_t start_y, int32_t target_range,
                                                     uint32_t unused_reserved, int32_t path_slot_index) {
    tr("pathfind_build_steps");
    g_build_steps_calls.push_back({start_x, start_y, target_range, unused_reserved, path_slot_index});
    return (g_build_steps_ret_pos < g_build_steps_ret_queue.size())
                                   ? g_build_steps_ret_queue[g_build_steps_ret_pos++]
                                   : 0;
}

struct FloodCall {
    int32_t query_cell, start_cell;
};
std::vector<FloodCall> g_flood_calls;
int32_t                rec_region_flood_reachable(int32_t query_cell, int32_t start_cell) {
    tr("region_flood_reachable");
    g_flood_calls.push_back({query_cell, start_cell});
    return 0; // return is discarded by the caller in BOTH call sites
}

struct RouteSearchCall {
    uint16_t start_region;
    int16_t  target_region;
};
std::vector<RouteSearchCall>         g_route_search_calls;
std::vector<std::vector<route_step>> g_route_search_write_queue;
size_t                               g_route_search_write_pos = 0;
int32_t                              rec_region_route_search(uint16_t start_region, int16_t target_region, uint8_t *out_path) {
    tr("region_route_search");
    g_route_search_calls.push_back({start_region, target_region});
    if (g_route_search_write_pos < g_route_search_write_queue.size()) {
        const auto &steps = g_route_search_write_queue[g_route_search_write_pos++];
        // committed llm_map_region_route_search spells this out-param uint8_t * (TACT1-P C6,
        // 2026-09-04); the mock's payload is really an array of route_step, so re-interpret it.
        auto *dst = reinterpret_cast<route_step *>(out_path);
        for (size_t i = 0; i < steps.size(); ++i) dst[i] = steps[i];
    }
    return 0; // discarded by the caller
}

int32_t g_guard_calls_count = 0;
void    rec_stack_capacity_guard_0x20() {
    tr("stack_capacity_guard_0x20");
    ++g_guard_calls_count;
}

const group_plan_formation_positions_calls g_calls = {
    &rec_tile_dist_wrapped,
    &rec_pathfind_build_steps,
    &rec_region_flood_reachable,
    &rec_region_route_search,
    &rec_stack_capacity_guard_0x20,
};

void reset_observations() {
    g_trace.clear();
    g_tile_dist_calls.clear();
    g_build_steps_calls.clear();
    g_build_steps_ret_queue.clear();
    g_build_steps_ret_pos = 0;
    g_flood_calls.clear();
    g_route_search_calls.clear();
    g_route_search_write_queue.clear();
    g_route_search_write_pos = 0;
    g_guard_calls_count      = 0;
}

// Resets the fixture, gives it a REAL wrap mask (0xff -- wrap_mask=0 from a bare fx.reset() would
// collapse every diamond-scan coordinate onto a single cell and make T6 untestable), and clears every
// recorder/control global. Call this FIRST in every test block.
void reset_and_seed(sim_fixture &fx) {
    fx.reset();
    fx.path_wrap_mask = 0xff;
    reset_observations();
}

} // namespace

void run_group_plan_formation_positions_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- the per-member loop (0x0041dc20-0x0041dc36: CMP vs GROUP_MEMBER_COUNT at 0x0041dc23, JMP
    // exit 0x0041dc2b, increment+continue 0x0041dc30-36) reading group_move_scratch[i] fresh each
    // iteration (snapshot at 0x0041dc38-0x0041dc4f) and calling llm_strat_tile_dist_wrapped
    // (0x0041dc52-0x0041dc64) with (member.tile_col, member.tile_row, *anchor_x, *anchor_y) BEFORE any
    // band logic. All 3 members here have no enabled weapons (fixture default), so every one also falls
    // through the cheap-default else-arm (0x0041dd85-0x0041ddaf) -- doubles as one T3/T4-style case.
    // =================================================================================================
    {
        reset_and_seed(fx);
        fx.group_anchor_x                 = 100;
        fx.group_anchor_y                 = 200;
        fx.group_member_count             = 3;
        fx.group_move_scratch[0].tile_col = 10;
        fx.group_move_scratch[0].tile_row = 20;
        fx.group_move_scratch[0].unit_idx = 5;
        fx.group_move_scratch[1].tile_col = 30;
        fx.group_move_scratch[1].tile_row = 40;
        fx.group_move_scratch[1].unit_idx = 6;
        fx.group_move_scratch[2].tile_col = 50;
        fx.group_move_scratch[2].tile_row = 60;
        fx.group_move_scratch[2].unit_idx = 7;
        fx.view_cur_player                = 1;

        sim_store own = fx.store();
        detail::group_plan_formation_positions(fx.view(), own, g_calls, /*target_type_mask=*/0);

        ck_eq((uint32_t)g_tile_dist_calls.size(), 3u,
              "T1: exactly group_member_count(3) tile_dist_wrapped calls (0x0041dc20-0x0041dc36 loop bound)");
        const int32_t cols[3] = {10, 30, 50}, rows[3] = {20, 40, 60};
        for (int i = 0; i < 3; ++i) {
            ck(g_tile_dist_calls[i].x1 == cols[i] && g_tile_dist_calls[i].y1 == rows[i] &&
                   g_tile_dist_calls[i].x2 == 100 && g_tile_dist_calls[i].y2 == 200,
               "T1: per-member tile_dist_wrapped(member.tile_col, member.tile_row, anchor_x, anchor_y) "
               "(0x0041dc38-0x0041dc64), member read from group_move_scratch[i]");
            ck_eq((uint32_t)fx.group_member_tile[i * 2 + 0], (uint32_t)cols[i],
                  "T1/T3-else: GROUP_MEMBER_TILE[i].col = member's own tile_col, no usable weapon band "
                  "(0x0041dd85-94)");
            ck_eq((uint32_t)fx.group_member_tile[i * 2 + 1], (uint32_t)rows[i],
                  "T1/T3-else: GROUP_MEMBER_TILE[i].row = member's own tile_row (0x0041dd9a-a9)");
        }
        ck(trace_eq({"tile_dist_wrapped", "tile_dist_wrapped", "tile_dist_wrapped"}),
           "T1: no other callee fires when every member takes the no-band cheap-default arm");
    }

    // =================================================================================================
    // T2 -- the weapon-band scan (0x0041dc81-0x0041dd7d): 4 slots, mixed enabled/disabled and
    // matching/non-matching target mask, proving BOTH filters (0x0041dcb7 enabled_2, 0x0041dcf1-dd02
    // target_type_mask&Weapon.target) are real, and that MIN/MAX (0x0041dd1d-3b /0x0041dd57-75) are
    // taken across only the counted slots. Verified via pathfind_build_steps's own recorded arguments
    // (0x0041ddb4-e5: EBX=range_max_band precedes ECX=range_min_band, matching the committed
    // prototype's (target_range, unused_reserved) order). This case also exercises T4 (build_result!=0
    // -- sentinel pre-seed proves GROUP_MEMBER_TILE is NOT touched on the success arm, 0x0041ddea-ec).
    // =================================================================================================
    {
        reset_and_seed(fx);
        const uint16_t player             = 3;
        fx.group_anchor_x                 = 1;
        fx.group_anchor_y                 = 2;
        fx.group_member_count             = 1;
        fx.group_move_scratch[0].tile_col = 5;
        fx.group_move_scratch[0].tile_row = 6;
        fx.group_move_scratch[0].unit_idx = 2;
        fx.view_cur_player                = player;

        unit &u                = fx.u(player, 2);
        u.weapons[0].weapon_id = 10;
        u.weapons[0].enabled_2 = 1; // counts
        u.weapons[1].weapon_id = 11;
        u.weapons[1].enabled_2 = 0; // DISABLED -- must not count even though its range is more extreme
        u.weapons[2].weapon_id = 12;
        u.weapons[2].enabled_2 = 1; // enabled but target-mask MISMATCH -- must not count
        u.weapons[3].weapon_id = 13;
        u.weapons[3].enabled_2 = 1; // counts, lower min AND higher max than slot 0
        u.path_slot_id         = 44;

        fx.cfg_weapons[10].target            = 0x01;
        fx.cfg_weapons[10].range_min[player] = 5;
        fx.cfg_weapons[10].range_max[player] = 50;
        fx.cfg_weapons[11].target            = 0x01;
        fx.cfg_weapons[11].range_min[player] = 1;    // would win the min if counted -- must NOT
        fx.cfg_weapons[11].range_max[player] = 99;   // would win the max if counted -- must NOT
        fx.cfg_weapons[12].target            = 0x02; // mismatches mask 0x01
        fx.cfg_weapons[12].range_min[player] = 2;
        fx.cfg_weapons[12].range_max[player] = 200;
        fx.cfg_weapons[13].target            = 0x01;
        fx.cfg_weapons[13].range_min[player] = 3;  // < slot0's 5 -> new min
        fx.cfg_weapons[13].range_max[player] = 80; // > slot0's 50 -> new max

        g_build_steps_ret_queue = {1};  // success -- skip the whole region/diamond block (T4)
        fx.group_member_tile[0] = 0x77; // sentinel -- proves the success arm does NOT write
        fx.group_member_tile[1] = 0x88;

        sim_store own = fx.store();
        detail::group_plan_formation_positions(fx.view(), own, g_calls, /*target_type_mask=*/0x01);

        ck_eq((uint32_t)g_build_steps_calls.size(), 1u,
              "T2: band 3<80 is usable -- pathfind_build_steps IS called (0x0041ddb4-e5)");
        ck(g_build_steps_calls[0].start_x == 5 && g_build_steps_calls[0].start_y == 6,
           "T2: pathfind_build_steps(start_x=x1, start_y=y1, ...) (0x0041ddd9-e2)");
        ck_eq((uint32_t)g_build_steps_calls[0].target_range, 80u,
              "T2: target_range(EBX) = range_max_band = MAX across only the counted slots (13's 80, not "
              "11's excluded 99 or 12's excluded 200) (0x0041dddc)");
        ck_eq(g_build_steps_calls[0].unused_reserved, 3u,
              "T2: unused_reserved(ECX) = range_min_band = MIN across only the counted slots (13's 3, "
              "not 0's raw 5, not 11's excluded 1) (0x0041ddd9)");
        ck_eq((uint32_t)g_build_steps_calls[0].path_slot_index, 44u,
              "T2: path_slot_index = unit.path_slot_id (0x0041ddb4-d8)");
        ck_eq((uint32_t)fx.group_member_tile[0], 0x77u,
              "T4: build_result!=0 -- GROUP_MEMBER_TILE untouched, sentinel survives (0x0041ddea-ec JNZ "
              "straight to loop increment)");
        ck_eq((uint32_t)fx.group_member_tile[1], 0x88u, "T4: same for the row byte");
        ck(trace_eq({"tile_dist_wrapped", "pathfind_build_steps"}),
           "T4: no flood_reachable/route_search/guard call on the build-success arm");
    }

    // =================================================================================================
    // T3 -- the local_34<local_30 branch's exact boundary (0x0041dd7d-83: CMP+JL, strict `<` not `<=`)
    // and the degenerate no-weapons-at-all case (min stays 0xff, max stays 0 -- 0xff<0 is false too).
    // =================================================================================================
    {
        // T3a: range_min_band == range_max_band == 10 (EQUAL, not less) -- cheap default, not a band.
        reset_and_seed(fx);
        const uint16_t player                = 1;
        fx.group_anchor_x                    = 9;
        fx.group_anchor_y                    = 9;
        fx.group_member_count                = 1;
        fx.group_move_scratch[0].tile_col    = 7;
        fx.group_move_scratch[0].tile_row    = 8;
        fx.group_move_scratch[0].unit_idx    = 3;
        fx.view_cur_player                   = player;
        unit &u                              = fx.u(player, 3);
        u.weapons[0].weapon_id               = 20;
        u.weapons[0].enabled_2               = 1;
        fx.cfg_weapons[20].target            = 0x01;
        fx.cfg_weapons[20].range_min[player] = 10;
        fx.cfg_weapons[20].range_max[player] = 10;

        sim_store own = fx.store();
        detail::group_plan_formation_positions(fx.view(), own, g_calls, /*target_type_mask=*/0x01);

        ck_eq((uint32_t)fx.group_member_tile[0], 7u,
              "T3a: range_min_band(10) < range_max_band(10) is FALSE (equal) -- cheap default = own tile "
              "(0x0041dd7d-83 CMP/JL, strict less-than)");
        ck_eq((uint32_t)fx.group_member_tile[1], 8u, "T3a: same for the row byte");
        ck(trace_eq({"tile_dist_wrapped"}), "T3a: no band -> no pathfind_build_steps call at all");
    }
    {
        // T3b: no weapon slots enabled at all -- min stays the 0xff seed, max stays the 0 seed.
        reset_and_seed(fx);
        fx.group_anchor_x                 = 0;
        fx.group_anchor_y                 = 0;
        fx.group_member_count             = 1;
        fx.group_move_scratch[0].tile_col = 15;
        fx.group_move_scratch[0].tile_row = 16;
        fx.group_move_scratch[0].unit_idx = 0;
        fx.view_cur_player                = 0; // fx.u(0,0) already has every weapons[] slot disabled

        sim_store own = fx.store();
        detail::group_plan_formation_positions(fx.view(), own, g_calls, /*target_type_mask=*/0xff);

        ck_eq((uint32_t)fx.group_member_tile[0], 15u,
              "T3b: zero counted slots -- range_min_band stays 0xff (seed, 0x0041dc6c), range_max_band "
              "stays 0 (seed, 0x0041dc73); 0xff<0 is false -- cheap default = own tile");
        ck_eq((uint32_t)fx.group_member_tile[1], 16u, "T3b: same for the row byte");
        ck(trace_eq({"tile_dist_wrapped"}), "T3b: no band -> no pathfind_build_steps call");
    }

    // =================================================================================================
    // T5 -- the reachability pre-pass (0x0041ddf2-0x0041deae), reached via build_result==0
    // (0x0041ddea-ec). Walk `region_list_head`->next clearing EVERY region's route_mark (0x0041ddf2-
    // 0x0041de1d), then set route_mark=1 on the member tile's own region (0x0041de1f-4e) AND each of
    // ITS neighbors (0x0041de5f-ae) -- NOT every region in the active list. A 4-node graph proves the
    // {target, neighbor1, neighbor2} vs {non-neighbor-but-in-list} split exactly.
    // =================================================================================================
    {
        reset_and_seed(fx);
        const uint16_t player               = 0;
        fx.group_anchor_x                   = 4;
        fx.group_anchor_y                   = 4;
        fx.group_member_count               = 1;
        fx.group_move_scratch[0].tile_col   = 9;
        fx.group_move_scratch[0].tile_row   = 9;
        fx.group_move_scratch[0].unit_idx   = 1;
        fx.view_cur_player                  = player;
        unit &u                             = fx.u(player, 1);
        u.weapons[0].weapon_id              = 1;
        u.weapons[0].enabled_2              = 1;
        fx.cfg_weapons[1].target            = 0x01;
        fx.cfg_weapons[1].range_min[player] = 1;
        fx.cfg_weapons[1].range_max[player] = 2;
        g_build_steps_ret_queue             = {0}; // fail -> reach the region block

        llm_map_region T{}, NB1{}, NB2{}, OTHER{};
        T.neighbor_count    = 2;
        T.neighbors[0]      = &NB1;
        T.neighbors[1]      = &NB2;
        T.route_mark        = 9; // sentinel, distinct from both 0 (cleared) and 1 (set)
        NB1.route_mark      = 9;
        NB2.route_mark      = 9;
        OTHER.route_mark    = 9; // in the active list, NOT a neighbor of T
        T.next              = &NB1;
        NB1.next            = &NB2;
        NB2.next            = &OTHER;
        OTHER.next          = nullptr;
        fx.region_list_head = &T;

        // Sentinel on the member tile too -- with region_grid all zero (fixture default) the diamond
        // scans find nothing, so this also independently exercises T7's "tile unchanged" claim.
        fx.group_member_tile[0] = 0xab;
        fx.group_member_tile[1] = 0xcd;

        sim_store own                   = fx.store();
        own.region_cell_at(9, 9).region = &T;
        detail::group_plan_formation_positions(fx.view(), own, g_calls, /*target_type_mask=*/0x01);

        ck_eq((uint32_t)T.route_mark, 1u,
              "T5: member tile's own region gets route_mark=1 (0x0041de3a-4e), after the clear pass "
              "(0x0041de10-13) zeroed every region in the active list first");
        ck_eq((uint32_t)NB1.route_mark, 1u, "T5: neighbors[0] also marked (0x0041de85-a1/0x0041dea4)");
        ck_eq((uint32_t)NB2.route_mark, 1u, "T5: neighbors[1] also marked (same loop, neighbor_count=2)");
        ck_eq((uint32_t)OTHER.route_mark, 0u,
              "T5: a region reachable via the active list's ->next chain but NOT one of T's neighbors is "
              "CLEARED (0x0041de10-13) and left at 0 -- the set-pass (0x0041de5f-ae) only walks "
              "neighbors[0..neighbor_count), not the whole active list");
        ck_eq((uint32_t)g_guard_calls_count, 2u,
              "T5 tail: region_grid all-zero -- both diamond scans find nothing, so the nothing-found "
              "arm's two stack_capacity_guard_0x20 calls fire (0x0041e5ef/0x0041e5fa)");
        ck_eq((uint32_t)fx.group_member_tile[0], 0xabu,
              "T5/T7: nothing-found arm never writes GROUP_MEMBER_TILE -- sentinel survives");
        ck_eq((uint32_t)fx.group_member_tile[1], 0xcdu, "T5/T7: same for the row byte");
        ck(trace_eq({"tile_dist_wrapped", "pathfind_build_steps", "region_flood_reachable",
                     "stack_capacity_guard_0x20", "stack_capacity_guard_0x20"}),
           "T5: exact call order through the nothing-found tail -- only ONE flood_reachable call "
           "(the reachability pre-pass at 0x0041dee2); the found-arm's second one never fires");
    }

    // =================================================================================================
    // T7 -- the "nothing found" arm in isolation (0x0041e5db-0x0041e5ff), with NO region graph at all
    // (region_list_head=nullptr) so this is independent of T5's route_mark setup.
    // =================================================================================================
    {
        reset_and_seed(fx);
        const uint16_t player               = 0;
        fx.group_anchor_x                   = 50;
        fx.group_anchor_y                   = 50;
        fx.group_member_count               = 1;
        fx.group_move_scratch[0].tile_col   = 1;
        fx.group_move_scratch[0].tile_row   = 1;
        fx.group_move_scratch[0].unit_idx   = 0;
        fx.view_cur_player                  = player;
        unit &u                             = fx.u(player, 0);
        u.weapons[0].weapon_id              = 2;
        u.weapons[0].enabled_2              = 1;
        fx.cfg_weapons[2].target            = 0x01;
        fx.cfg_weapons[2].range_min[player] = 0;
        fx.cfg_weapons[2].range_max[player] = 1;
        g_build_steps_ret_queue             = {0};
        fx.region_list_head                 = nullptr; // no active list; region_grid stays all-zero too

        fx.group_member_tile[0] = 0x11;
        fx.group_member_tile[1] = 0x22;

        sim_store own = fx.store();
        detail::group_plan_formation_positions(fx.view(), own, g_calls, /*target_type_mask=*/0x01);

        ck_eq((uint32_t)g_guard_calls_count, 2u,
              "T7: local_28(best)==0xffffff sentinel survives both diamond scans (region_grid all-zero, "
              "every candidate rejected as terrain==0) -- CMP/JNZ at 0x0041e5db-e2 takes the guard arm");
        ck(trace_eq({"tile_dist_wrapped", "pathfind_build_steps", "region_flood_reachable",
                     "stack_capacity_guard_0x20", "stack_capacity_guard_0x20"}),
           "T7: exact trace -- two guard calls (0x0041e5ef/0x0041e5fa), no second flood_reachable/"
           "route_search");
        ck_eq((uint32_t)fx.group_member_tile[0], 0x11u,
              "T7: GROUP_MEMBER_TILE untouched on the nothing-found arm -- sentinel survives (no write "
              "instruction exists between 0x0041e5db and the JMP back to the loop at 0x0041e5ff)");
        ck_eq((uint32_t)fx.group_member_tile[1], 0x22u, "T7: same for the row byte");
    }

    // =================================================================================================
    // T6 -- the two diamond scans (0x0041def8-0x0041e266 radius=range_min_band, 0x0041e266-0x0041e5db
    // radius=range_max_band). Each candidate's terrain_flags>>8 replaces `best` iff candidate<=best AND
    // candidate!=0 (the JA-skip at e.g. 0x0041df41-44 rejects candidate>best; the JZ-skip at e.g.
    // 0x0041df7e-80 rejects candidate==0) -- proving: zero is always rejected, a TIE (candidate==best)
    // still replaces (so the LATER-evaluated candidate wins), and the second (larger-radius) scan CAN
    // still improve on the first scan's winner with a strictly lower value.
    // =================================================================================================
    {
        reset_and_seed(fx);
        const uint16_t player               = 2;
        fx.group_anchor_x                   = 100;
        fx.group_anchor_y                   = 100;
        fx.group_member_count               = 1;
        fx.group_move_scratch[0].tile_col   = 50;
        fx.group_move_scratch[0].tile_row   = 60;
        fx.group_move_scratch[0].unit_idx   = 4;
        fx.view_cur_player                  = player;
        unit &u                             = fx.u(player, 4);
        u.weapons[0].weapon_id              = 3;
        u.weapons[0].enabled_2              = 1;
        fx.cfg_weapons[3].target            = 0x01;
        fx.cfg_weapons[3].range_min[player] = 1; // radius of the FIRST scan
        fx.cfg_weapons[3].range_max[player] = 5; // radius of the SECOND scan
        g_build_steps_ret_queue             = {0};
        fx.region_list_head                 = nullptr;

        // radius=1 scan, i=0: arm1 consider(ax-1+0,ay-0)=(99,100), arm2 consider(99,100) [same cell,
        // harmless re-tie], arm3 consider(ax+1-0,ay-0)=(101,100) THEN arm4 consider(101,100) [same] --
        // (99,100) and (101,100) are the ONLY two distinct cells radius=1 ever evaluates at i=0 (i=1's
        // four arms all collapse onto (100,99)/(100,101), left at 0 here and rejected). Both seeded
        // terrain=5, a TIE; per the <= compare (0x0041dfe2 arm3 comes AFTER arm1/arm2 in evaluation
        // order), arm3's LATER (101,100) must overwrite arm1's earlier (99,100).
        fx.region_grid[99 * MAP_GRID_DIM + 100].terrain_flags  = 5u << 8;
        fx.region_grid[101 * MAP_GRID_DIM + 100].terrain_flags = 5u << 8;
        // radius=5 scan, i=1 arm1: consider(ax-5+1,ay-1)=(96,99), terrain=3 -- STRICTLY LOWER than the
        // radius-1 winner's 5, proving the second (larger-radius) scan is not just ignored once radius1
        // finds something. (96,99) is Manhattan-distance 5 from the anchor, never touched by radius=1.
        fx.region_grid[96 * MAP_GRID_DIM + 99].terrain_flags = 3u << 8;
        // Every other cell evaluated by either scan stays 0 (fixture default) -- rejected.

        sim_store own = fx.store();
        detail::group_plan_formation_positions(fx.view(), own, g_calls, /*target_type_mask=*/0x01);

        ck_eq((uint32_t)fx.group_member_tile[0], 96u,
              "T6: final found_x=96 -- the radius-5 scan's terrain=3 candidate beat the radius-1 scan's "
              "terrain=5 tie-winner (0x0041e42c-63 arm, second scan)");
        ck_eq((uint32_t)fx.group_member_tile[1], 99u, "T6: final found_y=99");
        ck(g_flood_calls.size() == 2 &&
               g_flood_calls[1].query_cell == (int32_t)pack_xy(50, 60) &&
               g_flood_calls[1].start_cell == (int32_t)pack_xy(96, 99),
           "T6: found-arm region_flood_reachable(member=pack(50,60), found=pack(96,99)) "
           "(0x0041e604-29)");
        ck(g_route_search_calls.size() == 1 &&
               g_route_search_calls[0].start_region == pack_xy(50, 60) &&
               (uint16_t)g_route_search_calls[0].target_region == pack_xy(96, 99),
           "T6: region_route_search(start=member, target=found) (0x0041e634-3a, EAX=member/EDX=found)");
    }

    // =================================================================================================
    // T8/T9 -- the found arm's full tail (0x0041e604-0x0041e781): flood_reachable + route_search
    // argument packing/order, the path-buffer APPEND walk (0x0041e670-9e finds the first zero
    // run_length, 0x0041e6a5-1b copies the route WITHOUT touching earlier entries, 0x0041e71d-5e
    // re-terminates), and the final GROUP_MEMBER_TILE write (0x0041e765-81, the FOUND tile).
    // =================================================================================================
    {
        reset_and_seed(fx);
        const uint16_t player               = 1;
        const int32_t  slot                 = 3;
        fx.group_anchor_x                   = 70;
        fx.group_anchor_y                   = 80;
        fx.group_member_count               = 1;
        fx.group_move_scratch[0].tile_col   = 10;
        fx.group_move_scratch[0].tile_row   = 20;
        fx.group_move_scratch[0].unit_idx   = 6;
        fx.view_cur_player                  = player;
        unit &u                             = fx.u(player, 6);
        u.weapons[0].weapon_id              = 4;
        u.weapons[0].enabled_2              = 1;
        u.path_slot_id                      = (uint8_t)slot;
        fx.cfg_weapons[4].target            = 0x01;
        fx.cfg_weapons[4].range_min[player] = 0; // radius=0 -> the diamond degenerates to the anchor cell
        fx.cfg_weapons[4].range_max[player] = 1;
        g_build_steps_ret_queue             = {0};
        fx.region_list_head                 = nullptr;

        // radius=0 scan hits (anchor_x,anchor_y) itself (4 identical considers); seed it so found is
        // deterministically the anchor tile. radius=1 ring left at 0 (rejected) so it cannot change it.
        fx.region_grid[70 * MAP_GRID_DIM + 80].terrain_flags = 7u << 8;

        g_route_search_write_queue = {{route_step{/*dir_code=*/5, /*run_length=*/7},
                                       route_step{/*dir_code=*/9, /*run_length=*/2}}};

        sim_store own = fx.store();
        // Pre-existing path-buffer entries at slot 3: two real waypoints, then a zero terminator (the
        // append point), then two SENTINEL slots proving the copy+re-terminate writes land exactly
        // there, then a GUARD slot one past the terminator proving the write does not run past it.
        own.path_buffer_at(player, slot, 0).heading    = 1;
        own.path_buffer_at(player, slot, 0).run_length = 50;
        own.path_buffer_at(player, slot, 1).heading    = 2;
        own.path_buffer_at(player, slot, 1).run_length = 60;
        own.path_buffer_at(player, slot, 2).heading    = 99;
        own.path_buffer_at(player, slot, 2).run_length = 0; // append point
        own.path_buffer_at(player, slot, 3).heading    = 222;
        own.path_buffer_at(player, slot, 3).run_length = 111; // sentinel
        own.path_buffer_at(player, slot, 4).heading    = 88;
        own.path_buffer_at(player, slot, 4).run_length = 77; // sentinel
        own.path_buffer_at(player, slot, 5).heading    = 45;
        own.path_buffer_at(player, slot, 5).run_length = 123; // GUARD

        detail::group_plan_formation_positions(fx.view(), own, g_calls, /*target_type_mask=*/0x01);

        ck(g_flood_calls.size() == 2 &&
               g_flood_calls[0].query_cell == (int32_t)pack_xy(70, 80) &&
               g_flood_calls[0].start_cell == (int32_t)pack_xy(10, 20),
           "T8: reachability pre-pass region_flood_reachable(anchor=pack(70,80), member=pack(10,20)) "
           "(0x0041deb0-e7)");
        ck(g_flood_calls[1].query_cell == (int32_t)pack_xy(10, 20) &&
               g_flood_calls[1].start_cell == (int32_t)pack_xy(70, 80),
           "T8: found-arm region_flood_reachable(member=pack(10,20), found=pack(70,80)) -- ARGUMENT "
           "ORDER SWAPPED vs the first call (0x0041e604-29)");
        ck(g_route_search_calls.size() == 1 &&
               g_route_search_calls[0].start_region == pack_xy(10, 20) &&
               (uint16_t)g_route_search_calls[0].target_region == pack_xy(70, 80),
           "T8: region_route_search(start=member, target=found) (0x0041e62f-3a)");

        auto pb = [&](int32_t entry) { return own.path_buffer_at(player, slot, entry); };
        ck(pb(0).run_length == 50 && pb(0).heading == 1, "T8: entry 0 UNCHANGED -- append, not overwrite");
        ck(pb(1).run_length == 60 && pb(1).heading == 2, "T8: entry 1 UNCHANGED -- append starts past it");
        ck(pb(2).run_length == 7 && pb(2).heading == 5,
           "T8: entry 2 (the first zero-run_length slot, 0x0041e670-9e) = copied route[0] "
           "(run_length<-route.run_length, heading<-route.dir_code) (0x0041e6a5-1b)");
        ck(pb(3).run_length == 2 && pb(3).heading == 9, "T8: entry 3 = copied route[1]");
        ck(pb(4).run_length == 0 && pb(4).heading == 0,
           "T8: entry 4 = the re-terminate write (0x0041e71d-5e), overwriting the SENTINEL {77,88}");
        ck(pb(5).run_length == 123 && pb(5).heading == 45,
           "T10-style guard: entry 5 (one past the terminator) UNCHANGED -- the write does not run past "
           "the terminator slot");

        ck_eq((uint32_t)fx.group_member_tile[0], 70u,
              "T9: GROUP_MEMBER_TILE[slot].col = found_x(70), NOT the member's own tile_col(10) "
              "(0x0041e765-6d)");
        ck_eq((uint32_t)fx.group_member_tile[1], 80u,
              "T9: GROUP_MEMBER_TILE[slot].row = found_y(80), NOT the member's own tile_row(20) "
              "(0x0041e773-7b)");
        ck(trace_eq({"tile_dist_wrapped", "pathfind_build_steps", "region_flood_reachable",
                     "region_flood_reachable", "region_route_search"}),
           "T8: exact call order through the found arm");
    }

    // =================================================================================================
    // T10 -- non-corruption. A single found-arm scenario (same shape as T8) plus THREE guards: a region
    // node in the active list but NOT the target's region or a neighbor (route_mark must end up
    // CLEARED, never SET), a path-buffer slot for a DIFFERENT (player,slot) pair, and a
    // GROUP_MEMBER_TILE slot for an index past group_member_count. All three carry a sentinel value the
    // function has no reason to produce, so an unchanged read-back is a real proof, not a coincidence.
    // =================================================================================================
    {
        reset_and_seed(fx);
        const uint16_t player               = 5;
        const int32_t  slot                 = 8;
        fx.group_anchor_x                   = 33;
        fx.group_anchor_y                   = 44;
        fx.group_member_count               = 1;
        fx.group_move_scratch[0].tile_col   = 1;
        fx.group_move_scratch[0].tile_row   = 2;
        fx.group_move_scratch[0].unit_idx   = 9;
        fx.view_cur_player                  = player;
        unit &u                             = fx.u(player, 9);
        u.weapons[0].weapon_id              = 6;
        u.weapons[0].enabled_2              = 1;
        u.path_slot_id                      = (uint8_t)slot;
        fx.cfg_weapons[6].target            = 0x01;
        fx.cfg_weapons[6].range_min[player] = 0;
        fx.cfg_weapons[6].range_max[player] = 1;
        g_build_steps_ret_queue             = {0};

        llm_map_region T{}, GUARD_REGION{};
        T.neighbor_count        = 0; // no neighbors -- isolates the guard from T5's own coverage
        T.route_mark            = 9;
        GUARD_REGION.route_mark = 9; // sentinel -- in the active list, NOT the target's region
        T.next                  = &GUARD_REGION;
        GUARD_REGION.next       = nullptr;
        fx.region_list_head     = &T;

        fx.region_grid[33 * MAP_GRID_DIM + 44].terrain_flags = 2u << 8; // radius=0 hits the anchor cell
        g_route_search_write_queue                           = {{route_step{/*dir_code=*/1, /*run_length=*/1}}};

        sim_store own                                  = fx.store();
        own.region_cell_at(1, 2).region                = &T;
        own.path_buffer_at(player, slot, 0).heading    = 3;
        own.path_buffer_at(player, slot, 0).run_length = 20;
        own.path_buffer_at(player, slot, 1).heading    = 0;
        own.path_buffer_at(player, slot, 1).run_length = 0; // append point

        // The three guards -- addressed nowhere by this member_count=1 call.
        constexpr uint16_t GUARD_PLAYER = 6, GUARD_SLOT = 8;
        own.path_buffer_at(GUARD_PLAYER, GUARD_SLOT, 0).heading    = 202;
        own.path_buffer_at(GUARD_PLAYER, GUARD_SLOT, 0).run_length = 201;
        fx.group_member_tile[100]                                  = 0x9a; // slot 50's col byte
        fx.group_member_tile[101]                                  = 0x9b; // slot 50's row byte

        detail::group_plan_formation_positions(fx.view(), own, g_calls, /*target_type_mask=*/0x01);

        ck_eq((uint32_t)T.route_mark, 1u,
              "T10: the target's own region IS set (0x0041de3a-4e) -- sanity check that the guard below "
              "is really testing the neighbor boundary, not a no-op run");
        ck_eq((uint32_t)GUARD_REGION.route_mark, 0u,
              "T10 guard: a region reachable via ->next but not the target's region or a neighbor is "
              "CLEARED by the walk (0x0041de10-13) and never SET -- sentinel 9 does not survive");
        auto guard_pb = own.path_buffer_at(GUARD_PLAYER, GUARD_SLOT, 0);
        ck(guard_pb.run_length == 201 && guard_pb.heading == 202,
           "T10 guard: path_buffer_at(DIFFERENT player/slot, 0) unchanged -- the append walk only "
           "touches (player=5, slot=8)");
        ck_eq((uint32_t)fx.group_member_tile[100], 0x9au,
              "T10 guard: GROUP_MEMBER_TILE slot 50 (index past group_member_count=1) unchanged");
        ck_eq((uint32_t)fx.group_member_tile[101], 0x9bu, "T10 guard: same for its row byte");
    }
}

} // namespace mh::sim::test
