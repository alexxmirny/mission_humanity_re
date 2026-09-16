#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_dock_slot_is_busy @0x004d3b55. See the header banner above for the full derivation; the
// .cpp carries the per-branch address citation. Returns 1 (busy) or 0 (idle/empty chain).
int32_t dock_slot_is_busy(const sim_view &v, int32_t player, int32_t slot);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (sig_llm_strat_dock_slot_is_busy) exactly.
int32_t dock_slot_is_busy(int32_t player, int32_t slot);

namespace detail {
} // namespace detail

} // namespace mh::sim
