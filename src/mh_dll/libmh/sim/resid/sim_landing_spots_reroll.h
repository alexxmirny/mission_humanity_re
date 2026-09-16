#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call, indirected (like every sim/ TU) so detail:: stays testable under simtest.
// llm_rand_below is a frontier original (not among the sim_resid sibling translations -- confirmed
// in _CONTEXT_CE.md's callee table), so it is reached through mh::call:: in
// live_landing_spots_reroll_calls(), never called directly.
struct landing_spots_reroll_calls {
    int32_t (*rand_below)(int32_t upper_bound); // llm_rand_below @0x00499f49
};

const landing_spots_reroll_calls &live_landing_spots_reroll_calls();

namespace detail {

// llm_strat_landing_spots_reroll_out_of_bounds @0x00454de5. Walks the active landing-spot run
// (own.landing_spot_at), comparing each entry's x/y against v.map_width/v.map_height and rerolling
// the out-of-range axis (or axes) via c.rand_below. void return, matching the original.
void landing_spots_reroll_out_of_bounds(const sim_view &v, sim_store &own, const landing_spots_reroll_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_landing_spots_reroll_calls().
void landing_spots_reroll_out_of_bounds();

} // namespace mh::sim
