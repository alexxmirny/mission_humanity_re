//
// sim_spawn_ai_base_selftest.cpp -- `simtest` offline oracle for llm_strat_spawn_ai_base
// @0x004dcd0e (sim/resid/sim_spawn_ai_base.h/.cpp, RI-SIM / sim_resid batch E).
//
// NO SHADOW SITE (sim_resid rule 1) -- this offline oracle is the only verification. The sibling it
// calls, llm_strat_sort_sites_by_dist, has its own oracle
// (sim_sort_sites_by_dist_selftest.cpp); here it is driven FOR REAL (same-TU G21 call), so the site
// ordering this file asserts is the two functions composed, not a mock.
//
// Expected behaviour hand-derived from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_spawn_ai_base_004dcd0e.asm). The load-bearing structure:
//
//   0x004dcd24-d50  TWO scripts parsed unconditionally, in order: the fixed "init\AI.SCR" then
//                   sprintf("init\AI%02d.SCR", G_PLANET_INDEX). There is NO branch between them.
//   0x004dcd55-d69  ai_active_player_count is raised to player+1 only when it is <= player.
//   0x004dcd6e-d7f  profile[player].status_flags |= 0x2 (ALIVE) -- an OR, not an assignment.
//   0x004dcd9b-def8 the header scalars, in the original's write order.
//   0x004dce39-e8f  ai_clock = 0.001f, then three per-player STAGGERED clocks:
//                   player * <period> * STAGGER_SCALE, each with its OWN period.
//   0x004dcfb5-fe2  resource_spend_total[0..7] = 1 -- a divide-safe denominator, NOT a zero-init.
//   0x004dcfed-d048 the AI tile-flag grid seeded from map::g::passable: impassable -> 8, else 0.
//                   OUTER loop is x to map_width, INNER y to map_height; index (x << 8) | y.
//   0x004dd04c-087  all 32 ai_groups: member_count = 0, current_param = 3.
//   0x004dd08d-0e6  ai_player_relation[i] = +1 toward self, -1 toward everyone else.
//   0x004dd162-296  the coarse 4x4 resource sweep: sum(resources[x4][y4].value[0..7]); a sum
//                   STRICTLY greater than mine_worth records a site (status 0, build tiles -1);
//                   ANY nonzero sum (threshold-independent) OR-stamps 0x80 over the 4x4 footprint.
//   0x004dd296-29c sort_sites_by_dist(player) -- the intra-unit sibling.
//   0x004dd2a1-318 five ai_group_create calls; the goals 1/2/6/6/6 are written by FIXED index.
//   0x004dd321-3ba five task enqueues, params 0x1d3 / 0x183 / 0 / 0 / 0.
//   0x004dd3d9-43a six build-plan pushes, the last reading player+1's ai_housing_candidate_soldier.
//
#include "sim/resid/sim_spawn_ai_base.h"

#include <algorithm> // std::fill over `passable`
#include <cstdio>    // snprintf, in the sprintf mock
#include <cstring>
#include <string> // the parsed-script names are compared as strings

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- mocks --------------------------------------------------------------------------------------

std::vector<std::string> g_scr_parsed;
void                     stub_ai_scr_parse(char *filename) { g_scr_parsed.push_back(filename ? filename : "(null)"); }

// utils_sprintf__vi with the ONE format this function uses. Reproducing %02d rather than calling the
// real vararg thunk keeps the oracle offline; the format string itself is asserted separately.
std::vector<std::string> g_sprintf_fmts;
int32_t                  stub_sprintf_ai_scr(void *dst, const char *fmt, int32_t planet_index) {
    g_sprintf_fmts.push_back(fmt ? fmt : "(null)");
    char *out = (char *)dst;
    // The literal expansion of "init\\AI%02d.SCR".
    int n = snprintf(out, 256, "init\\AI%02d.SCR", planet_index);
    return n;
}

std::vector<int32_t> g_group_create_calls;
int32_t              stub_ai_group_create(int32_t player) {
    g_group_create_calls.push_back(player);
    return (int32_t)g_group_create_calls.size() - 1;
}

struct TaskCall {
    int32_t  player, group;
    uint16_t a2;
    uint32_t p4, p5, p6, p7, p8;
    uint16_t p9;
};
std::vector<TaskCall> g_task_calls;
void                  stub_ai_group_task_enqueue(int32_t player, int32_t group, uint16_t a2, uint32_t p4, uint32_t p5,
                                                 uint32_t p6, uint32_t p7, uint32_t p8, uint16_t p9) {
    g_task_calls.push_back({player, group, a2, p4, p5, p6, p7, p8, p9});
}

std::vector<int32_t> g_init_prio_calls;
void                 stub_ai_init_build_candidate_priorities(int32_t player) { g_init_prio_calls.push_back(player); }

struct PushCall {
    int32_t player, ai_build_id;
};
std::vector<PushCall> g_push_calls;
void                  stub_ai_build_plan_push(int32_t player, int32_t ai_build_id) {
    g_push_calls.push_back({player, ai_build_id});
}

// The real torus distance is not needed to prove this function; the sibling sort's own oracle covers
// the ordering rule. Here it returns a monotone key so the composed call is exercised end to end.
uint32_t stub_toroidal_dist_sq(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    const int32_t dx = x2 - x1, dy = y2 - y1;
    return (uint32_t)(dx * dx + dy * dy);
}

const spawn_ai_base_calls g_calls = {
    &stub_ai_scr_parse,
    &stub_sprintf_ai_scr,
    &stub_ai_group_create,
    &stub_ai_group_task_enqueue,
    &stub_ai_init_build_candidate_priorities,
    &stub_ai_build_plan_push,
    &stub_toroidal_dist_sq,
};

void reset_calls() {
    g_scr_parsed.clear();
    g_sprintf_fmts.clear();
    g_group_create_calls.clear();
    g_task_calls.clear();
    g_init_prio_calls.clear();
    g_push_calls.clear();
}

void run(sim_fixture &fx, int32_t player, int32_t is_alien, int32_t x, int32_t y) {
    reset_calls();
    sim_store own = fx.store();
    detail::spawn_ai_base(fx.view(), own, g_calls, player, is_alien, x, y);
}

// A small map keeps the two O(w*h) sweeps cheap and makes their bounds observable.
void small_map(sim_fixture &fx, int32_t w, int32_t h) {
    fx.map_width  = w;
    fx.map_height = h;
}

} // namespace

void run_spawn_ai_base_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- THE TWO SCRIPTS, IN ORDER, WITH NO BRANCH. The planet index is non-zero and single-digit
    // so the %02d zero-padding is visible; the format string handed to sprintf is asserted verbatim,
    // because a single vs double backslash there is the difference between a path and a literal.
    // =================================================================================================
    {
        fx.reset();
        small_map(fx, 8, 8);
        fx.planet_index = 7;

        run(fx, /*player=*/1, /*is_alien=*/0, /*x=*/10, /*y=*/20);

        ck(g_scr_parsed.size() == 2, "T1: BOTH scripts parsed, unconditionally, 0x004dcd24-d50");
        if (g_scr_parsed.size() == 2) {
            ck(g_scr_parsed[0] == "init\\AI.SCR", "T1: the boot script first, @0x00506d60");
            ck(g_scr_parsed[1] == "init\\AI07.SCR", "T1: then the per-planet one, %02d-padded");
        }
        ck(g_sprintf_fmts.size() == 1, "T1: sprintf called once");
        if (g_sprintf_fmts.size() == 1) {
            ck(g_sprintf_fmts[0] == "init\\AI%02d.SCR", "T1: the format string is the @0x00506d6c literal");
        }
    }

    // =================================================================================================
    // T2 -- THE ACTIVE-PLAYER HIGH-WATER MARK is raised only when it is <= player. Three cases around
    // the boundary, run against the same fixture shape:
    //   count 0 < player 3   -> raised to 4
    //   count 3 == player 3  -> raised to 4 (the compare is <=, so equality RAISES)
    //   count 9 > player 3   -> left at 9
    // =================================================================================================
    {
        const int32_t seeds[3] = {0, 3, 9};
        const int32_t want[3]  = {4, 4, 9};
        for (int32_t k = 0; k < 3; ++k) {
            fx.reset();
            small_map(fx, 8, 8);
            fx.ai_active_player_count = seeds[k];

            run(fx, /*player=*/3, 0, 0, 0);

            ck_eq((uint32_t)fx.ai_active_player_count, (uint32_t)want[k],
                  "T2: ai_active_player_count raised iff count <= player, 0x004dcd55-d69");
        }
    }

    // =================================================================================================
    // T3 -- THE PROFILE ALIVE BIT IS AN OR, not an assignment: an unrelated flag already set must
    // survive. Bit 0x2 goes in; bit 0x10 stays.
    // =================================================================================================
    {
        fx.reset();
        small_map(fx, 8, 8);
        fx.profiles[2].status_flags = 0x10;

        run(fx, /*player=*/2, 0, 0, 0);

        ck_eq((uint32_t)(fx.profiles[2].status_flags & 0x2u), 2u,
              "T3: STATUS_ALIVE set on the player's profile, 0x004dcd6e-d7f");
        ck_eq((uint32_t)(fx.profiles[2].status_flags & 0x10u), 0x10u,
              "T3: the pre-existing flag survives -- it is an OR, not a store");
    }

    // =================================================================================================
    // T4 -- THE HEADER SCALARS. Every one the body writes, checked against a fixture pre-dirtied with
    // a distinctive value, so a MISSING write is a failure rather than an accidental match with zero.
    // =================================================================================================
    {
        fx.reset();
        small_map(fx, 8, 8);
        fx.ai_cfg_start_units = 17;
        player_data &pd       = fx.players[4];
        // Pre-dirty every scalar this function is supposed to write.
        pd.is_alien_race                     = 0x5a;
        pd.ai_home_tile_x                    = 0x5a;
        pd.ai_home_tile_y                    = 0x5a;
        pd.ai_resource_shortage_state        = 0x5a;
        pd.ai_resource_need_score            = 0x5a;
        pd.ai_resource_need_threshold        = 0x5a;
        pd.ai_established                    = 0x5a;
        pd.ai_phase_flags                    = 0x5a;
        pd.ai_enabled                        = 0x5a;
        pd.ai_invasion_force                 = 0x5a;
        pd.ai_patrol_quadrant_cursor         = 0x5a;
        pd.ai_group_count                    = 0x5a;
        pd.ai_target_list_count              = 0x5a;
        pd.next_group_serial                 = 0x5a;
        pd.ai_bldg_queue_count               = 0x5a;
        pd.ai_attack_orders_issued           = 0x5a;
        pd.ai_labor_utilization              = 5.5;
        pd.ai_reposition_cached_member_count = 0x5a;
        pd.ai_start_units_remaining          = 0x5a;
        pd.ai_start_units_pending_spawn      = 0x5a;
        pd.ai_promo_credit                   = 0x5a;
        pd.ai_attack_milestone_index         = 0x5a;
        pd.ai_attack_milestone_clock         = 5.5f;
        pd.ai_map_changed_pending            = 0x5a;
        pd.ai_turret_rescan_pending          = 0x5a;
        pd.ai_spend_ring_cursor              = 0x5a;
        pd.ai_build_plan_cursor              = 0x5a;
        pd.ai_build_plan_len_and_flag        = 0x5a;

        run(fx, /*player=*/4, /*is_alien=*/1, /*x=*/123, /*y=*/45);

        ck_eq((uint32_t)pd.is_alien_race, 1u, "T4: is_alien_race takes the parameter, 0x004dcd9b");
        ck_eq((uint32_t)pd.ai_home_tile_x, 123u, "T4: ai_home_tile_x takes the x parameter");
        ck_eq((uint32_t)pd.ai_home_tile_y, 45u, "T4: ai_home_tile_y takes the y parameter");
        ck_eq((uint32_t)pd.ai_resource_shortage_state, 0u, "T4: ai_resource_shortage_state = 0");
        ck_eq((uint32_t)pd.ai_resource_need_score, 0u, "T4: ai_resource_need_score = 0");
        ck_eq((uint32_t)pd.ai_resource_need_threshold, 6u,
              "T4: ai_resource_need_threshold = 6 -- a literal, not a zero-init");
        ck_eq((uint32_t)pd.ai_established, 0u, "T4: ai_established = 0");
        ck_eq((uint32_t)pd.ai_phase_flags, 7u, "T4: ai_phase_flags = 7, 0x004dcdd5");
        ck_eq((uint32_t)pd.ai_enabled, 1u, "T4: ai_enabled = 1");
        ck_eq((uint32_t)pd.ai_invasion_force, 0u, "T4: ai_invasion_force = 0");
        ck_eq((uint32_t)pd.ai_patrol_quadrant_cursor, 0u, "T4: ai_patrol_quadrant_cursor = 0");
        ck_eq((uint32_t)pd.ai_group_count, 0u, "T4: ai_group_count = 0");
        ck_eq((uint32_t)pd.ai_target_list_count, 0u, "T4: ai_target_list_count = 0");
        ck_eq((uint32_t)pd.next_group_serial, 0u, "T4: next_group_serial = 0");
        ck_eq((uint32_t)pd.ai_bldg_queue_count, 0u, "T4: ai_bldg_queue_count = 0");
        ck_eq((uint32_t)pd.ai_attack_orders_issued, 0u, "T4: ai_attack_orders_issued = 0");
        ck_eq_d(pd.ai_labor_utilization, 0.0, "T4: ai_labor_utilization = 0.0 (the two-dword store)");
        ck_eq((uint32_t)pd.ai_reposition_cached_member_count, 0u, "T4: ai_reposition_cached_member_count = 0");
        ck_eq((uint32_t)pd.ai_start_units_remaining, 17u,
              "T4: ai_start_units_remaining = AI_CFG_START_UNITS, 0x004dcebd");
        ck_eq((uint32_t)pd.ai_start_units_pending_spawn, 17u, "T4: ... and the pending-spawn twin, 0x004dcec7");
        ck_eq((uint32_t)pd.ai_promo_credit, 0u, "T4: ai_promo_credit = 0");
        ck_eq((uint32_t)pd.ai_attack_milestone_index, 0u, "T4: ai_attack_milestone_index = 0");
        ck_eq_d((double)pd.ai_attack_milestone_clock, 0.0, "T4: ai_attack_milestone_clock = 0.0f");
        ck_eq((uint32_t)pd.ai_map_changed_pending, 0u, "T4: ai_map_changed_pending = 0");
        ck_eq((uint32_t)pd.ai_turret_rescan_pending, 0u, "T4: ai_turret_rescan_pending = 0");
        ck_eq((uint32_t)pd.ai_spend_ring_cursor, 0u, "T4: ai_spend_ring_cursor = 0, 0x004dcf86");
        ck_eq((uint32_t)pd.ai_build_plan_cursor, 0u, "T4: ai_build_plan_cursor = 0");
        ck_eq((uint32_t)pd.ai_build_plan_len_and_flag, 0u, "T4: ai_build_plan_len_and_flag = 0");
    }

    // =================================================================================================
    // T5 -- THE THREE STAGGERED CLOCKS, each with its OWN period. The three periods are distinct and
    // the player is non-zero, so a copy-paste that reused one period for two clocks is visible.
    //   clock_m = player * move_period     * stagger
    //   clock_t = player * tactic_period   * stagger
    //   clock_s = player * strategy_period * stagger
    // Player 0 is checked too: every stagger collapses to 0 while ai_clock stays 0.001f, which is what
    // separates "the clock base" from "the stagger".
    // =================================================================================================
    {
        fx.reset();
        small_map(fx, 8, 8);
        fx.ai_move_period                    = 2.0f;
        fx.ai_tactic_period                  = 8.0f;
        fx.ai_strategy_period                = 32.0f;
        fx.ai_base_spawn_timer_stagger_scale = 0.25;

        run(fx, /*player=*/3, 0, 0, 0);

        player_data &pd = fx.players[3];
        ck_eq_d((double)pd.ai_clock, (double)0.001f, "T5: ai_clock = 0.001f, the base, 0x004dce39");
        ck_eq_d((double)pd.ai_clock_m, (double)(float)(3.0 * 2.0 * 0.25), "T5: clock_m uses the MOVE period");
        ck_eq_d((double)pd.ai_clock_t, (double)(float)(3.0 * 8.0 * 0.25), "T5: clock_t uses the TACTIC period");
        ck_eq_d((double)pd.ai_clock_s, (double)(float)(3.0 * 32.0 * 0.25), "T5: clock_s uses the STRATEGY period");

        fx.reset();
        small_map(fx, 8, 8);
        fx.ai_move_period                    = 2.0f;
        fx.ai_base_spawn_timer_stagger_scale = 0.25;
        run(fx, /*player=*/0, 0, 0, 0);
        ck_eq_d((double)fx.players[0].ai_clock, (double)0.001f, "T5: player 0 still gets the 0.001f base");
        ck_eq_d((double)fx.players[0].ai_clock_m, 0.0, "T5: player 0's stagger is 0 -- it scales with the index");
    }

    // =================================================================================================
    // T6 -- THE ARRAY INITS. resource_spend_total is 1 (a divide-safe denominator, NOT a zero-init) --
    // the one place where "everything is zeroed" would be wrong. The two spend rings and
    // resource_spent are zeroed; the relation row is +1 toward self and -1 toward everyone else.
    // All are pre-dirtied so a missing loop fails.
    // =================================================================================================
    {
        fx.reset();
        small_map(fx, 8, 8);
        player_data &pd = fx.players[5];
        for (int32_t i = 0; i < 4; ++i) pd.resource_spent[i] = 0x5a;
        for (int32_t i = 0; i < 8; ++i) pd.resource_spend_total[i] = 0x5a;
        for (int32_t i = 0; i < 128; ++i) {
            pd.ai_spend_rate_denom_ring[i] = 0x5a;
            pd.ai_spend_rate_numer_ring[i] = 0x5a;
        }
        for (int32_t i = 0; i < 8; ++i) {
            pd.ai_intel_seen_count[i] = 0x5a;
            pd.ai_intel_flags[i]      = 0x5a;
            pd.ai_player_relation[i]  = 0x5a;
        }
        for (int32_t i = 0; i < 12; ++i) pd.ai_train_queued_by_ai_unit[i] = 0x5a;

        run(fx, /*player=*/5, 0, 0, 0);

        bool spent_zero = true, ring_zero = true, total_one = true, intel_zero = true, ai_unit_zero = true;
        for (int32_t i = 0; i < 4; ++i)
            if (pd.resource_spent[i] != 0) spent_zero = false;
        for (int32_t i = 0; i < 8; ++i)
            if (pd.resource_spend_total[i] != 1) total_one = false;
        for (int32_t i = 0; i < 128; ++i)
            if (pd.ai_spend_rate_denom_ring[i] != 0 || pd.ai_spend_rate_numer_ring[i] != 0) ring_zero = false;
        for (int32_t i = 0; i < 8; ++i)
            if (pd.ai_intel_seen_count[i] != 0 || pd.ai_intel_flags[i] != 0) intel_zero = false;
        for (int32_t i = 0; i < 12; ++i)
            if (pd.ai_train_queued_by_ai_unit[i] != 0) ai_unit_zero = false;

        ck(spent_zero, "T6: resource_spent[0..3] zeroed, 0x004dcf01-f34");
        ck(total_one, "T6: resource_spend_total[0..7] = 1, NOT 0 -- divide-safe, 0x004dcfb5-fe2");
        ck(ring_zero, "T6: both int[32][4] spend rings zeroed, 0x004dcf3d-f8c");
        ck(intel_zero, "T6: ai_intel_seen_count / ai_intel_flags zeroed, 0x004dd08d-0e6");
        ck(ai_unit_zero, "T6: ai_train_queued_by_ai_unit[0..11] zeroed, 0x004dd122-14f");
        for (int32_t i = 0; i < 8; ++i) {
            ck_eq((uint32_t)pd.ai_player_relation[i], (uint32_t)(i == 5 ? 1 : -1),
                  "T6: relation is +1 toward SELF and -1 toward everyone else");
        }
    }

    // =================================================================================================
    // T7 -- ai_train_queued_by_unit_type is cleared 0..UNIT.total INCLUSIVE. total is set to 3, and
    // index 4 is pre-dirtied and must SURVIVE; indices 0..3 must all be cleared. That is the pair that
    // pins an inclusive bound: an exclusive one leaves index 3 dirty, an over-run one clears index 4.
    // =================================================================================================
    {
        fx.reset();
        small_map(fx, 8, 8);
        fx.cfg_unit_sec.total = 3;
        player_data &pd       = fx.players[1];
        for (int32_t i = 0; i < 6; ++i) pd.ai_train_queued_by_unit_type[i] = 0x5a;

        run(fx, /*player=*/1, 0, 0, 0);

        for (int32_t i = 0; i <= 3; ++i)
            ck_eq((uint32_t)pd.ai_train_queued_by_unit_type[i], 0u,
                  "T7: cleared through index total INCLUSIVE, 0x004dd0ec-11c");
        ck_eq((uint32_t)pd.ai_train_queued_by_unit_type[4], 0x5au,
              "T7: index total+1 SURVIVES -- the bound is <=, not < and not the array extent");
    }

    // =================================================================================================
    // T8 -- THE TILE-FLAG GRID FROM `passable`, and its LOOP BOUNDS. The map is deliberately
    // NON-SQUARE (width 6, height 3) so swapping the two bounds changes which cells are written, and
    // the index layout ((x << 8) | y, x in the HIGH byte) is pinned by writing one impassable tile at
    // an asymmetric position.
    //   passable[(4 << 8) | 1] = 0 (impassable) -> grid there must be 8
    //   everything else in range passable       -> 0
    //   a cell OUTSIDE the bounds (x = 6)       -> untouched (pre-dirtied)
    // =================================================================================================
    {
        fx.reset();
        small_map(fx, /*w=*/6, /*h=*/3);
        std::fill(fx.passable.begin(), fx.passable.end(), (uint8_t)1);
        fx.passable[(size_t)((4 << 8) | 1)] = 0;
        player_data &pd                     = fx.players[1];
        pd.ai_tile_flags_grid[(4 << 8) | 1] = 0x5a;
        pd.ai_tile_flags_grid[(6 << 8) | 0] = 0x5a; // x past the width bound
        pd.ai_tile_flags_grid[(0 << 8) | 3] = 0x5a; // y past the height bound

        run(fx, /*player=*/1, 0, 0, 0);

        ck_eq((uint32_t)pd.ai_tile_flags_grid[(4 << 8) | 1], 8u,
              "T8: an impassable tile gets flag 8, at index (x << 8) | y, 0x004dcfed-d048");
        ck_eq((uint32_t)pd.ai_tile_flags_grid[(3 << 8) | 2], 0u, "T8: a passable in-range tile gets 0");
        ck_eq((uint32_t)pd.ai_tile_flags_grid[(6 << 8) | 0], 0x5au,
              "T8: x == map_width is OUTSIDE the outer bound -- untouched");
        ck_eq((uint32_t)pd.ai_tile_flags_grid[(0 << 8) | 3], 0x5au,
              "T8: y == map_height is OUTSIDE the inner bound -- untouched");
    }

    // =================================================================================================
    // T9 -- THE COARSE RESOURCE SWEEP. Two independent rules on the SAME cell sum, and the case is
    // built so they disagree, which is the only way to show they are independent:
    //   cell (1,0): sum 20  > mine_worth 10  -> a SITE is recorded AND the 0x80 stamp applied
    //   cell (0,1): sum 5   <= mine_worth 10 -> NO site, but the stamp IS applied (nonzero sum)
    //   cell (0,0): sum 0                    -> neither
    // The stamp covers the whole 4x4 fine footprint, checked at both corners of cell (0,1).
    // The recorded site's fields are checked: status 0, build tiles -1, and the coarse grid coords.
    // =================================================================================================
    {
        fx.reset();
        small_map(fx, /*w=*/8, /*h=*/8); // coarse 2x2
        fx.mine_worth = 10;
        std::fill(fx.passable.begin(), fx.passable.end(), (uint8_t)1);
        // resources is [64][64] row-major with x as the row: resources[x * 64 + y].
        fx.resources[(size_t)(1 * 64 + 0)].value[0] = 20; // coarse cell x4=1, y4=0
        fx.resources[(size_t)(0 * 64 + 1)].value[3] = 5;  // coarse cell x4=0, y4=1

        run(fx, /*player=*/1, 0, /*x=*/0, /*y=*/0);

        player_data &pd = fx.players[1];
        ck_eq((uint32_t)pd.ai_resource_site_count, 1u,
              "T9: only the cell strictly above mine_worth records a site, 0x004dd193-211");
        if (pd.ai_resource_site_count == 1) {
            const auto &s = pd.ai_resource_sites[0];
            ck_eq((uint32_t)(int32_t)s.grid_x, 1u, "T9: the site's grid_x is the COARSE x");
            ck_eq((uint32_t)(int32_t)s.grid_y, 0u, "T9: the site's grid_y is the COARSE y");
            ck_eq((uint32_t)(int32_t)s.status, 0u, "T9: status 0 = unresolved");
            ck_eq((uint32_t)(int32_t)s.build_tile_x, (uint32_t)-1, "T9: build_tile_x = -1 (unset)");
            ck_eq((uint32_t)(int32_t)s.build_tile_y, (uint32_t)-1, "T9: build_tile_y = -1 (unset)");
        }
        // The 0x80 stamp is NOT gated by mine_worth: the sub-threshold cell is stamped too.
        ck_eq((uint32_t)(pd.ai_tile_flags_grid[(0 << 8) | 4] & 0x80u), 0x80u,
              "T9: a sub-threshold but NONZERO cell is still stamped 0x80, 0x004dd211-25d");
        ck_eq((uint32_t)(pd.ai_tile_flags_grid[(3 << 8) | 7] & 0x80u), 0x80u,
              "T9: ... across the whole 4x4 footprint, far corner included");
        ck_eq((uint32_t)(pd.ai_tile_flags_grid[(4 << 8) | 0] & 0x80u), 0x80u,
              "T9: the above-threshold cell is stamped as well");
        ck_eq((uint32_t)(pd.ai_tile_flags_grid[(0 << 8) | 0] & 0x80u), 0u,
              "T9: an all-zero cell is not stamped");
    }

    // =================================================================================================
    // T10 -- THE THRESHOLD IS STRICT. A cell summing EXACTLY to mine_worth records no site; one more
    // records one. This is the boundary a `>=` mutation flips, and it is the only case that moves it.
    // =================================================================================================
    {
        for (int32_t bump = 0; bump < 2; ++bump) {
            fx.reset();
            small_map(fx, 8, 8);
            fx.mine_worth = 10;
            std::fill(fx.passable.begin(), fx.passable.end(), (uint8_t)1);
            fx.resources[(size_t)(0 * 64 + 0)].value[0] = (int16_t)(10 + bump);

            run(fx, /*player=*/1, 0, 0, 0);

            ck_eq((uint32_t)fx.players[1].ai_resource_site_count, (uint32_t)bump,
                  "T10: sum > mine_worth is STRICT -- equality records nothing, 0x004dd193");
        }
    }

    // =================================================================================================
    // T11 -- THE SIBLING SORT RUNS FOR REAL. Three sites are seeded by the sweep at coarse cells whose
    // distances from the home tile are deliberately out of order in scan order; after spawn_ai_base
    // returns they must be nearest-first. The distance mock here is a plain euclidean square, so the
    // expected order is computable by hand:
    //   home fine tile (0, 0); sites at coarse (0,2) (1,1) (2,0) -> fine (0,8) (4,4) (8,0)
    //   d^2 = 64, 32, 64 -- so the middle one sorts to the front and the two equal ones keep order.
    // Scan order is (y4 outer, x4 inner) = (0,2) then (1,1) then (2,0), which is NOT sorted order.
    // =================================================================================================
    {
        fx.reset();
        small_map(fx, /*w=*/12, /*h=*/12); // coarse 3x3
        fx.mine_worth = 0;
        std::fill(fx.passable.begin(), fx.passable.end(), (uint8_t)1);
        fx.resources[(size_t)(0 * 64 + 2)].value[0] = 5; // coarse (x4=0, y4=2)
        fx.resources[(size_t)(1 * 64 + 1)].value[0] = 5; // coarse (x4=1, y4=1)
        fx.resources[(size_t)(2 * 64 + 0)].value[0] = 5; // coarse (x4=2, y4=0)

        run(fx, /*player=*/1, 0, /*x=*/0, /*y=*/0);

        player_data &pd = fx.players[1];
        ck_eq((uint32_t)pd.ai_resource_site_count, 3u, "T11: three sites recorded");
        if (pd.ai_resource_site_count == 3) {
            ck_eq((uint32_t)(int32_t)pd.ai_resource_sites[0].grid_x, 1u,
                  "T11: the NEAREST site is first -- sort_sites_by_dist ran, 0x004dd296-29c");
            ck_eq((uint32_t)(int32_t)pd.ai_resource_sites[0].grid_y, 1u, "T11: ... its grid_y too");
        }
    }

    // =================================================================================================
    // T12 -- THE GROUPS. All 32 get member_count 0 and current_param 3 (pre-dirtied so a missing loop
    // fails), five ai_group_create calls happen with the player, and the goals land on groups 0..4 by
    // FIXED index -- 1, 2, 6, 6, 6 -- regardless of what ai_group_create returns. The mock returns
    // 0,1,2,3,4 here, so to show the index is fixed rather than the return value, group 5..31 are
    // asserted to keep the default current_param 3 with goal 0.
    // =================================================================================================
    {
        fx.reset();
        small_map(fx, 8, 8);
        player_data &pd = fx.players[2];
        for (int32_t g = 0; g < 32; ++g) {
            pd.ai_groups[g].member_count  = 0x5a;
            pd.ai_groups[g].current_param = 0x5a;
            pd.ai_groups[g].goal          = 0x5a;
        }

        run(fx, /*player=*/2, 0, 0, 0);

        for (int32_t g = 0; g < 32; ++g) {
            ck_eq((uint32_t)(int32_t)pd.ai_groups[g].member_count, 0u,
                  "T12: every group's member_count zeroed, 0x004dd04c-087");
            ck_eq((uint32_t)pd.ai_groups[g].current_param, 3u, "T12: every group's current_param = 3");
        }
        ck(g_group_create_calls.size() == 5, "T12: exactly five ai_group_create calls, 0x004dd2a1-318");
        for (size_t i = 0; i < g_group_create_calls.size(); ++i)
            ck_eq((uint32_t)g_group_create_calls[i], 2u, "T12: each takes the player parameter");
        const int16_t want_goal[5] = {1, 2, 6, 6, 6};
        for (int32_t g = 0; g < 5; ++g)
            ck_eq((uint32_t)(int32_t)pd.ai_groups[g].goal, (uint32_t)(int32_t)want_goal[g],
                  "T12: goals 1/2/6/6/6 written to groups 0..4 by FIXED index");
        // Group 5's goal is NOT written by this function -- the goal stores stop at index 4. It must
        // therefore still hold the pre-dirtied value, which is what shows the five stores are aimed
        // by fixed index rather than looping over the whole roster.
        ck_eq((uint32_t)(int32_t)pd.ai_groups[5].goal, 0x5au,
              "T12: group 5's goal is UNWRITTEN -- only groups 0..4 get one");
    }

    // =================================================================================================
    // T13 -- THE TASK ENQUEUES. Five calls, groups 0..4 in order, params 0x1d3 / 0x183 / 0 / 0 / 0 and
    // every other argument 0. The two non-zero params are what a copy-paste would get wrong.
    // =================================================================================================
    {
        fx.reset();
        small_map(fx, 8, 8);

        run(fx, /*player=*/6, 0, 0, 0);

        ck(g_task_calls.size() == 5, "T13: five task enqueues, 0x004dd321-3ba");
        if (g_task_calls.size() == 5) {
            const uint32_t want_p4[5] = {0x1d3u, 0x183u, 0u, 0u, 0u};
            for (int32_t i = 0; i < 5; ++i) {
                const TaskCall &t = g_task_calls[(size_t)i];
                ck_eq((uint32_t)t.player, 6u, "T13: each enqueue takes the player");
                ck_eq((uint32_t)t.group, (uint32_t)i, "T13: groups 0..4 in order");
                ck_eq(t.p4, want_p4[i], "T13: params 0x1d3 / 0x183 / 0 / 0 / 0");
                ck_eq((uint32_t)t.a2, 0u, "T13: a2 is 0");
                ck_eq(t.p5 | t.p6 | t.p7 | t.p8 | (uint32_t)t.p9, 0u, "T13: every trailing argument is 0");
            }
        }
    }

    // =================================================================================================
    // T14 -- THE SIX BUILD-PLAN PUSHES, in order, and the LAST ONE READS PLAYER+1's record. Each source
    // field carries a distinct value, and player+1's ai_housing_candidate_soldier differs from this
    // player's, so the documented cross-record alias is measured rather than assumed. The duplicated
    // first two pushes are asserted as duplicates -- that is the original's own quirk.
    //
    // ORDER MATTERS relative to the field writes: the pushes read fields that
    // ai_init_build_candidate_priorities is supposed to have filled, so the mock is checked to have
    // run exactly once and BEFORE them (its call is recorded, and the values pushed are the ones the
    // fixture holds at that point).
    // =================================================================================================
    {
        fx.reset();
        small_map(fx, 8, 8);
        player_data &pd                       = fx.players[3];
        player_data &next                     = fx.players[4];
        pd.ai_mine_candidate_tier1            = 11;
        pd.ai_build_candidate_primary         = 22;
        pd.ai_build_candidate_secondary       = 33;
        pd.ai_resource_shortage_candidates[0] = 44;
        pd.ai_housing_candidate_soldier       = 55; // THIS player's -- must NOT be the one pushed
        next.ai_housing_candidate_soldier     = 66; // player+1's -- the aliased read

        run(fx, /*player=*/3, 0, 0, 0);

        ck(g_init_prio_calls.size() == 1, "T14: ai_init_build_candidate_priorities called once, 0x004dd3ba");
        if (g_init_prio_calls.size() == 1)
            ck_eq((uint32_t)g_init_prio_calls[0], 3u, "T14: ... with the player");
        ck(g_push_calls.size() == 6, "T14: six build-plan pushes, 0x004dd3d9-43a");
        if (g_push_calls.size() == 6) {
            const int32_t want[6] = {11, 11, 22, 33, 44, 66};
            for (int32_t i = 0; i < 6; ++i) {
                ck_eq((uint32_t)g_push_calls[(size_t)i].player, 3u, "T14: each push takes the player");
                ck_eq((uint32_t)g_push_calls[(size_t)i].ai_build_id, (uint32_t)want[i],
                      "T14: the six pushed ids, incl. the DUPLICATED tier1 and player+1's housing id");
            }
        }
    }

    // =================================================================================================
    // T15 -- A NEIGHBOURING PLAYER'S RECORD IS NOT DISTURBED. player+1 is read by T14's last push, so
    // it is the record most likely to be written by accident; here its whole header is snapshotted and
    // compared. (The one field the function legitimately reads there is not written.)
    // =================================================================================================
    {
        fx.reset();
        small_map(fx, 8, 8);
        player_data &next               = fx.players[3];
        next.ai_enabled                 = 0x5a;
        next.ai_phase_flags             = 0x5a;
        next.ai_home_tile_x             = 0x5a;
        next.ai_resource_site_count     = 0x5a;
        next.ai_groups[0].current_param = 0x5a;

        run(fx, /*player=*/2, 0, 0, 0);

        ck_eq((uint32_t)next.ai_enabled, 0x5au, "T15: player+1's ai_enabled untouched");
        ck_eq((uint32_t)next.ai_phase_flags, 0x5au, "T15: ... ai_phase_flags");
        ck_eq((uint32_t)next.ai_home_tile_x, 0x5au, "T15: ... ai_home_tile_x");
        ck_eq((uint32_t)next.ai_resource_site_count, 0x5au, "T15: ... ai_resource_site_count");
        ck_eq((uint32_t)next.ai_groups[0].current_param, 0x5au, "T15: ... and its group records");
    }
}

} // namespace mh::sim::test
