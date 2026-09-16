//
// sim/sim_tile_neighbor_reverse_dir.h -- one tile step BACKWARD along a heading (RI-SIM / SIM1F):
//
//   llm_strat_tile_neighbor_reverse_dir   @0x0048b308 (0x74 bytes)
//
// `void __mh_watcall_ebx_volatile llm_strat_tile_neighbor_reverse_dir(int tile_x, int tile_y, int
// dir_index, uint *out_x, uint *out_y)`. The reverse-direction sibling of
// llm_strat_tile_neighbor_in_dir: it SUBTRACTS the heading's (dx,dy) step rather than adding it, then
// wraps each axis by the map's power-of-two width/height mask:
//   step    = dir_remap_table[dir_index].step_primary;   // 0x0048b32c-0x0048b32f (SHL 4 -> row, then
//                                                         //   .step_primary at +0, i.e. [dir<<4])
//   *out_x  = (uint)(tile_x - dir_step_offsets[step].dx) & map_width_mask;   // 0x0048b335-0x0048b34c
//   *out_y  = (uint)(tile_y - dir_step_offsets[step].dy) & map_height_mask;  // 0x0048b35a-0x0048b371
// C-precedence note (matches the .asm's SUB-then-AND order): the subtraction binds tighter than the
// mask, i.e. `(tile - off) & mask`, NOT `tile - (off & mask)`.
//
// PURE apart from the two read-only lookup tables (sim_view::dir_remap_table @0x00ae1c78,
// sim_view::dir_step_offsets @0x00ae2d08) and general.width_mask/height_mask
// (map_width_mask/map_height_mask) -- the only CALL is the inert assert_stack_capacity prologue, no
// other sim state is touched, so no `_calls` struct. The committed prototype
// (addr/mh_calls.gen.h) narrows the out params to `void*`.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_tile_neighbor_reverse_dir @0x0048b308. Pure (two read-only tables + the wrap masks);
// no callees.
void tile_neighbor_reverse_dir(const sim_view &v, int32_t tile_x, int32_t tile_y, int32_t dir_index,
                               uint32_t *out_x, uint32_t *out_y);


} // namespace detail

// Public wrapper. Signature matches the committed prototype in addr/mh_calls.gen.h exactly.
void tile_neighbor_reverse_dir(int32_t tile_x, int32_t tile_y, int32_t dir_index, uint32_t *out_x,
                               uint32_t *out_y);

} // namespace mh::sim
