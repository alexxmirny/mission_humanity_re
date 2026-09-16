#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call, indirected (like every sim/ TU) so detail:: stays testable under simtest.
// llm_strat_invasion_alert_clear is a frontier original (not among the sim_resid sibling
// translations in this slice -- confirmed in _CONTEXT_CE.md's callee table), so it is reached
// through mh::call:: in live_invasion_alert_reset_calls(), never called directly.
struct invasion_alert_reset_calls {
    void (*invasion_alert_clear)(int32_t planet); // llm_strat_invasion_alert_clear @0x0049b4dd
};

const invasion_alert_reset_calls &live_invasion_alert_reset_calls();

namespace detail {

// llm_strat_invasion_alert_reset_all @0x0049b447. Calls c.invasion_alert_clear(i) for i = 0..31,
// then zeroes own.advisor_next_time(). void return, matching the original. Takes no sim_view read
// -- the original reads nothing from state, only writes.
void invasion_alert_reset_all(sim_store &own, const invasion_alert_reset_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_invasion_alert_reset_calls().
void invasion_alert_reset_all();

} // namespace mh::sim
