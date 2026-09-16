//
// tact_mission_end_return_to_strategic_selftest.cpp -- offline oracle for
// llm_tact_mission_end_return_to_strategic (TACT1A, 2026-08-26). See
// tact/tact_mission_end_return_to_strategic.h for the derivation.
//
// WHY OFFLINE, NOT RIG: one of the nine direct outward calls,
// llm_snd_ambient_reseed_planet_event_times, is one of TACT-CUT2's ungated `effectful` shared
// callees (a real PRNG advance, tools/data/tact_shared_callees.json) -- arming this entry under
// shadow would double-fire it. All nine calls are mocked via the header's
// `mission_end_return_to_strategic_calls` struct, so this oracle never executes any real callee.
//
#include "tact/tact_mission_end_return_to_strategic.h"
#include "sim_test_support.h"
#include "tact_test_support.h"

namespace mh::tact::test {

namespace {

struct call_log {
    int      resolution_change_calls = 0;
    int      load_sprite_banks_calls = 0;
    int      view_tile_rows_calls    = 0;
    int      set_size_mode_calls     = 0;
    int32_t  set_size_mode_arg       = -1;
    int      load_planet_calls       = 0;
    int32_t  load_planet_planet_arg  = -1;
    uint32_t load_planet_param2_arg  = 0;
    uint32_t load_planet_rc          = 1; // what the mock returns
    int      squad_assault_calls     = 0;
    int      time_resync_calls       = 0;
    int      reseed_calls            = 0;
    int32_t  reseed_planet_arg       = -1;
    double   reseed_time_arg         = -1.0;
    int      finalize_calls          = 0;
    // Sequence stamps, so ORDER (not just "was it called") is checkable.
    int seq               = 0;
    int squad_assault_seq = -1;
    int time_resync_seq   = -1;
    int reseed_seq        = -1;
    int finalize_seq      = -1;
    // Simulates time_resync_and_tick's real body unconditionally overwriting LAST_GAME_TIME: when
    // set, mock_time_resync_and_tick writes a new value through this pointer, so a translation that
    // reads tv.last_game_time BEFORE calling time_resync_and_tick (wrong order) observes the STALE
    // seeded value while a correct read-after-call observes the mutation.
    double *last_game_time_ptr          = nullptr;
    double  last_game_time_after_resync = 222.0;
};

call_log &log() {
    static call_log l;
    return l;
}

void reset_log() { log() = call_log{}; }

void    mock_resolution_change() { ++log().resolution_change_calls; }
void    mock_load_sprite_banks() { ++log().load_sprite_banks_calls; }
void    mock_view_tile_rows_init() { ++log().view_tile_rows_calls; }
int32_t mock_set_size_mode(int32_t size_mode) {
    ++log().set_size_mode_calls;
    log().set_size_mode_arg = size_mode;
    return 0;
}
uint32_t mock_load_planet(int32_t planet, uint32_t param2) {
    ++log().load_planet_calls;
    log().load_planet_planet_arg = planet;
    log().load_planet_param2_arg = param2;
    return log().load_planet_rc;
}
void mock_squad_assault_resolve() {
    ++log().squad_assault_calls;
    log().squad_assault_seq = ++log().seq;
}
void mock_time_resync_and_tick() {
    ++log().time_resync_calls;
    log().time_resync_seq = ++log().seq;
    if (log().last_game_time_ptr != nullptr) *log().last_game_time_ptr = log().last_game_time_after_resync;
}
void mock_reseed(int32_t planet, double t) {
    ++log().reseed_calls;
    log().reseed_planet_arg = planet;
    log().reseed_time_arg   = t;
    log().reseed_seq        = ++log().seq;
}
void mock_finalize() {
    ++log().finalize_calls;
    log().finalize_seq = ++log().seq;
}

mission_end_return_to_strategic_calls mock_calls() {
    return {mock_resolution_change, mock_load_sprite_banks, mock_view_tile_rows_init,
            mock_set_size_mode, mock_load_planet, mock_squad_assault_resolve,
            mock_time_resync_and_tick, mock_reseed, mock_finalize};
}

} // namespace

void run_mission_end_return_to_strategic_tests() {
    // T1: view_size_mode == 0 -> tact_gfx_view_tile_rows_init(), NOT view_set_size_mode.
    {
        tact_fixture tfx;
        reset_log();
        tfx.view_size_mode = 0;
        tact_view  v       = tfx.view();
        tact_store own     = tfx.store();
        auto       c       = mock_calls();
        detail::mission_end_return_to_strategic(v, own, c);
        ck_eq((uint32_t)log().view_tile_rows_calls, 1u, "T1: view_size_mode==0 -> tile_rows_init, 0x0044d3d3");
        ck_eq((uint32_t)log().set_size_mode_calls, 0u, "T1: view_size_mode==0 -> NOT view_set_size_mode");
    }

    // T2: view_size_mode != 0 -> view_set_size_mode(mode), NOT tile_rows_init. Also checks the
    // argument passed is the mode itself, not a boolean/derived value.
    {
        tact_fixture tfx;
        reset_log();
        tfx.view_size_mode = 2;
        tact_view  v       = tfx.view();
        tact_store own     = tfx.store();
        auto       c       = mock_calls();
        detail::mission_end_return_to_strategic(v, own, c);
        ck_eq((uint32_t)log().view_tile_rows_calls, 0u, "T2: view_size_mode!=0 -> NOT tile_rows_init");
        ck_eq((uint32_t)log().set_size_mode_calls, 1u, "T2: view_size_mode!=0 -> view_set_size_mode, 0x0044d3df");
        ck_eq((uint32_t)log().set_size_mode_arg, 2u, "T2: view_set_size_mode(VIEW_SIZE_MODE) verbatim");
    }

    // T3: GAME_MODE is ALWAYS set to 2 (strategic), regardless of the branches below.
    {
        tact_fixture tfx;
        reset_log();
        tfx.game_mode        = 6; // tactical, the live value during an excursion
        tfx.planet_status[0] = 5; // arbitrary -- unrelated to this check
        log().load_planet_rc = 0; // even on a load FAILURE
        tact_view  v         = tfx.view();
        tact_store own       = tfx.store();
        auto       c         = mock_calls();
        detail::mission_end_return_to_strategic(v, own, c);
        ck_eq((uint32_t)tfx.game_mode, 2u, "T3: GAME_MODE = 2 unconditionally, 0x0044d3e4");
    }

    // T4: planet_status UNKNOWN(0) -> VISITED(1) latch.
    {
        tact_fixture tfx;
        reset_log();
        tfx.planet_index     = 7;
        tfx.planet_status[7] = 0; // E_PLANET_STATUS::UNKNOWN
        tact_view  v         = tfx.view();
        tact_store own       = tfx.store();
        auto       c         = mock_calls();
        detail::mission_end_return_to_strategic(v, own, c);
        ck_eq((uint32_t)tfx.planet_status[7], 1u,
              "T4: E_PLANET_STATUS UNKNOWN(0) -> VISITED(1), 0x0044d3fc-0x0044d404");
    }

    // T5: planet_status already non-zero (CONQUERED=2) -- the latch must NOT stomp it back to 1.
    {
        tact_fixture tfx;
        reset_log();
        tfx.planet_index     = 3;
        tfx.planet_status[3] = 2; // E_PLANET_STATUS::CONQUERED
        tact_view  v         = tfx.view();
        tact_store own       = tfx.store();
        auto       c         = mock_calls();
        detail::mission_end_return_to_strategic(v, own, c);
        ck_eq((uint32_t)tfx.planet_status[3], 2u,
              "T5: non-zero planet_status is left untouched, JNZ @0x0044d3fa");
    }

    // T6: map load FAILS (rc==0) -- the four post-load calls must NOT fire.
    {
        tact_fixture tfx;
        reset_log();
        log().load_planet_rc = 0;
        tact_view  v         = tfx.view();
        tact_store own       = tfx.store();
        auto       c         = mock_calls();
        detail::mission_end_return_to_strategic(v, own, c);
        ck_eq((uint32_t)log().load_planet_calls, 1u, "T6: map_load_planet_from_disk always attempted");
        ck_eq((uint32_t)log().squad_assault_calls, 0u, "T6: load failed -- NOT squad_assault_resolve");
        ck_eq((uint32_t)log().time_resync_calls, 0u, "T6: load failed -- NOT time_resync_and_tick");
        ck_eq((uint32_t)log().reseed_calls, 0u, "T6: load failed -- NOT snd_ambient_reseed");
        ck_eq((uint32_t)log().finalize_calls, 0u, "T6: load failed -- NOT mission_end_finalize_stub");
    }

    // T7: map load SUCCEEDS (rc!=0) -- all four post-load calls fire, planet index passed through
    // to BOTH map_load_planet_from_disk and snd_ambient_reseed identically, param_2 is the literal 1.
    {
        tact_fixture tfx;
        reset_log();
        tfx.planet_index     = 4;
        log().load_planet_rc = 1;
        tact_view  v         = tfx.view();
        tact_store own       = tfx.store();
        auto       c         = mock_calls();
        detail::mission_end_return_to_strategic(v, own, c);
        ck_eq((uint32_t)log().load_planet_param2_arg, 1u, "T7: map_LoadPlanetFromDisk's param_2 is the literal 1, 0x0044d40e");
        ck_eq((uint32_t)log().load_planet_planet_arg, 4u, "T7: G_PLANET_INDEX passed to map_load");
        ck_eq((uint32_t)log().squad_assault_calls, 1u, "T7: load succeeded -- squad_assault_resolve fires");
        ck_eq((uint32_t)log().time_resync_calls, 1u, "T7: load succeeded -- time_resync_and_tick fires");
        ck_eq((uint32_t)log().reseed_calls, 1u, "T7: load succeeded -- snd_ambient_reseed fires");
        ck_eq((uint32_t)log().finalize_calls, 1u, "T7: load succeeded -- mission_end_finalize_stub fires");
        ck_eq((uint32_t)log().reseed_planet_arg, 4u, "T7: reseed's planet arg == G_PLANET_INDEX");
    }

    // T8: ORDER -- squad_assault_resolve, THEN time_resync_and_tick, THEN reseed, THEN finalize.
    // Load-bearing for the 2026-08-26 correction: the original reads LAST_GAME_TIME AFTER calling
    // time_resync_and_tick (0x0044d42f then 0x0044d434/0x0044d43a), and that function's own body
    // unconditionally overwrites LAST_GAME_TIME first -- so reseed must observe the POST-resync
    // value, not whatever was in the view before this function ran at all.
    {
        tact_fixture tfx;
        reset_log();
        tfx.last_game_time       = 111.0; // the STALE, pre-call value -- must NOT reach reseed
        log().last_game_time_ptr = &tfx.last_game_time;
        log().load_planet_rc     = 1;
        tact_view  v             = tfx.view();
        tact_store own           = tfx.store();
        auto       c             = mock_calls();
        detail::mission_end_return_to_strategic(v, own, c);
        ck((log().squad_assault_seq < log().time_resync_seq) &&
               (log().time_resync_seq < log().reseed_seq) && (log().reseed_seq < log().finalize_seq),
           "T8: call order squad_assault -> time_resync -> reseed -> finalize, 0x0044d42a-0x0044d454");
        // The mock overwrites LAST_GAME_TIME to 222.0 as its own side effect (simulating
        // time_resync_and_tick's real body). A translation that read tv.last_game_time BEFORE
        // calling c.time_resync_and_tick() would observe the stale 111.0 instead.
        ck_eq_d(log().reseed_time_arg, 222.0,
                "T8: LAST_GAME_TIME read AFTER time_resync_and_tick, not before -- 0x0044d42f then "
                "0x0044d434/0x0044d43a; a pre-call read would see the stale 111.0");
    }
}

} // namespace mh::tact::test
