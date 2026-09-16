//
// sim_pathfind_grid_geometry_selftest.cpp -- `simtest` offline oracle for the three functions in
// sim/sim_pathfind_grid_geometry.h/.cpp (llm_strat_dir_step_factor, llm_strat_tile_neighbor_in_dir,
// llm_strat_heading_candidate_find_slot; RI-SIM / SIM1-G2).
//
// All three are NOT SHADOWABLE (0 tracked write cells -- see the .h banner and
// tools/data/ghidra_findings.json), so this offline oracle is their only evidence. All three are
// PURE (no `_calls` struct, only read-only lookup tables + the map's width/height masks).
//
#include "sim/sim_pathfind_grid_geometry.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

} // namespace

void run_pathfind_grid_geometry_tests() {
    // ---- llm_strat_dir_step_factor: pure int32_t -> double, no state at all. -----------------------
    // 0x00449b43-0x00449b5d: dir in {0x10, 0x16, 0x4, 0xa} -> 1.4 exactly; every other dir -> 1.0
    // exactly (both are exact IEEE-754 doubles, so == is the right comparison, not an epsilon).
    {
        const int32_t hits[] = {0x10, 0x16, 0x4, 0xa};
        for (int32_t dir : hits) {
            ck(detail::dir_step_factor(dir) == 1.4, "dir_step_factor: hit dir returns 1.4 exactly");
        }
        // Misses: 0 (below every hit), 1..3 (below 0x4), 5..9 (between 0x4 and 0xa), 0xb (just past
        // 0xa), 0xf/0x11 (bracketing 0x10 without matching), 0x15/0x17 (bracketing 0x16), and a large
        // out-of-table value -- covers "never matches", not just "one gap".
        const int32_t misses[] = {0, 1, 2, 3, 5, 6, 7, 8, 9, 0xb, 0xf, 0x11, 0x15, 0x17, 0x1000};
        for (int32_t dir : misses) {
            ck(detail::dir_step_factor(dir) == 1.0, "dir_step_factor: miss dir returns 1.0 exactly");
        }
    }

    // ---- llm_strat_tile_neighbor_in_dir: dir_remap_table[dir].step_primary indexes ------------------
    // dir_step_offsets[step_primary].{dx,dy}, added to (x,y) and wrapped through the map's masks.
    {
        sim_fixture fx;
        fx.reset();

        // A local remap table (fixture leaves dir_remap_table null by default, same convention as
        // sim_tile_neighbor_reverse_dir_selftest.cpp): step_primary[dir] = dir (identity), so
        // dir_step_offsets[dir] is indexed directly -- no second indirection to keep straight.
        constexpr int32_t          N_DIRS = 24;
        std::vector<dir_remap_row> local_remap((size_t)N_DIRS);
        for (int32_t i = 0; i < N_DIRS; ++i) {
            local_remap[(size_t)i].step_primary = i;
            local_remap[(size_t)i].step_alt1    = -1; // not read by this function -- sentinel
            local_remap[(size_t)i].step_alt2    = -1;
            local_remap[(size_t)i].step_alt3    = -1;
        }
        // Distinct, sign-crossing (dx,dy) per direction -- same construction as the reverse-dir
        // sibling test, different slope so a dx/dy swap or a wrong step_primary disagrees.
        for (int32_t i = 0; i < N_DIRS; ++i) {
            fx.dir_step_offsets[(size_t)i].dx = 200 - i * 17;
            fx.dir_step_offsets[(size_t)i].dy = i * 11 - 30;
        }

        sim_view v        = fx.view();
        v.dir_remap_table = local_remap.data();

        const int32_t  tile_x = 40, tile_y = 25;   // distinct, non-zero, non-symmetric
        const uint32_t wmask = map_width_mask(v);  // 0xff per sim_fixture::reset()
        const uint32_t hmask = map_height_mask(v); // 0x3f per sim_fixture::reset()

        for (int32_t dir = 0; dir < N_DIRS; ++dir) {
            int32_t out_col = -12345, out_row = -12345;
            detail::tile_neighbor_in_dir(v, tile_x, tile_y, dir, &out_col, &out_row);
            const int32_t dx         = fx.dir_step_offsets[(size_t)dir].dx;
            const int32_t dy         = fx.dir_step_offsets[(size_t)dir].dy;
            const int32_t expect_col = (int32_t)((uint32_t)(tile_x + dx) & wmask);
            const int32_t expect_row = (int32_t)((uint32_t)(tile_y + dy) & hmask);
            ck_eq(out_col, expect_col, "tile_neighbor_in_dir: out_col matches (x+dx)&width_mask");
            ck_eq(out_row, expect_row, "tile_neighbor_in_dir: out_row matches (y+dy)&height_mask");
        }

        // Hand-derived worked example for dir=0 (independent of the loop's own formula, so a bug
        // shared between this file and the header's comment would still be caught): dx=200, dy=-30.
        // out_col = (uint32_t)(40+200) & 0xff = 240. out_row = (uint32_t)(25-30) & 0x3f =
        // (uint32_t)(-5) & 0x3f = 0xFFFFFFFB & 0x3F = 0x3B = 59.
        {
            int32_t out_col = -1, out_row = -1;
            detail::tile_neighbor_in_dir(v, tile_x, tile_y, 0, &out_col, &out_row);
            ck_eq(out_col, 240, "tile_neighbor_in_dir dir=0: out_col = 240 (hand-derived)");
            ck_eq(out_row, 59, "tile_neighbor_in_dir dir=0: out_row = 59 (hand-derived)");
        }
    }

    // ---- llm_strat_heading_candidate_find_slot: scan slots 0..2 of heading*3, break on the ---------
    // turn_delta==-1 sentinel, return the first matching slot index, else -1.
    {
        sim_fixture fx;
        fx.reset();
        sim_view v = fx.view();

        constexpr int32_t HEADING_A = 3, HEADING_B = 7, HEADING_C = 11;

        // HEADING_A: all 3 slots active (turn_delta >= 0), distinct values -- match at slot 0, 1, 2,
        // and a miss that scans all three without matching.
        fx.heading_candidates[(size_t)(HEADING_A * 3 + 0)].turn_delta = 10;
        fx.heading_candidates[(size_t)(HEADING_A * 3 + 1)].turn_delta = 20;
        fx.heading_candidates[(size_t)(HEADING_A * 3 + 2)].turn_delta = 30;
        ck_eq(detail::heading_candidate_find_slot(v, HEADING_A, 10), 0,
              "heading_candidate_find_slot: match at slot 0");
        ck_eq(detail::heading_candidate_find_slot(v, HEADING_A, 20), 1,
              "heading_candidate_find_slot: match at slot 1");
        ck_eq(detail::heading_candidate_find_slot(v, HEADING_A, 30), 2,
              "heading_candidate_find_slot: match at slot 2 (last slot, no sentinel hit)");
        ck_eq(detail::heading_candidate_find_slot(v, HEADING_A, 99), -1,
              "heading_candidate_find_slot: no match, all 3 slots active -> -1");

        // HEADING_B: sentinel (-1) at slot 1 -- the loop must STOP there, so a value that would
        // match slot 2 (which is still != -1, but unreached) must NOT be found.
        fx.heading_candidates[(size_t)(HEADING_B * 3 + 0)].turn_delta = 5;
        fx.heading_candidates[(size_t)(HEADING_B * 3 + 1)].turn_delta = -1; // sentinel: stop here
        fx.heading_candidates[(size_t)(HEADING_B * 3 + 2)].turn_delta = 5;  // same value, unreached
        ck_eq(detail::heading_candidate_find_slot(v, HEADING_B, 5), 0,
              "heading_candidate_find_slot: match at slot 0 before the sentinel");
        ck_eq(detail::heading_candidate_find_slot(v, HEADING_B, -1), -1,
              "heading_candidate_find_slot: querying the sentinel value itself never matches "
              "(the sentinel check is `< 0`, tested before the equality check)");

        // HEADING_C: sentinel at slot 0 -- immediate stop, -1 regardless of query.
        fx.heading_candidates[(size_t)(HEADING_C * 3 + 0)].turn_delta = -1;
        fx.heading_candidates[(size_t)(HEADING_C * 3 + 1)].turn_delta = 0; // unreached
        fx.heading_candidates[(size_t)(HEADING_C * 3 + 2)].turn_delta = 0; // unreached
        ck_eq(detail::heading_candidate_find_slot(v, HEADING_C, 0), -1,
              "heading_candidate_find_slot: sentinel at slot 0 -> immediate -1");
    }
}

} // namespace mh::sim::test
