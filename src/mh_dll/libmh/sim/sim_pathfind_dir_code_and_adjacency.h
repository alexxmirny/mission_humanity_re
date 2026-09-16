//
// sim/sim_pathfind_dir_code_and_adjacency.h -- two toroidal grid-geometry primitives (RI-SIM /
// SIM1-G2):
//
//   llm_strat_pathfind_dir_code_from_delta @0x0041e790 (0xa6 bytes)
//   llm_strat_tiles_adjacent               @0x00498934 (0x8e bytes)
//
// ---- llm_strat_pathfind_dir_code_from_delta @0x0041e790 -------------------------------------------
// `int __watcall llm_strat_pathfind_dir_code_from_delta(int d_col_sign, int d_row_sign)`, matching
// the committed prototype exactly. A pure decision table over sign(d_col) (-1/0/1) x sign(d_row)
// (-1/0/1) -> a direction code in {1,4,7,10,13,16,19,22} (arithmetic step 3, matching the same
// 3-per-octant stride `sim_path_group_steps.cpp`'s own dir-code arithmetic uses); the (0,0) case
// shares code 1 with (0,1) (the "else" arm of the d_col_sign==0 branch, not a distinct case). Pure:
// no callee but the inert Watcom stack probe, no write.
//
// ---- llm_strat_tiles_adjacent @0x00498934 -----------------------------------------------------------
// `int __watcall llm_strat_tiles_adjacent(int x1, int y1, int x2, int y2)`, matching the committed
// prototype exactly. Calls the ORIGINAL `llm_strat_tile_delta_wrapped(x1,y1,x2,y2,&dx,&dy)` (already
// translated in SIM1F, sim_tile_delta_wrapped.cpp -- but every other TU in this codebase still
// reaches it via mh::call:: per the established convention, not a direct C++ call, so this file does
// the same via a `_calls` struct), then returns 1 iff BOTH `abs(dx)<=1` and `abs(dy)<=1` (i.e. the
// two tiles are the same tile or share an edge/corner on the wrapped grid), else 0. Pure otherwise:
// no write, no other callee.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one ORIGINAL callee `tiles_adjacent` reaches. Indirected (not a direct mh::call:: inside
// detail::) for the same reason as every other outward-calling TU in this batch: a direct call
// reaches into the live game image, which makes the body untestable by `net_selftest.exe simtest`.
struct tiles_adjacent_calls {
    void (*tile_delta_wrapped)(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t *out_dx,
                               int32_t *out_dy); // llm_strat_tile_delta_wrapped @0x004941b9
};

const tiles_adjacent_calls &live_tiles_adjacent_calls();

namespace detail {

// llm_strat_pathfind_dir_code_from_delta @0x0041e790. Pure; no sim_view/sim_store, no callees.
int32_t pathfind_dir_code_from_delta(int32_t d_col_sign, int32_t d_row_sign);

// llm_strat_tiles_adjacent @0x00498934. No sim_view/sim_store; one indirected callee.
int32_t tiles_adjacent(const tiles_adjacent_calls &c, int32_t x1, int32_t y1, int32_t x2, int32_t y2);

} // namespace detail

// Public wrappers. Match the committed prototypes / callable shapes exactly.
int32_t pathfind_dir_code_from_delta(int32_t d_col_sign, int32_t d_row_sign);
int32_t tiles_adjacent(int32_t x1, int32_t y1, int32_t x2, int32_t y2);

} // namespace mh::sim
