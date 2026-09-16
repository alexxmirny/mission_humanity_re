#include "sim/hostreach/sim_h_switch_to_planet.h"

#include <cstring>
#include <vector>

#include "sim_resid_sibling_mocks.h" // mock_lpop_calls / mock_pmsi_calls / sibling_rec, for the GENERATE arm
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- ambient-reseed byte-offset layout (sim/libtrans/sim_lt_ambient.h's own derivation) -----------
constexpr int32_t AMBIENT_PLANET_STRIDE    = 0x2d4;
constexpr int32_t AMBIENT_EVENT_STRIDE     = 0x24;
constexpr int32_t AMBIENT_REPLAY_DELAY_OFF = 0xc;
constexpr int32_t AMBIENT_RETRY_DELAY_OFF  = 0x14;
constexpr int32_t AMBIENT_NEXT_TIME_OFF    = 0x20;

void ambient_write_f64(sim_store &own, int32_t planet, int32_t event, int32_t field_off, double v) {
    uint8_t *base = own.snd_ambient_by_planet_base() + planet * AMBIENT_PLANET_STRIDE +
                    event * AMBIENT_EVENT_STRIDE + field_off;
    std::memcpy(base, &v, sizeof(double));
}
double ambient_read_f64(sim_store &own, int32_t planet, int32_t event, int32_t field_off) {
    double         v;
    const uint8_t *base = own.snd_ambient_by_planet_base() + planet * AMBIENT_PLANET_STRIDE +
                          event * AMBIENT_EVENT_STRIDE + field_off;
    std::memcpy(&v, base, sizeof(double));
    return v;
}

// ---- recorders --------------------------------------------------------------------------------------

struct save_call {
    uint32_t planet_index;
    uint32_t with_progress;
};
struct load_call {
    int32_t  planet_index;
    uint32_t with_progress;
};
struct read_map_call {
    uint32_t planet_index;
};

std::vector<save_call>     g_save_calls;
std::vector<load_call>     g_load_calls;
std::vector<read_map_call> g_read_map_calls;
int32_t                    g_load_return              = 1;
int32_t                    g_ticks_ms_calls           = 0;
int32_t                    g_time_tick_calls          = 0;
int32_t                    g_sim_tick_calls           = 0;
int32_t                    g_sim_clock_advance_calls  = 0;
int32_t                    g_sim_clock_advance_return = 0;
std::vector<double>        g_game_speed_at_tick; // sampled from g_own_ptr on every sim_tick call

// ORDER PROOF (GENERATE arm, G1 below). `own.outer_planet_landed_flag()` is written INSIDE
// land_players_on_planet, strictly after its own planet_map_session_init call and strictly before its
// per-player loop (sim_land_players_on_planet_selftest.cpp's T1/T6) -- so a snapshot of that flag taken
// by read_map_pre's own mock (BEFORE land_players_on_planet is even called) and another taken by
// diplomacy_ai_relation_swap's own mock (which only fires from INSIDE diplomacy_restore_relations,
// switch_to_planet's LAST GENERATE-arm call) is a real, mechanism-grounded order witness: unset at the
// first snapshot, set at the second, proves read_map_pre < land_players_on_planet <
// diplomacy_restore_relations without needing a bespoke sequence counter.
std::vector<uint32_t> g_read_map_pre_flag_snapshots;

// c_set_relation recorder (diplomacy_restore_relations threads this now) -- see mock_set_relation_calls().
struct swap_call {
    int32_t  player_a, player_b, relation_sign;
    uint32_t flag_snapshot; // own.outer_planet_landed_flag() at the moment of this call, see above
};
std::vector<swap_call> g_swap_calls;

// Set by each case before calling detail::switch_to_planet -- lets the captureless sim_tick lambda
// reach back into THIS test's own fixture/store to drain a queue or sample game_speed (same pattern
// sim_land_players_on_planet_selftest.cpp uses for its order_fx/order_player/... hook).
sim_fixture *g_fx_ptr   = nullptr;
sim_store   *g_own_ptr  = nullptr;
void (*g_on_sim_tick)() = nullptr; // optional extra hook, invoked after the built-in bookkeeping

void reset_recorders() {
    g_save_calls.clear();
    g_load_calls.clear();
    g_read_map_calls.clear();
    g_load_return              = 1;
    g_ticks_ms_calls           = 0;
    g_time_tick_calls          = 0;
    g_sim_tick_calls           = 0;
    g_sim_clock_advance_calls  = 0;
    g_sim_clock_advance_return = 0;
    g_game_speed_at_tick.clear();
    g_read_map_pre_flag_snapshots.clear();
    g_swap_calls.clear();
    g_fx_ptr      = nullptr;
    g_own_ptr     = nullptr;
    g_on_sim_tick = nullptr;
    sibling_rec() = sibling_record{}; // mock_lpop_calls/mock_pmsi_calls' shared counters
}

// c_set_relation mock -- threaded into diplomacy_restore_relations since the 2026-09-10 fix. Every
// member records and returns a safe value; none reaches a `mh::call::` thunk. The one this file cares
// about is diplomacy_ai_relation_swap, the edge diplomacy_set_relation.cpp calls with the DERIVED
// relation SIGN (not the raw stored relation byte) -- see sim_diplomacy_set_relation.cpp.
const diplomacy_set_relation_calls &mock_set_relation_calls() {
    static const diplomacy_set_relation_calls c = {
        [](char *) -> void * { return nullptr; }, // ansi_to_wide_scratch -- result only forwarded to w_sprintf below, never dereferenced here
        [](void *, const wchar_t *, int32_t, const wchar_t *, int32_t, const wchar_t *) -> int32_t {
            return 0; // w_sprintf_visis
        },
        [](void *, const wchar_t *, const wchar_t *) -> int32_t { return 0; }, // w_sprintf_vs
        [](int32_t player_a, int32_t player_b, int32_t relation_sign) -> int32_t {
            const uint32_t flag = (g_own_ptr != nullptr) ? (uint32_t)g_own_ptr->outer_planet_landed_flag() : 0u;
            g_swap_calls.push_back({player_a, player_b, relation_sign, flag});
            return 0;
        },
        []() {}, // net_chat_ally_mask_rebuild
    };
    return c;
}

const switch_to_planet_calls &recording_calls() {
    static const switch_to_planet_calls c = {
        []() -> uint32_t {
            ++g_ticks_ms_calls;
            return 0;
        },
        []() -> int32_t {
            ++g_time_tick_calls;
            return 0;
        },
        []() {
            ++g_sim_tick_calls;
            if (g_own_ptr != nullptr) g_game_speed_at_tick.push_back(g_own_ptr->game_speed());
            if (g_on_sim_tick != nullptr) g_on_sim_tick();
        },
        [](uint32_t planet_index, uint32_t with_progress) -> uint32_t {
            g_save_calls.push_back({planet_index, with_progress});
            return 1;
        },
        [](int32_t planet_index, uint32_t with_progress) -> uint32_t {
            g_load_calls.push_back({planet_index, with_progress});
            return static_cast<uint32_t>(g_load_return);
        },
        [](uint32_t planet_index) {
            g_read_map_calls.push_back({planet_index});
            // ORDER PROOF, see g_read_map_pre_flag_snapshots' comment: land_players_on_planet has not
            // run yet at this point, so its outer_planet_landed_flag latch must still read unset.
            if (g_own_ptr != nullptr)
                g_read_map_pre_flag_snapshots.push_back((uint32_t)g_own_ptr->outer_planet_landed_flag());
        },
        []() -> int32_t {
            ++g_sim_clock_advance_calls;
            return g_sim_clock_advance_return;
        },
    };
    return c;
}

} // namespace

void run_h_switch_to_planet_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- LOAD arm, queue EMPTY (loop never runs). Full happy path: sim_active cleared, game_speed
    // untouched, save fires with the OUTGOING planet BEFORE reassignment, G_PLANET_INDEX becomes the
    // target, load fires with the TARGET (not the outgoing planet), ambient reseed fires (bounded RNG
    // check), and the return value IS sim_clock_advance's return.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr int32_t OLD_PLANET    = 2; // outgoing -- distinct from TARGET so a swap disagrees
        constexpr int32_t TARGET_PLANET = 5;
        constexpr int16_t PLAYER_SIDE   = 1;
        fx.planet_index                 = OLD_PLANET; // G_PLANET_INDEX before the call
        fx.player_side                  = PLAYER_SIDE;
        fx.sim_active                   = 1;    // nonzero sentinel -- must become 0
        fx.game_speed                   = 4.25; // must be BIT-IDENTICAL after -- loop never runs
        fx.planet_status[TARGET_PLANET] = 3;    // != UNKNOWN(0) -> LOAD arm
        fx.last_game_time               = 1000.0;
        // profiles[PLAYER_SIDE].prod_queue_slot[OLD_PLANET] stays the fixture default 0 -> loop skipped.

        sim_store own = fx.store();
        sim_view  v   = fx.view();
        ambient_write_f64(own, TARGET_PLANET, 0, AMBIENT_REPLAY_DELAY_OFF, 5.0);
        ambient_write_f64(own, TARGET_PLANET, 0, AMBIENT_RETRY_DELAY_OFF, 3.0);  // range = trunc(8.0) = 8
        ambient_write_f64(own, TARGET_PLANET, 0, AMBIENT_NEXT_TIME_OFF, -999.0); // pre-call sentinel

        g_load_return              = 1; // load succeeds
        g_sim_clock_advance_return = 777;

        const int32_t ret = detail::switch_to_planet(v, own, recording_calls(), TARGET_PLANET);

        ck_eq((uint32_t)own.sim_active_mut(), 0u, "T1: sim_active cleared unconditionally, 0x0044ce35");
        ck_eq((uint32_t)g_ticks_ms_calls, 0u, "T1: queue empty -> fast-forward loop never entered, ticks_ms uncalled, 0x0044ce5e");
        ck_eq((uint32_t)g_time_tick_calls, 0u, "T1: ... time_tick uncalled either, 0x0044ce5e");
        ck_eq((uint32_t)g_sim_tick_calls, 0u, "T1: ... sim_tick uncalled either, 0x0044ce5e");
        ck_eq_d(fx.game_speed, 4.25, "T1: game_speed BIT-IDENTICAL -- never saved/forced/restored on the zero-queue arm");

        ck_eq((uint32_t)g_save_calls.size(), 1u, "T1: map_SavePlanetToDisk fires exactly once, unconditionally, 0x0044cece");
        ck_eq(g_save_calls[0].planet_index, (uint32_t)OLD_PLANET,
              "T1 ORDER PIN: save receives the OUTGOING planet index -- it was captured BEFORE "
              "G_PLANET_INDEX is reassigned to TARGET_PLANET at 0x0044cf43-0x0044cf46 (OLD != TARGET here, "
              "so a translation that reordered the write ahead of the save would read TARGET instead)");
        ck_eq(g_save_calls[0].with_progress, 1u, "T1: save's second argument is the literal 1, 0x0044cec4");

        ck_eq((uint32_t)own.planet_index_mut(), (uint32_t)TARGET_PLANET,
              "T1: G_PLANET_INDEX = the function's planet_index PARAMETER, before either arm, 0x0044cf43-0x0044cf46");

        ck_eq((uint32_t)g_load_calls.size(), 1u, "T1: LOAD arm -- map_LoadPlanetFromDisk fires once, 0x0044cf66");
        ck_eq(g_load_calls[0].planet_index, (uint32_t)TARGET_PLANET,
              "T1: load's planet arg is the TARGET (post-reassignment *v.planet_index), not the outgoing "
              "planet -- proves the arm reads G_PLANET_INDEX AFTER it was reassigned, 0x0044cf61");
        ck_eq(g_load_calls[0].with_progress, 1u, "T1: load's second argument is the literal 1, 0x0044cf5c");
        ck_eq((uint32_t)g_read_map_calls.size(), 0u,
              "T1: the LOAD arm never calls map_ReadMap_pre -- that is GENERATE-arm-only, 0x0044cf5a taken");

        const double next_time = ambient_read_f64(own, TARGET_PLANET, 0, AMBIENT_NEXT_TIME_OFF);
        // rng_next's draw is INCLUSIVE of its `hi` bound (sim/sim_rng_next.cpp: the max rotated value
        // 0xffff divides back to exactly `span`), so the closed interval is [1000,1008], not a half-open
        // one -- an exclusive upper bound here would be a false negative on the (rare but real) draw of 8.
        ck(next_time != -999.0 && next_time >= 1000.0 && next_time <= 1008.0,
           "T1: ambient reseed fired UNCONDITIONALLY after the arm -- event0's next_time moved off its "
           "pre-call sentinel into [current_time, current_time+trunc(replay+retry)] = [1000,1008], "
           "0x0044d09c-0x0044d0a6 (via the in-tree rand_below_fx sibling, not a mockable member)");

        ck_eq((uint32_t)g_sim_clock_advance_calls, 1u, "T1: llm_strat_sim_clock_advance fires exactly once, 0x0044d0d8");
        ck_eq((uint32_t)ret, 777u, "T1: the function's return IS sim_clock_advance's return, 0x0044d0dd-0x0044d0e0");
    }

    // =================================================================================================
    // T2 -- LOAD arm, load FAILS: returns -1 immediately and calls NOTHING afterward (no ambient
    // reseed, no sim_clock_advance) -- but the save (before the arm) and the reassignment (before the
    // arm check) still both happened, since they are unconditional and precede the failure point.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr int32_t OLD_PLANET    = 3;
        constexpr int32_t TARGET_PLANET = 6;
        constexpr int16_t PLAYER_SIDE   = 2;
        fx.planet_index                 = OLD_PLANET;
        fx.player_side                  = PLAYER_SIDE;
        fx.sim_active                   = 1;
        fx.planet_status[TARGET_PLANET] = 1; // != UNKNOWN -> LOAD arm
        // profiles[PLAYER_SIDE].prod_queue_slot[OLD_PLANET] stays 0 -> loop skipped (isolates this case).

        sim_store own = fx.store();
        sim_view  v   = fx.view();
        ambient_write_f64(own, TARGET_PLANET, 0, AMBIENT_NEXT_TIME_OFF, -999.0); // must survive untouched

        g_load_return              = 0; // load fails
        g_sim_clock_advance_return = 555;

        const int32_t ret = detail::switch_to_planet(v, own, recording_calls(), TARGET_PLANET);

        ck_eq((uint32_t)own.sim_active_mut(), 0u, "T2: sim_active cleared unconditionally even on the failure path, 0x0044ce35");
        ck_eq((uint32_t)ret, 0xffffffffu, "T2: load fails -> return -1 immediately, 0x0044cf6f-0x0044cf76");

        ck_eq((uint32_t)g_save_calls.size(), 1u,
              "T2: map_SavePlanetToDisk STILL fires -- it precedes the arm dispatch entirely, unaffected by the later failure, 0x0044cece");
        ck_eq(g_save_calls[0].planet_index, (uint32_t)OLD_PLANET, "T2: save still receives the OUTGOING planet");

        ck_eq((uint32_t)own.planet_index_mut(), (uint32_t)TARGET_PLANET,
              "T2: G_PLANET_INDEX is STILL reassigned to the target before the failing load -- the "
              "reassignment at 0x0044cf43 precedes the arm check, independent of load's outcome");

        ck_eq((uint32_t)g_load_calls.size(), 1u, "T2: map_LoadPlanetFromDisk was called once (and failed), 0x0044cf66");

        const double next_time = ambient_read_f64(own, TARGET_PLANET, 0, AMBIENT_NEXT_TIME_OFF);
        ck_eq_d(next_time, -999.0,
                "T2: ambient reseed did NOT fire on the failure path -- next_time still the pre-call "
                "sentinel, 0x0044cf6f-0x0044cf76 jumps straight to the return, skipping 0x0044d09c entirely");
        ck_eq((uint32_t)g_sim_clock_advance_calls, 0u,
              "T2: llm_strat_sim_clock_advance did NOT fire either -- a naive oracle gating this only "
              "behind the queue check would pass vacuously here since the mock is simply never reached");
    }

    // =================================================================================================
    // T3 -- LOAD arm, queue NONZERO: the fast-forward loop runs, forces game_speed to 10.0 for its
    // duration, drains after EXACTLY 3 mocked sim_tick calls, and restores the ORIGINAL game_speed.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr int32_t OLD_PLANET                         = 4;
        constexpr int32_t TARGET_PLANET                      = 7;
        constexpr int16_t PLAYER_SIDE                        = 3;
        fx.planet_index                                      = OLD_PLANET;
        fx.player_side                                       = PLAYER_SIDE;
        fx.game_speed                                        = 3.5; // must be restored to exactly this
        fx.profiles[PLAYER_SIDE].prod_queue_slot[OLD_PLANET] = 9;   // nonzero -> loop entered
        fx.planet_status[TARGET_PLANET]                      = 2;   // LOAD arm

        sim_store own = fx.store();
        sim_view  v   = fx.view();
        g_fx_ptr      = &fx;
        g_own_ptr     = &own;
        g_on_sim_tick = []() {
            // Drain the queue on the THIRD call -- the loop must run exactly 3 iterations, not 1 and
            // not forever.
            if (g_sim_tick_calls == 3) g_fx_ptr->profiles[3].prod_queue_slot[4] = 0;
        };
        g_load_return              = 1;
        g_sim_clock_advance_return = 42;

        detail::switch_to_planet(v, own, recording_calls(), TARGET_PLANET);

        ck_eq((uint32_t)g_ticks_ms_calls, 3u, "T3: llm_time_get_ticks_ms called exactly 3 times, one per iteration, 0x0044ce84");
        ck_eq((uint32_t)g_time_tick_calls, 3u, "T3: llm_strat_time_tick called exactly 3 times, 0x0044ce89");
        ck_eq((uint32_t)g_sim_tick_calls, 3u, "T3: llm_strat_sim_tick called exactly 3 times (the one that drains the queue), 0x0044ce8e");
        ck_eq((uint32_t)g_game_speed_at_tick.size(), 3u, "T3: game_speed sampled once per iteration");
        ck(g_game_speed_at_tick.size() == 3 && g_game_speed_at_tick[0] == 10.0 && g_game_speed_at_tick[1] == 10.0 &&
               g_game_speed_at_tick[2] == 10.0,
           "T3: game_speed reads 10.0 (0x40240000_00000000) DURING every iteration of the loop, 0x0044ce70-0x0044ce7a");
        ck_eq_d(fx.game_speed, 3.5, "T3: game_speed restored to the ORIGINAL saved value after the loop exits, 0x0044ceb4-0x0044cebf");
    }

    // =================================================================================================
    // T4 -- LIVE RE-READ PIN (header banner: "THE FAST-FORWARD LOOP RE-READS PlayerSide / G_PLANET_INDEX
    // LIVE, EVERY ITERATION"). Mid-loop, the mocked sim_tick swaps PlayerSide to a DECOY slot whose queue
    // is already empty, while leaving the ORIGINAL player's queue slot nonzero (a trap: a translation
    // that cached PlayerSide at loop entry would keep re-checking the stale slot and loop again). A
    // SAFETY NET on the second call unconditionally drains the original slot too, so even a caching bug
    // exits by iteration 2 -- this test cannot hang regardless of which behaviour the body has.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr int32_t OLD_PLANET                          = 8; // G_PLANET_INDEX -- untouched by this case's mutation
        constexpr int32_t TARGET_PLANET                       = 10;
        constexpr int16_t PLAYER_SIDE                         = 4;
        constexpr int16_t DECOY_PLAYER                        = 6;
        fx.planet_index                                       = OLD_PLANET;
        fx.player_side                                        = PLAYER_SIDE;
        fx.game_speed                                         = 9.125;
        fx.profiles[PLAYER_SIDE].prod_queue_slot[OLD_PLANET]  = 7; // nonzero -> loop entered
        fx.profiles[DECOY_PLAYER].prod_queue_slot[OLD_PLANET] = 0; // the decoy's own slot, already empty
        fx.planet_status[TARGET_PLANET]                       = 5; // LOAD arm afterward

        sim_store own = fx.store();
        sim_view  v   = fx.view();
        g_fx_ptr      = &fx;
        g_own_ptr     = &own;
        g_on_sim_tick = []() {
            if (g_sim_tick_calls == 1) {
                // Swap PlayerSide to the decoy (whose slot is already 0) -- the ORIGINAL player's slot
                // is left nonzero as the trap.
                g_fx_ptr->player_side = DECOY_PLAYER;
            } else if (g_sim_tick_calls >= 2) {
                // Safety net: unconditionally drain the original slot too, bounding this test at 2
                // iterations even if the mutation being pinned for makes it here.
                g_fx_ptr->profiles[PLAYER_SIDE].prod_queue_slot[OLD_PLANET] = 0;
            }
        };
        g_load_return = 1;

        detail::switch_to_planet(v, own, recording_calls(), TARGET_PLANET);

        ck_eq((uint32_t)g_sim_tick_calls, 1u,
              "T4 LIVE-REREAD PIN: the loop condition re-reads PlayerSide fresh from `v` -- after the "
              "first sim_tick call swaps PlayerSide to the (already-empty-queued) decoy, the loop exits "
              "immediately; a cached PlayerSide would instead keep re-checking the original (still "
              "nonzero) slot and only exit at the safety net's 2nd call, 0x0044ce93-0x0044ceb2");
        ck_eq_d(fx.game_speed, 9.125, "T4: game_speed still correctly restored regardless of the mid-loop PlayerSide swap");
        ck_eq((uint32_t)g_save_calls.size(), 1u, "T4: save still fires exactly once after the loop");
        ck_eq(g_save_calls[0].planet_index, (uint32_t)OLD_PLANET,
              "T4: save still reads G_PLANET_INDEX (untouched by this case's PlayerSide-only mutation) as the outgoing planet");
    }

    // =================================================================================================
    // T5 -- LOAD arm, the NEGATIVE side: with REAL recording mocks threaded in for c_land/c_pmsi/
    // c_set_relation (not the defaults), prove none of them fire on the LOAD arm -- the GENERATE-only
    // calls stay GENERATE-only. (T1-T4 already proved this indirectly by not faulting on the unmocked
    // defaults; this makes it an explicit, mutation-visible assertion instead.)
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr int32_t OLD_PLANET    = 9;
        constexpr int32_t TARGET_PLANET = 11;
        fx.planet_index                 = OLD_PLANET;
        fx.planet_status[TARGET_PLANET] = 4; // != UNKNOWN -> LOAD arm

        sim_store own = fx.store();
        sim_view  v   = fx.view();
        g_own_ptr     = &own;
        g_load_return = 1;

        detail::switch_to_planet(v, own, recording_calls(), TARGET_PLANET, mock_lpop_calls(),
                                 mock_pmsi_calls(), mock_set_relation_calls());

        ck_eq((uint32_t)g_read_map_calls.size(), 0u, "T5: LOAD arm -- map_ReadMap_pre never fires, 0x0044cf5a taken");
        ck_eq((uint32_t)sibling_rec().pmsi_bldg_recompute_cell_grid, 0u,
              "T5: LOAD arm -- planet_map_session_init (land_players' own first call) never runs, since "
              "land_players_on_planet itself is never reached");
        ck_eq((uint32_t)sibling_rec().lpop_reroll, 0u,
              "T5: LOAD arm -- land_players_on_planet's unconditional landing_spots_reroll_out_of_bounds never fires");
        ck_eq((uint32_t)sibling_rec().lpop_cam_mark_viewport_dirty, 0u,
              "T5: LOAD arm -- land_players_on_planet's unconditional cam_mark_viewport_dirty never fires either");
        ck_eq((uint32_t)g_swap_calls.size(), 0u,
              "T5: LOAD arm -- diplomacy_restore_relations never runs, so diplomacy_ai_relation_swap is never called");
        ck_eq((uint32_t)g_load_calls.size(), 1u, "T5: the LOAD arm's own call still fires exactly once, 0x0044cf66");
    }

    // =================================================================================================
    // G1 -- GENERATE arm (G_PLANET_STATUS[target]==UNKNOWN): the game-clock clamp FIRES (indexed by the
    // TARGET planet, not a decoy), read_map_pre / land_players_on_planet / session_clear_system_presence_
    // flag / diplomacy_restore_relations all fire, land_players receives the TARGET planet index (not
    // the outgoing one), the relation triple reaches diplomacy_ai_relation_swap, and map_LoadPlanetFromDisk
    // never fires. Safe since 2026-09-10: c_pmsi/c_set_relation are real recording mocks, never the
    // MH_LIBMH_BIND-routed defaults.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr int32_t OLD_PLANET               = 2;  // outside land_players' (6,0x1e) landed-flag latch range
        constexpr int32_t TARGET_PLANET            = 12; // inside it -- the latch proves land_players got THIS index
        constexpr int32_t DECOY_PLANET             = 3;  // a different planet's saved time -- must NOT be read
        fx.planet_index                            = OLD_PLANET;
        fx.planet_status[TARGET_PLANET]            = 0; // UNKNOWN -> GENERATE arm
        fx.game_clock                              = 100.0;
        fx.planet_time[TARGET_PLANET]              = 50.0; // < game_clock -> clamp fires, expect game_clock := 50.0
        fx.planet_time[DECOY_PLANET]               = 5.0;  // if the clamp read the wrong planet, game_clock would land here instead
        fx.current_system                          = 77;
        fx.cfg_planets[TARGET_PLANET].system_index = 77; // matches current_system; every OTHER planet defaults to 0 (skipped)
        // session_clear_system_presence_flag's tracked player (hardcoded index 2): starts ALIVE, no
        // other in-system planet blocks the clear (see above), so the clear is expected to fire.
        fx.profiles[2].status_flags = 0x2; // STATUS_ALIVE (bit1) set, SLOT_ENABLED (bit0) clear
        // diplomacy_restore_relations' relation-triple pin: only player 0 is SLOT_ENABLED, so the ONLY
        // pair processed is (0,0).
        fx.profiles[0].status_flags = 0x1; // SLOT_ENABLED (bit0), HUMAN bit clear -> land_players' AI arm

        sim_store own                = fx.store();
        sim_view  v                  = fx.view();
        own.player_relation_at(0, 0) = 1; // relation==1 -> diplomacy_set_relation's relation_sign := 1
        g_own_ptr                    = &own;
        g_load_return                = 1;

        const int32_t ret = detail::switch_to_planet(v, own, recording_calls(), TARGET_PLANET,
                                                     mock_lpop_calls(), mock_pmsi_calls(), mock_set_relation_calls());

        ck_eq((uint32_t)g_load_calls.size(), 0u, "G1: GENERATE arm -- map_LoadPlanetFromDisk never fires, 0x0044cf5a taken");

        ck_eq_d(own.game_clock_mut(), 50.0,
                "G1 CLAMP PIN: game_clock clamped down to planet_time[TARGET]=50.0 (< the initial 100.0), "
                "indexed by the TARGET planet -- NOT the decoy planet's 5.0, and not left at 100.0, 0x0044cf8c-0x0044cfab");

        ck_eq((uint32_t)g_read_map_calls.size(), 1u, "G1: map_ReadMap_pre fires once, 0x0044cfb6");
        ck_eq(g_read_map_calls[0].planet_index, (uint32_t)TARGET_PLANET, "G1: read_map_pre receives the TARGET planet");

        ck_eq((uint32_t)own.outer_planet_landed_flag(), 0xffffffffu,
              "G1: land_players_on_planet ran and latched outer_planet_landed_flag -- since the latch only "
              "fires for planet_index in (6,0x1e) and OLD_PLANET(2) is outside that range while "
              "TARGET_PLANET(12) is inside it, this proves land_players received the TARGET index, "
              "0x0045534e (via c_land, threaded since the original SIM1-H translation)");
        ck_eq((uint32_t)sibling_rec().pmsi_bldg_recompute_cell_grid, 1u,
              "G1: land_players_on_planet's own first call, planet_map_session_init, ran through the "
              "NEWLY-threaded c_pmsi mock -- this is exactly the edge that used to fault under net_selftest");
        ck_eq((uint32_t)sibling_rec().lpop_reroll, 1u, "G1: land_players_on_planet's unconditional reroll call fired");
        ck_eq((uint32_t)sibling_rec().lpop_cam_mark_viewport_dirty, 1u,
              "G1: land_players_on_planet's unconditional cam_mark_viewport_dirty call fired");

        ck_eq((uint32_t)(own.profile_at(2).status_flags & 0x2u), 0u,
              "G1: session_clear_system_presence_flag ran and cleared STATUS_ALIVE on player 2 -- no other "
              "in-system planet (current_system=77, matched only by TARGET_PLANET) blocked the clear, 0x004987ae");

        ck_eq((uint32_t)g_swap_calls.size(), 1u,
              "G1: diplomacy_restore_relations ran and reached diplomacy_ai_relation_swap exactly once (the "
              "ONLY pair with both slots SLOT_ENABLED is (0,0)), 0x00465c6e -> 0x0049a0b3");
        ck(g_swap_calls.size() == 1 && g_swap_calls[0].player_a == 0 && g_swap_calls[0].player_b == 0 &&
               g_swap_calls[0].relation_sign == 1,
           "G1 RELATION-TRIPLE PIN: the swap receives (player_a=0, player_b=0, relation_sign=1) -- "
           "relation_sign is the DERIVED sign for the stored relation byte 1, not the raw byte itself, "
           "sim_diplomacy_set_relation.cpp's relation==1 arm");

        ck((uint32_t)g_read_map_pre_flag_snapshots.size() == 1 && g_read_map_pre_flag_snapshots[0] == 0u,
           "G1 ORDER PIN (1/2): at the moment read_map_pre fires, outer_planet_landed_flag is still UNSET -- "
           "land_players_on_planet has not run yet, so read_map_pre precedes it, 0x0044cfb6 before 0x0045534e");
        ck(g_swap_calls.size() == 1 && g_swap_calls[0].flag_snapshot == 0xffffffffu,
           "G1 ORDER PIN (2/2): at the moment diplomacy_ai_relation_swap fires, outer_planet_landed_flag is "
           "ALREADY SET -- land_players_on_planet has already completed, so diplomacy_restore_relations "
           "(switch_to_planet's LAST GENERATE-arm call) runs after it, 0x00465c6e after 0x0045534e. Combined "
           "with pin (1/2): read_map_pre < land_players_on_planet < diplomacy_restore_relations. "
           "session_clear_system_presence_flag sits between the latter two in the .cpp's source order "
           "(0x0044cfc0-0x0044cfc5) but has ZERO outward calls of its own (see its header), so it cannot be "
           "given a runtime sequence stamp the way the other three can -- its OWN pin above (the STATUS_ALIVE "
           "clear) is the evidence this oracle can offer for it.");

        ck_eq((uint32_t)ret, 0u, "G1: sim_clock_advance's DEFAULT mock return (0, never overridden here) still threads through as the function's return");
    }

    // =================================================================================================
    // G2 -- GENERATE arm, the game-clock clamp's OTHER direction: planet_time[target] >= game_clock ->
    // the clock is NOT moved. Distinct planet/values from G1 so nothing here coincides with it.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr int32_t OLD_PLANET    = 4;
        constexpr int32_t TARGET_PLANET = 18;
        fx.planet_index                 = OLD_PLANET;
        fx.planet_status[TARGET_PLANET] = 0; // UNKNOWN -> GENERATE arm
        fx.game_clock                   = 100.0;
        fx.planet_time[TARGET_PLANET]   = 200.0; // >= game_clock -> clamp must NOT fire

        sim_store own = fx.store();
        sim_view  v   = fx.view();
        g_own_ptr     = &own;
        g_load_return = 1;

        detail::switch_to_planet(v, own, recording_calls(), TARGET_PLANET, mock_lpop_calls(),
                                 mock_pmsi_calls(), mock_set_relation_calls());

        ck_eq_d(own.game_clock_mut(), 100.0,
                "G2 CLAMP PIN (other direction): planet_time[TARGET]=200.0 >= game_clock=100.0 -> the JBE at "
                "0x0044cf98 skips the clamp entirely, game_clock stays 100.0, 0x0044cf8c-0x0044cfab");
        ck_eq((uint32_t)g_load_calls.size(), 0u, "G2: still the GENERATE arm -- map_LoadPlanetFromDisk never fires");
    }
}

} // namespace mh::sim::test
