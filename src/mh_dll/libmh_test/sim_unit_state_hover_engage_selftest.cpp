//
// sim_unit_state_hover_engage_selftest.cpp -- `simtest` oracle for
// llm_strat_unit_state_hover_engage @0x004807d9 (sim/sim_unit_state_hover_engage.h/.cpp, RI-SIM /
// SIM1-G1).
//
// SCOPE (honest, not exhaustive): the queued-order-peek gate as a CONJUNCTION of two independent
// operands (order_queued!=0 AND state==HOVER_ENGAGE, 0x004807f6/0x004807fa/0x00480801/0x00480806),
// both operands of the queued-order-record's own membership conjunction
// ((q.param0 in {ATTACK_BUILDING,ATTACK_UNIT,ATTACK_UNIT_RETURN}) AND (u.order in the SAME set),
// 0x0048083f-0x0048087e) tested so EACH operand independently gates, the queued-order
// apply-and-fall-through vs the facing-gated immediate-return-with-MOVE_PATH_12 vs the
// still-turning-park-in-2F-and-fall-through arms (0x00480880-0x004808e2), the tick-cost budget gate
// (stall/spend/exact-boundary, 0x0048090c-0x0048094d), the base-vs-mid-transition state dispatch
// (0x00480953-0x00480969), the base branch's OWN inner state==HOVER_ENGAGE gate on the elevation-XOR
// + hover_tile_crowded sidestep (0x00480a4f-0x00480b1e, full goal_x/goal_y/move_group_id field
// writes + call order), the common order-driven chase_check tail (0x00480b1e-0x00480b49), the
// mid-transition facing-turning gate (0x0048096f-0x0048099d), its own OR-condition MOVE_PATH_12
// shortcut with BOTH operands (order_queued!=0 OR state==HOVER_DISENGAGE, 0x004809a3-0x004809b8)
// tested independently, and the mid-transition order dispatch's three named arms + default
// (0x004809c9-0x00480a4a), plus neighbouring-slot non-corruption. It does NOT independently
// re-verify the Watcom `MOV AX,word[...]` upper-16-garbage-then-WORD-compare idiom at 0x004809ce
// beyond confirming the .cpp's `uint16_t pending_order` cast produces the same dispatch as the
// asm's word-sized CMPs (both are WORD comparisons, so the idiom is a non-issue here, already
// checked by inspection) -- and it does NOT probe order_queue_find_index's return-value truncation
// at values that would actually differ between the full int32 and the masked uint16 (all cases use
// small in-range indices, since nothing in the .asm's own logic depends on the truncation actually
// biting).
//
// sim_fixture leaves `sim_view::dir_remap_table` NULL (sim_test_support.h's own comment: "no
// offline case reads it ... verified via its live-view shadow arm, not offline" -- no longer true
// once this file's T7 exists). Same fix as sim_tile_neighbor_reverse_dir_selftest.cpp: sim_view is
// public and returned BY VALUE, so T7 builds its own local remap table and overrides that one field
// on its own copy of the view; dir_step_offsets is the shared fixture buffer (already bound), seeded
// by this file.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM tmp/decomp/llm_strat_unit_state_hover_engage_004807d9.asm --
// every assertion below cites the instruction address(es) it pins. NOT read off the .cpp body.
//
#include "sim/sim_unit_state_hover_engage.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- this TU's own local copies of `llm_strat_unit_state`/order-kind constants -----------------
// No backing Ghidra enum exists (same finding sim_unit_state_hover_engage.cpp's own banner
// documents) -- these are THIS file's local copies, value-identical to and cross-checked against
// the .cpp's anonymous-namespace constants (which have internal linkage and are not visible here),
// per the codebase's established per-TU-local-copy convention.
inline constexpr uint16_t UNIT_STATE_HOVER_ENGAGE    = 0x2e; // 0x004807f6/0x00480958/0x00480a54
inline constexpr uint16_t UNIT_STATE_HOVER_ENGAGE_2F = 0x2f; // 0x004808d8/0x00480958
inline constexpr uint16_t UNIT_STATE_HOVER_DISENGAGE = 0x37; // 0x00480964/0x004809b3
inline constexpr uint16_t UNIT_STATE_MOVE_PATH_12    = 0x12; // 0x004808c9/0x00480a23(EDX)/0x004809ba
inline constexpr uint16_t STATE_GROUP_STEP           = 0x0b; // 0x00480a23(EAX)/0x00480a34/0x00480b05
inline constexpr uint16_t STATE_ATTACK_UNIT          = 0x1a; // 0x0048083f.../0x004809ec/0x00480b23
inline constexpr uint16_t STATE_ATTACK_UNIT_RETURN   = 0x1b; // 0x0048084f.../0x004809f5/0x00480b2f
inline constexpr uint16_t STATE_ATTACK_BUILDING      = 0x1c; // 0x0048083f/0x004809e3/0x00480b3d
inline constexpr int32_t  ORDER_KIND_UNIT            = 0x80; // 0x0048080d

// ---- shared trace: one sequence proves CALL ORDER across all 6 callees -------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- per-callee recorders (6, one per unit_state_hover_engage_calls member) --------------------
struct FindIndexCall {
    int32_t player, unit_idx, kind_tag;
};
std::vector<FindIndexCall> g_find_index_calls;
int32_t                    g_find_index_ret = 0;
int32_t                    rec_order_queue_find_index(int32_t player, int32_t unit_idx, int32_t kind_tag) {
    tr("order_queue_find_index");
    g_find_index_calls.push_back({player, unit_idx, kind_tag});
    return g_find_index_ret;
}

struct ApplyDequeueCall {
    uint32_t player;
    int32_t  unit_idx, queue_idx;
};
std::vector<ApplyDequeueCall> g_apply_dequeue_calls;
void                          rec_order_queue_apply_and_dequeue(uint32_t player, int32_t unit_idx, int32_t queue_idx) {
    tr("order_queue_apply_and_dequeue");
    g_apply_dequeue_calls.push_back({player, unit_idx, queue_idx});
}

std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) {
    tr("unit_set_state");
    g_set_state_calls.push_back(new_state);
}

struct SetStateOrderCall {
    uint16_t new_state, new_order;
};
std::vector<SetStateOrderCall> g_set_state_order_calls;
void                           rec_unit_set_state_order(uint16_t new_state, uint16_t new_order) {
    tr("unit_set_state_order");
    g_set_state_order_calls.push_back({new_state, new_order});
}

struct CrowdedCall {
    int32_t  player;
    uint32_t unit_idx;
};
std::vector<CrowdedCall> g_crowded_calls;
int32_t                  g_crowded_ret = 0;
int32_t                  rec_unit_hover_tile_crowded(int32_t player, uint32_t unit_idx) {
    tr("unit_hover_tile_crowded");
    g_crowded_calls.push_back({player, unit_idx});
    return g_crowded_ret;
}

int32_t g_chase_check_calls = 0;
int32_t rec_unit_chase_check() {
    tr("unit_chase_check");
    ++g_chase_check_calls;
    return 0; // return value unused by the original (0x00480b44-0x00480b49)
}

const unit_state_hover_engage_calls g_calls = {
    &rec_order_queue_find_index,
    &rec_order_queue_apply_and_dequeue,
    &rec_unit_set_state,
    &rec_unit_set_state_order,
    &rec_unit_hover_tile_crowded,
    &rec_unit_chase_check,
};

void reset_observations() {
    g_trace.clear();
    g_find_index_calls.clear();
    g_apply_dequeue_calls.clear();
    g_set_state_calls.clear();
    g_set_state_order_calls.clear();
    g_crowded_calls.clear();
    g_chase_check_calls = 0;
}

// Fixed "guard" slot no test's own (player,index) ever touches -- seeded with sentinel nonzero data
// (including values that WOULD trigger several of this function's own branches, e.g. order_queued=1
// and state=HOVER_ENGAGE) each run so a wrong-index read/write is observable, not just a plain zero.
constexpr uint16_t GUARD_PLAYER = 5;
constexpr int32_t  GUARD_INDEX  = 8;

void seed_guard_slot(sim_fixture &fx) {
    unit &g          = fx.u(GUARD_PLAYER, GUARD_INDEX);
    g.unit_proto_id  = 77;
    g.order_queued   = 1;
    g.state          = UNIT_STATE_HOVER_ENGAGE;
    g.order          = STATE_ATTACK_UNIT;
    g.elevation      = 999;
    g.x              = 11;
    g.y              = 22;
    g.goal_x         = 33;
    g.goal_y         = 44;
    g.home_x         = 55;
    g.home_y         = 66;
    g.move_group_id  = 88888;
    g.activity_clock = 4242.0;
}

// A local remap table used only by T7 (the crowded-sidestep case) -- overrides sim_view's own NULL
// dir_remap_table on the RETURNED COPY of the view (sim_view is public, returned by value), same
// technique sim_tile_neighbor_reverse_dir_selftest.cpp already established. Sized to the fixture's
// real move_microsteps/move_heading domain (8 headings -- sim_test_support.h's own comment: "8
// headings is the dir8 domain").
std::vector<dir_remap_row> g_remap_table;

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player  = 0;
    int32_t  index   = 1;
    uint16_t cfg_row = 10;

    // Neutral defaults: order_queued=0 skips the queued-order peek entirely; state=MOVE_PATH_12
    // (0x12) is neither HOVER_ENGAGE(0x2e) nor HOVER_ENGAGE_2F/HOVER_DISENGAGE(0x2f/0x37), so it
    // reaches the base branch but its OWN inner "state==HOVER_ENGAGE" gate is false (no elevation
    // toggle, no crowded call); order=0x05 is not in the 3-member attack set, so the common tail
    // does not fire chase_check either. A test overrides only the fields its branch needs.
    uint8_t  order_queued = 0;
    uint16_t state        = UNIT_STATE_MOVE_PATH_12;
    uint16_t order        = 0x05;

    uint8_t facing_target    = 3;
    uint8_t move_heading     = 4;
    int32_t move_microstep   = 7;
    uint8_t microstep_facing = 9; // move_microsteps[heading*32+microstep].facing -- MISMATCH vs
                                  // facing_target by default (distinct sentinel values).

    int32_t elevation = 42; // sentinel, distinct from both 0 and 1 so a toggle is unambiguous
    uint8_t x = 60, y = 70;
    uint8_t goal_x = 80, goal_y = 90;
    uint8_t home_x = 100, home_y = 110;
    int32_t move_group_id = 12345; // sentinel nonzero, distinct from the 0 the sidestep writes

    double step_speed     = 2.0;   // cost = 2.0 * HOVER_STEP_COST_MULT(3.0) = 6.0
    double tick_budget    = 100.0; // plenty by default -- spend arm
    double activity_clock = 500.0; // sentinel, only ever written by the STALL arm

    int32_t order_find_index_ret = 7;    // the queue index the find_index stub returns
    int16_t order_queue_param0   = 0x05; // fx.order_queue[order_find_index_ret].param0, neutral

    int32_t crowded_ret = 0; // no sidestep by default

    bool     use_remap          = false;
    int32_t  remap_step_primary = 10;
    int32_t  step_dx = 0, step_dy = 0;
    uint32_t width_mask_override  = 0xff; // sim_fixture::reset()'s own default (kept unless a test
    uint32_t height_mask_override = 0x3f; // overrides it to make the mask actually bite, T7).
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();
    seed_guard_slot(fx);

    unit &u          = fx.u(s.player, s.index);
    u.unit_proto_id  = s.cfg_row;
    u.order_queued   = s.order_queued;
    u.state          = s.state;
    u.order          = s.order;
    u.facing_target  = s.facing_target;
    u.move_heading   = s.move_heading;
    u.move_microstep = s.move_microstep;
    u.elevation      = s.elevation;
    u.x              = s.x;
    u.y              = s.y;
    u.goal_x         = s.goal_x;
    u.goal_y         = s.goal_y;
    u.home_x         = s.home_x;
    u.home_y         = s.home_y;
    u.move_group_id  = s.move_group_id;
    u.activity_clock = s.activity_clock;

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = s.player;
    fx.view_cur_index  = (uint16_t)s.index;

    fx.cfg_units[s.cfg_row].step_speed[s.player] = s.step_speed;
    fx.tick_budget                               = s.tick_budget;

    fx.move_microsteps[(size_t)(s.move_heading * MICROSTEPS_PER_HEADING + s.move_microstep)].facing =
        s.microstep_facing;

    fx.order_queue[(size_t)s.order_find_index_ret].param0 = s.order_queue_param0;

    fx.geom.width_mask  = s.width_mask_override;
    fx.geom.height_mask = s.height_mask_override;

    if (s.use_remap) {
        g_remap_table.assign((size_t)8, dir_remap_row{});
        g_remap_table[(size_t)s.move_heading].step_primary   = s.remap_step_primary;
        g_remap_table[(size_t)s.move_heading].step_alt1      = -1; // sentinel -- not read by this fn
        g_remap_table[(size_t)s.move_heading].step_alt2      = -1;
        g_remap_table[(size_t)s.move_heading].step_alt3      = -1;
        fx.dir_step_offsets[(size_t)s.remap_step_primary].dx = s.step_dx;
        fx.dir_step_offsets[(size_t)s.remap_step_primary].dy = s.step_dy;
    }

    reset_observations();
    g_find_index_ret = s.order_find_index_ret;
    g_crowded_ret    = s.crowded_ret;

    sim_view v = fx.view();
    if (s.use_remap) v.dir_remap_table = g_remap_table.data();

    sim_store own = fx.store();
    detail::unit_state_hover_engage(v, own, g_calls);
}

} // namespace

void run_unit_state_hover_engage_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- the queued-order-peek GATE (0x004807f1-0x00480806) is a conjunction of TWO independent
    // operands (order_queued!=0 AND state==HOVER_ENGAGE); each must independently fail to skip the
    // peek. T1a proves order_queued==0 skips it (state IS HOVER_ENGAGE, so the base branch's own
    // elevation/crowded gate then runs on its own merits -- proves the SKIP, not a side effect of an
    // unrelated state). T1b/T1c prove state!=HOVER_ENGAGE skips it even with order_queued==1, AND
    // (crucially) that state=2F/37 route to the MID-TRANSITION branch rather than the base branch --
    // a poison `order` value (in the 3-member attack set) plus a facing MISMATCH means: correct
    // routing -> mid-transition's facing gate returns immediately with ZERO calls; a mis-route to the
    // base branch's common tail would fire chase_check. Empty trace is the pin.
    // =================================================================================================
    {
        Seed s;
        s.order_queued = 0;
        s.state        = UNIT_STATE_HOVER_ENGAGE; // gate's 2nd operand true, but 1st is false
        s.crowded_ret  = 0;
        seed_and_run(fx, s);
        ck(g_find_index_calls.empty(), "T1a: order_queued==0 -- queued-order peek does NOT run (0x004807fa JZ taken)");
        ck(trace_eq({"unit_hover_tile_crowded"}),
           "T1a: falls straight through to the budget tick; state==HOVER_ENGAGE still fires its OWN "
           "elevation/crowded gate on its own merits (0x00480a4f-0x00480a87)");
        ck_eq((uint32_t)fx.u(s.player, s.index).elevation, (uint32_t)(s.elevation ^ 1),
              "T1a: elevation ^= 1 (only low bit XORed in the asm, bit-equivalent to int32 ^=1) (0x00480a64-0x00480a6f)");
    }
    {
        Seed s;
        s.order_queued     = 1;
        s.state            = UNIT_STATE_HOVER_ENGAGE_2F; // gate's 1st operand true, 2nd false (!=0x2e)
        s.order            = STATE_ATTACK_UNIT;          // poison: would fire chase_check if mis-routed
        s.facing_target    = 1;
        s.microstep_facing = 2; // MISMATCH -- mid-transition's own facing gate must return early
        seed_and_run(fx, s);
        ck(g_find_index_calls.empty(), "T1b: state==0x2f (!=HOVER_ENGAGE) -- queued-order peek does NOT run (0x00480806 JZ not taken)");
        ck(g_trace.empty(),
           "T1b: state=0x2f correctly routes to the MID-TRANSITION branch (0x0048096f), whose facing "
           "gate (0x0048099d JNZ) returns immediately -- a base-branch mis-route would have fired "
           "chase_check on the poison ATTACK_UNIT order");
    }
    {
        Seed s;
        s.order_queued     = 1;
        s.state            = UNIT_STATE_HOVER_DISENGAGE; // 2nd operand false via the OTHER non-0x2e value
        s.order            = STATE_ATTACK_BUILDING;      // poison
        s.facing_target    = 1;
        s.microstep_facing = 2; // MISMATCH
        seed_and_run(fx, s);
        ck(g_find_index_calls.empty(), "T1c: state==0x37 (!=HOVER_ENGAGE) -- queued-order peek does NOT run");
        ck(g_trace.empty(), "T1c: state=0x37 also routes to MID-TRANSITION, same facing-gate early return, zero calls");
    }

    // =================================================================================================
    // T2 -- queued-order peek TAKEN (order_queued=1, state=HOVER_ENGAGE): find_index called with
    // (player, index, kind=ORDER_KIND_UNIT=0x80) (0x0048080d-0x00480825); the conjunction of
    // q.param0 AND u.order both in {ATTACK_BUILDING,ATTACK_UNIT,ATTACK_UNIT_RETURN} is TRUE (using
    // TWO DIFFERENT set members to prove it is a real membership test, not equality) ->
    // apply_and_dequeue(player, index, queue_idx) fires (0x00480880-0x00480897) and falls through
    // (does NOT return) to the budget-gated tick -- pinned here via a deliberately-insufficient
    // budget so the trace stops cleanly right after the two calls (T4 below separately proves the
    // fallthrough actually reaches the base branch's code).
    // =================================================================================================
    {
        Seed s;
        s.order_queued         = 1;
        s.state                = UNIT_STATE_HOVER_ENGAGE;
        s.order_queue_param0   = STATE_ATTACK_BUILDING; // member #1
        s.order                = STATE_ATTACK_UNIT;     // member #2 (distinct)
        s.order_find_index_ret = 7;
        s.step_speed           = 2.0; // cost = 6.0
        s.tick_budget          = 2.0; // insufficient -> stall right after, isolating this trace
        seed_and_run(fx, s);
        ck(g_find_index_calls.size() == 1 && g_find_index_calls[0].player == s.player &&
               g_find_index_calls[0].unit_idx == s.index && g_find_index_calls[0].kind_tag == ORDER_KIND_UNIT,
           "T2: order_queue_find_index(cur_player, cur_index, kind=0x80) (0x0048080d-0x00480825)");
        ck(g_apply_dequeue_calls.size() == 1 && g_apply_dequeue_calls[0].player == s.player &&
               g_apply_dequeue_calls[0].unit_idx == s.index &&
               g_apply_dequeue_calls[0].queue_idx == s.order_find_index_ret,
           "T2: order_queue_apply_and_dequeue(cur_player, cur_index, queue_idx) (0x00480880-0x00480897)");
        ck(trace_eq({"order_queue_find_index", "order_queue_apply_and_dequeue"}),
           "T2: exact call order -- find_index THEN apply_and_dequeue, then falls into the (stalled) budget tick");
        ck_eq_d(fx.u(s.player, s.index).activity_clock, s.activity_clock - s.tick_budget,
                "T2: fallthrough reached the STALL arm -- activity_clock -= tick_budget (0x00480925-0x00480928)");
        ck_eq_d(fx.tick_budget, 0.0, "T2: fallthrough reached the STALL arm -- tick_budget zeroed (0x0048092b-0x0048093f)");
    }

    // =================================================================================================
    // T3 -- conjunction FALSE, EACH operand independently: T3a via q.param0 NOT in the set (u.order
    // IS, to isolate), T3b via u.order NOT in the set (q.param0 IS). Both then take the facing-MATCH
    // sub-arm: unit_set_state(MOVE_PATH_12=0x12) fires and the function returns IMMEDIATELY --
    // skipping the budget-gated tick entirely this call (own.tick_budget()/activity_clock UNCHANGED
    // is the pin that the early return is real, not just "no further branch happened to fire").
    // =================================================================================================
    {
        Seed s;
        s.order_queued       = 1;
        s.state              = UNIT_STATE_HOVER_ENGAGE;
        s.order_queue_param0 = 0x05;              // NOT in the set
        s.order              = STATE_ATTACK_UNIT; // IS in the set -- isolates q.param0 as the failing operand
        s.facing_target      = 6;
        s.microstep_facing   = 6; // MATCH
        s.tick_budget        = 100.0;
        s.activity_clock     = 500.0;
        seed_and_run(fx, s);
        ck(g_apply_dequeue_calls.empty(), "T3a: q.param0 not in the set -- apply_and_dequeue does NOT fire (conjunction false)");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_MOVE_PATH_12,
           "T3a: unit_set_state(MOVE_PATH_12=0x12) (0x004808c9-0x004808ce)");
        ck(trace_eq({"order_queue_find_index", "unit_set_state"}), "T3a: exact call order, immediate return");
        ck_eq_d(fx.tick_budget, s.tick_budget, "T3a: immediate return -- tick_budget UNCHANGED (budget code never runs)");
        ck_eq_d(fx.u(s.player, s.index).activity_clock, s.activity_clock, "T3a: immediate return -- activity_clock UNCHANGED");
    }
    {
        Seed s;
        s.order_queued       = 1;
        s.state              = UNIT_STATE_HOVER_ENGAGE;
        s.order_queue_param0 = STATE_ATTACK_BUILDING; // IS in the set -- isolates u.order as the failing operand
        s.order              = 0x05;                  // NOT in the set
        s.facing_target      = 6;
        s.microstep_facing   = 6; // MATCH
        seed_and_run(fx, s);
        ck(g_apply_dequeue_calls.empty(), "T3b: u.order not in the set -- apply_and_dequeue does NOT fire (conjunction false)");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_MOVE_PATH_12,
           "T3b: unit_set_state(MOVE_PATH_12=0x12), same as T3a via the OTHER operand");
    }

    // =================================================================================================
    // T4 -- conjunction FALSE, facing MISMATCH sub-arm: unit_set_state(HOVER_ENGAGE_2F=0x2f) fires
    // (0x004808d8-0x004808e2) and FALLS THROUGH (no return) -- proven here by continuing all the way
    // into the base branch's own elevation toggle (since the set_state call is a stub and does not
    // actually mutate u.state, the fixture's state field is still HOVER_ENGAGE=0x2e from the gate
    // precondition, so the base branch's inner "state==HOVER_ENGAGE" gate legitimately fires again --
    // an expected, honest consequence of stubbing an outward call whose real effect is out of scope;
    // same posture sim_unit_state_die_explode_selftest.cpp's T10 documents for a cached-vs-mutated
    // read).
    // =================================================================================================
    {
        Seed s;
        s.order_queued       = 1;
        s.state              = UNIT_STATE_HOVER_ENGAGE;
        s.order_queue_param0 = 0x05; // neither operand matches -- simplest false conjunction
        s.order              = 0x05;
        s.facing_target      = 1;
        s.microstep_facing   = 2; // MISMATCH
        s.step_speed         = 2.0;
        s.tick_budget        = 100.0; // plenty -- spend arm
        s.crowded_ret        = 0;
        seed_and_run(fx, s);
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_HOVER_ENGAGE_2F,
           "T4: unit_set_state(HOVER_ENGAGE_2F=0x2f) (0x004808d8-0x004808dd)");
        ck(trace_eq({"order_queue_find_index", "unit_set_state", "unit_hover_tile_crowded"}),
           "T4: falls through past the (stubbed) set_state call into the budget-spend arm, then the "
           "base branch's own elevation/crowded gate (state field still reads 0x2e)");
        ck_eq_d(fx.tick_budget, s.tick_budget - s.step_speed * 3.0,
                "T4: fallthrough reached the SPEND arm -- tick_budget -= cost (0x00480944-0x0048094d)");
        ck_eq((uint32_t)fx.u(s.player, s.index).elevation, (uint32_t)(s.elevation ^ 1),
              "T4: fallthrough reached the base branch's elevation XOR (0x00480a64-0x00480a6f)");
    }

    // =================================================================================================
    // T5 -- the tick-cost BUDGET gate (0x0048090c-0x0048094d): cost = step_speed[player] * 3.0
    // (HOVER_STEP_COST_MULT, DAT_00501410 @0x00480903). Peek skipped (order_queued=0), state neutral
    // (MOVE_PATH_12) so the post-budget dispatch is a clean no-op tail either way -- isolates the
    // budget arithmetic. T5a: budget < cost -> STALL. T5b: budget == cost EXACTLY -> SPEND (JNC is
    // "not carry", i.e. NOT(budget<cost), so equality takes the spend arm). T5c: budget > cost -> SPEND.
    // =================================================================================================
    {
        Seed s;
        s.order_queued   = 0;
        s.state          = UNIT_STATE_MOVE_PATH_12; // neutral: base branch, elevation-if false, tail no-op
        s.step_speed     = 2.0;                     // cost = 6.0
        s.tick_budget    = 5.0;                     // < cost -> stall
        s.activity_clock = 500.0;
        seed_and_run(fx, s);
        ck(g_trace.empty(), "T5a: neutral state/order -- no calls either way, isolating pure budget arithmetic");
        ck_eq_d(fx.u(s.player, s.index).activity_clock, s.activity_clock - s.tick_budget,
                "T5a: budget(5.0) < cost(6.0) -- STALL: activity_clock -= tick_budget (0x00480925-0x00480928)");
        ck_eq_d(fx.tick_budget, 0.0, "T5a: STALL -- tick_budget zeroed (0x0048092b-0x0048093f)");

        s.tick_budget = 6.0; // == cost exactly
        seed_and_run(fx, s);
        ck_eq_d(fx.tick_budget, 0.0, "T5b: budget(6.0) == cost(6.0) EXACTLY -- SPEND arm taken (JNC @0x00480918), tick_budget -> 0");
        ck_eq_d(fx.u(s.player, s.index).activity_clock, s.activity_clock,
                "T5b: SPEND arm -- activity_clock UNCHANGED (only the STALL arm ever writes it)");

        s.tick_budget = 10.0; // > cost
        seed_and_run(fx, s);
        ck_eq_d(fx.tick_budget, 10.0 - 6.0, "T5c: budget(10.0) > cost(6.0) -- SPEND: tick_budget -= cost (0x00480944-0x0048094d)");
    }

    // =================================================================================================
    // T6 -- base branch, state==HOVER_ENGAGE(0x2e), crowded_ret=0 (no sidestep): elevation still
    // toggles (crowded() is called UNCONDITIONALLY whenever state==HOVER_ENGAGE, gated only by the
    // sidestep's OWN `if (crowded != 0)`, 0x00480a72-0x00480a87), and the common order-driven tail
    // (0x00480b1e-0x00480b49) fires chase_check only when u.order is in the 3-member attack set.
    // =================================================================================================
    {
        Seed s;
        s.order_queued = 0;
        s.state        = UNIT_STATE_HOVER_ENGAGE;
        s.crowded_ret  = 0;
        s.order        = 0x05; // NOT in the attack set
        seed_and_run(fx, s);
        ck(trace_eq({"unit_hover_tile_crowded"}), "T6a: crowded() called, no sidestep (ret=0), tail order does not match -- no chase_check");
        ck(g_set_state_calls.empty(), "T6a: no GROUP_STEP set_state (sidestep gated off)");
        ck_eq((uint32_t)fx.u(s.player, s.index).move_group_id, (uint32_t)s.move_group_id, "T6a: move_group_id UNCHANGED (sidestep did not run)");

        s.order = STATE_ATTACK_BUILDING; // now IS in the attack set
        seed_and_run(fx, s);
        ck(trace_eq({"unit_hover_tile_crowded", "unit_chase_check"}),
           "T6b: same as T6a but order in the attack set -- common tail fires chase_check (0x00480b44)");
        ck_eq(g_chase_check_calls, 1, "T6b: unit_chase_check called exactly once");
    }

    // =================================================================================================
    // T7 -- base branch, state==HOVER_ENGAGE(0x2e), crowded_ret!=0: the sidestep (0x00480a8d-
    // 0x00480b1e). step_primary = dir_remap_table[move_heading].step_primary; step =
    // dir_step_offsets[step_primary]; goal_x = (uint8_t)(x + (uint8_t)dx) & width_mask (byte-level
    // ADD/AND, 0x00480ac3-0x00480ade); goal_y analogous with height_mask (0x00480ae4-0x00480aff);
    // unit_set_state(GROUP_STEP=0xb) (0x00480b05-0x00480b0a); move_group_id=0 (0x00480b0f-0x00480b1e)
    // -- AFTER the set_state call, per the asm's own order. Masks overridden to values SMALL ENOUGH
    // that they actually clip the raw sum (a translation that forgot the mask would disagree).
    // T7b additionally sets an attack order to prove the common tail STILL fires after the sidestep.
    // =================================================================================================
    {
        Seed s;
        s.order_queued         = 0;
        s.state                = UNIT_STATE_HOVER_ENGAGE;
        s.crowded_ret          = 1;    // nonzero -> sidestep
        s.order                = 0x05; // isolate: no tail chase_check yet
        s.move_heading         = 2;
        s.use_remap            = true;
        s.remap_step_primary   = 10;
        s.step_dx              = 5;  // x=195 + 5 = 200 (0xC8) -- mask must actually clip this
        s.step_dy              = 30; // y=100 + 30 = 130 (0x82) -- mask must actually clip this
        s.x                    = 195;
        s.y                    = 100;
        s.width_mask_override  = 0x1f;  // 200 & 0x1f = 8   (200 unmasked would NOT equal 8)
        s.height_mask_override = 0x0f;  // 130 & 0x0f = 2   (130 unmasked would NOT equal 2)
        s.move_group_id        = 12345; // sentinel, must become 0
        seed_and_run(fx, s);
        const unit &u = fx.u(s.player, s.index);
        ck_eq((uint32_t)u.goal_x, 8u, "T7a: goal_x = (uint8_t)(x + dx) & width_mask = (195+5)&0x1f = 200&0x1f = 8 (0x00480ac3-0x00480ade)");
        ck_eq((uint32_t)u.goal_y, 2u, "T7a: goal_y = (uint8_t)(y + dy) & height_mask = (100+30)&0x0f = 130&0x0f = 2 (0x00480ae4-0x00480aff)");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == STATE_GROUP_STEP,
           "T7a: unit_set_state(GROUP_STEP=0xb) (0x00480b05-0x00480b0a)");
        ck_eq((uint32_t)u.move_group_id, 0u, "T7a: move_group_id = 0, AFTER the set_state call (0x00480b0f-0x00480b1e)");
        ck(trace_eq({"unit_hover_tile_crowded", "unit_set_state"}), "T7a: exact call order -- crowded THEN set_state(GROUP_STEP)");

        s.order = STATE_ATTACK_UNIT; // now the common tail also fires
        seed_and_run(fx, s);
        ck(trace_eq({"unit_hover_tile_crowded", "unit_set_state", "unit_chase_check"}),
           "T7b: sidestep's set_state(GROUP_STEP) THEN the common tail's chase_check, same call as T7a plus the tail");
    }

    // =================================================================================================
    // T8 -- base branch, state NOT in {HOVER_ENGAGE, HOVER_ENGAGE_2F, HOVER_DISENGAGE}
    // (e.g. MOVE_PATH_12=0x12): the elevation/crowded gate is entirely skipped (its OWN inner
    // "state==HOVER_ENGAGE" check fails, 0x00480a54-0x00480a59 JNZ), even with crowded_ret!=0 primed
    // (proves crowded() is genuinely NOT called, not just that its result was ignored) -- while the
    // common order-driven tail still runs unconditionally.
    // =================================================================================================
    {
        Seed s;
        s.order_queued = 0;
        s.state        = UNIT_STATE_MOVE_PATH_12; // != 0x2e/0x2f/0x37
        s.crowded_ret  = 1;                       // primed nonzero -- must NOT matter
        s.elevation    = 42;
        s.order        = STATE_ATTACK_UNIT; // tail should still fire
        seed_and_run(fx, s);
        ck(g_crowded_calls.empty(), "T8: state!=HOVER_ENGAGE -- unit_hover_tile_crowded NOT called at all (0x00480a59 JNZ taken)");
        ck_eq((uint32_t)fx.u(s.player, s.index).elevation, (uint32_t)s.elevation, "T8: elevation UNCHANGED (inner gate never entered)");
        ck(trace_eq({"unit_chase_check"}), "T8: the common tail still runs independently of the elevation/crowded gate");
    }

    // =================================================================================================
    // T9 -- mid-transition branch ENTRY (state==HOVER_ENGAGE_2F or HOVER_DISENGAGE both route here,
    // 0x00480958/0x00480964) with a facing MISMATCH: the facing gate (0x0048096f-0x0048099d) returns
    // immediately, ZERO calls -- a poison attack-set `order` proves this isn't a base-branch
    // mis-route (which would fire chase_check via the common tail).
    // =================================================================================================
    {
        Seed s;
        s.order_queued     = 0;
        s.state            = UNIT_STATE_HOVER_ENGAGE_2F;
        s.order            = STATE_ATTACK_UNIT; // poison
        s.facing_target    = 4;
        s.microstep_facing = 5; // MISMATCH
        seed_and_run(fx, s);
        ck(g_trace.empty(), "T9a: state=0x2f, facing mismatch -- immediate return, zero calls (0x0048099d JNZ taken)");

        s.state = UNIT_STATE_HOVER_DISENGAGE;
        s.order = STATE_ATTACK_BUILDING; // poison
        seed_and_run(fx, s);
        ck(g_trace.empty(), "T9b: state=0x37, facing mismatch -- same immediate return, zero calls");
    }

    // =================================================================================================
    // T10 -- mid-transition, facing MATCH: the MOVE_PATH_12 shortcut is an OR of two operands
    // (order_queued!=0 OR state==HOVER_DISENGAGE, 0x004809a3-0x004809b8), each tested independently.
    // T10c proves that when BOTH operands are false, the shortcut is NOT taken and control falls to
    // the order-dispatch tail instead (distinguished by which unit_set_state VALUE fires).
    // =================================================================================================
    {
        Seed s;
        s.order_queued     = 1; // 1st operand true
        s.state            = UNIT_STATE_HOVER_ENGAGE_2F;
        s.facing_target    = 8;
        s.microstep_facing = 8; // MATCH
        seed_and_run(fx, s);
        ck(trace_eq({"unit_set_state"}) && g_set_state_calls[0] == UNIT_STATE_MOVE_PATH_12,
           "T10a: order_queued!=0 -- unit_set_state(MOVE_PATH_12=0x12) shortcut fires (0x004809ac JNZ taken)");

        s.order_queued = 0;
        s.state        = UNIT_STATE_HOVER_DISENGAGE; // 2nd operand true (state==0x37 itself)
        seed_and_run(fx, s);
        ck(trace_eq({"unit_set_state"}) && g_set_state_calls[0] == UNIT_STATE_MOVE_PATH_12,
           "T10b: order_queued==0 but state==HOVER_DISENGAGE -- same shortcut via the OTHER operand (0x004809b8 JNZ not taken)");

        s.order_queued = 0;
        s.state        = UNIT_STATE_HOVER_ENGAGE_2F; // BOTH operands false
        s.order        = 0x05;                       // default arm of the order-dispatch tail
        seed_and_run(fx, s);
        ck(trace_eq({"unit_set_state"}) && g_set_state_calls[0] == UNIT_STATE_HOVER_ENGAGE,
           "T10c: both OR operands false -- falls to the order-dispatch tail's DEFAULT arm, "
           "unit_set_state(HOVER_ENGAGE=0x2e) (0x00480a40-0x00480a45), NOT the 0x12 shortcut");
    }

    // =================================================================================================
    // T11 -- mid-transition order-dispatch tail (reached only when BOTH T10 OR-operands are false):
    // dispatch on u.order. STATE_ATTACK_UNIT_RETURN(0x1b) copies home_x/home_y INTO goal_x/goal_y and
    // calls unit_set_state_order(GROUP_STEP=0xb, MOVE_PATH_12=0x12) (0x004809f5-0x00480a2d);
    // STATE_ATTACK_UNIT(0x1a) and STATE_ATTACK_BUILDING(0x1c) both call plain
    // unit_set_state(GROUP_STEP=0xb) WITHOUT touching goal_x/goal_y (0x004809ec/0x004809e3-0x00480a39).
    // (The DEFAULT arm is already pinned by T10c above.)
    // =================================================================================================
    {
        Seed s;
        s.order_queued     = 0;
        s.state            = UNIT_STATE_HOVER_ENGAGE_2F;
        s.facing_target    = 2;
        s.microstep_facing = 2; // MATCH
        s.order            = STATE_ATTACK_UNIT_RETURN;
        s.home_x           = 100;
        s.home_y           = 110;
        s.goal_x           = 80;
        s.goal_y           = 90; // distinct from home_x/home_y so the copy is observable
        seed_and_run(fx, s);
        const unit &u1 = fx.u(s.player, s.index);
        ck_eq((uint32_t)u1.goal_x, (uint32_t)s.home_x, "T11a: STATE_ATTACK_UNIT_RETURN -- goal_x = home_x (0x00480a00-0x00480a06)");
        ck_eq((uint32_t)u1.goal_y, (uint32_t)s.home_y, "T11a: goal_y = home_y (0x00480a0c-0x00480a1d)");
        ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].new_state == STATE_GROUP_STEP &&
               g_set_state_order_calls[0].new_order == UNIT_STATE_MOVE_PATH_12,
           "T11a: unit_set_state_order(new_state=GROUP_STEP=0xb, new_order=MOVE_PATH_12=0x12) (0x00480a23-0x00480a2d)");
        ck(g_set_state_calls.empty(), "T11a: plain unit_set_state does NOT fire on this arm");

        s.order = STATE_ATTACK_UNIT;
        seed_and_run(fx, s);
        const unit &u2 = fx.u(s.player, s.index);
        ck(trace_eq({"unit_set_state"}) && g_set_state_calls[0] == STATE_GROUP_STEP,
           "T11b: STATE_ATTACK_UNIT -- unit_set_state(GROUP_STEP=0xb) (0x004809ec-0x00480a39)");
        ck_eq((uint32_t)u2.goal_x, (uint32_t)s.goal_x, "T11b: goal_x UNCHANGED (no home-copy on this arm)");
        ck_eq((uint32_t)u2.goal_y, (uint32_t)s.goal_y, "T11b: goal_y UNCHANGED");

        s.order = STATE_ATTACK_BUILDING;
        seed_and_run(fx, s);
        ck(trace_eq({"unit_set_state"}) && g_set_state_calls[0] == STATE_GROUP_STEP,
           "T11c: STATE_ATTACK_BUILDING -- same unit_set_state(GROUP_STEP=0xb) via the OTHER membership check (0x004809e3-0x00480a39)");
    }

    // =================================================================================================
    // T12 -- neighbouring-slot non-corruption: a guard unit this function never addresses (seeded
    // with values that WOULD trigger several of this function's own branches, incl. order_queued=1
    // and state=HOVER_ENGAGE) stays exactly as seeded across an unrelated run.
    // =================================================================================================
    {
        Seed s; // reuse T7's sidestep scenario -- the most field-mutating case
        s.order_queued         = 0;
        s.state                = UNIT_STATE_HOVER_ENGAGE;
        s.crowded_ret          = 1;
        s.move_heading         = 2;
        s.use_remap            = true;
        s.remap_step_primary   = 10;
        s.step_dx              = 5;
        s.step_dy              = 30;
        s.x                    = 195;
        s.y                    = 100;
        s.width_mask_override  = 0x1f;
        s.height_mask_override = 0x0f;
        seed_and_run(fx, s);

        const unit &g = fx.u(GUARD_PLAYER, GUARD_INDEX);
        ck(g.unit_proto_id == 77 && g.order_queued == 1 && g.state == UNIT_STATE_HOVER_ENGAGE && g.order == STATE_ATTACK_UNIT,
           "T12: guard unit's dispatch-relevant fields untouched");
        ck(g.elevation == 999, "T12: guard unit's elevation untouched (would have toggled if wrongly addressed)");
        ck(g.x == 11 && g.y == 22 && g.goal_x == 33 && g.goal_y == 44 && g.home_x == 55 && g.home_y == 66,
           "T12: guard unit's position/goal/home fields untouched");
        ck(g.move_group_id == 88888, "T12: guard unit's move_group_id untouched (would have become 0 if wrongly addressed)");
        ck_eq_d(g.activity_clock, 4242.0, "T12: guard unit's activity_clock untouched");
    }
}

} // namespace mh::sim::test
