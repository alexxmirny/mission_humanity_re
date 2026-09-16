#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_storage_release_door_held_by_unit @0x00485466. See the header banner above for the full
// derivation; the .cpp carries the per-step address citation.
int32_t storage_release_door_held_by_unit(sim_store &own, int32_t player, int32_t unit_idx);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (addr/mh_calls.gen.h's llm_strat_storage_release_door_held_by_unit trampoline signature) exactly.
int32_t storage_release_door_held_by_unit(int32_t player, int32_t unit_idx);

namespace detail {
} // namespace detail

} // namespace mh::sim
