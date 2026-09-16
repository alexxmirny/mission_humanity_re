#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_bldg_footprint_is_clear @0x0049396b. See the header banner above for the full derivation; the
// .cpp carries the per-branch address citation. Returns 1 (clear) or 0 (blocked).
int32_t bldg_footprint_is_clear(const sim_view &v, int32_t x, int32_t y, int32_t building_type,
                                uint32_t viewer);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (sig_llm_bldg_footprint_is_clear) exactly.
int32_t bldg_footprint_is_clear(int32_t x, int32_t y, int32_t building_type, uint32_t viewer);

namespace detail {
} // namespace detail

} // namespace mh::sim
