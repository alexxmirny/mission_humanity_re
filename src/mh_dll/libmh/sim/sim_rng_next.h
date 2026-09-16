#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_rng_next @0x004b4d01. See the header banner above for the full derivation; the .cpp
// carries the per-line address citation.
int32_t rng_next(sim_store &own, int32_t channel, int32_t lo, int32_t hi);

} // namespace detail

// Live wrapper: the logic applied to state().own. Matches the committed prototype
// (addr/mh_export.gen.h's sig_llm_strat_rng_next / addr/mh_calls.gen.h's llm_strat_rng_next
// trampoline signature) exactly.
int32_t rng_next(int32_t channel, int32_t lo, int32_t hi);

namespace detail {
} // namespace detail

} // namespace mh::sim
