#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_bldg_find_idle_producer_for_unit @0x004e2478. See the header banner above for the full
// derivation; the .cpp carries the per-branch address citation. Returns the found building's roster
// slot index (>=1), or 0 if no idle producer of unit_id exists on player_id's side.
int32_t bldg_find_idle_producer_for_unit(const sim_view &v, int32_t player_id, int32_t unit_id);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (mh_calls.gen.h's llm_strat_bldg_find_idle_producer_for_unit) exactly.
int32_t bldg_find_idle_producer_for_unit(int32_t player_id, int32_t unit_id);

namespace detail {
} // namespace detail

} // namespace mh::sim
