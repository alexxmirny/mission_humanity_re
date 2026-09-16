//
// sim_pathfind_dir_code_and_adjacency_selftest.cpp -- `simtest` offline oracle for
// llm_strat_pathfind_dir_code_from_delta / llm_strat_tiles_adjacent
// (sim/sim_pathfind_dir_code_and_adjacency.h, RI-SIM / SIM1-G2). Both are NOT
// SHADOWABLE (0 tracked write cells per the write-closure derivation), so this is their only
// execution-proof evidence, alongside adversarial review.
//
#include "sim/sim_pathfind_dir_code_and_adjacency.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {
using namespace mh::sim;
} // namespace

void run_pathfind_dir_code_and_adjacency_tests() {
    // ---- llm_strat_pathfind_dir_code_from_delta: all 9 sign combinations ----------------------------
    ck_eq((uint32_t)detail::pathfind_dir_code_from_delta(0, -1), 13u, "dir_code(0,-1) = 13");
    ck_eq((uint32_t)detail::pathfind_dir_code_from_delta(0, 0), 1u, "dir_code(0,0) = 1");
    ck_eq((uint32_t)detail::pathfind_dir_code_from_delta(0, 1), 1u, "dir_code(0,1) = 1 (shares code w/ 0,0)");
    ck_eq((uint32_t)detail::pathfind_dir_code_from_delta(-1, 0), 7u, "dir_code(-1,0) = 7");
    ck_eq((uint32_t)detail::pathfind_dir_code_from_delta(-1, -1), 10u, "dir_code(-1,-1) = 10");
    ck_eq((uint32_t)detail::pathfind_dir_code_from_delta(-1, 1), 4u, "dir_code(-1,1) = 4");
    ck_eq((uint32_t)detail::pathfind_dir_code_from_delta(1, 0), 19u, "dir_code(1,0) = 19");
    ck_eq((uint32_t)detail::pathfind_dir_code_from_delta(1, -1), 16u, "dir_code(1,-1) = 16");
    ck_eq((uint32_t)detail::pathfind_dir_code_from_delta(1, 1), 22u, "dir_code(1,1) = 22");

    // ---- llm_strat_tiles_adjacent: mock tile_delta_wrapped, drive its (dx,dy) directly --------------
    {
        struct log_t {
            int32_t dx = 0, dy = 0;
        };
        static log_t               g_log;
        const tiles_adjacent_calls c{[](int32_t, int32_t, int32_t, int32_t, int32_t *out_dx,
                                        int32_t *out_dy) -> void {
            *out_dx = g_log.dx;
            *out_dy = g_log.dy;
        }};

        g_log = {0, 0};
        ck_eq((uint32_t)detail::tiles_adjacent(c, 0, 0, 0, 0), 1u, "tiles_adjacent: same tile -> 1");

        g_log = {1, 1};
        ck_eq((uint32_t)detail::tiles_adjacent(c, 0, 0, 0, 0), 1u,
              "tiles_adjacent: diagonal neighbor (1,1) -> 1");

        g_log = {-1, -1};
        ck_eq((uint32_t)detail::tiles_adjacent(c, 0, 0, 0, 0), 1u,
              "tiles_adjacent: diagonal neighbor (-1,-1) -> 1");

        g_log = {2, 0};
        ck_eq((uint32_t)detail::tiles_adjacent(c, 0, 0, 0, 0), 0u,
              "tiles_adjacent: abs(dx)=2 -> 0 (short-circuits before checking dy)");

        g_log = {0, 2};
        ck_eq((uint32_t)detail::tiles_adjacent(c, 0, 0, 0, 0), 0u, "tiles_adjacent: abs(dy)=2 -> 0");

        g_log = {-2, 0};
        ck_eq((uint32_t)detail::tiles_adjacent(c, 0, 0, 0, 0), 0u, "tiles_adjacent: abs(dx)=-2 -> 0");

        g_log = {1, -1};
        ck_eq((uint32_t)detail::tiles_adjacent(c, 0, 0, 0, 0), 1u,
              "tiles_adjacent: mixed-sign diagonal (1,-1) -> 1");
    }
}

} // namespace mh::sim::test
