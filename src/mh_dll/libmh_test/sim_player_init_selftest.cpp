//
// sim_player_init_selftest.cpp -- `simtest` oracle for llm_strat_player_profile_init @0x00454985,
// llm_strat_player_param_defaults_init @0x00455a84, and llm_strat_init_human_player_data @0x004dd91d
// (sim/resid/sim_player_init.h/.cpp, RI-SIM / sim_resid batch C).
//
// ---- NO SHADOW SITE (sim_resid rule 1) ----
// Session-entry writers, offline-oracle-only. This file IS the evidence.
//
// EXPECTED BEHAVIOUR, from the .asm (never the sibling .c -- it has lied on this project before,
// e.g. rendering `status |= 0x40` as a pointer into a struct field) and from the header banner:
//
// llm_strat_player_profile_init @0x00454985:
//   0x004549bc-0x004549d2: UNBOUNDED byte-pair copy name_str -> profile.name (offset 0x714).
//     PRESERVE-BUG: no clamp to the 32-byte array.
//   0x004549d4-0x004549f2: strlen(profile.name) via the freshly written copy.
//   0x004549f3/f6: CMP ECX,0xc / JBE -- length <= 12 skips truncation (boundary from BOTH sides:
//     12 does NOT truncate, 13 DOES).
//   0x004549ff: name[9] = 0. 0x00454a06: suffix source = s_..._00501039, memory-read-CONFIRMED this
//     session to be the literal bytes 2E 2E 2E 00 = "...". 0x00454a28-0x00454a3e: suffix copied to
//     name+9, so a truncated name reads "<first 9 chars>...".
//   0x00454a41-0x00454a4b: status_flags = controller_flags (offset 0x0).
//   0x00454a51-0x00454a5b: race = race (offset 0x4).
//   0x00454a61-0x00454a6b: color_index = color_index (offset 0x8).
//   0x00454a71-0x00454a80: flag_sprite_id = color_index + 0x453f (offset 0xc; 0x453f is an immediate
//     operand baked into the instruction).
//   0x00454a86-0x00454a8c: llm_strat_player_set_color(player, color_index) called exactly once.
//   0x00454a91-0x00454aa4: prod_check_clock = game_clock (offset 0x734; two plain 32-bit MOVs, no
//     x87 involved).
//   0x00454aaa-0x00454ab1: mother_established = 0 (offset 0x190), unconditionally.
//   0x00454abb-0x00454ac5: side_id = side_id (offset 0x73c).
//   0x00454acb-0x00454bf5: PRESERVE-BUG -- the eleven-array reset loop runs i = 1..31, NEVER touching
//     index 0, over prod_queue_slot/primary_mother_bldg/primary_mother_unit/units_alive/
//     buildings_alive/units_built_total/buildings_built_total/units_lost_total/buildings_lost_total/
//     units_killed_total/buildings_killed_total. Reproduced literally, not "fixed".
//   Never written by this function at all: landing_x/landing_y/landing_spot_index[32].
//
// llm_strat_player_param_defaults_init @0x00455a84: for player = 0..7 inclusive (loop starts at 0,
// no off-by-one here), three parallel double[8] slots, no x87 -- raw bit-pattern immediate MOVs:
//   0x00455ab6/ba: net_peer_horizon[player] = 10.0
//   0x00455ad0/da: net_peer_horizon_pending[player] = -1.0
//   0x00455aea/f4: game_speed_player_factor[player] = 1.0
//
// llm_strat_init_human_player_data @0x004dd91d:
//   0x004dd933-0x004dd93c: _G_LLM_STRAT_AI_ACTIVE_PLAYER_COUNT raised to player_idx+1 -- UNSIGNED
//     compare (CMP EAX,[count] / JC), the raise fires only when current count <= player_idx (JC
//     skips it, i.e. count > player_idx does NOT raise).
//   0x004dd955: is_alien_race = is_alien_race param (offset 0x104c0).
//   0x004dd95c: ai_enabled = 0 (offset 0x18).
//   0x004dd967: ai_group_count = 0 (offset 0x10564).
//   0x004dd972: next_group_serial = 0 (offset 0x10560).
//   0x004dd97d: ai_target_list_count = 0 (offset 0x25228).
//   0x004dd988: ai_bldg_queue_count = 0 (offset 0x2572c).
//   0x004dd993: ai_clock = 0.001f (offset 0x1003c; bit pattern 0x3a83126f).
//   0x004dd9a8-0x004dd9e6: ai_clock_m/_t/_s (offsets 0x10048/0x10044/0x10040) = (float)player_idx *
//     PERIOD * STAGGER, x87, each product independently FMUL'd off the same FILD/FST float snapshot
//     of player_idx. period is sim_view::ai_move_period/ai_tactic_period/ai_strategy_period; stagger
//     is the DOUBLE at 0x00506da8 (sim_view::ai_clock_stagger_fraction), NOT
//     sim_view::ai_invasion_clock_stagger @0x00506da0 (a different address, same value today).
//   0x004dd9ed: ai_attack_orders_issued = 0 (offset 0x2c).
//   0x004dd9f8: ai_map_changed_pending = 0 (offset 0x34).
//   0x004dda03: ai_turret_rescan_pending = 0 (offset 0x38).
//   0x004dda10-0x004dda4b: ai_player_relation[i] = (i == player_idx) ? 1 : -1, for i in [0, 8)
//     (offset 0x284c8; loop starts at 0, full 8 slots).
//   0x004dda4d-0x004dda4f: llm_strat_ai_init_build_candidate_priorities(player_idx) called
//     UNCONDITIONALLY, every call.
//
#include "sim/resid/sim_player_init.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct set_color_call {
    int32_t  player;
    uint32_t color_index;
};
std::vector<set_color_call> g_set_color_calls;
void                        rec_player_set_color(int32_t player_idx, uint32_t color_index) {
    g_set_color_calls.push_back({player_idx, color_index});
}

// Invoked by llm_strat_init_human_player_data (0x004dda4d-0x004dda4f), unconditionally, once per
// call. Not invoked by either of the other two functions in this TU -- present in the shared table
// only because player_init_calls is the ONE table per the header's own rationale.
struct ai_init_build_call {
    int32_t player_index;
};
std::vector<ai_init_build_call> g_ai_init_build_calls;
void                            rec_ai_init_build_candidate_priorities(int32_t player_index) {
    g_ai_init_build_calls.push_back({player_index});
}

const player_init_calls g_calls = {
    &rec_player_set_color,
    &rec_ai_init_build_candidate_priorities,
};

constexpr int32_t PLAYER = 5;

void reset_recorders() {
    g_set_color_calls.clear();
    g_ai_init_build_calls.clear();
}

} // namespace

void run_player_init_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- plain field stores, the player_set_color mock, mother_established forced to 0 regardless
    // of prior content, and the negative arm: landing_x/landing_y/landing_spot_index are NEVER written
    // by this function.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t controller_flags = 0x2Au; // 42
        constexpr uint32_t race             = 5u;
        constexpr uint32_t color_index      = 6u;
        constexpr double   game_clock       = 42.25;
        constexpr int32_t  side_id          = -3;
        char               name1[]          = "Ann";

        player_profile &profile       = fx.profiles[PLAYER];
        profile.mother_established    = 555;  // must be forced to 0 regardless
        profile.landing_x[0]          = 4001; // never written by this function
        profile.landing_y[0]          = 4002;
        profile.landing_spot_index[0] = 4003;

        sim_store own = fx.store();
        detail::player_profile_init(own, g_calls, PLAYER, controller_flags, race, game_clock,
                                    color_index, name1, side_id);

        ck_eq((uint32_t)profile.status_flags, controller_flags,
              "T1: status_flags = controller_flags, 0x00454a41-0x00454a4b");
        ck_eq((uint32_t)profile.race, race, "T1: race = race, 0x00454a51-0x00454a5b");
        ck_eq((uint32_t)profile.color_index, color_index,
              "T1: color_index = color_index, 0x00454a61-0x00454a6b");
        ck_eq((uint32_t)profile.flag_sprite_id, color_index + 0x453fu,
              "T1: flag_sprite_id = color_index + 0x453f (immediate operand), 0x00454a71-0x00454a80");
        ck((g_set_color_calls.size() == 1 && g_set_color_calls[0].player == PLAYER &&
            g_set_color_calls[0].color_index == color_index),
           "T1: player_set_color(player, color_index) called exactly once with both args, 0x00454a86-0x00454a8c");
        ck_eq_d(profile.prod_check_clock, game_clock,
                "T1: prod_check_clock = game_clock, 0x00454a91-0x00454aa4");
        ck_eq((uint32_t)profile.mother_established, 0u,
              "T1: mother_established forced to 0 unconditionally (was seeded 555), 0x00454aaa-0x00454ab1");
        ck_eq((uint32_t)profile.side_id, (uint32_t)side_id,
              "T1: side_id = side_id, 0x00454abb-0x00454ac5");
        ck_eq((uint32_t)profile.landing_x[0], 4001u,
              "T1: landing_x[0] untouched -- never written by this function");
        ck_eq((uint32_t)profile.landing_y[0], 4002u,
              "T1: landing_y[0] untouched -- never written by this function");
        ck_eq((uint32_t)profile.landing_spot_index[0], 4003u,
              "T1: landing_spot_index[0] untouched -- never written by this function");
    }

    // =================================================================================================
    // T2 -- short name (<=12 chars): copied verbatim, terminator present, NOT truncated.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        char name2[] = "Ann";

        player_profile &profile = fx.profiles[PLAYER];
        sim_store       own     = fx.store();
        detail::player_profile_init(own, g_calls, PLAYER, 0u, 0u, 0.0, 0u, name2, 0);

        ck(strcmp(profile.name, "Ann") == 0,
           "T2: short name copied verbatim ('Ann'), 0x004549bc-0x004549d2");
        ck_eq((uint32_t)(uint8_t)profile.name[3], 0u, "T2: terminator present at name[3]");
        ck_eq((uint32_t)(uint8_t)profile.name[9], 0u,
              "T2: name[9] untouched by the truncate branch (len 3 <= 12, no truncation), 0x004549f3/f6");
    }

    // =================================================================================================
    // T3 -- boundary from below: length exactly 12 must NOT truncate.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        char name3[] = "ABCDEFGHIJKL"; // 12 chars

        player_profile &profile = fx.profiles[PLAYER];
        sim_store       own     = fx.store();
        detail::player_profile_init(own, g_calls, PLAYER, 0u, 0u, 0.0, 0u, name3, 0);

        ck(strcmp(profile.name, "ABCDEFGHIJKL") == 0,
           "T3: length-12 name copied verbatim, NOT truncated (CMP ECX,0xc / JBE @0x004549f3/f6)");
        ck_eq((uint32_t)(uint8_t)profile.name[12], 0u, "T3: terminator present at name[12]");
        ck_eq((uint32_t)(uint8_t)profile.name[9], (uint32_t)(uint8_t)'J',
              "T3: name[9] is still the original 'J', NOT overwritten with '.' -- boundary-12 is the no-truncate side");
    }

    // =================================================================================================
    // T4 -- boundary from above: length exactly 13 MUST truncate to 9 chars + "...".
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        char name4[] = "ABCDEFGHIJKLM"; // 13 chars

        player_profile &profile = fx.profiles[PLAYER];
        sim_store       own     = fx.store();
        detail::player_profile_init(own, g_calls, PLAYER, 0u, 0u, 0.0, 0u, name4, 0);

        ck(strcmp(profile.name, "ABCDEFGHI...") == 0,
           "T4: length-13 name truncated to first 9 chars + '...' (CMP ECX,0xc / JBE @0x004549f3/f6)");
        ck_eq((uint32_t)(uint8_t)profile.name[9], 0x2Eu,
              "T4: name[9] == '.' (suffix bytes at 0x00501039 memory-read-CONFIRMED as 2E 2E 2E 00), 0x00454a28");
        ck_eq((uint32_t)(uint8_t)profile.name[10], 0x2Eu, "T4: name[10] == '.', 0x00454a28-0x00454a3e");
        ck_eq((uint32_t)(uint8_t)profile.name[11], 0x2Eu, "T4: name[11] == '.', 0x00454a28-0x00454a3e");
        ck_eq((uint32_t)(uint8_t)profile.name[12], 0u,
              "T4: name[12] == '\\0' -- terminator of the '...' suffix, 0x00454a3c/0x00454a3e");
    }

    // =================================================================================================
    // T5 -- a much-longer name truncates the same way: first 9 chars + "...".
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        char name5[] = "Abcdefghijklmnopqrstuvwxyz"; // 26 chars, well past the 13-char threshold

        player_profile &profile = fx.profiles[PLAYER];
        sim_store       own     = fx.store();
        detail::player_profile_init(own, g_calls, PLAYER, 0u, 0u, 0.0, 0u, name5, 0);

        ck(strcmp(profile.name, "Abcdefghi...") == 0,
           "T5: much-longer (26-char) name truncated to first 9 chars + '...', same predicate as T4");
        ck_eq((uint32_t)(uint8_t)profile.name[9], 0x2Eu, "T5: name[9] == '.', 0x00454a28");
        ck_eq((uint32_t)(uint8_t)profile.name[10], 0x2Eu, "T5: name[10] == '.', 0x00454a28-0x00454a3e");
        ck_eq((uint32_t)(uint8_t)profile.name[11], 0x2Eu, "T5: name[11] == '.', 0x00454a28-0x00454a3e");
        ck_eq((uint32_t)(uint8_t)profile.name[12], 0u, "T5: name[12] == '\\0', 0x00454a3c/0x00454a3e");
    }

    // =================================================================================================
    // T6 -- PRESERVE-BUG: the eleven-array reset loop runs i = 1..31, NEVER touching index 0. Seed
    // index 0/1/31 of every array with DISTINCT non-zero sentinels; index 0 must survive unchanged
    // (the bug), index 1 and 31 must be zeroed (both loop ends).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();

        player_profile &profile = fx.profiles[PLAYER];

        struct preserve_bug_field {
            int32_t    *arr;
            const char *name;
            const char *addr; // the store instruction inside the loop body for THIS field
        };
        preserve_bug_field fields[] = {
            {profile.prod_queue_slot, "prod_queue_slot", "0x00454af1"},
            {profile.primary_mother_bldg, "primary_mother_bldg", "0x00454b0a"},
            {profile.primary_mother_unit, "primary_mother_unit", "0x00454b23"},
            {profile.units_alive, "units_alive", "0x00454b3c"},
            {profile.buildings_alive, "buildings_alive", "0x00454b55"},
            {profile.units_built_total, "units_built_total", "0x00454b6e"},
            {profile.buildings_built_total, "buildings_built_total", "0x00454b87"},
            {profile.units_lost_total, "units_lost_total", "0x00454ba0"},
            {profile.buildings_lost_total, "buildings_lost_total", "0x00454bb9"},
            {profile.units_killed_total, "units_killed_total", "0x00454bd2"},
            {profile.buildings_killed_total, "buildings_killed_total", "0x00454beb"},
        };
        constexpr int kFieldCount = sizeof(fields) / sizeof(fields[0]);
        static_assert(kFieldCount == 11, "eleven fields per the header banner");

        for (int i = 0; i < kFieldCount; ++i) {
            fields[i].arr[0]  = 100 + i; // sentinel A: must SURVIVE (the bug)
            fields[i].arr[1]  = 200 + i; // sentinel B: must be zeroed (loop's first iteration)
            fields[i].arr[31] = 300 + i; // sentinel C: must be zeroed (loop's last iteration)
        }

        char empty_name[] = "";

        sim_store own = fx.store();
        detail::player_profile_init(own, g_calls, PLAYER, 0u, 0u, 0.0, 0u, empty_name, 0);

        for (int i = 0; i < kFieldCount; ++i) {
            char buf[224];
            std::snprintf(buf, sizeof(buf),
                          "T6 PRESERVE-BUG: %s[0] left untouched (loop starts at i=1, not 0), 0x00454acb",
                          fields[i].name);
            ck_eq((uint32_t)fields[i].arr[0], (uint32_t)(100 + i), buf);

            std::snprintf(buf, sizeof(buf), "T6: %s[1] zeroed by the loop's first iteration, %s",
                          fields[i].name, fields[i].addr);
            ck_eq((uint32_t)fields[i].arr[1], 0u, buf);

            std::snprintf(buf, sizeof(buf), "T6: %s[31] zeroed by the loop's last iteration (i<32), %s",
                          fields[i].name, fields[i].addr);
            ck_eq((uint32_t)fields[i].arr[31], 0u, buf);
        }
    }

    // =================================================================================================
    // T7 -- llm_strat_player_param_defaults_init: all three parallel double[8] arrays, all 8 players.
    // Seed all three with distinct wrong values first so a loop that writes only some slots fails.
    // =================================================================================================
    {
        fx.reset();
        for (int32_t p = 0; p < 8; ++p) {
            fx.net_peer_horizon[p]         = 1000.0 + p;
            fx.net_peer_horizon_pending[p] = 2000.0 + p;
            fx.game_speed_player_factor[p] = 3000.0 + p;
        }

        sim_store own = fx.store();
        detail::player_param_defaults_init(own);

        for (int32_t p = 0; p < 8; ++p) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "T7: net_peer_horizon[%d] = 10.0, 0x00455ab6/ba", (int)p);
            ck_eq_d(fx.net_peer_horizon[p], 10.0, buf);

            std::snprintf(buf, sizeof(buf), "T7: net_peer_horizon_pending[%d] = -1.0, 0x00455ad0/da",
                          (int)p);
            ck_eq_d(fx.net_peer_horizon_pending[p], -1.0, buf);

            std::snprintf(buf, sizeof(buf), "T7: game_speed_player_factor[%d] = 1.0, 0x00455aea/f4",
                          (int)p);
            ck_eq_d(fx.game_speed_player_factor[p], 1.0, buf);
        }
    }
}

// =====================================================================================================
// run_init_human_player_data_tests -- llm_strat_init_human_player_data @0x004dd91d. See the file's top
// banner's "llm_strat_init_human_player_data" section for the full field-by-field EXPECTED BEHAVIOUR.
// =====================================================================================================
void run_init_human_player_data_tests() {
    sim_fixture fx;

    // =================================================================================================
    // U1 -- active-player high-water mark seeded ABOVE player_idx: the UNSIGNED compare's JC skips the
    // raise (0x004dd933-0x004dd93c), so the count is left exactly as seeded.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t PIDX   = 4;
        fx.ai_active_player_count = 10; // ABOVE PIDX -- must survive unchanged

        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::init_human_player_data(v, own, g_calls, PIDX, 0);

        ck_eq((uint32_t)fx.ai_active_player_count, 10u,
              "U1: HWM seeded ABOVE player_idx (10 > 4) is left unchanged, 0x004dd933-0x004dd93c (JC skips the raise)");
    }

    // =================================================================================================
    // U2 -- HWM seeded EXACTLY EQUAL to player_idx: the boundary itself -- count <= player_idx DOES
    // raise, landing on player_idx+1.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t PIDX   = 4;
        fx.ai_active_player_count = 4; // == PIDX -- boundary, must raise

        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::init_human_player_data(v, own, g_calls, PIDX, 0);

        ck_eq((uint32_t)fx.ai_active_player_count, PIDX + 1,
              "U2: HWM seeded EQUAL to player_idx (4 == 4) raises to player_idx+1 = 5, 0x004dd933-0x004dd93c");
    }

    // =================================================================================================
    // U3 -- HWM seeded BELOW player_idx: the raise fires and lands on player_idx+1 (not count+1).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t PIDX   = 4;
        fx.ai_active_player_count = 1; // BELOW PIDX -- must raise, landing on PIDX+1, not 1+1

        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::init_human_player_data(v, own, g_calls, PIDX, 0);

        ck_eq((uint32_t)fx.ai_active_player_count, PIDX + 1,
              "U3: HWM seeded BELOW player_idx (1 < 4) raises to player_idx+1 = 5, not count+1, 0x004dd933-0x004dd93c");
    }

    // =================================================================================================
    // U4 -- every scalar field the header banner lists, each seeded to a DISTINCT non-zero sentinel
    // first so a dropped store fails by name. is_alien_race is the one field that is a pass-through of
    // the parameter rather than a reset-to-0, so its sentinel is DISTINCT from the param value too.
    // Also covers the callee: fires UNCONDITIONALLY, exactly once, with player_idx.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t PIDX          = 3;
        constexpr int32_t  IS_ALIEN_RACE = 42;

        player_data &pd             = fx.players[PIDX];
        pd.is_alien_race            = -999;   // sentinel, DISTINCT from IS_ALIEN_RACE -- must become 42
        pd.ai_enabled               = 111;    // sentinel -- must become 0
        pd.ai_group_count           = 222;    // sentinel -- must become 0
        pd.next_group_serial        = 333;    // sentinel -- must become 0
        pd.ai_target_list_count     = 444;    // sentinel -- must become 0
        pd.ai_bldg_queue_count      = 555;    // sentinel -- must become 0
        pd.ai_clock                 = 987.5f; // sentinel -- must become 0.001f (bits 0x3a83126f)
        pd.ai_attack_orders_issued  = 666;    // sentinel -- must become 0
        pd.ai_map_changed_pending   = 777;    // sentinel -- must become 0
        pd.ai_turret_rescan_pending = 888;    // sentinel -- must become 0

        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::init_human_player_data(v, own, g_calls, PIDX, IS_ALIEN_RACE);

        ck_eq((uint32_t)pd.is_alien_race, (uint32_t)IS_ALIEN_RACE,
              "U4: is_alien_race = is_alien_race param, 0x004dd955 (offset 0x104c0)");
        ck_eq((uint32_t)pd.ai_enabled, 0u, "U4: ai_enabled = 0, 0x004dd95c (offset 0x18)");
        ck_eq((uint32_t)pd.ai_group_count, 0u, "U4: ai_group_count = 0, 0x004dd967 (offset 0x10564)");
        ck_eq((uint32_t)pd.next_group_serial, 0u,
              "U4: next_group_serial = 0, 0x004dd972 (offset 0x10560)");
        ck_eq((uint32_t)pd.ai_target_list_count, 0u,
              "U4: ai_target_list_count = 0, 0x004dd97d (offset 0x25228)");
        ck_eq((uint32_t)pd.ai_bldg_queue_count, 0u,
              "U4: ai_bldg_queue_count = 0, 0x004dd988 (offset 0x2572c)");

        ck_eq_d((double)pd.ai_clock, (double)0.001f,
                "U4: ai_clock = 0.001f (exact float), 0x004dd993 (offset 0x1003c)");
        uint32_t ai_clock_bits = 0;
        std::memcpy(&ai_clock_bits, &pd.ai_clock, sizeof(ai_clock_bits));
        ck_eq(ai_clock_bits, 0x3a83126fu,
              "U4: ai_clock bit pattern == 0x3a83126f, the original's literal immediate operand, 0x004dd993");

        ck_eq((uint32_t)pd.ai_attack_orders_issued, 0u,
              "U4: ai_attack_orders_issued = 0, 0x004dd9ed (offset 0x2c)");
        ck_eq((uint32_t)pd.ai_map_changed_pending, 0u,
              "U4: ai_map_changed_pending = 0, 0x004dd9f8 (offset 0x34)");
        ck_eq((uint32_t)pd.ai_turret_rescan_pending, 0u,
              "U4: ai_turret_rescan_pending = 0, 0x004dda03 (offset 0x38)");

        ck((g_ai_init_build_calls.size() == 1 && g_ai_init_build_calls[0].player_index == (int32_t)PIDX),
           "U4: ai_init_build_candidate_priorities(player_idx) called exactly once, unconditionally, with player_idx, 0x004dda4d-0x004dda4f");
    }

    // =================================================================================================
    // U5 -- the three staggered clocks: DELIBERATELY DIFFERENT periods (fixture reset() defaults:
    // move=1.0, tactic=5.0, strategy=10.0) and ai_clock_stagger_fraction set to a value DIFFERENT from
    // ai_invasion_clock_stagger (left at its reset() default 0.125 -- a DIFFERENT fixture member/region,
    // per sim_view's own banner and sim_test_support.h's SIM-RESID-IF REOPEN comment), so a case that
    // read the wrong stagger constant, or the wrong period, disagrees here. player_idx is non-zero (3).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t PIDX = 3;

        fx.ai_clock_stagger_fraction = 0.25; // DISTINCT from ai_invasion_clock_stagger's 0.125

        player_data &pd = fx.players[PIDX];

        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::init_human_player_data(v, own, g_calls, PIDX, 0);

        // 3 * 1.0 * 0.25 = 0.75 ; 3 * 5.0 * 0.25 = 3.75 ; 3 * 10.0 * 0.25 = 7.5 -- all exactly
        // representable in float, so exact equality is meaningful (same convention as
        // sim_spawn_invasion_force_selftest.cpp's analogous ai_clock_m/_t/_s checks).
        ck(pd.ai_clock_m == 0.75f,
           "U5: ai_clock_m = player_idx * ai_move_period * ai_clock_stagger_fraction = 3*1.0*0.25 = 0.75, 0x004dd9a8-0x004dd9ba (offset 0x10048)");
        ck(pd.ai_clock_t == 3.75f,
           "U5: ai_clock_t = player_idx * ai_tactic_period * ai_clock_stagger_fraction = 3*5.0*0.25 = 3.75, 0x004dd9c1-0x004dd9d0 (offset 0x10044)");
        ck(pd.ai_clock_s == 7.5f,
           "U5: ai_clock_s = player_idx * ai_strategy_period * ai_clock_stagger_fraction = 3*10.0*0.25 = 7.5, 0x004dd9d7-0x004dd9e6 (offset 0x10040)");
    }

    // =================================================================================================
    // U6 -- player_idx == 0: all three staggered clocks are 0.0 regardless of the periods/stagger (the
    // FILD'd player index itself is 0), even with non-trivial period/stagger values in the fixture.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.ai_move_period            = 9.0f;
        fx.ai_tactic_period          = 13.0f;
        fx.ai_strategy_period        = 17.0f;
        fx.ai_clock_stagger_fraction = 0.5;

        player_data &pd = fx.players[0];

        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::init_human_player_data(v, own, g_calls, 0u, 0);

        ck(pd.ai_clock_m == 0.0f,
           "U6: player_idx == 0 -> ai_clock_m = 0.0 regardless of periods, 0x004dd9a8-0x004dd9ba");
        ck(pd.ai_clock_t == 0.0f,
           "U6: player_idx == 0 -> ai_clock_t = 0.0 regardless of periods, 0x004dd9c1-0x004dd9d0");
        ck(pd.ai_clock_s == 0.0f,
           "U6: player_idx == 0 -> ai_clock_s = 0.0 regardless of periods, 0x004dd9d7-0x004dd9e6");
    }

    // =================================================================================================
    // U7 -- ai_player_relation[i] = (i == player_idx) ? 1 : -1, ALL EIGHT slots individually, with
    // player_idx neither 0 nor 7 (3), so both array endpoints are checked as genuinely "not self".
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        constexpr uint32_t PIDX = 3;

        player_data &pd = fx.players[PIDX];
        for (int32_t i = 0; i < 8; ++i) pd.ai_player_relation[i] = 9000 + i; // sentinel, all 8 slots

        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::init_human_player_data(v, own, g_calls, PIDX, 0);

        for (int32_t i = 0; i < 8; ++i) {
            char    buf[176];
            int32_t want = (i == (int32_t)PIDX) ? 1 : -1;
            std::snprintf(
                buf, sizeof(buf),
                "U7: ai_player_relation[%d] = %d (i %s player_idx=3), 0x004dda10-0x004dda4b (offset 0x284c8)",
                (int)i, (int)want, (i == (int32_t)PIDX) ? "==" : "!=");
            ck_eq((uint32_t)pd.ai_player_relation[i], (uint32_t)want, buf);
        }
    }
}

} // namespace mh::sim::test
