#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_rng_seed_ch0 @0x00499fbf. Delegates to detail::rng_seed_channel(own, 0, value) -- see
// the header derivation. void return, matching the original.
void rng_seed_ch0(sim_store &own, uint32_t value);

// llm_strat_rng_seed_ch1 @0x00499ff2. Delegates to detail::rng_seed_channel(own, 1, value) -- see
// the header derivation. void return, matching the original.
void rng_seed_ch1(sim_store &own, uint32_t value);

} // namespace detail

// Live wrappers: the logic applied to state().own. Match each original's committed __watcall
// prototype exactly (single uint32_t argument, void return).
void rng_seed_ch0(uint32_t value);
void rng_seed_ch1(uint32_t value);

namespace detail {
} // namespace detail

} // namespace mh::sim
