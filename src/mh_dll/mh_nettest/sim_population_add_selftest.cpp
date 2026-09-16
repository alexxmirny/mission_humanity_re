//
// sim_population_add_selftest.cpp -- `simtest` cases for llm_strat_population_add
// (sim/sim_population_add.h/.cpp, RI-SIM / SIM1F). This is the offline oracle the
// batch-F done_when names: "population add/remove is covered by a simtest case at both bounds
// (housing exactly equal to population -- the branch neither growth nor decay should take)".
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_strat_population_add_00491328.asm), not read off the .cpp:
//
//   count == 0 (growth):  pop_fraction += human * 0.1 * (colony_hp_sum+1) / (colony_hp_max_sum+1)
//   count != 0 (direct):  pop_fraction += count
//   pop_total = trunc(pop_fraction)
//   if (count == 0 && housing_prev < pop_total) { pop_total = housing_prev; pop_fraction = pop_total; }
//   human = (pop_total - workers_employed) - human_in_field
//   if ((uint16_t)player == PlayerSide) game_SetEvent(7)      // build/projects UI refresh
//
// pop_growth_factor is 0.1 exactly. All the growth cases below are chosen so the products/quotients
// are exact in double, so trunc() and the clamp compare test integer arithmetic rather than FP
// rounding luck.
//
#include "sim/sim_population_add.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- the one outward callee (game_SetEvent), recorded ----
std::vector<uint32_t> g_events;
uint32_t              stub_set_event(uint32_t type) {
    g_events.push_back(type);
    return 0;
}
const population_add_calls g_calls = {stub_set_event};

// Seed one player's pop_stats record and run the reimplementation.
void run_add(sim_fixture &fx, uint16_t player, int32_t count, double pop_fraction, int32_t human,
             int32_t colony_hp_sum, int32_t colony_hp_max_sum, int32_t housing_prev,
             int32_t workers_employed, int32_t human_in_field) {
    pop_stats &ps        = fx.population[player];
    ps.pop_fraction      = pop_fraction;
    ps.human             = human;
    ps.colony_hp_sum     = colony_hp_sum;
    ps.colony_hp_max_sum = colony_hp_max_sum;
    ps.housing_prev      = housing_prev;
    ps.workers_employed  = workers_employed;
    ps.human_in_field    = human_in_field;
    ps.pop_total         = -999; // poison: population_add must overwrite it

    sim_store own = fx.store();
    detail::population_add(fx.view(), own, g_calls, player, count);
}

} // namespace

void run_population_add_tests() {
    sim_fixture fx;

    // ---- C1: count != 0, direct add, NO clamp (clamp is count==0 only) -----------------------------
    // pop_fraction 10.0 + count 5 = 15.0 -> pop_total 15. housing_prev 8 is BELOW pop_total but the
    // clamp must not fire because count != 0. human = 15 - 2 - 1 = 12.
    fx.reset();
    g_events.clear();
    fx.player_side = 3; // a DIFFERENT player, so no UI event for player 0
    run_add(fx, /*player*/ 0, /*count*/ 5, /*pop_fraction*/ 10.0, /*human*/ 99, /*hp_sum*/ 0,
            /*hp_max*/ 0, /*housing_prev*/ 8, /*workers*/ 2, /*in_field*/ 1);
    ck_eq_d(fx.population[0].pop_fraction, 15.0, "C1: pop_fraction = 10 + count(5)");
    ck_eq((uint32_t)fx.population[0].pop_total, 15u, "C1: pop_total = trunc(15.0), no clamp on count!=0");
    ck_eq((uint32_t)fx.population[0].human, 12u, "C1: human = 15 - workers(2) - in_field(1)");
    ck(g_events.empty(), "C1: no UI event (player 0 != PlayerSide 3)");

    // ---- C2: count == 0, growth, result BELOW housing, no clamp ------------------------------------
    // growth = human(10) * 0.1 * (hp_sum 9 +1=10) / (hp_max 9 +1=10) = 10*0.1*10/10 = 1.0.
    // pop_fraction 2.0 + 1.0 = 3.0 -> pop_total 3. housing_prev 5 (> 3) so no clamp.
    // human = 3 - 1 - 1 = 1. (initial human 10 is what the growth term used; final human is recomputed.)
    fx.reset();
    g_events.clear();
    fx.player_side = 4;
    run_add(fx, /*player*/ 1, /*count*/ 0, /*pop_fraction*/ 2.0, /*human*/ 10, /*hp_sum*/ 9,
            /*hp_max*/ 9, /*housing_prev*/ 5, /*workers*/ 1, /*in_field*/ 1);
    ck_eq_d(fx.population[1].pop_fraction, 3.0, "C2: pop_fraction = 2.0 + growth(1.0)");
    ck_eq((uint32_t)fx.population[1].pop_total, 3u, "C2: pop_total = trunc(3.0), below housing, no clamp");
    ck_eq((uint32_t)fx.population[1].human, 1u, "C2: human = 3 - workers(1) - in_field(1)");

    // ---- C3: count == 0, growth, result ABOVE housing -> CLAMP fires -------------------------------
    // growth = human(100) * 0.1 * (9+1) / (9+1) = 10.0. pop_fraction 4.0 + 10.0 = 14.0 -> pop_total 14.
    // housing_prev 8 (< 14) so clamp: pop_total = 8, pop_fraction = 8.0. human = 8 - 0 - 0 = 8.
    fx.reset();
    g_events.clear();
    fx.player_side = 4;
    run_add(fx, /*player*/ 2, /*count*/ 0, /*pop_fraction*/ 4.0, /*human*/ 100, /*hp_sum*/ 9,
            /*hp_max*/ 9, /*housing_prev*/ 8, /*workers*/ 0, /*in_field*/ 0);
    ck_eq((uint32_t)fx.population[2].pop_total, 8u, "C3: pop_total clamped to housing_prev(8)");
    ck_eq_d(fx.population[2].pop_fraction, 8.0, "C3: pop_fraction reset to clamped pop_total");
    ck_eq((uint32_t)fx.population[2].human, 8u, "C3: human = 8 - 0 - 0");

    // ---- C4: THE done_when BOUNDARY -- pop_total EXACTLY EQUALS housing_prev, clamp must NOT fire --
    // growth = human(10) * 0.1 * (9+1) / (9+1) = 1.0. pop_fraction 4.0 + 1.0 = 5.0 -> pop_total 5.
    // housing_prev 5. The clamp gate is `housing_prev < pop_total` (STRICT), so 5 < 5 is false: no
    // clamp, pop_fraction stays 5.0 (NOT reset). This is the "neither growth-clip nor decay" branch.
    fx.reset();
    g_events.clear();
    fx.player_side = 4;
    run_add(fx, /*player*/ 3, /*count*/ 0, /*pop_fraction*/ 4.0, /*human*/ 10, /*hp_sum*/ 9,
            /*hp_max*/ 9, /*housing_prev*/ 5, /*workers*/ 2, /*in_field*/ 0);
    ck_eq((uint32_t)fx.population[3].pop_total, 5u, "C4: pop_total = 5 == housing_prev, boundary");
    ck_eq_d(fx.population[3].pop_fraction, 5.0,
            "C4: pop_fraction NOT reset (strict < gate: 5<5 false, no clamp)");
    ck_eq((uint32_t)fx.population[3].human, 3u, "C4: human = 5 - workers(2) - in_field(0)");

    // ---- C5: PlayerSide match fires the build/projects UI event (event 7) --------------------------
    fx.reset();
    g_events.clear();
    fx.player_side = 6;
    run_add(fx, /*player*/ 6, /*count*/ 3, /*pop_fraction*/ 0.0, /*human*/ 0, /*hp_sum*/ 0,
            /*hp_max*/ 0, /*housing_prev*/ 100, /*workers*/ 0, /*in_field*/ 0);
    ck(g_events.size() == 1 && g_events[0] == 7u,
       "C5: player == PlayerSide -> game_SetEvent(BUILD_PROJECTS_REFRESH=7) once");
}

} // namespace mh::sim::test
