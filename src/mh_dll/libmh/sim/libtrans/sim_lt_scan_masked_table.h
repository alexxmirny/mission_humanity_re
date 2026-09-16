#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_scan_masked_table_for_empty_cell @0x004b4bdd. `wrap_mask` is the caller-supplied binding for
// _G_LLM_STRAT_AI_GRID_WRAP_MASK (mh::ai::grid_wrap_mask() in the live wrapper below, fixture storage
// in the offline oracle). See the header banner for the destructive-cursor derivation.
int32_t scan_masked_table_for_empty_cell(uint32_t *wrap_mask, const uint8_t *grid, int32_t grid_width,
                                         int32_t grid_height, const uint8_t *footprint_mask,
                                         int32_t span_x, int32_t span_y, int32_t start_x,
                                         int32_t start_y);

} // namespace detail

// int32_t __cdecl llm_scan_masked_table_for_empty_cell(...) -- addr/mh_calls.gen.h:1589. Matches the
// committed prototype's `uint8_t *grid` / `uint8_t *footprint_mask` exactly (TACT1-P C6, 2026-09-04 --
// the committed row carries the `byte *` pointee now, not a generic `void *`); the additional cast to
// `const uint8_t *` happens at the detail:: boundary just below.
int32_t scan_masked_table_for_empty_cell(uint8_t *grid, int32_t grid_width, int32_t grid_height,
                                         uint8_t *footprint_mask, int32_t span_x, int32_t span_y,
                                         int32_t start_x, int32_t start_y);

namespace detail {
} // namespace detail

} // namespace mh::sim
