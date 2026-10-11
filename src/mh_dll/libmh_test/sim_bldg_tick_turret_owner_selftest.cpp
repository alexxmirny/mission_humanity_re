//
// sim_bldg_tick_turret_owner_selftest.cpp -- `simtest` cases for patching:PT-FIX1
// ([net] turret_reload_owner_fix) in detail::building_tick (sim/sim_building_tick.cpp).
//
// Retail ticks llm_strat_bldg_turret_reload_tick for EVERY A_/H_TURRET building, rubble included.
// A destroyed turret's slot is freed (turrets[p][sub_id].b_index = 0, llm_strat_bldg_unmap_footprint)
// and re-used by the next turret (b_index = its building index, map_CreateBuilding), but the rubble
// keeps its sub_id and a tick budget that grows every step, so the rubble's call drains the NEW
// turret's reload_timer. The fix ticks a turret only for the building that owns its slot.
//
// The reload tick under test is the REAL translated body (detail::bldg_turret_reload_tick) driven
// through the calls table, so the observable is the live turret's reload_timer itself.
//
#include "sim/sim_bldg_turret_reload_tick.h"
#include "sim/sim_building_tick.h"
#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_TURRET

#include <cstdint>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr uint16_t PLAYER    = 2;
constexpr uint8_t  SLOT      = 5;
constexpr int32_t  RUBBLE    = 7;  // building index of the destroyed turret's rubble
constexpr int32_t  LIVE      = 12; // building index of the turret that re-used the slot
constexpr uint16_t CFG_ROW   = 9;
constexpr double   START_TMR = 0.2;

sim_fixture *g_fx    = nullptr;
int          g_calls = 0;

void rec_reload(uint16_t player, uint32_t index, double budget) {
    ++g_calls;
    sim_store own = g_fx->store();
    detail::bldg_turret_reload_tick(g_fx->view(), own, player, index, budget);
}
void noop_anim() {}
void noop_pip(uint16_t, int32_t) {}
void noop_damage() {}
void drain_budget() { g_fx->tick_budget = 0.0; } // the state handler consumed the tick budget

const building_tick_calls g_tick_calls = {&rec_reload, &noop_anim, &noop_pip, &noop_damage};

// Tick building `index` (whose turret slot is SLOT) with the slot's b_index = `owner`.
// Returns the slot's reload_timer afterwards; g_calls holds the number of reload ticks issued.
double run(sim_fixture &fx, bool fix, int32_t index, int32_t owner, uint16_t state) {
    fx.reset();
    g_fx    = &fx;
    g_calls = 0;
    set_turret_reload_owner_fix(fix);

    building &b                    = fx.b(PLAYER, index);
    b.building_id                  = CFG_ROW;
    b.sub_id                       = SLOT;
    b.state                        = state;
    b.built_flags                  = 3;
    b.last_tick_time               = 0.0;
    fx.cfg_buildings[CFG_ROW].type = BUILDING_TYPE_A_TURRET;
    fx.bldg_state_funcs[state]     = &drain_budget;
    fx.game_clock                  = 0.1; // budget = 0.1 s per tick

    turret &t           = fx.turrets[(size_t)PLAYER * (size_t)TURRETS_PER_PLAYER + SLOT];
    t.b_index           = (int16_t)owner;
    t.reload_ready_flag = 0;
    t.reload_timer      = START_TMR;
    t.weapon_id         = 1;
    t.ammo              = 5;

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = PLAYER;
    fx.view_cur_index   = (uint16_t)index;

    sim_store own = fx.store();
    detail::building_tick(fx.view(), own, g_tick_calls);
    const double r = t.reload_timer;
    set_turret_reload_owner_fix(false);
    return r;
}

} // namespace

void run_bldg_tick_turret_owner_tests() {
    sim_fixture fx;

    // (1) SHARED SLOT, fix OFF = retail: the rubble (state 4) ticks the live turret's slot.
    double r = run(fx, false, RUBBLE, LIVE, 4);
    ck_eq(g_calls, 1, "PT-FIX1 off: rubble sharing a reused slot still issues the reload tick (retail)");
    ck_eq_d(r, START_TMR - 0.1, "PT-FIX1 off: the live turret's reload_timer is drained by the rubble's budget");

    // (2) SHARED SLOT, fix ON: the rubble no longer ticks it.
    r = run(fx, true, RUBBLE, LIVE, 4);
    ck_eq(g_calls, 0, "PT-FIX1 on: rubble sharing a reused slot issues NO reload tick");
    ck_eq_d(r, START_TMR, "PT-FIX1 on: the live turret's reload_timer is untouched by the rubble");

    // (3) the freed slot (b_index 0) with only the rubble left: skipped as well (harmless either way).
    run(fx, true, RUBBLE, 0, 4);
    ck_eq(g_calls, 0, "PT-FIX1 on: rubble whose slot is free issues no reload tick");

    // (4) UNSHARED / OWNING turret: bit-identical with the key on and off, and the tick runs.
    const double off = run(fx, false, LIVE, LIVE, 0x7a);
    const int    c0  = g_calls;
    const double on  = run(fx, true, LIVE, LIVE, 0x7a);
    const int    c1  = g_calls;
    ck_eq(c0, 1, "PT-FIX1 off: the owning turret is ticked");
    ck_eq(c1, 1, "PT-FIX1 on: the owning turret is ticked");
    ck_eq_d(on, off, "PT-FIX1: an unshared turret's reload_timer is bit-identical key on and off");
    ck_eq_d(on, START_TMR - 0.1, "PT-FIX1 on: the owner's reload_timer drains by exactly its own budget");
}

} // namespace mh::sim::test
