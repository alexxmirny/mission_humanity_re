#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_unit_hover_tile_crowded @0x0048cf3e. See the header derivation above; the .cpp carries
// the per-branch address citation. Returns 1 (another unit on this tile is HOVER_ENGAGE) or 0.
int32_t unit_hover_tile_crowded(const sim_view &v, int32_t player, uint32_t unit_index);

} // namespace detail

// Live wrapper: the logic applied to state().read. Matches the committed prototype
// (sig_llm_strat_unit_hover_tile_crowded) exactly.
int32_t unit_hover_tile_crowded(int32_t player, uint32_t unit_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
