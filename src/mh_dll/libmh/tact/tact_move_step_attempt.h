//
// tact/tact_move_step_attempt.h -- TACT1B slice: the single-step move/path-probe helper.
//
//   llm_tact_move_step_attempt @0x00494b29 (0x351)
//
// Given a mover's tile (src_col,src_row) and a destination tile (dst_col,dst_row), either:
//  (a) they are already identical -- report success, no path slot used;
//  (b) the destination tile is currently VISIBLE (tile_objects flags[1] bit 0x80 -- the same bit
//      move_path_preview_walk.h reads as the fog-of-war "currently visible" bit, NOT independently
//      re-verified for this function specifically) -- run the real flood-fill pathfinder
//      (llm_tact_move_path_build, frontier) from dst back to src and succeed only if the backtrace
//      reconnects all the way to dst;
//  (c) the destination is NOT visible -- compute a single greedy octant step from src toward dst
//      (one of 8 (delta_col,delta_row) directions), reject it if either flanking corner tile is
//      blocked (no cutting a diagonal corner between two blocked orthogonal tiles), and succeed
//      only if that ONE step lands exactly on the destination (i.e. src was already adjacent).
//
// The direction codes (0x1,0x4,0x7,0xa,0xd,0x10,0x13,0x16) are 8 evenly-spaced notches of the
// project's 24-notch dir24 system (TACT_DIR24_SLOTS) -- named here by the (delta_col,delta_row)
// they correspond to rather than a compass label, since no compass orientation for dir24 is
// established elsewhere in this codebase.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

namespace detail {

// llm_tact_move_step_attempt @0x00494b29.
int32_t move_step_attempt(tact_view &v, tact_store &own, int32_t src_col, int32_t src_row,
                          int32_t dst_col, int32_t dst_row);

} // namespace detail

int32_t move_step_attempt(int32_t src_col, int32_t src_row, int32_t dst_col, int32_t dst_row);

// DECLARED HERE so another TU's rebind can name it (TACT1-P C4, 2026-09-04): the committed row
// spells all four coordinates unsigned, the wrapper above spells them int32_t.
// The binder pins every target against the COMMITTED export prototype, and compares types
// EXACTLY (rebind_verify.gen.cpp's per-row static_assert). Where the public wrapper above spells
// that shape differently, the committed shape still has to exist somewhere -- that is this shim,
// and all it does is forward. It sat beside the differential oracle until F2D retired it and was
// never part of it; gen_libmh_rebind routes the row here through libmh_rebind_targets.json.
namespace rebind_arm {
int32_t move_step_attempt(uint32_t src_col, uint32_t src_row, uint32_t dst_col, uint32_t dst_row);
} // namespace rebind_arm


} // namespace mh::tact
