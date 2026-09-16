//
// tact/tact_mission_end_return_to_strategic.h -- TACT1A: the tactical excursion's EXIT, called from
// llm_tact_frame when the mission signals completion. Reverses llm_tact_mission_start: restores the
// view/resolution, flips GAME_MODE back to strategic, latches the planet VISITED, and reloads the
// planet's strategic state from disk (undoing the 128x128 wipe llm_tact_map_reset performed on
// entry -- see the region_ownership.json G_PLANET_STATUS/tile_objects/passable "IN-PROCESS this is
// a shared write" notes).
//
//   llm_tact_mission_end_return_to_strategic @0x0044d3a8 (0xc0)
//
// STRUCTURALLY UN-SHADOW-ARMABLE, not merely deferred: one of its nine direct outward calls,
// llm_snd_ambient_reseed_planet_event_times, is itself one of TACT-CUT2's ungated `effectful` shared
// callees (tools/data/tact_shared_callees.json: a 20-iteration loop of llm_rand_below_fx draws that
// ADVANCE the strategic PRNG, no entry gate installed anywhere). Arming this entry under shadow
// would double-fire that draw sequence -- 20 extra PRNG advances plus 20 different
// events[i].next_time overwrites -- regardless of what the other eight calls do. So ALL NINE
// outward calls are indirected via a `_calls` struct (this codebase's sim/ pattern; tact precedent
// tact_squad_assault_resolve.h) and the dispatch logic is proven OFFLINE, which never executes any
// of the nine real callees.
//
// COMMITTED PROTOTYPE IS void(void) despite the disassembly computing and returning a local success
// flag in EAX at 0x0044d45b/0x0044d45e (1 = map load + finalize succeeded, 0 = map load failed) --
// every reference in mh_calls.gen.h and the .asm header agrees the return is discarded by every
// caller. The flag decides nothing about what THIS function does (both arms already performed every
// write the flag would gate), so a void translation is behaviourally complete; the two-outcome shape
// is captured below as the two OFFLINE test cases and the flag's value itself is not tracked.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The nine outward calls this function makes, indirected for offline testability.
struct mission_end_return_to_strategic_calls {
    void (*gfx_apply_resolution_change)();            // llm_gfx_apply_resolution_change @0x00429371
    void (*gfx_load_all_sprite_banks)();              // llm_gfx_load_all_sprite_banks @0x00465093
    void (*tact_gfx_view_tile_rows_init)();           // llm_tact_gfx_view_tile_rows_init @0x004294e3
    int32_t (*view_set_size_mode)(int32_t size_mode); // llm_view_set_size_mode @0x0044e47d
    uint32_t (*map_load_planet_from_disk)(int32_t  planet,
                                          uint32_t param_2); // map_LoadPlanetFromDisk @0x00448107
    void (*squad_assault_resolve)();                         // llm_strat_squad_assault_resolve @0x0044d81d
    void (*time_resync_and_tick)();                          // llm_strat_time_resync_and_tick @0x00449e21
    void (*snd_ambient_reseed_planet_event_times)(int32_t planet_index,
                                                  double  current_time); // @0x0045e277
    void (*mission_end_finalize_stub)();                                // llm_tact_mission_end_finalize_stub @0x0044da59
};

const mission_end_return_to_strategic_calls &live_mission_end_return_to_strategic_calls();

namespace detail {

// llm_tact_mission_end_return_to_strategic @0x0044d3a8.
//
// 1. @0x0044d3c0-0x0044d3e4: gfx_apply_resolution_change(); gfx_load_all_sprite_banks(); then if
//    VIEW_SIZE_MODE == 0, tact_gfx_view_tile_rows_init() -- else view_set_size_mode(VIEW_SIZE_MODE).
// 2. @0x0044d3e4: GAME_MODE = 2 (strategic) -- the canonical cross-mode flag; region_ownership.json
//    names this function an accepted cross-writer by design.
// 3. @0x0044d3eb-0x0044d40e: if planet_status_at(planet_index) == E_PLANET_STATUS::UNKNOWN (0),
//    latch it to VISITED (1) -- Ghidra enum /Manual/game/E_PLANET_STATUS (UNKNOWN=0, VISITED=1,
//    CONQUERED=2, INVASION=4). region_ownership.json's producer-consumer note: SwitchToPlanet
//    consumes this latch on the planet's next visit.
// 4. @0x0044d40e-0x0044d42a: rc = map_load_planet_from_disk(planet_index, 1). rc == 0 (failure) ->
//    skip step 5 entirely (the local success flag stays 0, discarded by every caller regardless).
// 5. @0x0044d42a-0x0044d454 (only when step 4 succeeded): squad_assault_resolve();
//    time_resync_and_tick(); THEN snd_ambient_reseed_planet_event_times(planet_index,
//    LAST_GAME_TIME) -- LAST_GAME_TIME is read AFTER time_resync_and_tick, at
//    0x0044d434/0x0044d43a, and time_resync_and_tick's own body unconditionally OVERWRITES
//    LAST_GAME_TIME with a fresh time_GetCurrentTime() read (tact_shared_callees.json's own
//    derivation of that function) -- so the value passed on is the POST-resync stamp, not the
//    pre-call one. Reproduced here by reading tv.last_game_time only after calling
//    c.time_resync_and_tick(), matching the instruction order exactly (reading it before would
//    silently pass the previous frame's stamp instead). mission_end_finalize_stub() is committed
//    void(void) -- the caller's own EAX=DAT_00500f3c load is inert, see that function's own header:
//    a proven no-op that discards whatever it is handed.
void mission_end_return_to_strategic(const tact_view &tv, tact_store &own,
                                     const mission_end_return_to_strategic_calls &c);

} // namespace detail

void mission_end_return_to_strategic();


} // namespace mh::tact
