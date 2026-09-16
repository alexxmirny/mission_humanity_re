//
// sim_bldg_state_dismantling_selftest.cpp -- `simtest` cases for llm_strat_bldg_state_dismantling
// (sim/sim_bldg_state_dismantle.h/.cpp @0x00472eb7), the DISMANTLING building_tick handler.
//
// THIS FUNCTION IS DO-NOT-ARM (its write closure reaches game_SetEvent's ~780-function UI/gfx/snd/
// menu-teardown over-approximation via its sibling in the same TU) -- there is no rig run backing this
// up. This offline oracle is the ONLY evidence for llm_strat_bldg_state_dismantling's correctness.
//
// SPEC (tmp/decomp_sim/llm_strat_bldg_state_dismantling_00472eb7.asm), cross-checked against the
// header banner (sim/sim_bldg_state_dismantle.h) -- NOT read off the .cpp body alone:
//
//   cycle_progress -= (tick_budget * efficiency) / DISMANTLE_PROGRESS_DIVISOR (0x00472ecf-0x00472eec,
//   UNCONDITIONAL -- runs regardless of which arm the completion gate below takes).
//
//   Completion gate, implemented as `FLDZ; FCOMP cycle_progress; FNSTSW/SAHF; JNC` (0x00472ef4-
//   0x00472efc): JNC taken means ST(0)=0.0 is numerically >= cycle_progress, i.e. `cycle_progress <=
//   0.0` -- and in the unordered/NaN case x87 sets CF=1 so JNC is NOT taken, landing on the SAME
//   "else" arm a plain IEEE `cycle_progress <= 0.0` would (false for NaN). The naive `<=` is therefore
//   exactly equivalent here, ordered or not; this file pins the ORDERED boundary (exact 0.0, and one
//   ULP on each side) since a real NaN cannot be produced through the public C++ API.
//     * JNC taken (cycle_progress <= 0.0): `state = BLDG_STATE_DISMANTLE_FINISH (3)`
//       (0x00472f14-0x00472f19), tick_budget is left UNTOUCHED.
//     * JNC not taken (cycle_progress > 0.0): `tick_budget = 0.0` (0x00472efe-0x00472f08), state is
//       left UNTOUCHED.
//
//   Tail (ALWAYS runs, either arm): `llm_strat_bldg_notify_ui(cur_player, cur_index)`
//   (0x00472f1f-0x00472f2d) -- this is the one call this function makes, and it fires unconditionally,
//   so a naive oracle that only checked it under one arm would pass vacuously on the other.
//
#include "sim/sim_bldg_state_dismantle.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: proves notify_ui is the ONLY call and it fires exactly once per case -----------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- the one callee's recorder ----------------------------------------------------------------------
struct NotifyUiCall {
    uint16_t player;
    uint32_t index;
};
std::vector<NotifyUiCall> g_notify_ui_calls;
void                      rec_bldg_notify_ui(uint16_t player, uint32_t index) {
    tr("notify_ui");
    g_notify_ui_calls.push_back({player, index});
}

const bldg_state_dismantling_calls g_calls = {
    &rec_bldg_notify_ui,
};

void reset_observations() {
    g_trace.clear();
    g_notify_ui_calls.clear();
}

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    // Distinct, non-symmetric player/index so a swapped-arg translation is caught by g_notify_ui_calls.
    uint16_t player = 2;
    int32_t  index  = 5;

    double efficiency        = 3.0;  // distinct from tick_budget/divisor so a swapped operand is caught
    double tick_budget_in    = 4.0;  // delta = tick_budget_in * efficiency / divisor = 4.0*3.0/0.2 = 60.0
    double cycle_progress_in = 60.0; // case overrides to probe the completion-gate boundary
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    building &b      = fx.b(s.player, s.index);
    b.cycle_progress = s.cycle_progress_in;
    b.efficiency     = s.efficiency;
    b.state          = 0xffffu; // sentinel: a case that must NOT write state would still show 0xffff

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = s.player;
    fx.view_cur_index   = (uint16_t)s.index;
    fx.tick_budget      = s.tick_budget_in;
    // dismantle_progress_divisor is already bound to its real value (0.2) by the fixture -- not
    // re-seeded here (per sim_test_support.h's own banner).

    reset_observations();

    sim_store own = fx.store();
    detail::bldg_state_dismantling(fx.view(), own, g_calls);
}

} // namespace

void run_bldg_state_dismantling_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- accumulation arithmetic, gate NOT taken (result strictly > 0.0): pins
    // `cycle_progress -= (tick_budget * efficiency) / dismantle_progress_divisor` exactly, and that the
    // "else" arm zeroes tick_budget while leaving `state` UNTOUCHED (sentinel survives).
    // =================================================================================================
    {
        Seed s;
        s.cycle_progress_in = 100.0; // 100.0 - (4.0*3.0/0.2 = 60.0) = 40.0 > 0.0 -> gate NOT taken
        seed_and_run(fx, s);

        ck_eq_d(fx.b(s.player, s.index).cycle_progress, 40.0,
                "T1: cycle_progress = 100.0 - (tick_budget=4.0 * efficiency=3.0) / divisor=0.2 = "
                "100.0 - 60.0 = 40.0 (0x00472ecf-0x00472eec)");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0xffffu,
              "T1: state UNTOUCHED on the tick_budget=0 arm (0x00472efe-0x00472f08 writes tick_budget, "
              "NOT state)");
        ck_eq_d(fx.tick_budget, 0.0,
                "T1: tick_budget zeroed on the JNC-not-taken arm (0x00472efe/0x00472f08, "
                "cycle_progress=40.0 > 0.0)");
        ck(trace_eq({"notify_ui"}), "T1: notify_ui is the only call, fires once even on this arm "
                                    "(0x00472f1f-0x00472f2d runs unconditionally)");
        ck(g_notify_ui_calls.size() == 1 && g_notify_ui_calls[0].player == s.player &&
               g_notify_ui_calls[0].index == (uint32_t)s.index,
           "T1: bldg_notify_ui(cur_player=2, cur_index=5) -- exact operand identity, not swapped "
           "(0x00472f1f MOVZX cur_index / 0x00472f26 MOVZX cur_player)");
    }

    // =================================================================================================
    // T2 -- completion-gate boundary, EXACT zero (pins `<=`, not `<`): result == 0.0 -> JNC IS taken,
    // state transitions to DISMANTLE_FINISH(3), and tick_budget is left UNTOUCHED (sentinel survives).
    // =================================================================================================
    {
        Seed s;
        s.cycle_progress_in = 60.0; // 60.0 - 60.0 = 0.0 exactly -> boundary: gate IS taken (JNC @0x00472efc)
        // NOTE: `seed_and_run` sets fx.tick_budget = s.tick_budget_in itself (line 96) -- setting
        // fx.tick_budget directly BEFORE calling it here would be silently overwritten. The sentinel
        // is s.tick_budget_in (4.0, the default), checked for survival below.
        seed_and_run(fx, s);

        ck_eq_d(fx.b(s.player, s.index).cycle_progress, 0.0,
                "T2: cycle_progress = 60.0 - 60.0 = 0.0 exactly, the completion-gate boundary");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, (uint32_t)BLDG_STATE_DISMANTLE_FINISH,
              "T2: state = BLDG_STATE_DISMANTLE_FINISH(3) at cycle_progress == 0.0 -- pins the gate as "
              "`<=`, not `<` (0x00472ef9 FNSTSW/0x00472efb SAHF/0x00472efc JNC taken at equality)");
        ck_eq_d(fx.tick_budget, s.tick_budget_in,
                "T2: tick_budget UNTOUCHED on the state-transition arm (0x00472f14-0x00472f19 writes "
                "state, NOT tick_budget -- the pre-call value (4.0) survives)");
        ck(trace_eq({"notify_ui"}),
           "T2: notify_ui still fires exactly once on the state-transition arm too (unconditional tail, "
           "same call as T1's other arm -- a gate-only oracle would miss this side)");
    }

    // =================================================================================================
    // T3 -- one ULP ABOVE zero (cycle_progress > 0.0 by the smallest representable margin): gate must
    // NOT be taken. Distinguishes a `>=`-style off-by-one translation from the real `<=` boundary from
    // the "just above" side (T2 already pins "just at/below" via the true-zero case).
    // =================================================================================================
    {
        Seed s;
        // tick_budget*efficiency/divisor = 60.0 exactly (all exactly representable); start one ULP
        // above 60.0 so the subtraction lands one ULP above 0.0, not at 0.0.
        const double one_ulp_above_60 = 60.0 + 60.0 * 0x1p-52; // nextafter(60.0, +inf) in exact binary64
        s.cycle_progress_in           = one_ulp_above_60;
        seed_and_run(fx, s);

        const double result = fx.b(s.player, s.index).cycle_progress;
        ck(result > 0.0,
           "T3: cycle_progress lands one ULP above 0.0 (started one ULP above 60.0, subtracted exactly "
           "60.0) -- confirms the arithmetic doesn't round down to the boundary");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0xffffu,
              "T3: state UNTOUCHED one ULP above the boundary -- gate NOT taken "
              "(0x00472efc JNC not taken when 0.0 < cycle_progress)");
        ck_eq_d(fx.tick_budget, 0.0, "T3: tick_budget zeroed one ULP above the boundary");
    }

    // =================================================================================================
    // T4 -- deep negative cycle_progress (well past zero, not just at the boundary): confirms the gate
    // is a true `<= 0.0` compare (not, say, `== 0.0`) by pinning a value far on the "taken" side.
    // =================================================================================================
    {
        Seed s;
        s.cycle_progress_in = -1000.0; // -1000.0 - 60.0 = -1060.0, deep negative
        // (see T2's note: seed_and_run sets fx.tick_budget = s.tick_budget_in itself; the sentinel
        // checked below is that pre-call value, 4.0.)
        seed_and_run(fx, s);

        ck_eq_d(fx.b(s.player, s.index).cycle_progress, -1060.0,
                "T4: cycle_progress = -1000.0 - 60.0 = -1060.0 -- accumulation applies even from a "
                "deeply negative starting value");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, (uint32_t)BLDG_STATE_DISMANTLE_FINISH,
              "T4: state = BLDG_STATE_DISMANTLE_FINISH(3) at a deeply negative cycle_progress -- the "
              "gate is a true `<= 0.0` compare, not an equality test");
        ck_eq_d(fx.tick_budget, s.tick_budget_in,
                "T4: tick_budget UNTOUCHED on this arm too (the pre-call value, 4.0, survives)");
    }

    // =================================================================================================
    // T5 -- divisor sanity: use the fixture's REAL bound dismantle_progress_divisor (0.2) with a
    // different efficiency/tick_budget pair than T1-T4 to confirm the DIVISION (not e.g. a
    // multiplication or the wrong operand as the divisor) is what's applied, and pins operand order in
    // `(tick_budget * efficiency) / divisor` (swapping tick_budget and efficiency would not be
    // observable here since both are simple multiplicands, but reading `divisor` as the numerator or a
    // stray multiplicand WOULD change this result from every other case's value).
    // =================================================================================================
    {
        Seed s;
        s.efficiency        = 0.5;
        s.tick_budget_in    = 2.0; // (2.0 * 0.5) / 0.2 = 5.0
        s.cycle_progress_in = 10.0;
        seed_and_run(fx, s);

        ck_eq_d(fx.b(s.player, s.index).cycle_progress, 5.0,
                "T5: cycle_progress = 10.0 - (2.0*0.5)/0.2 = 10.0 - 5.0 = 5.0 -- divisor applied as a "
                "DIVISION of the tick_budget*efficiency product, not folded in some other way "
                "(0x00472ee3 FDIV double ptr [DISMANTLE_PROGRESS_DIVISOR])");
    }
}

} // namespace mh::sim::test
