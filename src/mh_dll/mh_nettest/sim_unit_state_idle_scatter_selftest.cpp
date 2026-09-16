//
// sim_unit_state_idle_scatter_selftest.cpp -- `simtest` oracle for
// llm_strat_unit_state_idle_scatter @0x00485ebb (sim/sim_unit_state_idle_scatter.h/.cpp, RI-SIM /
// SIM1-G1).
//
// SCOPE: the order_queued early-return gate (0x00485ed8 CMP / 0x00485edc JZ) that zeroes tick_budget
// and returns BEFORE anything else runs; the heli-class membership test as FOUR near-identical
// CMP/JZ arms collapsed to one OR in the .cpp (0x00485f20/0x00485f52/0x00485f86/0x00485fba -- each of
// the four UNIT_TYPE_A_HELI/H_HELI/_HELI_CARGO(x2) values individually, plus one adjacent non-member
// value that must NOT trigger it) -- unit_set_state(HOVER_ENGAGE=0x2e) and immediate return
// (0x00485fc3-0x00485fcd); the scatter-offset draw loop (0x00485fd2-0x00485ffa) -- dx drawn FIRST
// then dy (not swapped), the reroll-while-BOTH-zero condition (`OR EAX,EDX; TEST; JZ`, not an
// independent per-axis reroll), and the exact upper_bound(5) argument to every rand_below call; the
// goal_x/goal_y torus-wrap-mask arithmetic against width_mask/height_mask (0x00485ffc-0x0048603e,
// covering both a positive-overflow clip and a negative-delta byte-wrap) plus the UNCONDITIONAL
// home_x/home_y <- current x/y stamp (0x0048603e-0x0048606c, unaffected by anything below it); the
// LANDING_REQUEST-override conjunction (order==0x29 AND home_storage_slot!=0, 0x00486071-0x00486085)
// as two INDEPENDENTLY-gating operands, its storage_get_approach_tile call + exact args + the two
// out-param -> goal_x/goal_y assignments (0x00486085-0x004860b8) OVERRIDING the scatter goal already
// computed, and its unit_set_state(move_op_arg) re-entry call (0x004860be-0x004860da); the plain-
// scatter else arm's unit_set_state_order(move_op_arg, PATROL_SWAP=0x10) call (0x004860dc-0x004860f7);
// and neighbouring-slot non-corruption.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM
// tmp/decomp_sim/llm_strat_unit_state_idle_scatter_00485ebb.asm -- every assertion below cites the
// instruction address(es) it pins. NOT read off the .cpp body.
//
#include "sim/sim_unit_state_idle_scatter.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim/sim_order_enqueue.h" // UNIT_TYPE_A_HELI/H_HELI/_CARGO, UNIT_STATE_PATROL_SWAP/HOVER_ENGAGE
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: proves CALL ORDER across all 4 unit_state_idle_scatter_calls members -----------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- per-callee recorders (4, one per unit_state_idle_scatter_calls member) ------------------------
std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) {
    tr("unit_set_state");
    g_set_state_calls.push_back(new_state);
}

// RAND_RET_DEFAULT is deliberately the value that decodes to dx=0/dy=0 (a rand_below(5) return of 2 ->
// 2-2=0) -- the loop's OWN reroll trigger. This is intentional, not an oversight: the original's
// draw loop (0x00485fd2-0x00485ffa) has no iteration cap, so an UNCANNED call must fail LOUDLY (the
// whole simtest run hangs) rather than silently returning a plausible-looking value that happens to
// terminate the loop. Every case below that reaches this loop supplies an explicit `rand_ret` pair
// whose first nonzero draw is reached within a bounded, known number of calls -- this default is only
// ever actually consumed by T4's deliberate first (0,0) reroll pair below.
std::vector<int32_t> g_rand_ub;  // upper_bound argument, one entry per rand_below call
std::vector<int32_t> g_rand_ret; // canned return values, consumed dx-then-dy per iteration
constexpr int32_t    RAND_RET_DEFAULT = 2;
int32_t              rec_rand_below(int32_t upper_bound) {
    tr("rand_below");
    g_rand_ub.push_back(upper_bound);
    const size_t i = g_rand_ub.size() - 1;
    return i < g_rand_ret.size() ? g_rand_ret[i] : RAND_RET_DEFAULT;
}

struct StorageApproachCall {
    uint16_t player;
    uint16_t unit_index;
    uint32_t storage_idx;
};
std::vector<StorageApproachCall> g_storage_calls;
uint32_t                         g_storage_out_fine_x = 0;
uint32_t                         g_storage_out_fine_y = 0;
void                             rec_storage_get_approach_tile(uint16_t player, uint16_t unit_index, uint32_t *out_fine_x,
                                                               uint32_t *out_fine_y, uint32_t storage_idx) {
    tr("storage_get_approach_tile");
    g_storage_calls.push_back({player, unit_index, storage_idx});
    *out_fine_x = g_storage_out_fine_x;
    *out_fine_y = g_storage_out_fine_y;
}

struct SetStateOrderCall {
    uint16_t new_state, new_order;
};
std::vector<SetStateOrderCall> g_set_state_order_calls;
void                           rec_unit_set_state_order(uint16_t new_state, uint16_t new_order) {
    tr("unit_set_state_order");
    g_set_state_order_calls.push_back({new_state, new_order});
}

const unit_state_idle_scatter_calls g_calls = {
    &rec_unit_set_state,
    &rec_rand_below,
    &rec_storage_get_approach_tile,
    &rec_unit_set_state_order,
};

void reset_observations() {
    g_trace.clear();
    g_set_state_calls.clear();
    g_rand_ub.clear();
    g_rand_ret.clear();
    g_storage_calls.clear();
    g_set_state_order_calls.clear();
}

// Fixed "guard" slot no test's own (player,index) ever touches -- seeded with sentinel nonzero data
// (including values that WOULD trigger several of this function's own branches, e.g. order_queued=1
// and order==LANDING_REQUEST with a real home_storage_slot) each run so a wrong-index read/write is
// observable, not just a plain zero.
constexpr uint16_t GUARD_PLAYER = 4;
constexpr int32_t  GUARD_INDEX  = 7;

void seed_guard_slot(sim_fixture &fx) {
    unit &g             = fx.u(GUARD_PLAYER, GUARD_INDEX);
    g.unit_proto_id     = 66;
    g.order_queued      = 1;
    g.order             = IDLE_SCATTER_ORDER_LANDING_REQUEST;
    g.home_storage_slot = 9;
    g.x                 = 12;
    g.y                 = 34;
    g.goal_x            = 56;
    g.goal_y            = 78;
    g.home_x            = 90;
    g.home_y            = 11;
}

// ---- fixture seeding -------------------------------------------------------------------------------
struct Seed {
    uint16_t player  = 0;
    int32_t  index   = 1;
    uint16_t cfg_row = 10;

    // Neutral defaults: order_queued=0 clears the early-return gate; proto_type=0 is not one of the
    // four heli members (0xf/0x10/0x17/0x18) so the heli branch does not fire; order=0x05 is not
    // LANDING_REQUEST(0x29) so the override never engages unless a case asks for it.
    uint8_t  order_queued      = 0;
    uint32_t proto_type        = 0;
    uint16_t order             = 0x05;
    uint8_t  home_storage_slot = 0;

    uint8_t x = 30, y = 20;

    uint32_t width_mask  = 0xff; // sim_fixture::reset()'s own default
    uint32_t height_mask = 0x3f;

    double tick_budget = 12.5; // nonzero sentinel -- must read back 0.0 ONLY via the order_queued arm

    // cfg_units[proto].move_op_arg -- distinct from PATROL_SWAP(0x10) and from HOVER_ENGAGE(0x2e) so
    // a translation that swapped in one of the other two constants is caught.
    uint8_t move_op_arg = 0x21;

    std::vector<int32_t> rand_ret; // canned rand_below returns, consumed dx-then-dy per iteration

    // Distinct from each other and from any scatter-computed goal in these cases, so the override
    // is visible against the goal the draw loop would otherwise have produced.
    uint32_t storage_out_fine_x = 111;
    uint32_t storage_out_fine_y = 222;
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();
    seed_guard_slot(fx);

    unit &u             = fx.u(s.player, s.index);
    u.unit_proto_id     = s.cfg_row;
    u.order_queued      = s.order_queued;
    u.order             = s.order;
    u.home_storage_slot = s.home_storage_slot;
    u.x                 = s.x;
    u.y                 = s.y;
    // Sentinels distinct from x/y/each other, so an unwritten goal/home is visible rather than
    // coincidentally matching what the function would have written anyway.
    u.goal_x = 250;
    u.goal_y = 251;
    u.home_x = 252;
    u.home_y = 253;

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = s.player;
    fx.view_cur_index  = (uint16_t)s.index;

    fx.cfg_units[s.cfg_row].type        = s.proto_type;
    fx.cfg_units[s.cfg_row].move_op_arg = s.move_op_arg;

    fx.geom.width_mask  = s.width_mask;
    fx.geom.height_mask = s.height_mask;

    fx.tick_budget = s.tick_budget;

    reset_observations();
    g_rand_ret           = s.rand_ret;
    g_storage_out_fine_x = s.storage_out_fine_x;
    g_storage_out_fine_y = s.storage_out_fine_y;

    sim_store own = fx.store();
    detail::unit_state_idle_scatter(fx.view(), own, g_calls);
}

} // namespace

void run_unit_state_idle_scatter_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- order_queued early-return gate (0x00485ed8 CMP / 0x00485edc JZ). T1a: nonzero -- returns
    // immediately, tick_budget zeroed, NO callee runs at all (not even rand_below), and goal/home are
    // UNTOUCHED (still the sentinels the seeder wrote). T1b: zero -- does NOT return early, proven by
    // reaching the heli branch (which the gate would otherwise have skipped over) AND by tick_budget
    // staying at its sentinel (only the order_queued arm ever zeroes it).
    // =================================================================================================
    {
        Seed s;
        s.order_queued = 5; // nonzero
        seed_and_run(fx, s);
        ck(g_trace.empty(), "T1a: order_queued!=0 -- no callee runs at all (early return, 0x00485edc JZ taken)");
        ck_eq_d(fx.tick_budget, 0.0, "T1a: tick_budget zeroed (0x00485ede/0x00485ee8, both dwords)");
        const unit &u = fx.u(s.player, s.index);
        ck(u.goal_x == 250 && u.goal_y == 251 && u.home_x == 252 && u.home_y == 253,
           "T1a: goal_x/goal_y/home_x/home_y UNCHANGED -- the early return precedes every later write");
    }
    {
        Seed s;
        s.order_queued = 0;
        s.proto_type   = UNIT_TYPE_A_HELI; // reaches gate 2, which the T1a gate would have skipped past
        seed_and_run(fx, s);
        ck(trace_eq({"unit_set_state"}) && g_set_state_calls[0] == UNIT_STATE_HOVER_ENGAGE,
           "T1b: order_queued==0 -- gate NOT taken (0x00485edc JZ not taken), falls into the heli-class "
           "dispatch and calls unit_set_state(HOVER_ENGAGE)");
        ck_eq_d(fx.tick_budget, s.tick_budget,
                "T1b: tick_budget UNCHANGED -- only the order_queued arm ever zeroes it");
    }

    // =================================================================================================
    // T2 -- heli-class membership (0x00485ef7-0x00485fc3): all FOUR distinct CMP/JZ arms individually
    // (A_HELI=0xf, H_HELI=0x10, A_HELI_CARGO=0x17, H_HELI_CARGO=0x18), each firing
    // unit_set_state(HOVER_ENGAGE=0x2e) (0x00485fc3-0x00485fcd) and returning immediately -- NO draw,
    // NO goal/home write. Plus ONE adjacent non-member value (A_PLANE=0x11, between H_HELI and the
    // two _CARGO members) that must NOT trigger the branch and instead falls through to the draw.
    // =================================================================================================
    {
        const uint32_t members[4] = {UNIT_TYPE_A_HELI, UNIT_TYPE_H_HELI, UNIT_TYPE_A_HELI_CARGO,
                                     UNIT_TYPE_H_HELI_CARGO};
        for (uint32_t m : members) {
            Seed s;
            s.order_queued = 0;
            s.proto_type   = m;
            seed_and_run(fx, s);
            ck(trace_eq({"unit_set_state"}) && g_set_state_calls[0] == UNIT_STATE_HOVER_ENGAGE,
               "T2: heli-class type -- unit_set_state(HOVER_ENGAGE=0x2e) fires and nothing else "
               "(0x00485f20/0x00485f52/0x00485f86/0x00485fba CMP, 0x00485fc3 CALL)");
            ck_eq((uint32_t)g_rand_ub.size(), 0u, "T2: heli-class type -- rand_below NOT called (immediate return)");
            const unit &u = fx.u(s.player, s.index);
            ck(u.goal_x == 250 && u.goal_y == 251 && u.home_x == 252 && u.home_y == 253,
               "T2: heli-class type -- goal_x/goal_y/home_x/home_y UNCHANGED (returns before step 4)");
        }

        Seed s;
        s.order_queued = 0;
        s.proto_type   = UNIT_TYPE_A_PLANE; // 0x11 -- adjacent to H_HELI(0x10), NOT a member
        s.order        = 0x05;              // not LANDING_REQUEST -- lands in the plain-scatter else arm
        s.rand_ret     = {4, 4};            // dx=2, dy=2 -- terminates the draw loop in one iteration
        seed_and_run(fx, s);
        ck_eq((uint32_t)g_rand_ub.size(), 2u,
              "T2: A_PLANE(0x11) is NOT a heli member -- falls through to the scatter draw (0x00485f5d "
              "JNZ / 0x00485f91 JNZ / 0x00485fc1 JNZ all taken)");
        ck(g_set_state_calls.empty(), "T2: A_PLANE(0x11) -- unit_set_state(HOVER_ENGAGE) does NOT fire");
    }

    // =================================================================================================
    // T3 -- the scatter draw's ORDER (0x00485fd2-0x00485ffa): dx is the FIRST rand_below(5) draw, dy
    // the SECOND -- NOT swapped. Asymmetric canned returns (4 then 1 -> dx=2, dy=-1) make a swap
    // produce a DIFFERENT, checkable goal (dx=-1,dy=2 instead). Full-width masks isolate the pure
    // value from any wrap clipping. Also pins the exact upper_bound(5) argument on both draws.
    // =================================================================================================
    {
        Seed s;
        s.order_queued = 0;
        s.proto_type   = 0; // not heli
        s.order        = 0x05;
        s.x = 30, s.y = 20;
        s.width_mask = 0xff, s.height_mask = 0xff; // no clipping -- isolates the raw value
        s.rand_ret = {4, 1};                       // dx = 4-2 = 2, dy = 1-2 = -1
        seed_and_run(fx, s);
        ck_eq((uint32_t)g_rand_ub.size(), 2u, "T3: exactly two rand_below draws, no reroll");
        ck_eq((uint32_t)g_rand_ub[0], 5u, "T3: dx draw's upper_bound == 5 (0x00485fd2 MOV EAX,0x5)");
        ck_eq((uint32_t)g_rand_ub[1], 5u, "T3: dy draw's upper_bound == 5 (0x00485fe2 MOV EAX,0x5)");
        const unit &u = fx.u(s.player, s.index);
        ck_eq((uint32_t)u.goal_x, (30u + 2u) & 0xffu,
              "T3: goal_x uses the FIRST draw as dx=2 (0x00485fd7 CALL is dx, not dy) -- a swap would "
              "give goal_x=(30-1)&0xff=29, not 32");
        ck_eq((uint32_t)u.goal_y, (uint32_t)((20 - 1) & 0xff),
              "T3: goal_y uses the SECOND draw as dy=-1 (0x00485fe7 CALL is dy, not dx) -- a swap would "
              "give goal_y=(20+2)&0xff=22, not 19");
    }

    // =================================================================================================
    // T4 -- the reroll condition (0x00485ff2-0x00485ffa: OR EAX,EDX; TEST; JZ) rerolls the WHOLE PAIR
    // only while BOTH dx AND dy are exactly zero. First canned pair (2,2) decodes to dx=0/dy=0 ->
    // reroll; second pair (3,2) decodes to dx=1/dy=0 -- a SINGLE nonzero axis is enough to exit, so
    // the loop must stop here (not keep rerolling because dy is still 0). Exactly 4 draws total.
    // =================================================================================================
    {
        Seed s;
        s.order_queued = 0;
        s.proto_type   = 0;
        s.order        = 0x05;
        s.x = 30, s.y = 20;
        s.width_mask = 0xff, s.height_mask = 0xff;
        s.rand_ret = {2, 2, 3, 2}; // pair1: dx=0,dy=0 (reroll); pair2: dx=1,dy=0 (exit)
        seed_and_run(fx, s);
        ck_eq((uint32_t)g_rand_ub.size(), 4u,
              "T4: exactly 4 rand_below calls -- one reroll (both-zero pair), then a pair with a single "
              "nonzero axis exits the loop (0x00485ffa JZ taken once, then not taken)");
        for (int32_t ub : g_rand_ub) ck_eq((uint32_t)ub, 5u, "T4: every draw's upper_bound == 5");
        const unit &u = fx.u(s.player, s.index);
        ck_eq((uint32_t)u.goal_x, (30u + 1u) & 0xffu, "T4: final dx=1 (second pair) is the one actually used");
        ck_eq((uint32_t)u.goal_y, (uint32_t)(20 & 0xff), "T4: final dy=0 (second pair) is the one actually used");
    }

    // =================================================================================================
    // T5 -- goal_x/goal_y torus-wrap masking (0x00485ffc-0x0048603e) and the UNCONDITIONAL home_x/
    // home_y <- current x/y stamp (0x0048603e-0x0048606c). T5a: a restrictive width_mask actually
    // clips a positive overflow (an unmasked translation would disagree). T5b: a negative dx exercises
    // the byte-level wraparound (x+dx computed then masked, matching the asm's 8-bit ADD-then-AND
    // idiom). T5c: a restrictive height_mask clips analogously on the y axis. home_x/home_y are
    // checked in all three as the CURRENT (pre-offset) x/y, never the goal.
    // =================================================================================================
    {
        Seed s; // T5a: positive overflow, width_mask bites
        s.order_queued = 0;
        s.proto_type   = 0;
        s.order        = 0x05;
        s.x = 30, s.y = 20;
        s.width_mask = 0x1f, s.height_mask = 0xff; // 0x1f actually clips 32; 0xff leaves y alone
        s.rand_ret = {4, 2};                       // dx=2, dy=0
        seed_and_run(fx, s);
        const unit &u = fx.u(s.player, s.index);
        ck_eq((uint32_t)u.goal_x, 0u,
              "T5a: goal_x = (30+2) & 0x1f = 32 & 0x1f = 0, NOT the unmasked 32 (0x0048600a-0x00486017)");
        ck_eq((uint32_t)u.home_x, 30u, "T5a: home_x = CURRENT x (30), unaffected by the goal offset/mask");
        ck_eq((uint32_t)u.home_y, 20u, "T5a: home_y = CURRENT y (20)");
    }
    {
        Seed s; // T5b: negative dx, byte-level wraparound with a full mask
        s.order_queued = 0;
        s.proto_type   = 0;
        s.order        = 0x05;
        s.x = 0, s.y = 20;
        s.width_mask = 0xff, s.height_mask = 0xff;
        s.rand_ret = {0, 2}; // dx = 0-2 = -2, dy = 0
        seed_and_run(fx, s);
        const unit &u = fx.u(s.player, s.index);
        ck_eq((uint32_t)u.goal_x, 254u,
              "T5b: goal_x = (uint32_t)(0 + -2) & 0xff = 0xFFFFFFFE & 0xff = 254 -- the 8-bit byte-wrap "
              "idiom (0x00485ffc-0x00486010), not a saturated/clamped negative");
        ck_eq((uint32_t)u.home_x, 0u, "T5b: home_x = CURRENT x (0), NOT the wrapped goal");
    }
    {
        Seed s; // T5c: height_mask bites analogously on the y axis
        s.order_queued = 0;
        s.proto_type   = 0;
        s.order        = 0x05;
        s.x = 30, s.y = 10;
        s.width_mask = 0xff, s.height_mask = 0x07; // 0x07 actually clips 8
        s.rand_ret = {2, 0};                       // dx=0, dy=0-2=-2
        seed_and_run(fx, s);
        const unit &u = fx.u(s.player, s.index);
        ck_eq((uint32_t)u.goal_y, 0u,
              "T5c: goal_y = (uint32_t)(10 + -2) & 0x07 = 8 & 0x07 = 0, NOT the unmasked 8 "
              "(0x0048602b-0x00486038)");
        ck_eq((uint32_t)u.home_y, 10u, "T5c: home_y = CURRENT y (10)");
    }

    // =================================================================================================
    // T6 -- LANDING_REQUEST override, BOTH conjunction operands true (order==0x29 AND
    // home_storage_slot!=0, 0x00486071-0x00486085): storage_get_approach_tile is called with EXACT
    // args (player, unit_index, storage_idx=0) (0x00486085-0x0048609b); its two out-params OVERRIDE
    // the scatter-computed goal (0x004860a0-0x004860b8); unit_set_state(move_op_arg) re-enters the
    // move-op state (0x004860be-0x004860da), NOT unit_set_state_order; the exact CALL ORDER is the
    // scatter draw FIRST (unconditional), then storage_get_approach_tile, then unit_set_state; home_x/
    // home_y and u.order itself are untouched by this arm.
    // =================================================================================================
    {
        Seed s;
        s.order_queued      = 0;
        s.proto_type        = 0;
        s.order             = IDLE_SCATTER_ORDER_LANDING_REQUEST; // 0x29
        s.home_storage_slot = 9;                                  // nonzero -- 2nd operand true
        s.x = 30, s.y = 20;
        s.width_mask = 0xff, s.height_mask = 0xff;
        s.rand_ret           = {4, 4}; // scatter goal would be (32,22) if NOT overridden
        s.move_op_arg        = 0x21;
        s.storage_out_fine_x = 111; // distinct from the (32,22) scatter goal above
        s.storage_out_fine_y = 222;
        seed_and_run(fx, s);
        ck(g_storage_calls.size() == 1 && g_storage_calls[0].player == s.player &&
               g_storage_calls[0].unit_index == (uint16_t)s.index && g_storage_calls[0].storage_idx == 0u,
           "T6: storage_get_approach_tile(cur_player, cur_index, storage_idx=0) -- exact args "
           "(0x0048608d MOVZX EDX,CUR_INDEX / 0x00486094 MOVZX EAX,CUR_PLAYER / 0x00486085 PUSH 0x0)");
        const unit &u = fx.u(s.player, s.index);
        ck_eq((uint32_t)u.goal_x, 111u,
              "T6: goal_x = out_fine_x = 111, OVERRIDING the scatter draw's 32 (0x004860a6-0x004860a9)");
        ck_eq((uint32_t)u.goal_y, 222u,
              "T6: goal_y = out_fine_y = 222, OVERRIDING the scatter draw's 22 (0x004860af-0x004860b8)");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == s.move_op_arg,
           "T6: unit_set_state(move_op_arg=0x21) fires -- re-enters the move-op state directly "
           "(0x004860be-0x004860da)");
        ck(g_set_state_order_calls.empty(), "T6: unit_set_state_order does NOT fire on this arm");
        ck(trace_eq({"rand_below", "rand_below", "storage_get_approach_tile", "unit_set_state"}),
           "T6: exact call order -- the scatter draw is UNCONDITIONAL and runs before the "
           "LANDING_REQUEST gate is even tested, then storage_get_approach_tile, then unit_set_state");
        ck((uint32_t)u.order == IDLE_SCATTER_ORDER_LANDING_REQUEST,
           "T6: u.order itself UNCHANGED (still LANDING_REQUEST) -- this function never rewrites Order");
        ck_eq((uint32_t)u.home_x, 30u, "T6: home_x still the CURRENT x, unaffected by the override");
        ck_eq((uint32_t)u.home_y, 20u, "T6: home_y still the CURRENT y, unaffected by the override");
    }

    // =================================================================================================
    // T7 -- LANDING_REQUEST override conjunction FALSE, EACH operand independently (both fall to the
    // plain-scatter ELSE arm, 0x004860dc-0x004860f7): T7a isolates home_storage_slot==0 (order IS
    // 0x29); T7b isolates order!=0x29 (home_storage_slot IS nonzero). Both assert
    // storage_get_approach_tile did NOT fire, unit_set_state_order fired with the EXACT args
    // (new_state=move_op_arg, new_order=PATROL_SWAP=0x10 -- per the __watcall EAX/EDX param mapping
    // at 0x004860e1-0x004860f7: EAX=move_op_arg is param 1, EDX=0x10 is param 2), and the
    // scatter-computed goal from step 4 is RETAINED (not overwritten by anything in this arm).
    // =================================================================================================
    {
        Seed s; // T7a: order IS LANDING_REQUEST, but home_storage_slot==0
        s.order_queued      = 0;
        s.proto_type        = 0;
        s.order             = IDLE_SCATTER_ORDER_LANDING_REQUEST;
        s.home_storage_slot = 0; // 2nd operand false
        s.x = 30, s.y = 20;
        s.width_mask = 0xff, s.height_mask = 0xff;
        s.rand_ret    = {4, 4}; // dx=2, dy=2
        s.move_op_arg = 0x21;
        seed_and_run(fx, s);
        ck(g_storage_calls.empty(), "T7a: home_storage_slot==0 -- storage_get_approach_tile does NOT fire (0x00486081 JNZ not taken)");
        ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].new_state == s.move_op_arg &&
               g_set_state_order_calls[0].new_order == UNIT_STATE_PATROL_SWAP,
           "T7a: unit_set_state_order(new_state=move_op_arg=0x21, new_order=PATROL_SWAP=0x10) "
           "(0x004860dc MOV EDX,0x10 / 0x004860f0 move_op_arg into EAX / 0x004860f7 CALL)");
        const unit &u = fx.u(s.player, s.index);
        ck_eq((uint32_t)u.goal_x, (30u + 2u) & 0xffu, "T7a: goal_x retains the SCATTER draw (32), not overridden");
        ck_eq((uint32_t)u.goal_y, (20u + 2u) & 0xffu, "T7a: goal_y retains the SCATTER draw (22), not overridden");
    }
    {
        Seed s; // T7b: order is NOT LANDING_REQUEST, but home_storage_slot IS nonzero
        s.order_queued      = 0;
        s.proto_type        = 0;
        s.order             = 0x05; // 1st operand false
        s.home_storage_slot = 9;
        s.x = 40, s.y = 50;
        s.width_mask = 0xff, s.height_mask = 0xff;
        s.rand_ret    = {3, 0}; // dx=1, dy=-2
        s.move_op_arg = 0x21;
        seed_and_run(fx, s);
        ck(g_storage_calls.empty(), "T7b: order!=0x29 -- storage_get_approach_tile does NOT fire (0x00486076 JNZ not taken)");
        ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].new_state == s.move_op_arg &&
               g_set_state_order_calls[0].new_order == UNIT_STATE_PATROL_SWAP,
           "T7b: same else-arm call, via the OTHER operand being false");
        const unit &u = fx.u(s.player, s.index);
        ck_eq((uint32_t)u.goal_x, (40u + 1u) & 0xffu, "T7b: goal_x retains the SCATTER draw");
        ck_eq((uint32_t)u.goal_y, (uint32_t)((50 - 2) & 0xff), "T7b: goal_y retains the SCATTER draw");
    }

    // =================================================================================================
    // T8 -- non-corruption: a guard unit this function never addresses (seeded with values that WOULD
    // trigger several of this function's own branches, incl. order_queued=1 and a live LANDING_REQUEST
    // dock slot) stays exactly as seeded across an unrelated run -- reusing T6's override scenario, the
    // most field-mutating case.
    // =================================================================================================
    {
        Seed s;
        s.order_queued      = 0;
        s.proto_type        = 0;
        s.order             = IDLE_SCATTER_ORDER_LANDING_REQUEST;
        s.home_storage_slot = 9;
        s.x = 30, s.y = 20;
        s.width_mask = 0xff, s.height_mask = 0xff;
        s.rand_ret           = {4, 4};
        s.storage_out_fine_x = 111;
        s.storage_out_fine_y = 222;
        seed_and_run(fx, s);

        const unit &g = fx.u(GUARD_PLAYER, GUARD_INDEX);
        ck(g.unit_proto_id == 66 && g.order_queued == 1 && g.order == IDLE_SCATTER_ORDER_LANDING_REQUEST &&
               g.home_storage_slot == 9,
           "T8: guard unit's dispatch-relevant fields untouched");
        ck(g.x == 12 && g.y == 34, "T8: guard unit's x/y untouched");
        ck(g.goal_x == 56 && g.goal_y == 78, "T8: guard unit's goal_x/goal_y untouched (would have become 111/222 if wrongly addressed)");
        ck(g.home_x == 90 && g.home_y == 11, "T8: guard unit's home_x/home_y untouched (would have become 30/20 if wrongly addressed)");
    }
}

} // namespace mh::sim::test
