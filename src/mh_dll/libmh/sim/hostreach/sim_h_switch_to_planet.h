#pragma once
#include <cstdint>

#include "sim/hostreach/sim_h_diplomacy_restore_relations.h" // intra-slice sibling, direct call (rule 3a)
#include "sim/libtrans/sim_lt_ambient.h"                     // already-translated sim sibling, direct call (CORRECTION #3)
#include "sim/resid/sim_land_players_on_planet.h"            // threaded sibling, rule 3c (CORRECTION #1)
#include "sim/resid/sim_session_clear_presence_flag.h"       // struct-less sibling, direct call (CORRECTION #2)
#include "sim/sim_state.h"

namespace mh::sim {

// The genuine frontier callees plus the one owned-row-with-no-sibling-yet, indirected like every sim/
// TU so detail:: stays testable under simtest (translator-brief rule 3b). See the header banner's
// CALLEE ROUTING section for why llm_game_land_players_on_planet / llm_game_session_clear_system_
// presence_flag / llm_diplomacy_restore_relations / llm_snd_ambient_reseed_planet_event_times are NOT
// members here (each is an already-translated sim-closure sibling, called directly instead).
struct switch_to_planet_calls {
    uint32_t (*time_get_ticks_ms)(); // llm_time_get_ticks_ms @0x004d056c (frontier)
    int32_t (*time_tick)();          // llm_strat_time_tick @0x0043eea3 (frontier for this closure -- see banner)
    void (*sim_tick)();              // llm_strat_sim_tick @0x0043f3eb (frontier)
    uint32_t (*save_planet_to_disk)(uint32_t planet_index,
                                    uint32_t with_progress); // map_SavePlanetToDisk @0x00447bb3 (frontier -- see banner)
    uint32_t (*load_planet_from_disk)(int32_t  planet_index,
                                      uint32_t with_progress); // map_LoadPlanetFromDisk @0x00448107 (frontier -- see banner)
    void (*read_map_pre)(uint32_t planet_index);               // map_ReadMap_pre @0x004552d6 (frontier)
    int32_t (*sim_clock_advance)();                            // llm_strat_sim_clock_advance @0x0044d238 (owned row, MH_LIBMH_BIND, declared_needs)
};

const switch_to_planet_calls &live_switch_to_planet_calls();

namespace detail {

// SwitchToPlanet @0x0044ce13. Reads the roster/planet/session inputs through `v`, mutates
// _G_LLM_STRAT_SIM_ACTIVE / game_speed / G_PLANET_INDEX / _G_LLM_STRAT_GAME_CLOCK / planet_time through
// `own`, reaches the frontier + still-struct-less-owned originals through `c`, and reaches the four
// already-translated siblings (diplomacy_restore_relations / session_clear_system_presence_flag /
// ambient_reseed_planet_event_times direct, land_players_on_planet threaded via `c_land`) as described
// in the header banner's CALLEE ROUTING section. Returns the original's incidental int result:
// llm_strat_sim_clock_advance's return, or -1 if map_LoadPlanetFromDisk fails.
int32_t switch_to_planet(
    const sim_view &v, sim_store &own, const switch_to_planet_calls &c, int32_t planet_index,
    const land_players_on_planet_calls &c_land = live_land_players_on_planet_calls(),
    // TRANSITIVE THREADING -- translator-brief rule 3c, "for the WHOLE closure below you, not just
    // your direct callee". Added 2026-09-10 after this unit's offline oracle could not test the
    // GENERATE arm AT ALL: threading only `c_land` left land_players_on_planet's own 5th parameter
    // taking its `live_planet_map_session_init_calls()` default, and that table's members are
    // MH_LIBMH_BIND rows. Under net_selftest.exe `load_gates(nullptr, 0)` makes every such row read
    // `armed() == false`, so the macro expands to `&::mh::call::<fn>` -- a naked pointer at the
    // original game's absolute VA, which the test process does not map. Calling it FAULTS, and a
    // fault in one oracle takes the whole simtest binary down rather than failing one case. Same
    // reasoning for diplomacy_restore_relations' own defaulted table one level down.
    const planet_map_session_init_calls &c_pmsi         = live_planet_map_session_init_calls(),
    const diplomacy_set_relation_calls  &c_set_relation = live_diplomacy_set_relation_calls());

} // namespace detail

// Live wrapper: the logic applied to state() and live_switch_to_planet_calls(). Parameter/return types
// match the committed promotion seam exactly (sig_SwitchToPlanet in addr/mh_export.gen.h):
// int32_t(__cdecl*)(int32_t planet_index), EAX-passed parameter, int return -- this is the promotion
// seam itself; do not change it.
int32_t switch_to_planet(int32_t planet_index);

} // namespace mh::sim
