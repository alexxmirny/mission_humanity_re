#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_unit_set_state_of @0x004869b0. See the header derivation above: one write,
// `unit_at(player, unit_index).state = state`, no callees besides the inert Watcom stack probe --
// same "no _calls struct" shape as sim_unit_set_state.cpp's sibling, except this one addresses the
// roster explicitly rather than through the current-unit cursor.
void unit_set_state_of(sim_store &own, int32_t player, int32_t unit_index, int16_t state);

} // namespace detail

// Live wrapper: the logic applied to state().own. Matches the original's committed
// __watcall(EAX,EDX,BX) shape (sig_llm_strat_unit_set_state_of).
void unit_set_state_of(int32_t player, int32_t unit_index, int16_t state);

namespace detail {
} // namespace detail

} // namespace mh::sim
