#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_map_bldg_footprint_clear_passable @0x0049383b. See the header banner above for the full
// derivation; the .cpp carries the per-block address citation.
void bldg_footprint_clear_passable(const sim_view &v, sim_store &own, int32_t origin_x,
                                   int32_t origin_y, int32_t building_idx);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (sig_llm_map_bldg_footprint_clear_passable) exactly.
void bldg_footprint_clear_passable(int32_t origin_x, int32_t origin_y, int32_t building_idx);

namespace detail {
} // namespace detail

} // namespace mh::sim
