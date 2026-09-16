#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_invasion_alert_clear @0x0049b4dd. Write-only, unconditional, no bounds check on
// `planet` (see header banner). `own.invasion_alert_time_at(planet) = -1.0;` is the bit-identical
// translation of the two raw dword-immediate stores.
void invasion_alert_clear(sim_store &own, int32_t planet);

// llm_strat_invasion_alert_poll @0x0049b51c. See the header banner for the full derivation of both
// x87 tests and the disagreement with the batch hazard note (uncertainties[]). Needs BOTH halves of
// state: `v` for the read-only interval constant, `own` for the read+write scan/re-arm of the timer
// array.
int32_t invasion_alert_poll(const sim_view &v, sim_store &own, double now);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Match the committed __watcall(EAX=planet)/__watcall(stack double, RET 8) shapes.
void    invasion_alert_clear(int32_t planet);
int32_t invasion_alert_poll(double now);

namespace detail {
} // namespace detail

} // namespace mh::sim
