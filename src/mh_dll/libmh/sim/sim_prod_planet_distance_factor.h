#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_prod_planet_distance_factor @0x00490221. See the header banner above for the full derivation.
// Unconditionally returns 1.0; `src_planet`/`dest_planet` are read into locals in the original and
// then never used again, so they are accepted here (to match the committed prototype exactly) but
// genuinely unused.
double planet_distance_factor(int32_t src_planet, int32_t dest_planet);

} // namespace detail

// Public wrapper. Parameter/return types match the committed prototype
// (sig_llm_prod_planet_distance_factor) exactly: double(int32_t src_planet, int32_t dest_planet).
double planet_distance_factor(int32_t src_planet, int32_t dest_planet);

namespace detail {
} // namespace detail

} // namespace mh::sim
