#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_unit_set_state @0x004866c9. See the header derivation above: one write, `cur_unit().
// state = new_state`, no callees besides the inert Watcom stack probe -- same "no _calls struct"
// shape as sim_unit_state_predicates.cpp's pure bodies, except this one WRITES through sim_store
// rather than only reading sim_view.
void unit_set_state(sim_store &own, uint16_t new_state);

} // namespace detail

// Live wrapper: the logic applied to state().own. Matches the original's committed __watcall(AX)
// shape (sig_llm_strat_unit_set_state).
void unit_set_state(uint16_t new_state);

namespace detail {
} // namespace detail

} // namespace mh::sim
