#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_path_free_slot @0x004969e8. See the header derivation above. No outward calls besides
// the inert Watcom stack probe, so there is no `_calls` struct -- same shape as
// sim_unit_housing_count.cpp's pure body.
void path_free_slot(const sim_view &v, sim_store &own, uint16_t player, int32_t unit_index);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the original's committed __watcall(AX,EDX)
// shape (sig_llm_strat_path_free_slot).
void path_free_slot(uint16_t player, int32_t unit_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
