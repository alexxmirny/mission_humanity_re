//
// sim_pathfind_geom_misc_selftest.cpp -- `simtest` offline oracle for the four functions in this
// slice that are NOT SHADOWABLE (0 tracked write cells, per the write-closure derivation -- see
// each header's own banner), so this is their only execution-proof evidence:
//   llm_strat_pathfind_mark_group_member_regions (sim/sim_pathfind_geom_misc.h)
//   llm_strat_pathfind_target_hook_stub           (sim/sim_pathfind_geom_misc.h)
//   llm_strat_dir_step_toroidal_dist               (sim/sim_pathfind_geom_misc.h)
//   llm_strat_pathtrace_dirs_get                    (sim/sim_pathtrace_dir_table.h)
//   llm_strat_path_alloc_slot                       (sim/sim_path_alloc_slot.h -- "shadowable only
//                                                     through a callee", so also offline-only here)
//
// All 5 were adversarially reviewed clean (reimpl-verify.js, RI-SIM / SIM1-G2,
// 2026-08-21) before this oracle was written.
//
#include "sim/sim_pathfind_geom_misc.h"
#include "sim/sim_pathtrace_dir_table.h"
#include "sim/sim_path_alloc_slot.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

} // namespace

void run_pathfind_geom_misc_tests() {
    // ---- llm_strat_pathfind_mark_group_member_regions: void, writes region->route_mark=2 through --
    // a heap pointer for every member whose current tile resolves to a region; members whose tile
    // has no region (region_cell_at(...).region == nullptr) must be skipped without touching memory.
    {
        sim_fixture fx;
        fx.reset();
        sim_store own = fx.store();

        llm_map_region node_a{};
        llm_map_region node_b{};
        node_a.route_mark = 0;
        node_b.route_mark = 0;

        fx.group_member_count       = 3;
        fx.group_members[0].cur_col = 5;
        fx.group_members[0].cur_row = 10;
        fx.group_members[1].cur_col = 7;
        fx.group_members[1].cur_row = 20;
        fx.group_members[2].cur_col = 200; // no region at this tile -- must be skipped, not crash
        fx.group_members[2].cur_row = 200;

        own.region_cell_at(5, 10).region    = &node_a;
        own.region_cell_at(7, 20).region    = &node_b;
        own.region_cell_at(200, 200).region = nullptr;

        sim_view v = fx.view();
        detail::pathfind_mark_group_member_regions(v, own);

        ck_eq((uint32_t)node_a.route_mark, 2u,
              "mark_group_member_regions: member 0's region gets route_mark=2");
        ck_eq((uint32_t)node_b.route_mark, 2u,
              "mark_group_member_regions: member 1's region gets route_mark=2");
        // Mutation check: an UNLISTED region must stay untouched.
        llm_map_region node_c{};
        node_c.route_mark               = 0;
        own.region_cell_at(9, 9).region = &node_c;
        detail::pathfind_mark_group_member_regions(v, own);
        ck_eq((uint32_t)node_c.route_mark, 0u,
              "mark_group_member_regions: a region no member occupies stays route_mark=0");
    }

    // ---- llm_strat_pathfind_target_hook_stub: true no-op -- both params stored, never read. --------
    // No observable state at all; the only thing to check is that it does not crash and does not
    // touch any tracked region. Call it with extreme values to make an accidental future write
    // (e.g. indexing an array with target_col) show up as a crash rather than pass silently.
    {
        detail::pathfind_target_hook_stub(0, 0);
        detail::pathfind_target_hook_stub(-1, -1);
        detail::pathfind_target_hook_stub(0x7fffffff, (int32_t)0x80000000);
        ck(true, "pathfind_target_hook_stub: no-op call with boundary values does not crash");
    }

    // ---- llm_strat_dir_step_toroidal_dist: toroidal Euclidean distance, wrap-then-square-then------
    // sqrt-then-trunc. group_move_scratch[i].tile_col/.tile_row per entry; half_width/half_height are
    // read via the MUTABLE accessor (no const view member exists for them, matching
    // sim_pathfind_route_leg_group_and_sort.cpp's identical precedent).
    {
        sim_fixture fx;
        fx.reset();
        sim_store own                         = fx.store();
        own.group_move_dist_half_width_mut()  = 100;
        own.group_move_dist_half_height_mut() = 60;
        // fx.reset() sets map_width=200, map_height=120 (see sim_test_support.h).

        const dir_step_toroidal_dist_calls c{[](double x) -> double { return sqrt(x); }};

        auto set_member = [&](int32_t i, int32_t col, int32_t row) {
            fx.group_move_scratch[(size_t)i].tile_col = col;
            fx.group_move_scratch[(size_t)i].tile_row = row;
        };

        sim_view v = fx.view();

        // Case 1: no wrap on either axis. dcol=|10-13|=3, drow=|10-14|=4 -> sqrt(9+16)=5.0 exactly.
        set_member(0, 10, 10);
        set_member(1, 13, 14);
        ck_eq((uint32_t)detail::dir_step_toroidal_dist(v, own, c, 0, 1), 5u,
              "dir_step_toroidal_dist: no-wrap 3-4-5 case -> 5");

        // Case 2: col wraps, row does not. dcol=|5-195|=190 > half_width(100) -> 190-200=-10.
        // drow=0. sqrt(100+0)=10.0 exactly.
        set_member(2, 5, 50);
        set_member(3, 195, 50);
        ck_eq((uint32_t)detail::dir_step_toroidal_dist(v, own, c, 2, 3), 10u,
              "dir_step_toroidal_dist: col-wrap-only -> 10");

        // Case 3: row wraps, col does not. drow=|5-115|=110 > half_height(60) -> 110-120=-10.
        // dcol=0. sqrt(0+100)=10.0 exactly.
        set_member(4, 50, 5);
        set_member(5, 50, 115);
        ck_eq((uint32_t)detail::dir_step_toroidal_dist(v, own, c, 4, 5), 10u,
              "dir_step_toroidal_dist: row-wrap-only -> 10");

        // Case 4: BOTH wrap. dcol=|1-198|=197>100 -> 197-200=-3. drow=|1-117|=116>60 -> 116-120=-4.
        // sqrt(9+16)=5.0 exactly.
        set_member(6, 1, 1);
        set_member(7, 198, 117);
        ck_eq((uint32_t)detail::dir_step_toroidal_dist(v, own, c, 6, 7), 5u,
              "dir_step_toroidal_dist: both-axes-wrap 3-4-5 case -> 5");

        // Symmetry: swapping a/b must not change the (unsigned, squared) result.
        ck_eq((uint32_t)detail::dir_step_toroidal_dist(v, own, c, 7, 6), 5u,
              "dir_step_toroidal_dist: symmetric in its two arguments");

        // Case 5: DISCRIMINATES half_width from half_height on the COL axis -- half_width=100,
        // half_height=60. dcol=80 is <= half_width (no wrap) but > half_height (would wrap if the
        // col-axis check used the WRONG half-extent). drow=0. If wired correctly: no wrap, dist =
        // sqrt(6400)=80. If the col check used half_height by mistake: dcol -> 80-200=-120, dist =
        // sqrt(14400) = 120.
        set_member(8, 10, 30);
        set_member(9, 90, 30);
        ck_eq((uint32_t)detail::dir_step_toroidal_dist(v, own, c, 8, 9), 80u,
              "dir_step_toroidal_dist: col axis checks against half_WIDTH, not half_height (80 <= "
              "100 -> no wrap)");
    }

    // ---- llm_strat_pathtrace_dirs_get: returns the raw address of pathtrace_dirs[0]. ---------------
    {
        sim_fixture fx;
        fx.reset();
        sim_store own    = fx.store();
        uint8_t  *got    = detail::pathtrace_dirs_get(own);
        uint8_t  *expect = &own.pathtrace_dir_at(0);
        ck(got == expect, "pathtrace_dirs_get: returns &pathtrace_dirs[0], not a copy");
        // Prove it aliases the tracked array, not a snapshot: writing through the returned pointer
        // must be visible through the store's own accessor.
        *got = 0x42;
        ck_eq((uint32_t)own.pathtrace_dir_at(0), 0x42u,
              "pathtrace_dirs_get: the returned pointer really aliases pathtrace_dirs[0]");
    }

    // ---- llm_strat_path_alloc_slot: linear-scan PATH_SLOT_FLAGS[path_group_idx][0..100) for the ----
    // first free (!=1) slot; calls the ORIGINAL path_attach_slot(path_group_idx, entity_id, slot) on
    // a hit and returns the slot index; returns -1 with NO callee call if every slot is busy.
    {
        struct log_t {
            int     attach_calls = 0;
            int32_t last_player = -1, last_entity = -1, last_slot = -1;
        };
        static log_t g_log;
        g_log = log_t{};
        const path_alloc_slot_calls c{[](int32_t player, int32_t entity, int32_t slot) -> void {
            ++g_log.attach_calls;
            g_log.last_player = player;
            g_log.last_entity = entity;
            g_log.last_slot   = slot;
        }};

        sim_fixture fx;
        fx.reset();
        sim_store own = fx.store();

        constexpr int32_t PLAYER = 3, ENTITY = 777;

        // All 100 slots for PLAYER busy (flag=1) -- must return -1 and never call the callee.
        for (int32_t s = 0; s < 100; ++s) own.path_slot_flag_at(PLAYER, s) = 1;
        ck_eq((uint32_t)detail::path_alloc_slot(own, c, PLAYER, ENTITY), (uint32_t)(int32_t)-1,
              "path_alloc_slot: all 100 slots busy -> -1");
        ck_eq((uint32_t)g_log.attach_calls, 0u,
              "path_alloc_slot: all-busy path never calls path_attach_slot");

        // Slot 42 free (flag=0) among busy neighbours -- must find exactly that one and call the
        // callee with (player, entity, slot) in that order, then return 42.
        own.path_slot_flag_at(PLAYER, 42) = 0;
        g_log                             = log_t{};
        ck_eq((uint32_t)detail::path_alloc_slot(own, c, PLAYER, ENTITY), 42u,
              "path_alloc_slot: returns the first free slot (42)");
        ck_eq((uint32_t)g_log.attach_calls, 1u, "path_alloc_slot: calls path_attach_slot exactly once");
        ck_eq((uint32_t)g_log.last_player, (uint32_t)PLAYER, "path_alloc_slot: attach player arg");
        ck_eq((uint32_t)g_log.last_entity, (uint32_t)ENTITY, "path_alloc_slot: attach entity arg");
        ck_eq((uint32_t)g_log.last_slot, 42u, "path_alloc_slot: attach slot arg = 42");

        // Slot 0 free -- the FIRST-slot boundary, and a different player index must not see it
        // (per-player isolation of the array).
        for (int32_t s = 0; s < 100; ++s) own.path_slot_flag_at(PLAYER, s) = 1;
        own.path_slot_flag_at(PLAYER, 0) = 0;
        g_log                            = log_t{};
        ck_eq((uint32_t)detail::path_alloc_slot(own, c, PLAYER, ENTITY), 0u,
              "path_alloc_slot: slot 0 free -> returns 0 (not confused with the all-busy -1)");

        constexpr int32_t OTHER_PLAYER = 4;
        for (int32_t s = 0; s < 100; ++s) own.path_slot_flag_at(OTHER_PLAYER, s) = 1;
        g_log = log_t{};
        ck_eq((uint32_t)detail::path_alloc_slot(own, c, OTHER_PLAYER, ENTITY), (uint32_t)(int32_t)-1,
              "path_alloc_slot: a different player's all-busy row does not see PLAYER's free slot 0");
    }
}

} // namespace mh::sim::test
