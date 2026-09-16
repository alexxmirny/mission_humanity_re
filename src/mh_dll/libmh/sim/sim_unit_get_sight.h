#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_unit_get_sight @0x004d4888. Returns Unit[units[ref_owner(unit_ref)][unit_index].
// unit_proto_id].sight, zero-extended. Pure read, no callees.
uint32_t unit_get_sight(const sim_view &v, uint32_t unit_ref, int32_t unit_index);

} // namespace detail

// Live wrapper: the logic applied to state().read. Matches the committed prototype
// (sig_llm_strat_unit_get_sight) exactly.
uint32_t unit_get_sight(uint32_t unit_ref, int32_t unit_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
