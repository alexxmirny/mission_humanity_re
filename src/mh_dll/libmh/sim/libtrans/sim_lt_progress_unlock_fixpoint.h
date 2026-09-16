#pragma once
#include <cstdint>

#include "sim/sim_game_handle_progress.h" // handle_progress_calls / live_handle_progress_calls / detail::game_handle_progress
#include "sim/sim_game_update_progress.h" // game_update_progress_calls / live_game_update_progress_calls / detail::game_update_progress
#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_progress_propagate_unlocks @0x00440cb5. The tech-tree unlock fixpoint. Reads `v.progress` /
// `v.cfg_inventions` / `v.player_side` / `v.planet_index`; writes nothing itself -- every effect goes
// through `c_hp` (game_HandleProgress, called directly as a detail:: body, never the public wrapper --
// see the header banner). void return, matching the original AND the corrected plate (G3): the
// "return value = last scanned index" reading in the OLD plate was wrong twice over (the function is
// void, and there is no such return anywhere in the body).
//
// NO SHADOW SITE (hazard 8 above) -- offline (`net_selftest libtranstest`) + adversarial review only.
void progress_propagate_unlocks(const sim_view &v, sim_store &own, uint16_t player,
                                const handle_progress_calls &c_hp = live_handle_progress_calls());

// llm_progress_recheck_projects @0x00499174. Sweeps every PROJECT-type entry with `f3 == false` and
// re-runs game_HandleProgress; tail-calls `progress_propagate_unlocks`, FORWARDING the same `c_hp` it
// received (hazard 6's width split: `player` indexes `progress` unmasked, calls pass the low 16 bits).
void progress_recheck_projects(const sim_view &v, sim_store &own, uint32_t player,
                               const handle_progress_calls &c_hp = live_handle_progress_calls());

// llm_progress_recheck_buildings @0x004991f2. Sweeps every BUILDING-type entry with `available ==
// false && f3 == false` (NOTE the opposite `available` sense from collect_available_projects, hazard
// 7) and re-runs game_HandleProgress; tail-calls `progress_propagate_unlocks` the same way.
void progress_recheck_buildings(const sim_view &v, sim_store &own, uint32_t player,
                                const handle_progress_calls &c_hp = live_handle_progress_calls());

// llm_progress_collect_available_projects @0x004990bf. Sweeps every PROJECT-type entry with
// `available == true && acquired == false && f3 == false` and calls game_UpdateProgress (NOT
// HandleProgress -- the one sweeper that reaches the ACQUISITION half); tail-calls
// `progress_propagate_unlocks`, forwarding `c_hp` down alongside its own `c_up`.
void progress_collect_available_projects(
    const sim_view &v, sim_store &own, uint32_t player,
    const game_update_progress_calls &c_up = live_game_update_progress_calls(),
    const handle_progress_calls      &c_hp = live_handle_progress_calls());

} // namespace detail

// Live wrappers: the logic applied to state() and the two cross-TU siblings' live `_calls` tables.
// Each matches its committed export prototype exactly (sig_llm_progress_propagate_unlocks /
// sig_llm_progress_collect_available_projects / sig_llm_progress_recheck_projects /
// sig_llm_progress_recheck_buildings, addr/mh_export.gen.h).
void progress_propagate_unlocks(uint16_t player);
void progress_recheck_projects(uint32_t player);
void progress_recheck_buildings(uint32_t player);
void progress_collect_available_projects(uint32_t player);

namespace detail {
} // namespace detail

} // namespace mh::sim
