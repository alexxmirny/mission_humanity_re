#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim::detail {

// llm_game_session_clear_system_presence_flag @0x004987ae. Reads Planets[]/G_PLANET_INDEX/
// CurrentSystem/G_PLANET_STATUS/the player-profile roster through `v`; conditionally clears
// STATUS_ALIVE (bit 0x2) on _G_LLM_STRAT_PLAYERS[2].status_flags through `own`. No outward calls.
void session_clear_system_presence_flag(const sim_view &v, sim_store &own);

} // namespace mh::sim::detail

namespace mh::sim {

// Live wrapper: the logic applied to state().
void session_clear_system_presence_flag();

} // namespace mh::sim
