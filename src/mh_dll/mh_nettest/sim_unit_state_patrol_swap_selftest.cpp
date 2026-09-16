//
// sim_unit_state_patrol_swap_selftest.cpp -- `simtest` cases for llm_strat_unit_state_patrol_swap
// @0x0047e2e2 (sim/sim_unit_state_patrol_swap.h/.cpp). This is a SIM1-G1 move-state handler; per the
// rig-shadow-deferred posture for this batch, this offline oracle is the PRIMARY verification.
//
// SCOPE: the four-field goal<->home swap (order matters -- goal_x/goal_y must pick up the OLD
// home_x/home_y, not the value home_x/home_y is about to be overwritten with), the
// llm_strat_unit_set_state_order call (args + literal order code 0x10), the "does NOT touch
// tick_budget" negative claim (no reference to its address anywhere in the 0x9e-byte body), and
// neighbouring-slot non-corruption.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM
// tmp/decomp/llm_strat_unit_state_patrol_swap_0047e2e2.asm -- every assertion below cites the
// instruction address(es) it pins. NOT read off the .cpp body.
//
#include "sim/sim_unit_state_patrol_swap.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- the one recorder: unit_set_state_order(move_op_arg, order_code) ---------------------------
struct SetStateOrderCall {
    uint16_t move_op_arg;
    uint16_t order_code;
};
std::vector<SetStateOrderCall> g_set_state_order_calls;
void                           rec_unit_set_state_order(uint16_t move_op_arg, uint16_t order_code) {
    g_set_state_order_calls.push_back({move_op_arg, order_code});
}

const unit_state_patrol_swap_calls g_calls = {
    &rec_unit_set_state_order,
};

void reset_observations() { g_set_state_order_calls.clear(); }

// Fixed "guard" slot no test's own (player,index) ever touches -- seeded with sentinel nonzero data
// each run so a wrong-index write lands somewhere observable. Kept outside every test's own
// player (0..1) / index (1..3) range below.
constexpr uint16_t GUARD_PLAYER = 5;
constexpr int32_t  GUARD_INDEX  = 9;

void seed_guard_slot(sim_fixture &fx) {
    unit &g         = fx.u(GUARD_PLAYER, GUARD_INDEX);
    g.unit_proto_id = 77;
    g.x             = 201;
    g.y             = 202;
    g.goal_x        = 203;
    g.goal_y        = 204;
    g.home_x        = 205;
    g.home_y        = 206;
}

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player  = 0;
    int32_t  index   = 1;
    uint16_t cfg_row = 7;

    // FOUR DISTINCT, non-symmetric values so a wrong swap order or a swapped field is caught.
    uint8_t x      = 37;
    uint8_t y      = 91;
    uint8_t goal_x = 12; // pre-swap sentinel -- must be OVERWRITTEN
    uint8_t goal_y = 200;
    uint8_t home_x = 150;
    uint8_t home_y = 63;

    uint8_t move_op_arg = 0x5a; // distinct from PATROL_SWAP(0x10) and from every field value above

    double tick_budget = 42.0; // nonzero, must be left UNTOUCHED (0x9e-byte body has no ref to it)
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();
    seed_guard_slot(fx);

    unit &u         = fx.u(s.player, s.index);
    u.unit_proto_id = s.cfg_row;
    u.x             = s.x;
    u.y             = s.y;
    u.goal_x        = s.goal_x;
    u.goal_y        = s.goal_y;
    u.home_x        = s.home_x;
    u.home_y        = s.home_y;

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = s.player;
    fx.view_cur_index  = (uint16_t)s.index;

    fx.cfg_units[s.cfg_row].move_op_arg = s.move_op_arg;

    fx.tick_budget = s.tick_budget;

    reset_observations();

    sim_store own = fx.store();
    detail::unit_state_patrol_swap(fx.view(), own, g_calls);
}

} // namespace

void run_unit_state_patrol_swap_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- the four-field goal<->home swap, order-load-bearing: goal_x/goal_y must end up holding the
    // OLD home_x/home_y (0x0047e2fa-0x0047e328), and ONLY THEN home_x/home_y is overwritten from x/y
    // (0x0047e328-0x0047e356). x/y themselves are untouched (nothing in the body writes them). Seeded
    // with 4 distinct, non-symmetric values so a wrong swap order (goal would pick up the ALREADY-
    // overwritten home_x/home_y) or a swapped field (e.g. home_x=y instead of home_x=x) disagrees here.
    // =================================================================================================
    {
        Seed s;
        s.player = 0;
        s.index  = 1;
        s.x      = 37;
        s.y      = 91;
        s.goal_x = 12;
        s.goal_y = 200;
        s.home_x = 150;
        s.home_y = 63;
        seed_and_run(fx, s);

        const unit &out = fx.u(s.player, s.index);
        ck_eq((uint32_t)out.goal_x, (uint32_t)s.home_x,
              "T1: goal_x = OLD home_x (0x0047e2fa-0x0047e311, dest[+0x86]=src[+0x88])");
        ck_eq((uint32_t)out.goal_y, (uint32_t)s.home_y,
              "T1: goal_y = OLD home_y (0x0047e311-0x0047e328, dest[+0x87]=src[+0x89])");
        ck_eq((uint32_t)out.home_x, (uint32_t)s.x,
              "T1: home_x = x, AFTER goal was already written from the OLD home_x "
              "(0x0047e328-0x0047e33f, dest[+0x88]=src[+0x84])");
        ck_eq((uint32_t)out.home_y, (uint32_t)s.y,
              "T1: home_y = y, AFTER goal was already written from the OLD home_y "
              "(0x0047e33f-0x0047e356, dest[+0x89]=src[+0x85])");
        ck_eq((uint32_t)out.x, (uint32_t)s.x, "T1: x is untouched (no write to +0x84 anywhere in the body)");
        ck_eq((uint32_t)out.y, (uint32_t)s.y, "T1: y is untouched (no write to +0x85 anywhere in the body)");

        // A wrong-order translation (home_x/home_y overwritten BEFORE goal_x/goal_y is read) would
        // instead produce goal_x==x(37) and goal_y==y(91) here -- both distinct from the correct
        // goal_x==150/goal_y==63 above, so either mistake is caught by the two goal_* checks alone.
        ck(out.goal_x != s.x && out.goal_y != s.y,
           "T1: goal_x/goal_y did NOT pick up the NEW home_x/home_y (would happen under a reordered "
           "swap that overwrites home_x/home_y before reading it into goal_x/goal_y)");

        ck(g_set_state_order_calls.size() == 1 &&
               g_set_state_order_calls[0].move_op_arg == (uint16_t)s.move_op_arg &&
               g_set_state_order_calls[0].order_code == UNIT_STATE_PATROL_SWAP_ORDER,
           "T1: unit_set_state_order(Unit[proto].move_op_arg, PATROL_SWAP=0x10) fires exactly once, "
           "param order EAX=move_op_arg/EDX=0x10 (0x0047e356 MOV EDX,0x10 precedes the move_op_arg "
           "computation; call at 0x0047e371)");

        ck_eq_d(fx.tick_budget, s.tick_budget,
                "T1: tick_budget is left UNTOUCHED -- no reference to its address anywhere in the "
                "0x9e-byte body (unlike most of this batch's handlers, this one spends no per-tick "
                "budget)");

        const unit &g = fx.u(GUARD_PLAYER, GUARD_INDEX);
        ck(g.unit_proto_id == 77 && g.x == 201 && g.y == 202 && g.goal_x == 203 && g.goal_y == 204 &&
               g.home_x == 205 && g.home_y == 206,
           "T1: a neighbouring roster slot this call never addresses is left exactly as seeded");
    }

    // =================================================================================================
    // T2 -- a SECOND, DIFFERENT (player,index,proto,field-values) combination, proving the swap reads
    // THIS unit's own fields (not a fixed slot) and that the move_op_arg read is indexed by THIS
    // unit's own unit_proto_id (0x0047e360 MOVZX EAX,[EAX+0x2] then IMUL EAX,0x23f -- Unit[proto],
    // 0x0047e36a byte read at +0xe4a184 == move_op_arg@0xec), not a hardcoded row.
    // =================================================================================================
    {
        Seed s;
        s.player      = 1;
        s.index       = 3;
        s.cfg_row     = 42; // a DIFFERENT proto row than T1's 7
        s.x           = 8;
        s.y           = 250;
        s.goal_x      = 99;
        s.goal_y      = 5;
        s.home_x      = 17;
        s.home_y      = 240;
        s.move_op_arg = 0x3c; // a DIFFERENT move_op_arg than T1's 0x5a
        s.tick_budget = 0.0;  // also cover the tick_budget==0 edge -- still must stay 0, not get spent
        seed_and_run(fx, s);

        const unit &out = fx.u(s.player, s.index);
        ck_eq((uint32_t)out.goal_x, (uint32_t)s.home_x, "T2: goal_x = OLD home_x (second, distinct fixture)");
        ck_eq((uint32_t)out.goal_y, (uint32_t)s.home_y, "T2: goal_y = OLD home_y (second, distinct fixture)");
        ck_eq((uint32_t)out.home_x, (uint32_t)s.x, "T2: home_x = x (second, distinct fixture)");
        ck_eq((uint32_t)out.home_y, (uint32_t)s.y, "T2: home_y = y (second, distinct fixture)");

        ck(g_set_state_order_calls.size() == 1 &&
               g_set_state_order_calls[0].move_op_arg == (uint16_t)s.move_op_arg &&
               g_set_state_order_calls[0].order_code == UNIT_STATE_PATROL_SWAP_ORDER,
           "T2: unit_set_state_order got Unit[42].move_op_arg(0x3c), not T1's Unit[7].move_op_arg(0x5a) "
           "-- proves the read is indexed by THIS unit's own unit_proto_id, not a fixed row");

        ck_eq_d(fx.tick_budget, 0.0, "T2: tick_budget==0 on entry stays exactly 0 -- not spent/reset either way");
    }
}

} // namespace mh::sim::test
