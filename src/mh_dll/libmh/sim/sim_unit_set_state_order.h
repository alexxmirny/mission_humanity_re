#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_unit_set_state_order @0x00486657. See the header derivation above: two writes,
// `cur_unit().state = state` then `.order = order`, no callees besides the inert Watcom stack probe --
// same "no _calls struct" shape as sim_unit_set_state_order_of.cpp's explicit-target sibling.
void unit_set_state_order(sim_store &own, uint16_t state, uint16_t order);

} // namespace detail

// Live wrapper: the logic applied to state().own. Matches the original's committed __watcall(EAX,EDX)
// shape (sig_llm_strat_unit_set_state_order).
void unit_set_state_order(uint16_t state, uint16_t order);

namespace detail {
} // namespace detail

} // namespace mh::sim
