//
// sim_storage_scrap_docked_units_of_type_selftest.cpp -- `simtest` oracle for
// llm_strat_storage_scrap_docked_units_of_type @0x0046cb5a (sim/sim_storage_scrap.h/.cpp, RI-SIM /
// SIM1-G3).
//
// ARMABLE (arm_ready:true, 3 real direct writes: door_waiter_count, target_ref, target_index -- see
// the header's CORRECTION note) but NOT COVERED: two consecutive all-AI soaks (15000 steps @1000%,
// default scenario AND a 90-building save-seeded scenario) reached 0 calls -- the soak never happened
// to scrap a mid-exit-wait unit via a permanently-blocked door. This offline oracle is this slice's
// evidence in the meantime; a future rig run with a targeted save can still add T1 coverage on top.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM tmp/decomp_sim/llm_strat_storage_scrap_docked_units_of_type_0046cb5a.asm
// (not the .cpp):
//   sub_id = buildings[player][building_index].sub_id (byte @+0xc6), read ONCE before the loop.
//   for (i = 0; i < storage[player][sub_id].docked_count [RE-READ fresh every pass]; ++i):
//     docked_unit = storage[player][sub_id].docked_units[i] (full 32-bit).
//     SKIP entirely (straight to ++i) unless units[player][docked_unit].state == UNIT_STATE_EXIT_WAIT
//       (0x22).
//     door_waiter_count -= 1 -- DIRECT WRITE, unconditional once the state gate passes.
//     if order_queued != 0: order_queue_find_index(AMBIENT cur_player, AMBIENT cur_index,
//       ORDER_KIND_UNIT=0x80) -- NOT this function's own (player, docked_unit); then
//       order_queue_apply_and_dequeue(player, docked_unit, queue_idx TRUNCATED to 16 bits).
//     if target_ref != 0: target_release_ref(player, docked_unit, mode=1), THEN DIRECTLY zero both
//       target_ref and target_index in THIS function (a genuine second write site, not a mirror of
//       what the callee does).
//     proto2/state re-read FRESH; if state == cfg_units[proto2].move_op_arg OR == move_op_code:
//       unit_notify_status(player, docked_unit, 0x67).
//     unit_set_state_of(player, docked_unit, UNIT_STATE_PARKED=0x1f) -- ALWAYS, fires whether or not
//       the notify above fired.
//
#include "sim/sim_storage_scrap.h"

#include <cstdint>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr uint32_t PLAYER         = 4;
constexpr int32_t  BUILDING_INDEX = 2;
constexpr int32_t  SUB_ID         = 6; // distinct from BUILDING_INDEX

struct call_log {
    enum kind_t { FIND_INDEX,
                  APPLY_DEQUEUE,
                  RELEASE_REF,
                  NOTIFY_STATUS,
                  SET_STATE };
    struct ev {
        kind_t   kind;
        uint32_t a, b, c;
    };
    std::vector<ev> events;
    void            reset() { events.clear(); }
};
call_log g_log;
int32_t  g_find_index_result = 0;

const storage_scrap_of_type_calls &recording_calls() {
    static const storage_scrap_of_type_calls c = {
        [](int32_t player, int32_t unit_idx, int32_t kind_tag) -> int32_t {
            g_log.events.push_back(
                call_log::ev{call_log::FIND_INDEX, (uint32_t)player, (uint32_t)unit_idx,
                             (uint32_t)kind_tag});
            return g_find_index_result;
        },
        [](uint32_t player, int32_t unit_idx, int32_t queue_idx) -> void {
            g_log.events.push_back(
                call_log::ev{call_log::APPLY_DEQUEUE, player, (uint32_t)unit_idx, (uint32_t)queue_idx});
        },
        [](uint32_t player_idx, int32_t unit_idx, uint32_t mode) -> void {
            g_log.events.push_back(
                call_log::ev{call_log::RELEASE_REF, player_idx, (uint32_t)unit_idx, mode});
        },
        [](uint32_t player, int32_t unit_index, uint32_t status_code) -> void {
            g_log.events.push_back(
                call_log::ev{call_log::NOTIFY_STATUS, player, (uint32_t)unit_index, status_code});
        },
        [](int32_t player, int32_t unit_index, int16_t state) -> void {
            g_log.events.push_back(call_log::ev{call_log::SET_STATE, (uint32_t)player,
                                                (uint32_t)unit_index, (uint32_t)(uint16_t)state});
        },
    };
    return c;
}

void ck_kind(int32_t n, call_log::kind_t kind, const char *what) {
    if ((size_t)n >= g_log.events.size()) {
        ck(false, what);
        return;
    }
    ck(g_log.events[(size_t)n].kind == kind, what);
}

unit_storage &storage_row(sim_fixture &f, uint32_t player, int32_t slot) {
    return f.storage[(size_t)player * (size_t)STORAGE_PER_PLAYER + (size_t)slot];
}

void put_storage(sim_fixture &f, uint32_t player, int32_t slot, int32_t docked_count,
                 std::initializer_list<int32_t> units) {
    unit_storage &s = storage_row(f, player, slot);
    s.docked_count  = docked_count;
    int32_t i       = 0;
    for (int32_t u : units) s.docked_units[i++] = u;
}

} // namespace

// ---- S1: a docked unit NOT in EXIT_WAIT is skipped entirely -- no writes, no calls at all.
void test_non_exit_wait_unit_skipped_entirely() {
    sim_fixture fx;
    fx.b(PLAYER, BUILDING_INDEX).sub_id = (uint8_t)SUB_ID;
    put_storage(fx, PLAYER, SUB_ID, 1, {10});
    fx.u(PLAYER, 10).state                            = 0x1f; // UNIT_STATE_PARKED, NOT EXIT_WAIT
    storage_row(fx, PLAYER, SUB_ID).door_waiter_count = 5;    // guard: must stay untouched

    sim_view  v   = fx.view();
    sim_store own = fx.store();
    g_log.reset();
    detail::storage_scrap_docked_units_of_type(v, own, recording_calls(), PLAYER, BUILDING_INDEX);

    ck_eq((uint32_t)g_log.events.size(), 0u, "S1: non-EXIT_WAIT docked unit -> zero calls");
    ck_eq((uint32_t)storage_row(fx, PLAYER, SUB_ID).door_waiter_count, 5u,
          "S1: door_waiter_count untouched when the state gate fails");
}

// ---- S2: qualifying unit, order_queued==0, target_ref==0 -- the MINIMAL path: door_waiter_count
// decrements, no queue/target calls, notify skipped (state != move_op_arg/move_op_code), set_state
// fires last.
void test_minimal_path_decrement_and_set_state_only() {
    sim_fixture fx;
    fx.b(PLAYER, BUILDING_INDEX).sub_id = (uint8_t)SUB_ID;
    put_storage(fx, PLAYER, SUB_ID, 1, {20});
    unit &du                                          = fx.u(PLAYER, 20);
    du.state                                          = UNIT_STATE_EXIT_WAIT;
    du.order_queued                                   = 0;
    du.target_ref                                     = 0;
    du.target_index                                   = 0x1234; // guard: must stay untouched since target_ref==0
    du.unit_proto_id                                  = 7;
    fx.cfg_units[7].move_op_arg                       = 0x99; // distinct from state (0x22) -- notify must NOT fire
    fx.cfg_units[7].move_op_code                      = 0x98;
    storage_row(fx, PLAYER, SUB_ID).door_waiter_count = 3;

    sim_view  v   = fx.view();
    sim_store own = fx.store();
    g_log.reset();
    detail::storage_scrap_docked_units_of_type(v, own, recording_calls(), PLAYER, BUILDING_INDEX);

    ck_eq((uint32_t)storage_row(fx, PLAYER, SUB_ID).door_waiter_count, 2u,
          "S2: door_waiter_count decremented unconditionally once the state gate passes");
    ck_eq((uint32_t)g_log.events.size(), 1u,
          "S2: order_queued==0 and target_ref==0 -> only set_state fires");
    ck_kind(0, call_log::SET_STATE, "S2[0]: set_state fires even on the minimal path");
    ck_eq(g_log.events[0].c, 0x1fu, "S2[0]: set_state -> UNIT_STATE_PARKED (0x1f)");
    ck_eq((uint32_t)(uint16_t)fx.u(PLAYER, 20).target_index, 0x1234u,
          "S2: target_index untouched when target_ref was already 0");
}

// ---- S3: order_queued!=0 -- find_index is called with the AMBIENT cur_player/cur_index, NOT this
// function's own (player, docked_unit); apply_and_dequeue then gets (player, docked_unit,
// TRUNCATED queue_idx).
void test_order_queued_uses_ambient_player_index_and_truncates_queue_idx() {
    sim_fixture fx;
    fx.b(PLAYER, BUILDING_INDEX).sub_id = (uint8_t)SUB_ID;
    put_storage(fx, PLAYER, SUB_ID, 1, {30});
    unit &du                    = fx.u(PLAYER, 30);
    du.state                    = UNIT_STATE_EXIT_WAIT;
    du.order_queued             = 1;
    du.target_ref               = 0;
    du.unit_proto_id            = 7;
    fx.cfg_units[7].move_op_arg = fx.cfg_units[7].move_op_code = 0x99; // != state, no notify

    // Ambient globals, DISTINCT from PLAYER (4) and docked_unit (30) so a translation that
    // (wrongly) forwards its own params instead would be caught.
    fx.view_cur_player  = 1;
    fx.view_cur_index   = 55;
    g_find_index_result = 0x10007; // > 16 bits -- apply_and_dequeue must see only 0x0007

    sim_view  v   = fx.view();
    sim_store own = fx.store();
    g_log.reset();
    detail::storage_scrap_docked_units_of_type(v, own, recording_calls(), PLAYER, BUILDING_INDEX);

    ck_eq((uint32_t)g_log.events.size(), 3u,
          "S3: order_queued!=0 -> find_index + apply_and_dequeue + set_state");
    ck_kind(0, call_log::FIND_INDEX, "S3[0]: order_queue_find_index");
    ck_eq(g_log.events[0].a, 1u, "S3[0] FINDING: find_index's 1st arg is the AMBIENT cur_player (1), "
                                 "not this function's own player (4)");
    ck_eq(g_log.events[0].b, 55u, "S3[0] FINDING: find_index's 2nd arg is the AMBIENT cur_index (55), "
                                  "not the docked_unit (30)");
    ck_eq(g_log.events[0].c, ORDER_KIND_UNIT, "S3[0]: find_index's kind_tag is ORDER_KIND_UNIT (0x80)");
    ck_kind(1, call_log::APPLY_DEQUEUE, "S3[1]: order_queue_apply_and_dequeue");
    ck_eq(g_log.events[1].a, PLAYER, "S3[1]: apply_and_dequeue's player IS this function's own player");
    ck_eq(g_log.events[1].b, 30u, "S3[1]: apply_and_dequeue's unit_idx IS the docked_unit");
    ck_eq(g_log.events[1].c, 0x0007u,
          "S3[1] FINDING: apply_and_dequeue's queue_idx is TRUNCATED to 16 bits (0x10007 -> 0x0007)");
    ck_kind(2, call_log::SET_STATE, "S3[2]: set_state fires last");
}

// ---- S4: target_ref!=0 -- target_release_ref(player, docked_unit, mode=1) fires, THEN target_ref
// and target_index are DIRECTLY zeroed in this function (not merely a mirror of the callee, which
// this recording mock never touches -- so a passing case here proves the direct writes are real).
void test_target_ref_nonzero_releases_then_directly_clears_both_fields() {
    sim_fixture fx;
    fx.b(PLAYER, BUILDING_INDEX).sub_id = (uint8_t)SUB_ID;
    put_storage(fx, PLAYER, SUB_ID, 1, {40});
    unit &du                    = fx.u(PLAYER, 40);
    du.state                    = UNIT_STATE_EXIT_WAIT;
    du.order_queued             = 0;
    du.target_ref               = 99;
    du.target_index             = 88;
    du.unit_proto_id            = 7;
    fx.cfg_units[7].move_op_arg = fx.cfg_units[7].move_op_code = 0x99;

    sim_view  v   = fx.view();
    sim_store own = fx.store();
    g_log.reset();
    detail::storage_scrap_docked_units_of_type(v, own, recording_calls(), PLAYER, BUILDING_INDEX);

    ck_eq((uint32_t)g_log.events.size(), 2u, "S4: target_release_ref + set_state (no queue calls)");
    ck_kind(0, call_log::RELEASE_REF, "S4[0]: target_release_ref");
    ck_eq(g_log.events[0].b, 40u, "S4[0]: target_release_ref's unit_idx is the docked_unit");
    ck_eq(g_log.events[0].c, 1u, "S4[0]: target_release_ref's mode is 1");
    ck_kind(1, call_log::SET_STATE, "S4[1]: set_state fires after the release");
    ck_eq((uint32_t)(uint16_t)fx.u(PLAYER, 40).target_ref, 0u,
          "S4: target_ref DIRECTLY zeroed by this function (the mock callee never touches it)");
    ck_eq((uint32_t)(uint16_t)fx.u(PLAYER, 40).target_index, 0u,
          "S4: target_index DIRECTLY zeroed by this function too");
}

// ---- S5: the move_op_arg/move_op_code notify gate -- state matching EITHER field fires notify;
// set_state still follows unconditionally.
void test_notify_fires_on_move_op_arg_or_move_op_code_match() {
    sim_fixture fx;
    fx.b(PLAYER, BUILDING_INDEX).sub_id = (uint8_t)SUB_ID;
    put_storage(fx, PLAYER, SUB_ID, 1, {50});
    unit &du         = fx.u(PLAYER, 50);
    du.state         = UNIT_STATE_EXIT_WAIT;
    du.order_queued  = 0;
    du.target_ref    = 0;
    du.unit_proto_id = 9;
    // move_op_code matches state (0x22); move_op_arg does not -- proves the OR, not just the arg half.
    fx.cfg_units[9].move_op_arg  = 0x11;
    fx.cfg_units[9].move_op_code = UNIT_STATE_EXIT_WAIT;

    sim_view  v   = fx.view();
    sim_store own = fx.store();
    g_log.reset();
    detail::storage_scrap_docked_units_of_type(v, own, recording_calls(), PLAYER, BUILDING_INDEX);

    ck_eq((uint32_t)g_log.events.size(), 2u, "S5: notify (via move_op_code match) + set_state");
    ck_kind(0, call_log::NOTIFY_STATUS, "S5[0]: unit_notify_status fires on move_op_code match");
    ck_eq(g_log.events[0].c, 0x67u, "S5[0]: notify's status_code is 0x67");
    ck_kind(1, call_log::SET_STATE, "S5[1]: set_state fires after notify");
}

// ---- S6: sub_id INDIRECTION + docked_count==0 -- the loop condition fails on its first test, and
// (as S1's poison already covers per-unit skipping) a decoy at storage[player][building_index] must
// not be touched.
void test_sub_id_indirection_and_zero_docked_count() {
    sim_fixture fx;
    fx.b(PLAYER, BUILDING_INDEX).sub_id = (uint8_t)SUB_ID;
    put_storage(fx, PLAYER, SUB_ID, 0, {});
    put_storage(fx, PLAYER, BUILDING_INDEX, 2, {1, 2}); // decoy at the building_index-numbered slot
    fx.u(PLAYER, 1).state = UNIT_STATE_EXIT_WAIT;       // would qualify if the decoy were walked
    fx.u(PLAYER, 2).state = UNIT_STATE_EXIT_WAIT;

    sim_view  v   = fx.view();
    sim_store own = fx.store();
    g_log.reset();
    detail::storage_scrap_docked_units_of_type(v, own, recording_calls(), PLAYER, BUILDING_INDEX);

    ck_eq((uint32_t)g_log.events.size(), 0u,
          "S6: sub_id's row has docked_count=0 -> zero calls, decoy at building_index untouched");
}

void run_storage_scrap_docked_units_of_type_tests() {
    test_non_exit_wait_unit_skipped_entirely();
    test_minimal_path_decrement_and_set_state_only();
    test_order_queued_uses_ambient_player_index_and_truncates_queue_idx();
    test_target_ref_nonzero_releases_then_directly_clears_both_fields();
    test_notify_fires_on_move_op_arg_or_move_op_code_match();
    test_sub_id_indirection_and_zero_docked_count();
}

} // namespace mh::sim::test
