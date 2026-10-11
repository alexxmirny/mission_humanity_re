//
// sim_bldg_tick_turret_rate_selftest.cpp -- `simtest` cases for patching:PT-FIX2
// ([net] turret_reload_realtime_fix / turret_fire_rate_mult) in detail::building_tick.
//
// Retail hands llm_strat_bldg_turret_reload_tick the re-counted budget GAME_CLOCK - last_tick_time, and
// the attack state gives an unspent budget back (last_tick_time -= B), so that budget runs dt, 2dt ..
// K*dt (K = ceil(c/dt), c = 0.05) and the reload timer drains ~(K+1)/2 x real time. The fix drains by the
// step's real dt (GAME_TIME_DELTA) times the multiplier.
//
// The reload tick under test is the REAL translated body; the state handler is a MODEL of the attack
// state's budget handling for an aligned target (c = 0.05 per pass, unspent budget given back, fire when
// ready and budget remains) -- the model of the PT-FIX2 analysis, so the fix-off arm reproduces its
// numbers. Observables: the reload timer's ready time (within one step of short/mult) and the fire
// interval (ready time + the attack state's own 0.05 s poll).
//
#include "sim/sim_bldg_turret_reload_tick.h"
#include "sim/sim_building_tick.h"
#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_TURRET

#include <cmath>
#include <cstdint>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr uint16_t PLAYER   = 1;
constexpr uint8_t  SLOT     = 3;
constexpr int32_t  INDEX    = 4;
constexpr uint16_t CFG_ROW  = 9;
constexpr double   ATTACK_C = 0.05; // per_shot_cost 0.1 * 0.5

sim_fixture        *g_fx    = nullptr;
double              g_short = 0.2;
std::vector<double> g_fires;
std::vector<double> g_ready; // fire -> reload-ready times, read off the reload timer
double              g_last_fire = -1;

turret &the_turret();
void    rec_reload(uint16_t player, uint32_t index, double budget) {
    sim_store     own = g_fx->store();
    const uint8_t was = the_turret().reload_ready_flag;
    detail::bldg_turret_reload_tick(g_fx->view(), own, player, index, budget);
    if (!was && the_turret().reload_ready_flag && g_last_fire >= 0) g_ready.push_back(g_fx->game_clock - g_last_fire);
}
void noop_anim() {}
void noop_pip(uint16_t, int32_t) {}
void noop_damage() {}

turret &the_turret() { return g_fx->turrets[(size_t)PLAYER * (size_t)TURRETS_PER_PLAYER + SLOT]; }

// One call of the attack state (aligned target): see the banner.
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
            t.reload_timer      = g_short;
            t.reload_ready_flag = 0;
            g_fires.push_back(g_fx->game_clock);
            g_last_fire = g_fx->game_clock;
        }
    }
}

const building_tick_calls g_tick_calls = {&rec_reload, &noop_anim, &noop_pip, &noop_damage};

void arm(sim_fixture &fx, uint8_t built_flags) {
    fx.reset();
    g_fx = &fx;
    g_fires.clear();
    g_ready.clear();
    g_last_fire                    = -1;
    building &b                    = fx.b(PLAYER, INDEX);
    b.building_id                  = CFG_ROW;
    b.sub_id                       = SLOT;
    b.state                        = 0x7a;
    b.built_flags                  = built_flags;
    b.last_tick_time               = 0.0;
    fx.cfg_buildings[CFG_ROW].type = BUILDING_TYPE_A_TURRET;
    fx.bldg_state_funcs[0x7a]      = &attack_state;
    turret &t                      = the_turret();
    t.b_index                      = (int16_t)INDEX;
    t.weapon_id                    = 1;
    t.ammo                         = 5;
    fx.cur_building_ptr            = &b;
    fx.view_cur_player             = PLAYER;
    fx.view_cur_index              = (uint16_t)INDEX;
}

struct result {
    double avg_interval = 0, min_interval = 0, max_interval = 0;
    double avg_ready = 0; // fire -> reload ready, read off the reload timer
    int    shots     = 0;
};

result run(sim_fixture &fx, bool fix, double mult, double dt, double short_time, double seconds) {
    arm(fx, 3);
    g_short = short_time;
    set_turret_reload_realtime(fix, mult);
    turret &t           = the_turret();
    t.reload_ready_flag = 1; // created armed
    t.reload_timer      = 0.0;

    const int n = (int)(seconds / dt + 0.5);
    for (int i = 1; i <= n; ++i) {
        fx.game_clock      = fx.game_clock + dt; // accumulated, as sim_tick does
        fx.game_time_delta = dt;
        sim_store own      = fx.store();
        detail::building_tick(fx.view(), own, g_tick_calls);
    }
    set_turret_reload_realtime(false, 2.0);

    result r;
    r.shots = (int)g_fires.size();
    if (g_fires.size() >= 3) {
        double sum = 0, mn = 1e9, mx = 0;
        for (size_t i = 1; i < g_fires.size(); ++i) {
            const double iv = g_fires[i] - g_fires[i - 1];
            sum += iv;
            mn = iv < mn ? iv : mn;
            mx = iv > mx ? iv : mx;
        }
        r.avg_interval = sum / (double)(g_fires.size() - 1);
        r.min_interval = mn;
        r.max_interval = mx;
    }
    if (!g_ready.empty()) {
        double sum = 0;
        for (double v : g_ready) sum += v;
        r.avg_ready = sum / (double)g_ready.size();
    }
    return r;
}

void ck_true(bool c, const char *what) { ck_eq(c ? 1u : 0u, 1u, what); }
bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

} // namespace

void run_bldg_tick_turret_rate_tests() {
    sim_fixture  fx;
    const double dts[]    = {0.002, 0.010, 0.020, 0.100};
    const double shorts[] = {0.2, 1.75}; // weapon 8 / weapon 21 short_time
    const double mult     = 2.0;

    // (a) fix ON: the reload timer is READY within one step of short/mult at every step size, and the
    // fire interval is that plus the attack state's own poll (<= ceil(c/dt)*dt + one step).
    for (double sh : shorts) {
        for (double dt : dts) {
            const result r    = run(fx, true, mult, dt, sh, sh < 1.0 ? 60.0 : 300.0);
            const double want = sh / mult;
            ck_true(r.shots > 10, "PT-FIX2 on: the turret fires repeatedly");
            ck_true(r.avg_ready > want - 1e-9 && r.avg_ready < want + dt + 1e-9,
                    "PT-FIX2 on: fire -> reload ready equals short_time/mult within one step");
            const double poll = std::ceil(ATTACK_C / dt) * dt + dt;
            ck_true(r.min_interval >= want - 1e-9 && r.max_interval <= want + poll + 1e-9,
                    "PT-FIX2 on: fire interval = short_time/mult + at most the attack state's own poll");
        }
    }
    // mult scales the reload time: 1.0 = cfg timing, 4.0 = a quarter; above the clamp acts as 10.
    ck_true(near(run(fx, true, 1.0, 0.020, 1.75, 300.0).avg_ready, 1.75, 0.0201), "PT-FIX2 mult=1: reload ready at the cfg short_time");
    ck_true(near(run(fx, true, 4.0, 0.020, 1.75, 300.0).avg_ready, 1.75 / 4.0, 0.0201), "PT-FIX2 mult=4: reload ready at short_time/4");
    ck_true(near(run(fx, true, 100.0, 0.020, 1.75, 300.0).avg_ready, 1.75 / 10.0, 0.0201), "PT-FIX2: a mult above the clamp acts as 10");

    // (a) fix OFF reproduces the re-count model.
    ck_true(near(run(fx, false, mult, 0.010, 0.2, 60.0).avg_interval, 0.1056, 0.003), "PT-FIX2 off: weapon 8 @10 ms ~0.106 s (re-count model)");
    ck_true(near(run(fx, false, mult, 0.020, 0.2, 60.0).avg_interval, 0.1083, 0.003), "PT-FIX2 off: weapon 8 @20 ms ~0.108 s");
    ck_true(near(run(fx, false, mult, 0.100, 0.2, 60.0).avg_interval, 0.2005, 0.01), "PT-FIX2 off: weapon 8 @100 ms ~0.20 s (cfg timing)");
    ck_true(near(run(fx, false, mult, 0.010, 1.75, 300.0).avg_interval, 0.5429, 0.01), "PT-FIX2 off: weapon 21 @10 ms ~0.54 s");
    ck_true(near(run(fx, false, mult, 0.020, 1.75, 300.0).avg_interval, 0.8917, 0.01), "PT-FIX2 off: weapon 21 @20 ms ~0.89 s");
    ck_true(near(run(fx, false, mult, 0.100, 1.75, 300.0).avg_interval, 1.7995, 0.02), "PT-FIX2 off: weapon 21 @100 ms ~1.80 s (cfg timing)");

    // At the retail MP step (0.1 s) with mult 1.0 the fix is a no-op against retail.
    {
        const result a = run(fx, false, 1.0, 0.100, 1.75, 300.0);
        const result b = run(fx, true, 1.0, 0.100, 1.75, 300.0);
        // (retail jitters 1.7/1.8 s at this exact tie -- 0.1 s steps against a 0.1 s multiple, float noise in
        // clock - last_tick_time; the real-dt drain is exact there -- so the comparison is to the cfg time.)
        ck_true(near(a.avg_interval, 1.8, 0.02) && near(b.avg_interval, 1.8, 1e-6),
                "PT-FIX2: at the retail MP step with mult 1.0 the fix gives the cfg timing retail only approximates");
    }

    // The construction energy gate (budget forced to 0) must not drain the timer with the fix on.
    {
        arm(fx, 1); // under construction
        fx.b(PLAYER, INDEX).energy = 5.0;
        set_turret_reload_realtime(true, 2.0);
        turret &t           = the_turret();
        t.reload_ready_flag = 0;
        t.reload_timer      = 1.0;
        fx.game_clock       = 0.1;
        fx.game_time_delta  = 0.1;
        sim_store own       = fx.store();
        detail::building_tick(fx.view(), own, g_tick_calls);
        set_turret_reload_realtime(false, 2.0);
        ck_eq_d(t.reload_timer, 1.0, "PT-FIX2 on: a zero budget (construction energy gate) drains nothing");
    }
    ck_eq_d(clamp_turret_fire_rate_mult(0.0), 0.1, "PT-FIX2: mult clamps up to 0.1");
    ck_eq_d(clamp_turret_fire_rate_mult(1e9), 10.0, "PT-FIX2: mult clamps down to 10");
}

} // namespace mh::sim::test
