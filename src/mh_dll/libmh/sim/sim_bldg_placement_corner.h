#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_bldg_calc_placement_corner_from_center_by_type @0x0048d0ea. See the header banner above for
// the full derivation; the .cpp carries the per-block address citation. Out params are `uint32_t *`,
// matching the asm's own `uint *out_x` / `uint *out_y` (masked, so never negative).
void bldg_calc_placement_corner_from_center_by_type(const sim_view &v, uint16_t building_type,
                                                    int32_t center_x, int32_t center_y,
                                                    uint32_t *out_x, uint32_t *out_y);

} // namespace detail

// Live wrapper: the logic applied to state().read. Signature matches the committed __watcall shape
// already in addr/mh_calls.gen.h / addr/mh_export.gen.h exactly
// (sig_llm_bldg_calc_placement_corner_from_center_by_type).
void bldg_calc_placement_corner_from_center_by_type(uint16_t building_type, int32_t center_x,
                                                    int32_t center_y, uint32_t *out_x, uint32_t *out_y);

namespace detail {
} // namespace detail

} // namespace mh::sim
