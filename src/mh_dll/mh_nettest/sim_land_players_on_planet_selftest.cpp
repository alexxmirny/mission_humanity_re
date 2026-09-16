//
// sim_land_players_on_planet_selftest.cpp -- `simtest` offline oracle for
// llm_game_land_players_on_planet @0x0045534e (sim/resid/sim_land_players_on_planet.h/.cpp,
// RI-SIM sim_resid batch A/B). THE LARGEST BODY IN THIS SLICE (0x3aa bytes).
//
// NO SHADOW SITE (sim_resid rule 1) -- session-entry writer, runs once per new-game/planet-land.
// This offline oracle is the only verification (proof:OFFLINE).
//
// THE DOMAIN FACT THIS FILE MOST CARES ABOUT (see the module header's own banner): at
// 0x004553c1-0x004553cf the function tests `status_flags & HUMAN` (mask 0x4) and JZ; HUMAN bit
// CLEAR routes to the AI arm (0x0045550a), HUMAN bit SET falls through to the HUMAN arm
// (0x004553d5). ONLY the AI arm reaches llm_strat_spawn_ai_base. Getting the JZ sense backwards
// would silently swap which arm calls spawn_ai_base vs init_human_player_data -- every AI-arm and
// HUMAN-arm case below asserts BOTH that its own arm's call fired AND that the other arm's call did
// NOT, so that swap cannot pass.
//
// EXPECTED BEHAVIOUR, derived from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_game_land_players_on_planet_0045534e.asm) -- the .asm is the spec, never
// the .c beside it -- and cross-checked against the module header's own address citations.
//
#include "sim/resid/sim_land_players_on_planet.h"

#include <string>

#include "sim/sim_event_codes.h" // SESSION_SP
#include "sim_test_support.h"
#include "sim_resid_sibling_mocks.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// player_profile.status_flags bits -- TU-local copy, same values + same "why a local copy" reasoning
// the module .cpp gives for its own copy (sim_land_players_on_planet.cpp's anonymous namespace).
constexpr uint32_t SLOT_ENABLED = 0x1u;
constexpr uint32_t HUMAN_BIT    = 0x4u;
// game_e_race -- same value sim_land_players_on_planet.cpp's own RACE_ALIEN uses.
constexpr uint32_t RACE_ALIEN = 2u;

// ---- recorder -------------------------------------------------------------------------------------

struct claim_call {
    uint32_t player;
    uint32_t planet;
};
struct unit_create_call {
    uint32_t x, y;
    uint16_t unit, player;
    uint8_t  is_ship;
};
struct spawn_ai_call {
    int32_t player, is_alien, x, y;
};
struct init_human_call {
    uint32_t player;
    int32_t  is_alien;
};
struct land_dmp_call {
    void       *dst;
    std::string fmt, a0, a1;
    int32_t     a2, a3;
};
struct coord_msg_call {
    void   *dst;
    int32_t a0, a1;
};

struct recorder {
    std::vector<claim_call>       claim;
    std::vector<unit_create_call> unit_create;
    std::vector<spawn_ai_call>    spawn_ai;
    std::vector<init_human_call>  init_human;
    std::vector<land_dmp_call>    land_dmp;
    std::vector<coord_msg_call>   coord_msg;
    std::vector<int32_t>          cam_col;
    std::vector<int32_t>          cam_row;
    std::vector<uint32_t>         get_starting_unit_race_args;
    int32_t                       reroll_n              = 0;
    int32_t                       dirty_n               = 0;
    int32_t                       get_starting_unit_ret = 0;

    // ORDER HOOK (T-ORDER-AI below): when order_fx is set, the claim_landing_spot mock mutates that
    // fixture's landing_x/landing_y for (order_player, order_planet) to (order_new_x, order_new_y) --
    // proving the AI arm's x/y READ happens AFTER the claim call, not from a value cached before it.
    sim_fixture *order_fx     = nullptr;
    int32_t      order_player = -1, order_planet = -1, order_new_x = 0, order_new_y = 0;
};
recorder g_rec;

const land_players_on_planet_calls &recording_calls() {
    static const land_players_on_planet_calls c = {
        [](uint32_t player, uint32_t planet) -> int32_t {
            g_rec.claim.push_back({player, planet});
            if (g_rec.order_fx != nullptr && (int32_t)player == g_rec.order_player &&
                (int32_t)planet == g_rec.order_planet) {
                g_rec.order_fx->profiles[player].landing_x[planet] = g_rec.order_new_x;
                g_rec.order_fx->profiles[player].landing_y[planet] = g_rec.order_new_y;
            }
            return 0;
        },
        [](uint32_t x, uint32_t y, uint16_t unit, uint16_t player, uint8_t is_ship) -> uint32_t {
            g_rec.unit_create.push_back({x, y, unit, player, is_ship});
            return 1;
        },
        [](int32_t player, int32_t is_alien, int32_t x, int32_t y) {
            g_rec.spawn_ai.push_back({player, is_alien, x, y});
        },
        []() { ++g_rec.reroll_n; },
        [](uint32_t player_idx, int32_t is_alien_race) {
            g_rec.init_human.push_back({player_idx, is_alien_race});
        },
        [](int32_t col) { g_rec.cam_col.push_back(col); },
        [](int32_t row) { g_rec.cam_row.push_back(row); },
        []() { ++g_rec.dirty_n; },
        [](uint32_t race) -> int32_t {
            g_rec.get_starting_unit_race_args.push_back(race);
            return g_rec.get_starting_unit_ret;
        },
        [](void *dst, const char *format, const char *a0, const char *a1, int32_t a2,
           int32_t a3) -> int32_t {
            g_rec.land_dmp.push_back(
                {dst, format ? format : "", a0 ? a0 : "", a1 ? a1 : "", a2, a3});
            return 0;
        },
        [](void *dst, const wchar_t *format, int32_t a0, int32_t a1) -> int32_t {
            (void)format;
            g_rec.coord_msg.push_back({dst, a0, a1});
            return 0;
        },
    };
    return c;
}

} // namespace

void run_land_players_on_planet_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- outer-planet-landed latch fires: planet_index=10 is inside (6, 0x1f), flag starts 0 -> -1.
    // All players disabled (reset()'s default), isolating this latch from the loop.
    // 0x0045536e-0x00455387.
    // =================================================================================================
    {
        fx.reset();
        g_rec         = recorder{};
        sim_view  v   = fx.view();
        sim_store own = fx.store();
        sibling_rec() = sibling_record{};
        fx.map_width  = 100; // so the sibling's width_m/height_m writes are observable and DISTINCT
        fx.map_height = 40;
        own           = fx.store();
        v             = fx.view();
        detail::land_players_on_planet(v, own, recording_calls(), 10, mock_pmsi_calls());
        ck_eq((uint32_t)own.outer_planet_landed_flag(), 0xffffffffu,
              "T1: planet_index=10 in (6,0x1f), flag was 0 -> latched to -1, 0x00455387");

        // THE INTRA-SLICE SIBLING FIRES, and it is the FIRST thing this body does (0x00455369),
        // before the per-player loop starts. Added 2026-08-31 after a mutation that DELETED this
        // call outright turned NOTHING red across the whole 9201-check suite -- not this file's own
        // 22 cases, not the two composites above it. A composite oracle that cannot see a dropped
        // sibling is not testing the composite.
        //
        // Both halves are asserted because either alone is weak: the counter proves the edge was
        // taken, the width_m/height_m values prove the sibling actually RAN (a table bound to
        // no-ops would satisfy the counter and nothing else). map_width-1 / map_height-1 is the
        // sibling's own documented contract, see sim_planet_map_session_init.cpp.
        ck_eq((uint32_t)sibling_rec().pmsi_bldg_recompute_cell_grid, 1u,
              "T1: planet_map_session_init reached exactly once, 0x00455369");
        ck_eq((uint32_t)own.width_m_mut(), 99u,
              "T1: ... and it RAN -- width_m = map_width - 1");
        ck_eq((uint32_t)own.height_m_mut(), 39u,
              "T1: ... and it RAN -- height_m = map_height - 1");
    }

    // =================================================================================================
    // T2 -- lower boundary: planet_index=6 -> JLE fires (<=6 skips the latch entirely), flag stays 0.
    // 0x00455372.
    // =================================================================================================
    {
        fx.reset();
        g_rec         = recorder{};
        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::land_players_on_planet(v, own, recording_calls(), 6, mock_pmsi_calls());
        ck_eq((uint32_t)own.outer_planet_landed_flag(), 0u,
              "T2: planet_index=6 (boundary, JLE) -> latch skipped, flag stays 0, 0x00455372");
    }

    // =================================================================================================
    // T3 -- upper boundary inclusive: planet_index=0x1e (30) -> JL fires (<0x1f), flag latches to -1.
    // 0x00455378.
    // =================================================================================================
    {
        fx.reset();
        g_rec         = recorder{};
        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::land_players_on_planet(v, own, recording_calls(), 0x1e, mock_pmsi_calls());
        ck_eq((uint32_t)own.outer_planet_landed_flag(), 0xffffffffu,
              "T3: planet_index=0x1e (boundary, still <0x1f) -> latched to -1, 0x00455378");
    }

    // =================================================================================================
    // T4 -- upper boundary exclusive: planet_index=0x1f (31) -> JL fails (31 not <0x1f), the LANDED
    // flag (distinct from the LAND_STATE latch at the tail, see T21) stays 0. 0x00455378.
    // =================================================================================================
    {
        fx.reset();
        g_rec         = recorder{};
        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::land_players_on_planet(v, own, recording_calls(), 0x1f, mock_pmsi_calls());
        ck_eq((uint32_t)own.outer_planet_landed_flag(), 0u,
              "T4: planet_index=0x1f -> the LANDED latch's range is exclusive of 0x1f, stays 0, "
              "0x00455378 (distinct from the LAND_STATE latch tested in T21)");
    }

    // =================================================================================================
    // T5 -- sentinel preserve: flag pre-set to a nonzero 5 at planet_index=10 (in-range) -> the JZ gate
    // at 0x00455383 means it is NOT blindly overwritten to -1 and NOT reset to 0; it stays 5.
    // =================================================================================================
    {
        fx.reset();
        g_rec                          = recorder{};
        sim_store own                  = fx.store();
        own.outer_planet_landed_flag() = 5;
        sim_view v                     = fx.view();
        detail::land_players_on_planet(v, own, recording_calls(), 10, mock_pmsi_calls());
        ck_eq((uint32_t)own.outer_planet_landed_flag(), 5u,
              "T5: flag pre-set to 5 (nonzero) -> gate at 0x00455383 only fires when flag==0, stays "
              "5, not blindly assigned -1");
    }

    // =================================================================================================
    // T6/T7 -- the two calls that fire UNCONDITIONALLY every invocation, regardless of planet_index or
    // player roster: llm_strat_landing_spots_reroll_out_of_bounds (0x00455391, right after the LANDED
    // latch, before the player loop) and llm_map_cam_mark_viewport_dirty (0x004556e9, the very last
    // call). All 8 players disabled (reset()'s default) so nothing else can produce these calls --
    // a naive oracle that only checked these behind the per-player loop would pass vacuously here.
    // =================================================================================================
    {
        fx.reset();
        g_rec         = recorder{};
        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::land_players_on_planet(v, own, recording_calls(), 0, mock_pmsi_calls());
        ck_eq((uint32_t)g_rec.reroll_n, 1u,
              "T6: landing_spots_reroll_out_of_bounds fires unconditionally, 0x00455391 (all players "
              "disabled, nothing else in the body could have produced this call)");
        ck_eq((uint32_t)g_rec.dirty_n, 1u,
              "T7: cam_mark_viewport_dirty fires unconditionally at the tail, 0x004556e9 (same "
              "all-disabled scenario)");
    }

    // =================================================================================================
    // T8 -- disabled slot skip: player 3's status_flags has SLOT_ENABLED clear -- the ENTIRE per-player
    // body is skipped for it, including the primary_mother_unit tail mark that runs for every ENABLED
    // player regardless of arm. 0x004553b4/0x004553bb.
    // =================================================================================================
    {
        fx.reset();
        g_rec                       = recorder{};
        fx.profiles[3].status_flags = 0; // disabled (bit0 clear)
        fx.profiles[3].race         = RACE_ALIEN;
        fx.profiles[3].landing_x[0] = 999;
        fx.profiles[3].landing_y[0] = 888;
        sim_view  v                 = fx.view();
        sim_store own               = fx.store();
        detail::land_players_on_planet(v, own, recording_calls(), 0, mock_pmsi_calls());
        ck_eq((uint32_t)g_rec.claim.size(), 0u,
              "T8: disabled slot 3 -> claim_landing_spot never called for it, 0x004553b4/0x004553bb");
        ck_eq((uint32_t)own.profile_at(3).primary_mother_unit[0], 0u,
              "T8: disabled slot 3 -> primary_mother_unit[0] NOT set, the tail mark at 0x004555ab "
              "never runs for a disabled slot either");
    }

    // =================================================================================================
    // T9 -- AI ARM, LOW slot (player=0, first loop iteration). HUMAN bit clear -> AI arm
    // (0x0045550a..0x00455599). session_mode=0 (!=SP) makes the eligibility gate bypass unconditionally
    // (0x0045550a-0x00455519). race=ALIEN -> is_alien=1 (0x0045556b-0x00455584). Distinct landing_x/y
    // so a wrong pair/order disagrees. init_human_player_data must NOT fire for this player -- the
    // domain-fact pin: only the AI arm reaches spawn_ai_base.
    // =================================================================================================
    {
        fx.reset();
        g_rec                        = recorder{};
        fx.session_mode              = 0;
        fx.profiles[0].status_flags  = SLOT_ENABLED; // AI (HUMAN bit clear)
        fx.profiles[0].race          = RACE_ALIEN;
        fx.profiles[0].landing_x[15] = 111;
        fx.profiles[0].landing_y[15] = 222;
        sim_view  v                  = fx.view();
        sim_store own                = fx.store();
        detail::land_players_on_planet(v, own, recording_calls(), 15, mock_pmsi_calls());

        ck_eq((uint32_t)g_rec.claim.size(), 1u, "T9: AI arm calls claim_landing_spot once, 0x00455536");
        ck_eq(g_rec.claim[0].player, 0u, "T9: claim_landing_spot's player arg is 0 (first slot)");
        ck_eq(g_rec.claim[0].planet, 15u,
              "T9: claim_landing_spot's planet arg is this function's OWN planet_index param (15)");
        ck_eq((uint32_t)g_rec.spawn_ai.size(), 1u,
              "T9: AI arm (HUMAN bit clear) calls spawn_ai_base, 0x00455597");
        ck((int32_t)g_rec.spawn_ai[0].player == 0 && g_rec.spawn_ai[0].is_alien == 1 &&
               g_rec.spawn_ai[0].x == 111 && g_rec.spawn_ai[0].y == 222,
           "T9: spawn_ai_base(player=0, is_alien=1 [race==ALIEN], x=111, y=222), 0x0045553b-0x00455597");
        ck_eq((uint32_t)g_rec.init_human.size(), 0u,
              "T9 DOMAIN-FACT PIN: HUMAN bit clear -> init_human_player_data must NOT fire (only the "
              "AI arm's spawn_ai_base fires); a reversed JZ sense at 0x004553cf would call this instead");
        ck_eq((uint32_t)own.profile_at(0).primary_mother_unit[15], 1u,
              "T9: primary_mother_unit[15] set for the AI arm too (both arms converge), 0x004555ab");
    }

    // =================================================================================================
    // T10 -- AI ARM, HIGH slot (player=7, LAST loop iteration). Distinct values from T9: race != ALIEN
    // -> is_alien=0 (the OTHER side of the AI-arm race test), distinct x/y.
    // =================================================================================================
    {
        fx.reset();
        g_rec                        = recorder{};
        fx.session_mode              = 0;
        fx.profiles[7].status_flags  = SLOT_ENABLED; // AI
        fx.profiles[7].race          = 1;            // != RACE_ALIEN -> is_alien=0
        fx.profiles[7].landing_x[15] = 333;
        fx.profiles[7].landing_y[15] = 444;
        sim_view  v                  = fx.view();
        sim_store own                = fx.store();
        detail::land_players_on_planet(v, own, recording_calls(), 15, mock_pmsi_calls());

        ck_eq((uint32_t)g_rec.claim.size(), 1u, "T10: AI arm calls claim_landing_spot once for slot 7");
        ck_eq(g_rec.claim[0].player, 7u, "T10: claim_landing_spot's player arg is 7 (last slot)");
        ck_eq((uint32_t)g_rec.spawn_ai.size(), 1u, "T10: AI arm calls spawn_ai_base for the last slot too");
        ck((int32_t)g_rec.spawn_ai[0].player == 7 && g_rec.spawn_ai[0].is_alien == 0 &&
               g_rec.spawn_ai[0].x == 333 && g_rec.spawn_ai[0].y == 444,
           "T10: spawn_ai_base(player=7, is_alien=0 [race!=ALIEN], x=333, y=444), 0x0045556b-0x00455597");
        ck_eq((uint32_t)g_rec.init_human.size(), 0u,
              "T10: HUMAN bit clear on the LAST slot too -> init_human_player_data still does not fire");
        ck_eq((uint32_t)own.profile_at(7).primary_mother_unit[15], 1u,
              "T10: primary_mother_unit[15] set for slot 7, 0x004555ab");
    }

    // =================================================================================================
    // T-ORDER-AI -- proves the AI arm's landing_x/landing_y READ happens AFTER claim_landing_spot
    // (0x00455536), not from a value cached before it: seed STALE x/y, have the claim mock overwrite
    // them with NEW values when called, and assert spawn_ai_base receives the NEW ones. A translation
    // that read x/y before calling claim (or cached them) would see the stale 999/888 here instead.
    // 0x00455536 (claim) then 0x0045553b-0x00455568 (re-read x/y).
    // =================================================================================================
    {
        fx.reset();
        g_rec                       = recorder{};
        fx.session_mode             = 0;
        fx.profiles[2].status_flags = SLOT_ENABLED;
        fx.profiles[2].race         = RACE_ALIEN;
        fx.profiles[2].landing_x[8] = 999; // STALE -- must not be what spawn_ai_base sees
        fx.profiles[2].landing_y[8] = 888; // STALE
        g_rec.order_fx              = &fx;
        g_rec.order_player          = 2;
        g_rec.order_planet          = 8;
        g_rec.order_new_x           = 55;
        g_rec.order_new_y           = 66;
        sim_view  v                 = fx.view();
        sim_store own               = fx.store();
        detail::land_players_on_planet(v, own, recording_calls(), 8, mock_pmsi_calls());

        ck_eq((uint32_t)g_rec.spawn_ai.size(), 1u, "T-ORDER-AI: spawn_ai_base fired once");
        ck(g_rec.spawn_ai[0].x == 55 && g_rec.spawn_ai[0].y == 66,
           "T-ORDER-AI: spawn_ai_base got the POST-claim x/y (55,66), not the stale pre-claim (999,888) "
           "-- proves the read at 0x0045553b-0x00455568 happens AFTER claim_landing_spot @0x00455536, "
           "not before/cached");
    }

    // =================================================================================================
    // T11 -- ELIGIBILITY: the "two DIFFERENT globals" pin. session_mode==SESSION_SP and player(5)>=2,
    // so the enemy compare at 0x0045551b-0x0045552e applies. The compare indexes cfg_planets by
    // G_PLANET_INDEX (fx.planet_index, the UNRELATED "current planet" global) -- NOT by this
    // function's OWN planet_index parameter. G_PLANET_INDEX=9 has enemy=2 (player 5 > 2 -> NOT
    // eligible); the function's OWN planet_index param is a DECOY 30, whose cfg_planets[30].enemy=100
    // would make player 5 eligible if the wrong index were used. claim/spawn_ai_base must NOT fire,
    // but the tail mark (indexed by the function's OWN planet_index=30) still does.
    // =================================================================================================
    {
        fx.reset();
        g_rec                   = recorder{};
        fx.session_mode         = SESSION_SP;
        fx.planet_index         = 9; // G_PLANET_INDEX -- the REAL index the gate reads
        fx.cfg_planets[9].enemy = 2; // player(5) > 2 -> NOT eligible
        // DECOY at the function's own param -- reading THIS instead of G_PLANET_INDEX would
        // wrongly pass. Slot 30, not 40: `Planets` is 32 slots (PLANET_COUNT 0x20) and so is
        // llm_strat_player_profile::landing_x[32], so a param of 40 indexes past BOTH -- ASan
        // called it, a heap-buffer-overflow 9558 bytes past the cfg_planets allocation. 30 is
        // in range, still != G_PLANET_INDEX(9), and still inside the (6, 0x1f) latch window.
        fx.cfg_planets[30].enemy    = 100;
        fx.profiles[5].status_flags = SLOT_ENABLED; // AI
        sim_view  v                 = fx.view();
        sim_store own               = fx.store();
        detail::land_players_on_planet(v, own, recording_calls(), 30, mock_pmsi_calls()); // function's OWN planet_index

        ck_eq((uint32_t)g_rec.claim.size(), 0u,
              "T11: G_PLANET_INDEX(9).enemy=2 < player(5) -> NOT eligible, claim_landing_spot skipped, "
              "0x0045552e (reading cfg_planets[30] -- the decoy -- would wrongly pass)");
        ck_eq((uint32_t)g_rec.spawn_ai.size(), 0u, "T11: not eligible -> spawn_ai_base also skipped");
        ck_eq((uint32_t)own.profile_at(5).primary_mother_unit[30], 1u,
              "T11: the tail mark still runs (indexed by the function's OWN planet_index=30) even "
              "though the AI arm's claim/spawn were skipped, 0x0045559c-0x004555ab");
    }

    // =================================================================================================
    // T12 -- ELIGIBILITY boundary, equal case: player(5) <= G_PLANET_INDEX's enemy(5) -> eligible.
    // Same decoy setup as T11 but enemy raised to exactly 5.
    // =================================================================================================
    {
        fx.reset();
        g_rec                       = recorder{};
        fx.session_mode             = SESSION_SP;
        fx.planet_index             = 9;
        fx.cfg_planets[9].enemy     = 5; // player(5) <= 5 -> eligible (boundary)
        fx.profiles[5].status_flags = SLOT_ENABLED;
        sim_view  v                 = fx.view();
        sim_store own               = fx.store();
        detail::land_players_on_planet(v, own, recording_calls(), 30, mock_pmsi_calls());

        ck_eq((uint32_t)g_rec.claim.size(), 1u,
              "T12: player(5) <= enemy(5) -> eligible (boundary, not strict <), 0x0045552e");
    }

    // =================================================================================================
    // T13 -- ELIGIBILITY bypass via player<2: session_mode==SP, player=1 (<2), enemy=0 (would FAIL
    // player<=enemy if that branch were reached) -- but player<2 short-circuits the enemy compare
    // entirely, so this is still eligible. 0x00455513-0x00455519.
    // =================================================================================================
    {
        fx.reset();
        g_rec                       = recorder{};
        fx.session_mode             = SESSION_SP;
        fx.planet_index             = 3;
        fx.cfg_planets[3].enemy     = 0; // 1<=0 is false -- must NOT be reached
        fx.profiles[1].status_flags = SLOT_ENABLED;
        sim_view  v                 = fx.view();
        sim_store own               = fx.store();
        detail::land_players_on_planet(v, own, recording_calls(), 12, mock_pmsi_calls());

        ck_eq((uint32_t)g_rec.claim.size(), 1u,
              "T13: player(1)<2 bypasses the enemy compare entirely -> eligible even though "
              "player<=enemy(0) would be false, 0x00455513-0x00455519");
    }

    // =================================================================================================
    // T14 -- HUMAN ARM, get_starting_unit returns 0 -> game_land_no_start_unit_flag set, NO unit_create.
    // Also the domain-fact pin from the HUMAN side: spawn_ai_base must NOT fire. race=ALIEN -> DMP race
    // letter is the single character "A" (not the word "ALIEN"). session_mode=0 makes
    // try_starting_unit unconditionally true (bypasses the SP-specific system check).
    // =================================================================================================
    {
        fx.reset();
        g_rec                                 = recorder{};
        fx.session_mode                       = 0;
        fx.profiles[1].status_flags           = SLOT_ENABLED | HUMAN_BIT;
        fx.profiles[1].race                   = RACE_ALIEN;
        fx.profiles[1].landing_spot_index[20] = 77;
        g_rec.get_starting_unit_ret           = 0;
        sim_view  v                           = fx.view();
        sim_store own                         = fx.store();
        detail::land_players_on_planet(v, own, recording_calls(), 20, mock_pmsi_calls());

        ck_eq((uint32_t)g_rec.claim.size(), 1u, "T14: HUMAN arm always calls claim_landing_spot, 0x004553db");
        ck_eq((uint32_t)g_rec.spawn_ai.size(), 0u,
              "T14 DOMAIN-FACT PIN: HUMAN bit set -> spawn_ai_base must NOT fire");
        ck_eq((uint32_t)g_rec.get_starting_unit_race_args.size(), 1u,
              "T14: get_starting_unit called once, 0x00455464");
        ck_eq(g_rec.get_starting_unit_race_args[0], RACE_ALIEN,
              "T14: get_starting_unit's race arg is profiles[1].race");
        ck_eq((uint32_t)own.game_land_no_start_unit_flag(), 1u,
              "T14: starting_unit==0 -> game_land_no_start_unit_flag=1, 0x004554d0");
        ck_eq((uint32_t)g_rec.unit_create.size(), 0u,
              "T14: starting_unit==0 -> unit_create is NOT called, 0x00455470");
        ck_eq((uint32_t)g_rec.land_dmp.size(), 1u, "T14: the DMP-path sprintf fires, 0x0045542d");
        const land_dmp_call &dmp = g_rec.land_dmp[0];
        ck((void *)dmp.dst == (void *)fx.land_dmp_scratch.data(),
           "T14: DMP sprintf writes into own.land_dmp_scratch() (0x00e58146), DISTINCT from the "
           "already-registered dmp_path_scratch (0x00e58245)");
        ck(dmp.fmt == "%s%s_%02d%02d.DMP", "T14: DMP sprintf format string, 0x00501041");
        ck(dmp.a0 == "init\\", "T14: DMP sprintf a0 = \"init\\\\\", 0x005d05f0");
        ck(dmp.a1 == "A",
           "T14: DMP sprintf a1 is the ONE-CHARACTER race letter \"A\" for ALIEN, NOT the word "
           "\"ALIEN\" -- 0x0050103d");
        ck_eq((uint32_t)dmp.a2, 20u, "T14: DMP sprintf a2 = this function's OWN planet_index param (20)");
        ck_eq((uint32_t)dmp.a3, 77u,
              "T14: DMP sprintf a3 = landing_spot_index[planet_index] (77), distinct from a2 so a "
              "swapped pair fails");
        ck_eq((uint32_t)g_rec.init_human.size(), 1u,
              "T14: init_human_player_data still fires at the tail of the HUMAN arm, 0x00455500");
        ck(g_rec.init_human[0].player == 1 && g_rec.init_human[0].is_alien == 1,
           "T14: init_human_player_data(player=1, is_alien=1) -- re-derived race check, 0x004554da");
    }

    // =================================================================================================
    // T15 -- HUMAN ARM, get_starting_unit returns nonzero -> unit_create fires with is_ship==THE
    // LITERAL 2 (0x00455472-0x00455477 pushes immediate 2, NOT a boolean true==1 -- the module header's
    // own PRESERVE-BUG-shaped correction note). race != ALIEN -> DMP letter "H". player(6) !=
    // player_side (default 7) -> the ctrl-group branch must NOT fire.
    // =================================================================================================
    {
        fx.reset();
        g_rec                        = recorder{};
        fx.session_mode              = 0;
        fx.profiles[6].status_flags  = SLOT_ENABLED | HUMAN_BIT;
        fx.profiles[6].race          = 1; // != ALIEN -> letter "H"
        fx.profiles[6].landing_x[20] = 501;
        fx.profiles[6].landing_y[20] = 602;
        g_rec.get_starting_unit_ret  = 42; // nonzero starting unit id
        sim_view  v                  = fx.view();
        sim_store own                = fx.store();
        detail::land_players_on_planet(v, own, recording_calls(), 20, mock_pmsi_calls());

        ck_eq((uint32_t)own.game_land_no_start_unit_flag(), 0u,
              "T15: starting_unit!=0 -> the no-start-unit flag is NOT set");
        ck_eq((uint32_t)g_rec.unit_create.size(), 1u, "T15: unit_create fires, 0x004554aa");
        const unit_create_call &uc = g_rec.unit_create[0];
        ck(uc.x == 501 && uc.y == 602 && uc.unit == 42 && uc.player == 6,
           "T15: unit_create(x=501, y=602, unit=42 [starting_unit], player=6)");
        ck_eq((uint32_t)uc.is_ship, 2u,
              "T15 PIN: is_ship is the LITERAL 2 the assembly pushes at 0x00455472-0x00455477 -- NOT "
              "boolean true(1); the Ghidra .c draft's `true` rendering is the wrong bit pattern");
        ck(g_rec.land_dmp.size() == 1 && g_rec.land_dmp[0].a1 == "H",
           "T15: DMP sprintf a1 is \"H\" for a non-ALIEN race, 0x0050103f");
        ck_eq((uint32_t)own.ctrl_group_at(0).count, 0u,
              "T15: player(6) != PlayerSide(default 7) -> ctrl_group_at(0) untouched, 0x004554b9");
    }

    // =================================================================================================
    // T16 -- HUMAN ARM, the ctrl-group branch: player == PlayerSide -> ctrl_group_at(0) gets count=1,
    // unit_ids[0]=1. Interior slot (player=4), exercising the loop's middle rather than an edge.
    // =================================================================================================
    {
        fx.reset();
        g_rec                        = recorder{};
        fx.session_mode              = 0;
        fx.player_side               = 4; // == the player under test
        fx.profiles[4].status_flags  = SLOT_ENABLED | HUMAN_BIT;
        fx.profiles[4].race          = 1;
        fx.profiles[4].landing_x[20] = 71;
        fx.profiles[4].landing_y[20] = 82;
        g_rec.get_starting_unit_ret  = 9;
        sim_view  v                  = fx.view();
        sim_store own                = fx.store();
        detail::land_players_on_planet(v, own, recording_calls(), 20, mock_pmsi_calls());

        ck_eq((uint32_t)g_rec.claim.size(), 1u, "T16: HUMAN arm, interior slot 4 -- claim fires");
        ck_eq(g_rec.claim[0].player, 4u, "T16: claim's player arg is 4 (interior slot)");
        ck_eq((uint32_t)own.ctrl_group_at(0).count, 1u,
              "T16: player(4) == PlayerSide(4) -> ctrl_group_at(0).count=1, 0x004554bb");
        ck_eq((uint32_t)own.ctrl_group_at(0).unit_ids[0], 1u,
              "T16: ctrl_group_at(0).unit_ids[0]=1, 0x004554c5");
        ck_eq((uint32_t)own.profile_at(4).primary_mother_unit[20], 1u,
              "T16: primary_mother_unit[20] set for the interior slot too, 0x004555ab");
    }

    // =================================================================================================
    // T17 -- try_starting_unit gate, FALSE arm: session_mode==SP AND this function's OWN planet_index
    // (99) != system_define_index_base[current_system*35+4] (77) -> get_starting_unit/unit_create are
    // SKIPPED ENTIRELY, but init_human_player_data (outside this conditional) still fires.
    // 0x00455435-0x00455451.
    // =================================================================================================
    {
        fx.reset();
        g_rec                                           = recorder{};
        fx.session_mode                                 = SESSION_SP;
        fx.current_system                               = 2;
        fx.system_define_index_base[2 * (0x8c / 4) + 4] = 77; // != planet_index param (99)
        fx.profiles[1].status_flags                     = SLOT_ENABLED | HUMAN_BIT;
        fx.profiles[1].race                             = RACE_ALIEN;
        sim_view  v                                     = fx.view();
        sim_store own                                   = fx.store();
        detail::land_players_on_planet(v, own, recording_calls(), 99, mock_pmsi_calls());

        ck_eq((uint32_t)g_rec.get_starting_unit_race_args.size(), 0u,
              "T17: planet_index(99) != system_define_index_base[...] (77) -> get_starting_unit "
              "SKIPPED, 0x00455451");
        ck_eq((uint32_t)g_rec.unit_create.size(), 0u, "T17: unit_create also skipped");
        ck_eq((uint32_t)own.game_land_no_start_unit_flag(), 0u,
              "T17: no-start-unit flag not touched either -- the whole block was skipped, not entered "
              "and found starting_unit==0");
        ck_eq((uint32_t)g_rec.init_human.size(), 1u,
              "T17: init_human_player_data STILL fires -- it is outside the try_starting_unit "
              "conditional, 0x00455500");
    }

    // =================================================================================================
    // T18 -- try_starting_unit gate, TRUE arm via the SP boundary match: session_mode==SP AND
    // planet_index(55) == system_define_index_base[current_system*35+4] (55) -> get_starting_unit DOES
    // fire.
    // =================================================================================================
    {
        fx.reset();
        g_rec                                           = recorder{};
        fx.session_mode                                 = SESSION_SP;
        fx.current_system                               = 3;
        fx.system_define_index_base[3 * (0x8c / 4) + 4] = 55; // == planet_index param (55)
        fx.profiles[1].status_flags                     = SLOT_ENABLED | HUMAN_BIT;
        g_rec.get_starting_unit_ret                     = 0;
        sim_view  v                                     = fx.view();
        sim_store own                                   = fx.store();
        detail::land_players_on_planet(v, own, recording_calls(), 55, mock_pmsi_calls());

        ck_eq((uint32_t)g_rec.get_starting_unit_race_args.size(), 1u,
              "T18: planet_index(55) == system_define_index_base[current_system*35+4] (55) -> "
              "get_starting_unit fires, 0x0045544b/0x00455457 (SYSTEM_FIELD_INDEX_4 boundary match)");
    }

    // =================================================================================================
    // T19 -- the camera-centering + coord-message tail, unconditional and independent of the player
    // loop (all 8 slots disabled here). PlayerSide and LOCAL_PLAYER_SLOT are DELIBERATELY DIFFERENT
    // slots (3 vs 5) with DIFFERENT landing coordinates, so consuming the wrong slot in either
    // consumer fails. The two "out-pointer pairs" (col from x/win_w/width_mask, row from
    // y/win_h/height_mask) use DIFFERENT divisors and DIFFERENT masks (geom defaults: width_mask=0xff,
    // height_mask=0x3f; win_w=640, win_h=480) so consuming the wrong half of either pair fails too.
    // =================================================================================================
    {
        fx.reset();
        g_rec                        = recorder{};
        fx.player_side               = 3;
        fx.local_player_slot         = 5;
        fx.profiles[3].landing_x[18] = 700; // PlayerSide's spot -- feeds the camera pan + UI marker
        fx.profiles[3].landing_y[18] = 300;
        fx.profiles[5].landing_x[18] = 111; // LOCAL_PLAYER_SLOT's spot -- feeds the coord debug text
        fx.profiles[5].landing_y[18] = 222;
        sim_view  v                  = fx.view();
        sim_store own                = fx.store();
        detail::land_players_on_planet(v, own, recording_calls(), 18, mock_pmsi_calls());

        ck_eq((uint32_t)g_rec.coord_msg.size(), 1u, "T19: coord_msg_sprintf fires once, 0x00455606");
        ck((void *)g_rec.coord_msg[0].dst == (void *)fx.text_scratch.data(),
           "T19: coord_msg_sprintf writes into own.text_scratch(), G_TEXT_TMP");
        ck(g_rec.coord_msg[0].a0 == 111 && g_rec.coord_msg[0].a1 == 222,
           "T19: coord_msg uses LOCAL_PLAYER_SLOT(5)'s landing spot (111,222), NOT PlayerSide(3)'s "
           "(700,300) -- 0x004555c4-0x004555fa");

        ck_eq((uint32_t)g_rec.cam_col.size(), 1u, "T19: cam_set_col fires once, 0x00455649");
        const int32_t expect_col = (700 - (640 / 64)) & 0xff; // PlayerSide's x, win_w, width_mask
        ck_eq((uint32_t)g_rec.cam_col[0], (uint32_t)expect_col,
              "T19: cam_set_col uses PlayerSide(3)'s x=700, win_w=640 (/64 trunc), width_mask=0xff, "
              "0x0045560e-0x00455649 (NOT LOCAL_PLAYER_SLOT's x, and NOT the y/height pair)");

        ck_eq((uint32_t)g_rec.cam_row.size(), 1u, "T19: cam_set_row fires once, 0x00455689");
        const int32_t expect_row = (300 - (480 / 64)) & 0x3f; // PlayerSide's y, win_h, height_mask
        ck_eq((uint32_t)g_rec.cam_row[0], (uint32_t)expect_row,
              "T19: cam_set_row uses PlayerSide(3)'s y=300, win_h=480 (/64 trunc), height_mask=0x3f "
              "(DIFFERENT divisor+mask from cam_set_col), 0x0045564e-0x00455689");

        ck_eq((uint32_t)own.ui_base_marker_coords_at(0).cam_col, 700u,
              "T19: ui_base_marker_coords[0].cam_col = PlayerSide's x UNWRAPPED (no mask/subtract), "
              "0x004556a3-0x004556a9");
        ck_eq((uint32_t)own.ui_base_marker_coords_at(0).cam_row, 300u,
              "T19: ui_base_marker_coords[0].cam_row = PlayerSide's y UNWRAPPED too, 0x004556c3-0x004556c9");
    }

    // =================================================================================================
    // T20a/b/c -- the SECOND outer-planet latch, at the tail: outer_planet_land_state, planet_index
    // ==0x1f ONLY (distinct condition from T1-T5's outer_planet_landed_flag, which is planets 7..0x1e).
    // =================================================================================================
    {
        fx.reset();
        g_rec         = recorder{};
        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::land_players_on_planet(v, own, recording_calls(), 0x1f, mock_pmsi_calls());
        ck_eq((uint32_t)own.outer_planet_land_state(), 0xffffffffu,
              "T20a: planet_index==0x1f, land_state was 0 -> latched to -1, 0x004556df");
    }
    {
        fx.reset();
        g_rec         = recorder{};
        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::land_players_on_planet(v, own, recording_calls(), 0x1e, mock_pmsi_calls());
        ck_eq((uint32_t)own.outer_planet_land_state(), 0u,
              "T20b: planet_index==0x1e (!=0x1f) -> land_state latch does NOT fire, stays 0, 0x004556ce");
    }
    {
        fx.reset();
        g_rec                         = recorder{};
        sim_store own                 = fx.store();
        own.outer_planet_land_state() = 7; // nonzero sentinel
        sim_view v                    = fx.view();
        detail::land_players_on_planet(v, own, recording_calls(), 0x1f, mock_pmsi_calls());
        ck_eq((uint32_t)own.outer_planet_land_state(), 7u,
              "T20c: land_state pre-set to 7 (nonzero) at planet_index==0x1f -> gate only fires when "
              "==0, stays 7, not blindly overwritten, 0x004556db");
    }
}

} // namespace mh::sim::test
