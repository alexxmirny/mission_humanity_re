#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {
namespace detail {

// llm_strat_dir_step_factor @0x00449b28.
//
// A per-24-way-heading move-speed multiplier: 1.4 (0x3FF6666666666666, i.e. the exact double bit
// pattern the constant pool holds, not a re-derived literal) for the four diagonal-ish headings
// {4, 10, 16, 22}, else 1.0. Reads no sim state at all -- pure function of `dir`, so `detail::` takes
// no `sim_view`, matching sim_facing24_from_points.h's identical no-state-read shape.
double dir_step_factor(int32_t dir);

// llm_strat_tile_neighbor_in_dir @0x0048b294.
//
// Steps one tile from (x, y) in 8-way direction `dir`, wrapping through the map's torus masks:
//   step = v.dir_step_offsets[v.dir_remap_table[dir].step_primary]   (SAME step index for both axes)
//   *out_col = (x + step.dx) & map_width_mask(v)
//   *out_row = (y + step.dy) & map_height_mask(v)
// `dir_remap_table[dir]` only ever contributes `.step_primary` here (the other three alt-step fields
// are read by other functions in this cluster, not this one) -- see sim_state.h's `dir_remap_table`/
// `dir_step_offsets` comments for the table shapes; do not re-derive them from raw offsets.
void tile_neighbor_in_dir(const sim_view &v, int32_t x, int32_t y, int32_t dir, int32_t *out_col,
                          int32_t *out_row);

// llm_strat_heading_candidate_find_slot @0x0048d2f4.
//
// Linear-scans up to 3 candidate slots of `v.heading_candidates[heading*3 + i]` (i = 0..2) for the
// first whose `.turn_delta` equals `turn_delta`, returning that slot index; stops early at the first
// slot whose `.turn_delta == -1` (the documented "inactive/end of candidates" sentinel -- see
// sim_state.h's `heading_candidates` comment) or after all 3 slots, returning -1 if no match was
// found (including when the sentinel was hit before any slot 0 was checked).
int32_t heading_candidate_find_slot(const sim_view &v, int32_t heading, int32_t turn_delta);

} // namespace detail

// Live wrappers: the logic applied to state().read. Each matches its original's committed __watcall
// shape (see addr/mh_export.gen.h's sig_ typedefs) -- note tile_neighbor_in_dir's out-params carry the
// committed `int32_t *` pointee at the public boundary (TACT1-P C6, 2026-09-04), matching
// sig_llm_strat_tile_neighbor_in_dir exactly, same convention as sim_unit_predict_coords.cpp's public
// wrapper.
double  dir_step_factor(int32_t dir);
void    tile_neighbor_in_dir(int32_t x, int32_t y, int32_t dir, int32_t *out_col, int32_t *out_row);
int32_t heading_candidate_find_slot(int32_t heading, int32_t turn_delta);

namespace detail {
} // namespace detail

} // namespace mh::sim
