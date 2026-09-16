//
// sim_invasion_alert_arm_selftest.cpp -- `simtest` offline oracle for llm_strat_invasion_alert_arm
// (sim/sim_invasion_alert_arm.h/.cpp, RI-SIM / SIM1F). See tmp/decomp/llm_strat_invasion_alert_arm_
// 0049b49e.asm: the whole body is one unconditional 8-byte store,
// `own.invasion_alert_time_at(planet) = timestamp` -- no branches, no callees (the leading CALL is the
// inert Watcom stack probe). Pure state, no `_calls` struct exists for this function (the header
// declares none), so this is a plain read-back-after-write oracle, not a call-recording one.
//
#include "sim/sim_invasion_alert_arm.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

void run(sim_fixture &fx, int32_t planet, double timestamp) {
    sim_store own = fx.store();
    detail::invasion_alert_arm(own, planet, timestamp);
}

} // namespace

void run_invasion_alert_arm_tests() {
    sim_fixture fx;

    // ---- ARM: a plain planet writes exactly its own slot, distinct from its neighbours -------------
    // Mutation note: catches an off-by-one in the planet index (writing planet+1/planet-1 instead), a
    // dropped write entirely (fixture would stay 0.0), or a swapped (planet, timestamp) argument order
    // (would fail ck_eq_d below since timestamp=250.5 is not a valid array index-adjacent value the
    // fixture would coincidentally match).
    fx.reset();
    fx.invasion_alert_time[3] = 999.0;  // neighbour-adjacent sentinel, must survive untouched
    fx.invasion_alert_time[5] = -111.0; // another neighbour, must also survive untouched
    run(fx, 4, 250.5);
    ck_eq_d(fx.invasion_alert_time[4], 250.5, "ARM: invasion_alert_time[4] = timestamp (250.5)");
    ck_eq_d(fx.invasion_alert_time[3], 999.0, "ARM: neighbour slot [3] untouched");
    ck_eq_d(fx.invasion_alert_time[5], -111.0, "ARM: neighbour slot [5] untouched");

    // ---- ARM at a different, non-adjacent planet index + a DISTINCT timestamp value, to catch a
    // translation that hardcodes either operand instead of threading the parameters through.
    fx.reset();
    run(fx, 17, 42.75);
    ck_eq_d(fx.invasion_alert_time[17], 42.75, "ARM: a second (planet, timestamp) pair, both distinct from the first case");
    ck_eq_d(fx.invasion_alert_time[4], 0.0, "ARM: the FIRST case's slot (4) is back to 0.0 after reset() -- confirms this run() didn't reuse stale state");

    // ---- RE-ARM: writing the SAME planet twice overwrites (no accumulation / no "first write wins"
    // latch) -- the store is unconditional every call, matching the disassembly having no guard.
    fx.reset();
    run(fx, 9, 10.0);
    run(fx, 9, 20.0);
    ck_eq_d(fx.invasion_alert_time[9], 20.0, "RE-ARM: second call overwrites the first (20.0, not 10.0 and not 30.0)");

    // ---- boundary: planet 0 and planet 31 (the real extent's two ends) both address correctly, not
    // just the "comfortable middle" indices used above.
    fx.reset();
    run(fx, 0, 1.5);
    run(fx, 31, 2.5);
    ck_eq_d(fx.invasion_alert_time[0], 1.5, "boundary: planet 0");
    ck_eq_d(fx.invasion_alert_time[31], 2.5, "boundary: planet 31");
}

} // namespace mh::sim::test
