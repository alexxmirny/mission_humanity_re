#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_prod_bind_planet @0x0048feef. See the header banner above for the full derivation; the
// .cpp carries the per-branch address citation. Returns 1 iff the queue slot was free and the claim
// succeeded, 0 (no-op) if it was already occupied.
int32_t prod_bind_planet(sim_store &own, int32_t player, int32_t queue_slot, int32_t shuttle_slot);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (addr/mh_calls.gen.h's llm_strat_prod_bind_planet trampoline signature) exactly.
int32_t prod_bind_planet(int32_t player, int32_t queue_slot, int32_t shuttle_slot);

namespace detail {
} // namespace detail

} // namespace mh::sim
