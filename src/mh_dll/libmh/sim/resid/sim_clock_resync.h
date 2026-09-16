#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim::detail {

// llm_strat_clock_resync_units_and_buildings @0x00499a3a. Reads _G_LLM_STRAT_PLAYERS[].status_flags
// through `v`; rewrites every live unit's activity_clock/rotation_clock and every live building's
// last_tick_time/anim_dur[12] through `own`. No outward calls.
void clock_resync_units_and_buildings(const sim_view &v, sim_store &own, double new_time);

} // namespace mh::sim::detail

namespace mh::sim {

// Live wrapper: the logic applied to state(). Matches the original's committed
// `void __watcall llm_strat_clock_resync_units_and_buildings(double new_time)` signature.
void clock_resync_units_and_buildings(double new_time);

} // namespace mh::sim
