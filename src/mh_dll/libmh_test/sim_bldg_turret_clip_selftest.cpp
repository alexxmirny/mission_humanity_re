//
// libmh_test/sim_bldg_turret_clip_selftest.cpp -- patching:PT-FIX4 ([net] turret_clip_fix): the turret magazine.
//
// Retail map_CreateBuilding never initialises turret.ammo (+0x2f) / resupply_cycles_remaining (+0x33), and
// llm_strat_bldg_turret_reload_tick refills only at ammo == 0, so the first shot takes ammo to -1 and the clip /
// long reload never engage. With the key on: (A) creation sets ammo = Weapon.ammo and resupply = -1 (fresh AND
// reused slots); (B) the refill runs for ammo <= 0 (signed), healing a turret that already carries negative ammo.
//
// The create_building body and the reload tick under test are the REAL translated bodies. The attack state is a
// MODEL of llm_strat_turret_fire's bookkeeping (0x0047c01d-0x0047c0f8): ammo -= 1; ammo != 0 -> reload = short_time,
// ammo == 0 -> reload = long_time. Weapon 8 of the cfg: ammo 3, short 0.2, long 0.4.
//
#include "sim/sim_bldg_turret_reload_tick.h"
#include "sim/sim_building_tick.h"
#include "sim/sim_map_create_building.h"
#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_TURRET

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr uint16_t PLAYER   = 1;
constexpr uint8_t  SLOT     = 3;
constexpr int32_t  INDEX    = 4;
constexpr int32_t  CFG_ROW  = 9;
constexpr uint8_t  WEAPON   = 1;
constexpr double   ATTACK_C = 0.05;
constexpr double   SHORT_T  = 0.2;
constexpr double   LONG_T   = 0.4;

sim_fixture        *g_fx = nullptr;
std::vector<double> g_fires;
std::vector<int>    g_fire_ammo; // ammo after each shot
std::vector<double> g_fire_timer;

turret &the_turret() { return g_fx->turrets[(size_t)PLAYER * (size_t)TURRETS_PER_PLAYER + SLOT]; }

// ---- create_building stubs (all no-ops except the zero-fill the body relies on) ----------------
void *stub_fill(void *p, uint32_t n, uint8_t d) {
    memset(p, d, n);
    return p;
}
uint32_t stub_event(uint32_t) { return 0; }
void     stub_sight(uint32_t, int32_t, int32_t, int32_t, uint8_t) {}
void     stub_area(int32_t, int32_t, uint8_t *) {}
int32_t  stub_assign(uint32_t, uint32_t, int32_t) { return 0; }
void     stub_staff(uint16_t, int32_t) {}
void     stub_refresh(uint16_t, int32_t) {}

const create_building_calls g_create_calls = {stub_fill, stub_event, stub_sight, stub_area, stub_assign, stub_staff, stub_refresh};

void create_turret(sim_fixture &fx) {
    g_fx                                                                = &fx;
    cfg_building &cfg                                                   = fx.cfg_buildings[CFG_ROW];
    cfg.type                                                            = BUILDING_TYPE_A_TURRET;
    cfg.builder_count                                                   = 0;
    cfg.sight                                                           = 8;
    cfg.weapon_id                                                       = WEAPON;
    fx.cfg_weapons[WEAPON].ammo                                         = 3;
    fx.turrets[(size_t)PLAYER * (size_t)TURRETS_PER_PLAYER + 0].b_index = 0;
    sim_store own                                                       = fx.store();
    detail::create_building(fx.view(), own, g_create_calls, PLAYER, (uint32_t)INDEX, 50u, 12, CFG_ROW, 0u, (uint32_t)SLOT);
}

// ---- tick model ----------------------------------------------------------------------------------
void rec_reload(uint16_t player, uint32_t index, double budget) {
    sim_store own = g_fx->store();
    detail::bldg_turret_reload_tick(g_fx->view(), own, player, index, budget);
}
void noop_anim() {}
void noop_pip(uint16_t, int32_t) {}
void noop_damage() {}

// One call of the attack state (aligned target), with llm_strat_turret_fire's ammo bookkeeping.
void attack_state() {
    double   &B = g_fx->tick_budget;
    building &b = g_fx->b(PLAYER, INDEX);
    turret   &t = the_turret();
    if (B < ATTACK_C) {
        b.last_tick_time -= B;
        B = 0.0;
        return;
    }
    B -= ATTACK_C;
    if (B > 0.0) {
        if (!t.reload_ready_flag) {
            B = 0.0;
        } else {
            t.ammo -= 1;
            t.reload_timer      = t.ammo != 0 ? SHORT_T : LONG_T;
            t.reload_ready_flag = 0;
            g_fires.push_back(g_fx->game_clock);
            g_fire_ammo.push_back(t.ammo);
            g_fire_timer.push_back(t.reload_timer);
        }
    }
}

const building_tick_calls g_tick_calls = {&rec_reload, &noop_anim, &noop_pip, &noop_damage};

void arm_building(sim_fixture &fx) {
    building &b               = fx.b(PLAYER, INDEX);
    b.building_id             = CFG_ROW;
    b.sub_id                  = SLOT;
    b.state                   = 0x7a;
    b.built_flags             = 3;
    b.last_tick_time          = 0.0;
    fx.bldg_state_funcs[0x7a] = &attack_state;
    fx.cur_building_ptr       = &b;
    fx.view_cur_player        = PLAYER;
    fx.view_cur_index         = (uint16_t)INDEX;
}

void run_ticks(sim_fixture &fx, double dt, double seconds) {
    const int n = (int)(seconds / dt + 0.5);
    for (int i = 1; i <= n; ++i) {
        fx.game_clock      = fx.game_clock + dt;
        fx.game_time_delta = dt;
        sim_store own      = fx.store();
        detail::building_tick(fx.view(), own, g_tick_calls);
    }
}

void ck_true(bool c, const char *what) { ck_eq(c ? 1u : 0u, 1u, what); }

// Fresh run: create the turret (clip on/off), tick for `seconds`, record fires.
void fresh_run(sim_fixture &fx, bool clip, double dt, double seconds) {
    fx.reset();
    g_fires.clear();
    g_fire_ammo.clear();
    g_fire_timer.clear();
    set_turret_clip_fix(clip);
    set_turret_reload_realtime(true, 1.0); // drain = real dt: the cfg timing, so the cycle is easy to read
    create_turret(fx);
    arm_building(fx);
    run_ticks(fx, dt, seconds);
    set_turret_reload_realtime(false, 2.0);
    set_turret_clip_fix(false);
}

} // namespace

void run_bldg_turret_clip_tests() {
    sim_fixture fx;

    // (A) creation: fresh slot (zeroed) and reused slot (the dead turret's negative ammo / stale countdown).
    for (int reused = 0; reused < 2; ++reused) {
        for (int on = 0; on < 2; ++on) {
            fx.reset();
            g_fx = &fx;
            if (reused) {
                turret &t0                   = the_turret();
                t0.ammo                      = -5;
                t0.resupply_cycles_remaining = 7;
            }
            set_turret_clip_fix(on != 0);
            create_turret(fx);
            set_turret_clip_fix(false);
            const turret &t = the_turret();
            if (on) {
                ck_eq((uint32_t)t.ammo, 3u, "PT-FIX4 on: creation sets ammo = Weapon.ammo (fresh and reused slot)");
                ck_eq((uint32_t)t.resupply_cycles_remaining, 0xffffffffu, "PT-FIX4 on: creation sets resupply_cycles_remaining = -1");
            } else if (reused) {
                ck_eq((uint32_t)t.ammo, (uint32_t)-5, "PT-FIX4 off: a reused slot keeps the dead turret's ammo (retail)");
                ck_eq((uint32_t)t.resupply_cycles_remaining, 7u, "PT-FIX4 off: resupply_cycles_remaining untouched (retail)");
            } else {
                ck_eq((uint32_t)t.ammo, 0u, "PT-FIX4 off: a fresh slot starts at ammo 0 (retail)");
                ck_eq((uint32_t)t.resupply_cycles_remaining, 0u, "PT-FIX4 off: a fresh slot's resupply countdown stays 0 (retail)");
            }
            ck_eq((uint32_t)t.reload_ready_flag, 1u, "PT-FIX4: the turret is still created armed");
            ck_eq((uint32_t)t.resupply_available, 1u, "PT-FIX4: resupply_available still 1");
        }
    }

    // (B) the cycle, key ON: 3 shots at short_time, then long_time, repeating; ammo 2,1,0,2,1,0.
    {
        const double dt = 0.010;
        fresh_run(fx, true, dt, 12.0);
        ck_true(g_fires.size() >= 12, "PT-FIX4 on: the turret fires repeatedly");
        bool ok_ammo = true, ok_timer = true;
        for (size_t i = 0; i < g_fires.size(); ++i) {
            const int want_ammo = (int)(2 - (i % 3));
            if (g_fire_ammo[i] != want_ammo) ok_ammo = false;
            if (std::fabs(g_fire_timer[i] - (want_ammo == 0 ? LONG_T : SHORT_T)) > 1e-9) ok_timer = false;
        }
        ck_true(ok_ammo, "PT-FIX4 on: ammo after the shots runs 2,1,0,2,1,0 (the refill restores Weapon.ammo)");
        ck_true(ok_timer, "PT-FIX4 on: the 3rd shot of each clip arms long_time, the others short_time");
        const double poll  = std::ceil(ATTACK_C / dt) * dt + dt;
        bool         ok_iv = true;
        for (size_t i = 1; i < g_fires.size(); ++i) {
            const double iv   = g_fires[i] - g_fires[i - 1];
            const double want = ((i - 1) % 3 == 2) ? LONG_T : SHORT_T; // interval after shot i-1 (0-based)
            if (iv < want - 1e-9 || iv > want + poll + 1e-9) ok_iv = false;
        }
        ck_true(ok_iv, "PT-FIX4 on: 3 shots at short_time, then a long_time wait (each + at most the attack poll)");
    }

    // (B') key OFF: retail -- ammo runs -1,-2,-3 ..., every interval is short_time.
    {
        const double dt = 0.010;
        fresh_run(fx, false, dt, 12.0);
        ck_true(g_fires.size() >= 12, "PT-FIX4 off: the turret fires repeatedly");
        bool         ok_ammo = true, ok_iv = true;
        const double poll = std::ceil(ATTACK_C / dt) * dt + dt;
        for (size_t i = 0; i < g_fires.size(); ++i) {
            if (g_fire_ammo[i] != -(int)(i + 1)) ok_ammo = false;
            if (std::fabs(g_fire_timer[i] - SHORT_T) > 1e-9) ok_ammo = false;
            if (i && g_fires[i] - g_fires[i - 1] > SHORT_T + poll + 1e-9) ok_iv = false;
        }
        ck_true(ok_ammo, "PT-FIX4 off: ammo -1,-2,-3 ... and the timer is always short_time (retail)");
        ck_true(ok_iv, "PT-FIX4 off: every interval is the short one (retail)");
    }

    // (C) the refill condition: a completed reload with ammo -5 / 0 / 2.
    for (int on = 0; on < 2; ++on) {
        const int32_t seeds[3] = {-5, 0, 2};
        for (int32_t seed : seeds) {
            fx.reset();
            g_fx = &fx;
            create_turret(fx);
            set_turret_clip_fix(on != 0);
            turret &t           = the_turret();
            t.ammo              = seed;
            t.reload_ready_flag = 0;
            t.reload_timer      = 0.1;
            sim_store own       = fx.store();
            detail::bldg_turret_reload_tick(fx.view(), own, PLAYER, (uint32_t)INDEX, 0.2);
            set_turret_clip_fix(false);
            ck_eq((uint32_t)t.reload_ready_flag, 1u, "PT-FIX4: the completed reload marks the turret ready");
            uint32_t want = (uint32_t)seed;
            if (seed == 0) want = 3u;
            if (seed < 0 && on) want = 3u;
            ck_eq((uint32_t)t.ammo, want,
                  on ? "PT-FIX4 on: refill at ammo <= 0 (-5 and 0 heal to Weapon.ammo, 2 untouched)"
                     : "PT-FIX4 off: refill only at exactly 0 (-5 stays -5)");
        }
    }
}

} // namespace mh::sim::test
