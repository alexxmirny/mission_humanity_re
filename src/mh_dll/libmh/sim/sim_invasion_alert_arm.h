#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_invasion_alert_arm @0x0049b49e.
// `own.invasion_alert_time_at(planet) = timestamp`. Write-only -- this function never reads the
// old value, and the write is unconditional (no bounds check on `planet` in the assembly; the
// caller is trusted to pass a valid planet index, same as every other sim accessor's contract).
void invasion_alert_arm(sim_store &own, int32_t planet, double timestamp);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Matches the committed __watcall(EAX, stack double, RET 8) shape.
void invasion_alert_arm(int32_t planet, double timestamp);

namespace detail {
} // namespace detail

} // namespace mh::sim
