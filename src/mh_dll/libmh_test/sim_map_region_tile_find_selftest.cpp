//
// sim_map_region_tile_find_selftest.cpp -- `simtest` offline oracle for
// llm_map_region_find_nearest_valid_tile, llm_strat_pathfind_find_closer_visible_tile and
// llm_map_region_walk_to_valid_tile (sim/sim_map_region_tile_find.h/.cpp, RI-SIM / SIM1-G2).
//
// ALL THREE are NOT SHADOWABLE (0 tracked write cells -- pure queries) -- this offline oracle is
// their only evidence, same posture as sim_pathfind_grid_geometry.cpp's own family.
//
// walk_to_valid_tile's case (d) below is a REGRESSION TEST for a real bug reimpl-verify.js caught
// (2026-08-20): an earlier draft indexed v.passable[] with `(col<<8)|row` where the original uses
// `(col<<8)+row` -- col/row there are UNMASKED int32_t on the loop's first iteration, so ADD and OR
// disagree whenever row>=256. See sim_map_region_tile_find.cpp's fix comment for the derivation.
//
// find_closer_visible_tile's one outward call (tile_dist_wrapped) is injected via
// find_closer_visible_tile_calls, NOT called bare -- calling the ORIGINAL mh::call:: function directly
// segfaults outside the live mh.exe process (caught by actually running this suite, 2026-08-20/21, not
// just building it). The mock below is plain Manhattan distance -- exact metric doesn't matter for
// these tests, only that "one step toward dst is strictly closer" holds, which it does for any sane
// distance function.
//
#include "sim/sim_map_region_tile_find.h"

#include "sim_test_support.h"

#include <cstdlib>

namespace mh::sim::test {
namespace {
const find_closer_visible_tile_calls &mock_tile_dist_calls() {
    static const find_closer_visible_tile_calls c = {
        [](int32_t x1, int32_t y1, int32_t x2, int32_t y2) -> int32_t {
            return std::abs(x1 - x2) + std::abs(y1 - y2);
        },
    };
    return c;
}
} // namespace

void run_map_region_tile_find_tests() {
    // ================================================================================================
    // find_nearest_valid_tile
    // ================================================================================================
    // ---- (a) the immediate first ring-1 step (dir=1) lands on a valid region -> found there. --------
    {
        sim_fixture fx;
        fx.reset();
        sim_store own                          = fx.store();
        fx.path_wrap_mask                      = 0xff;
        fx.map_width                           = 200;
        fx.map_dir_step_deltas[1 * 2 + 0]      = 1; // dir=1: +1 col, +0 row
        fx.map_dir_step_deltas[1 * 2 + 1]      = 0;
        own.region_cell_at(6, 5).terrain_flags = 0x100u; // upper 24 bits = 1: assigned, not the
                                                         // 0xffffff "pending" sentinel
        sim_view      v   = fx.view();
        uint8_t       col = 5, row = 5;
        const int32_t r = detail::find_nearest_valid_tile(v, own, &col, &row);
        ck_eq((uint32_t)r, 1u, "find_nearest_valid_tile: dir=1 ring-1 step lands on a valid cell -> found");
        ck_eq(col, 6u, "find_nearest_valid_tile: *col updated to the found tile's column");
        ck_eq(row, 5u, "find_nearest_valid_tile: *row updated to the found tile's row");
    }

    // ---- (b) no assigned region anywhere within a small map_width -> exhausted, returns 0. ----------
    {
        sim_fixture fx;
        fx.reset();
        sim_store own     = fx.store();
        fx.path_wrap_mask = 0xff;
        fx.map_width      = 2; // ring reaches 2 after one full ring-1 pass -> loop exits
        sim_view      v   = fx.view();
        uint8_t       col = 5, row = 5;
        const int32_t r = detail::find_nearest_valid_tile(v, own, &col, &row);
        ck_eq((uint32_t)r, 0u, "find_nearest_valid_tile: no assigned region within range -> 0");
    }

    // ================================================================================================
    // walk_to_valid_tile
    // ================================================================================================
    // ---- (c) src == dst immediately -> returns 1, no stepping, out_col/out_row untouched. -----------
    {
        sim_fixture fx;
        fx.reset();
        sim_store own         = fx.store();
        fx.path_wrap_mask     = 0xff;
        fx.map_width          = 200;
        fx.map_height         = 200;
        sim_view      v       = fx.view();
        uint32_t      out_col = 0xdeadbeef, out_row = 0xdeadbeef;
        const int32_t r =
            detail::walk_to_valid_tile(v, own, 42, 42, 42, 42, &out_col, &out_row);
        ck_eq((uint32_t)r, 1u, "walk_to_valid_tile: src==dst -> 1 (reached destination)");
        ck_eq(out_col, 0xdeadbeefu, "walk_to_valid_tile: src==dst -> out_col NOT written");
        ck_eq(out_row, 0xdeadbeefu, "walk_to_valid_tile: src==dst -> out_row NOT written");
    }

    // ---- (d) REGRESSION: passable[] must be indexed with ADD, not OR, when col/row are not yet
    // byte-range-disjoint (row >= 256 on the very first, unmasked check). ------------------------------
    {
        sim_fixture fx;
        fx.reset();
        sim_store own     = fx.store();
        fx.path_wrap_mask = 0xff;
        fx.map_width      = 1000;
        fx.map_height     = 1000;
        // region assigned + not the flood-pending sentinel at (col=1,row=300) so the walk gets past
        // both region checks and reaches the passable[] test.
        own.region_cell_at(1, 300).terrain_flags = 0x100u;
        // ADD index (col<<8)+row = 256+300 = 556: the CORRECT index per the original's disassembly.
        fx.passable[556] = 1;
        // OR index (col<<8)|row = 300: what a WRONG `|`-based translation would read instead -- kept
        // at 0 so the two indices disagree and the test can tell which one the code actually used.
        fx.passable[300]      = 0;
        sim_view      v       = fx.view();
        uint32_t      out_col = 0xdeadbeef, out_row = 0xdeadbeef;
        const int32_t r =
            detail::walk_to_valid_tile(v, own, 1, 300, 99, 99, &out_col, &out_row);
        // With the correct ADD index, passable[556]==1 -> the walk stops at the START tile without
        // ever stepping (returns 0, *out_col/*out_row == the unmoved src position). A buggy OR-based
        // translation would read passable[300]==0 instead, fail to stop, and step at least once --
        // landing on a DIFFERENT (col,row), so this also catches the bug by disagreement even without
        // hand-deriving the buggy trajectory.
        ck_eq((uint32_t)r, 0u, "walk_to_valid_tile: ADD-indexed passable[] stops immediately -> 0");
        ck_eq(out_col, 1u, "walk_to_valid_tile: stops at the START tile's column (no step taken)");
        ck_eq(out_row, 300u, "walk_to_valid_tile: stops at the START tile's row (no step taken)");
    }

    // ================================================================================================
    // find_closer_visible_tile
    // ================================================================================================
    // ---- (e) the ring-1 candidate one step toward the destination is passable + discovered +
    // strictly closer -> found there. -------------------------------------------------------------
    {
        sim_fixture fx;
        fx.reset();
        sim_store own        = fx.store();
        fx.path_wrap_mask    = 0xff;
        fx.map_width         = 200;
        fx.map_height        = 200;
        fx.group_order_owner = 0; // vis_mask = 1<<0 = 1
        // dst is due north of src by 10 rows; one step north (row-1) is strictly closer.
        const uint8_t src_col = 50, src_row = 50;
        const int32_t dst_col = 50, dst_row = 40;
        fx.passable[(uint32_t)(src_col << 8) | (uint32_t)(src_row - 1)] = 1;
        own.fog_discovered_at(src_col, src_row - 1)                     = 1; // bit 0 set -> visible to owner 0
        sim_view      v                                                 = fx.view();
        uint32_t      out_col = 0xdeadbeef, out_row = 0xdeadbeef;
        const int32_t r = detail::find_closer_visible_tile(v, own, mock_tile_dist_calls(), src_col,
                                                           src_row, &out_col, &out_row, dst_col,
                                                           dst_row);
        ck_eq((uint32_t)r, 0u,
              "find_closer_visible_tile: one step toward dst is passable+discovered+closer -> found (0)");
        ck_eq(out_col, src_col, "find_closer_visible_tile: found tile's column is src_col (north step)");
        ck_eq(out_row, (uint32_t)(src_row - 1), "find_closer_visible_tile: found tile's row is one north of src");
    }

    // ---- (f) nothing passable/discovered anywhere within a small map_width -> exhausted, returns 1.
    {
        sim_fixture fx;
        fx.reset();
        sim_store own         = fx.store();
        fx.path_wrap_mask     = 0xff;
        fx.map_width          = 2; // exhausts after one ring
        fx.map_height         = 200;
        fx.group_order_owner  = 0;
        sim_view      v       = fx.view();
        uint32_t      out_col = 0, out_row = 0;
        const int32_t r = detail::find_closer_visible_tile(v, own, mock_tile_dist_calls(), 10, 10,
                                                           &out_col, &out_row, 20, 20);
        ck_eq((uint32_t)r, 1u, "find_closer_visible_tile: nothing passable/discovered in range -> 1");
    }
}

} // namespace mh::sim::test
