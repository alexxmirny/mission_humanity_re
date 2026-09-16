#pragma once
#include <cstdint>

#include "sim/resid/sim_land_players_on_planet.h" // INTRA-SLICE: detail::land_players_on_planet
#include "sim/resid/sim_session_state_reset.h"    // INTRA-SLICE: detail::session_state_reset
#include "sim/sim_state.h"

namespace mh::sim {

// The outward calls, indirected (like every sim/ TU) so detail:: stays testable under simtest.
struct planet_session_begin_calls {
    // llm_strat_rng_seed_wallclock_seconds @0x00499dc8 IS GONE (LIFT-TABLE S5, 2026-09-09) -- it
    // became an INIT PARAMETER (libmh_set_session_seed), so this TU reads mh::state::session_seed()
    // rather than pulling a wall-clock read through the host table.
    void (*rng_seed_ch0)(uint32_t seed);             // llm_strat_rng_seed_ch0 @0x00499fbf
    void (*rng_seed_ch1)(uint32_t seed);             // llm_strat_rng_seed_ch1 @0x00499ff2
    void (*ui_message_queue_clear_all)();            // llm_ui_message_queue_clear_all @0x00455b0a
    void (*map_read_map_pre)(uint32_t planet_index); // map_ReadMap_pre @0x004552d6
    void (*player_set_human)(uint8_t player);        // llm_game_player_set_human @0x0049e759
    // getAsciiVer @0x004cf427: mh_calls.gen.h declares this returning uint32_t, but every call site
    // (including this one) immediately uses the result as a pointer -- see the DECLARED NEED in the
    // translation report. Kept at its generated (numeric) type here; detail:: casts the result.
    uint32_t (*get_ascii_ver)(void *str);
    void (*player_profile_init)(uint32_t player, uint32_t controller_flags, uint32_t race,
                                double game_clock, uint32_t color_index, char *name_str,
                                int32_t side_id);        // llm_strat_player_profile_init @0x00454985
    void (*diplomacy_init_skirmish)();                   // llm_diplomacy_init_skirmish @0x00465baf
    void (*reload_snapshot_resync_clocks)(double now);   // llm_game_reload_snapshot_resync_clocks @0x00425bcd
    void (*progress_propagate_unlocks)(uint16_t player); // llm_progress_propagate_unlocks @0x00440cb5
    // (llm_strat_session_begin_empty_stub @0x00454966 was a member here until SIMABI-HOOKS
    // 2026-09-10: retail's body is a stack probe and a return, so the call is gone.)
    void (*time_resync_and_tick)(); // llm_strat_time_resync_and_tick @0x00449e21
    void (*snd_ambient_reseed_planet_event_times)(int32_t planet_index,
                                                  double  current_time); // @0x0045e277
    void (*map_cam_mark_viewport_dirty)();                              // llm_map_cam_mark_viewport_dirty @0x004a5a6b
};

const planet_session_begin_calls &live_planet_session_begin_calls();

namespace detail {

// llm_strat_planet_session_begin @0x004541c3. Reads the planet/text/clock inputs through `v`,
// mutates player_race / session_mode / player_side / local_player_slot / sim_active /
// show_unit_flags / mp_ally_victory_rule_flag through `own`, reaches the effectful UI/map/profile
// originals through `c`, and drives three intra-slice siblings directly
// (session_state_reset / land_players_on_planet / session_clear_system_presence_flag).
// ---- the offline-oracle seam (SIM-RESID batch B, 2026-08-31) ----------------------------------
// The trailing `c_*` parameters exist so an OFFLINE ORACLE can substitute a SIBLING's calls table.
// Each is defaulted to the same `live_*_calls()` this body used to name inline, so every production
// caller is unchanged and the default IS the previous behaviour. Why it cannot stay inline:
// `net_selftest.exe` loads no game image, and a `live_*` table holds `mh::call::` naked thunks
// against absolute game VAs -- so a `detail::` body that binds one itself FAULTS before its first
// assertion rather than failing, and the parent's own mock cannot intercept it because the sibling
// is not reached through `c`. TRANSITIVE, not just direct: this closure is three deep, and an
// oracle for the top must be able to mock the bottom. Gated by tools/lint_detail_calls.py.
void planet_session_begin(
    const sim_view &v, sim_store &own, const planet_session_begin_calls &c, int32_t race,
    int32_t                              reset_flag,
    const session_state_reset_calls     &c_ssr  = live_session_state_reset_calls(),
    const new_game_init_calls           &c_ngi  = live_new_game_init_calls(),
    const land_players_on_planet_calls  &c_lpop = live_land_players_on_planet_calls(),
    const planet_map_session_init_calls &c_pmsi = live_planet_map_session_init_calls());

} // namespace detail

// Live wrapper: the logic applied to state() and live_planet_session_begin_calls().
void planet_session_begin(int32_t race, int32_t reset_flag);

} // namespace mh::sim
