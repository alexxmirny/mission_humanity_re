#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_unit_state_parked_noop @0x0047e276. See the header derivation above: the entire body
// is `own.tick_budget() = 0.0;`, no callees besides the inert Watcom stack probe -- same
// "no _calls struct" shape as sim_unit_set_state.cpp's pure write.
void unit_state_parked_noop(sim_store &own);

// llm_strat_unit_state_default_noop @0x0047e2ac. Byte-for-byte identical body to
// unit_state_parked_noop above (see the header derivation) -- a SEPARATE detail:: function because
// it is a separate original function/address with its own shadow site (translator-brief rule 4).
void unit_state_default_noop(sim_store &own);

} // namespace detail

// Live wrappers: the logic applied to state().own. Match the originals' committed void(void)
// prototypes exactly.
void unit_state_parked_noop();
void unit_state_default_noop();

namespace detail {
} // namespace detail

} // namespace mh::sim
