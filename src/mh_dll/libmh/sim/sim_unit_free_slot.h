#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_unit_free_slot @0x00487b25. See the header banner for the full four-write derivation.
// Write-only -- this function never reads through `v` (no sim_view parameter needed), matching
// sim_invasion_alert_arm.h's identical "pure sim_store write, no callees" shape.
void unit_free_slot(sim_store &own, uint32_t player, int32_t slot);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Matches the committed prototype (sig_llm_strat_unit_free_slot: void(uint32_t, int32_t)) exactly.
void unit_free_slot(uint32_t player, int32_t slot);

namespace detail {
} // namespace detail

} // namespace mh::sim
