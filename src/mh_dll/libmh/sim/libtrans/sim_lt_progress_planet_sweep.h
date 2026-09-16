#pragma once
#include <cstdint>

#include "sim/sim_game_handle_progress.h" // sibling: handle_progress_calls, detail::game_handle_progress,
                                          // and (see banner above) INVENTION_TYPE_PLANET/_SYSTEM
#include "sim/sim_game_update_progress.h" // sibling: game_update_progress_calls, detail::game_update_progress
#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_progress_recheck_planet_system_all_players @0x00454846 (0x88 B). For every Progress[] entry of
// type PLANET or SYSTEM, calls game_HandleProgress(player, inv) for every SLOT_ENABLED player.
void recheck_planet_system_all_players(
    const sim_view &v, sim_store &own,
    const handle_progress_calls &c_hp = live_handle_progress_calls());

// game_UpdatePlanetProgress @0x004548ce (0x98 B). Same sweep as
// recheck_planet_system_all_players, but each surviving (player, inv) pair calls
// game_HandleProgress(player, inv) AND THEN game_UpdateProgress(player, inv).
void game_update_planet_progress(
    const sim_view &v, sim_store &own,
    const handle_progress_calls      &c_hp = live_handle_progress_calls(),
    const game_update_progress_calls &c_up = live_game_update_progress_calls());

} // namespace detail

// Live wrappers: the logic applied to state(). Both match their committed export prototypes
// (`void(__cdecl*)(void)`) exactly -- neither takes an argument.
void recheck_planet_system_all_players();
void game_update_planet_progress();

namespace detail {
} // namespace detail

} // namespace mh::sim
