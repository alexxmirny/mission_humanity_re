#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_storage_type_accepts_unit @0x00497b91. See the header banner above for the full
// derivation of both branches; the .cpp carries the per-line address citations.
int32_t storage_type_accepts_unit(const sim_view &v, uint32_t building_index, uint16_t unit_index);

} // namespace detail

// Live wrapper: the logic applied to state().read. Matches the committed prototype
// (sig_llm_strat_storage_type_accepts_unit) exactly.
int32_t storage_type_accepts_unit(uint32_t building_index, uint16_t unit_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
