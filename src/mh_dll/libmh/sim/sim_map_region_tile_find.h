#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// find_closer_visible_tile's one outward call, injected rather than called bare through mh::call:: --
// mh::call::llm_strat_tile_dist_wrapped jumps to the ORIGINAL function's fixed VA inside the live
// mh.exe process; net_selftest.exe has no such address mapped, so an offline oracle needs a seam to
// substitute a mock (segfault caught by actually running the suite, 2026-08-20/21, not just building
// it -- see sim_map_region_tile_find_selftest.cpp). find_nearest_valid_tile/walk_to_valid_tile make no
// outward calls at all and need no such struct.
struct find_closer_visible_tile_calls {
    int32_t (*tile_dist_wrapped)(int32_t x1, int32_t y1, int32_t x2, int32_t y2); // llm_strat_tile_dist_wrapped @0x0049404e
};
const find_closer_visible_tile_calls &live_find_closer_visible_tile_calls();

namespace detail {

// llm_map_region_find_nearest_valid_tile @0x0041ed6d.
//
// Ring/spiral search that MUTATES *col/*row IN PLACE as it steps, starting from the caller-supplied
// position: for ring = 1, 2, 3, ... while ring < map width, walk `ring` steps in one compass
// direction then `ring` steps in the next (directions cycle 1 -> 7 -> 13 -> 19 -> 1..., i.e. quarter
// turns through v.map_dir_step_deltas' 24-entry compass), checking the NEW cell after each step (step
// happens BEFORE the check, so the function's own starting cell is never itself tested). A cell is
// "valid" when its region_cell_at().terrain_flags upper-24-bit field is neither 0 nor the 0xffffff
// sentinel (see the .cpp's DECLARED NEED about this field). Returns 1 the instant a valid cell is
// found (*col/*row now hold it) or 0 if the ring grew to the map width without one (in which case
// *col/*row hold whatever position the search last stepped to -- not meaningful on failure, same as
// the original).
int32_t find_nearest_valid_tile(const sim_view &v, sim_store &own, uint8_t *col, uint8_t *row);

// llm_strat_pathfind_find_closer_visible_tile @0x0041fff5.
//
// Ring/spiral search starting from (src_col, src_row) (a copy is walked; the original position is
// only used to seed `best_dist`, never mutated through the caller's storage). Direction rotates 90
// degrees ((ddx,ddy) -> (-ddy,ddx)) after each `ring`-length leg, same "ring=1,1,2,2,3,3..." growth
// as find_nearest_valid_tile but expressed as a signed (ddx,ddy) pair rather than a compass-table
// index. A candidate cell qualifies when v.passable[] is nonzero AND the fog-of-war discovered plane
// has the current group-order owner's visibility bit set (own.fog_discovered_at(col,row) &
// (1 << *v.group_order_owner)) AND it is STRICTLY closer to (dst_col,dst_row) (by
// mh::call::llm_strat_tile_dist_wrapped) than the ORIGINAL (src_col,src_row) was, computed once
// before the search starts. Returns 0 and writes *out_col/*out_row the instant such a cell is found,
// or 1 (out params untouched) if the ring grew to the map width without one.
int32_t find_closer_visible_tile(const sim_view &v, sim_store &own,
                                 const find_closer_visible_tile_calls &c, uint8_t src_col,
                                 uint8_t src_row, uint32_t *out_col, uint32_t *out_row,
                                 int32_t dst_col, int32_t dst_row);

// llm_map_region_walk_to_valid_tile @0x004201e4.
//
// Steps (col,row) one tile at a time toward (dst_col,dst_row) along the toroidal-shortest direction
// per axis, stopping the INSTANT EITHER axis reaches its destination value (loop guard is
// `col != dst_col && row != dst_row` -- an OR-to-exit, confirmed from the raw JZ/JNZ fallthrough
// chain at 0x00420205-0x00420217, NOT the AND-style "walk until exactly at the destination" a naive
// reading would assume -- see the .cpp's uncertainty note) or the instant the CURRENT tile (before
// stepping) is blocked (own.region_cell_at(col,row).terrain_flags upper-24-bits assigned AND not the
// 0xffffff sentinel, AND v.passable[] nonzero there). After the loop, a SEPARATE exact-match check
// (col==dst_col && row==dst_row, proper AND) decides the return: 1 if the walk landed exactly on the
// destination (out params NOT written), 0 otherwise (*out_col/*out_row written with wherever the walk
// actually stopped -- exact match on one axis only, or an obstruction).
int32_t walk_to_valid_tile(const sim_view &v, sim_store &own, uint32_t src_col, uint32_t src_row,
                           uint32_t dst_col, uint32_t dst_row, uint32_t *out_col, uint32_t *out_row);

} // namespace detail

// Live wrappers: the logic applied to state().read/state().own. Each matches its original's committed
// __watcall/__mh_watcall_ecx_ebx_volatile shape (see addr/mh_export.gen.h's sig_ typedefs) -- the
// boundary carries the committed pointee (TACT1-P C6, 2026-09-04), same convention as
// sim_pathfind_grid_geometry.h's tile_neighbor_in_dir.
int32_t find_nearest_valid_tile(uint8_t *col, uint8_t *row);
int32_t find_closer_visible_tile(uint8_t src_col, uint8_t src_row, uint32_t *out_col, uint32_t *out_row,
                                 int32_t dst_col, int32_t dst_row);
int32_t walk_to_valid_tile(uint32_t src_col, uint32_t src_row, uint32_t dst_col, uint32_t dst_row,
                           uint32_t *out_col, uint32_t *out_row);

namespace detail {
} // namespace detail

} // namespace mh::sim
