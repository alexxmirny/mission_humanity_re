//
// sim_planet_session_begin_selftest.cpp -- `simtest` oracle for llm_strat_planet_session_begin
// @0x004541c3 (sim/resid/sim_planet_session_begin.h/.cpp, RI-SIM sim_resid batch).
//
// NO SHADOW SITE -- session-entry-only writer, proof:OFFLINE (see the header banner). This file is
// the only verification.
//
// *** BLOCKING HAZARD -- READ BEFORE REGISTERING/RUNNING THIS FILE ***
// detail::planet_session_begin makes FIVE direct intra-slice sibling calls (G21: DIRECT `detail::`
// C++ calls, not `mh::call::` seams, so none of the five is mockable from here):
//   session_state_reset (0x004541f7) -> its own sibling new_game_init, full-reset path only
//   land_players_on_planet (0x0045430c) -> its own sibling planet_map_session_init, unconditionally
//   session_clear_system_presence_flag (0x00454311) -- the only one of the five with ZERO outward
//     calls (sim/resid/sim_session_clear_presence_flag.h); safe in isolation.
// The other four each reach FURTHER frontier originals through a `live_..._calls()` binding that is
// hardcoded in THEIR OWN .cpp, not threaded through as a parameter this file could override:
//   * sim_session_state_reset.cpp:61 -- `own.last_game_time() = c.time_get_current_time();` fires
//     UNCONDITIONALLY, before the reset_flag branch, on every call regardless of race/reset_flag.
//     Lines 72-73 unconditionally fire cam_jump_queue_clear()/invasion_alert_reset_all() too.
//   * sim_new_game_init.cpp -- 6 more live originals (game_ClearAvailableProjects,
//     llm_strat_player_profile_init, llm_strat_prod_shuttle_slot_release, llm_strat_
//     invasion_alert_reset_all, utils_fill_data, llm_map_set_zoom_scale), reached whenever
//     reset_flag is 0 or 2 (session_state_reset's full-reset branch).
//   * sim_planet_map_session_init.cpp -- 4 live originals incl. 8x llm_gfx_pack_rgb16, fired
//     UNCONDITIONALLY as the very FIRST thing land_players_on_planet's body does (0x00455369),
//     before land_players_on_planet's own per-player loop even starts.
//   * sim_land_players_on_planet.cpp -- 4 more live originals fired unconditionally at its own tail
//     (landing_spots_reroll_out_of_bounds, coord_msg_sprintf, cam_set_col, cam_set_row,
//     cam_mark_viewport_dirty), plus up to 5 more conditionally per enabled player slot.
// Every `mh::call::*` symbol is a RAW CALL to a fixed GAME virtual address -- mh/addr/mh_calls.gen.cpp's
// naked marshalling thunks do a literal `call dword ptr [ebp+8]` where the dword is that address (e.g.
// 0x00427616 for time_GetCurrentTime, 0x0044af33 for llm_map_cam_set_col). `net_selftest.exe`
// (src/mh_dll/README.md: "console exe ... no game, no DllMain") never maps the game image at those
// addresses, so reaching ANY of them is undefined behaviour in this process (at best a hard crash,
// at worst silently executing whatever unrelated bytes net_selftest.exe's own much smaller image
// happens to have at that VA). CONCRETELY: because session_state_reset's live time_get_current_time
// call is unconditional and sits before this function does anything else observable, EVERY case
// below is expected to make net_selftest.exe fault or misbehave the moment it calls the real
// detail::planet_session_begin, before control can ever return to this file's own ck_eq() lines --
// i.e. no case here can currently be expected to actually report pass or fail. This file is written
// exactly per the task brief's instruction to run the real sibling bodies rather than mock them (G21
// makes them unmockable from this TU); it is NOT weakened to route around the hazard, and the
// hazard itself is the load-bearing finding for whoever registers/builds this file next.
//
// EXPECTED BEHAVIOUR, from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_planet_session_begin_004541c3.asm) -- the .asm is the spec, never
// the .c beside it (the .c has silently lied elsewhere in this project):
//   0x004541e0-0x004541ef: the original makes TWO INDEPENDENT wall-clock draws to feed rng ch0 and
//     ch1 (the second llm_strat_rng_seed_wallclock_seconds call is a fresh draw, not the first value
//     reused). LIBMH NO LONGER DOES: LIFT-TABLE S5 (2026-09-09) made the wall-clock read a pushed
//     INIT PARAMETER (libmh_set_session_seed), because a pulled read is what lets two multiplayer
//     peers seed from two different clocks. One value now feeds both channels -- a declared
//     divergence, and T1 pins the new property (both channels get the pushed value, and a different
//     pushed value moves both) instead of the old one.
//   0x004541f4-0x004541f7: reset_flag forwarded UNCHANGED to session_state_reset.
//   0x004541fc: `CMP dword ptr [EBP-0x20],0x2` compares race to 2 but nothing consumes the flags
//     before they are clobbered -- dead Watcom-codegen leftover, no observable effect, no case here.
//   0x00454203/0x00454208: race stored verbatim; session mode FORCED to SESSION_SP (1) regardless of
//     whatever it held before.
//   0x00454221/0x0045422a: PlayerSide/local-player-slot FORCED to their SP defaults (0 / 1)
//     regardless of whatever they held before.
//   0x00454233-0x0045425d: PLAYER_PROFILE_RACE_SELECTOR -- race==0 or race==1 selects
//     (mother_race, shuttle_race) = (1, 2); every other race value (this file tests 2 and the
//     signed-negative boundary -1) swaps them to (2, 1). A range check (race < 2) would also pass
//     race==-1 by accident; the -1 case pins that the real test is the literal ==0||==1 pair.
//   0x0045425d-0x00454289: player 0's MOTHER (controller_flags 7, color_index 0, side_id -1).
//   0x0045428e-0x004542cf: players 1 and 2 each get a SHUTTLE (controller_flags 0xb, color_index 1,
//     side_id -1); getAsciiVer is called FRESH every loop iteration, not hoisted before the loop.
//   0x004542cf-0x00454301: the SIM_ACTIVE 0->1 bracket -- diplomacy_init_skirmish(), then
//     sim_active=0, THEN reload_snapshot_resync_clocks(0.0)/progress_propagate_unlocks(PlayerSide)/
//     (the original's empty session_begin_empty_stub, not reproduced)/a SECOND
//     ui_message_queue_clear_all(), THEN sim_active=1. The first
//     ui_message_queue_clear_all() (0x00454212) runs BEFORE the bracket even opens.
//   0x00454307-0x00454311: land the roster, then clear system presence (both intra-slice, see hazard
//     note above).
//   0x0045431b-0x0045433d: ambient-sound reseed from LAST_GAME_TIME + rng ch0 reseed from the
//     persisted single-byte seed -- UNREACHABLE today (see hazard note), asserted anyway per the
//     "do not weaken a case" rule.
//   0x00454342-0x00454350: show_unit_flags / mp_ally_victory_rule_flag cleared for the fresh SP
//     session -- UNREACHABLE today, asserted anyway.
//
#include "sim/resid/sim_planet_session_begin.h"

#include "sim/sim_event_codes.h" // SESSION_SP
#include "sim_test_support.h"
#include "state/host_api.h"    // LIFT-TABLE S5: libmh_set_session_seed, the pushed RNG seed
#include "state/region_view.h" // SIMABI-STRING: the runtime's own hash_slice(), for T12's arm
#include "sim_resid_sibling_mocks.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// TU-local copies of the .cpp's own anonymous-namespace constants -- not exported by the header,
// same "each TU keeps its own copy" convention sim_land_players_on_planet.cpp's banner documents.
constexpr uint32_t PROFILE_CONTROLLER_MOTHER  = 7;    // 0x00454282
constexpr uint32_t PROFILE_CONTROLLER_SHUTTLE = 0xb;  // 0x004542c0
constexpr int32_t  TEXT_MOTHER_ARRIVED        = 0xa9; // 0x00454266
constexpr int32_t  TEXT_SHUTTLE_ARRIVED       = 0xaa; // 0x004542a4

struct profile_init_call {
    uint32_t    player, controller_flags, race, color_index;
    double      game_clock;
    const void *name_str;
    int32_t     side_id;
};

// Captureless lambdas convert to the plain function pointers planet_session_begin_calls holds, same
// shape as sim_prod_shuttle_unload_selftest.cpp's single file-scope `recorder g_rec`.
struct recorder {
    // ---- 0x004541e0-0x004541ef: the two rng reseeds. The wall-clock DRAW is no longer a callee
    // (LIFT-TABLE S5): libmh reads one pushed session parameter, so these record what the pushed
    // value became rather than what a mocked callee returned. Set it with libmh_set_session_seed.
    std::vector<uint32_t> ch0_seeds; // 0x004541e5
    std::vector<uint32_t> ch1_seeds; // 0x004541ef

    // ---- UI / map --------------------------------------------------------------------------------
    std::vector<int32_t>  msg_clear_sim_active_at_call; // 0x00454212 (before) / 0x004542f8 (in-bracket)
    std::vector<uint32_t> map_read_pre_planet;          // 0x0045421c

    // ---- roster ------------------------------------------------------------------------------------
    std::vector<uint8_t>           player_set_human_calls; // 0x0045425f
    int32_t                        ascii_ver_calls = 0;
    std::vector<const void *>      ascii_ver_inputs;
    std::vector<uint32_t>          ascii_ver_returns; // fed back in call order
    std::vector<profile_init_call> profile_inits;     // [0]=mother, [1]=player1 shuttle, [2]=player2 shuttle

    // ---- the SIM_ACTIVE bracket ----------------------------------------------------------------
    int32_t               diplomacy_calls = 0;       // 0x004542cf
    std::vector<double>   reload_now;                // 0x004542e2
    std::vector<int32_t>  reload_sim_active_at_call; // captured inside the mock, same call
    std::vector<uint16_t> progress_players;          // 0x004542ee

    // ---- the tail -- UNREACHABLE today, see the file banner's hazard note ------------------------
    int32_t time_resync_calls = 0; // 0x00454316
    struct snd_call {
        int32_t planet_index;
        double  current_time;
    };
    std::vector<snd_call> snd_calls;           // 0x0045432c
    int32_t               cam_dirty_calls = 0; // 0x00454331

    sim_store *own = nullptr; // set per-case so a mock can read live state at its own call time

    // SIMABI-STRING (T12): make the profile_init mock REPRODUCE the original's name copy instead of
    // only recording the pointer. OFF by default and it has to be -- every other case feeds
    // ascii_ver_returns fake non-pointer handles (0xe001, 0x6001, ...) that must never be
    // dereferenced. See T12 for the fidelity argument and the decompile it is taken from.
    bool profile_name_copy = false;

    void reset() { *this = recorder{}; }
};
recorder g_rec;

const planet_session_begin_calls &mock_calls() {
    static const planet_session_begin_calls c = {
        [](uint32_t seed) { g_rec.ch0_seeds.push_back(seed); },
        [](uint32_t seed) { g_rec.ch1_seeds.push_back(seed); },
        []() { g_rec.msg_clear_sim_active_at_call.push_back(g_rec.own->sim_active_mut()); },
        [](uint32_t planet_index) { g_rec.map_read_pre_planet.push_back(planet_index); },
        [](uint8_t player) { g_rec.player_set_human_calls.push_back(player); },
        [](void *str) -> uint32_t {
            g_rec.ascii_ver_inputs.push_back(str);
            const uint32_t v = g_rec.ascii_ver_returns[(size_t)g_rec.ascii_ver_calls];
            ++g_rec.ascii_ver_calls;
            return v;
        },
        [](uint32_t player, uint32_t controller_flags, uint32_t race, double game_clock,
           uint32_t color_index, char *name_str, int32_t side_id) {
            g_rec.profile_inits.push_back(
                {player, controller_flags, race, color_index, game_clock, name_str, side_id});
            // The ONE thing the original does that a recording mock cannot: copy name_str into
            // _G_LLM_STRAT_PLAYERS[player].name. Transcribed from the decompile of
            // llm_strat_player_profile_init @0x00454985 -- its copy loop is an unrolled-by-2 strcpy
            // that stops at the terminator in either half, so std::strcpy is byte-exact; the
            // >12-char truncate-to-9-plus-ellipsis branch below it is NOT modelled and T12 asserts
            // its names are short enough that the original would not take it either.
            if (g_rec.profile_name_copy && g_rec.own != nullptr && name_str != nullptr)
                strcpy(g_rec.own->profile_at((int32_t)player).name, name_str);
        },
        []() { ++g_rec.diplomacy_calls; },
        [](double now) {
            g_rec.reload_now.push_back(now);
            g_rec.reload_sim_active_at_call.push_back(g_rec.own->sim_active_mut());
        },
        [](uint16_t player) { g_rec.progress_players.push_back(player); },
        []() { ++g_rec.time_resync_calls; },
        [](int32_t planet_index, double current_time) {
            g_rec.snd_calls.push_back({planet_index, current_time});
        },
        []() { ++g_rec.cam_dirty_calls; },
    };
    return c;
}

} // namespace

void run_planet_session_begin_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- both rng channels are seeded from the PUSHED session seed. The original draws the
    // wall clock twice here and this arm used to pin that the second draw was independent of the
    // first; LIFT-TABLE S5 replaced the pulled read with one init parameter (libmh_set_session_seed),
    // so what is left to pin is that BOTH channels get it and neither gets 0 by omission.
    // 0x004541e0-0x004541ef.
    // =================================================================================================
    {
        fx.reset();
        g_rec.reset();
        libmh_set_session_seed(111);
        g_rec.ascii_ver_returns = {0xa001, 0xa002, 0xa003};
        sim_store own           = fx.store();
        g_rec.own               = &own;

        detail::planet_session_begin(fx.view(), own, mock_calls(), /*race=*/0, /*reset_flag=*/0, mock_ssr_calls(),
                                     mock_ngi_calls(), mock_lpop_calls(), mock_pmsi_calls());

        // TWO, not one: llm_strat_rng_seed_ch0 is called at 0x004541e5 (the wallclock draw this
        // case is about) AND again at 0x0045433d (from _G_LLM_STRAT_RNG_SEED_BYTE, the tail
        // reseed T11 pins). Both are unconditional in the disassembly. This line said `1` while
        // T11 in the same file said `2` -- authored before the file could run, so nothing
        // reconciled them. ch1 really is called once (0x004541ef only).
        ck_eq((uint32_t)g_rec.ch0_seeds.size(), 2u,
              "T1: rng_seed_ch0 called twice, 0x004541e5 (wallclock) + 0x0045433d (seed byte)");
        ck_eq((uint32_t)g_rec.ch1_seeds.size(), 1u, "T1: rng_seed_ch1 called exactly once, 0x004541ef");
        ck_eq(g_rec.ch0_seeds.empty() ? 0u : g_rec.ch0_seeds[0], 111u,
              "T1: ch0 seeded from the pushed session seed, 0x004541e0-0x004541e5");
        ck_eq(g_rec.ch1_seeds.empty() ? 0u : g_rec.ch1_seeds[0], 111u,
              "T1: ch1 seeded from the SAME pushed value -- the declared S5 divergence from the "
              "original's two independent draws, 0x004541ea-0x004541ef");
        // AND THE PUSH IS WHAT DID IT, not a constant: re-run with a different seed and both move.
        g_rec.ch0_seeds.clear();
        g_rec.ch1_seeds.clear();
        g_rec.ascii_ver_returns = {0xa001, 0xa002, 0xa003};
        g_rec.ascii_ver_calls   = 0;
        libmh_set_session_seed(222);
        detail::planet_session_begin(fx.view(), own, mock_calls(), 0, 0, mock_ssr_calls(),
                                     mock_ngi_calls(), mock_lpop_calls(), mock_pmsi_calls());
        ck((!g_rec.ch0_seeds.empty() && g_rec.ch0_seeds[0] == 222u) &&
               (!g_rec.ch1_seeds.empty() && g_rec.ch1_seeds[0] == 222u),
           "T1: a DIFFERENT pushed seed reaches both channels -- the slot is read per session, not "
           "captured once");
        libmh_set_session_seed(0);
    }

    // =================================================================================================
    // T2 -- player_race stored verbatim; session_mode FORCED to SESSION_SP regardless of the prior
    // value; PlayerSide/local_player_slot FORCED to their SP defaults (0/1) regardless of the prior
    // values. 0x00454203/0x00454208/0x00454221/0x0045422a.
    // =================================================================================================
    {
        fx.reset();
        g_rec.reset();
        g_rec.ascii_ver_returns = {0xb001, 0xb002, 0xb003};
        fx.session_mode         = 42; // distinct sentinel, must be overwritten
        fx.player_side          = 9;  // distinct sentinel, must be overwritten
        fx.local_player_slot    = 9;  // distinct sentinel, must be overwritten
        sim_store own           = fx.store();
        g_rec.own               = &own;

        detail::planet_session_begin(fx.view(), own, mock_calls(), /*race=*/55, /*reset_flag=*/0, mock_ssr_calls(),
                                     mock_ngi_calls(), mock_lpop_calls(), mock_pmsi_calls());

        ck_eq((uint32_t)own.player_race_mut(), 55u, "T2: player_race stored verbatim, 0x00454203");
        ck_eq((uint32_t)own.session_mode(), (uint32_t)SESSION_SP,
              "T2: session_mode FORCED to SESSION_SP(1) regardless of the prior value(42), 0x00454208");
        ck_eq((uint32_t)own.player_side_mut(), 0u,
              "T2: PlayerSide FORCED to 0 regardless of the prior value(9), 0x00454221");
        ck_eq((uint32_t)own.local_player_slot_mut(), 1u,
              "T2: local_player_slot FORCED to 1 regardless of the prior value(9), 0x0045422a");
    }

    // =================================================================================================
    // T3a/b/c/d -- PLAYER_PROFILE_RACE_SELECTOR, all four sides of the ==0||==1 test:
    //   race=0  -> (mother_race,shuttle_race) = (1,2)   [0x00454237 JZ taken]
    //   race=1  -> (mother_race,shuttle_race) = (1,2)   [0x0045423d JNZ NOT taken]
    //   race=2  -> (mother_race,shuttle_race) = (2,1)   [both compares miss]
    //   race=-1 -> (mother_race,shuttle_race) = (2,1)   [signed-negative boundary: a "race < 2" range
    //              check would also pass this by accident -- pins that the real test is the literal
    //              pair of equality compares, not a range]
    // profile_inits[0] is the mother call, [1]/[2] are the two shuttle calls (race field only differs
    // by branch, so this also confirms both shuttle calls share shuttle_race).
    // =================================================================================================
    {
        struct case_t {
            int32_t     race;
            uint32_t    want_mother_race, want_shuttle_race;
            const char *tag;
        };
        const case_t cases[] = {
            {0, 1, 2, "T3a: race=0 -> mother_race=1/shuttle_race=2, 0x00454237/0x0045423f-0x00454246"},
            {1, 1, 2, "T3b: race=1 -> mother_race=1/shuttle_race=2 (JNZ not taken), 0x0045423d"},
            {2, 2, 1, "T3c: race=2 -> mother_race=2/shuttle_race=1 (both compares miss), 0x0045424f-0x00454256"},
            {-1, 2, 1, "T3d: race=-1 -> mother_race=2/shuttle_race=1 (signed-negative boundary, literal "
                       "==0||==1 test, NOT a race<2 range check), 0x00454233-0x0045423d"},
        };
        for (const case_t &tc : cases) {
            fx.reset();
            g_rec.reset();
            g_rec.ascii_ver_returns = {0xc001, 0xc002, 0xc003};
            sim_store own           = fx.store();
            g_rec.own               = &own;

            detail::planet_session_begin(fx.view(), own, mock_calls(), tc.race, /*reset_flag=*/0, mock_ssr_calls(),
                                         mock_ngi_calls(), mock_lpop_calls(), mock_pmsi_calls());

            ck_eq((uint32_t)g_rec.profile_inits.size(), 3u, "T3: exactly 3 player_profile_init calls");
            if (g_rec.profile_inits.size() == 3) {
                ck_eq(g_rec.profile_inits[0].race, tc.want_mother_race, tc.tag);
                char buf1[160];
                snprintf(buf1, sizeof buf1, "%s (player1 shuttle)", tc.tag);
                char buf2[160];
                snprintf(buf2, sizeof buf2, "%s (player2 shuttle)", tc.tag);
                ck_eq(g_rec.profile_inits[1].race, tc.want_shuttle_race, buf1);
                ck_eq(g_rec.profile_inits[2].race, tc.want_shuttle_race, buf2);
            }
        }
    }

    // =================================================================================================
    // T4 -- controller_flags/color_index/side_id/player-id are FIXED per-slot constants, distinct
    // between mother and shuttle so a swapped pair fails: mother=(player0, flags 7, color 0, side -1),
    // both shuttles=(player1/2, flags 0xb, color 1, side -1). 0x0045425f-0x00454289 (mother),
    // 0x0045428e-0x004542cf (shuttles, players 1 and 2 inclusive).
    // =================================================================================================
    {
        fx.reset();
        g_rec.reset();
        g_rec.ascii_ver_returns = {0xd001, 0xd002, 0xd003};
        sim_store own           = fx.store();
        g_rec.own               = &own;

        detail::planet_session_begin(fx.view(), own, mock_calls(), /*race=*/0, /*reset_flag=*/0, mock_ssr_calls(),
                                     mock_ngi_calls(), mock_lpop_calls(), mock_pmsi_calls());

        ck_eq((uint32_t)g_rec.profile_inits.size(), 3u, "T4: exactly 3 player_profile_init calls");
        if (g_rec.profile_inits.size() == 3) {
            const auto &mother = g_rec.profile_inits[0];
            const auto &sh1    = g_rec.profile_inits[1];
            const auto &sh2    = g_rec.profile_inits[2];
            ck_eq(mother.player, 0u, "T4: mother is player 0, 0x0045425d");
            ck_eq(mother.controller_flags, PROFILE_CONTROLLER_MOTHER,
                  "T4: mother controller_flags=7, 0x00454282");
            ck_eq(mother.color_index, 0u, "T4: mother color_index=0, 0x00454289 arg");
            ck_eq((uint32_t)mother.side_id, (uint32_t)-1, "T4: mother side_id=-1, 0x00454264");
            ck_eq(sh1.player, 1u, "T4: first shuttle is player 1, 0x0045428e loop start");
            ck_eq(sh2.player, 2u, "T4: second shuttle is player 2, loop bound EBP-0x18<=3");
            ck_eq(sh1.controller_flags, PROFILE_CONTROLLER_SHUTTLE,
                  "T4: player1 shuttle controller_flags=0xb, 0x004542c0");
            ck_eq(sh2.controller_flags, PROFILE_CONTROLLER_SHUTTLE,
                  "T4: player2 shuttle controller_flags=0xb, same site, second iteration");
            ck_eq(sh1.color_index, 1u, "T4: player1 shuttle color_index=1, 0x004542c8 arg");
            ck_eq(sh2.color_index, 1u, "T4: player2 shuttle color_index=1, same site, second iteration");
            ck_eq((uint32_t)sh1.side_id, (uint32_t)-1, "T4: player1 shuttle side_id=-1, 0x004542a2");
            ck_eq((uint32_t)sh2.side_id, (uint32_t)-1, "T4: player2 shuttle side_id=-1, same site");
        }
    }

    // =================================================================================================
    // T5 -- getAsciiVer is called FRESH every loop iteration, not hoisted before the loop: seeding a
    // DISTINCT return per call and asserting player1's and player2's shuttle name_str values DIFFER
    // catches a hoist-to-one-cached-call bug (which would give both the SAME value). Also pins the
    // INPUT pointer: mother reads text_ptrs[0xa9], both shuttle iterations read the SAME
    // text_ptrs[0xaa] (only the RETURN differs per call, not what is read).
    // =================================================================================================
    {
        fx.reset();
        g_rec.reset();
        static const wchar_t mother_glyph[]  = L"M";
        static const wchar_t shuttle_glyph[] = L"S";
        fx.text_ptrs[TEXT_MOTHER_ARRIVED]    = mother_glyph;
        fx.text_ptrs[TEXT_SHUTTLE_ARRIVED]   = shuttle_glyph;
        g_rec.ascii_ver_returns              = {0xe001, 0xe002, 0xe003}; // 3 DISTINCT returns
        sim_store own                        = fx.store();
        g_rec.own                            = &own;

        detail::planet_session_begin(fx.view(), own, mock_calls(), /*race=*/0, /*reset_flag=*/0, mock_ssr_calls(),
                                     mock_ngi_calls(), mock_lpop_calls(), mock_pmsi_calls());

        ck_eq((uint32_t)g_rec.ascii_ver_calls, 3u,
              "T5: getAsciiVer called exactly 3 times (1 mother + 2 shuttle, NOT hoisted/cached)");
        if (g_rec.ascii_ver_inputs.size() == 3) {
            ck_eq(g_rec.ascii_ver_inputs[0] == (const void *)mother_glyph ? 1u : 0u, 1u,
                  "T5: mother call reads text_ptrs[0xa9], 0x00454266");
            ck_eq(g_rec.ascii_ver_inputs[1] == (const void *)shuttle_glyph ? 1u : 0u, 1u,
                  "T5: player1 shuttle call reads text_ptrs[0xaa], 0x004542a4");
            ck_eq(g_rec.ascii_ver_inputs[2] == (const void *)shuttle_glyph ? 1u : 0u, 1u,
                  "T5: player2 shuttle call reads the SAME text_ptrs[0xaa] (re-evaluated, not cached)");
        }
        if (g_rec.profile_inits.size() == 3) {
            ck_eq(g_rec.profile_inits[0].name_str == (const void *)(uintptr_t)0xe001 ? 1u : 0u, 1u,
                  "T5: mother name_str = getAsciiVer's own return, 0x00454289 arg");
            ck_eq(g_rec.profile_inits[1].name_str == (const void *)(uintptr_t)0xe002 ? 1u : 0u, 1u,
                  "T5: player1 shuttle name_str = ITS OWN getAsciiVer call's return (0xe002)");
            ck_eq(g_rec.profile_inits[2].name_str == (const void *)(uintptr_t)0xe003 ? 1u : 0u, 1u,
                  "T5: player2 shuttle name_str = A DIFFERENT return (0xe003) than player1's -- a "
                  "hoisted/cached getAsciiVer call would give both shuttles the SAME (0xe002) value");
        }
    }

    // =================================================================================================
    // T6 -- *v.game_clock is read VERBATIM for all three player_profile_init calls (mother AND both
    // shuttles), a single distinctive non-0/1 value so a translation that substituted a literal 0.0
    // (a very easy typo next to the `0.0` literal used elsewhere in this same function, see T9) is
    // caught. 0x00454279/0x00454273 (mother), same stack slots reloaded per shuttle iteration.
    // =================================================================================================
    {
        fx.reset();
        g_rec.reset();
        g_rec.ascii_ver_returns = {0xf001, 0xf002, 0xf003};
        fx.game_clock           = 777.25;
        // The sibling's clock, deliberately DISTINCT from every other double in this case: if
        // the profile init ever read last_game_time instead of game_clock, the assertion below
        // would see 999.5 rather than 0.
        sibling_rec()                 = sibling_record{};
        sibling_rec().ssr_clock_value = 999.5;
        sim_store own                 = fx.store();
        g_rec.own                     = &own;

        detail::planet_session_begin(fx.view(), own, mock_calls(), /*race=*/0, /*reset_flag=*/0, mock_ssr_calls(),
                                     mock_ngi_calls(), mock_lpop_calls(), mock_pmsi_calls());

        if (g_rec.profile_inits.size() == 3) {
            // ZERO, not the seeded 777.25, and the reason is the SIBLING: planet_session_begin
            // calls session_state_reset at 0x004541f7 BEFORE this read at 0x00454279, and that
            // body zeroes the whole clock quintet unconditionally -- above its reset_flag branch,
            // so no flag value avoids it (sim_session_state_reset.cpp, 0x00453f33-0x00453f98).
            // The 777.25 here was authored while the sibling could not run; it asserted a value
            // the original never produces either. The discrimination that remains is real: a
            // frame that read last_game_time by mistake would see 999.5.
            ck_eq_d(g_rec.profile_inits[0].game_clock, 0.0,
                    "T6: mother game_clock = *v.game_clock (0 -- the sibling zeroed it), 0x00454279");
            ck_eq_d(g_rec.profile_inits[1].game_clock, 0.0, "T6: player1 shuttle game_clock = *v.game_clock");
            ck_eq_d(g_rec.profile_inits[2].game_clock, 0.0, "T6: player2 shuttle game_clock = *v.game_clock");
        }
    }

    // =================================================================================================
    // T7 -- llm_game_player_set_human is called EXACTLY ONCE, with player=0 (the mother slot only --
    // the two shuttle players do NOT get this call). 0x0045425d-0x0045425f.
    // =================================================================================================
    {
        fx.reset();
        g_rec.reset();
        g_rec.ascii_ver_returns = {0x1001, 0x1002, 0x1003};
        sim_store own           = fx.store();
        g_rec.own               = &own;

        detail::planet_session_begin(fx.view(), own, mock_calls(), /*race=*/0, /*reset_flag=*/0, mock_ssr_calls(),
                                     mock_ngi_calls(), mock_lpop_calls(), mock_pmsi_calls());

        ck_eq((uint32_t)g_rec.player_set_human_calls.size(), 1u,
              "T7: llm_game_player_set_human called exactly once, 0x0045425f");
        if (!g_rec.player_set_human_calls.empty())
            ck_eq(g_rec.player_set_human_calls[0], 0u, "T7: called with player=0 (mother slot), 0x0045425d");
    }

    // =================================================================================================
    // T8 -- map_ReadMap_pre receives *v.planet_index VERBATIM (the CURRENT-planet global, read before
    // the roster loop). 0x00454217-0x0045421c.
    // =================================================================================================
    {
        fx.reset();
        g_rec.reset();
        g_rec.ascii_ver_returns = {0x2001, 0x2002, 0x2003};
        fx.planet_index         = 13;
        sim_store own           = fx.store();
        g_rec.own               = &own;

        detail::planet_session_begin(fx.view(), own, mock_calls(), /*race=*/0, /*reset_flag=*/0, mock_ssr_calls(),
                                     mock_ngi_calls(), mock_lpop_calls(), mock_pmsi_calls());

        ck_eq((uint32_t)g_rec.map_read_pre_planet.size(), 1u, "T8: map_ReadMap_pre called exactly once");
        if (!g_rec.map_read_pre_planet.empty())
            // DERIVED, not the seeded 13: session_state_reset's full-reset arm (reset_flag 0)
            // re-derives G_PLANET_INDEX from the system table before this read. Comparing against
            // the view AFTER the call keeps the property this case is about -- that the argument
            // is *v.planet_index and not some neighbouring global -- without pinning a value the
            // sibling owns. Nothing between 0x00454217 and the return writes it again.
            ck_eq(g_rec.map_read_pre_planet[0], (uint32_t)*fx.view().planet_index,
                  "T8: map_ReadMap_pre(planet_index) = *v.planet_index, 0x00454217-0x0045421c");
    }

    // =================================================================================================
    // T9 -- the SIM_ACTIVE 0->1 bracket, in order: diplomacy_init_skirmish() fires ONCE, THEN
    // sim_active is set to 0, THEN reload_snapshot_resync_clocks(LITERAL 0.0)/progress_propagate_
    // unlocks(PlayerSide)/a SECOND ui_message_queue_clear_all() all run
    // while sim_active reads 0, THEN sim_active is set to 1. The FIRST ui_message_queue_clear_all()
    // (0x00454212) runs BEFORE the bracket opens, while sim_active still holds this case's seeded
    // sentinel (77) -- proving the two calls straddle the 0/1 toggle rather than both landing on the
    // same side of it. 0x004542cf-0x00454301.
    // =================================================================================================
    {
        fx.reset();
        g_rec.reset();
        g_rec.ascii_ver_returns = {0x3001, 0x3002, 0x3003};
        fx.sim_active           = 77; // distinct sentinel, neither 0 nor 1
        fx.player_side          = 5;  // overwritten to 0 by this function before progress_propagate_unlocks
        sim_store own           = fx.store();
        g_rec.own               = &own;

        detail::planet_session_begin(fx.view(), own, mock_calls(), /*race=*/0, /*reset_flag=*/0, mock_ssr_calls(),
                                     mock_ngi_calls(), mock_lpop_calls(), mock_pmsi_calls());

        ck_eq((uint32_t)g_rec.msg_clear_sim_active_at_call.size(), 2u,
              "T9: ui_message_queue_clear_all called exactly twice, 0x00454212 and 0x004542f8");
        if (g_rec.msg_clear_sim_active_at_call.size() == 2) {
            ck_eq((uint32_t)g_rec.msg_clear_sim_active_at_call[0], 77u,
                  "T9: FIRST clear (0x00454212) runs BEFORE the bracket -- sim_active still 77");
            ck_eq((uint32_t)g_rec.msg_clear_sim_active_at_call[1], 0u,
                  "T9: SECOND clear (0x004542f8) runs INSIDE the bracket -- sim_active reads 0 here");
        }
        ck_eq((uint32_t)g_rec.diplomacy_calls, 1u, "T9: diplomacy_init_skirmish called exactly once, 0x004542cf");
        ck_eq((uint32_t)g_rec.reload_now.size(), 1u, "T9: reload_snapshot_resync_clocks called exactly once");
        if (!g_rec.reload_now.empty()) {
            ck_eq_d(g_rec.reload_now[0], 0.0, "T9: reload_snapshot_resync_clocks(now) is the LITERAL 0.0, 0x004542de-0x004542e2");
            ck_eq((uint32_t)g_rec.reload_sim_active_at_call[0], 0u,
                  "T9: sim_active already 0 by the time reload_snapshot_resync_clocks fires, 0x004542d4-0x004542e2");
        }
        ck_eq((uint32_t)g_rec.progress_players.size(), 1u, "T9: progress_propagate_unlocks called exactly once");
        if (!g_rec.progress_players.empty())
            ck_eq(g_rec.progress_players[0], 0u,
                  "T9: progress_propagate_unlocks(PlayerSide) reads PlayerSide AFTER it was forced to 0, "
                  "0x004542e7-0x004542ee");
        ck_eq((uint32_t)own.sim_active_mut(), 1u, "T9: sim_active ends the bracket at 1, 0x004542fd");
    }

    // =================================================================================================
    // T10a/b -- reset_flag is forwarded UNCHANGED to session_state_reset (0x004541f4-0x004541f7); this
    // is observed through session_state_reset's OWN documented branch on CurrentSystem (see
    // sim/resid/sim_session_state_reset.h): reset_flag 0 (full reset) PINS CurrentSystem to 1
    // regardless of the prior value; reset_flag 1 (soft path) INCREMENTS the prior value by exactly 1.
    // A translation that dropped or mis-forwarded reset_flag would land on the wrong one of these two,
    // very different, outcomes.
    // =================================================================================================
    {
        fx.reset();
        g_rec.reset();
        g_rec.ascii_ver_returns = {0x4001, 0x4002, 0x4003};
        fx.current_system       = 5; // distinct sentinel; full-reset path must overwrite this to 1
        sim_store own           = fx.store();
        g_rec.own               = &own;

        detail::planet_session_begin(fx.view(), own, mock_calls(), /*race=*/0, /*reset_flag=*/0, mock_ssr_calls(),
                                     mock_ngi_calls(), mock_lpop_calls(), mock_pmsi_calls());

        ck_eq((uint32_t)own.current_system_mut(), 1u,
              "T10a: reset_flag=0 forwarded to session_state_reset's FULL-RESET path -> CurrentSystem "
              "pinned to 1 (was 41), sim_session_state_reset.cpp:83");
    }
    {
        fx.reset();
        g_rec.reset();
        g_rec.ascii_ver_returns = {0x5001, 0x5002, 0x5003};
        // 5, not 41: the soft path increments CurrentSystem and then indexes
        // system_define_index_base[CurrentSystem*0x23 + 4], and that table is 32 systems long --
        // in the fixture AND in the game. 41 -> 42 reads slot 1474 of 1120. ASan called it a
        // heap-use-after-free at sim_session_state_reset.cpp:123 the first time this case ever
        // ran; the case was authored while the file could not be built.
        fx.current_system = 5; // must become 6 (soft path increments by exactly 1)
        sim_store own     = fx.store();
        g_rec.own         = &own;

        detail::planet_session_begin(fx.view(), own, mock_calls(), /*race=*/0, /*reset_flag=*/1, mock_ssr_calls(),
                                     mock_ngi_calls(), mock_lpop_calls(), mock_pmsi_calls());

        ck_eq((uint32_t)own.current_system_mut(), 6u,
              "T10b: reset_flag=1 forwarded to session_state_reset's SOFT path -> CurrentSystem "
              "incremented by exactly 1 (41 -> 42), sim_session_state_reset.cpp:122 (NOT pinned to 1, "
              "which would wrongly match T10a instead)");
    }

    // =================================================================================================
    // T11 -- the tail: rng ch0 re-seeded from the persisted single-BYTE seed (boundary value 0xff, the
    // top of the byte range, to prove the byte-to-uint32_t widening does not sign-extend or truncate);
    // the ambient-sound reseed reads BOTH *v.planet_index and own.last_game_time() (DELIBERATELY
    // DIFFERENT fixture values so consuming the wrong one of the two fails); show_unit_flags/
    // mp_ally_victory_rule_flag cleared to 0 despite non-zero seeds. 0x00454336-0x00454350.
    // UNREACHABLE TODAY -- see the file banner's hazard note (land_players_on_planet, called just
    // before this span, faults first); asserted anyway per "do not weaken a case to make it pass".
    // =================================================================================================
    {
        fx.reset();
        g_rec.reset();
        g_rec.ascii_ver_returns = {0x6001, 0x6002, 0x6003};
        fx.rng_seed_byte        = 0xff; // top-of-byte-range boundary
        fx.planet_index         = 9;    // DISTINCT from last_game_time's value below
        // last_game_time is NOT observable as seeded: session_state_reset overwrites it with
        // c.time_get_current_time() unconditionally at 0x00453f45, before this frame reads it.
        // Seeding the SIBLING's clock is what keeps the discrimination -- 456.5 would prove
        // nothing, because the original does not produce it either.
        sibling_rec()                 = sibling_record{};
        sibling_rec().ssr_clock_value = 456.5;
        fx.show_unit_flags            = 0x77; // must be cleared to 0
        fx.mp_ally_victory_rule_flag  = 0x55; // must be cleared to 0
        sim_store own                 = fx.store();
        g_rec.own                     = &own;

        detail::planet_session_begin(fx.view(), own, mock_calls(), /*race=*/0, /*reset_flag=*/0, mock_ssr_calls(),
                                     mock_ngi_calls(), mock_lpop_calls(), mock_pmsi_calls());

        ck_eq((uint32_t)g_rec.ch0_seeds.size(), 2u,
              "T11: rng_seed_ch0 called TWICE total -- the early wallclock draw AND this late "
              "rng_seed_byte reseed, 0x00454336-0x0045433d");
        if (g_rec.ch0_seeds.size() == 2)
            ck_eq(g_rec.ch0_seeds[1], 0xffu,
                  "T11: second ch0 call seeded from own.rng_seed_byte()=0xff (byte-range boundary, no "
                  "sign-extension/truncation), 0x00454336-0x0045433d");
        ck_eq((uint32_t)g_rec.snd_calls.size(), 1u, "T11: snd_ambient_reseed_planet_event_times called once");
        if (!g_rec.snd_calls.empty()) {
            ck_eq((uint32_t)g_rec.snd_calls[0].planet_index, (uint32_t)*fx.view().planet_index,
                  "T11: snd reseed planet_index = *v.planet_index, NOT last_game_time's value, "
                  "0x00454327-0x0045432c");
            ck_eq_d(g_rec.snd_calls[0].current_time, sibling_rec().ssr_clock_value,
                    "T11: snd reseed current_time = own.last_game_time() -- the value the sibling's "
                    "clock call left there -- NOT planet_index's value, 0x0045431b-0x0045432c");
        }
        ck_eq((uint32_t)g_rec.cam_dirty_calls, 1u,
              "T11: map_cam_mark_viewport_dirty called once at the very tail, 0x00454331");
        ck_eq((uint32_t)own.show_unit_flags(), 0u,
              "T11: show_unit_flags cleared to 0 despite being seeded 0x77, 0x00454342");
        ck_eq((uint32_t)own.mp_ally_victory_rule_flag_mut(), 0u,
              "T11: mp_ally_victory_rule_flag cleared to 0 despite being seeded 0x55, 0x0045434c");
    }

    // =================================================================================================
    // T12 -- SIMABI-STRING (2026-09-10): the POSITIVE hash-mutation arm for getAsciiVer.
    //
    // WHAT IT SETTLES. The ledger used to say utils_wide_to_short_str was "the one entry in this table
    // whose result feeds hashed sim state". It is not: THIS entry's output reaches
    // _G_LLM_STRAT_PLAYERS too. The route is one hop longer, and the hop is an ORIGINAL, which is why
    // the claim went unmeasured for so long -- 0x0045425d-0x004542cf hands getAsciiVer's return to
    // llm_strat_player_profile_init as name_str, and it is THAT body (decompiled @0x00454985) that
    // copies the bytes into _G_LLM_STRAT_PLAYERS[player].name, inside the 14848-byte `strat_players`
    // slice. So the mock reproduces exactly that copy (recorder::profile_name_copy) and nothing else.
    //
    // WHY THE MOCK IS FAITHFUL AND NOT A CONVENIENT ASSUMPTION: the original's copy is an unrolled-by-2
    // strcpy that breaks on the terminator in either half -- it never writes past the NUL -- and the
    // only other thing it does to `name` is the >12-character truncate-to-9-plus-"..." branch. This arm
    // asserts its names are shorter than that, so the branch the mock omits is one the original would
    // not take either.
    //
    // Same shape as sim_start_tutorial's T10: three runs, the middle one with ONE byte of the codec's
    // returned buffer flipped, hashed through the runtime's own hash_slice()/emit_slice() with
    // RID_STRAT_PLAYERS rebased onto the fixture's own profile array, and the third restoring the byte
    // so a moving hash cannot be mistaken for a noisy one.
    // =================================================================================================
    {
        namespace st = mh::state;

        const st::hash_region &hr = st::HASH_REGIONS[st::HIDX_STRAT_PLAYERS];
        ck(hr.rid == st::RID_STRAT_PLAYERS && hr.offset == 0,
           "T12: premise -- HIDX_STRAT_PLAYERS is the WHOLE of RID_STRAT_PLAYERS, so emit_slice takes "
           "the raw block path and the fixture array can stand in for it");
        ck_eq(hr.len, (uint32_t)(fx.profiles.size() * sizeof(player_profile)),
              "T12: premise -- the slice is exactly llm_strat_player_profile[8] (14848 = 8 * 0x740)");
        ck(st::owner_of(st::RID_STRAT_PLAYERS) == nullptr,
           "T12: premise -- nothing owns RID_STRAT_PLAYERS, so emit_slice reads bytes at an address");

        // The three converted arrival names the fake host codec hands back, one per getAsciiVer call
        // (mother, then the two shuttles). Under 13 characters each -- see the fidelity note above.
        static char codec_out[3][16];
        ck(sizeof("Matka") - 1 <= 12 && sizeof("Prom") - 1 <= 12,
           "T12: premise -- both names are <= 12 chars, so llm_strat_player_profile_init's "
           "truncate-to-9-plus-ellipsis branch (the part the mock omits) is not taken");

        std::vector<uint8_t> img_a((size_t)hr.len), img_b((size_t)hr.len);

        // One full run of the body with a chosen mother-name first byte, hashed at the named slice.
        auto arm = [&](char mother_first_byte, std::vector<uint8_t> *img) -> uint64_t {
            strcpy(codec_out[0], "Matka");
            strcpy(codec_out[1], "Prom");
            strcpy(codec_out[2], "Prom");
            codec_out[0][0] = mother_first_byte;

            fx.reset();
            g_rec.reset();
            sibling_rec()           = sibling_record{};
            g_rec.profile_name_copy = true;
            g_rec.ascii_ver_returns = {(uint32_t)(uintptr_t)codec_out[0],
                                       (uint32_t)(uintptr_t)codec_out[1],
                                       (uint32_t)(uintptr_t)codec_out[2]};
            sim_store own           = fx.store();
            g_rec.own               = &own;

            detail::planet_session_begin(fx.view(), own, mock_calls(), /*race=*/0, /*reset_flag=*/0,
                                         mock_ssr_calls(), mock_ngi_calls(), mock_lpop_calls(),
                                         mock_pmsi_calls());

            st::rebase(st::RID_STRAT_PLAYERS, (uint32_t)(uintptr_t)fx.profiles.data(), hr.len);
            const uint64_t h = st::hash_slice(st::HIDX_STRAT_PLAYERS, true);
            if (img != nullptr) memcpy(img->data(), fx.profiles.data(), (size_t)hr.len);
            st::unrebase(st::RID_STRAT_PLAYERS);
            return h;
        };

        const uint64_t h_a = arm('M', &img_a);
        const uint64_t h_b = arm('N', &img_b);  // ONE byte of the returned buffer flipped: 'M' -> 'N'
        const uint64_t h_r = arm('M', nullptr); // reverted

        uint32_t differing = 0;
        for (uint32_t i = 0; i < hr.len; ++i)
            if (img_a[i] != img_b[i]) ++differing;
        printf("  [SIMABI-STRING] getAsciiVer -> HIDX_STRAT_PLAYERS(\"%s\"): compared %u bytes, "
               "%u differing, hash %016llx -> %016llx, reverted %016llx\n",
               hr.name, hr.len, differing, (unsigned long long)h_a, (unsigned long long)h_b,
               (unsigned long long)h_r);

        ck(differing > 0,
           "T12: flipping ONE byte of getAsciiVer's returned buffer changes the hashed "
           "`strat_players` slice -- so this entry feeds hashed state too, and the ledger's 'the one "
           "entry' claim was wrong");
        ck(h_b != h_a,
           "T12: ... and the slice HASH moves with it (hash_slice(HIDX_STRAT_PLAYERS), the "
           "determinism gate's own emitter)");
        ck(h_r == h_a,
           "T12: ... and reverting the byte restores the hash EXACTLY -- the control that keeps this "
           "arm from passing on a noisy hash");
    }
}

} // namespace mh::sim::test
