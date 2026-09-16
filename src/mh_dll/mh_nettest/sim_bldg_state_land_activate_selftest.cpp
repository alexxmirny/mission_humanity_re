//
// sim_bldg_state_land_activate_selftest.cpp -- `simtest` cases for llm_strat_bldg_state_land_activate
// (sim/sim_bldg_state_deploy.h/.cpp -- detail::bldg_state_land_activate), SIM1B building_tick
// machinery, LIFTOFF-DEPLOY family (sibling of bldg_state_deploy_anim_wait / bldg_state_deploy_start,
// each in their own arm of the same source file but NOT covered here).
//
// SPEC (tmp/decomp_sim/llm_strat_bldg_state_land_activate_00471288.asm), cross-checked against the
// .cpp/.h's own per-line address citations -- NOT read off the .cpp body alone:
//
//   0x004712a0-0x004712aa: if (cur_building->online_state == 0xa) { tick_budget = 0.0; return; } --
//     early return, calling NOTHING and writing NOTHING else.
//   0x004712b0-0x004712ca: else llm_strat_bldg_register_online(cur_player, cur_index, param_3,
//     param_4, GAME_CLOCK) -- a MIXED calling convention: EAX/EDX are freshly loaded with
//     cur_player/cur_index (NOT this function's own param_1/param_2, which are dead -- overwritten
//     before ever being read, per the header banner's HAZARD note), EBX/ECX are this function's OWN
//     param_3/param_4 forwarded verbatim (never written anywhere in the body), and GAME_CLOCK reaches
//     the callee as a genuine double (2 PUSHes, high dword then low dword).
//   0x004712cf-0x004712eb: cur_building->state = (uint16_t)(Building[building_id].state_transition_ids[1]
//     & 0xffff) -- building_id read ONCE here and reused for the dispatch immediately below; the load
//     is a 0x66-prefixed 16-bit MOV (word ptr), so only the LOW 16 bits of the 4-byte slot are read.
//   0x004712ef-0x00471357: dispatch on Building[building_id].type:
//     A_MOTHER(0x6) [decision at 0x00471329, JNZ 0x00471357] / H_MOTHER(0x1a) [decision at
//     0x00471319, JBE 0x0047132f] -- both land on LAB_0047132f ->
//       llm_strat_bldg_flush_cargo_hold(cur_player, cur_index) @0x0047132f-0x0047133d.
//     A_SHUTTLE(0xd) [decision at 0x0047130d, JBE 0x00471344] / H_SHUTTLE(0x21) [decision at
//     0x0047131f, JZ 0x00471344] -- both land on LAB_00471344 ->
//       llm_strat_prod_unload_cargo_manifest(cur_player, cur_index) @0x00471344-0x00471352, return
//       value discarded (no read of EAX after the call).
//     anything else -> no call (falls straight to LAB_00471357 / the shared epilogue).
//   No notify_ui call anywhere in this function (confirmed absent from the .asm listing).
//
#include "sim/sim_bldg_state_deploy.h"

#include <cstdint>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: proves call order / exclusivity across the three callees ---------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

// ---- per-callee recorders (3, one per bldg_state_land_activate_calls member) ---------------------
struct RegisterOnlineCall {
    int16_t  player;
    int32_t  building_index;
    uint32_t param_3;
    uint32_t param_4;
    double   anim_dur;
};
std::vector<RegisterOnlineCall> g_register_online_calls;
void                            rec_register_online(int16_t player, int32_t building_index, uint32_t param_3,
                                                    uint32_t param_4, double anim_dur) {
    tr("register_online");
    g_register_online_calls.push_back({player, building_index, param_3, param_4, anim_dur});
}

struct CargoCall {
    uint32_t player;
    int32_t  building_index;
};
std::vector<CargoCall> g_flush_cargo_hold_calls;
void                   rec_flush_cargo_hold(uint32_t player, int32_t building_index) {
    tr("flush_cargo_hold");
    g_flush_cargo_hold_calls.push_back({player, building_index});
}

std::vector<CargoCall> g_unload_cargo_manifest_calls;
int32_t                g_unload_cargo_manifest_ret = 0;
int32_t                rec_unload_cargo_manifest(uint32_t player, int32_t building_index) {
    tr("unload_cargo_manifest");
    g_unload_cargo_manifest_calls.push_back({player, building_index});
    return g_unload_cargo_manifest_ret;
}

const bldg_state_land_activate_calls g_calls = {
    &rec_register_online,
    &rec_flush_cargo_hold,
    &rec_unload_cargo_manifest,
};

void reset_observations() {
    g_trace.clear();
    g_register_online_calls.clear();
    g_flush_cargo_hold_calls.clear();
    g_unload_cargo_manifest_calls.clear();
}

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player  = 0;
    int32_t  index   = 1;
    uint16_t cfg_row = 10;

    // 0xa is the ONLY early-exit value (0x004712a5 CMP ...,0xa); anything else takes the main body.
    int16_t online_state = 0;

    // Not one of {A_MOTHER,H_MOTHER,A_SHUTTLE,H_SHUTTLE} by default -- isolates the register_online /
    // state-mask cases from the cargo dispatch.
    uint8_t type = 0x55;

    // state_transition_ids[1] with nonzero UPPER 16 bits, so a translation that forgot the 0xffff
    // mask (0x004712e4-0x004712eb) would disagree with this fixture.
    int32_t state_transition_1 = 0x1234ABCD;

    // Distinct sentinel double so a translation that dropped/garbled the GAME_CLOCK marshalling is
    // caught (0x004712b0/0x004712b6's two PUSHes -> the committed `double anim_dur` reconstruction).
    double game_clock = 987654.321;

    // param_1/param_2 (EAX/EDX, DEAD -- overwritten before ever being read) vs. param_3/param_4
    // (EBX/ECX, forwarded verbatim into register_online). All four DISTINCT from each other and from
    // cur_player/cur_index, so a translation that forwarded the wrong pair (or read a dead one)
    // disagrees with this fixture.
    uint32_t param_1 = 0xAAAA1111;
    uint32_t param_2 = 0xBBBB2222;
    uint32_t param_3 = 0x33334444;
    uint32_t param_4 = 0x55556666;

    // Sentinel the function must overwrite to 0.0 on the early-exit path, and must NOT touch on the
    // main-body path (0x00471359-0x00471363 only runs when online_state==0xa).
    double tick_budget = 777.0;

    // Sentinel state value, distinct from state_transition_1's low 16 bits (0xABCD) and from the
    // early-exit path's "leave state alone" expectation.
    uint16_t state_sentinel = 0xBEEF;
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    building &b    = fx.b(s.player, s.index);
    b.building_id  = s.cfg_row;
    b.online_state = s.online_state;
    b.state        = s.state_sentinel;

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = s.player;
    fx.view_cur_index   = (uint16_t)s.index;

    cfg_building &cb           = fx.cfg_buildings[s.cfg_row];
    cb.type                    = s.type;
    cb.state_transition_ids[1] = s.state_transition_1;

    fx.game_clock  = s.game_clock;
    fx.tick_budget = s.tick_budget;

    g_unload_cargo_manifest_ret = 0;

    reset_observations();

    sim_store own = fx.store();
    detail::bldg_state_land_activate(fx.view(), own, s.param_1, s.param_2, s.param_3, s.param_4, g_calls);
}

} // namespace

void run_bldg_state_land_activate_tests() {
    sim_fixture fx;

    // =================================================================================================
    // L1 -- EARLY EXIT (0x004712a0-0x004712aa/0x00471359-0x0047136d): online_state==0xa skips
    // EVERYTHING -- no calls fire at all, tick_budget is zeroed, and building.state is left untouched
    // (the only write on this path is the two-dword tick_budget store; there is no state write until
    // 0x004712cf, which this path never reaches).
    // =================================================================================================
    {
        Seed s;
        s.online_state = 0xa;
        seed_and_run(fx, s);

        ck(g_trace.empty(), "L1 @0x004712a5/JZ 0x00471359: online_state==0xa -- NO calls fire at all");
        ck(g_register_online_calls.empty(), "L1: register_online does not fire on the early-exit path");
        ck(g_flush_cargo_hold_calls.empty(), "L1: flush_cargo_hold does not fire on the early-exit path");
        ck(g_unload_cargo_manifest_calls.empty(),
           "L1: unload_cargo_manifest does not fire on the early-exit path");
        ck_eq_d(fx.tick_budget, 0.0, "L1 @0x00471359-0x00471363: tick_budget zeroed (2-dword MOV store)");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, (uint32_t)s.state_sentinel,
              "L1: building.state is UNTOUCHED on the early-exit path (no write before the return)");
    }

    // =================================================================================================
    // L2 -- register_online's MIXED-convention call (0x004712b0-0x004712ca): cur_player/cur_index
    // reach it FRESHLY loaded (not this function's own param_1/param_2), param_3/param_4 are THIS
    // function's own params forwarded VERBATIM, and GAME_CLOCK reaches it as the real double.
    // =================================================================================================
    {
        Seed s;
        s.player = 2;
        s.index  = 7;
        seed_and_run(fx, s);

        ck(g_register_online_calls.size() == 1, "L2 @0x004712ca: register_online called exactly once");
        if (g_register_online_calls.size() == 1) {
            const auto &c = g_register_online_calls[0];
            ck_eq((uint32_t)c.player, (uint32_t)s.player,
                  "L2 @0x004712c3: register_online's player = cur_player (MOVZX EAX,[CUR_PLAYER]), NOT param_1");
            ck_eq((uint32_t)c.building_index, (uint32_t)s.index,
                  "L2 @0x004712bc: register_online's index = cur_index (MOVZX EDX,[CUR_INDEX]), NOT param_2");
            ck_eq(c.param_3, s.param_3,
                  "L2 @0x004712b0/EBX: register_online's param_3 = this function's OWN param_3, forwarded verbatim");
            ck_eq(c.param_4, s.param_4,
                  "L2 @0x004712b0/ECX: register_online's param_4 = this function's OWN param_4, forwarded verbatim");
            ck(c.param_3 != s.param_1 && c.param_4 != s.param_2,
               "L2: the forwarded pair is param_3/param_4, distinct from the dead param_1/param_2 sentinels");
            ck_eq_d(c.anim_dur, s.game_clock,
                    "L2 @0x004712b0-0x004712b6: anim_dur = *GAME_CLOCK (2-PUSH double marshalling)");
        }
    }

    // =================================================================================================
    // L2b -- param_1/param_2 are DEAD (overwritten before ever being read, per the header's HAZARD
    // note): changing ONLY param_1/param_2 while holding param_3/param_4/cur_player/cur_index/
    // game_clock fixed must reproduce the IDENTICAL register_online call as L2.
    // =================================================================================================
    {
        Seed s;
        s.player  = 2;
        s.index   = 7;
        s.param_1 = 0x99990000; // different dead values from L2's...
        s.param_2 = 0x88880000; // ...s.param_1/s.param_2
        seed_and_run(fx, s);

        ck(g_register_online_calls.size() == 1,
           "L2b: register_online still called exactly once with param_1/param_2 changed");
        if (g_register_online_calls.size() == 1) {
            const auto &c = g_register_online_calls[0];
            ck_eq((uint32_t)c.player, (uint32_t)s.player,
                  "L2b: player unaffected by the param_1/param_2 change");
            ck_eq((uint32_t)c.building_index, (uint32_t)s.index,
                  "L2b: index unaffected by the param_1/param_2 change");
            ck_eq(c.param_3, s.param_3,
                  "L2b @0x004712b0-0x004712ca: param_1/param_2 changing has NO EFFECT on the forwarded param_3");
            ck_eq(c.param_4, s.param_4,
                  "L2b @0x004712b0-0x004712ca: param_1/param_2 changing has NO EFFECT on the forwarded param_4");
        }
    }

    // =================================================================================================
    // L3 -- state write masks to 16 bits (0x004712cf-0x004712eb): state_transition_ids[1] seeded with
    // nonzero UPPER 16 bits; only the LOW word survives into building.state (0x66-prefixed word MOV).
    // =================================================================================================
    {
        Seed s;
        s.state_transition_1 = 0x1234ABCD;
        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0xABCDu,
              "L3 @0x004712e4/MOV AX,word ptr[...]: state = state_transition_ids[1] & 0xffff (LOW word only)");
        ck(fx.b(s.player, s.index).state != (uint16_t)0x1234,
           "L3: the upper 16 bits (0x1234) do NOT survive into building.state as their own value");
    }

    // =================================================================================================
    // L4 -- type dispatch, A_MOTHER (0x6): decision at 0x00471329, JNZ 0x00471357 falls through to
    // LAB_0047132f -> flush_cargo_hold fires; unload_cargo_manifest does NOT.
    // =================================================================================================
    {
        Seed s;
        s.type = BUILDING_TYPE_A_MOTHER;
        seed_and_run(fx, s);

        ck(g_register_online_calls.size() == 1, "L4: register_online still fires (unconditional spine)");
        ck(g_flush_cargo_hold_calls.size() == 1,
           "L4 @0x0047132f-0x0047133d: A_MOTHER(0x6) -> flush_cargo_hold fires exactly once");
        ck(g_unload_cargo_manifest_calls.empty(),
           "L4: A_MOTHER -- unload_cargo_manifest does NOT fire (exclusive arms)");
        if (g_flush_cargo_hold_calls.size() == 1) {
            ck((uint32_t)g_flush_cargo_hold_calls[0].player == (uint32_t)s.player &&
                   g_flush_cargo_hold_calls[0].building_index == s.index,
               "L4: flush_cargo_hold(cur_player, cur_index)");
        }
    }

    // =================================================================================================
    // L5 -- type dispatch, H_MOTHER (0x1a): same branch target (LAB_0047132f) as A_MOTHER, reached via
    // the decision at 0x00471319, JBE 0x0047132f leg of the chain -- flush_cargo_hold fires, unload does not.
    // =================================================================================================
    {
        Seed s;
        s.type = BUILDING_TYPE_H_MOTHER;
        seed_and_run(fx, s);

        ck(g_flush_cargo_hold_calls.size() == 1,
           "L5 @0x00471319/JBE 0x0047132f: H_MOTHER(0x1a) takes the SAME arm as A_MOTHER -- flush_cargo_hold fires");
        ck(g_unload_cargo_manifest_calls.empty(), "L5: H_MOTHER -- unload_cargo_manifest does NOT fire");
    }

    // =================================================================================================
    // L6 -- type dispatch, A_SHUTTLE (0xd): decision at 0x0047130d, JBE 0x00471344 -> unload_cargo_manifest
    // fires (return value discarded); flush_cargo_hold does NOT.
    // =================================================================================================
    {
        Seed s;
        s.type                      = BUILDING_TYPE_A_SHUTTLE;
        g_unload_cargo_manifest_ret = 0x7777; // arbitrary nonzero -- proves the discarded return value
        seed_and_run(fx, s);                  // has no further effect (no capture site exists to read it)

        ck(g_unload_cargo_manifest_calls.size() == 1,
           "L6 @0x00471344-0x00471352: A_SHUTTLE(0xd) -> unload_cargo_manifest fires exactly once");
        ck(g_flush_cargo_hold_calls.empty(), "L6: A_SHUTTLE -- flush_cargo_hold does NOT fire");
        if (g_unload_cargo_manifest_calls.size() == 1) {
            ck((uint32_t)g_unload_cargo_manifest_calls[0].player == (uint32_t)s.player &&
                   g_unload_cargo_manifest_calls[0].building_index == s.index,
               "L6: unload_cargo_manifest(cur_player, cur_index)");
        }
    }

    // =================================================================================================
    // L7 -- type dispatch, H_SHUTTLE (0x21): decision at 0x0047131f, JZ 0x00471344 -> the SAME
    // unload_cargo_manifest arm as A_SHUTTLE; flush_cargo_hold does NOT fire.
    // =================================================================================================
    {
        Seed s;
        s.type = BUILDING_TYPE_H_SHUTTLE;
        seed_and_run(fx, s);

        ck(g_unload_cargo_manifest_calls.size() == 1,
           "L7 @0x00471323/JZ 0x00471344: H_SHUTTLE(0x21) takes the SAME arm as A_SHUTTLE -- unload fires");
        ck(g_flush_cargo_hold_calls.empty(), "L7: H_SHUTTLE -- flush_cargo_hold does NOT fire");
    }

    // =================================================================================================
    // L8 -- type dispatch, a PLAIN type outside all four dispatch values: neither call fires
    // (falls through the whole chain to JMP 0x00471357, the shared "no call" landing pad).
    // =================================================================================================
    {
        Seed s;
        s.type = 0x2; // not A_MOTHER/H_MOTHER/A_SHUTTLE/H_SHUTTLE
        seed_and_run(fx, s);

        ck(g_register_online_calls.size() == 1, "L8: register_online still fires (unconditional spine)");
        ck(g_flush_cargo_hold_calls.empty(), "L8 @0x00471325/JMP 0x00471357: plain type -- flush_cargo_hold does NOT fire");
        ck(g_unload_cargo_manifest_calls.empty(),
           "L8 @0x00471325/JMP 0x00471357: plain type -- unload_cargo_manifest does NOT fire");
    }
}

} // namespace mh::sim::test
