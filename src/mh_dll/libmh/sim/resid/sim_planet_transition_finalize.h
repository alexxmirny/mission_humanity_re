#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

struct planet_transition_finalize_calls {
    void (*time_resync_and_tick)();                                   // llm_strat_time_resync_and_tick @0x00449e21
    int32_t (*invasion_due_check)();                                  // llm_strat_invasion_due_check @0x0049949d -- returns the original's 0/1 flag (prototype corrected 2026-08-31, EN v384); this caller DISCARDS it, exactly as the original does at 0x0044d187
    void (*invasion_alert_clear)(int32_t planet);                     // llm_strat_invasion_alert_clear @0x0049b4dd
    void (*prod_deliver_arrivals)();                                  // llm_strat_prod_deliver_arrivals @0x0048dc65
    int32_t (*deploy_starting_squad)();                               // llm_strat_deploy_starting_squad @0x00465d16
    uint32_t (*player_presence_lost)(uint32_t player, uint32_t mode); // llm_strat_player_presence_lost @0x00498089 (ORIGINAL -- not mh::sim::player_presence_lost)
    void (*cam_mark_viewport_dirty)();                                // llm_map_cam_mark_viewport_dirty @0x004a5a6b
};

const planet_transition_finalize_calls &live_planet_transition_finalize_calls();

namespace detail {

// llm_strat_planet_transition_finalize @0x0044d160. Reads G_PLANET_INDEX/G_PLANET_STATUS/
// PlayerSide/Planets[]/progress through `v`, writes cam_pan_target_col / _G_LLM_STRAT_SIM_ACTIVE /
// _G_LLM_STRAT_PLANET_TRANSITION_STATE through `own`, and reaches every callee (all frontier)
// through `c`. void return, matching the original.
void planet_transition_finalize(const sim_view &v, sim_store &own,
                                const planet_transition_finalize_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_planet_transition_finalize_calls().
void planet_transition_finalize();

} // namespace mh::sim
