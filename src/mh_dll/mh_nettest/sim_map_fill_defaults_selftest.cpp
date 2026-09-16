//
// sim_map_fill_defaults_selftest.cpp -- `simtest` oracle for map_FillDefaults @0x0045603e
// (sim/resid/sim_map_fill_defaults.h/.cpp, RI-SIM / sim_resid batch C).
//
// arm_ready:false -- session-entry writer that calls the UI wall (llm_ui_bldg_panel_open); see
// sim_map_fill_defaults.h's "NO SHADOW SITE" banner. This offline oracle is its only evidence.
//
// EXPECTED BEHAVIOUR straight off the .asm (tmp/decomp_sim_resid/map_FillDefaults_0045603e.asm),
// NOT the Ghidra .c beside it (which silently assumed the subtick_b_clock store below was
// per-player -- it is not):
//   0x00456056: llm_ui_bldg_panel_open(), unconditional, BEFORE any other store.
//   0x0045605b-0x004561ff: 25 `utils_fill_data(dest, len, 0)` bulk zero-fills, in this exact
//     order (see the `expected[]` table in T1 below). Three of them -- map_object_table
//     @0x00456111, map_objects @0x00456122, map_halfres_grid @0x00456133 -- are the ones the
//     first translation draft was missing (SIM-RESID-IF's third close).
//   0x00456204: _G_LLM_STRAT_FOREIGN_BLDG_EVENT_PENDING = 0.
//   0x0045620e-0x0045644b: per-player loop, p = 0..7 (`CMP ...,0x8` @0x00456215):
//     path_free_slot_count[p] = 100 (@0x0045622b); path_slot_flags[p][0..99] = 0 (`CMP ...,0x64`
//     @0x0045623c, store @0x00456250); power_stats[p].ratio = 1.0 (@0x0045625d/0x00456267);
//     population[p].subtick_a_clock = *game_clock, a STRAIGHT COPY not a sum (@0x00456275/
//     0x0045627b); storage_stats[p].cap_accum[0..9]/cap_prev[0..9] = 0 (`CMP ...,0xa` @0x00456288,
//     stores @0x004562a1/0x004562b7); then the PRESERVE-BUG (@0x004562c3-0x004562d9, see below);
//     productions[p][0..7].b_index = 0 (`CMP ...,0x8` @0x004562e0, store @0x004562fd);
//     storage[p][0..24].b_index = 0 (`CMP ...,0x19` @0x00456310, store @0x0045632d);
//     turrets[p][0..31].b_index = 0, WORD store (`CMP ...,0x20` @0x00456340, store @0x0045635a);
//     labs[p][0..24].b_index = 0 (`CMP ...,0x19` @0x0045636c, store @0x00456388);
//     mines[p][0..31].b_index = 0 (`CMP ...,0x20` @0x0045639b, store @0x004563b5);
//     soldiers[p][0..99].owner_unit = 0, WORD store (`CMP ...,0x64` @0x004563c8, store @0x004563e2);
//     progress[p][0..Progress[0].index).{available,acquired,f3} = false, a DYNAMIC bound read
//     fresh off cfg_inventions[0].index @0x00e16305 (MOVZX @0x004563f4, CMP @0x004563fb, stores
//     @0x00456416/0x0045642c/0x00456442) -- NOT a hardcoded 300 (T2 below pins this).
//   0x00456450-0x00456477: ctrl_groups[0..9].count = 0 (`CMP ...,0xa` @0x00456457, store
//     @0x0045646b).
//   0x00456477-0x004564af: order_count=0 (@0x00456477), order_staging_count=0 (@0x00456481),
//     order_pending_count=0 (@0x0045648b), mouse_buttons_prev=0 (@0x00456495), ctrl_group[0].count
//     re-zeroed a second time (harmless PRESERVE-BUG, literal re-store @0x0045649c),
//     ui_selected_bldg_index=0 (@0x004564a6), click_select_target_id=0 (@0x004564af).
//
// ---- THE PRESERVE-BUG (0x004562c3-0x004562d9) ------------------------------------------------------
// The original means to seed storage_stats[p].subtick_b_clock (offset 0x50 in the 0x58-stride
// struct), but its index comes from `local_1c`, the INNER cap_accum/cap_prev loop counter that
// the immediately-preceding loop just left at 10 -- NOT from `local_18`/p, the outer per-player
// index. `IMUL EAX,[local_1c=10],0x58` = 0x370; `FSTP [EAX+0xbf4d50]` (0x004562d3) therefore
// always lands at the SAME fixed address 0xbf50c0 on all eight iterations, which is 0x40 bytes
// into _G_LLM_STRAT_DEATH_ANIM_TABLE (int32_t[6][4], row stride 0x10) -- i.e. elements
// [4][0]/[4][1], int32 flat indices 16/17. No player's real subtick_b_clock is EVER written by
// this function. Reproduced, not fixed (Law 2) -- see sim_map_fill_defaults.h's own
// "PRESERVE-BUG" section.
//
#include "sim/resid/sim_map_fill_defaults.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct fill_call {
    const void *dest;
    uint32_t    len;
    uint8_t     fill;
};
std::vector<fill_call> g_fill_calls;
int                    g_panel_open_calls             = 0;
int                    g_panel_open_fills_seen_so_far = -1; // # of fills recorded BEFORE panel_open fired

void *rec_fill_data(void *ptr, uint32_t size, uint8_t default_value) {
    g_fill_calls.push_back({ptr, size, default_value});
    return ptr;
}

void rec_bldg_panel_open() {
    ++g_panel_open_calls;
    if (g_panel_open_fills_seen_so_far < 0) g_panel_open_fills_seen_so_far = (int)g_fill_calls.size();
}

const map_fill_defaults_calls g_calls = {
    &rec_bldg_panel_open,
    &rec_fill_data,
};

void reset_recorders() {
    g_fill_calls.clear();
    g_panel_open_calls             = 0;
    g_panel_open_fills_seen_so_far = -1;
}

} // namespace

void run_map_fill_defaults_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- exhaustive: the panel-open call fires first; all 25 fill_data calls happen in original
    // order against the right (dest, len); the three map-plane fills are pinned individually and
    // proven pairwise distinct; every per-player default and the PRESERVE-BUG are pinned exactly.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();

        // ---- the two fixture constants the PRESERVE-BUG's sum depends on: DISTINCT, non-round, so a
        // case using only one of them (or the wrong pair) disagrees with the death_anim_table result.
        const double game_clock_seed    = 137.5; // fx.game_clock default is 0.0
        const double storage_delay_seed = 0.25;  // fx.storage_stats_init_delay default is 10.0 (round)
        fx.game_clock                   = game_clock_seed;
        fx.storage_stats_init_delay     = storage_delay_seed;

        // ---- sentinels for everything this function is expected to overwrite, so a dropped/short
        // loop or a wrong destination leaves a visible survivor -----------------------------------
        fx.foreign_bldg_event_pending = 424242;
        fx.order_queue_count          = 111;
        fx.order_staging_count        = 222;
        fx.order_pending_count        = 333;
        fx.mouse_buttons_prev         = 0x5a;
        fx.ui_selected_bldg_index     = 4321;
        fx.click_select_target_id     = 1234;

        for (int32_t p = 0; p < MAX_PLAYERS; ++p) {
            fx.path_free_slot_count[(size_t)p] = -999 - p;
            for (int32_t slot = 0; slot < PATH_SLOTS_PER_PLAYER; ++slot)
                fx.path_slot_flags[(size_t)(p * PATH_SLOTS_PER_PLAYER + slot)] = 0xff;
            fx.power_stats_rows[(size_t)p].ratio     = 2.0 + p * 0.5; // != 1.0
            fx.population[(size_t)p].subtick_a_clock = -1.0 - p;      // must be overwritten by a COPY, not 0
            for (int32_t i = 0; i < 10; ++i) {
                fx.storage_stats_rows[(size_t)p].cap_accum[i] = 7000 + p * 10 + i;
                fx.storage_stats_rows[(size_t)p].cap_prev[i]  = 8000 + p * 10 + i;
            }
            // DISTINCT per player, non-round: proves no player's copy is ever written (the whole
            // point of the PRESERVE-BUG).
            fx.storage_stats_rows[(size_t)p].subtick_b_clock = 9000.0 + p * 3.25;

            for (int32_t j = 0; j < PRODUCTIONS_PER_PLAYER; ++j)
                fx.productions[(size_t)(p * PRODUCTIONS_PER_PLAYER + j)].b_index = 1000 + p * 100 + j;
            for (int32_t j = 0; j < STORAGE_PER_PLAYER; ++j)
                fx.storage[(size_t)(p * STORAGE_PER_PLAYER + j)].b_index = 2000 + p * 100 + j;
            for (int32_t j = 0; j < TURRETS_PER_PLAYER; ++j)
                fx.turrets[(size_t)(p * TURRETS_PER_PLAYER + j)].b_index = (int16_t)(3000 + p * 40 + j);
            for (int32_t j = 0; j < LABS_PER_PLAYER; ++j)
                fx.labs[(size_t)(p * LABS_PER_PLAYER + j)].b_index = 4000 + p * 100 + j;
            for (int32_t j = 0; j < MINES_PER_PLAYER; ++j)
                fx.mines[(size_t)(p * MINES_PER_PLAYER + j)].b_index = 5000 + p * 100 + j;
            for (int32_t j = 0; j < SOLDIERS_PER_PLAYER; ++j)
                fx.soldiers[(size_t)(p * SOLDIERS_PER_PLAYER + j)].owner_unit = (int16_t)(6000 + p * 100 + j);

            // progress: seed EVERY row (0..PROGRESS_ROW_COUNT-1), not just the ones the dynamic bound
            // below will touch, so row `index` and beyond are provably left ALONE.
            for (int32_t row = 0; row < PROGRESS_ROW_COUNT; ++row) {
                auto &pr     = fx.progress[(size_t)(p * PROGRESS_ROW_COUNT + row)];
                pr.available = 1;
                pr.acquired  = 1;
                pr.f3        = 1;
            }
        }
        for (int32_t g = 0; g < 10; ++g) fx.ctrl_groups[(size_t)g].count = 9000 + g;

        // death_anim_table: a distinct pattern over the whole tracked region, so the straddling
        // 8-byte store is caught at EXACTLY indices 16/17 and neighbours 15/18 are provably untouched.
        for (size_t i = 0; i < fx.death_anim_table.size(); ++i) fx.death_anim_table[i] = (int32_t)(6000 + i * 17);

        // The dynamic progress-row bound this call will use: small and non-zero (T2 below covers the
        // index==0 boundary).
        fx.cfg_inventions[0].index = 5;

        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::map_fill_defaults(v, own, g_calls);

        // ---- llm_ui_bldg_panel_open: unconditional, exactly once, at the TOP -----------------------
        ck_eq((uint32_t)g_panel_open_calls, 1u,
              "T1: llm_ui_bldg_panel_open fires exactly once, 0x00456056");
        ck_eq((uint32_t)g_panel_open_fills_seen_so_far, 0u,
              "T1: llm_ui_bldg_panel_open fires BEFORE any utils_fill_data call (unconditional, at the "
              "top of the function) -- 0x00456056 precedes the first fill at 0x00456067");

        // ---- the 25 utils_fill_data calls: exact count, exact order, exact (dest, len, fill=0) -----
        struct expected_fill {
            const char *name;
            const void *dest;
            uint32_t    len;
            const char *addr;
        };
        const expected_fill expected[] = {
            {"power_stats", fx.power_stats_rows.data(), 0xc0, "0x00456067"},
            {"unit_housing", fx.unit_housing.data(), 0x200, "0x00456078"},
            {"population", fx.population.data(), 0x1a0, "0x00456089"},
            {"player_resources", fx.player_resources.data(), 0x140, "0x0045609a"},
            {"units", fx.units.data(), 0x2d820, "0x004560ab"},
            {"buildings", fx.buildings.data(), 0x35520, "0x004560bc"},
            {"fog_of_war", fx.fog_of_war_bytes.data(), 0x90000, "0x004560cd"},
            {"projectile_pool", fx.projectile_pool.data(), 0x2c4fc, "0x004560de"},
            {"fx_anim_pool", fx.fx_anim_pool.data(), 0x33450, "0x004560ef"},
            {"tile_objects", fx.tile_objects.data(), 0x80000, "0x00456100"},
            {"map_object_table", fx.map_object_table_bytes.data(), 0x3a980, "0x00456111"},
            {"map_objects", fx.map_objects_bytes.data(), 0x3a980, "0x00456122"},
            {"map_halfres_grid", fx.map_halfres_grid_bytes.data(), 0x10000, "0x00456133"},
            {"order_queue", fx.order_queue.data(), 0x4fb0, "0x00456144"},
            {"resources", fx.resources.data(), 0x10000, "0x00456155"},
            {"landing_spots", fx.landing_spots.data(), 0xc0, "0x00456166"},
            {"soldiers", fx.soldiers.data(), 0x5aa0, "0x00456177"},
            {"productions", fx.productions.data(), 0x6540, "0x00456188"},
            {"mines", fx.mines.data(), 0x3800, "0x00456199"},
            {"storage", fx.storage.data(), 0xbea0, "0x004561aa"},
            {"turrets", fx.turrets.data(), 0x3700, "0x004561bb"},
            {"labs", fx.labs.data(), 0x640, "0x004561cc"},
            {"storage_stats", fx.storage_stats_rows.data(), 0x2c0, "0x004561dd"},
            {"path_buffers", fx.path_buffers.data(), 0x75300, "0x004561ee"},
            {"passable", fx.passable.data(), 0x10000, "0x004561ff"},
        };
        constexpr size_t kExpectedFills = sizeof(expected) / sizeof(expected[0]);
        static_assert(kExpectedFills == 25, "map_FillDefaults issues exactly 25 utils_fill_data calls");

        ck_eq((uint32_t)g_fill_calls.size(), (uint32_t)kExpectedFills,
              "T1: exactly 25 utils_fill_data calls (a dropped/duplicated fill changes this count), "
              "0x00456067-0x004561ff");
        for (size_t i = 0; i < kExpectedFills && i < g_fill_calls.size(); ++i) {
            char msg[240];
            std::snprintf(msg, sizeof(msg), "T1: fill #%zu (%s) dest is &fx.%s.data()[0], in original order, %s",
                          i, expected[i].name, expected[i].name, expected[i].addr);
            ck(g_fill_calls[i].dest == expected[i].dest, msg);
            std::snprintf(msg, sizeof(msg), "T1: fill #%zu (%s) len == 0x%x, %s", i, expected[i].name,
                          expected[i].len, expected[i].addr);
            ck_eq(g_fill_calls[i].len, expected[i].len, msg);
            std::snprintf(msg, sizeof(msg), "T1: fill #%zu (%s) fill byte == 0 (XOR EDX,EDX before every call), %s",
                          i, expected[i].name, expected[i].addr);
            ck_eq((uint32_t)g_fill_calls[i].fill, 0u, msg);
        }

        // ---- the three map-plane fills: individually pinned above AND proven pairwise distinct here
        // (same 0x3a980-length pair for the first two -- a copy-pasted destination would still pass a
        // length-only check) -----------------------------------------------------------------------
        if (g_fill_calls.size() > 12) {
            ck(g_fill_calls[10].dest != g_fill_calls[11].dest && g_fill_calls[11].dest != g_fill_calls[12].dest &&
                   g_fill_calls[10].dest != g_fill_calls[12].dest,
               "T1: map_object_table (0x00456111) / map_objects (0x00456122) / map_halfres_grid "
               "(0x00456133) fill destinations are THREE DISTINCT pointers -- these three were missing "
               "from the first translation draft and are the reason this row was deferred");
        }

        // ---- _G_LLM_STRAT_FOREIGN_BLDG_EVENT_PENDING -------------------------------------------------
        ck_eq((uint32_t)fx.foreign_bldg_event_pending, 0u,
              "T1: _G_LLM_STRAT_FOREIGN_BLDG_EVENT_PENDING = 0, 0x00456204");

        // ---- per-player defaults ---------------------------------------------------------------------
        for (int32_t p = 0; p < MAX_PLAYERS; ++p) {
            char msg[300];

            std::snprintf(msg, sizeof(msg), "T1: path_free_slot_count[%d] = 100, 0x0045622b", p);
            ck_eq((uint32_t)fx.path_free_slot_count[(size_t)p], 100u, msg);

            for (int32_t slot = 0; slot < PATH_SLOTS_PER_PLAYER; ++slot) {
                std::snprintf(msg, sizeof(msg), "T1: path_slot_flags[%d][%d] = 0, 0x00456250", p, slot);
                ck_eq((uint32_t)fx.path_slot_flags[(size_t)(p * PATH_SLOTS_PER_PLAYER + slot)], 0u, msg);
            }

            std::snprintf(msg, sizeof(msg), "T1: power_stats[%d].ratio = 1.0, 0x0045625d/0x00456267", p);
            ck_eq_d(fx.power_stats_rows[(size_t)p].ratio, 1.0, msg);

            std::snprintf(msg, sizeof(msg),
                          "T1: population[%d].subtick_a_clock = *game_clock, a STRAIGHT COPY (not "
                          "+storage_stats_init_delay), 0x00456275/0x0045627b",
                          p);
            ck_eq_d(fx.population[(size_t)p].subtick_a_clock, game_clock_seed, msg);

            for (int32_t i = 0; i < 10; ++i) {
                std::snprintf(msg, sizeof(msg), "T1: storage_stats[%d].cap_accum[%d] = 0, 0x004562a1", p, i);
                ck_eq((uint32_t)fx.storage_stats_rows[(size_t)p].cap_accum[i], 0u, msg);
                std::snprintf(msg, sizeof(msg), "T1: storage_stats[%d].cap_prev[%d] = 0, 0x004562b7", p, i);
                ck_eq((uint32_t)fx.storage_stats_rows[(size_t)p].cap_prev[i], 0u, msg);
            }

            for (int32_t j = 0; j < PRODUCTIONS_PER_PLAYER; ++j) {
                std::snprintf(msg, sizeof(msg), "T1: productions[%d][%d].b_index = 0, 0x004562fd", p, j);
                ck_eq((uint32_t)fx.productions[(size_t)(p * PRODUCTIONS_PER_PLAYER + j)].b_index, 0u, msg);
            }
            for (int32_t j = 0; j < STORAGE_PER_PLAYER; ++j) {
                std::snprintf(msg, sizeof(msg), "T1: storage[%d][%d].b_index = 0, 0x0045632d", p, j);
                ck_eq((uint32_t)fx.storage[(size_t)(p * STORAGE_PER_PLAYER + j)].b_index, 0u, msg);
            }
            for (int32_t j = 0; j < TURRETS_PER_PLAYER; ++j) {
                std::snprintf(msg, sizeof(msg), "T1: turrets[%d][%d].b_index = 0 (WORD store), 0x0045635a", p, j);
                ck_eq((uint32_t)(uint16_t)fx.turrets[(size_t)(p * TURRETS_PER_PLAYER + j)].b_index, 0u, msg);
            }
            for (int32_t j = 0; j < LABS_PER_PLAYER; ++j) {
                std::snprintf(msg, sizeof(msg), "T1: labs[%d][%d].b_index = 0, 0x00456388", p, j);
                ck_eq((uint32_t)fx.labs[(size_t)(p * LABS_PER_PLAYER + j)].b_index, 0u, msg);
            }
            for (int32_t j = 0; j < MINES_PER_PLAYER; ++j) {
                std::snprintf(msg, sizeof(msg), "T1: mines[%d][%d].b_index = 0, 0x004563b5", p, j);
                ck_eq((uint32_t)fx.mines[(size_t)(p * MINES_PER_PLAYER + j)].b_index, 0u, msg);
            }
            for (int32_t j = 0; j < SOLDIERS_PER_PLAYER; ++j) {
                std::snprintf(msg, sizeof(msg), "T1: soldiers[%d][%d].owner_unit = 0 (WORD store), 0x004563e2", p,
                              j);
                ck_eq((uint32_t)(uint16_t)fx.soldiers[(size_t)(p * SOLDIERS_PER_PLAYER + j)].owner_unit, 0u, msg);
            }

            // progress: rows [0, index) cleared, row `index` (the first OUT-of-range row) untouched.
            for (int32_t row = 0; row < 5; ++row) {
                auto &pr = fx.progress[(size_t)(p * PROGRESS_ROW_COUNT + row)];
                std::snprintf(msg, sizeof(msg), "T1: progress[%d][%d].available = false, 0x00456416", p, row);
                ck_eq((uint32_t)pr.available, 0u, msg);
                std::snprintf(msg, sizeof(msg), "T1: progress[%d][%d].acquired = false, 0x0045642c", p, row);
                ck_eq((uint32_t)pr.acquired, 0u, msg);
                std::snprintf(msg, sizeof(msg), "T1: progress[%d][%d].f3 = false, 0x00456442", p, row);
                ck_eq((uint32_t)pr.f3, 0u, msg);
            }
            {
                auto &pr = fx.progress[(size_t)(p * PROGRESS_ROW_COUNT + 5)];
                std::snprintf(msg, sizeof(msg),
                              "T1: progress[%d][5] (== Progress[0].index, the first OUT-of-range row) "
                              "UNCHANGED -- the row bound is read fresh off cfg_inventions[0].index, "
                              "0x004563f4/0x004563fb",
                              p);
                ck(pr.available == 1 && pr.acquired == 1 && pr.f3 == 1, msg);
            }
        }

        // ---- PRESERVE-BUG: the straddling subtick_b_clock store -------------------------------------
        const double expect_sum = game_clock_seed + storage_delay_seed;
        int32_t      expect_words[2];
        std::memcpy(expect_words, &expect_sum, sizeof(expect_sum));
        ck_eq((uint32_t)fx.death_anim_table[16], (uint32_t)expect_words[0],
              "T1: death_anim_table int32 index 16 ([4][0]) = LOW half of *game_clock + "
              "*storage_stats_init_delay, the PRESERVE-BUG's straddling FSTP, 0x004562c3-0x004562d3");
        ck_eq((uint32_t)fx.death_anim_table[17], (uint32_t)expect_words[1],
              "T1: death_anim_table int32 index 17 ([4][1]) = HIGH half of the same double, "
              "0x004562c3-0x004562d3");
        double got_roundtrip = 0.0;
        std::memcpy(&got_roundtrip, &fx.death_anim_table[16], sizeof(got_roundtrip));
        ck_eq_d(got_roundtrip, expect_sum,
                "T1: death_anim_table[16..17] round-tripped as one double == *game_clock + "
                "*storage_stats_init_delay, 0x004562c3-0x004562d9");

        ck_eq((uint32_t)fx.death_anim_table[15], (uint32_t)(6000 + 15 * 17),
              "T1: death_anim_table index 15 (just BEFORE the straddling store) UNCHANGED -- an "
              "off-by-one destination would corrupt this, 0x004562d3");
        ck_eq((uint32_t)fx.death_anim_table[18], (uint32_t)(6000 + 18 * 17),
              "T1: death_anim_table index 18 (just AFTER the straddling store) UNCHANGED -- an "
              "off-by-one destination would corrupt this, 0x004562d3");

        for (int32_t p = 0; p < MAX_PLAYERS; ++p) {
            char         msg[420];
            const double sentinel = 9000.0 + p * 3.25;
            std::snprintf(
                msg, sizeof(msg),
                "T1: storage_stats[%d].subtick_b_clock UNCHANGED (still its seeded sentinel %.4f) -- the "
                "PRESERVE-BUG means NO player's real subtick_b_clock is ever written by this function "
                "(the store's index comes from local_1c==10, not p); THIS ASSERTS THE BUGGY BEHAVIOUR "
                "and must not be \"fixed\", 0x004562cf/0x004562d3",
                p, sentinel);
            ck_eq_d(fx.storage_stats_rows[(size_t)p].subtick_b_clock, sentinel, msg);
        }

        // ---- ctrl_groups[0..9].count = 0 (covers the harmless PRESERVE-BUG re-zero of group 0) -----
        for (int32_t g = 0; g < 10; ++g) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "T1: ctrl_groups[%d].count = 0, 0x0045646b", g);
            ck_eq((uint32_t)fx.ctrl_groups[(size_t)g].count, 0u, msg);
        }
        ck_eq((uint32_t)fx.ctrl_groups[0].count, 0u,
              "T1: ctrl_groups[0].count re-zeroed a second time (harmless PRESERVE-BUG, literal "
              "instruction-for-instruction re-store), 0x0045649c");

        // ---- tail scalars -----------------------------------------------------------------------------
        ck_eq((uint32_t)fx.order_queue_count, 0u, "T1: _G_LLM_STRAT_ORDER_QUEUE_COUNT = 0, 0x00456477");
        ck_eq((uint32_t)fx.order_staging_count, 0u, "T1: _G_LLM_STRAT_ORDER_STAGING_COUNT = 0, 0x00456481");
        ck_eq((uint32_t)fx.order_pending_count, 0u, "T1: _G_LLM_STRAT_ORDER_PENDING_COUNT = 0, 0x0045648b");
        ck_eq((uint32_t)fx.mouse_buttons_prev, 0u, "T1: _G_LLM_MOUSE_BUTTONS_PREV = 0, 0x00456495");
        ck_eq((uint32_t)fx.ui_selected_bldg_index, 0u,
              "T1: _G_LLM_STRAT_UI_SELECTED_BLDG_INDEX = 0, 0x004564a6");
        ck_eq((uint32_t)fx.click_select_target_id, 0u, "T1: _G_LLM_CLICK_SELECT_TARGET_ID = 0, 0x004564af");
    }

    // =================================================================================================
    // T2 -- the progress row bound is DYNAMIC, read fresh off cfg_inventions[0].index, not a hardcoded
    // constant: with the row limit set to 0, NO row of ANY player's progress table is touched.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();

        fx.game_clock               = 12.0;
        fx.storage_stats_init_delay = 3.0;
        fx.cfg_inventions[0].index  = 0; // Progress[0].index -- the row loop bound

        for (int32_t p = 0; p < MAX_PLAYERS; ++p) {
            auto &pr     = fx.progress[(size_t)(p * PROGRESS_ROW_COUNT)];
            pr.available = 1;
            pr.acquired  = 1;
            pr.f3        = 1;
        }

        sim_view  v   = fx.view();
        sim_store own = fx.store();
        detail::map_fill_defaults(v, own, g_calls);

        for (int32_t p = 0; p < MAX_PLAYERS; ++p) {
            auto &pr = fx.progress[(size_t)(p * PROGRESS_ROW_COUNT)];
            char  msg[220];
            std::snprintf(msg, sizeof(msg),
                          "T2: Progress[0].index == 0 -> the row loop body never runs for player %d "
                          "(`JG` @0x004563fe not taken) -- progress[%d][0] stays at its seeded sentinel, "
                          "0x004563f4/0x004563fb",
                          p, p);
            ck(pr.available == 1 && pr.acquired == 1 && pr.f3 == 1, msg);
        }
    }
}

} // namespace mh::sim::test
