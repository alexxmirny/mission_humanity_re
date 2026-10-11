//
// libmh_test/sim_bldg_turret_poll_selftest.cpp -- patching:PT-FIX5 ([net] turret_fire_poll_fix): a finished reload
// fires on the step it completes.
//
// llm_strat_bldg_state_turret_attack charges every aim pass per_shot_cost * 0.5 = 0.05 s of the step's tick budget
// (budget below that is banked into last_tick_time) and its final gate fires only with budget LEFT and
// reload_ready_flag == 1, so a ready, already-aimed turret waited for the poll: fire interval = reload + 0..~0.06 s.
// With the key on a READY turret already within the aim tolerance skips the aim loop and fires at once.
//
// The attack state, the reload tick and the building tick under test are the REAL translated bodies (the outward
// calls are recorders; turret_fire is a MODEL of llm_strat_turret_fire's timer bookkeeping: reload_timer = short_time,
// reload_ready_flag = 0). Observables: the fire interval at dt 2/10/20/100 ms (weapon 8 short 0.2 and weapon 21 short
// 1.75, mult 2.0), the first-fire time of a turret that must turn (unchanged by the key), and single-call cases.
//
#include "sim/sim_bldg_state_turret.h"
#include "sim/sim_bldg_turret_reload_tick.h"
#include "sim/sim_building_tick.h"
#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_TURRET

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr uint16_t PLAYER    = 1;
constexpr uint8_t  SLOT      = 3;
constexpr int32_t  INDEX     = 4;
constexpr uint16_t CFG_ROW   = 9;
constexpr uint16_t ATTACK_ST = 0x7b; // BLDG_STATE_TURRET_ATTACK
constexpr int32_t  BEARING   = 16;
constexpr double   COST      = 0.1; // cfg per_shot_cost; the attack state halves it -> 0.05 s per aim pass
constexpr double   POLL      = 0.05;
constexpr uint32_t TGT_OWNER = 2;
constexpr int32_t  TGT_SLOT  = 4;

sim_fixture        *g_fx    = nullptr;
double              g_short = 0.2;
std::vector<double> g_fires;

turret &the_turret() { return g_fx->turrets[(size_t)PLAYER * (size_t)TURRETS_PER_PLAYER + SLOT]; }

void rec_coords(uint16_t, int32_t, int32_t *x, int32_t *y) {
    *x = 0;
    *y = 0;
}
void     rec_unit_coords(uint16_t, int32_t, int32_t *x, int32_t *y) { *x = *y = 0; }
uint32_t rec_dist(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) { return 0; }
int32_t  rec_dir(int32_t, int32_t, int32_t, int32_t) { return BEARING; }
void     rec_notify(uint16_t, uint32_t) {}
void     rec_fire(uint32_t, uint32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, uint8_t) {
    turret &t           = the_turret();
    t.reload_timer      = g_short;
    t.reload_ready_flag = 0;
    g_fires.push_back(g_fx->game_clock);
}
const bldg_state_turret_attack_calls g_attack_calls = {&rec_coords, &rec_unit_coords, &rec_dist, &rec_dir, &rec_notify, &rec_fire};

void attack_state() {
    sim_store own = g_fx->store();
    detail::bldg_state_turret_attack(g_fx->view(), own, g_attack_calls);
}

void rec_reload(uint16_t player, uint32_t index, double budget) {
    sim_store own = g_fx->store();
    detail::bldg_turret_reload_tick(g_fx->view(), own, player, index, budget);
}
void                      noop_anim() {}
void                      noop_pip(uint16_t, int32_t) {}
void                      noop_damage() {}
const building_tick_calls g_tick_calls = {&rec_reload, &noop_anim, &noop_pip, &noop_damage};

void arm(sim_fixture &fx, int32_t heading) {
    fx.reset();
    g_fx = &fx;
    g_fires.clear();
    building &b                               = fx.b(PLAYER, INDEX);
    b.building_id                             = CFG_ROW;
    b.sub_id                                  = SLOT;
    b.state                                   = ATTACK_ST;
    b.built_flags                             = 3;
    b.efficiency                              = 1.0;
    b.last_tick_time                          = 0.0;
    fx.cfg_buildings[CFG_ROW].type            = BUILDING_TYPE_A_TURRET;
    fx.cfg_buildings[CFG_ROW].per_shot_cost   = COST;
    fx.cfg_buildings[CFG_ROW].sprite_quantity = 1;
    building &tb                              = fx.b(TGT_OWNER, TGT_SLOT);
    tb.building_id                            = 55;
    tb.energy                                 = 40.0;
    fx.bldg_state_funcs[ATTACK_ST]            = &attack_state;
    turret &t                                 = the_turret();
    t.b_index                                 = (int16_t)INDEX;
    t.weapon_id                               = 1;
    t.ammo                                    = 5;
    t.counter_ref                             = (uint8_t)TGT_OWNER; // BUILDING counter-target
    t.counter_target_slot                     = TGT_SLOT;
    t.aim_heading                             = heading;
    t.reload_ready_flag                       = 1;
    t.reload_timer                            = 0.0;
    fx.cur_building_ptr                       = &b;
    fx.view_cur_player                        = PLAYER;
    fx.view_cur_index                         = (uint16_t)INDEX;
}

struct result {
    double avg = 0, mn = 0, mx = 0;
    double first = -1; // clock of the first fire
    int    shots = 0;
};

result run(sim_fixture &fx, bool poll_fix, double dt, double short_time, double seconds, int32_t heading, double mult = 2.0) {
    arm(fx, heading);
    g_short = short_time;
    set_turret_reload_realtime(true, mult);
    set_turret_fire_poll_fix(poll_fix);
    const int n = (int)(seconds / dt + 0.5);
    for (int i = 1; i <= n; ++i) {
        fx.game_clock      = fx.game_clock + dt; // accumulated, as sim_tick does
        fx.game_time_delta = dt;
        sim_store own      = fx.store();
        detail::building_tick(fx.view(), own, g_tick_calls);
    }
    set_turret_fire_poll_fix(false);
    set_turret_reload_realtime(false, 2.0);

    result r;
    r.shots = (int)g_fires.size();
    if (!g_fires.empty()) r.first = g_fires[0];
    if (g_fires.size() >= 3) {
        double sum = 0, mn = 1e9, mx = 0;
        for (size_t i = 1; i < g_fires.size(); ++i) {
            const double iv = g_fires[i] - g_fires[i - 1];
            sum += iv;
            mn = iv < mn ? iv : mn;
            mx = iv > mx ? iv : mx;
        }
        r.avg = sum / (double)(g_fires.size() - 1);
        r.mn  = mn;
        r.mx  = mx;
    }
    return r;
}

void ck_true(bool c, const char *what) { ck_eq(c ? 1u : 0u, 1u, what); }

} // namespace

void run_bldg_turret_poll_tests() {
    sim_fixture fx;
    if (std::getenv("MH_PT5_PRINT")) { // print the measured table
        for (double sh : {0.2, 1.75})
            for (double dt : {0.002, 0.010, 0.020, 0.100})
                for (int on = 0; on < 2; ++on) {
                    const result r = run(fx, on != 0, dt, sh, sh < 1.0 ? 60.0 : 300.0, BEARING);
                    std::printf("PT5 w_short=%.2f dt=%.3f key=%d avg=%.4f min=%.4f max=%.4f shots=%d\n", sh, dt, on, r.avg, r.mn, r.mx, r.shots);
                }
    }
    const double dts[]    = {0.002, 0.010, 0.020, 0.100};
    const double shorts[] = {0.2, 1.75};
    const double mult     = 2.0;

    // (a) key ON, aimed: every fire interval is short/mult within one step, at every step size.
    for (double sh : shorts) {
        for (double dt : dts) {
            const result r    = run(fx, true, dt, sh, sh < 1.0 ? 60.0 : 300.0, BEARING);
            const double want = sh / mult;
            ck_true(r.shots > 10, "PT-FIX5 on: the aimed turret fires repeatedly");
            ck_true(r.mn >= want - 1e-9 && r.mx <= want + dt + 1e-9,
                    "PT-FIX5 on: aimed fire interval = short_time/mult within one step");
        }
    }
    // (b) key OFF is the retail poll: the same turret fires up to ~0.06 s late at the small steps (weapon 8).
    {
        const result off20 = run(fx, false, 0.020, 0.2, 60.0, BEARING);
        const result on20  = run(fx, true, 0.020, 0.2, 60.0, BEARING);
        const result off10 = run(fx, false, 0.010, 0.2, 60.0, BEARING);
        const result on10  = run(fx, true, 0.010, 0.2, 60.0, BEARING);
        const result off02 = run(fx, false, 0.002, 0.2, 60.0, BEARING);
        const result on02  = run(fx, true, 0.002, 0.2, 60.0, BEARING);
        ck_true(off20.avg > on20.avg + 0.005, "PT-FIX5 off: weapon 8 @20 ms is slower than with the key on (the poll)");
        ck_true(off10.avg > on10.avg + 0.005, "PT-FIX5 off: weapon 8 @10 ms is slower than with the key on");
        ck_true(off02.avg > on02.avg + 0.0005 && off02.mx > on02.mx + 0.02, "PT-FIX5 off: weapon 8 @2 ms has late shots (up to ~0.15 s) the key removes");
        ck_true(off10.mx > 0.2 / mult + 0.04 && off20.mx > on20.mx + 0.01, "PT-FIX5 off: the retail interval exceeds short/mult by more than a step");
        // The retail step (0.1 s): one aim pass fits in the step, so nothing waits and the key changes nothing.
        const result off100 = run(fx, false, 0.100, 0.2, 60.0, BEARING);
        const result on100  = run(fx, true, 0.100, 0.2, 60.0, BEARING);
        ck_true(std::fabs(off100.avg - on100.avg) < 1e-9, "PT-FIX5: @100 ms the key changes nothing (the pass always fits)");
    }
    // (c) turning is unchanged: a turret 6 headings off (outside the tolerance 2) needs 4 passes = 0.2 s before its
    // first shot, with the key on or off, at every step size.
    for (double dt : dts) {
        const result off = run(fx, false, dt, 0.2, 5.0, 10);
        const result on  = run(fx, true, dt, 0.2, 5.0, 10);
        ck_true(off.first >= 4 * POLL - 1e-9, "PT-FIX5: turning costs 0.05 s per heading (key off)");
        ck_true(std::fabs(on.first - off.first) < 1e-9, "PT-FIX5: the first shot of a turning turret is not moved by the key");
        ck_true(the_turret().aim_heading >= 14, "PT-FIX5: the turret reached the tolerance band");
    }
    // (d) single-call cases. Aimed + ready + a budget below one pass: key on fires and leaves the budget whole;
    // key off banks it and does not fire.
    for (int on = 0; on < 2; ++on) {
        arm(fx, BEARING);
        g_short        = 0.2;
        fx.tick_budget = 0.01;
        set_turret_fire_poll_fix(on != 0);
        attack_state();
        set_turret_fire_poll_fix(false);
        ck_eq((uint32_t)g_fires.size(), on ? 1u : 0u, on ? "PT-FIX5 on: aimed + ready + 0.01 s budget fires" : "PT-FIX5 off: the same call does not fire");
        ck_eq_d(fx.tick_budget, on ? 0.01 : 0.0, on ? "PT-FIX5 on: the budget is left whole" : "PT-FIX5 off: the budget was banked");
    }
    // Not ready: key on takes the retail loop (the pass is charged, the leftover discarded, nothing fires).
    {
        arm(fx, BEARING);
        g_short                        = 0.2;
        the_turret().reload_ready_flag = 0;
        fx.tick_budget                 = 0.2;
        set_turret_fire_poll_fix(true);
        attack_state();
        set_turret_fire_poll_fix(false);
        ck_eq((uint32_t)g_fires.size(), 0u, "PT-FIX5 on: a turret still reloading does not fire");
        ck_eq_d(fx.tick_budget, 0.0, "PT-FIX5 on: a turret still reloading discards the leftover (retail)");
    }
    // Ready but 6 headings off: key on takes the retail loop and turns (one pass costs 0.05 s).
    {
        arm(fx, 10);
        g_short        = 0.2;
        fx.tick_budget = 0.06;
        set_turret_fire_poll_fix(true);
        attack_state();
        set_turret_fire_poll_fix(false);
        ck_eq((uint32_t)the_turret().aim_heading, 11u, "PT-FIX5 on: a turret that must turn still turns one heading per 0.05 s");
        ck_eq((uint32_t)g_fires.size(), 0u, "PT-FIX5 on: a turning turret does not fire");
    }
}

} // namespace mh::sim::test
