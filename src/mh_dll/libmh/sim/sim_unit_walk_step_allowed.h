#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {
namespace detail {

// llm_strat_unit_walk_step_allowed @0x0048c79a. See the header banner above for the full derivation,
// including the DECLARED DIVERGENCE on the unreachable `independent > 2` arm.
int32_t unit_walk_step_allowed(const sim_view &v, uint32_t player, int32_t unit_idx);

} // namespace detail

// Live wrapper: the logic applied to state().read. Matches the committed prototype
// (sig_llm_strat_unit_walk_step_allowed) exactly.
int32_t unit_walk_step_allowed(uint32_t player, int32_t unit_idx);

namespace detail {
} // namespace detail

} // namespace mh::sim
