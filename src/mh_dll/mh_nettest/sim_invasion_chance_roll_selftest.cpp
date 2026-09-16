//
// sim_invasion_chance_roll_selftest.cpp -- `simtest` offline oracle for llm_strat_invasion_chance_roll
// (sim/sim_invasion.h/.cpp, RI-SIM / SIM1F). chance_roll calls game_HandleInvasion DIRECTLY
// as an in-TU C++ call (not through the `invasion_calls` struct), so exercising a "the roll succeeded"
// path here necessarily also runs handle_invasion's own body -- that is inherent to the two functions'
// translation shape (see sim_invasion.h's banner), not scope creep. This file's assertions stay focused
// on CHANCE_ROLL's own decisions (the early-out gate, the two roll thresholds, the *3 multiplier, the
// current_system*2 subtraction, and the loop's early-exit-on-first-success); handle_invasion's own
// eligibility/branch logic gets its dedicated, more thorough coverage in
// sim_handle_invasion_selftest.cpp, called directly (not through this indirection).
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY (tmp/decomp/llm_strat_invasion_chance_roll_0049953a.asm),
// not the .cpp -- see sim_invasion.h's banner for the address-by-address pseudocode this test walks
// against. Every outward call (rand_below + the six invasion_calls callees, all shared with
// handle_invasion since it is called with the SAME `c`) is a RECORDING STUB -- nothing here reaches the
// live game image, so this oracle is fully offline.
//
#include "sim/sim_invasion.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- recording stubs for every invasion_calls callee -------------------------------------------------
std::vector<int32_t> g_rand_below_args;
std::vector<int32_t> g_rand_below_returns;
size_t               g_rand_below_pos = 0;

struct VssCall {
    void          *dst;
    const wchar_t *fmt, *a0, *a1;
};
struct ViCall {
    void          *dst;
    const wchar_t *fmt;
    int32_t        a0;
};
std::vector<ViCall>  g_vi_calls;
std::vector<VssCall> g_vss_calls;
std::vector<void *>  g_print_calls;
int32_t              g_spawn_return = 1; // set per-case before run()
std::vector<int32_t> g_spawn_calls;      // count via size(); no args (spawn_enemy_landing takes none)
struct RevokeCall {
    uint16_t player, progress_id;
};
std::vector<RevokeCall> g_revoke_calls;
struct PresenceCall {
    uint32_t player, mode;
};
std::vector<PresenceCall> g_presence_calls;
struct AlertCall {
    int32_t planet;
    double  timestamp;
};
std::vector<AlertCall> g_alert_calls;

// Clears only the RECORDING accumulators (the *_calls/*_args logs) and rewinds the rand_below
// replay cursor -- NOT the PROGRAMMED INPUTS (g_rand_below_returns, g_spawn_return). run() calls
// this as its first action, and each case seeds those inputs BEFORE calling run(); clearing them
// here (as an earlier revision did) wiped the seeding, so every case that queued a roll saw
// rand_below(...)==0 and every "spawn fails" case saw spawn succeed. Same recordings-only shape as
// the template selftest's run() (sim_game_set_event_selftest.cpp). Each case that makes a call
// seeds its own returns first, so persisting them across cases is safe.
void reset_calls() {
    g_rand_below_args.clear();
    g_rand_below_pos = 0;
    g_vi_calls.clear();
    g_vss_calls.clear();
    g_print_calls.clear();
    g_spawn_calls.clear();
    g_revoke_calls.clear();
    g_presence_calls.clear();
    g_alert_calls.clear();
}

int32_t stub_rand_below(int32_t upper_bound) {
    g_rand_below_args.push_back(upper_bound);
    int32_t r = (g_rand_below_pos < g_rand_below_returns.size()) ? g_rand_below_returns[g_rand_below_pos] : 0;
    ++g_rand_below_pos;
    return r;
}
int32_t stub_w_sprintf_vi(void *dst, const wchar_t *format, int32_t a0) {
    g_vi_calls.push_back({dst, format, a0});
    return 0;
}
int32_t stub_w_sprintf_vss(void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1) {
    g_vss_calls.push_back({dst, format, a0, a1});
    return 0;
}
uint32_t stub_print_text_message(void *text) {
    g_print_calls.push_back(text);
    return 0;
}
int32_t stub_spawn_enemy_landing() {
    g_spawn_calls.push_back(1);
    return g_spawn_return;
}
void stub_revoke_invention(uint16_t player, uint16_t progress_id) {
    g_revoke_calls.push_back({player, progress_id});
}
uint32_t stub_player_presence_lost(uint32_t player, uint32_t mode) {
    g_presence_calls.push_back({player, mode});
    return 0;
}
void stub_invasion_alert_arm(int32_t planet, double timestamp) {
    g_alert_calls.push_back({planet, timestamp});
}

const invasion_calls g_calls = {
    stub_rand_below,
    stub_w_sprintf_vi,
    stub_w_sprintf_vss,
    stub_print_text_message,
    stub_spawn_enemy_landing,
    stub_revoke_invention,
    stub_player_presence_lost,
    stub_invasion_alert_arm,
};

// `System[1].planets[1]` byte-offset derivation -- see sim_invasion.h's "SYSTEM ENTRY PLANET" section.
// Reproduced here (not included from the .cpp, which keeps its copy anonymous-namespace-local) so the
// test can seed the SAME address chance_roll's home_planet_index() reads.
constexpr int32_t SYSTEM_STRIDE_INTS     = 0x8c / 4; // 35
constexpr int32_t SYSTEM_HOME_ROW        = 1;
constexpr int32_t SYSTEM_ICON_INT_OFFSET = 0x10 / 4; // 4
void              set_home_planet(sim_fixture &fx, int32_t home) {
    fx.system_define_index_base[SYSTEM_HOME_ROW * SYSTEM_STRIDE_INTS + SYSTEM_ICON_INT_OFFSET] = home;
}

int32_t run(sim_fixture &fx, int32_t building_completed) {
    reset_calls();
    sim_store own = fx.store();
    return detail::invasion_chance_roll(fx.view(), own, g_calls, building_completed);
}

} // namespace

void run_invasion_chance_roll_tests() {
    sim_fixture fx;

    // ==== the early-out gate: planet_index==0x1f && current_system==0, BOTH sides ===================
    // C1a: both true -> early out, unconditionally, before any call is made (not even rand_below).
    // Mutation note: dropping the "&&" for "||" here would early-out on EITHER condition; C1b/C1c below
    // (one true, one false) catch that.
    fx.reset();
    fx.planet_index   = 0x1f;
    fx.current_system = 0;
    set_home_planet(fx, 5);     // irrelevant on this path, just != 0x1f so it wouldn't matter either way
    g_rand_below_returns = {5}; // would trigger an invasion if the gate were skipped
    ck_eq((uint32_t)run(fx, 1), 0u, "C1a: planet==0x1f && system==0 -> return 0");
    ck((uint32_t)g_rand_below_args.size() == 0, "C1a: no rand_below call at all (true early-out)");

    // C1b: planet==0x1f but system!=0 -> NOT an early-out; falls through to the roll.
    fx.reset();
    fx.planet_index   = 0x1f;
    fx.current_system = 1;
    set_home_planet(fx, 0);      // != 0x1f, so the home-planet gate doesn't also block this
    g_rand_below_returns = {30}; // >= 0x1e -> the ROLL gate (not the early-out) is what blocks this one
    ck_eq((uint32_t)run(fx, 1), 0u, "C1b: planet==0x1f, system!=0 -> not early-out, blocked by roll");
    ck((uint32_t)g_rand_below_args.size() == 1, "C1b: rand_below WAS called (proves the early-out was skipped)");

    // C1c: system==0 but planet!=0x1f -> NOT an early-out either.
    fx.reset();
    fx.planet_index   = 5;
    fx.current_system = 0;
    set_home_planet(fx, 99); // != 5, so the home-planet gate doesn't also block this
    g_rand_below_returns = {30};
    ck_eq((uint32_t)run(fx, 1), 0u, "C1c: system==0, planet!=0x1f -> not early-out, blocked by roll");
    ck((uint32_t)g_rand_below_args.size() == 1, "C1c: rand_below WAS called");

    // ==== building_completed == 1: the immediate-roll arm ===========================================
    // C2a: planet == home_planet_index -> skipped REGARDLESS of the roll (roll queued low/favorable).
    // Mutation note: catches a translation that drops the "planet != home" guard entirely.
    fx.reset();
    fx.planet_index   = 12;
    fx.current_system = 3;
    set_home_planet(fx, 12);    // home == current planet
    g_rand_below_returns = {5}; // would pass roll<0x1e if reached
    ck_eq((uint32_t)run(fx, 1), 0u, "C2a: planet==home -> return 0 even with a favorable roll");
    ck((uint32_t)g_rand_below_args.size() == 1, "C2a: rand_below(100) was still called once (the roll itself runs)");
    ck((uint32_t)g_spawn_calls.size() == 0, "C2a: handle_invasion never reached (no spawn_enemy_landing call)");

    // C2b: planet != home, roll == 0x1e (30) exactly -> the gate is roll < 0x1e, so 30 FAILS it.
    fx.reset();
    fx.planet_index   = 12;
    fx.current_system = 3;
    set_home_planet(fx, 99);
    g_rand_below_returns = {30};
    ck_eq((uint32_t)run(fx, 1), 0u, "C2b: roll==0x1e (boundary) fails the <0x1e gate -> return 0");
    ck((uint32_t)g_rand_below_args[0] == 100, "C2b: rand_below called with upper_bound=100");
    ck((uint32_t)g_spawn_calls.size() == 0, "C2b: handle_invasion not reached");

    // C2c: planet != home, roll == 0x1d (29, one below the boundary) -> PASSES, spawn succeeds ->
    // return 1, and the whole own-planet success chain fires (print_text_message + revoke_invention +
    // planet_status write), with NO player_presence_lost call.
    fx.reset();
    fx.planet_index                    = 12;
    fx.current_system                  = 3;
    fx.player_side                     = 6;
    fx.cfg_planets[12].invention_index = 44;
    set_home_planet(fx, 99);
    static const wchar_t *TXT_BEGUN      = L"invasion begun (sentinel)";
    fx.text_ptrs[TEXT_ID_INVASION_BEGUN] = TXT_BEGUN;
    g_rand_below_returns                 = {29};
    g_spawn_return                       = 7; // nonzero -> success arm
    ck_eq((uint32_t)run(fx, 1), 1u, "C2c: roll==0x1d (boundary-1) passes -> chance_roll returns 1");
    ck((uint32_t)g_spawn_calls.size() == 1, "C2c: spawn_enemy_landing called exactly once");
    ck(g_print_calls.size() == 1 && g_print_calls[0] == (void *)TXT_BEGUN,
       "C2c: print_text_message(text_ptrs[INVASION_BEGUN])");
    ck(g_revoke_calls.size() == 1 && g_revoke_calls[0].player == 6 && g_revoke_calls[0].progress_id == 44,
       "C2c: revoke_invention(player_side=6, invention_index=44)");
    ck_eq((uint32_t)fx.planet_status[12], (uint32_t)PLANET_STATUS_INVASION, "C2c: planet_status[12] = INVASION");
    ck((uint32_t)g_presence_calls.size() == 0, "C2c: no player_presence_lost on the success arm");

    // C2d: same gate pass, but spawn FAILS (0) -> player_presence_lost(local_player_slot, 0) instead;
    // planet_status is NOT written; chance_roll still returns 1 (handle_invasion's own-planet arm
    // always returns 1, success or not -- see sim_handle_invasion_selftest.cpp for that in isolation).
    fx.reset();
    fx.planet_index      = 12;
    fx.current_system    = 3;
    fx.local_player_slot = 3;
    set_home_planet(fx, 99);
    g_rand_below_returns = {10};
    g_spawn_return       = 0; // failure arm
    ck_eq((uint32_t)run(fx, 1), 1u, "C2d: spawn fails but handle_invasion still reports applied -> 1");
    ck(g_presence_calls.size() == 1 && g_presence_calls[0].player == 3 && g_presence_calls[0].mode == 0,
       "C2d: player_presence_lost(local_player_slot=3, mode=0)");
    ck((uint32_t)g_print_calls.size() == 0, "C2d: no print_text_message on the failure arm");
    ck_eq((uint32_t)fx.planet_status[12], 0u, "C2d: planet_status[12] left untouched (still 0)");

    // ==== building_completed == 0: the 32-planet scan arm ============================================
    // C3a: no planet's system_index matches current_system (all default to 0) -> the loop never enters
    // its body for ANY iteration; zero rand_below and zero w_sprintf__vi calls.
    fx.reset();
    fx.current_system = 99; // no cfg_planets[i].system_index (all 0 by reset()) equals this
    set_home_planet(fx, 5);
    ck_eq((uint32_t)run(fx, 0), 0u, "C3a: no system match anywhere -> return 0");
    ck((uint32_t)g_rand_below_args.size() == 0, "C3a: zero rand_below calls");
    ck((uint32_t)g_vi_calls.size() == 0, "C3a: zero w_sprintf__vi calls (body never entered)");

    // C3b: exactly one planet matches system_index, but it IS the home planet -> skipped via the
    // second continue, BEFORE the (always-called) w_sprintf__vi.
    fx.reset();
    fx.current_system              = 7;
    fx.cfg_planets[3].system_index = 7;
    set_home_planet(fx, 3);
    ck_eq((uint32_t)run(fx, 0), 0u, "C3b: sole system-matching planet is home -> return 0");
    ck((uint32_t)g_vi_calls.size() == 0, "C3b: w_sprintf__vi NOT called for the home planet");

    // C3c: one planet matches system, is not home, status != CONQUERED and != current planet_index ->
    // w_sprintf__vi fires (unconditional side effect once the two continues pass) but the status gate
    // then blocks the roll entirely (no rand_below call at all).
    fx.reset();
    fx.current_system              = 7;
    fx.planet_index                = 20; // != 5, so "i == planet_index" does not rescue the gate
    fx.cfg_planets[5].system_index = 7;
    fx.planet_status[5]            = 0; // != PLANET_STATUS_CONQUERED
    set_home_planet(fx, 99);
    ck_eq((uint32_t)run(fx, 0), 0u, "C3c: status gate fails -> return 0");
    ck(g_vi_calls.size() == 1 && g_vi_calls[0].dst == (void *)fx.text_scratch.data() && g_vi_calls[0].a0 == 5,
       "C3c: w_sprintf__vi(text_scratch(), fmt, i=5) fired exactly once (always-called side effect)");
    ck((uint32_t)g_rand_below_args.size() == 0, "C3c: status gate blocked the roll -> no rand_below call");

    // C3d: status == CONQUERED (distant planet, i != planet_index, no *3 multiplier), and the
    // current_system*2 SUBTRACTION is exercised at its exact pass boundary: rand_below(100)->20,
    // current_system=3 -> roll = 20 - 3*2 = 14, which is < 0xf(15) -> triggers handle_invasion, which
    // (with progress.acquired seeded) succeeds as the DISTANT-planet arm: planet_invasion_time_at is
    // stamped and invasion_alert_arm fires. Mutation note: dropping the "- current_system*2" term
    // would leave roll=20 (>=15), failing this case's own boundary -- a strong discriminator.
    fx.reset();
    fx.current_system                                = 3;
    fx.planet_index                                  = 20;
    fx.game_clock                                    = 500.0;
    fx.cfg_planets[8].system_index                   = 3;
    fx.cfg_planets[8].invention_index                = 2;
    fx.planet_status[8]                              = PLANET_STATUS_CONQUERED;
    fx.player_side                                   = 4;
    fx.progress[4 * PROGRESS_ROW_COUNT + 2].acquired = 1; // eligibility for handle_invasion's distant arm
    set_home_planet(fx, 99);
    g_rand_below_returns = {20, 1}; // [0]=the 100-roll -> 14 after subtraction; [1]=the r for since_time
    ck_eq((uint32_t)run(fx, 0), 1u, "C3d: 20 - 3*2 = 14 < 15 -> handle_invasion fires -> return 1");
    ck((uint32_t)g_rand_below_args.size() == 2, "C3d: two rand_below calls (the 100-roll, then the 2-roll for since_time)");
    ck_eq((uint32_t)g_rand_below_args[1], 2u, "C3d: second rand_below call is rand_below(2)");
    // since_time = invasion_roll_base_time(300.0) - (r+1)*60 = 300 - (1+1)*60 = 180.0;
    // time_delta = game_clock(500) - since_time(180) = 320; clamped = max(320, planet_time[8]=0) = 320.
    ck_eq_d(fx.planet_invasion_time[8], 320.0, "C3d: planet_invasion_time[8] = clamp(time_delta, planet_time[8])");
    ck(g_alert_calls.size() == 1 && g_alert_calls[0].planet == 8 && g_alert_calls[0].timestamp == 500.0,
       "C3d: invasion_alert_arm(planet=8, timestamp=*game_clock=500.0)");

    // C3h: same shape as C3d but at the FAIL boundary: rand_below(100)->21 -> roll = 21-6 = 15, which
    // is NOT < 0xf -> no second rand_below call, no handle_invasion.
    fx.reset();
    fx.current_system              = 3;
    fx.planet_index                = 20;
    fx.cfg_planets[8].system_index = 3;
    fx.planet_status[8]            = PLANET_STATUS_CONQUERED;
    set_home_planet(fx, 99);
    g_rand_below_returns = {21};
    ck_eq((uint32_t)run(fx, 0), 0u, "C3h: 21 - 3*2 = 15, boundary FAIL (not <15) -> return 0");
    ck((uint32_t)g_rand_below_args.size() == 1, "C3h: only the 100-roll fires, no since_time roll");

    // C3e: the *3 MULTIPLIER, discriminating case -- i == planet_index (rescues the status gate even
    // though status != CONQUERED), base roll 6 would PASS unmultiplied (6<15) but FAILS multiplied
    // (18>=15). current_system=0 so the subtraction term is inert here (isolates the multiplier).
    // Mutation note: a translation that forgets "roll *= 3" would see 6<15 and WRONGLY trigger.
    fx.reset();
    fx.current_system              = 0;
    fx.planet_index                = 9;
    fx.cfg_planets[9].system_index = 0;
    fx.planet_status[9]            = 0; // != CONQUERED; rescued only by i==planet_index
    set_home_planet(fx, 99);
    g_rand_below_returns = {6};
    ck_eq((uint32_t)run(fx, 0), 0u, "C3e: 6*3=18 >= 15 -> multiplier correctly blocks the invasion");
    ck((uint32_t)g_rand_below_args.size() == 1, "C3e: no second rand_below call (gate failed)");
    ck((uint32_t)g_spawn_calls.size() == 0, "C3e: handle_invasion not reached");

    // C3f: multiplier applied AND still passes -- rand_below(100)->2, *3 = 6 < 15 -> the else-branch
    // ALWAYS computes since_time via a second rand_below(2) call before invoking handle_invasion(i,
    // since_time), even here where i==planet_index (since_time only ends up UNUSED once inside
    // handle_invasion's own-planet arm, which doesn't reference it -- but chance_roll itself always
    // spends the call). handle_invasion then runs the own-planet arm (unconditionally eligible since
    // i==planet_index) -> spawn succeeds -> return 1.
    fx.reset();
    fx.current_system              = 0;
    fx.planet_index                = 9;
    fx.cfg_planets[9].system_index = 0;
    fx.planet_status[9]            = 0;
    set_home_planet(fx, 99);
    g_rand_below_returns = {2, 3}; // [0]=100-roll -> 2*3=6<15; [1]=the r for since_time (unused downstream)
    g_spawn_return       = 5;
    ck_eq((uint32_t)run(fx, 0), 1u, "C3f: 2*3=6 < 15 -> multiplier correctly ALLOWS the invasion -> 1");
    ck((uint32_t)g_rand_below_args.size() == 2, "C3f: since_time's rand_below(2) still fires even though the own-planet arm won't use it");
    ck_eq((uint32_t)g_rand_below_args[1], 2u, "C3f: second rand_below call is rand_below(2)");
    ck((uint32_t)g_spawn_calls.size() == 1, "C3f: own-planet arm reached (spawn_enemy_landing called)");

    // ==== loop early-exit: the FIRST successful planet stops the scan, the second is never visited ===
    fx.reset();
    fx.current_system                                = 7;
    fx.planet_index                                  = 20;
    fx.cfg_planets[2].system_index                   = 7;
    fx.cfg_planets[2].invention_index                = 0;
    fx.planet_status[2]                              = PLANET_STATUS_CONQUERED;
    fx.cfg_planets[6].system_index                   = 7; // a SECOND planet that would ALSO trigger, if reached
    fx.planet_status[6]                              = PLANET_STATUS_CONQUERED;
    fx.player_side                                   = 1;
    fx.progress[1 * PROGRESS_ROW_COUNT + 0].acquired = 1; // eligibility for planet 2's invention row 0
    set_home_planet(fx, 99);
    g_rand_below_returns = {5, 0}; // planet 2: roll = 5 - current_system(7)*2 = -9, well under 15 -> triggers; r=0 for since_time
    ck_eq((uint32_t)run(fx, 0), 1u, "loop-exit: planet 2 (first match) triggers -> return 1");
    ck((uint32_t)g_vi_calls.size() == 1, "loop-exit: w_sprintf__vi fired ONCE (only planet 2 was visited)");
    ck_eq((uint32_t)g_vi_calls[0].a0, 2u, "loop-exit: the one w_sprintf__vi call was for planet 2, not 6");
    ck((uint32_t)g_rand_below_args.size() == 2, "loop-exit: exactly 2 rand_below calls (planet 6 never reached)");
}

} // namespace mh::sim::test
