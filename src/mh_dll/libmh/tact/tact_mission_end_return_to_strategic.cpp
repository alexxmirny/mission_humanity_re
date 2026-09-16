//
// tact/tact_mission_end_return_to_strategic.cpp -- see tact_mission_end_return_to_strategic.h.
// Translated from the DISASSEMBLY, not from Ghidra's C.
//
#include "tact/tact_mission_end_return_to_strategic.h"

#include "addr/mh_calls.gen.h" // frontier callees + the in-manifest cross-TU siblings
                               // llm_strat_squad_assault_resolve / llm_tact_mission_end_finalize_stub
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const mission_end_return_to_strategic_calls &live_mission_end_return_to_strategic_calls() {
    static const mission_end_return_to_strategic_calls c = {
        mh::tact_host().llm_gfx_apply_resolution_change,
        mh::state::evt::inv_all_sprite_banks,
        mh::state::evt::inv_tact_view_tile_rows,
        mh::tact_host().llm_view_set_size_mode,
        MH_LIBMH_BIND(map_LoadPlanetFromDisk),
        MH_LIBMH_BIND(llm_strat_squad_assault_resolve),
        MH_LIBMH_BIND(llm_strat_time_resync_and_tick),
        MH_LIBMH_BIND(llm_snd_ambient_reseed_planet_event_times),
        MH_LIBMH_BIND(llm_tact_mission_end_finalize_stub),
    };
    return c;
}

namespace detail {

void mission_end_return_to_strategic(const tact_view &tv, tact_store &own,
                                     const mission_end_return_to_strategic_calls &c) {
    // @0x0044d3c0-0x0044d3e4
    c.gfx_apply_resolution_change();
    c.gfx_load_all_sprite_banks();
    if (*tv.view_size_mode == 0) {
        c.tact_gfx_view_tile_rows_init();
    } else {
        c.view_set_size_mode(*tv.view_size_mode);
    }

    // @0x0044d3e4: the canonical cross-mode flag, an accepted cross-write per region_ownership.json.
    own.game_mode() = 2;

    // @0x0044d3eb-0x0044d40e: UNKNOWN(0) -> VISITED(1) latch, E_PLANET_STATUS.
    const int32_t planet = *tv.planet_index;
    if (own.planet_status_at(planet) == 0) {
        own.planet_status_at(planet) = 1;
    }

    // @0x0044d40e-0x0044d42a
    const uint32_t rc = c.map_load_planet_from_disk(planet, 1);
    if (rc == 0) return; // load failed -- the local success flag stays 0, discarded by every caller

    // @0x0044d42a-0x0044d454: only on a successful load.
    c.squad_assault_resolve();
    c.time_resync_and_tick();
    // Read AFTER time_resync_and_tick, matching the original's instruction order exactly --
    // time_resync_and_tick's own body unconditionally overwrites LAST_GAME_TIME first.
    c.snd_ambient_reseed_planet_event_times(planet, *tv.last_game_time);
    c.mission_end_finalize_stub();
}

} // namespace detail

void mission_end_return_to_strategic() {
    tact_state st = state();
    detail::mission_end_return_to_strategic(st.read, st.own,
                                            live_mission_end_return_to_strategic_calls());
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
