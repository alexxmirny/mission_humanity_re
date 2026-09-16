//
// ai/ai_grid_stencil.h -- the AI influence-grid WINDOW TESTS (RI-AI / AI1A batch A layer 6, the last
// layer of batch A).
//
// Two __cdecl leaf functions with no callees of their own. They are the SAME WALK with different
// per-cell predicates, which is why they live in one translation unit:
//
//   grid_match_stencil                  every 'care' cell must EQUAL target_byte (all three site
//                                       scanners pass 2)
//   grid_stencil_all_near_unthreatened  every 'care' cell must have (cell & 0x40) == 0 and
//                                       (cell & 0x1f) <= 3 -- no enemy-turret threat, and within
//                                       three influence-flood steps of my own area
//
// A 'care' cell is one whose corresponding `footprint_mask` byte is non-zero. The mask is walked
// CONTIGUOUSLY, one byte per cell, and is NOT reset per row -- it is a span_x x span_y block laid out
// with the Y axis contiguous (see ai_grid_stencil.cpp on which axis is which and how that is proved).
//
// Their only write is the shared torus wrap mask (own.grid_wrap_mask), stored on entry and read back
// inside the loop; the grid and the footprint mask are READ through caller-supplied pointers, so the
// state matrix records exactly one region for each of them and that is correct rather than thin.
//
// THE WRAP IS DESTRUCTIVE AND CONDITIONAL, and that is the whole subtlety of these two bodies -- see
// the .cpp. Do not "simplify" it into a per-cell `(x & wx) << 8 | (y & wy)`.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrappers below are this applied to state(); the split costs one inlined
// call. (reimpl-loop: "if a module's logic is pure over its state, this is almost always the cheap
// test" -- and here it is the ONLY practical cover for the wrap branch, which needs a window that
// crosses the torus seam.)
namespace detail {

// llm_strat_ai_grid_match_stencil @0x004b4b1c.
int32_t grid_match_stencil(const ai_view &v, const ai_store &own, const uint8_t *grid,
                           int32_t grid_width, int32_t grid_height, const uint8_t *footprint_mask,
                           int32_t span_x, int32_t span_y, int32_t start_x, int32_t start_y,
                           int32_t target_byte);

// llm_strat_ai_grid_stencil_all_near_unthreatened @0x004b4b7a.
int32_t grid_stencil_all_near_unthreatened(const ai_view &v, const ai_store &own,
                                           const uint8_t *grid, int32_t grid_width,
                                           int32_t grid_height, const uint8_t *footprint_mask,
                                           int32_t span_x, int32_t span_y, int32_t start_x,
                                           int32_t start_y);

} // namespace detail

int32_t grid_match_stencil(const uint8_t *grid, int32_t grid_width, int32_t grid_height,
                           const uint8_t *footprint_mask, int32_t span_x, int32_t span_y,
                           int32_t start_x, int32_t start_y, int32_t target_byte);
int32_t grid_stencil_all_near_unthreatened(const uint8_t *grid, int32_t grid_width,
                                           int32_t grid_height, const uint8_t *footprint_mask,
                                           int32_t span_x, int32_t span_y, int32_t start_x,
                                           int32_t start_y);

} // namespace mh::ai
