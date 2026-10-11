//
// sim_unit_move_path_air_facing_selftest.cpp -- `simtest` cases for patching:PT-FIX3
// ([net] air_order_facing_fix) in detail::unit_state_move_path (sim/sim_unit_state_move_path.cpp).
//
// The deadlock (RE-MISC10): llm_strat_order_queue_dispatch defers an air unit's order while
// facing_target != MOVE_MICROSTEPS[heading][microstep].facing and sets order_queued; the move_path
// state returns early at microstep 31 && order_queued without reconciling the two, and
// update_rotation never rotates a non-independent unit in state 0x12 -- so the order is deferred
// forever. The fix snaps facing_target/facing_current to the table facing in that early return.
//
// Driven with the REAL dispatcher and the REAL move_path, one pair per game step, so "applied within
// a bounded number of steps" is observed, not inferred: the dispatcher's own fingerprint for an
// applied unit order is llm_unit_set_order_param, and a deferred one stays in the queue.
//
#include "sim/sim_order_dispatch.h"
#include "sim/sim_unit_state_move_path.h"

#include "sim_dispatch_calls.gen.h"
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;
using rec::dc;

constexpr int32_t  PLAYER   = 3;
constexpr int32_t  OBJ      = 5;
constexpr uint16_t PROTO    = 7;
constexpr uint8_t  HEADING  = 2;
constexpr uint8_t  TBL_FACE = 9;  // MOVE_MICROSTEPS[HEADING][31].facing
constexpr uint8_t  STALE    = 17; // the facing the unit is stuck at
constexpr int      BOUND    = 3;  // steps in which the fixed arm must have applied the order
constexpr int      STEPS    = 12; // steps run for the unfixed arm

int32_t  chase_noop() { return 0; }
void     path_free_noop(uint16_t, int32_t) {}
void     notify_noop(uint32_t, int32_t, uint32_t) {}
void     set_state_noop(uint16_t) {}
void     set_state_order_noop(uint16_t, uint16_t) {}
int32_t  target_class_noop(uint32_t, int32_t) { return 0; }
uint32_t in_range_noop(int32_t, int32_t, int32_t, int32_t, int32_t) { return 0; }
void     approach_noop(uint16_t, uint16_t, uint32_t *, uint32_t *, uint32_t) {}
void     unlink_noop(uint32_t, uint16_t) {}
void     fow_remove_noop(uint32_t, int32_t, int32_t, uint8_t) {}
void     put_noop(uint16_t, uint16_t, uint8_t, uint8_t) {}
void     fow_update_noop(uint32_t, uint32_t, uint32_t, uint8_t) {}

const unit_state_move_path_calls g_mp_calls = {&chase_noop,
                                               &path_free_noop,
                                               &notify_noop,
                                               &set_state_noop,
                                               &set_state_order_noop,
                                               &target_class_noop,
                                               &in_range_noop,
                                               &approach_noop,
                                               &unlink_noop,
                                               &fow_remove_noop,
                                               &put_noop,
                                               &fow_update_noop};

order make_record() {
    order q{};
    q.owner_and_kind = (uint16_t)(ORDER_KIND_UNIT_EX | (uint32_t)PLAYER);
    q.unit_index     = (uint16_t)OBJ;
    q.param0         = 0;
    q.order_code     = 0xe7;
    for (int32_t &a : q.args) a = -1;
    return q;
}

struct outcome {
    int     applied_at; // 1-based step at which the dispatcher applied the order, 0 = never
    int     queue_left; // records still queued after the last step
    uint8_t order_queued;
    uint8_t facing_target;
    uint8_t facing_current;
    int32_t microstep;
    double  budget;
};

// One unit stuck in state 0x12 at microstep 31 with order_queued, one queued unit order. Each step: the
// dispatcher, then (while the order is still queued) the unit's move_path tick.
outcome run(sim_fixture &f, bool fix, uint32_t type, uint8_t facing_target, uint8_t facing_current, int steps) {
    set_air_order_facing_fix(fix);
    f.reset();
    f.cfg_units[PROTO].type                                         = type;
    f.move_microsteps[HEADING * MICROSTEPS_PER_HEADING + 31].facing = TBL_FACE;
    f.cfg_units[PROTO].step_speed[PLAYER]                           = 1.0;

    unit &u          = f.u(PLAYER, OBJ);
    u                = unit{};
    u.energy         = 1.0;
    u.unit_proto_id  = PROTO;
    u.state          = 0x12;
    u.order          = 0x12;
    u.move_microstep = 31;
    u.move_heading   = HEADING;
    u.order_queued   = 1;
    u.facing_target  = facing_target;
    u.facing_current = facing_current;

    f.order_queue[0]    = make_record();
    f.order_queue_count = 1;
    f.cur_unit_ptr      = &u;
    f.view_cur_player   = (uint16_t)PLAYER;
    f.view_cur_index    = (uint16_t)OBJ;

    outcome o{};
    for (int s = 1; s <= steps; ++s) {
        {
            sim_view  v   = f.view();
            sim_store own = f.store();
            dc().reset();
            dc().ret[rec::DC_llm_unit_state_is_boarding] = 0;
            dispatch_calls c                             = rec::recording_calls();
            detail::order_queue_dispatch(v, own, c);
            if (o.applied_at == 0 && dc().count(rec::DC_llm_unit_set_order_param) > 0) o.applied_at = s;
        }
        if (o.applied_at) break; // the order is consumed; the unit is no longer stuck
        f.tick_budget = 55.0;
        sim_store own = f.store();
        detail::unit_state_move_path(f.view(), own, g_mp_calls);
        o.budget = f.tick_budget;
    }
    o.queue_left     = f.order_queue_count;
    o.order_queued   = f.u(PLAYER, OBJ).order_queued;
    o.facing_target  = f.u(PLAYER, OBJ).facing_target;
    o.facing_current = f.u(PLAYER, OBJ).facing_current;
    o.microstep      = f.u(PLAYER, OBJ).move_microstep;
    set_air_order_facing_fix(false);
    return o;
}

} // namespace

void run_unit_move_path_air_facing_tests() {
    sim_fixture f;

    // (1) THE DEADLOCK: a helicopter (type 0x0f) at ms 31, order queued, facing_target stale.
    outcome off = run(f, false, 0x0f, STALE, STALE, STEPS);
    ck_eq(off.applied_at, 0, "PT-FIX3 off: a stale-facing air unit never applies the queued order (retail)");
    ck_eq(off.queue_left, 1, "PT-FIX3 off: the order is still queued after the whole run");
    ck_eq((int)off.facing_target, (int)STALE, "PT-FIX3 off: facing_target is never reconciled");
    ck_eq((int)off.order_queued, 1, "PT-FIX3 off: order_queued stays set");

    outcome on = run(f, true, 0x0f, STALE, STALE, STEPS);
    ck(on.applied_at > 0 && on.applied_at <= BOUND, "PT-FIX3 on: the queued order is applied within the bound");
    ck_eq(on.queue_left, 0, "PT-FIX3 on: the order left the queue");

    // the first step alone: dispatch defers, then move_path snaps both facings, zeroes the budget,
    // leaves the microstep and order_queued alone
    {
        outcome one = run(f, true, 0x0f, STALE, STALE, 1);
        ck_eq(one.applied_at, 0, "PT-FIX3 on: the first dispatch still defers (facing stale)");
        ck_eq((int)one.facing_target, (int)TBL_FACE, "PT-FIX3 on: move_path snaps facing_target to the table facing");
        ck_eq((int)one.facing_current, (int)TBL_FACE, "PT-FIX3 on: move_path snaps facing_current to the table facing");
        ck_eq(one.microstep, 31, "PT-FIX3 on: the snap does not advance the microstep");
        ck_eq((int)one.order_queued, 1, "PT-FIX3 on: order_queued is left for the dispatcher to clear");
        ck_eq_d(one.budget, 0.0, "PT-FIX3 on: the early return still zeroes the tick budget");
    }

    // (2) a plane (type 0x11) behaves the same: the test is type > 0xe
    on = run(f, true, 0x11, STALE, STALE, STEPS);
    ck(on.applied_at > 0 && on.applied_at <= BOUND, "PT-FIX3 on: a plane (type 0x11) is covered too");

    // (3) bit-identical cases, key on vs off
    {
        // ground unit (type 5): the dispatcher skips the turn gate, move_path must leave the facing alone
        outcome g0 = run(f, false, 5, STALE, STALE, 1);
        outcome g1 = run(f, true, 5, STALE, STALE, 1);
        ck_eq(g1.applied_at, g0.applied_at, "PT-FIX3: ground unit -- applied step identical key on and off");
        ck_eq((int)g1.facing_target, (int)g0.facing_target, "PT-FIX3: ground unit -- facing_target identical on/off");
        ck_eq((int)g1.facing_current, (int)g0.facing_current, "PT-FIX3: ground unit -- facing_current identical on/off");
        ck_eq(g1.queue_left, g0.queue_left, "PT-FIX3: ground unit -- queue identical on/off");
        ck_eq_d(g1.budget, g0.budget, "PT-FIX3: ground unit -- tick budget identical on/off");

        // an air unit whose facing_target already matches: dispatch applies at step 1 with or without the key
        outcome a0 = run(f, false, 0x0f, TBL_FACE, STALE, STEPS);
        outcome a1 = run(f, true, 0x0f, TBL_FACE, STALE, STEPS);
        ck_eq(a0.applied_at, 1, "PT-FIX3 off: a matching-facing air unit applies at once");
        ck_eq(a1.applied_at, a0.applied_at, "PT-FIX3: matching-facing air unit -- applied step identical on/off");
        ck_eq((int)a1.facing_current, (int)a0.facing_current, "PT-FIX3: matching-facing air unit -- facing_current identical");
        ck_eq((int)a1.facing_target, (int)a0.facing_target, "PT-FIX3: matching-facing air unit -- facing_target identical");

        // matching facing_target, move_path ticked directly with the order still queued: nothing to
        // snap, so the lagging facing_current is NOT overwritten
        set_air_order_facing_fix(true);
        f.reset();
        f.cfg_units[PROTO].type                                         = 0x0f;
        f.move_microsteps[HEADING * MICROSTEPS_PER_HEADING + 31].facing = TBL_FACE;
        unit &u                                                         = f.u(PLAYER, OBJ);
        u                                                               = unit{};
        u.unit_proto_id                                                 = PROTO;
        u.state                                                         = 0x12;
        u.move_microstep                                                = 31;
        u.move_heading                                                  = HEADING;
        u.order_queued                                                  = 1;
        u.facing_target                                                 = TBL_FACE;
        u.facing_current                                                = STALE;
        f.cur_unit_ptr                                                  = &u;
        f.view_cur_player                                               = (uint16_t)PLAYER;
        f.view_cur_index                                                = (uint16_t)OBJ;
        f.tick_budget                                                   = 5.0;
        sim_store own                                                   = f.store();
        detail::unit_state_move_path(f.view(), own, g_mp_calls);
        set_air_order_facing_fix(false);
        ck_eq((int)f.u(PLAYER, OBJ).facing_current, (int)STALE,
              "PT-FIX3 on: matching facing_target -- the lagging facing_current is left alone");
    }
}

} // namespace mh::sim::test
