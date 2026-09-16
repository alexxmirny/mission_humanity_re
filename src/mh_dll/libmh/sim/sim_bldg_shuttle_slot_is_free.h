#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_PORT / _H_PORT / _A_MOTHER / _H_MOTHER (already committed there)
#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_bldg_shuttle_slot_is_free @0x0048fa97. See the header banner above for the full
// derivation. Returns 1 (free), 0 (busy), or -1 (not a shuttle-capable building type).
int32_t bldg_shuttle_slot_is_free(const sim_view &v, int32_t player, int32_t building_id);

} // namespace detail

// Live wrapper: the logic applied to state().read. Matches the committed prototype
// (sig_llm_strat_bldg_shuttle_slot_is_free) exactly.
int32_t bldg_shuttle_slot_is_free(int32_t player, int32_t building_id);

namespace detail {
} // namespace detail

} // namespace mh::sim
