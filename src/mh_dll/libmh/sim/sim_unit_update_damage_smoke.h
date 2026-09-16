#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_unit_update_damage_smoke @0x004870d8. See the header banner above for the full
// derivation; the .cpp carries the per-instruction address citation. No outward calls -- the only
// callee in the original is utils_math_trunc, which is reproduced as inline asm rather than
// indirected through a `calls` struct (there is nothing left to indirect).
void unit_update_damage_smoke(const sim_view &v, sim_store &own, uint32_t player, int32_t unit_idx);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (sig_llm_strat_unit_update_damage_smoke) exactly.
void unit_update_damage_smoke(uint32_t player, int32_t unit_idx);

namespace detail {
} // namespace detail

} // namespace mh::sim
