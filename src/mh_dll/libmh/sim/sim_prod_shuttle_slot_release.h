#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_prod_shuttle_slot_release @0x0046318d. See the header banner above for the full
// derivation; the .cpp carries the per-store address citation.
void prod_shuttle_slot_release(sim_store &own, int32_t player, int32_t slot);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (addr/mh_calls.gen.h's llm_strat_prod_shuttle_slot_release trampoline signature) exactly.
void prod_shuttle_slot_release(int32_t player, int32_t slot);

namespace detail {
} // namespace detail

} // namespace mh::sim
