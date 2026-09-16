#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim::detail {

// llm_snd_ambient_reseed_planet_event_times @0x0045e277. Reads/writes
// `own.snd_ambient_by_planet_base()` (see the header banner for the byte-offset derivation) for the
// one planet named by `planet_index`; calls the in-tree sibling `rand_below_fx` once per event, in
// order. No outward (`mh::call::`) edges.
void ambient_reseed_planet_event_times(sim_store &own, int32_t planet_index, double current_time);

} // namespace mh::sim::detail

namespace mh::sim {

// Live wrapper: the logic applied to state().own. Matches the original's committed
// `void __watcall llm_snd_ambient_reseed_planet_event_times(int planet_index, double current_time)`
// signature (planet_index in EAX, current_time on the stack).
void ambient_reseed_planet_event_times(int32_t planet_index, double current_time);

} // namespace mh::sim
