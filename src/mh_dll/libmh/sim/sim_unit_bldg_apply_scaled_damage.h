#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_unit_bldg_apply_scaled_damage @0x0049a9aa. See the header banner above for the full derivation;
// the .cpp carries the per-branch address citation. No outward calls besides the inert stack probe.
void unit_bldg_apply_scaled_damage(const sim_view &v, sim_store &own, uint32_t target_selector,
                                   int32_t target_index);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (sig_llm_unit_bldg_apply_scaled_damage) exactly: void(uint32_t target_selector, int32_t target_index).
void unit_bldg_apply_scaled_damage(uint32_t target_selector, int32_t target_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
