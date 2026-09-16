#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_unit_force_disembark @0x0046d0cd. See the header derivation above. No outward calls besides the
// inert Watcom stack probe, so there is no `_calls` struct -- same shape as
// sim_unit_path_free_slot.cpp's pure body.
void unit_force_disembark(const sim_view &v, sim_store &own, uint32_t unit_player, int32_t unit_index);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the original's committed __watcall(EAX,EDX)
// shape (sig_llm_unit_force_disembark).
void unit_force_disembark(uint32_t unit_player, int32_t unit_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
