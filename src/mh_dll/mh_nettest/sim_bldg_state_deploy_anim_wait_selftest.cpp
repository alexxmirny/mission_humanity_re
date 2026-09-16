//
// sim_bldg_state_deploy_anim_wait_selftest.cpp -- `simtest` cases for
// llm_strat_bldg_state_deploy_anim_wait (sim/sim_bldg_state_deploy.h/.cpp, detail::
// bldg_state_deploy_anim_wait). SIM1B building_tick machinery family, sibling of
// sim_bldg_state_destroyed_selftest.cpp -- same shape, narrower function.
//
// This function was armed on the rig in a prior slice but got ZERO calls there (the scenario never
// produced online_state==0xc), so this offline oracle is currently the ONLY evidence for its
// behaviour -- every branch below is pinned against the disassembly directly.
//
// EXPECTED BEHAVIOUR, HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_deploy_anim_wait_00471239.asm), cross-checked against the
// .cpp's own address citations -- NOT read off the .cpp body alone:
//
//   0x00471251/0x0047125d: MOV EAX,[_G_LLM_STRAT_CUR_BUILDING]           -- fetch cur_building twice
//   0x00471256:            CMP word ptr [EAX+0x17],0xc                  -- online_state == 0xc ?
//   0x0047125b:            JNZ 0x0047126a                               -- branch away if NOT equal
//   0x00471262:            MOV word ptr [EAX+0xd],0x7c                  -- state = TO_UNIT (0x7c)
//   0x00471268:            JMP 0x0047127e                               -- skip the else arm entirely
//   LAB_0047126a (0x0047126a-0x0047127e): MOV [_G_LLM_STRAT_TICK_BUDGET],0x0 (both dwords of the
//                          double) -- tick_budget = 0.0. This arm never touches cur_building->state.
//
// So: online_state==0xc writes ONLY state (TO_UNIT); tick_budget is left completely alone -- a naive
// "always zero tick_budget" bug would slip past a test that seeds tick_budget at its natural zero
// default, so this file seeds it to a nonzero sentinel and asserts it SURVIVES on that arm.
// online_state!=0xc writes ONLY tick_budget (to exactly 0.0); state is left completely alone -- this
// file seeds state to a sentinel and asserts it is unchanged, and exercises the not-equal arm at three
// distinct values (0, 0xb, 0xd) straddling 0xc on both sides to prove the test is an EQUALITY compare,
// not a range check.
//
#include "sim/sim_bldg_state_deploy.h"

#include <cstdint>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// online_state gate value this function tests against -- see sim_bldg_state_deploy.cpp's own
// DEPLOY_ANIM_WAIT_ONLINE_STATE_ARRIVED (0xc). Not reused from there directly: this test file pins
// the literal independently, straight from the disassembly's `CMP word ptr [EAX+0x17],0xc`.
constexpr int16_t ONLINE_STATE_GATE = 0xc;

// state sentinel -- distinct from both BLDG_STATE_TO_UNIT (0x7c) and any plausible real state value,
// so a translation that accidentally writes state on the else-arm (or writes the wrong constant on
// the if-arm) is caught rather than coincidentally matching.
constexpr uint16_t STATE_SENTINEL = 0xBEEF;

// tick_budget sentinel -- nonzero and distinct from 0.0 (the value this function's else-arm writes),
// so a translation that unconditionally zeroes tick_budget (the "always zero" bug the header calls
// out) is caught on the if-arm case below instead of passing by coincidence.
constexpr double TICK_BUDGET_SENTINEL = 12.5;

struct Seed {
    int16_t  online_state     = 0;
    uint16_t state_seed       = STATE_SENTINEL;
    double   tick_budget_seed = TICK_BUDGET_SENTINEL;
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    building &b    = fx.b(0, 0);
    b.online_state = s.online_state;
    b.state        = s.state_seed;

    fx.cur_building_ptr = &b;
    fx.tick_budget      = s.tick_budget_seed;

    sim_store own = fx.store();
    detail::bldg_state_deploy_anim_wait(own);
}

} // namespace

void run_bldg_state_deploy_anim_wait_tests() {
    sim_fixture fx;

    // =================================================================================================
    // W1 -- online_state == 0xc (the gate value, 0x00471256): state becomes TO_UNIT (0x7c,
    // 0x00471262); tick_budget is UNTOUCHED -- must still read back the seeded nonzero sentinel, which
    // is exactly the case a naive "always zero tick_budget" bug would fail.
    // =================================================================================================
    {
        Seed s;
        s.online_state     = ONLINE_STATE_GATE;
        s.state_seed       = STATE_SENTINEL;
        s.tick_budget_seed = TICK_BUDGET_SENTINEL;
        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.b(0, 0).state, (uint32_t)BLDG_STATE_TO_UNIT,
              "W1 @0x00471262: online_state==0xc -- cur_building->state = TO_UNIT(0x7c)");
        ck_eq_d(fx.tick_budget, TICK_BUDGET_SENTINEL,
                "W1 @0x0047125b/0x00471268: online_state==0xc takes the JNZ-not-taken/JMP path -- "
                "tick_budget keeps its seeded nonzero value, never reaches LAB_0047126a's zero-store");
    }

    // =================================================================================================
    // W2/W3/W4 -- online_state != 0xc, at three distinct values straddling the gate on both sides (0,
    // 0xb just below, 0xd just above) to prove @0x00471256/0x0047125b is an EQUALITY test against 0xc,
    // not a range check. Each: tick_budget becomes exactly 0.0 (LAB_0047126a, both dwords stored);
    // state is left completely UNCHANGED from its seeded sentinel (this arm never writes [EAX+0xd]).
    // =================================================================================================
    {
        Seed s;
        s.online_state     = 0;
        s.state_seed       = STATE_SENTINEL;
        s.tick_budget_seed = TICK_BUDGET_SENTINEL;
        seed_and_run(fx, s);

        ck_eq_d(fx.tick_budget, 0.0,
                "W2 @LAB_0047126a: online_state==0 (!=0xc) -- tick_budget = 0.0");
        ck_eq((uint32_t)fx.b(0, 0).state, (uint32_t)STATE_SENTINEL,
              "W2: online_state==0 -- cur_building->state left UNCHANGED (this arm never writes it)");
    }
    {
        Seed s;
        s.online_state     = 0xb; // one below the gate
        s.state_seed       = STATE_SENTINEL;
        s.tick_budget_seed = TICK_BUDGET_SENTINEL;
        seed_and_run(fx, s);

        ck_eq_d(fx.tick_budget, 0.0,
                "W3 @LAB_0047126a: online_state==0xb (one below the gate) -- tick_budget = 0.0, proving "
                "the gate is an EQUALITY test (a '<' range check would also take this arm, but so would "
                "a '<=' one -- W4 below is what actually separates them)");
        ck_eq((uint32_t)fx.b(0, 0).state, (uint32_t)STATE_SENTINEL,
              "W3: online_state==0xb -- cur_building->state left UNCHANGED");
    }
    {
        Seed s;
        s.online_state     = 0xd; // one above the gate
        s.state_seed       = STATE_SENTINEL;
        s.tick_budget_seed = TICK_BUDGET_SENTINEL;
        seed_and_run(fx, s);

        ck_eq_d(fx.tick_budget, 0.0,
                "W4 @LAB_0047126a: online_state==0xd (one above the gate) -- tick_budget = 0.0; combined "
                "with W1 (==0xc takes the OTHER arm), this pins the test as == not <= or >=");
        ck_eq((uint32_t)fx.b(0, 0).state, (uint32_t)STATE_SENTINEL,
              "W4: online_state==0xd -- cur_building->state left UNCHANGED");
    }
}

} // namespace mh::sim::test
