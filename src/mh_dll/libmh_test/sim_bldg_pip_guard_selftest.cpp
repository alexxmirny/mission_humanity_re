//
// sim_bldg_pip_guard_selftest.cpp -- `simtest` cases for the rc9.4 hotfix `[net] bldg_pip_guard`
// (field crashes 0x0047908b / 0x004793e1, tmp/report_triage_2026-10-15/crash_7908b.md).
//
// Retail indexes the 4-entry A_OGIEN table with a building's pip_level[slot] in update_charge_pips and
// tick_pip_anim, unchecked. The guard (detail::building_tick before the pip-anim gate, and
// detail::bldg_update_charge_pips on entry) zeroes an illegal pip state the way map_CreateBuilding does.
// Cases: planted garbage is zeroed (building_tick and update_charge_pips entries), the grow loop that
// follows only ever reads A_OGIEN[0..3], legal states are bit-identical with the key on and off, and the
// key off leaves the garbage alone (retail).
//
#include "sim/sim_bldg_update_charge_pips.h"
#include "sim/sim_building_tick.h"

#include <cstdint>
#include <cstring>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr uint16_t PLAYER  = 1;
constexpr int32_t  INDEX   = 10;
constexpr uint16_t CFG_ROW = 7;
constexpr int32_t  GARBAGE = (int32_t)0xC384DE82u; // one of the field crash's candidate values (negative)

sim_fixture *g_fx        = nullptr;
int          g_pip_calls = 0;

void noop_reload(uint16_t, uint32_t, double) {}
void noop_anim() {}
void rec_pip(uint16_t, int32_t) { ++g_pip_calls; }
void noop_damage() {}
void drain_budget() { g_fx->tick_budget = 0.0; }

const building_tick_calls g_tick_calls = {&noop_reload, &noop_anim, &rec_pip, &noop_damage};

int32_t                             never_rand(int32_t) { return 0; } // pip_slot_count 1: the grow loop never rolls
const bldg_update_charge_pips_calls g_pip_calls_tbl = {&never_rand};

building &setup(sim_fixture &fx, const int32_t lv[4], int32_t active) {
    fx.reset();
    g_fx                                     = &fx;
    g_pip_calls                              = 0;
    building &b                              = fx.b(PLAYER, INDEX);
    b.building_id                            = CFG_ROW;
    b.state                                  = 0x7a;
    b.built_flags                            = 3;
    b.last_tick_time                         = 0.0;
    fx.bldg_state_funcs[0x7a]                = &drain_budget;
    fx.cfg_buildings[CFG_ROW].pip_slot_count = 1;
    fx.cfg_buildings[CFG_ROW].energy         = 100.0;
    b.energy                                 = 100.0;
    for (int i = 0; i < 4; ++i) {
        b.pip_level[i] = lv[i];
        b.pip_frame[i] = 55 + i;
        b.pip_timer[i] = 0.25 * i;
    }
    b.pip_active_count  = active;
    fx.game_clock       = 3.5;
    fx.planet_index     = 4;
    fx.cur_building_ptr = &b;
    fx.view_cur_player  = PLAYER;
    fx.view_cur_index   = (uint16_t)INDEX;
    return b;
}

void tick(sim_fixture &fx, bool on) {
    set_bldg_pip_guard(on);
    sim_store own = fx.store();
    detail::building_tick(fx.view(), own, g_tick_calls);
    set_bldg_pip_guard(false);
}

bool zeroed(const building &b, double clock) {
    for (int i = 0; i < 4; ++i)
        if (b.pip_level[i] != 0 || b.pip_frame[i] != 0 || b.pip_timer[i] != clock) return false;
    return b.pip_active_count == 0;
}

} // namespace

void run_bldg_pip_guard_tests() {
    sim_fixture fx;

    // (1) building_tick, key ON: garbage in slot 0 -> the whole pip state is zeroed, the pip anim is skipped.
    {
        const int32_t lv[4] = {GARBAGE, 3, 0, 0};
        building     &b     = setup(fx, lv, 2);
        tick(fx, true);
        ck(zeroed(b, 3.5), "pip guard on: garbage pip_level[0] -> frame/level 0, timer = GAME_CLOCK, active 0");
        ck_eq((uint32_t)g_pip_calls, 0, "pip guard on: tick_pip_anim not reached for the zeroed record");
    }
    // (2) key OFF = retail: the garbage stays and the pip anim runs with it.
    {
        const int32_t lv[4] = {GARBAGE, 3, 0, 0};
        building     &b     = setup(fx, lv, 2);
        tick(fx, false);
        ck_eq((uint32_t)b.pip_level[0], (uint32_t)GARBAGE, "pip guard off: garbage pip_level[0] untouched (retail)");
        ck_eq((uint32_t)g_pip_calls, 1, "pip guard off: tick_pip_anim called (retail)");
    }
    // (3) active count out of range on its own (17 and -1), levels legal.
    for (int32_t bad : {17, -1}) {
        const int32_t lv[4] = {1, 1, 0, 0};
        building     &b     = setup(fx, lv, bad);
        tick(fx, true);
        ck(zeroed(b, 3.5), "pip guard on: pip_active_count outside 0..16 -> zeroed");
    }
    // (3b) a level of 5 in an unused slot (3) is out of range too.
    {
        const int32_t lv[4] = {0, 0, 0, 5};
        building     &b     = setup(fx, lv, 0);
        tick(fx, true);
        ck(zeroed(b, 3.5), "pip guard on: pip_level[3] = 5 -> zeroed");
    }
    // (4) LEGAL states: bit-identical with the key on and off (boundary values included).
    {
        const int32_t lv[4] = {4, 0, 1, 4};
        building     &b     = setup(fx, lv, 16);
        tick(fx, true);
        building on_copy;
        memcpy(&on_copy, &b, sizeof(building));
        const int on_calls = g_pip_calls;
        setup(fx, lv, 16);
        tick(fx, false);
        ck(memcmp(&on_copy, &fx.b(PLAYER, INDEX), sizeof(building)) == 0,
           "pip guard: a legal pip state (levels 0..4, active 16) is bit-identical key on and off");
        ck_eq((uint32_t)on_calls, 1, "pip guard on: a legal record still runs tick_pip_anim");
        ck_eq((uint32_t)fx.b(PLAYER, INDEX).pip_level[0], 4, "pip guard on: legal pip_level[0] = 4 untouched");
    }
    // (5) update_charge_pips entry, key ON: garbage + damage -> zeroed, then the grow loop lights slot 0 from
    //     A_OGIEN[0..3] only (target = 1 slot * 4 * (100-25)/100 = 3), never the garbage index.
    {
        const int32_t lv[4] = {GARBAGE, 0, 0, 0};
        building     &b     = setup(fx, lv, 0);
        b.energy            = 25.0;
        set_bldg_pip_guard(true);
        sim_store own = fx.store();
        detail::bldg_update_charge_pips(fx.view(), own, g_pip_calls_tbl, PLAYER, (uint32_t)INDEX);
        set_bldg_pip_guard(false);
        ck_eq((uint32_t)b.pip_level[0], 3, "pip guard on (update_charge_pips): slot 0 relit to the target level 3");
        ck_eq((uint32_t)b.pip_active_count, 3, "pip guard on (update_charge_pips): active = target 3");
        ck_eq((uint32_t)b.pip_frame[0], (uint32_t)fx.pip_fire_anim_frames[2],
              "pip guard on (update_charge_pips): frame read from A_OGIEN[2] (in bounds)");
    }
    // (6) update_charge_pips, legal state, key on vs off: identical.
    {
        const int32_t lv[4] = {1, 0, 0, 0};
        building     &b     = setup(fx, lv, 1);
        b.energy            = 25.0;
        set_bldg_pip_guard(true);
        sim_store own = fx.store();
        detail::bldg_update_charge_pips(fx.view(), own, g_pip_calls_tbl, PLAYER, (uint32_t)INDEX);
        set_bldg_pip_guard(false);
        building on_copy;
        memcpy(&on_copy, &b, sizeof(building));
        building &b2 = setup(fx, lv, 1);
        b2.energy    = 25.0;
        sim_store o2 = fx.store();
        detail::bldg_update_charge_pips(fx.view(), o2, g_pip_calls_tbl, PLAYER, (uint32_t)INDEX);
        ck(memcmp(&on_copy, &b2, sizeof(building)) == 0,
           "pip guard (update_charge_pips): a legal pip state is bit-identical key on and off");
    }
}

} // namespace mh::sim::test
