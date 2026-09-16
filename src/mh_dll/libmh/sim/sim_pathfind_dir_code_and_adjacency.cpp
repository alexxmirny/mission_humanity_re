//
// sim/sim_pathfind_dir_code_and_adjacency.cpp -- see sim_pathfind_dir_code_and_adjacency.h.
// Translated from the DISASSEMBLY (tmp/decomp_sim/llm_strat_pathfind_dir_code_from_delta_0041e790.asm,
// tmp/decomp_sim/llm_strat_tiles_adjacent_00498934.asm), address-by-address.
//
// No entry-point seam: both functions have 0 tracked write cells per tmp/state_matrix.json --
// matches this slice's write-set preflight (NOT SHADOWABLE). Evidence is the offline oracle
// (sim_pathfind_dir_code_and_adjacency_selftest.cpp) plus adversarial review.
//
#include "sim/sim_pathfind_dir_code_and_adjacency.h"

#include "addr/mh_calls.gen.h"  // typed callable for the ORIGINAL function tiles_adjacent still calls out to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const tiles_adjacent_calls &live_tiles_adjacent_calls() {
    static const tiles_adjacent_calls c = {
        MH_LIBMH_BIND(llm_strat_tile_delta_wrapped),
    };
    return c;
}

namespace detail {

int32_t pathfind_dir_code_from_delta(int32_t d_col_sign, int32_t d_row_sign) {
    // 0x0041e7b4-0x0041e824: a pure decision table over sign(d_col) x sign(d_row).
    if (d_col_sign == 0) {
        return (d_row_sign == -1) ? 13 : 1;
    }
    if (d_col_sign == -1) {
        if (d_row_sign == 0) return 7;
        if (d_row_sign == -1) return 10;
        return 4;
    }
    // 0x0041e7ff-0x0041e824: the "else" arm -- any d_col_sign other than 0 or -1.
    if (d_row_sign == 0) return 19;
    if (d_row_sign == -1) return 16;
    return 22;
}

int32_t tiles_adjacent(const tiles_adjacent_calls &c, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    int32_t dx = 0, dy = 0;
    c.tile_delta_wrapped(x1, y1, x2, y2, &dx, &dy); // 0x00498969

    // 0x0049896e-0x004989a8: abs(dx) <= 1.
    const int32_t abs_dx = (dx < 0) ? -dx : dx;
    if (abs_dx > 1) return 0;

    // 0x0049898b-0x004989b1: abs(dy) <= 1.
    const int32_t abs_dy = (dy < 0) ? -dy : dy;
    return (abs_dy <= 1) ? 1 : 0;
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

int32_t pathfind_dir_code_from_delta(int32_t d_col_sign, int32_t d_row_sign) {
    return detail::pathfind_dir_code_from_delta(d_col_sign, d_row_sign);
}

int32_t tiles_adjacent(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    return detail::tiles_adjacent(live_tiles_adjacent_calls(), x1, y1, x2, y2);
}

} // namespace mh::sim
