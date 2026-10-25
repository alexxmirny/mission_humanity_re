#pragma once
#include <cstdint>

#include "sim/sim_game_handle_progress.h" // handle_progress_calls / live_handle_progress_calls / detail::game_handle_progress
#include "sim/sim_game_update_progress.h" // game_update_progress_calls / live_game_update_progress_calls / detail::game_update_progress
#include "sim/sim_state.h"

namespace mh::sim {

// mods:A2 + mods:UNL1 -- `[net] project_gate_fix` (launcher "Tech tree fixes"; one switch drives BOTH fixes, the recheck below included): drop the PROJECT gate in progress_propagate_unlocks (retail withholds a
// research PROJECT whose depend[] are all acquired once the CURRENT planet's own invention is acquired by
// PlayerSide, i.e. on every conquered planet: the offer waits for an unconquered one). Default off = retail.
// Set from reimpl_fixes by lockstep::set_fixes. Campaign only: a multiplayer/skirmish session sits on the
// pseudo planet 0x1f whose invention_index is 0 (never acquired), so the gate is open there either way.
void set_project_gate_fix(bool on);
bool project_gate_fix();

namespace detail {

// mods:UNL1 -- the recheck itself. A no-op (returns -1) unless `on` and the session is single-player
// (session_mode == SESSION_SP); `arrival` additionally requires G_PLANET_STATUS[G_PLANET_INDEX] != UNKNOWN (a planet the
// player has been on: a first arrival is the retail session-begin / landing path). Runs `propagate(player)` for the LOCAL
// player (PlayerSide) first, then for every OTHER player whose profile has status_flags bit0 (slot enabled) set, in
// ascending index -- the same filter and order game_UpdateProgress's PLANET/SYSTEM arms use for "the other players". Returns
// the number of players propagated. `propagate` is the live progress_propagate_unlocks in production, a recorder in the
// selftest. Every message / event the pass can raise is gated on player == PlayerSide inside HandleProgress's callees
// (add_to_available_buildings_with_check, notify_unit_available, add_planet_to_available, notify_system_available,
// handle_upgrade), so a computer player's pass is silent: it only fills that player's own offer lists.
int progress_recheck_unlocks(const sim_view &v, bool on, bool arrival, void (*propagate)(uint16_t player));

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

// mods:UNL1 live wrappers (state() + the switch). Called after a load (llm_game_load's apply tail) and at the tail of a
// planet transition; each is a no-op with the switch off or outside single-player.
void progress_recheck_after_load();
void progress_recheck_after_arrival();

namespace detail {
} // namespace detail

} // namespace mh::sim
