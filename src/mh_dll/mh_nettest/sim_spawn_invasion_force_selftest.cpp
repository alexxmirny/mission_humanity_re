//
// sim_spawn_invasion_force_selftest.cpp -- `simtest` offline oracle for llm_strat_spawn_invasion_force
// (sim/sim_spawn_invasion_force.{h,cpp}, RI-SIM / SIM1F batch F -- the LAST function of
// the whole SIM migration). The function initializes player_data[player] as an active AI invasion
// faction. Its tail call spawns units (and does file I/O in scr_parse), so it is DO-NOT-ARM (no
// shadow) and this offline oracle is the ONLY proof.
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY (tmp/decomp/llm_strat_spawn_invasion_force_004dd446.asm).
// The strategy: POISON the whole player_data record with a non-zero pattern before the call so every
// field the function is supposed to write (zero, set, or compute) is OBSERVABLE against the poison,
// and the four delegated helpers are stubbed and their calls/arguments asserted.
//
#include "sim/sim_spawn_invasion_force.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- recorded outward calls ----------------------------------------------------------------------
std::vector<int32_t> g_scr_parse;    // llm_strat_ai_scr_parse (record nothing but the count)
int32_t              g_sprintf;      // utils_sprintf__vi
std::vector<int32_t> g_group_create; // llm_strat_ai_group_create (player each call)
struct EnqCall {
    int32_t  player, group;
    uint32_t param4;
};
std::vector<EnqCall> g_enqueue;         // llm_strat_ai_group_task_enqueue
int32_t              g_init_priorities; // llm_strat_ai_init_build_candidate_priorities
int32_t              g_reinforce;       // llm_strat_ai_invasion_spawn_reinforcements

void    stub_scr_parse(char *) { g_scr_parse.push_back(1); }
int32_t stub_sprintf(void *, const char *, int32_t) {
    g_sprintf++;
    return 0;
}
int32_t stub_group_create(int32_t p) {
    g_group_create.push_back(p);
    return 0;
}
void stub_enqueue(int32_t p, int32_t g, uint16_t, uint32_t p4, uint32_t, uint32_t, uint32_t, uint32_t,
                  uint16_t) {
    g_enqueue.push_back({p, g, p4});
}
void    stub_init_priorities(int32_t) { g_init_priorities++; }
int32_t stub_reinforce(int32_t) {
    g_reinforce++;
    return 0x1234; // a distinctive return so the oracle can prove the function forwards it
}

const spawn_invasion_force_calls g_calls = {stub_scr_parse, stub_sprintf,
                                            stub_group_create, stub_enqueue,
                                            stub_init_priorities, stub_reinforce};

void clear_calls() {
    g_scr_parse.clear();
    g_group_create.clear();
    g_enqueue.clear();
    g_sprintf = g_init_priorities = g_reinforce = 0;
}

uint32_t run(sim_fixture &fx, uint32_t player, int32_t is_alien, int32_t hx, int32_t hy, int32_t pts) {
    clear_calls();
    sim_store own = fx.store();
    return detail::spawn_invasion_force(fx.view(), own, g_calls, player, is_alien, hx, hy, pts);
}

constexpr uint32_t STATUS_ALIVE = 0x2u;
constexpr uint32_t STATUS_HUMAN = 0x4u;

// Poison the whole record so a field the function leaves alone stays poisoned and a field it writes
// is observable. 0xAA -> every int reads 0xAAAAAAAA, every byte 0xAA, every short 0xAAAA.
void poison(sim_fixture &fx, uint32_t player) {
    memset(&fx.players[player], 0xAA, sizeof(player_data));
}

} // namespace

void run_spawn_invasion_force_tests() {
    sim_fixture fx;

    // ================================================================================================
    // CASE A -- the full initialization on a fresh player. player 3, alien race, home (40,50),
    // invasion_points 7. A small map (4x3) with a mixed passability pattern, cfg unit count 5.
    // Everything is checked against the 0xAA poison.
    // ================================================================================================
    fx.reset();
    fx.planet_index              = 2;
    fx.ai_active_player_count    = 3; // player 3 >= 3 -> should bump to 4
    fx.ai_move_period            = 1.0f;
    fx.ai_tactic_period          = 5.0f;
    fx.ai_strategy_period        = 10.0f;
    fx.ai_invasion_clock_stagger = 0.125;
    fx.ai_cfg_start_units        = 12;
    fx.cfg_unit_sec.total        = 5;
    fx.map_width                 = 4;
    fx.map_height                = 3;
    fx.profiles[3].status_flags  = STATUS_HUMAN; // a pre-existing bit that must survive the OR
    // Passability: column-major (x<<8)|y. Mark (1,1) and (2,0) IMPASSABLE (0), everything else
    // passable (1) so both grid branches are exercised.
    for (int32_t x = 0; x < 4; ++x)
        for (int32_t y = 0; y < 3; ++y) fx.passable[(x << 8) | y] = 1;
    fx.passable[(1 << 8) | 1] = 0;
    fx.passable[(2 << 8) | 0] = 0;
    poison(fx, 3);
    {
        uint32_t     r  = run(fx, 3, 1, 40, 50, 7);
        player_data &pd = fx.players[3];

        // -- return value forwards invasion_spawn_reinforcements' result --
        ck_eq(r, 0x1234u, "A: returns llm_strat_ai_invasion_spawn_reinforcements' result");

        // -- header scalars / flags --
        ck_eq((uint32_t)pd.is_alien_race, 1u, "A: is_alien_race = arg");
        ck_eq((uint32_t)pd.ai_home_tile_x, 40u, "A: ai_home_tile_x = arg");
        ck_eq((uint32_t)pd.ai_home_tile_y, 50u, "A: ai_home_tile_y = arg");
        ck_eq((uint32_t)pd.ai_resource_shortage_state, 0u, "A: ai_resource_shortage_state = 0");
        ck_eq((uint32_t)pd.ai_resource_need_score, 0u, "A: ai_resource_need_score = 0");
        ck_eq((uint32_t)pd.ai_resource_need_threshold, 6u, "A: ai_resource_need_threshold = 6");
        ck_eq((uint32_t)pd.ai_established, 1u, "A: ai_established = 1");
        ck_eq((uint32_t)pd.ai_phase_flags, 4u, "A: ai_phase_flags = 4");
        ck_eq((uint32_t)pd.ai_enabled, 1u, "A: ai_enabled = 1");
        ck_eq((uint32_t)pd.ai_invasion_force, 1u, "A: ai_invasion_force = 1");
        ck_eq((uint32_t)pd.ai_invasion_points, 7u, "A: ai_invasion_points = arg");
        ck_eq((uint32_t)pd.ai_patrol_quadrant_cursor, 0u, "A: ai_patrol_quadrant_cursor = 0");
        ck_eq((uint32_t)pd.ai_group_count, 0u, "A: ai_group_count = 0");
        ck_eq((uint32_t)pd.ai_target_list_count, 0u, "A: ai_target_list_count = 0");
        ck_eq((uint32_t)pd.next_group_serial, 0u, "A: next_group_serial = 0");
        ck_eq((uint32_t)pd.ai_bldg_queue_count, 0u, "A: ai_bldg_queue_count = 0");
        ck_eq((uint32_t)pd.ai_attack_orders_issued, 0u, "A: ai_attack_orders_issued = 0");
        ck_eq_d(pd.ai_labor_utilization, 0.0, "A: ai_labor_utilization = 0.0");
        ck_eq((uint32_t)pd.ai_reposition_cached_member_count, 0u, "A: ai_reposition_cached_member_count = 0");
        ck_eq((uint32_t)pd.ai_start_units_remaining, 12u, "A: ai_start_units_remaining = CFG_START_UNITS");
        ck_eq((uint32_t)pd.ai_start_units_pending_spawn, 12u, "A: ai_start_units_pending_spawn = CFG_START_UNITS");
        ck_eq((uint32_t)pd.ai_promo_credit, 0u, "A: ai_promo_credit = 0");
        ck_eq((uint32_t)pd.ai_attack_milestone_index, 0u, "A: ai_attack_milestone_index = 0");
        ck(pd.ai_attack_milestone_clock == 0.0f, "A: ai_attack_milestone_clock = 0.0f");
        ck_eq((uint32_t)pd.ai_map_changed_pending, 0u, "A: ai_map_changed_pending = 0");
        ck_eq((uint32_t)pd.ai_turret_rescan_pending, 0u, "A: ai_turret_rescan_pending = 0");
        ck_eq((uint32_t)pd.ai_resource_site_count, 0u, "A: ai_resource_site_count = 0");
        ck_eq((uint32_t)pd.ai_build_plan_cursor, 0u, "A: ai_build_plan_cursor = 0");
        ck_eq((uint32_t)pd.ai_build_plan_len_and_flag, 0u, "A: ai_build_plan_len_and_flag = 0");

        // -- the AI clocks: 0.001 base + player * period * 0.125 stagger (player 3) --
        ck(pd.ai_clock == 0.001f, "A: ai_clock = 0.001f");
        ck(pd.ai_clock_m == 0.375f, "A: ai_clock_m = 3 * 1.0 * 0.125 = 0.375");
        ck(pd.ai_clock_t == 1.875f, "A: ai_clock_t = 3 * 5.0 * 0.125 = 1.875");
        ck(pd.ai_clock_s == 3.75f, "A: ai_clock_s = 3 * 10.0 * 0.125 = 3.75");

        // -- resource_spent[4] zeroed, resource_spend_total[8] = 1 --
        bool spent_ok = true, total_ok = true;
        for (int32_t i = 0; i < 4; ++i)
            if (pd.resource_spent[i] != 0) spent_ok = false;
        for (int32_t i = 0; i < 8; ++i)
            if (pd.resource_spend_total[i] != 1) total_ok = false;
        ck(spent_ok, "A: resource_spent[0..3] = 0");
        ck(total_ok, "A: resource_spend_total[0..7] = 1 (divide-safe denominator)");

        // -- both spend rings fully zeroed (128 each) + cursor --
        bool rings_ok = true;
        for (int32_t i = 0; i < 128; ++i)
            if (pd.ai_spend_rate_denom_ring[i] != 0 || pd.ai_spend_rate_numer_ring[i] != 0)
                rings_ok = false;
        ck(rings_ok, "A: ai_spend_rate_denom_ring / numer_ring [0..127] = 0");
        ck_eq((uint32_t)pd.ai_spend_ring_cursor, 0u, "A: ai_spend_ring_cursor = 0");

        // -- ai_groups: member_count=0, current_param=3 for all 32; goal 1/2/6/6/6 for groups 0..4 --
        bool groups_ok = true;
        for (int32_t g = 0; g < 32; ++g)
            if (pd.ai_groups[g].member_count != 0 || pd.ai_groups[g].current_param != 3)
                groups_ok = false;
        ck(groups_ok, "A: all 32 ai_groups: member_count=0, current_param=3");
        ck((int)pd.ai_groups[0].goal == 1 && (int)pd.ai_groups[1].goal == 2 &&
               (int)pd.ai_groups[2].goal == 6 && (int)pd.ai_groups[3].goal == 6 &&
               (int)pd.ai_groups[4].goal == 6,
           "A: ai_groups[0..4].goal = 1/2/6/6/6");

        // -- the three int[8] intel/relation rows --
        bool intel_ok = true, rel_ok = true;
        for (int32_t i = 0; i < 8; ++i) {
            if (pd.ai_intel_seen_count[i] != 0 || pd.ai_intel_flags[i] != 0) intel_ok = false;
            if (pd.ai_player_relation[i] != (i == 3 ? 1 : -1)) rel_ok = false;
        }
        ck(intel_ok, "A: ai_intel_seen_count / ai_intel_flags [0..7] = 0");
        ck(rel_ok, "A: ai_player_relation[i] = (i==player)?1:-1");

        // -- ai_train_queued_by_unit_type[0..5] zeroed (INCLUSIVE of total=5); [6] stays poisoned --
        bool train_ok = true;
        for (int32_t i = 0; i <= 5; ++i)
            if (pd.ai_train_queued_by_unit_type[i] != 0) train_ok = false;
        ck(train_ok, "A: ai_train_queued_by_unit_type[0..total] = 0 (inclusive)");
        ck(pd.ai_train_queued_by_unit_type[6] == (int32_t)0xAAAAAAAA,
           "A: ai_train_queued_by_unit_type[total+1] left untouched (poison survives)");

        // -- the AI tile flag grid, from passability: impassable(0)->8, passable(nonzero)->0 --
        bool grid_ok = true;
        for (int32_t x = 0; x < 4; ++x)
            for (int32_t y = 0; y < 3; ++y) {
                const int32_t idx  = (x << 8) | y;
                const uint8_t want = (fx.passable[idx] == 0) ? 8 : 0;
                if (pd.ai_tile_flags_grid[idx] != want) grid_ok = false;
            }
        ck(grid_ok, "A: ai_tile_flags_grid seeded from passable (impassable->8, passable->0)");

        // -- profile ALIVE bit set, HUMAN bit preserved; active-player-count bumped to player+1 --
        ck_eq((uint32_t)fx.profiles[3].status_flags, STATUS_HUMAN | STATUS_ALIVE,
              "A: profile ALIVE set, other bits preserved");
        ck_eq((uint32_t)fx.ai_active_player_count, 4u, "A: active_player_count bumped to player+1");

        // -- delegated calls: 2 scr_parse, 1 sprintf, 5 group_create, 5 enqueue, 1 init, 1 reinforce --
        ck_eq((uint32_t)g_scr_parse.size(), 2u, "A: llm_strat_ai_scr_parse called twice");
        ck_eq((uint32_t)g_sprintf, 1u, "A: utils_sprintf called once (per-planet filename)");
        ck_eq((uint32_t)g_group_create.size(), 5u, "A: llm_strat_ai_group_create called 5x");
        ck(g_group_create.size() == 5 && g_group_create[0] == 3 && g_group_create[4] == 3,
           "A: group_create called with the player id");
        ck_eq((uint32_t)g_enqueue.size(), 5u, "A: llm_strat_ai_group_task_enqueue called 5x");
        ck_eq((uint32_t)g_init_priorities, 1u, "A: init_build_candidate_priorities called once");
        ck_eq((uint32_t)g_reinforce, 1u, "A: invasion_spawn_reinforcements called once");
        // enqueue args: (player, group, param4). group0->0x1d3, group1->0x183, groups2..4->0.
        bool enq_ok = g_enqueue.size() == 5;
        if (enq_ok) {
            const uint32_t want4[5] = {0x1d3u, 0x183u, 0u, 0u, 0u};
            for (int32_t i = 0; i < 5; ++i)
                if (g_enqueue[i].player != 3 || g_enqueue[i].group != i ||
                    g_enqueue[i].param4 != want4[i])
                    enq_ok = false;
        }
        ck(enq_ok, "A: enqueue args (player,group,param4) = (3,i, 0x1d3/0x183/0/0/0)");
    }

    // ================================================================================================
    // CASE B -- active_player_count is NOT bumped when the player is already within it. count 6,
    // player 2 -> 2 < 6, stays 6.
    // ================================================================================================
    fx.reset();
    fx.ai_active_player_count = 6;
    fx.cfg_unit_sec.total     = 0;
    fx.map_width              = 0; // skip the grid loop entirely for this case
    fx.map_height             = 0;
    {
        run(fx, 2, 0, 10, 10, 3);
        ck_eq((uint32_t)fx.ai_active_player_count, 6u, "B: active_player_count NOT bumped (player < count)");
        ck_eq((uint32_t)fx.players[2].ai_invasion_force, 1u, "B: still initialises the record");
    }
    // Exact boundary: count == player -> the `count <= player` test bumps it (to player+1).
    fx.reset();
    fx.ai_active_player_count = 4;
    fx.map_width              = 0;
    fx.map_height             = 0;
    {
        run(fx, 4, 0, 10, 10, 3);
        ck_eq((uint32_t)fx.ai_active_player_count, 5u, "B: boundary count==player bumps to player+1");
    }

    // ================================================================================================
    // CASE C -- a non-alien player, checking is_alien_race passes 0 through and relation self index
    // moves with the player (player 0 here: relation[0]=1, the rest -1).
    // ================================================================================================
    fx.reset();
    fx.cfg_unit_sec.total = 0;
    fx.map_width          = 0;
    fx.map_height         = 0;
    poison(fx, 0);
    {
        run(fx, 0, 0, 5, 6, 99);
        player_data &pd = fx.players[0];
        ck_eq((uint32_t)pd.is_alien_race, 0u, "C: is_alien_race = 0");
        ck_eq((uint32_t)pd.ai_invasion_points, 99u, "C: ai_invasion_points = 99");
        bool rel_ok = true;
        for (int32_t i = 0; i < 8; ++i)
            if (pd.ai_player_relation[i] != (i == 0 ? 1 : -1)) rel_ok = false;
        ck(rel_ok, "C: relation self-index tracks player 0");
        // clocks for player 0: all zero except the 0.001 base.
        ck(pd.ai_clock == 0.001f && pd.ai_clock_m == 0.0f && pd.ai_clock_t == 0.0f &&
               pd.ai_clock_s == 0.0f,
           "C: player 0 clocks -> base 0.001, staggers 0");
    }
}

} // namespace mh::sim::test
