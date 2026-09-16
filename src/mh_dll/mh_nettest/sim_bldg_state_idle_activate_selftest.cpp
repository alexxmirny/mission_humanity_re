//
// sim_bldg_state_idle_activate_selftest.cpp -- `simtest` cases for llm_strat_bldg_state_idle_activate
// (sim/sim_bldg_state_reset_idle.h/.cpp, detail::bldg_state_idle_activate), SIM1B building_tick
// machinery -- the "idle, decide turret-scan vs plain idle-noop" per-state tick handler.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_idle_activate_00472415.asm), cross-checked against the .h/.cpp's
// own per-line address citations -- NOT read off the .cpp body alone:
//
//   0x00472432: online_state = 1 (unconditional, before the cfg read).
//   0x0047243d-0x0047244d: type = Building[cur_building->building_id].type (uint8_t).
//   0x00472450-0x00472462: the exact CMP/JC/JBE/JZ four-way branch that picks `state`:
//     JC   @0x00472454 taken   (type <  A_TURRET/0x5)                    -> state = IDLE_NOOP_88 (0x88)
//     JC   not taken, JBE @0x0047245a taken (type == A_TURRET, JC already proved type>=A_TURRET)
//                                                                         -> state = TURRET_SCAN  (0x7a)
//     JBE  not taken, JZ  @0x00472460 taken (type == H_TURRET/0x19)      -> state = TURRET_SCAN  (0x7a)
//     JZ   not taken (type > A_TURRET and type != H_TURRET)              -> state = IDLE_NOOP_88 (0x88)
//   This file pins all FOUR arms individually -- including a value STRICTLY BETWEEN A_TURRET(0x5) and
//   H_TURRET(0x19), and a value ABOVE H_TURRET -- so a translation that collapsed the chain to the
//   equivalent-looking `type == A_TURRET || type == H_TURRET` boolean (which happens to produce the
//   same truth table, per the .h banner's own note) cannot silently pass by only checking the two
//   boundary values.
//   0x0047247e-0x0047248c: llm_strat_bldg_notify_ui(cur_player, cur_index) -- unconditional, all arms.
//   0x00472491-0x004724a5: tick_budget = 0.0 -- unconditional, all arms (same tail as
//   bldg_state_idle_noop's own tick_budget zero).
//
#include "sim/sim_bldg_state_reset_idle.h"

#include <cstdint>
#include <vector>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_TURRET (0x05) / BUILDING_TYPE_H_TURRET (0x19)
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- the one callee this closure reaches --------------------------------------------------------
struct NotifyCall {
    uint16_t player;
    uint32_t index;
};
std::vector<NotifyCall> g_notify_calls;
void                    rec_bldg_notify_ui(uint16_t player, uint32_t index) {
    g_notify_calls.push_back({player, index});
}

const bldg_state_reset_idle_calls g_calls = {
    &rec_bldg_notify_ui,
};

void reset_observations() { g_notify_calls.clear(); }

// ---- fixture seeding -----------------------------------------------------------------------------
// DISTINCT, non-default, non-symmetric values throughout (sim_test_support.h's own rule): player !=
// index, building_id (cfg_row) distinct from both, and the pre-call sentinels for state/online_state/
// tick_budget are each distinct from every value the function can legitimately produce, so a case that
// forgets to write one of them is caught rather than accidentally matching a coincidental default.
struct Seed {
    uint16_t player  = 3;
    int32_t  index   = 7;
    uint16_t cfg_row = 37; // building_id -> Building[cfg_row], distinct from player/index
    uint8_t  type    = 0;  // set per case

    double tick_budget = 12.5; // nonzero sentinel -- the function must zero it on every arm
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    building &b    = fx.b(s.player, s.index);
    b.building_id  = s.cfg_row;
    b.state        = (uint16_t)0xDEAD; // sentinel, distinct from both IDLE_NOOP_88(0x88) and TURRET_SCAN(0x7a)
    b.online_state = (int16_t)-999;    // sentinel, distinct from the 1 the function must write

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = s.player;
    fx.view_cur_index   = (uint16_t)s.index;

    cfg_building &cb = fx.cfg_buildings[s.cfg_row];
    cb.type          = s.type;

    fx.tick_budget = s.tick_budget;

    reset_observations();

    sim_store own = fx.store();
    detail::bldg_state_idle_activate(fx.view(), own, g_calls);
}

} // namespace

void run_bldg_state_idle_activate_tests() {
    sim_fixture fx;

    // =================================================================================================
    // A1 -- type < A_TURRET (0x5): the JC branch @0x00472454 is taken -> IDLE_NOOP_88.
    // =================================================================================================
    {
        Seed s;
        s.type = 0; // 0 < 5
        seed_and_run(fx, s);
        ck_eq((uint32_t)fx.b(s.player, s.index).state, (uint32_t)BLDG_STATE_IDLE_NOOP_88,
              "A1 @0x00472454 (JC taken, type=0 < A_TURRET/0x5): state = IDLE_NOOP_88 (0x88)");
    }

    // =================================================================================================
    // A2 -- type == A_TURRET (0x5) EXACTLY (lower boundary): JC not taken (0x00472454), JBE taken
    // (0x0047245a) -> TURRET_SCAN. Proves the boundary itself, not just "less than".
    // =================================================================================================
    {
        Seed s;
        s.type = BUILDING_TYPE_A_TURRET; // == 0x5
        seed_and_run(fx, s);
        ck_eq((uint32_t)fx.b(s.player, s.index).state, (uint32_t)BLDG_STATE_TURRET_SCAN,
              "A2 @0x0047245a (JC not taken, JBE taken, type==A_TURRET/0x5 exactly): state = TURRET_SCAN (0x7a)");
    }

    // =================================================================================================
    // A3 -- type STRICTLY BETWEEN A_TURRET(0x5) and H_TURRET(0x19), e.g. 0x10: JC not taken, JBE not
    // taken, JZ not taken (0x00472460 -- cmp 0x10,0x19 fails) -> falls through to the JMP @0x00472462
    // -> IDLE_NOOP_88. This is the case that would catch a translation collapsed to the equivalent-
    // looking `type == A_TURRET || type == H_TURRET` boolean landing on the WRONG side by accident, and
    // more importantly proves the "else" arm is reached by RANGE (any value strictly between the two
    // named constants), not merely by falling through two adjacent case labels.
    // =================================================================================================
    {
        Seed s;
        s.type = 0x10; // strictly between 0x5 and 0x19
        seed_and_run(fx, s);
        ck_eq((uint32_t)fx.b(s.player, s.index).state, (uint32_t)BLDG_STATE_IDLE_NOOP_88,
              "A3 @0x00472462 (JZ not taken, type=0x10 strictly between A_TURRET/0x5 and H_TURRET/0x19): "
              "state = IDLE_NOOP_88 (0x88)");
    }

    // =================================================================================================
    // A4 -- type == H_TURRET (0x19) EXACTLY (upper boundary): JC not taken, JBE not taken, JZ taken
    // (0x00472460) -> TURRET_SCAN.
    // =================================================================================================
    {
        Seed s;
        s.type = BUILDING_TYPE_H_TURRET; // == 0x19
        seed_and_run(fx, s);
        ck_eq((uint32_t)fx.b(s.player, s.index).state, (uint32_t)BLDG_STATE_TURRET_SCAN,
              "A4 @0x00472460 (JZ taken, type==H_TURRET/0x19 exactly): state = TURRET_SCAN (0x7a)");
    }

    // =================================================================================================
    // A5 -- type ABOVE H_TURRET (0x19), e.g. 0x20: JC not taken, JBE not taken, JZ not taken -> the same
    // else arm as A3, but from above the upper boundary rather than between the two constants -- proves
    // the else arm is not merely "anything <= some upper bound".
    // =================================================================================================
    {
        Seed s;
        s.type = 0x20; // > H_TURRET/0x19
        seed_and_run(fx, s);
        ck_eq((uint32_t)fx.b(s.player, s.index).state, (uint32_t)BLDG_STATE_IDLE_NOOP_88,
              "A5 @0x00472462 (JZ not taken, type=0x20 > H_TURRET/0x19): state = IDLE_NOOP_88 (0x88)");
    }

    // =================================================================================================
    // A6 -- effects that must hold on EVERY arm, checked across all five type values in one case:
    // online_state is always set to 1 (0x00472432, BEFORE the cfg read/branch -- so it does not depend
    // on which arm is taken), notify_ui always fires exactly once with (cur_player, cur_index)
    // (0x0047247e-0x0047248c), and tick_budget is always zeroed (0x00472491-0x004724a5).
    // =================================================================================================
    {
        const uint8_t types[] = {0, BUILDING_TYPE_A_TURRET, 0x10, BUILDING_TYPE_H_TURRET, 0x20};
        for (uint8_t t : types) {
            Seed s;
            s.type = t;
            seed_and_run(fx, s);

            ck_eq((uint32_t)(uint16_t)fx.b(s.player, s.index).online_state, 1u,
                  "A6 @0x00472432: online_state = 1 regardless of branch (type-dependent case above)");

            ck(g_notify_calls.size() == 1, "A6 @0x0047248c: bldg_notify_ui fires exactly once, all arms");
            if (g_notify_calls.size() == 1) {
                ck(g_notify_calls[0].player == s.player && g_notify_calls[0].index == (uint32_t)s.index,
                   "A6 @0x0047247e-0x0047248c: bldg_notify_ui(cur_player, cur_index)");
            }

            ck_eq_d(fx.tick_budget, 0.0, "A6 @0x00472491-0x004724a5: tick_budget zeroed, all arms");
        }
    }
}

} // namespace mh::sim::test
