#include "sim/sim_invasion_alert_arm.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void invasion_alert_arm(sim_store &own, int32_t planet, double timestamp) {
    // 0x0049b4b9-0x0049b4cb: one 8-byte store, no branches, no callees.
    own.invasion_alert_time_at(planet) = timestamp;
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void invasion_alert_arm(int32_t planet, double timestamp) {
    sim_state st = state();
    detail::invasion_alert_arm(st.own, planet, timestamp);
}


} // namespace mh::sim
