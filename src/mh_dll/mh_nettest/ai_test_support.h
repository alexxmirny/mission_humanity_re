//
// ai_test_support.h -- the `aitest` fixture and check helper, shared by every ai test TU.
//
// Extracted from ai_selftest.cpp on 2026-08-29 (AI1D verification-debt drain). Nothing here is new:
// the counters, `ck`, `trace_on` and `struct fixture` are the ones ai_selftest.cpp has carried since
// AI0, moved VERBATIM -- the only edits are the namespace and the `inline` keywords. It exists for
// the same reason sim_test_support.h does: a single 12k-line selftest file is one file, and one file
// is exactly the collision that stops several authors writing oracles at once.
//
// THE COUNTERS ARE INLINE VARIABLES, i.e. ONE PER PROGRAM, not one per TU. That is the point: every
// TU's cases add into the same totals and `run_aitest` prints one number at the end. A `static` here
// would silently give each TU its own counter and the summary would report only ai_selftest.cpp's.
//
// TWO RULES THE FIXTURE FOLLOWS, both learned expensively and both repeated in the member comments:
//   SEED WITH DISTINCT, NON-SYMMETRIC VALUES -- two fields holding the same number make a swapped
//   translation pass (promo_add/promo_sub are 3 and 7 although the shipped AI.SCR ships both as 1).
//   SIZE BUFFERS WITH PARENTHESES, NOT BRACES -- `std::vector<uint32_t> v{128}` is a ONE-element
//   vector holding 128, which cost aitest a 0xC0000374 on ~60% of runs before anyone found it.
//
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ai/ai_state.h"

namespace mh::ai::test {

inline int g_checks = 0, g_fails = 0;

// Set AITEST_TRACE=1 to print and FLUSH every check as it is reached.
//
// This exists for the mutation campaigns. stdout is fully buffered here, so a mutation that makes a
// body read out of bounds takes the whole transcript down with it: the run reports no FAIL lines and
// no "N checks" tail, and tools/mutate.py can only score that as MISSED -- indistinguishable from an
// assertion too weak to notice. It bit the build_sources campaign (which fixed its one case by
// making the mutant terminate) and again the queue-dispatch campaign, at which point paying for the
// answer once was cheaper than diagnosing it a third time. With tracing on, the last line printed is
// the check that was in flight when the process died.
inline int trace_on() {
    static const int on = [] {
        const char *e = getenv("AITEST_TRACE");
        return (e != nullptr && *e != '\0' && *e != '0') ? 1 : 0;
    }();
    return on;
}

inline void ck(bool ok, const char *what) {
    ++g_checks;
    if (trace_on()) {
        printf("  [%3d] %s: %s\n", g_checks, ok ? "ok  " : "FAIL", what);
        fflush(stdout);
    }
    if (!ok) {
        ++g_fails;
        if (!trace_on()) printf("  FAIL: %s\n", what);
        fflush(stdout);
    }
}

using namespace mh::ai;

// ---- the fixture --------------------------------------------------------------------------------
//
// NINE player records, not eight. llm_strat_ai_init_build_candidate_priorities writes four slots
// through `players[player + 1]`, so a fixture sized to the real array would make the very behaviour
// under test an out-of-bounds write into the test's own heap. The ninth record IS the .bss the
// original scribbles on for player 7, and having it lets that write be OBSERVED rather than merely
// tolerated.
struct fixture {
    std::vector<player_data> players{MAX_PLAYERS + 1};
    // ---- batch C layer 1 ----
    // The SESSION player records (_G_LLM_STRAT_PLAYERS), a DIFFERENT table from `players` above.
    // Zeroed by reset(), i.e. status_flags starts with ALIVE CLEAR -- so a test that forgets to set
    // it gets the early return rather than a silently-passing body.
    std::vector<player_profile>   profiles{MAX_PLAYERS};
    std::vector<unit>             units{(size_t)MAX_PLAYERS * UNITS_PER_PLAYER};
    std::vector<building>         buildings{(size_t)MAX_PLAYERS * BUILDINGS_PER_PLAYER};
    std::vector<engage_candidate> scratch{ENGAGE_SCRATCH_CAP};
    int32_t                       scratch_count = 0;
    // ---- the two scan/attack scratch counters active_unit_tick writes directly ----
    // active_unit_tick writes these two scalars DIRECTLY (not through gc): scan_target_count is
    // zeroed at the top of every group's scan dispatch, attack_candidate_count at the top of the
    // shared per-group body. Bound here so the translation has somewhere real to write; the two
    // BACKING ARRAYS (_G_LLM_STRAT_AI_SCAN_TARGETS / _G_LLM_STRAT_AI_ATTACK_CANDIDATES) are
    // deliberately NOT bound -- this offline suite's active_unit_tick coverage stops at the goal
    // dispatch and the per-unit walk (both of which stay entirely inside the stub scan producers,
    // never populate a real target, so scan_target_count never leaves 0 and the sort+scoring tail
    // that WOULD dereference the arrays is never reached). See the test's own header comment.
    int32_t scan_target_count_v      = 0;
    int32_t attack_candidate_count_v = 0;
    // The two CONFIG tables and the three scalars batch A layer 1 added to the view. The map dims
    // are deliberately NOT powers-of-two-minus-one traps: 64 x 48 is rectangular, so a translation
    // that swaps width and height (or x and y) shows up instead of cancelling out.
    std::vector<cfg_building> cfg_buildings{BUILDING_TYPE_COUNT};
    std::vector<cfg_weapon>   cfg_weapons{WEAPON_TYPE_COUNT};
    int32_t                   map_w = 64, map_h = 48;
    int32_t                   active_players = 0;
    // ---- batch A layer 4 ----
    // The production records and the cfg Unit SECTION record (of which only `.total` is read). The
    // section is one object, not a table, so it is a plain member.
    std::vector<production> productions{(size_t)MAX_PLAYERS * PRODUCTIONS_PER_PLAYER};
    cfg_unit_section        unit_sec{};
    // ---- batch A layer 5 ----
    // The shared build-site candidate list and its count. Sized to the real extent so an overrun in
    // a body under test lands in this vector rather than in the next member.
    std::vector<site_candidate> site_cands{(size_t)SITE_CANDIDATE_CAP};
    int32_t                     site_count = 0;
    // ---- batch A layer 6 ----
    // The torus wrap-mask scratch (_G_LLM_STRAT_AI_GRID_WRAP_MASK). It is BOTH a view field and a
    // store field on the real binding, and the stencil pair stores it and reads it back inside its
    // own loop -- so the fixture must alias the two, exactly as state() does.
    uint32_t wrap_mask = 0;
    // ---- batch B layer 0 ----
    // _G_LLM_STRAT_AI_FOREIGN_BLDG_CHANGE_FLAG. Read-only to the AI, and the ONE input of
    // register_attacker_damage that is neither a parameter nor a player_data field -- so it is the
    // input a translation is most likely to drop. Defaults to 0, the branch the game spends most of
    // its time on.
    int32_t foreign_flag = 0;
    // ---- batch C layer 2 (2026-08-05) ----
    // game::t::Player_s (`PlayerSide`), the LOCAL player index. Deliberately 7 rather than 0: the
    // tests use low slots as their actor, so the default puts every body's `player == PlayerSide`
    // gate on the NOT-local arm and a test wanting the local arm has to say so.
    uint16_t player_side = 7;
    // ---- batch B layer 2 ----
    // The cfg Unit TABLE (Unit[100]) -- in the view since batch A layer 2 but never bound by this
    // fixture until now, because no tested body had read it. queue_flush_unit_train_entries_2 maps a
    // queue entry's unit id to Unit[id].ai_unit, so it does.
    std::vector<cfg_unit> cfg_units{UNIT_TYPE_COUNT};
    // _G_LLM_STRAT_AI_RESOURCE_SPEND_WEIGHTS, int[4]. Deliberately left at the four DISTINCT values
    // set in reset() rather than all-ones: an equal-weight table cannot separate a translation that
    // indexes the weight by the wrong axis.
    int32_t spend_weights[4] = {0, 0, 0, 0};
    // The invention pair and the two remaining cfg SECTION records, added for
    // llm_strat_ai_plan_unit_training. `inventions` is the shared CFG table (Ghidra `Progress`),
    // `progress` the per-player acquisition state (Ghidra `progress`) -- two different objects whose
    // labels differ only in case, which is exactly the confusion a fixture should make impossible.
    // The two sections carry only `.total`, and both are used as INCLUSIVE loop bounds.
    // ---- the group relocation scratch list the two harvest bodies publish ----
    // _G_LLM_STRAT_AI_GROUP_UNIT_SCRATCH_LIST/_COUNT -- the member-id list the two harvest bodies
    // (group_task_advance_to_anchor / group_task_disperse_passable) PUBLISH for their mover to read
    // back. Sized one past the real 100-entry extent so a harvest that runs one too long lands in
    // this vector's own tail rather than in the next member, and the count assertion still catches it.
    // PARENTHESES, NOT BRACES -- see the ring_counts note below. Written `{101}` when it was added
    // (2026-08-06 layer 3) it was ONE element holding 101, and the very first harvest test writes
    // three, which is what made `aitest` crash 3/20 with 0xC0000374 until 2026-08-06.
    std::vector<int32_t>         group_scratch       = std::vector<int32_t>(101, 0);
    int32_t                      group_scratch_count = 0;
    std::vector<cfg_invention>   inventions{(size_t)PROGRESS_ROW_COUNT};
    std::vector<player_progress> progress{(size_t)MAX_PLAYERS * PROGRESS_ROW_COUNT};
    cfg_building_section         bldg_sec{};
    cfg_progress_section         prog_sec{};
    // ---- batch B layer 3 ----
    // The per-player housing ledger and the AI.SCR train-queue cap. The four housing classes are
    // given DISTINCT default capacities in reset() rather than one shared number, so a translation
    // that pairs a used_* column with the wrong cap_prev_* column cannot pass by coincidence.
    std::vector<housing_stats> housing{(size_t)MAX_PLAYERS};
    uint32_t                   train_cap = 0;
    // ---- batch B layer 3, the build-queue dispatch arms ----
    // The terrain/occupancy plane the footprint test walks. byte[256][256], COLUMN-major, index
    // (x << 8) | y -- sized to the real extent so a body that walked it row-major lands inside this
    // vector rather than in the next member. Nothing here READS it; it is passed straight through to
    // llm_scan_masked_table_for_empty_cell, so what the tests assert about it is the POINTER and the
    // (width, height) pair that travel beside it.
    // PARENTHESES, not braces: with a scalar element type `{n}` is a one-element initializer list,
    // which would give a 1-byte plane and let a footprint walk run off it.
    std::vector<uint8_t> passable = std::vector<uint8_t>((size_t)256 * 256, 0);
    // _G_LLM_STRAT_AI_CFG_PROMO_ADD / _SUB. Deliberately given DIFFERENT default values here even
    // though the shipped AI.SCR sets both to 1: with both at 1 a translation that swapped them --
    // and their arithmetic IS inverted relative to their names, so a swap is the likely error --
    // would pass every credit assertion by coincidence.
    int32_t promo_add = 0, promo_sub = 0;
    // ---- batch B layer 3, the storage/mine planners ----
    // The CAPACITY ledger and the HOLDINGS array -- two different objects with two different
    // strides, and getting them the wrong way round is the exact error the original's plate made,
    // so they are separate members with separate defaults rather than one shared table.
    std::vector<storage_stats> storage{(size_t)MAX_PLAYERS};
    std::vector<int32_t>       resources =
        std::vector<int32_t>((size_t)MAX_PLAYERS * RESOURCE_SLOTS_PER_PLAYER, 0);
    // AI.SCR fSiloRatio. Defaulted to the shipped 1.1 so a test that forgets to set it still
    // exercises the realistic threshold rather than 0 (which would make NOTHING short, i.e. the
    // vacuous direction).
    float silo = 1.1f;
    // _G_LLM_STRAT_AI_MINE_REBALANCE_MIN_COUNT. Seeded to a POISON value in reset() rather than 0,
    // so a body that never writes it is distinguishable from one that writes 2*base+1 when base
    // happens to be 0 -- which, with an all-zero fixture, is the common case.
    int32_t mine_min = 0;
    // ---- batch B layer 3, the turret / worker planners (2026-08-03) ----
    // The population ledger. Its three read columns are given DISTINCT defaults in reset() for the
    // usual reason: with pop_total == housing_prev the reserve is forced to zero, which is one of the
    // branches under test, so an all-equal fixture would take it by accident on every case.
    std::vector<pop_stats> pop{(size_t)MAX_PLAYERS};
    // The four AI.SCR scalars. Defaulted to the SHIPPED values (50 / 0.2 / 0.3 / 1.1) so a test that
    // forgets one still runs against the real thresholds rather than against 0, which for
    // unemployed_ratio would make the reserve identically zero -- the vacuous direction.
    int32_t unemployed_min   = 0;
    float   unemployed_ratio = 0.0f;
    float   max_fuck_ratio   = 0.0f;
    float   extra_space      = 0.0f;
    // The two per-building connectivity planes. Sized to the real extent, and the flood-fill stub
    // writes into whichever one it is handed -- so a translation that passed the same plane twice
    // makes the cut-vertex comparison trivially equal and the test that names it fails.
    std::vector<uint8_t> conn_base  = std::vector<uint8_t>((size_t)BUILDINGS_PER_PLAYER, 0);
    std::vector<uint8_t> conn_trial = std::vector<uint8_t>((size_t)BUILDINGS_PER_PLAYER, 0);
    // The map plane and the two wrap MASKS the turret planner's spiral scan reads, plus the spiral
    // table itself. Bound here for the first time -- no earlier tested body read them. The masks are
    // extent-1 and the extents are therefore powers of two here (128 x 64), UNLIKE map_w / map_h
    // above, because AND-masking only works on powers of two; the two pairs are deliberately
    // different numbers so a body that used the extent where the mask belongs is visible.
    std::vector<tile_object> tiles  = std::vector<tile_object>((size_t)256 * 256);
    uint32_t                 map_wm = 0, map_hm = 0;
    // AI1-close (2026-08-30). _G_LLM_STRAT_AI_BUILDING_CANDIDATE_SCRATCH_LIST[256] / _COUNT, the
    // reset-then-fill pair llm_strat_ai_group_collect_buildings_of_types appends into. Bound here
    // for the first time -- the batch-C row was closed at T3 on a ZERO-CALL shadow site, so this
    // fixture binding is the compensating test AI1's done_when asks for. Sized to the real 256-entry
    // extent and NOT padded: the original has no capacity check (ai_group_building_scan.h), so an
    // append that runs past the end must land where a sanitiser sees it, not in quiet headroom.
    // PARENTHESES, NOT BRACES -- scalar element type; see the ring_counts note below.
    std::vector<int32_t> bldg_cands      = std::vector<int32_t>(256, 0);
    int32_t              bldg_cand_count = 0;
    // AI1D (2026-08-28). The three AI.SCR scheduler periods. Seeded in reset() to DISTINCT,
    // non-symmetric values on purpose -- three clocks stepped by the same number make a swapped
    // period pass, which is exactly the mutation a players_tick oracle has to be able to catch.
    float ai_strategy_period = 0.0f, ai_tactic_period = 0.0f, ai_move_period = 0.0f;
    // PARENTHESES, NOT BRACES. `std::vector<uint32_t> ring_counts{128}` is the initializer_list
    // constructor: ONE element whose value is 128, not 128 elements. It read as a size for a year
    // and cost a day: test_plan_turret_upgrade writes ring_counts[15], i.e. 60 bytes past a 4-byte
    // allocation, which corrupted the heap and made `aitest` crash on a LARGE FRACTION of runs
    // (measured 5/8) with 0xC0000374 -- while still printing "712 checks, 0 failures" whenever it
    // survived. The gate runs each selftest once, so it read green throughout. Found 2026-08-03 with
    // page heap.
    //
    // The trap only fires for SCALAR element types: for a struct like spiral_offset there is no
    // implicit int -> element conversion, so `spiral{4096}` falls back to the count constructor and
    // really is 4096 entries. That asymmetry is exactly why it hid -- the line above it was fine.
    std::vector<spiral_offset> spiral      = std::vector<spiral_offset>(4096);
    std::vector<uint32_t>      ring_counts = std::vector<uint32_t>(128, 0);
    // ---- batch B layers 4 and 6, the mine-worth gate (2026-08-03) ----
    // The map's coarse resource plane, map::resources[64][64]. PARENTHESES for the same reason as
    // ring_counts above, and sized to the real 64*64 extent so a body that indexed it [y][x] on a
    // deliberately ASYMMETRIC fixture lands on a cell this file can assert about rather than off the
    // end. (64x64 is the real shape, so the x/y swap has to be caught by CONTENT, not by extent --
    // see the paired cells test_site_worth seeds.)
    std::vector<map_resources> res_plane = std::vector<map_resources>((size_t)64 * 64);
    // _G_LLM_STRAT_AI_CFG_MINE_WORTH, the AI.SCR `nMineWorth`. Defaulted in reset() to the SHIPPED
    // 5000 rather than 0: at 0 the unsigned `total >= threshold` is true for every input, which is
    // the vacuous direction and would make half the assertions here pass on any body.
    int32_t mine_worth = 0;
    // _G_LLM_STRAT_AI_RESOURCE_VALUE_WEIGHTS, int[4] for resource ids 1..4 -- ONE-BASED in the
    // original's addressing, so element [0] is id 1. Defaulted to the image's {4, 2, 2, 1}: four
    // DISTINCT values, so a translation that indexed the weight by the wrong id cannot pass by
    // coincidence, and so that the off-by-one this table exists to document (reading id 0 out of
    // _G_LLM_STRAT_AI_ACTIVE_PLAYER_COUNT, or running one past the end at id 4) changes the answer.
    int32_t value_weights[4] = {0, 0, 0, 0};
    // ---- batch B layer 4/5, the mine portfolio (2026-08-05) ----
    // The three PARALLEL scratch tables, at their real 32-row extent. PARENTHESES for `mine_roster`
    // for the same reason as ring_counts above -- it is a scalar element type, so braces would give
    // a ONE-element vector and every collect past the first would corrupt the heap.
    //
    // Sized to 32 EXACTLY and not padded, deliberately: the original has no bound here (the collect
    // walk is driven by the 100-slot roster), so a fixture with headroom would quietly absorb the
    // overrun the header documents instead of letting a sanitiser see it. No test below drives more
    // than a handful of mines.
    std::vector<int32_t>      mine_roster = std::vector<int32_t>((size_t)MINE_SCRATCH_ROWS, 0);
    std::vector<mine_quality> mine_qual   = std::vector<mine_quality>((size_t)MINE_SCRATCH_ROWS);
    std::vector<mine_yield>   mine_yields = std::vector<mine_yield>((size_t)MINE_SCRATCH_ROWS);
    // The 5x5 sampling kernel. reset() seeds it with the SHIPPED table, read out of the image
    // 2026-08-05 (centre 1.0, the 8 ring-1 cells 0.5, the 16 ring-2 cells 0.1, in that order) -- NOT
    // an invented table, because the three bucket thresholds the estimator compares against ARE
    // these three weights minus an epsilon, so a made-up weight would make the bucket tests
    // untestable. The ORDER matters as much as the values: the scan stops at the first hit, so
    // "nearest wins" is a property of the table's ordering, and a shuffled table would pass a body
    // that scanned backwards.
    std::vector<mine_kernel_cell> mine_kernel =
        std::vector<mine_kernel_cell>((size_t)MINE_KERNEL_CELLS);
    // _G_LLM_STRAT_AI_MINE_LOW_SHARE_PCT_THRESHOLD. Seeded to the image's 10 rather than 0: at 0 the
    // unsigned `share >= threshold` is true for every input, so the FIRST keep test would fire on
    // every mine and none could ever be retired -- the vacuous direction.
    int32_t mine_low_share = 0;
    // ---- batch C layer 2, the group-formation tuning scalars (2026-08-05) ----
    // The four .data constants named in EN v223. reset() seeds them with the IMAGE values, which
    // happen to be four distinct numbers (3 / 2 / 100000 / 5), so a translation that read one where
    // another belongs cannot pass by coincidence. patrol_bounces is deliberately the 100000 the
    // image ships rather than something small: what it exercises is the 16-bit TRUNCATION on the way
    // into the task record's sub_code, and a value under 0x8000 would hide it.
    int32_t patrol_size = 0, patrol_max = 0, patrol_bounces = 0, surplus_min = 0;

    fixture() {
        // The fixture's whole purpose is bounds-correct buffers standing in for fixed-extent
        // globals, so it checks its own extents rather than trusting a declaration to say what it
        // means. These are `ck` and not `assert` DELIBERATELY: this file is built Release with
        // NDEBUG, where assert() is a no-op, so an assert here would document the invariant while
        // enforcing nothing -- in the exact build the gate runs.
        ck(ring_counts.size() == 128, "fixture: ring_counts extent is the real uint[128]");
        ck(spiral.size() == 4096, "fixture: spiral extent is the fixture's 4096-entry table");
        ck(conn_base.size() == (size_t)BUILDINGS_PER_PLAYER, "fixture: conn_base extent");
        ck(conn_trial.size() == (size_t)BUILDINGS_PER_PLAYER, "fixture: conn_trial extent");
        ck(passable.size() == (size_t)256 * 256, "fixture: passable extent");
        ck(tiles.size() == (size_t)256 * 256, "fixture: tiles extent");
        ck(mine_roster.size() == (size_t)MINE_SCRATCH_ROWS, "fixture: mine_roster extent");
        ck(mine_qual.size() == (size_t)MINE_SCRATCH_ROWS, "fixture: mine_qual extent");
        ck(mine_yields.size() == (size_t)MINE_SCRATCH_ROWS, "fixture: mine_yields extent");
        ck(mine_kernel.size() == (size_t)MINE_KERNEL_CELLS, "fixture: mine_kernel extent");
        ck(bldg_cands.size() == 256, "fixture: bldg_cands extent is the real int32[256]");
        reset();
    }

    void reset() {
        std::memset(players.data(), 0, players.size() * sizeof(player_data));
        std::memset(profiles.data(), 0, profiles.size() * sizeof(player_profile));
        std::memset(units.data(), 0, units.size() * sizeof(unit));
        std::memset(buildings.data(), 0, buildings.size() * sizeof(building));
        std::memset(scratch.data(), 0, scratch.size() * sizeof(engage_candidate));
        std::memset(cfg_buildings.data(), 0, cfg_buildings.size() * sizeof(cfg_building));
        std::memset(cfg_weapons.data(), 0, cfg_weapons.size() * sizeof(cfg_weapon));
        std::memset(productions.data(), 0, productions.size() * sizeof(production));
        std::memset(&unit_sec, 0, sizeof(unit_sec));
        std::memset(site_cands.data(), 0, site_cands.size() * sizeof(site_candidate));
        scratch_count  = 0;
        active_players = 0;
        site_count     = 0;
        wrap_mask      = 0;
        foreign_flag   = 0;
        player_side    = 7; // see the member comment: NOT any test's actor by default
        std::memset(cfg_units.data(), 0, cfg_units.size() * sizeof(cfg_unit));
        spend_weights[0] = 1;
        spend_weights[1] = 10;
        spend_weights[2] = 100;
        spend_weights[3] = 1000;
        std::memset(inventions.data(), 0, inventions.size() * sizeof(cfg_invention));
        std::memset(progress.data(), 0, progress.size() * sizeof(player_progress));
        std::memset(&bldg_sec, 0, sizeof(bldg_sec));
        std::memset(&prog_sec, 0, sizeof(prog_sec));
        std::memset(housing.data(), 0, housing.size() * sizeof(housing_stats));
        train_cap = 0;
        std::memset(passable.data(), 0, passable.size());
        promo_add = 3; // see the member comment: distinct, so a swap cannot pass by coincidence
        promo_sub = 7;
        std::memset(storage.data(), 0, storage.size() * sizeof(storage_stats));
        std::fill(resources.begin(), resources.end(), 0);
        silo     = 1.1f;
        mine_min = -12345; // poison; see the member comment
        std::memset(pop.data(), 0, pop.size() * sizeof(pop_stats));
        unemployed_min   = 50;   // the shipped nUnemployedMin
        unemployed_ratio = 0.2f; // fUnemployedRatio
        max_fuck_ratio   = 0.3f; // fMaxFuckRatio  -- a FLOOR on ai_labor_utilization, despite the name
        extra_space      = 1.1f; // fExtraSpace    -- a housing-headroom threshold
        std::fill(conn_base.begin(), conn_base.end(), (uint8_t)0);
        std::fill(conn_trial.begin(), conn_trial.end(), (uint8_t)0);
        std::memset(tiles.data(), 0, tiles.size() * sizeof(tile_object));
        map_wm             = 127;  // 128 x 64, i.e. extent-1 -- see the member comment on why not 64 x 48
        ai_strategy_period = 3.0f; // distinct and non-symmetric -- see the member comment
        ai_tactic_period   = 5.0f;
        ai_move_period     = 7.0f;
        map_hm             = 63;
        std::memset(spiral.data(), 0, spiral.size() * sizeof(spiral_offset));
        std::fill(ring_counts.begin(), ring_counts.end(), (uint32_t)0);
        std::memset(res_plane.data(), 0, res_plane.size() * sizeof(map_resources));
        mine_worth       = 5000; // the shipped nMineWorth, NOT the image's 3000 -- see the member
        value_weights[0] = 4;    // id 1  -- the image's {4, 2, 2, 1}
        value_weights[1] = 2;    // id 2
        value_weights[2] = 2;    // id 3
        value_weights[3] = 1;    // id 4
        std::fill(mine_roster.begin(), mine_roster.end(), 0);
        std::memset(mine_qual.data(), 0, mine_qual.size() * sizeof(mine_quality));
        std::memset(mine_yields.data(), 0, mine_yields.size() * sizeof(mine_yield));
        seed_mine_kernel();
        mine_low_share = 10; // the image value at 0x006693ac
        std::fill(bldg_cands.begin(), bldg_cands.end(), 0);
        bldg_cand_count = 0;
        patrol_size     = 3;      // _G_LLM_STRAT_AI_PATROL_GROUP_SIZE       @0x00669344
        patrol_max      = 2;      // _G_LLM_STRAT_AI_PATROL_GROUP_MAX        @0x00669348
        patrol_bounces  = 100000; // _G_LLM_STRAT_AI_PATROL_WANDER_BOUNCES   @0x0066934c
        surplus_min     = 5;      // _G_LLM_STRAT_AI_SURPLUS_GROUP_MIN_POOL4 @0x006693a0
    }

    // The shipped 25-cell kernel, dumped from /eng/mh.exe @0x006616bc on 2026-08-05. Ring 0 first,
    // then the 8 Chebyshev-ring-1 cells, then the 16 ring-2 cells -- which is what makes "the
    // NEAREST deposit wins" true of a walk that simply stops at the first hit.
    void seed_mine_kernel() {
        static const int32_t RING1[8][2]  = {{-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, -1}, {1, 0}, {1, 1}};
        static const int32_t RING2[16][2] = {{-2, -2}, {-2, -1}, {-2, 0}, {-2, 1}, {-2, 2}, {-1, -2}, {-1, 2}, {0, -2}, {0, 2}, {1, -2}, {1, 2}, {2, -2}, {2, -1}, {2, 0}, {2, 1}, {2, 2}};
        mine_kernel[0]                    = {0, 0, 1.0};
        for (int i = 0; i < 8; ++i) mine_kernel[1 + i] = {RING1[i][0], RING1[i][1], 0.5};
        for (int i = 0; i < 16; ++i) mine_kernel[9 + i] = {RING2[i][0], RING2[i][1], 0.1};
    }

    ai_view view() const {
        ai_view v{};
        v.players       = players.data();
        v.strat_players = profiles.data();
        // batch C layer 2 (2026-08-05). The LOCAL player index. Defaults to a slot NO test uses as
        // its actor (reset() sets it), so a body gated on `player == PlayerSide` takes the
        // NOT-local arm unless a test says otherwise -- the arm that is harder to notice missing.
        v.player_side            = &player_side;
        v.units                  = units.data();
        v.buildings              = buildings.data();
        v.engage_scratch         = scratch.data();
        v.engage_scratch_count   = &scratch_count;
        v.scan_target_count      = &scan_target_count_v;
        v.attack_candidate_count = &attack_candidate_count_v;
        v.cfg_buildings          = cfg_buildings.data();
        v.cfg_weapons            = cfg_weapons.data();
        v.map_width              = &map_w;
        // AI1D: the three scheduler-period floats llm_strat_ai_players_tick steps its clocks by.
        v.ai_strategy_period               = &ai_strategy_period;
        v.ai_tactic_period                 = &ai_tactic_period;
        v.ai_move_period                   = &ai_move_period;
        v.map_height                       = &map_h;
        v.active_player_count              = &active_players;
        v.productions                      = productions.data();
        v.cfg_unit_sec                     = &unit_sec;
        v.site_candidates                  = site_cands.data();
        v.site_candidate_count             = &site_count;
        v.grid_wrap_mask                   = &wrap_mask;
        v.foreign_bldg_change_flag         = &foreign_flag;
        v.cfg_units                        = cfg_units.data();
        v.spend_weights                    = spend_weights;
        v.cfg_inventions                   = inventions.data();
        v.progress                         = progress.data();
        v.cfg_building_sec                 = &bldg_sec;
        v.cfg_progress_sec                 = &prog_sec;
        v.unit_housing                     = housing.data();
        v.train_queue_per_unit_cap         = &train_cap;
        v.passable                         = passable.data();
        v.promo_add                        = &promo_add;
        v.promo_sub                        = &promo_sub;
        v.storage                          = storage.data();
        v.player_resources                 = resources.data();
        v.silo_ratio                       = &silo;
        v.mine_rebalance_min_count         = &mine_min;
        v.pop                              = pop.data();
        v.unemployed_min                   = &unemployed_min;
        v.unemployed_ratio                 = &unemployed_ratio;
        v.max_fuck_ratio                   = &max_fuck_ratio;
        v.extra_space                      = &extra_space;
        v.bldg_connectivity_base           = conn_base.data();
        v.bldg_connectivity_trial          = conn_trial.data();
        v.tile_objects                     = tiles.data();
        v.map_width_mask                   = &map_wm;
        v.map_height_mask                  = &map_hm;
        v.spiral_offsets                   = spiral.data();
        v.spiral_ring_cell_counts          = ring_counts.data();
        v.resources                        = res_plane.data();
        v.mine_worth                       = &mine_worth;
        v.resource_value_weights           = value_weights;
        v.mine_roster_index                = mine_roster.data();
        v.mine_quality_hist                = mine_qual.data();
        v.mine_yield_estimate              = mine_yields.data();
        v.mine_yield_kernel                = mine_kernel.data();
        v.mine_low_share_pct_threshold     = &mine_low_share;
        v.building_candidate_scratch_list  = bldg_cands.data();
        v.building_candidate_scratch_count = &bldg_cand_count;
        v.patrol_group_size                = &patrol_size;
        v.patrol_group_max                 = &patrol_max;
        v.patrol_wander_bounces            = &patrol_bounces;
        v.surplus_group_min_pool4          = &surplus_min;
        return v;
    }
    ai_store store() {
        ai_store s{};
        s.players              = players.data();
        s.engage_scratch       = scratch.data();
        s.engage_scratch_count = &scratch_count;
        // Aliased with the view members above, same arrangement as wrap_mask elsewhere in this
        // fixture: active_unit_tick writes scan_target_count/attack_candidate_count directly (not
        // through gc), so the two storage-side pointers must be the SAME cells the view reads back.
        s.scan_target_count      = &scan_target_count_v;
        s.attack_candidate_count = &attack_candidate_count_v;
        s.site_candidates        = site_cands.data();
        s.site_candidate_count   = &site_count;
        s.grid_wrap_mask         = &wrap_mask;
        // Aliased with the view member above, exactly as state() binds it: the mine planner stores
        // it and then a callee reads it back, so a fixture that gave the two different storage
        // could not observe the ordering constraint at all.
        s.mine_rebalance_min_count = &mine_min;
        // Aliased with the view members above for the same reason as the mine scratch: the turret
        // planner hands these two planes to the flood fill through the STORE and then reads them back
        // through the VIEW, so a fixture that gave the two different storage could not observe the
        // cut-vertex comparison at all.
        s.bldg_connectivity_base  = conn_base.data();
        s.bldg_connectivity_trial = conn_trial.data();
        // Aliased with the view members for the third time and the same reason: the rebalance fills
        // row i through the STORE and reads it back through the VIEW inside the same call, so
        // separate storage would make every retire decision score against zeros.
        s.mine_roster_index              = mine_roster.data();
        s.mine_quality_hist              = mine_qual.data();
        s.mine_yield_estimate            = mine_yields.data();
        s.group_relocation_scratch_list  = group_scratch.data();
        s.group_relocation_scratch_count = &group_scratch_count;
        // ---- RI-AI batch D / AI1D (2026-08-28) ----
        // The field-scoped roster window over the SAME two vectors the view publishes as const, so a
        // batch-D body's write is observable through v.units / v.buildings in the same test. Giving
        // it separate storage would make every membership assertion score against a second roster
        // nothing else reads -- the vacuity these aliasing comments keep warning about.
        s.roster.bind(units.data(), buildings.data());
        // Aliased with the view members above for the same reason as every other scratch pair here:
        // group_collect_buildings_of_types APPENDS through the STORE and the test reads the result
        // back through the VIEW, so separate storage would make every append assertion vacuous.
        s.building_candidate_scratch_list  = bldg_cands.data();
        s.building_candidate_scratch_count = &bldg_cand_count;
        // Aliased with v.map_width_mask / v.map_height_mask for the same reason: players_tick writes
        // extent-1 into the very cells every masked coordinate later reads back.
        s.map_width_mask  = &map_wm;
        s.map_height_mask = &map_hm;
        return s;
    }

    player_progress &prog(int p, int row) {
        return progress[(size_t)p * PROGRESS_ROW_COUNT + row];
    }
    unit       &u(int p, int i) { return units[(size_t)p * UNITS_PER_PLAYER + i]; }
    building   &b(int p, int i) { return buildings[(size_t)p * BUILDINGS_PER_PLAYER + i]; }
    production &prod(int p, int slot) {
        return productions[(size_t)p * PRODUCTIONS_PER_PLAYER + slot];
    }
    // Append a build-site candidate, exactly the way the three scanners do (append at the inherited
    // count, then increment) so a test can set up an inherited list without going through one.
    void push_site(int32_t x, int32_t y, int32_t kind, uint32_t dist_sq) {
        site_cands[site_count].tile_x  = x;
        site_cands[site_count].tile_y  = y;
        site_cands[site_count].kind    = kind;
        site_cands[site_count].dist_sq = dist_sq;
        ++site_count;
    }
    void push(uint32_t ref, int32_t idx) {
        scratch[scratch_count].target_ref   = ref;
        scratch[scratch_count].target_index = idx;
        scratch[scratch_count].dist_sq      = 0;
        ++scratch_count;
    }
};

} // namespace mh::ai::test
