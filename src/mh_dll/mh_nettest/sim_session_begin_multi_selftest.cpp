//
// sim_session_begin_multi_selftest.cpp -- `simtest` offline oracle for llm_strat_session_begin_multi
// @0x0045435f (sim/resid/sim_session_begin_multi.h/.cpp, RI-SIM sim_resid batch A/B). The MULTIPLAYER
// session-entry writer: per-player race/side table, the lobby slot descriptors, the chat target mask,
// the lockstep adapt clock and the RNG seed byte.
//
// NO SHADOW SITE -- session-entry-only writer, proof:OFFLINE (see the header banner). This file is
// the only verification.
//
// KNOWN HAZARD, flagged for the conductor rather than worked around here (out of this file's scope):
// `detail::session_begin_multi` makes TWO INTRA-SLICE calls as direct, unconditional C++ calls --
// `detail::session_state_reset(v, own, live_session_state_reset_calls(), 2)` as its very FIRST
// statement, and `detail::land_players_on_planet(v, own, live_land_players_on_planet_calls(), ...)`
// later -- and NEITHER is reachable through this file's `session_begin_multi_calls` mock (sim_resid
// rule 2 / G21: intra-set edges are not mockable seams). `live_session_state_reset_calls()` and
// `live_land_players_on_planet_calls()` are themselves bound to real ABSOLUTE ADDRESSES in the
// original game image (e.g. `mh::call::time_GetCurrentTime` -> a naked-asm indirect CALL to the
// literal VA 0x00427616, per addr/mh_calls.gen.cpp's `s_void_EAX`/etc. thunks) -- and
// `sim_session_state_reset.cpp`'s full-reset path (reset_flag==2, always taken here) reaches such a
// call in its SECOND executed statement (`c.time_get_current_time()`, before this function's own
// state is even reachable), then calls the further intra-slice `new_game_init`, which itself fires
// eight more live calls. `net_selftest.exe` is a standalone console exe with "no game, no DllMain"
// (src/mh_dll/README.md) -- those VAs are not backed by the real callees in that process. This is a
// BATCH-WIDE gap (every one of the ten sim_resid functions with an intra-slice edge inherits it, not
// just this one) -- the other 8 siblings also have no selftest yet, and the two that DO
// (sim_clock_resync, sim_session_clear_presence_flag) are exactly the two with ZERO outward calls.
// The cases below assert the INTENDED behaviour transcribed from the disassembly (correct once the
// conductor gives the intra-slice chain a test-mode override); they are not evidence this file can
// run today without crashing net_selftest.exe. See the returned report for the full writeup.
//
// EXPECTED BEHAVIOUR, from the header banner's own derivation (sim/resid/sim_session_begin_multi.h)
// and the raw disassembly (tmp/decomp_sim_resid/llm_strat_session_begin_multi_0045435f.asm):
//   0x0045437a: detail::session_state_reset(2) -- INTRA-SLICE, unconditional, always full-reset.
//   0x0045438c-0x004543a9: PlayerSide = net_local_player_slot; player_race =
//     Players[PlayerSide].race_or_faction (`Players`/player_desc_slots, NOT `profiles`).
//   0x004543ae-0x004543cd: session_mode = (host_count <= 1) ? MP_LOCAL(2) : MP_LOCKSTEP(3), unsigned
//     compare (JBE).
//   0x004543cd-0x004543eb: FOUR literal, unconditional pokes -- System int-index 4 (0x00be1a40)=0x1f,
//     Planets[0x1f].system_index=0, G_PLANET_INDEX=0x1f, CurrentSystem=0.
//   0x004543f5-0x00454422: rng_seed_byte = cfg_blob[0x14]; show_unit_flags=1;
//     mp_ally_victory_rule_flag=0; chat_target_mask=0; then rng_seed_ch0(rng_seed_byte).
//   0x00454427-0x00454447: player_set_human(PlayerSide), ui_message_queue_clear_all() (1st of 2,
//     EFFECTFUL WALL), map_read_map_pre(G_PLANET_INDEX), net_lockstep_peer_timing_reset().
//   0x00454447-0x004544b7: for i in 0..7, if Players[i].controller_flags != 0,
//     player_profile_init(i, controller_flags, race_or_faction, *game_clock, color_or_team, &name,
//     scenario_side_id).
//   0x004544b9-0x004544be: diplomacy_init_multiplayer(), THEN sim_active=0.
//   0x004544ca-0x004544d8: reload_snapshot_resync_clocks(0.0), progress_propagate_unlocks(PlayerSide).
//   0x004544dd-0x004544eb: if tutorial_step==0, update_planet_progress().
//   0x004544eb-0x00454504: ui_message_queue_clear_all() (2nd of 2 -- PRESERVE, not a dedup target),
//     THEN sim_active=1, detail::land_players_on_planet(...) -- INTRA-SLICE, game_speed_recompute().
//   0x00454509-0x00454513: lobby_map_recv_step_stub() (EMPTY -- not reproduced since SIMABI-HOOKS)
//   and net_lockstep_sync_delay_stub() (both STUBS in the original,
//     called anyway); ret = the stub's result.
//   0x00454516-0x0045454f: if (ret < 0 && tutorial_step == 0) { (the original's empty
//   teardown_hook_stub, not reproduced)
//     menu_force_return_to_main(); return -1; } else { lockstep_adapt_next_time = *game_clock +
//     lockstep_session_adapt_delay; return ret; }
//
#include "sim/resid/sim_session_begin_multi.h"

#include "sim_test_support.h"
#include "sim_resid_sibling_mocks.h"

#include <cstring>
#include <string>

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- recorded outward calls (session_begin_multi's OWN `_calls` struct only -- the two intra-slice
// edges are NOT routed through this and are not observable here, see the file banner) --------------
struct ProfileInitCall {
    uint32_t    player;
    uint32_t    controller_flags;
    uint32_t    race;
    double      game_clock;
    uint32_t    color_index;
    std::string name;
    int32_t     side_id;
};

std::vector<void *>          g_scenario_clone_calls;
std::vector<uint32_t>        g_rng_seed_ch0_calls;
std::vector<uint8_t>         g_player_set_human_calls;
int32_t                      g_ui_msg_clear_count = 0;
std::vector<uint32_t>        g_map_read_map_pre_calls;
int32_t                      g_lockstep_peer_timing_reset_count = 0;
std::vector<ProfileInitCall> g_profile_init_calls;
int32_t                      g_diplomacy_init_mp_count = 0;
std::vector<double>          g_reload_snapshot_calls;
std::vector<uint16_t>        g_progress_propagate_calls;
int32_t                      g_update_planet_progress_count  = 0;
int32_t                      g_game_speed_recompute_count    = 0;
int32_t                      g_net_lockstep_sync_delay_count = 0;
int32_t                      g_net_lockstep_sync_delay_ret   = 0; // settable per case, NOT cleared by clear_calls()
int32_t                      g_menu_force_return_count       = 0;

// ORDER-OBSERVATION captures: the value of `sim_active` as seen by four of the mocked callees, so a
// case can prove the two writes (0 then 1) landed in the right places relative to these calls rather
// than merely checking the final value. -999 = "this mock was never reached in this case".
sim_fixture *g_fx                          = nullptr;
int32_t      g_sim_active_at_diplomacy     = -999;
int32_t      g_sim_active_at_resync        = -999;
int32_t      g_sim_active_at_progress_unlk = -999;
int32_t      g_sim_active_at_update_prog   = -999;

void stub_scenario_planet_clone(void *cfg_blob) { g_scenario_clone_calls.push_back(cfg_blob); }
void stub_rng_seed_ch0(uint32_t seed) { g_rng_seed_ch0_calls.push_back(seed); }
void stub_player_set_human(uint8_t player) { g_player_set_human_calls.push_back(player); }
void stub_ui_message_queue_clear_all() { ++g_ui_msg_clear_count; }
void stub_map_read_map_pre(uint32_t planet_index) { g_map_read_map_pre_calls.push_back(planet_index); }
void stub_net_lockstep_peer_timing_reset() { ++g_lockstep_peer_timing_reset_count; }
void stub_player_profile_init(uint32_t player, uint32_t controller_flags, uint32_t race,
                              double game_clock, uint32_t color_index, char *name_str,
                              int32_t side_id) {
    g_profile_init_calls.push_back({player, controller_flags, race, game_clock, color_index,
                                    name_str != nullptr ? std::string(name_str) : std::string(),
                                    side_id});
}
void stub_diplomacy_init_multiplayer() {
    ++g_diplomacy_init_mp_count;
    if (g_fx != nullptr) g_sim_active_at_diplomacy = g_fx->sim_active;
}
void stub_reload_snapshot_resync_clocks(double now) {
    g_reload_snapshot_calls.push_back(now);
    if (g_fx != nullptr) g_sim_active_at_resync = g_fx->sim_active;
}
void stub_progress_propagate_unlocks(uint16_t player) {
    g_progress_propagate_calls.push_back(player);
    if (g_fx != nullptr) g_sim_active_at_progress_unlk = g_fx->sim_active;
}
void stub_update_planet_progress() {
    ++g_update_planet_progress_count;
    if (g_fx != nullptr) g_sim_active_at_update_prog = g_fx->sim_active;
}
void    stub_game_speed_recompute() { ++g_game_speed_recompute_count; }
int32_t stub_net_lockstep_sync_delay_stub() {
    ++g_net_lockstep_sync_delay_count;
    return g_net_lockstep_sync_delay_ret;
}
void stub_menu_force_return_to_main() { ++g_menu_force_return_count; }

const session_begin_multi_calls g_calls = {
    stub_scenario_planet_clone,
    stub_rng_seed_ch0,
    stub_player_set_human,
    stub_ui_message_queue_clear_all,
    stub_map_read_map_pre,
    stub_net_lockstep_peer_timing_reset,
    stub_player_profile_init,
    stub_diplomacy_init_multiplayer,
    stub_reload_snapshot_resync_clocks,
    stub_progress_propagate_unlocks,
    stub_update_planet_progress,
    stub_game_speed_recompute,
    stub_net_lockstep_sync_delay_stub,
    stub_menu_force_return_to_main,
};

void clear_calls() {
    g_scenario_clone_calls.clear();
    g_rng_seed_ch0_calls.clear();
    g_player_set_human_calls.clear();
    g_ui_msg_clear_count = 0;
    g_map_read_map_pre_calls.clear();
    g_lockstep_peer_timing_reset_count = 0;
    g_profile_init_calls.clear();
    g_diplomacy_init_mp_count = 0;
    g_reload_snapshot_calls.clear();
    g_progress_propagate_calls.clear();
    g_update_planet_progress_count  = 0;
    g_game_speed_recompute_count    = 0;
    g_net_lockstep_sync_delay_count = 0;
    // g_net_lockstep_sync_delay_ret is deliberately NOT reset here -- each case that cares sets it
    // explicitly right before calling run(); cases that don't care get the 0 (non-negative, success
    // branch) left by the previous case or the file's initial value.
    g_menu_force_return_count = 0;
    g_sim_active_at_diplomacy = g_sim_active_at_resync = g_sim_active_at_progress_unlk =
        g_sim_active_at_update_prog                    = -999;
}

int32_t run(sim_fixture &fx, void *cfg_blob) {
    clear_calls();
    g_fx          = &fx;
    sim_store own = fx.store();
    return detail::session_begin_multi(fx.view(), own, g_calls, cfg_blob, mock_ssr_calls(),
                                       mock_ngi_calls(), mock_lpop_calls(), mock_pmsi_calls());
}

} // namespace

void run_session_begin_multi_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- PlayerSide/player_race derivation. PlayerSide = net_local_player_slot (a WRITER, first in
    // this closure); player_race reads `Players`/player_desc_slots at THAT index, not `profiles`.
    // =================================================================================================
    {
        fx.reset();
        fx.net_local_player_slot                = 5;
        fx.player_desc_slots[5].race_or_faction = 42;
        fx.net_lobby_scan_host_count            = 1;
        uint8_t blob[32]                        = {};

        run(fx, blob);

        ck_eq((uint32_t)fx.player_side, 5u,
              "T1a: PlayerSide = net_local_player_slot, 0x0045438c-0x00454392");
        ck_eq((uint32_t)fx.player_race, 42u,
              "T1b: player_race = Players[PlayerSide].race_or_faction (Players, NOT profiles), "
              "0x00454398-0x004543a9");
    }

    // =================================================================================================
    // T2/T3/T4 -- session_mode boundary. Unsigned JBE @0x004543b5: host_count in {0,1} -> LOCAL(2),
    // host_count >= 2 -> LOCKSTEP(3). Both sides of the boundary at 1/2 are pinned.
    // =================================================================================================
    {
        fx.reset();
        fx.net_local_player_slot     = 0;
        fx.net_lobby_scan_host_count = 0;
        uint8_t blob[32]             = {};
        run(fx, blob);
        ck_eq((uint32_t)fx.session_mode, 2u,
              "T2: host_count=0 (<=1) -> session_mode=SESSION_MP_LOCAL(2), 0x004543ae-0x004543c3");
    }
    {
        fx.reset();
        fx.net_local_player_slot     = 0;
        fx.net_lobby_scan_host_count = 1;
        uint8_t blob[32]             = {};
        run(fx, blob);
        ck_eq((uint32_t)fx.session_mode, 2u,
              "T3: host_count=1 (boundary, still <=1) -> LOCAL(2), 0x004543ae-0x004543b5/0x004543c3");
    }
    {
        fx.reset();
        fx.net_local_player_slot     = 0;
        fx.net_lobby_scan_host_count = 2;
        uint8_t blob[32]             = {};
        run(fx, blob);
        ck_eq((uint32_t)fx.session_mode, 3u,
              "T4: host_count=2 (boundary, >1, JBE does not fire) -> LOCKSTEP(3), 0x004543b5-0x004543bd");
    }

    // =================================================================================================
    // T5 -- the four literal, unconditional home-planet-slot pokes. Every target is pre-seeded to a
    // DIFFERENT wrong value first, so the assertion proves an overwrite happened rather than
    // coincidentally matching a zeroed default. (The intra-slice session_state_reset ALSO rewrites
    // G_PLANET_INDEX/CurrentSystem earlier in the same call, to derived/1 respectively -- these four
    // literal pokes run strictly AFTER it and always win; see the header's own note.)
    // =================================================================================================
    {
        fx.reset();
        fx.net_local_player_slot     = 0;
        fx.net_lobby_scan_host_count = 1;
        // INDEX 4, not 1 -- `MOV dword ptr [0x00be1a40],0x1f` @0x004543cd against System's base
        // 0x00be1a30 is base + 0x10 = int32 index 4. This case asserted index 1 until 2026-08-31,
        // and so did the translation: BOTH read the slot number off the Ghidra draft's field NAME
        // ("planets[1]") rather than off the address, and sim_land_players_on_planet.cpp already
        // warns that name is untrustworthy because cfg_final_struct_System is anchored 8 bytes
        // late. An oracle that shares the translation's premise cannot fail on it, which is why
        // this needed adversarial review and not another mutation.
        fx.system_define_index_base[4]    = 0xDEADu; // the home-planet cell, pre-seeded wrong
        fx.system_define_index_base[1]    = 0xBEEFu; // the define_index/name field -- must NOT move
        fx.cfg_planets[0x1f].system_index = 7;       // pre-seeded wrong
        fx.planet_index                   = 3;       // pre-seeded wrong
        fx.current_system                 = 9;       // pre-seeded wrong
        uint8_t blob[32]                  = {};

        run(fx, blob);

        ck_eq((uint32_t)fx.system_define_index_base[4], 0x1fu,
              "T5a: System int-index 4 (0x00be1a40) = 0x1f, overwriting pre-seeded 0xDEAD, "
              "0x004543cd-0x004543d7");
        ck_eq((uint32_t)fx.system_define_index_base[1], 0xBEEFu,
              "T5a: ... and int-index 1 (0x00be1a34, the define_index/name field three other "
              "functions read as a G_TEXT_PTRS index) is UNTOUCHED -- the off-by-three-ints write "
              "this case used to encode would clobber it");
        ck_eq((uint32_t)fx.cfg_planets[0x1f].system_index, 0u,
              "T5b: Planets[0x1f].system_index = 0, overwriting pre-seeded 7, 0x004543d7-0x004543e1");
        ck_eq((uint32_t)fx.planet_index, 0x1fu,
              "T5c: G_PLANET_INDEX = 0x1f, final value wins over the intra-slice reset's own rewrite, "
              "0x004543e1-0x004543eb");
        ck_eq((uint32_t)fx.current_system, 0u,
              "T5d: CurrentSystem = 0, final value wins over the intra-slice reset's own rewrite (1), "
              "0x004543eb-0x004543f5");
    }

    // =================================================================================================
    // T6 -- the RNG seed byte: read from cfg_blob+0x14 (the caller-owned opaque blob, NOT through
    // v/own), stored, then RE-READ from storage to seed channel 0.
    // =================================================================================================
    {
        fx.reset();
        fx.net_local_player_slot     = 0;
        fx.net_lobby_scan_host_count = 1;
        uint8_t blob[32]             = {};
        blob[0x14]                   = 0xAB;

        run(fx, blob);

        ck_eq((uint32_t)fx.rng_seed_byte, 0xABu,
              "T6a: rng_seed_byte = cfg_blob[0x14] = 0xAB, 0x004543f5-0x004543fb");
        ck_eq((uint32_t)g_rng_seed_ch0_calls.size(), 1u, "T6b: rng_seed_ch0 called exactly once");
        if (!g_rng_seed_ch0_calls.empty()) {
            ck_eq(g_rng_seed_ch0_calls[0], 0xABu,
                  "T6c: rng_seed_ch0 called with the re-read stored byte (0xAB), 0x0045441b-0x00454422");
        }
    }

    // =================================================================================================
    // T7 -- the three other unconditional per-session pokes, each pre-seeded to a distinct wrong value.
    // =================================================================================================
    {
        fx.reset();
        fx.net_local_player_slot     = 0;
        fx.net_lobby_scan_host_count = 1;
        fx.show_unit_flags           = 0;    // wrong (should become 1)
        fx.mp_ally_victory_rule_flag = 99;   // wrong (should become 0)
        fx.chat_target_mask          = 0xFF; // wrong (should become 0)
        uint8_t blob[32]             = {};

        run(fx, blob);

        ck_eq((uint32_t)fx.show_unit_flags, 1u,
              "T7a: show_unit_flags = 1, overwriting pre-seeded 0, 0x00454400-0x0045440a");
        ck_eq((uint32_t)fx.mp_ally_victory_rule_flag, 0u,
              "T7b: mp_ally_victory_rule_flag = 0, overwriting pre-seeded 99, 0x0045440a-0x00454414");
        ck_eq((uint32_t)fx.chat_target_mask, 0u,
              "T7c: chat_target_mask = 0, overwriting pre-seeded 0xFF, 0x00454414-0x0045441b");
    }

    // =================================================================================================
    // T8 -- calls that fire UNCONDITIONALLY (no gate at all): scenario_planet_clone (forwards the
    // SAME cfg_blob pointer), player_set_human, the surviving stub (net_lockstep_sync_delay_stub --
    // the empty lobby_map_recv_step_stub beside it is no longer called), map_read_map_pre (reads the ALREADY-poked planet index, 0x1f),
    // net_lockstep_peer_timing_reset, and ui_message_queue_clear_all called TWICE.
    // =================================================================================================
    {
        fx.reset();
        fx.net_local_player_slot     = 3;
        fx.net_lobby_scan_host_count = 1;
        uint8_t blob[32]             = {};

        run(fx, blob);

        ck_eq((uint32_t)g_scenario_clone_calls.size(), 1u,
              "T8a: scenario_planet_clone called exactly once, 0x00454384-0x00454387");
        if (!g_scenario_clone_calls.empty()) {
            ck(g_scenario_clone_calls[0] == (void *)blob,
               "T8b: scenario_planet_clone forwards the SAME cfg_blob pointer, unmodified");
        }
        ck_eq((uint32_t)g_player_set_human_calls.size(), 1u,
              "T8c: player_set_human called exactly once");
        if (!g_player_set_human_calls.empty()) {
            ck_eq((uint32_t)g_player_set_human_calls[0], 3u,
                  "T8d: player_set_human((uint8_t)PlayerSide=3), 0x00454427-0x0045442e");
        }
        ck_eq((uint32_t)g_ui_msg_clear_count, 2u,
              "T8e: ui_message_queue_clear_all called TWICE -- PRESERVE, the original genuinely calls "
              "it twice, NOT a dedup target, 0x0045442e-0x00454433 and 0x004544eb-0x004544f0");
        ck_eq((uint32_t)g_map_read_map_pre_calls.size(), 1u,
              "T8f: map_read_map_pre called exactly once");
        if (!g_map_read_map_pre_calls.empty()) {
            ck_eq(g_map_read_map_pre_calls[0], 0x1fu,
                  "T8g: map_read_map_pre(G_PLANET_INDEX) reads the ALREADY-poked value 0x1f, "
                  "0x00454438-0x00454442");
        }
        ck_eq((uint32_t)g_lockstep_peer_timing_reset_count, 1u,
              "T8h: net_lockstep_peer_timing_reset called exactly once, 0x00454442-0x00454447");
        ck_eq((uint32_t)g_net_lockstep_sync_delay_count, 1u,
              "T8j: net_lockstep_sync_delay_stub called anyway (STUB), 0x0045450e");
    }

    // =================================================================================================
    // T9 -- the per-player loop @0x00454447-0x004544b7. FIRST (0), one INTERIOR (3), and LAST (7) slot
    // all enabled (controller_flags != 0) with DISTINCT seeded fields per slot; every other slot left
    // disabled (controller_flags == 0, the reset() default) and must NOT produce a call. Also pins the
    // ORDER against the intra-slice session_state_reset: game_clock is seeded to a nonzero, distinctive
    // value (555.5), but session_state_reset's full-reset path (always taken, reset_flag==2) zeroes
    // game_clock BEFORE this loop reads it -- so every recorded call must show game_clock == 0.0, NOT
    // 555.5. If the intra-slice reset call were skipped or reordered, this would read 555.5 instead.
    // =================================================================================================
    {
        fx.reset();
        fx.net_local_player_slot     = 1; // disabled slot below, kept simple/unrelated to this case
        fx.net_lobby_scan_host_count = 1;
        fx.game_clock                = 555.5; // ORDER PIN -- see comment above

        fx.player_desc_slots[0].controller_flags = 0x7; // FIRST, enabled
        fx.player_desc_slots[0].race_or_faction  = 1;
        fx.player_desc_slots[0].color_or_team    = 11;
        fx.player_desc_slots[0].scenario_side_id = 100;
        std::strcpy(fx.player_desc_slots[0].name, "Alpha");

        fx.player_desc_slots[3].controller_flags = 0xB; // INTERIOR, enabled
        fx.player_desc_slots[3].race_or_faction  = 2;
        fx.player_desc_slots[3].color_or_team    = 22;
        fx.player_desc_slots[3].scenario_side_id = 200;
        std::strcpy(fx.player_desc_slots[3].name, "Bravo");

        fx.player_desc_slots[7].controller_flags = 0x5; // LAST, enabled
        fx.player_desc_slots[7].race_or_faction  = 3;
        fx.player_desc_slots[7].color_or_team    = 33;
        fx.player_desc_slots[7].scenario_side_id = 300;
        std::strcpy(fx.player_desc_slots[7].name, "Charlie");
        // slots 1, 2, 4, 5, 6 keep reset()'s default controller_flags == 0 (disabled).

        uint8_t blob[32] = {};
        run(fx, blob);

        ck_eq((uint32_t)g_profile_init_calls.size(), 3u,
              "T9a: exactly 3 profile_init calls -- one per enabled (controller_flags != 0) slot, "
              "disabled slots 1/2/4/5/6 produce none, 0x0045445b-0x00454466/0x004544b7");
        if (g_profile_init_calls.size() == 3) {
            const ProfileInitCall &a = g_profile_init_calls[0];
            const ProfileInitCall &b = g_profile_init_calls[1];
            const ProfileInitCall &c = g_profile_init_calls[2];

            ck_eq(a.player, 0u, "T9b: call 0 is player slot 0 (FIRST), ascending loop order");
            ck_eq(a.controller_flags, 0x7u, "T9c: slot0 controller_flags forwarded verbatim");
            ck_eq(a.race, 1u, "T9d: slot0 race_or_faction forwarded verbatim");
            ck_eq(a.color_index, 11u, "T9e: slot0 color_or_team forwarded verbatim");
            ck_eq((uint32_t)a.side_id, 100u, "T9f: slot0 scenario_side_id forwarded verbatim");
            ck(a.name == "Alpha", "T9g: slot0 name pointer forwards Players[0].name's own buffer");
            ck_eq_d(a.game_clock, 0.0,
                    "T9h: slot0 game_clock arg == 0.0 (post-intra-slice-reset), NOT the 555.5 seed "
                    "-- ORDER pin, 0x00454493-0x0045449d vs the intra-slice reset at 0x0045437f");

            ck_eq(b.player, 3u, "T9i: call 1 is player slot 3 (INTERIOR)");
            ck_eq(b.controller_flags, 0xBu, "T9j: slot3 controller_flags forwarded verbatim");
            ck_eq(b.race, 2u, "T9k: slot3 race_or_faction forwarded verbatim");
            ck_eq(b.color_index, 22u, "T9l: slot3 color_or_team forwarded verbatim");
            ck_eq((uint32_t)b.side_id, 200u, "T9m: slot3 scenario_side_id forwarded verbatim");
            ck(b.name == "Bravo", "T9n: slot3 name pointer forwards Players[3].name's own buffer");
            ck_eq_d(b.game_clock, 0.0, "T9o: slot3 game_clock arg == 0.0, same ORDER pin as T9h");

            ck_eq(c.player, 7u, "T9p: call 2 is player slot 7 (LAST)");
            ck_eq(c.controller_flags, 0x5u, "T9q: slot7 controller_flags forwarded verbatim");
            ck_eq(c.race, 3u, "T9r: slot7 race_or_faction forwarded verbatim");
            ck_eq(c.color_index, 33u, "T9s: slot7 color_or_team forwarded verbatim");
            ck_eq((uint32_t)c.side_id, 300u, "T9t: slot7 scenario_side_id forwarded verbatim");
            ck(c.name == "Charlie", "T9u: slot7 name pointer forwards Players[7].name's own buffer");
            ck_eq_d(c.game_clock, 0.0, "T9v: slot7 game_clock arg == 0.0, same ORDER pin as T9h");
        }
    }

    // =================================================================================================
    // T10 -- sim_active ORDER pin across the whole tail, PLUS the tutorial_step==0 positive arm for
    // update_planet_progress. sim_active is seeded to 7 (neither 0 nor 1, so each observation below
    // proves something rather than coincidentally matching a default), and four mocks capture the
    // value THEY see it hold: diplomacy_init_multiplayer fires BEFORE the first write (sees 7);
    // reload_snapshot_resync_clocks / progress_propagate_unlocks / update_planet_progress all fire
    // AFTER the first write and BEFORE the second (see 0); the final state is 1.
    // =================================================================================================
    {
        fx.reset();
        fx.net_local_player_slot      = 0;
        fx.net_lobby_scan_host_count  = 1;
        fx.tutorial_step              = 0;
        fx.sim_active                 = 7; // distinct sentinel
        g_net_lockstep_sync_delay_ret = 5; // non-negative: reach the tail undisturbed
        uint8_t blob[32]              = {};

        run(fx, blob);

        ck_eq((uint32_t)g_sim_active_at_diplomacy, 7u,
              "T10a: sim_active still the pre-call sentinel(7) when diplomacy_init_multiplayer fires "
              "-- that call precedes own.sim_active_mut()=0, 0x004544b9 vs 0x004544be");
        ck_eq((uint32_t)g_sim_active_at_resync, 0u,
              "T10b: sim_active==0 by reload_snapshot_resync_clocks (after 1st write, before 2nd), "
              "0x004544be vs 0x004544ca");
        ck_eq((uint32_t)g_sim_active_at_progress_unlk, 0u,
              "T10c: sim_active still 0 at progress_propagate_unlocks, same window");
        ck_eq((uint32_t)g_sim_active_at_update_prog, 0u,
              "T10d: sim_active still 0 at update_planet_progress (tutorial_step==0, so it fires), "
              "same window, 0x004544dd-0x004544eb");
        ck_eq((uint32_t)fx.sim_active, 1u,
              "T10e: sim_active==1 after return (2nd write, 0x004544f0) -- AFTER the 2nd "
              "ui_message_queue_clear_all and before the intra-slice land_players_on_planet");
        ck_eq((uint32_t)g_update_planet_progress_count, 1u,
              "T10f: tutorial_step==0 -> update_planet_progress DOES fire, 0x004544e6");
        ck_eq((uint32_t)g_diplomacy_init_mp_count, 1u,
              "T10g: diplomacy_init_multiplayer called exactly once, 0x004544b9");
        ck_eq((uint32_t)g_reload_snapshot_calls.size(), 1u,
              "T10h: reload_snapshot_resync_clocks called exactly once, 0x004544ca");
        if (!g_reload_snapshot_calls.empty()) {
            ck_eq_d(g_reload_snapshot_calls[0], 0.0,
                    "T10i: reload_snapshot_resync_clocks called with the LITERAL 0.0, NOT game_clock, "
                    "0x004544c8-0x004544ca");
        }
        ck_eq((uint32_t)g_progress_propagate_calls.size(), 1u,
              "T10j: progress_propagate_unlocks called exactly once, 0x004544d1-0x004544d8");
        if (!g_progress_propagate_calls.empty()) {
            ck_eq((uint32_t)g_progress_propagate_calls[0], (uint32_t)fx.player_side,
                  "T10k: progress_propagate_unlocks(PlayerSide), 0x004544d1-0x004544d8");
        }
        ck_eq((uint32_t)g_game_speed_recompute_count, 1u,
              "T10l: game_speed_recompute called exactly once, 0x00454504");
    }

    // =================================================================================================
    // T11 -- tutorial_step gate, NEGATIVE arm: nonzero tutorial_step suppresses update_planet_progress.
    // =================================================================================================
    {
        fx.reset();
        fx.net_local_player_slot      = 0;
        fx.net_lobby_scan_host_count  = 1;
        fx.tutorial_step              = 3; // nonzero
        g_net_lockstep_sync_delay_ret = 5;
        uint8_t blob[32]              = {};

        run(fx, blob);

        ck_eq((uint32_t)g_update_planet_progress_count, 0u,
              "T11: tutorial_step != 0 -> update_planet_progress NOT called, 0x004544dd-0x004544e6");
    }

    // =================================================================================================
    // T12 -- return-value/tail branch, SUCCESS arm (ret >= 0). Returns the stub's raw, DISTINCTIVE
    // result verbatim (777, not 0 or a truncated/boolean value), and arms lockstep_adapt_next_time =
    // game_clock (forced to 0.0 by the intra-slice reset, same ORDER fact as T9) + the adapt delay.
    // teardown/menu-return are NOT called on this arm.
    // =================================================================================================
    {
        fx.reset();
        fx.net_local_player_slot        = 0;
        fx.net_lobby_scan_host_count    = 1;
        fx.tutorial_step                = 0;
        fx.game_clock                   = 555.5; // forced to 0.0 by the intra-slice reset, same as T9
        fx.lockstep_session_adapt_delay = 2.5;   // distinct nonzero
        g_net_lockstep_sync_delay_ret   = 777;   // distinctive positive
        uint8_t blob[32]                = {};

        int32_t ret = run(fx, blob);

        ck_eq((uint32_t)ret, 777u,
              "T12a: success branch returns net_lockstep_sync_delay_stub's raw result verbatim, "
              "0x00454550-0x00454553");
        ck_eq_d(fx.lockstep_adapt_next_time, 2.5,
                "T12b: lockstep_adapt_next_time = game_clock(forced 0.0) + adapt_delay(2.5) = 2.5, "
                "0x00454525-0x00454531");
        ck_eq((uint32_t)g_menu_force_return_count, 0u,
              "T12d: menu_force_return_to_main NOT called on the success branch");
    }

    // =================================================================================================
    // T13 -- return-value/tail branch, ERROR arm (ret < 0 AND tutorial_step == 0). Returns the LITERAL
    // -1 (not the raw stub result), fires the teardown+menu EFFECTFUL WALL exactly once each, and does
    // NOT write lockstep_adapt_next_time (a pre-seeded sentinel must survive untouched).
    // =================================================================================================
    {
        fx.reset();
        fx.net_local_player_slot      = 0;
        fx.net_lobby_scan_host_count  = 1;
        fx.tutorial_step              = 0;
        fx.lockstep_adapt_next_time   = 123.456; // sentinel -- must survive this branch untouched
        g_net_lockstep_sync_delay_ret = -3;
        uint8_t blob[32]              = {};

        int32_t ret = run(fx, blob);

        ck_eq((uint32_t)ret, (uint32_t)-1,
              "T13a: error branch returns the LITERAL -1, NOT the raw stub result (-3), "
              "0x0045451c-0x0045453d");
        ck_eq((uint32_t)g_menu_force_return_count, 1u,
              "T13c: menu_force_return_to_main called exactly once (EFFECTFUL WALL), 0x00454544");
        ck_eq_d(fx.lockstep_adapt_next_time, 123.456,
                "T13d: lockstep_adapt_next_time NOT written on the error branch -- sentinel survives, "
                "0x00454525 skipped");
    }

    // =================================================================================================
    // T14 -- return-value/tail branch, the OTHER half of the `&&`: ret < 0 but tutorial_step != 0. The
    // error branch's condition needs BOTH; a nonzero tutorial_step defeats it even though ret is
    // negative, so this falls to the else branch -- raw ret returned (not -1), lockstep_adapt_next_time
    // IS written, and teardown/menu are NOT called. Distinguishes this from T13's arm cleanly.
    // =================================================================================================
    {
        fx.reset();
        fx.net_local_player_slot        = 0;
        fx.net_lobby_scan_host_count    = 1;
        fx.tutorial_step                = 9; // nonzero
        fx.lockstep_session_adapt_delay = 4.0;
        g_net_lockstep_sync_delay_ret   = -7;
        uint8_t blob[32]                = {};

        int32_t ret = run(fx, blob);

        ck_eq((uint32_t)ret, (uint32_t)-7,
              "T14a: tutorial_step != 0 defeats the error branch's `&&` even though ret<0 -- returns "
              "the RAW ret(-7), not -1, 0x00454516-0x0045451c/0x0045453d");
        ck_eq((uint32_t)g_menu_force_return_count, 0u,
              "T14c: menu_force_return_to_main NOT called, same reason");
        ck_eq_d(fx.lockstep_adapt_next_time, 0.0 + 4.0,
                "T14d: lockstep_adapt_next_time IS written on the else branch even though ret was "
                "negative, 0x00454525-0x00454531");
    }
}

} // namespace mh::sim::test
