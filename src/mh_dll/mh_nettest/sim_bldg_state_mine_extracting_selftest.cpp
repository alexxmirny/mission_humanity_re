//
// sim_bldg_state_mine_extracting_selftest.cpp -- offline `simtest` oracle for
// llm_strat_bldg_state_mine_extracting @0x0047457a (sim/sim_bldg_state_mine.h/.cpp, SIM1-G4 third
// slice).
//
// EXPECTED BEHAVIOUR, from the disassembly (tmp/decomp_sim/llm_strat_bldg_state_mine_extracting_
// 0047457a.asm) and the header's derivation (sim_bldg_state_mine.h):
//   0x00474597-0x004745a9: cycle_progress = tick_budget*efficiency + cycle_progress -- the
//     accumulation happens BEFORE the gate check below, unconditionally.
//   0x004745b4-0x004745bd: FCOMP/FNSTSW/SAHF/JC gate on cycle_progress vs MINE_EXTRACT_PERIOD. The
//     SKIP branch (JC taken) is cycle_progress < period OR either operand NaN; this file restates it
//     as `!(cycle_progress >= period)` (NaN-safe -- see the header's FP-COMPARISON NOTE). Skip path
//     (0x00474630-0x00474644): tick_budget=0.0, RETURN -- no completion_dispatch call, no state
//     change, cycle_progress left at the just-accumulated value (not reset).
//   Do-work path (0x004745bf-0x004745d9): completion_dispatch(cur_player, cur_index, param_3,
//     param_4, *game_clock) -- param_3/param_4 are THIS function's own incoming param_3/param_4,
//     forwarded VERBATIM (the header's REGISTER FORWARDING hazard); param_1/param_2 are never read
//     anywhere in the body (genuinely dead).
//   0x004745de-0x004745e3: state = MINE_CHECK_DEPOSITS (0x73), unconditional on the do-work path.
//   0x004745ee-0x00474609: efficiency==0.0 (bit-exact test, true for BOTH +0.0 and -0.0, false for
//     NaN) -> efficiency = 1.0. efficiency!=0.0 -> left untouched.
//   0x00474610-0x00474628: tick_budget = (cycle_progress + MINE_EXTRACT_PERIOD_NEG) / efficiency
//     (the POST-self-heal efficiency). cycle_progress itself is NOT written back here -- it stays at
//     the value the initial accumulation produced.
//
#include "sim/sim_bldg_state_mine.h"

#include <cstdint>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct completion_dispatch_call {
    uint32_t param_1;
    uint32_t param_2;
    uint32_t param_3;
    uint32_t param_4;
    double   param_5;
};
std::vector<completion_dispatch_call> g_dispatch_calls;

void rec_bldg_completion_dispatch(uint32_t param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4,
                                  double param_5) {
    g_dispatch_calls.push_back({param_1, param_2, param_3, param_4, param_5});
}

const bldg_state_mine_extracting_calls g_calls = {
    &rec_bldg_completion_dispatch,
};

void reset_recorders() { g_dispatch_calls.clear(); }

} // namespace

void run_bldg_state_mine_extracting_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- SKIP path: accumulation lands just BELOW the period. tick_budget zeroed, dispatch NEVER
    // called, state/efficiency untouched, cycle_progress left at the accumulated (not reset) value.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        building &b      = fx.b(0, 0); // == own.cur_building() -- cur_building_ptr defaults to buildings[0]
        b.efficiency     = 2.0;
        b.cycle_progress = 3.0;
        b.state          = 0x11; // sentinel, distinct from MINE_CHECK_DEPOSITS(0x73) -- must stay this
        fx.tick_budget   = 1.0;  // accumulated = 1.0*2.0 + 3.0 = 5.0 < MINE_EXTRACT_PERIOD(10.0)

        sim_store own = fx.store();
        detail::bldg_state_mine_extracting(fx.view(), own, g_calls, 0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC,
                                           0xDDDDDDDD);

        ck(g_dispatch_calls.empty(), "T1: skip path -- completion_dispatch NEVER called, 0x004745bd JC taken");
        ck_eq_d(fx.tick_budget, 0.0, "T1: tick_budget zeroed on skip, 0x00474630-0x0047463a");
        ck_eq_d(b.cycle_progress, 5.0,
                "T1: cycle_progress left at the ACCUMULATED value (1.0*2.0+3.0), not reset, 0x004745a9");
        ck_eq((uint32_t)b.state, 0x11u, "T1: state untouched on skip -- no MINE_CHECK_DEPOSITS write");
        ck_eq_d(b.efficiency, 2.0, "T1: efficiency untouched on skip -- self-heal guard is do-work-only");
    }

    // =================================================================================================
    // T2 -- BOUNDARY: accumulation lands EXACTLY at the period. Per the header's FP-COMPARISON note
    // the do-branch is `cycle_progress >= period`, so period itself must NOT skip -- this pins that
    // the gate is `!(>=)`, not a naive `<` that happens to agree everywhere except this one value.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        building &b        = fx.b(0, 0);
        b.efficiency       = 1.0;
        b.cycle_progress   = 9.0;
        fx.tick_budget     = 1.0; // accumulated = 1.0*1.0 + 9.0 = 10.0 == MINE_EXTRACT_PERIOD, do-work branch
        fx.view_cur_player = 6;
        fx.view_cur_index  = 21;
        fx.game_clock      = 100.0;

        sim_store own = fx.store();
        detail::bldg_state_mine_extracting(fx.view(), own, g_calls, 0, 0, 0x11112222, 0x33334444);

        ck(g_dispatch_calls.size() == 1,
           "T2: boundary cycle_progress==period takes the DO-work branch, 0x004745bd JC not taken");
        const auto &call = g_dispatch_calls[0];
        ck_eq(call.param_1, 6u, "T2: dispatch param_1 == cur_player, 0x004745d2");
        ck_eq(call.param_2, 21u, "T2: dispatch param_2 == cur_index, 0x004745cb");
        ck_eq(call.param_3, 0x11112222u, "T2: dispatch param_3 == this function's OWN param_3, forwarded");
        ck_eq(call.param_4, 0x33334444u, "T2: dispatch param_4 == this function's OWN param_4, forwarded");
        ck_eq_d(call.param_5, 100.0, "T2: dispatch param_5 == *game_clock, 0x004745bf-0x004745c5");
        ck_eq((uint32_t)b.state, 0x73u, "T2: state = MINE_CHECK_DEPOSITS(0x73) after dispatch, 0x004745e3");
        ck_eq_d(b.cycle_progress, 10.0,
                "T2: cycle_progress left at the accumulated value (10.0), NOT written back, 0x00474615-0x0047461e");
        ck_eq_d(fx.tick_budget, 0.0,
                "T2: tick_budget = (10.0 + -10.0)/1.0 = 0.0, 0x00474610-0x00474628");
    }

    // =================================================================================================
    // T3 -- general DO-WORK path, well past the period (accumulation crosses the threshold from below
    // to well above it): full REGISTER FORWARDING pin with distinct nonzero sentinels for
    // param_3/param_4, a nonzero efficiency LEFT UNCHANGED (no self-heal), and the full tick_budget
    // formula.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        building &b        = fx.b(0, 0);
        b.efficiency       = 0.4; // nonzero -- must survive the self-heal guard untouched
        b.cycle_progress   = 20.0;
        fx.tick_budget     = 5.0; // accumulated = 5.0*0.4 + 20.0 = 22.0, well past period(10.0)
        fx.view_cur_player = 5;
        fx.view_cur_index  = 12;
        fx.game_clock      = 777.5;

        sim_store own = fx.store();
        // param_1/param_2 sentinels distinct from cur_player/cur_index (5/12), so a mistaken forward
        // would be caught below; param_3/param_4 the REAL forwarded sentinels.
        detail::bldg_state_mine_extracting(fx.view(), own, g_calls, 0xDEADBEEF, 0x0BADF00D, 0xCAFEBABE,
                                           0xFACE1234);

        ck(g_dispatch_calls.size() == 1, "T3: completion_dispatch called exactly once on the do-work path");
        const auto &call = g_dispatch_calls[0];
        ck_eq(call.param_1, 5u,
              "T3: dispatch param_1 == cur_player (5), NOT this function's own param_1 (0xDEADBEEF) -- "
              "param_1 is dead, 0x004745d2");
        ck_eq(call.param_2, 12u,
              "T3: dispatch param_2 == cur_index (12), NOT this function's own param_2 (0x0BADF00D) -- "
              "param_2 is dead, 0x004745cb");
        ck_eq(call.param_3, 0xCAFEBABEu,
              "T3: dispatch param_3 == this function's OWN param_3, forwarded VERBATIM, 0x0047458c-0x004745d9");
        ck_eq(call.param_4, 0xFACE1234u,
              "T3: dispatch param_4 == this function's OWN param_4, forwarded VERBATIM, 0x0047458c-0x004745d9");
        ck_eq_d(call.param_5, 777.5, "T3: dispatch param_5 == *game_clock, 0x004745bf-0x004745c5");
        ck_eq((uint32_t)b.state, 0x73u, "T3: state = MINE_CHECK_DEPOSITS(0x73), 0x004745e3");
        ck_eq_d(b.efficiency, 0.4,
                "T3: nonzero efficiency LEFT UNCHANGED -- self-heal guard is ==0.0 only, 0x004745ee-0x004745fb");
        ck_eq_d(b.cycle_progress, 22.0,
                "T3: cycle_progress left at the accumulated value (22.0), NOT written back");
        ck_eq_d(fx.tick_budget, (22.0 + -10.0) / 0.4,
                "T3: tick_budget = (cycle_progress + MINE_EXTRACT_PERIOD_NEG) / efficiency, 0x00474610-0x00474628");
    }

    // =================================================================================================
    // T4 -- efficiency self-heal, POSITIVE zero: efficiency == +0.0 -> reset to 1.0. Chosen so the
    // accumulation (tick_budget*efficiency=0) doesn't itself cross the gate -- cycle_progress starts
    // already at/above the period so the do-work branch is reached with efficiency still 0.0 at the
    // self-heal check.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        building &b      = fx.b(0, 0);
        b.efficiency     = 0.0; // +0.0
        b.cycle_progress = 15.0;
        fx.tick_budget   = 3.0; // accumulated = 3.0*0.0 + 15.0 = 15.0 >= period(10.0), do-work branch

        sim_store own = fx.store();
        detail::bldg_state_mine_extracting(fx.view(), own, g_calls, 0, 0, 0x55556666, 0x77778888);

        ck(g_dispatch_calls.size() == 1, "T4: do-work branch reached with efficiency still +0.0 at the check");
        ck_eq_d(b.efficiency, 1.0, "T4: efficiency==+0.0 self-heals to 1.0, 0x004745ee-0x00474609");
        ck_eq_d(b.cycle_progress, 15.0, "T4: cycle_progress left at the accumulated value (15.0)");
        ck_eq_d(fx.tick_budget, (15.0 + -10.0) / 1.0,
                "T4: tick_budget computed with the POST-self-heal efficiency (1.0), 0x00474610-0x00474628");
    }

    // =================================================================================================
    // T5 -- efficiency self-heal, NEGATIVE zero: efficiency == -0.0 also self-heals to 1.0 (the
    // header's "bit-exact for +/-0.0" claim -- a naive `efficiency > 0.0 ? ... : reset` or a sign-only
    // test could get -0.0 wrong).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        building &b      = fx.b(0, 0);
        b.efficiency     = -0.0; // negative zero
        b.cycle_progress = 12.0;
        fx.tick_budget   = 0.0; // accumulated = 0.0*(-0.0) + 12.0 = 12.0 >= period(10.0), do-work branch

        sim_store own = fx.store();
        detail::bldg_state_mine_extracting(fx.view(), own, g_calls, 0, 0, 0x99990000, 0x11110000);

        ck(g_dispatch_calls.size() == 1, "T5: do-work branch reached with efficiency still -0.0 at the check");
        ck_eq_d(b.efficiency, 1.0, "T5: efficiency==-0.0 ALSO self-heals to 1.0, 0x004745ee-0x00474609");
        ck_eq_d(fx.tick_budget, (12.0 + -10.0) / 1.0,
                "T5: tick_budget computed with the POST-self-heal efficiency (1.0)");
    }

    // =================================================================================================
    // T6 -- dead-parameter differential pin: param_1/param_2 are swapped between two otherwise-
    // identical runs (0/0 vs corrupting sentinels 0xDEADBEEF/0x0BADF00D). Every observable result
    // (tick_budget, state, cycle_progress, AND the dispatch call's own param_1/param_2) must be
    // IDENTICAL -- if either were read anywhere in the body (the FP formula, the gate, an index), the
    // corrupting run would diverge.
    // =================================================================================================
    {
        auto run_with = [&](uint32_t param_1, uint32_t param_2) {
            fx.reset();
            reset_recorders();
            building &b        = fx.b(0, 0);
            b.efficiency       = 1.0;
            b.cycle_progress   = 20.0; // already >= period; tick_budget=0 keeps accumulation trivial
            fx.tick_budget     = 0.0;
            fx.view_cur_player = 9;
            fx.view_cur_index  = 3;
            fx.game_clock      = 42.0;

            sim_store own = fx.store();
            detail::bldg_state_mine_extracting(fx.view(), own, g_calls, param_1, param_2, 0xAAAA, 0xBBBB);
        };

        run_with(0, 0);
        ck(g_dispatch_calls.size() == 1, "T6a: dispatch called once with param_1/param_2 == 0");
        const double   budget_a = fx.tick_budget;
        const uint16_t state_a  = fx.b(0, 0).state;
        const double   cycle_a  = fx.b(0, 0).cycle_progress;
        const auto     call_a   = g_dispatch_calls[0];

        run_with(0xDEADBEEF, 0x0BADF00D);
        ck(g_dispatch_calls.size() == 1, "T6b: dispatch called once with param_1/param_2 == corrupting sentinels");
        ck_eq_d(fx.tick_budget, budget_a,
                "T6: param_1/param_2 do not affect tick_budget -- genuinely dead, 0x0047458c-0x004745d9");
        ck_eq((uint32_t)fx.b(0, 0).state, (uint32_t)state_a, "T6: param_1/param_2 do not affect state");
        ck_eq_d(fx.b(0, 0).cycle_progress, cycle_a, "T6: param_1/param_2 do not affect cycle_progress");
        ck_eq(g_dispatch_calls[0].param_1, call_a.param_1,
              "T6: dispatch's own param_1 (cur_player) unaffected by this function's param_1");
        ck_eq(g_dispatch_calls[0].param_2, call_a.param_2,
              "T6: dispatch's own param_2 (cur_index) unaffected by this function's param_2");
    }
}

} // namespace mh::sim::test
