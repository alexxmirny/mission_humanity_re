#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_unit_bldg_apply_lethal_damage @0x0049aa6e. See the header banner above for the full
// derivation; the .cpp carries the per-branch address citation.
void unit_bldg_apply_lethal_damage(const sim_view &v, sim_store &own, uint32_t target_ref,
                                   int32_t target_index);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototype (sig_llm_unit_bldg_apply_lethal_damage,
// addr/mh_export.gen.h) exactly: void(uint32_t target_ref, int32_t target_index).
void unit_bldg_apply_lethal_damage(uint32_t target_ref, int32_t target_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
