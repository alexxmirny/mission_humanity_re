#pragma once
#include <cstdint>

#include "sim/resid/sim_land_players_on_planet.h" // INTRA-SLICE: detail::land_players_on_planet
#include "sim/resid/sim_session_state_reset.h"    // INTRA-SLICE: detail::session_state_reset
#include "sim/sim_state.h"

namespace mh::sim {

// The outward calls, indirected (like every sim/ TU) so detail:: stays testable under simtest. All
// are frontier originals (none of them are among the ten sim_resid sibling translations) EXCEPT the
// two intra-slice edges, which are reached directly via detail:: (see the header banner), not
// through this struct.
struct session_begin_multi_calls {
    void (*scenario_planet_clone)(void *cfg_blob);   // llm_strat_scenario_planet_clone @0x0045ba25
    void (*rng_seed_ch0)(uint32_t seed);             // llm_strat_rng_seed_ch0 @0x00499fbf
    void (*player_set_human)(uint8_t player);        // llm_game_player_set_human @0x0049e759
    void (*ui_message_queue_clear_all)();            // llm_ui_message_queue_clear_all @0x00455b0a -- EFFECTFUL WALL, called twice
    void (*map_read_map_pre)(uint32_t planet_index); // map_ReadMap_pre @0x004552d6
    void (*net_lockstep_peer_timing_reset)();        // llm_net_lockstep_peer_timing_reset @0x0049fef7
    void (*player_profile_init)(uint32_t player, uint32_t controller_flags, uint32_t race,
                                double game_clock, uint32_t color_index, char *name_str,
                                int32_t side_id);        // llm_strat_player_profile_init @0x00454985
    void (*diplomacy_init_multiplayer)();                // llm_diplomacy_init_multiplayer @0x00465ac0
    void (*reload_snapshot_resync_clocks)(double now);   // llm_game_reload_snapshot_resync_clocks @0x00425bcd
    void (*progress_propagate_unlocks)(uint16_t player); // llm_progress_propagate_unlocks @0x00440cb5
    void (*update_planet_progress)();                    // game_UpdatePlanetProgress @0x004548ce
    void (*game_speed_recompute)();                      // llm_game_speed_recompute @0x00497623
    // Two of the original's stubs here are EMPTY (stack probe + return) and are no longer called at
    // all since SIMABI-HOOKS 2026-09-10: llm_lobby_map_recv_step_stub @0x0049bfa5 before the delay
    // stub, and llm_teardown_hook_stub @0x0049bc44 on the error branch. The delay stub below is a
    // different animal -- it RETURNS a value the body branches on, so it stays.
    int32_t (*net_lockstep_sync_delay_stub)(); // llm_net_lockstep_sync_delay_stub @0x0049c02c -- STUB, called anyway
    void (*menu_force_return_to_main)();       // llm_menu_force_return_to_main @0x004c862f -- EFFECTFUL WALL
};

const session_begin_multi_calls &live_session_begin_multi_calls();

namespace detail {

// llm_strat_session_begin_multi @0x0045435f. Reads game_clock/tutorial_step/the two new-declared
// Players()/net_* regions through `v`, writes PlayerSide/player_race/session_mode/the four
// home-planet-slot cells/rng_seed_byte/show_unit_flags/mp_ally_victory_rule_flag/chat_target_mask/
// sim_active/lockstep_adapt_next_time through `own`, reaches every frontier callee through `c`, and
// makes the two intra-slice calls directly. Returns the original's `int` (see the header's RETURN
// VALUE note).
// ---- the offline-oracle seam (SIM-RESID batch B, 2026-08-31) ----------------------------------
// The trailing `c_*` parameters exist so an OFFLINE ORACLE can substitute a SIBLING's calls table.
// Each is defaulted to the same `live_*_calls()` this body used to name inline, so every production
// caller is unchanged and the default IS the previous behaviour. Why it cannot stay inline:
// `net_selftest.exe` loads no game image, and a `live_*` table holds `mh::call::` naked thunks
// against absolute game VAs -- so a `detail::` body that binds one itself FAULTS before its first
// assertion rather than failing, and the parent's own mock cannot intercept it because the sibling
// is not reached through `c`. TRANSITIVE, not just direct: this closure is three deep, and an
// oracle for the top must be able to mock the bottom. Gated by tools/lint_detail_calls.py.
int32_t session_begin_multi(
    const sim_view &v, sim_store &own, const session_begin_multi_calls &c, void *cfg_blob,
    const session_state_reset_calls     &c_ssr  = live_session_state_reset_calls(),
    const new_game_init_calls           &c_ngi  = live_new_game_init_calls(),
    const land_players_on_planet_calls  &c_lpop = live_land_players_on_planet_calls(),
    const planet_map_session_init_calls &c_pmsi = live_planet_map_session_init_calls());

} // namespace detail

// Live wrapper: the logic applied to state() and live_session_begin_multi_calls().
int32_t session_begin_multi(void *cfg_blob);

// ---- THE SESSION-ENTRY OBSERVER (C10, 2026-09-05) -------------------------------------------------
//
// WHY. net_seams.cpp detours this entry, and it carries more than a log line: the MANUAL-MENU MP
// HOST-COUNT FIX-UP (DAT_005d54bc is reset 2->0 in the async gap before this call, so without the
// fix-up the host builds a mode-2 solo game while the client builds mode-3) and mh::desync's
// session_reset(). Promotion runs BEFORE that detour installs -- measured, 10:02:04.219 against
// 10:02:04.524 -- so install_export claims the entry first and the trampoline is then refused with
// `; session_begin_multi logger NOT armed`. Both effects were silently lost whenever
// [promote] sim_resid=1, which is the ship default.
//
// This is the D18 arrangement, not a rebind: there the harness LOSES the entry race and registers a
// callback into the promoted body instead (mh::sim::set_dispatch_observer). Same shape, same reason.
// Fired before the body, matching where the displaced run-before trampoline ran -- load-bearing here,
// since the fix-up must land before the body reads the value it keys SESSION_MODE off.
//
// LAYERING: a setter, so a reimplementation TU need not include a seams header.
void set_session_begin_multi_observer(void (*fn)());

// What is registered, or nullptr -- so a test can assert the WIRING, not the pointer's existence.
void (*session_begin_multi_observer())();

// Call the registered observer, if any. Split out from the body for the same reason
// fire_dispatch_observer is: the body needs live game state and cannot be driven offline, this can.
void fire_session_begin_multi_observer();

} // namespace mh::sim
