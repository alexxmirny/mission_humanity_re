#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_unit_set_state_order_of @0x00486913. See the header derivation above: two writes,
// `unit_at(player, unit_index).state = state` then `.order = param`, no callees besides the inert
// Watcom stack probe -- same "no _calls struct" shape as sim_unit_set_state_of.cpp's sibling.
void unit_set_state_order_of(sim_store &own, int32_t player, int32_t unit_index, int16_t state,
                             int16_t param);

} // namespace detail

// Live wrapper: the logic applied to state().own. Matches the original's committed
// __watcall(EAX,EDX,BX,CX) shape (sig_llm_strat_unit_set_state_order_of).
void unit_set_state_order_of(int32_t player, int32_t unit_index, int16_t state, int16_t param);

namespace detail {
} // namespace detail

} // namespace mh::sim
