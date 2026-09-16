//
// orders/issue/issue_promote.cpp -- see issue_promote.h for what this installs and why the
// dead rows are excluded. This file is the MECHANICAL half: one adapter per verified row, one
// MH_EXPORT_REPLACE seam per row, one install-table entry per row.
//
// THE ADAPTER SHAPE. Each `promoted_arm::<name>` below has the EXACT parameter/return types of
// `::mh::exp::sig_<original>` (addr/mh_export.gen.h) -- MH_EXPORT_REPLACE binds it through a
// function pointer of that type, so any drift is a COMPILE error, not a stack that comes apart
// at the naked entry thunk. Where the sig's register-derived widths/signs differ from the
// domain's own C++ wrapper (orders/issue/issue_*.h) -- e.g. a sig's `uint32_t a2` against a
// wrapper's `int32_t target_x` -- the adapter casts explicitly at the call site, exactly as
// order_queue.cpp's promoted:: forms make the ORIGINAL's own narrowing visible rather than
// leaving it as an implicit conversion a reader has to reconstruct.
//
// MACRO TRAP: MH_EXPORT_REPLACE(FN, IMPL) is a macro, and IMPL must be a NAMED function -- a
// lambda's brace/comma tokens break the expansion. Every adapter below is named for exactly
// that reason, never inlined at the MH_EXPORT_REPLACE call site.
//
#include "orders/issue/issue_promote.h"

#include "addr/mh_export.gen.h" // MH_EXPORT_REPLACE, sig_<fn> -- the type-checked binding

// LIB-REF-IN: every covered adapter in this file forwards through libmh_issue_order with its own
// LIBMH_ORD_* id, not into the C++ wrapper. That entry DISPATCHES on the id -- it does not run a
// sequence -- so a per-row seam reaches exactly its own body, and the generated thunk unpacks argv
// back into the committed prototype, so the int32_t widening is a round trip rather than a
// narrowing. libmh builds the 68-byte record either way (orders/order_codec.h stays the single
// encoding). The argv packing order is the ADAPTER'S OWN parameter order and its length is
// cross-checked against the committed prototype by gen_libmh_inbound.py, which also re-derives the
// routing census and fails on an unrouted covered row. See sim/sim_hostreach_promote.cpp for the
// mechanism and the ordering hazard the open pays for.
#include "../../../libmh/include/libmh_host_in.h"
#include "orders/order_queue.h" // say() -- reused from the container's [promote] logger
#include "orders/issue/issue_ai_move_primitives.h"
#include "orders/issue/issue_bldg_depart.h"
#include "orders/issue/issue_bldg_footprint.h"
#include "orders/issue/issue_bldg_orders_b.h"
#include "orders/issue/issue_group_formation_move.h"
#include "orders/issue/issue_group_orders.h"
#include "orders/issue/issue_order_admin.h"
#include "orders/issue/issue_order_debug.h"
#include "orders/issue/issue_order_player.h"
#include "orders/issue/issue_state.h" // the pilot's 7: bldg_order_activate/deactivate_enqueue/
                                      // production_add/_remove/load_resource/repair_cycle_start,
                                      // order_debug_kill_group
#include "orders/issue/issue_turret_orders.h"
#include "orders/issue/issue_unit_attack.h"
#include "orders/issue/issue_unit_move.h"
#include "orders/issue/issue_unit_storage.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

namespace mh::orders::issue::promoted_arm {
namespace {

// PER-ROW FIRST-CALL LIVENESS (anti-vacuity). One flag per row, cleared at load; the FIRST call
// through a row's adapter flips it and logs exactly once -- "is the promoted body ever ENTERED",
// which "N/N seams installed" alone does not prove (install only proves the entry bytes were
// patched, not that anything ever executed through them). Plain static storage, no lock: this is
// the same discipline order_queue.cpp's own promoted::g_calls counters use -- a game loop is not
// contending threads against this array, and a lost race would at worst repeat one log line.
bool g_seen[62] = {};

void note_first_call(int idx, const char *name) {
    if (g_seen[idx]) return;
    g_seen[idx] = true;
    say("; [promote] orders_issue: %s call #1 (OURS is live)\n", name);
}

} // namespace

// 0: llm_strat_bldg_order_depart_confirm_dispatch
void bldg_order_depart_confirm_dispatch() {
    note_first_call(0, "llm_strat_bldg_order_depart_confirm_dispatch");

    ::libmh_issue_order(LIBMH_ORD_BLDG_ORDER_DEPART_CONFIRM_DISPATCH, nullptr, 0u);
}

// 1: llm_strat_group_order_ack_voice
void group_order_ack_voice() {
    note_first_call(1, "llm_strat_group_order_ack_voice");

    ::libmh_order_ack_voice();
}

// 2: llm_strat_group_issue_move_order_deferred
void group_issue_move_order_deferred(int32_t op_code, int32_t op_arg, int32_t modifier) {
    note_first_call(2, "llm_strat_group_issue_move_order_deferred");

    const int32_t argv[3] = {(int32_t)op_code, (int32_t)op_arg, (int32_t)modifier};
    ::libmh_issue_order(LIBMH_ORD_GROUP_ISSUE_MOVE_ORDER_DEFERRED, argv, 3u);
}

// 3: llm_strat_group_issue_move_order_confirmed
void group_issue_move_order_confirmed(int32_t dst_x, int32_t dst_y) {
    note_first_call(3, "llm_strat_group_issue_move_order_confirmed");

    const int32_t argv[2] = {(int32_t)dst_x, (int32_t)dst_y};
    ::libmh_issue_order(LIBMH_ORD_GROUP_ISSUE_MOVE_ORDER_CONFIRMED, argv, 2u);
}

// 4: llm_strat_bldg_footprint_random_point
void bldg_footprint_random_point(uint32_t  param_1,
                                 uint32_t  param_2,
                                 int32_t   a2,
                                 int32_t   param_4,
                                 uint32_t *param_5,
                                 uint32_t *param_6) {
    note_first_call(4, "llm_strat_bldg_footprint_random_point");
    ::mh::orders::issue::bldg_footprint_random_point(
        param_1,
        param_2,
        (uint32_t)a2,
        param_4,
        param_5,
        param_6);
}

// 5: llm_strat_unit_order_move
void unit_order_move(uint32_t param_1,
                     int32_t  param_2,
                     uint32_t a2,
                     uint32_t param_4,
                     uint32_t param_5) {
    note_first_call(5, "llm_strat_unit_order_move");

    const int32_t argv[5] = {(int32_t)param_1, (int32_t)param_2, (int32_t)a2, (int32_t)param_4, (int32_t)param_5};
    ::libmh_issue_order(LIBMH_ORD_UNIT_ORDER_MOVE, argv, 5u);
}

// 6: llm_strat_unit_order_move_default
void unit_order_move_default(uint32_t param_1,
                             int32_t  param_2,
                             uint32_t a2,
                             uint32_t param_4,
                             uint32_t param_5) {
    note_first_call(6, "llm_strat_unit_order_move_default");
    ::mh::orders::issue::unit_order_move_default(
        param_1,
        param_2,
        (int32_t)a2,
        (int32_t)param_4,
        (int32_t)param_5);
}

// 7: llm_strat_unit_order_scatter_from_spawn
void unit_order_scatter_from_spawn(uint16_t param_1,
                                   int32_t  param_2,
                                   uint32_t a2,
                                   uint32_t param_4) {
    note_first_call(7, "llm_strat_unit_order_scatter_from_spawn");
    ::mh::orders::issue::unit_order_scatter_from_spawn(
        param_1,
        param_2,
        (int32_t)a2,
        (int32_t)param_4);
}

// 8: llm_strat_unit_order_exit_storage
void unit_order_exit_storage(uint32_t param_1,
                             uint32_t param_2,
                             int32_t  a2,
                             uint32_t param_4,
                             uint32_t param_5) {
    note_first_call(8, "llm_strat_unit_order_exit_storage");

    const int32_t argv[5] = {(int32_t)param_1, (int32_t)param_2, (int32_t)a2, (int32_t)param_4, (int32_t)param_5};
    ::libmh_issue_order(LIBMH_ORD_UNIT_ORDER_EXIT_STORAGE, argv, 5u);
}

// 9: llm_strat_unit_order_move_confirmed_with_bump
void unit_order_move_confirmed_with_bump(uint32_t param_1,
                                         int32_t  param_2,
                                         uint32_t a2,
                                         uint32_t param_4) {
    note_first_call(9, "llm_strat_unit_order_move_confirmed_with_bump");
    ::mh::orders::issue::unit_order_move_confirmed_with_bump(
        param_1,
        param_2,
        (int32_t)a2,
        (int32_t)param_4);
}

// 10: llm_strat_unit_order_attack_target
void unit_order_attack_target(uint32_t param_1,
                              int32_t  param_2,
                              uint32_t a2,
                              int32_t  param_4,
                              uint32_t param_5) {
    note_first_call(10, "llm_strat_unit_order_attack_target");
    ::mh::orders::issue::unit_order_attack_target(param_1, param_2, a2, param_4, param_5);
}

// 11: llm_strat_unit_order_attack_target_alt
void unit_order_attack_target_alt(uint32_t player,
                                  int32_t  unit_idx,
                                  uint32_t target_side,
                                  int32_t  target_unit_idx,
                                  uint32_t weapon_idx) {
    note_first_call(11, "llm_strat_unit_order_attack_target_alt");
    ::mh::orders::issue::unit_order_attack_target_alt(
        player,
        unit_idx,
        target_side,
        target_unit_idx,
        weapon_idx);
}

// 12: llm_strat_unit_order_attack_unit
void unit_order_attack_unit(uint32_t param_1,
                            int32_t  param_2,
                            uint32_t a2,
                            int32_t  param_4,
                            uint32_t param_5) {
    note_first_call(12, "llm_strat_unit_order_attack_unit");
    ::mh::orders::issue::unit_order_attack_unit(param_1, param_2, a2, param_4, param_5);
}

// 13: llm_strat_unit_order_attack_building_reposition
void unit_order_attack_building_reposition(uint32_t param_1,
                                           int32_t  param_2,
                                           uint32_t a2,
                                           int32_t  param_4,
                                           uint32_t param_5) {
    note_first_call(13, "llm_strat_unit_order_attack_building_reposition");
    ::mh::orders::issue::unit_order_attack_building_reposition(
        param_1,
        param_2,
        a2,
        param_4,
        param_5);
}

// 14: llm_strat_unit_order_move_relative
void unit_order_move_relative(uint32_t param_1, int32_t param_2, int32_t a2, int32_t param_4) {
    note_first_call(14, "llm_strat_unit_order_move_relative");

    const int32_t argv[4] = {(int32_t)param_1, (int32_t)param_2, (int32_t)a2, (int32_t)param_4};
    ::libmh_issue_order(LIBMH_ORD_UNIT_ORDER_MOVE_RELATIVE, argv, 4u);
}

// 15: llm_strat_unit_order_auto_launch_from_storage
void unit_order_auto_launch_from_storage(uint32_t player,
                                         int32_t  unit_idx,
                                         uint32_t x,
                                         uint32_t y) {
    note_first_call(15, "llm_strat_unit_order_auto_launch_from_storage");

    const int32_t argv[4] = {(int32_t)player, (int32_t)unit_idx, (int32_t)x, (int32_t)y};
    ::libmh_issue_order(LIBMH_ORD_UNIT_ORDER_AUTO_LAUNCH_FROM_STORAGE, argv, 4u);
}

// 16: llm_strat_order_create_unit_debug
uint32_t order_create_unit_debug(uint32_t param_1,
                                 uint32_t param_2,
                                 uint32_t a2,
                                 uint32_t param_4) {
    note_first_call(16, "llm_strat_order_create_unit_debug");
    const int32_t argv[4] = {(int32_t)param_1, (int32_t)param_2, (int32_t)a2, (int32_t)param_4};
    return (uint32_t)::libmh_issue_order(LIBMH_ORD_ORDER_CREATE_UNIT_DEBUG, argv, 4u);
}

// 17: llm_strat_order_queue_construction_debug
uint32_t order_queue_construction_debug(uint32_t param_1,
                                        uint32_t param_2,
                                        uint32_t a2,
                                        uint32_t param_4) {
    note_first_call(17, "llm_strat_order_queue_construction_debug");
    const int32_t argv[4] = {(int32_t)param_1, (int32_t)param_2, (int32_t)a2, (int32_t)param_4};
    return (uint32_t)::libmh_issue_order(LIBMH_ORD_ORDER_QUEUE_CONSTRUCTION_DEBUG, argv, 4u);
}

// 18: llm_strat_bldg_order_production_add
void bldg_order_production_add(uint32_t player, int32_t bldg_idx, int32_t unit_type) {
    note_first_call(18, "llm_strat_bldg_order_production_add");

    const int32_t argv[3] = {(int32_t)player, (int32_t)bldg_idx, (int32_t)unit_type};
    ::libmh_issue_order(LIBMH_ORD_BLDG_ORDER_PRODUCTION_ADD, argv, 3u);
}

// 19: llm_strat_bldg_order_production_remove
void bldg_order_production_remove(uint32_t player, int32_t bldg_idx, int32_t unit_type) {
    note_first_call(19, "llm_strat_bldg_order_production_remove");

    const int32_t argv[3] = {(int32_t)player, (int32_t)bldg_idx, (int32_t)unit_type};
    ::libmh_issue_order(LIBMH_ORD_BLDG_ORDER_PRODUCTION_REMOVE, argv, 3u);
}

// 20: llm_strat_turret_order_target_unit
void turret_order_target_unit(uint16_t player,
                              uint16_t building_or_id,
                              uint32_t target_or_side,
                              uint32_t target_unit_idx) {
    note_first_call(20, "llm_strat_turret_order_target_unit");

    const int32_t argv[4] = {(int32_t)player, (int32_t)building_or_id, (int32_t)target_or_side, (int32_t)target_unit_idx};
    ::libmh_issue_order(LIBMH_ORD_TURRET_ORDER_TARGET_UNIT, argv, 4u);
}

// 21: llm_strat_turret_order_target_building
void turret_order_target_building(uint16_t param_1,
                                  uint16_t param_2,
                                  uint32_t a2,
                                  uint32_t param_4) {
    note_first_call(21, "llm_strat_turret_order_target_building");

    const int32_t argv[4] = {(int32_t)param_1, (int32_t)param_2, (int32_t)a2, (int32_t)param_4};
    ::libmh_issue_order(LIBMH_ORD_TURRET_ORDER_TARGET_BUILDING, argv, 4u);
}

// 22: llm_strat_bldg_order_assign_workers
void bldg_order_assign_workers(uint16_t player, uint16_t building_index, uint32_t count) {
    note_first_call(22, "llm_strat_bldg_order_assign_workers");

    const int32_t argv[3] = {(int32_t)player, (int32_t)building_index, (int32_t)count};
    ::libmh_issue_order(LIBMH_ORD_BLDG_ORDER_ASSIGN_WORKERS, argv, 3u);
}

// 23: llm_strat_bldg_order_unassign_workers
void bldg_order_unassign_workers(uint16_t player, uint16_t building_index, uint32_t count) {
    note_first_call(23, "llm_strat_bldg_order_unassign_workers");

    const int32_t argv[3] = {(int32_t)player, (int32_t)building_index, (int32_t)count};
    ::libmh_issue_order(LIBMH_ORD_BLDG_ORDER_UNASSIGN_WORKERS, argv, 3u);
}

// 24: llm_strat_bldg_order_activate
void bldg_order_activate(uint32_t player, uint16_t bldg_idx) {
    note_first_call(24, "llm_strat_bldg_order_activate");
    ::mh::orders::issue::bldg_order_activate(player, bldg_idx);
}

// 25: llm_strat_bldg_order_deactivate
void bldg_order_deactivate(uint32_t player, uint16_t building_idx) {
    note_first_call(25, "llm_strat_bldg_order_deactivate");
    ::mh::orders::issue::bldg_order_deactivate(player, building_idx);
}

// 26: llm_strat_bldg_order_deactivate_enqueue
void bldg_order_deactivate_enqueue(uint16_t owner_kind, uint16_t bldg_index) {
    note_first_call(26, "llm_strat_bldg_order_deactivate_enqueue");
    ::mh::orders::issue::bldg_order_deactivate_enqueue((uint32_t)owner_kind, bldg_index);
}

// 27: llm_strat_bldg_order_toggle_active
void bldg_order_toggle_active(uint16_t player, int32_t building_index) {
    note_first_call(27, "llm_strat_bldg_order_toggle_active");

    const int32_t argv[2] = {(int32_t)player, (int32_t)building_index};
    ::libmh_issue_order(LIBMH_ORD_BLDG_ORDER_TOGGLE_ACTIVE, argv, 2u);
}

// 28: llm_strat_bldg_order_upgrade
void bldg_order_upgrade(uint32_t player, uint32_t bldg_unit_id) {
    note_first_call(28, "llm_strat_bldg_order_upgrade");

    const int32_t argv[2] = {(int32_t)player, (int32_t)bldg_unit_id};
    ::libmh_issue_order(LIBMH_ORD_BLDG_ORDER_UPGRADE, argv, 2u);
}

// 29: llm_strat_bldg_order_repair_cycle_start
void bldg_order_repair_cycle_start(uint32_t player, int32_t bldg_idx) {
    note_first_call(29, "llm_strat_bldg_order_repair_cycle_start");

    const int32_t argv[2] = {(int32_t)player, (int32_t)bldg_idx};
    ::libmh_issue_order(LIBMH_ORD_BLDG_ORDER_REPAIR_CYCLE_START, argv, 2u);
}

// 30: llm_strat_bldg_order_hangar_recharge
void bldg_order_hangar_recharge(uint32_t player, int32_t bldg_idx) {
    note_first_call(30, "llm_strat_bldg_order_hangar_recharge");

    const int32_t argv[2] = {(int32_t)player, (int32_t)bldg_idx};
    ::libmh_issue_order(LIBMH_ORD_BLDG_ORDER_HANGAR_RECHARGE, argv, 2u);
}

// 31: llm_strat_bldg_order_restart_construction
void bldg_order_restart_construction(uint32_t target_id, uint16_t player) {
    note_first_call(31, "llm_strat_bldg_order_restart_construction");

    const int32_t argv[2] = {(int32_t)target_id, (int32_t)player};
    ::libmh_issue_order(LIBMH_ORD_BLDG_ORDER_RESTART_CONSTRUCTION, argv, 2u);
}

// 32: llm_strat_bldg_order_purge_dead_docked
void bldg_order_purge_dead_docked(uint32_t player, uint16_t bldg_idx) {
    note_first_call(32, "llm_strat_bldg_order_purge_dead_docked");

    const int32_t argv[2] = {(int32_t)player, (int32_t)bldg_idx};
    ::libmh_issue_order(LIBMH_ORD_BLDG_ORDER_PURGE_DEAD_DOCKED, argv, 2u);
}

// 33: llm_strat_bldg_order_shuttle_depart
void bldg_order_shuttle_depart(uint32_t param_1, uint16_t param_2, uint32_t a2) {
    note_first_call(33, "llm_strat_bldg_order_shuttle_depart");
    ::mh::orders::issue::bldg_order_shuttle_depart(param_1, param_2, (int32_t)a2);
}

// 34: llm_strat_bldg_order_shuttle_depart_enqueue
void bldg_order_shuttle_depart_enqueue(uint16_t player, uint16_t bldg_idx, uint32_t planet_idx) {
    note_first_call(34, "llm_strat_bldg_order_shuttle_depart_enqueue");
    ::mh::orders::issue::bldg_order_shuttle_depart_enqueue(player, bldg_idx, (int32_t)planet_idx);
}

// 35: llm_strat_bldg_order_flush_cargo_hold
void bldg_order_flush_cargo_hold(uint32_t player, uint32_t bldg_unit_id) {
    note_first_call(35, "llm_strat_bldg_order_flush_cargo_hold");

    const int32_t argv[2] = {(int32_t)player, (int32_t)bldg_unit_id};
    ::libmh_issue_order(LIBMH_ORD_BLDG_ORDER_FLUSH_CARGO_HOLD, argv, 2u);
}

// 36: llm_strat_bldg_order_load_passengers
void bldg_order_load_passengers(uint16_t player, uint16_t bldg_idx, uint32_t planet_idx) {
    note_first_call(36, "llm_strat_bldg_order_load_passengers");

    const int32_t argv[3] = {(int32_t)player, (int32_t)bldg_idx, (int32_t)planet_idx};
    ::libmh_issue_order(LIBMH_ORD_BLDG_ORDER_LOAD_PASSENGERS, argv, 3u);
}

// 37: llm_strat_bldg_order_unload_passengers
void bldg_order_unload_passengers(uint32_t player, uint16_t bldg_idx, uint32_t passenger_count) {
    note_first_call(37, "llm_strat_bldg_order_unload_passengers");

    const int32_t argv[3] = {(int32_t)player, (int32_t)bldg_idx, (int32_t)passenger_count};
    ::libmh_issue_order(LIBMH_ORD_BLDG_ORDER_UNLOAD_PASSENGERS, argv, 3u);
}

// 38: llm_strat_bldg_order_load_resource
void bldg_order_load_resource(uint32_t param_1, uint16_t param_2, uint32_t a2, uint32_t param_4) {
    note_first_call(38, "llm_strat_bldg_order_load_resource");

    const int32_t argv[4] = {(int32_t)param_1, (int32_t)param_2, (int32_t)a2, (int32_t)param_4};
    ::libmh_issue_order(LIBMH_ORD_BLDG_ORDER_LOAD_RESOURCE, argv, 4u);
}

// 39: llm_strat_bldg_order_unload_resource
void bldg_order_unload_resource(uint32_t player,
                                uint16_t bldg_idx,
                                uint32_t resource_slot,
                                uint32_t count) {
    note_first_call(39, "llm_strat_bldg_order_unload_resource");

    const int32_t argv[4] = {(int32_t)player, (int32_t)bldg_idx, (int32_t)resource_slot, (int32_t)count};
    ::libmh_issue_order(LIBMH_ORD_BLDG_ORDER_UNLOAD_RESOURCE, argv, 4u);
}

// 40: llm_strat_bldg_order_port_depart
void bldg_order_port_depart(uint16_t param_1, uint16_t param_2, uint32_t a2) {
    note_first_call(40, "llm_strat_bldg_order_port_depart");
    ::mh::orders::issue::bldg_order_port_depart((uint32_t)param_1, param_2, (int32_t)a2);
}

// 41: llm_strat_bldg_order_port_depart_enqueue
void bldg_order_port_depart_enqueue(uint16_t player, uint16_t bldg_idx, uint32_t planet_idx) {
    note_first_call(41, "llm_strat_bldg_order_port_depart_enqueue");
    ::mh::orders::issue::bldg_order_port_depart_enqueue(
        (uint32_t)player,
        bldg_idx,
        (int32_t)planet_idx);
}

// 42: llm_strat_bldg_order_mother_depart
void bldg_order_mother_depart(uint32_t param_1, uint16_t param_2, uint32_t a2) {
    note_first_call(42, "llm_strat_bldg_order_mother_depart");
    ::mh::orders::issue::bldg_order_mother_depart(param_1, param_2, (int32_t)a2);
}

// 43: llm_strat_bldg_order_mother_depart_enqueue
void bldg_order_mother_depart_enqueue(uint16_t param_1, uint16_t param_2, uint32_t a2) {
    note_first_call(43, "llm_strat_bldg_order_mother_depart_enqueue");
    ::mh::orders::issue::bldg_order_mother_depart_enqueue((uint32_t)param_1, param_2, (int32_t)a2);
}

// 44: llm_strat_bldg_order_depart_dispatch_by_type
void bldg_order_depart_dispatch_by_type(uint32_t player,
                                        int32_t  bldg_idx,
                                        uint32_t dest_planet_idx) {
    note_first_call(44, "llm_strat_bldg_order_depart_dispatch_by_type");

    const int32_t argv[3] = {(int32_t)player, (int32_t)bldg_idx, (int32_t)dest_planet_idx};
    ::libmh_issue_order(LIBMH_ORD_BLDG_ORDER_DEPART_DISPATCH_BY_TYPE, argv, 3u);
}

// 45: llm_strat_bldg_order_start_research_project
void bldg_order_start_research_project(uint32_t param_1, uint16_t param_2, uint32_t a2) {
    note_first_call(45, "llm_strat_bldg_order_start_research_project");

    const int32_t argv[3] = {(int32_t)param_1, (int32_t)param_2, (int32_t)a2};
    ::libmh_issue_order(LIBMH_ORD_BLDG_ORDER_START_RESEARCH_PROJECT, argv, 3u);
}

// 46: llm_strat_order_grant_resource
void order_grant_resource(uint32_t param_1, uint32_t param_2, uint32_t a2) {
    note_first_call(46, "llm_strat_order_grant_resource");

    const int32_t argv[3] = {(int32_t)param_1, (int32_t)param_2, (int32_t)a2};
    ::libmh_issue_order(LIBMH_ORD_ORDER_GRANT_RESOURCE, argv, 3u);
}

// 47: llm_strat_order_population_delta_debug
void order_population_delta_debug(uint32_t player_id, int32_t population_delta) {
    note_first_call(47, "llm_strat_order_population_delta_debug");

    const int32_t argv[2] = {(int32_t)player_id, (int32_t)population_delta};
    ::libmh_issue_order(LIBMH_ORD_ORDER_POPULATION_DELTA_DEBUG, argv, 2u);
}

// 48: llm_strat_order_collect_available_projects
void order_collect_available_projects(uint32_t side) {
    note_first_call(48, "llm_strat_order_collect_available_projects");

    const int32_t argv[1] = {(int32_t)side};
    ::libmh_issue_order(LIBMH_ORD_ORDER_COLLECT_AVAILABLE_PROJECTS, argv, 1u);
}

// 49: llm_strat_order_recheck_projects
void order_recheck_projects(uint32_t side) {
    note_first_call(49, "llm_strat_order_recheck_projects");

    const int32_t argv[1] = {(int32_t)side};
    ::libmh_issue_order(LIBMH_ORD_ORDER_RECHECK_PROJECTS, argv, 1u);
}

// 50: llm_strat_order_recheck_buildings
void order_recheck_buildings(uint32_t side) {
    note_first_call(50, "llm_strat_order_recheck_buildings");

    const int32_t argv[1] = {(int32_t)side};
    ::libmh_issue_order(LIBMH_ORD_ORDER_RECHECK_BUILDINGS, argv, 1u);
}

// 51: llm_strat_order_recheck_planet_system_all_players
void order_recheck_planet_system_all_players() {
    note_first_call(51, "llm_strat_order_recheck_planet_system_all_players");

    ::libmh_issue_order(LIBMH_ORD_ORDER_RECHECK_PLANET_SYSTEM_ALL_PLAYERS, nullptr, 0u);
}

// 52: llm_strat_order_credit_conquest_kills
void order_credit_conquest_kills(uint32_t side) {
    note_first_call(52, "llm_strat_order_credit_conquest_kills");

    const int32_t argv[1] = {(int32_t)side};
    ::libmh_issue_order(LIBMH_ORD_ORDER_CREDIT_CONQUEST_KILLS, argv, 1u);
}

// 53: llm_strat_order_set_player_relation
void order_set_player_relation(uint32_t player, uint32_t opponent_idx, uint8_t relation_value) {
    note_first_call(53, "llm_strat_order_set_player_relation");

    const int32_t argv[3] = {(int32_t)player, (int32_t)opponent_idx, (int32_t)relation_value};
    ::libmh_issue_order(LIBMH_ORD_ORDER_SET_PLAYER_RELATION, argv, 3u);
}

// 54: llm_strat_order_set_player_control_mode
void order_set_player_control_mode(uint32_t param_1, uint32_t param_2, char a2) {
    note_first_call(54, "llm_strat_order_set_player_control_mode");

    const int32_t argv[3] = {(int32_t)param_1, (int32_t)param_2, (int32_t)(uint8_t)a2};
    ::libmh_issue_order(LIBMH_ORD_ORDER_SET_PLAYER_CONTROL_MODE, argv, 3u);
}

// 55: llm_strat_order_debug_energy_refill_full
void order_debug_energy_refill_full(uint32_t player_id, int32_t target_id) {
    note_first_call(55, "llm_strat_order_debug_energy_refill_full");

    const int32_t argv[2] = {(int32_t)player_id, (int32_t)target_id};
    ::libmh_issue_order(LIBMH_ORD_ORDER_DEBUG_ENERGY_REFILL_FULL, argv, 2u);
}

// 56: llm_strat_order_debug_damage_scaled
void order_debug_damage_scaled(uint32_t player_id, int32_t damage_amount) {
    note_first_call(56, "llm_strat_order_debug_damage_scaled");

    const int32_t argv[2] = {(int32_t)player_id, (int32_t)damage_amount};
    ::libmh_issue_order(LIBMH_ORD_ORDER_DEBUG_DAMAGE_SCALED, argv, 2u);
}

// 57: llm_strat_order_debug_kill_group
void order_debug_kill_group(uint32_t player_id, int32_t damage_amount) {
    note_first_call(57, "llm_strat_order_debug_kill_group");

    const int32_t argv[2] = {(int32_t)player_id, (int32_t)damage_amount};
    ::libmh_issue_order(LIBMH_ORD_ORDER_DEBUG_KILL_GROUP, argv, 2u);
}

// 58: llm_strat_order_fow_reveal_full
void order_fow_reveal_full(uint32_t player_id) {
    note_first_call(58, "llm_strat_order_fow_reveal_full");

    const int32_t argv[1] = {(int32_t)player_id};
    ::libmh_issue_order(LIBMH_ORD_ORDER_FOW_REVEAL_FULL, argv, 1u);
}

// 59: llm_strat_ai_group_move_formation_rotating
void group_move_formation_rotating(uint32_t player,
                                   int32_t  unused_param2,
                                   int32_t  unused_param3,
                                   int32_t  target_x,
                                   int32_t  target_y) {
    note_first_call(59, "llm_strat_ai_group_move_formation_rotating");
    ::mh::orders::issue::group_move_formation_rotating(
        player,
        unused_param2,
        unused_param3,
        target_x,
        target_y);
}

// 60: llm_strat_ai_unit_flag_and_move
void unit_flag_and_move(uint32_t param_1, int32_t param_2, uint32_t a2, uint32_t param_4) {
    note_first_call(60, "llm_strat_ai_unit_flag_and_move");
    ::mh::orders::issue::unit_flag_and_move(param_1, param_2, a2, param_4);
}

// 61: llm_strat_ai_group_scatter_to_passable_tile
void group_scatter_to_passable_tile(uint32_t param_1, int32_t param_2, int32_t a2) {
    note_first_call(61, "llm_strat_ai_group_scatter_to_passable_tile");
    ::mh::orders::issue::group_scatter_to_passable_tile(param_1, param_2, a2);
}

} // namespace mh::orders::issue::promoted_arm

// ---- the 62 live seams (37 T1 + 25 T2 of tools/data/orders_issue_migration.json's `state:
// "verified"` rows) -- one MH_EXPORT_REPLACE per row, at file scope per the macro's own contract. --
MH_EXPORT_REPLACE(llm_strat_bldg_order_depart_confirm_dispatch, mh::orders::issue::promoted_arm::bldg_order_depart_confirm_dispatch)
MH_EXPORT_REPLACE(llm_strat_group_order_ack_voice, mh::orders::issue::promoted_arm::group_order_ack_voice)
MH_EXPORT_REPLACE(llm_strat_group_issue_move_order_deferred, mh::orders::issue::promoted_arm::group_issue_move_order_deferred)
MH_EXPORT_REPLACE(llm_strat_group_issue_move_order_confirmed, mh::orders::issue::promoted_arm::group_issue_move_order_confirmed)
MH_EXPORT_REPLACE(llm_strat_bldg_footprint_random_point, mh::orders::issue::promoted_arm::bldg_footprint_random_point)
MH_EXPORT_REPLACE(llm_strat_unit_order_move, mh::orders::issue::promoted_arm::unit_order_move)
MH_EXPORT_REPLACE(llm_strat_unit_order_move_default, mh::orders::issue::promoted_arm::unit_order_move_default)
MH_EXPORT_REPLACE(llm_strat_unit_order_scatter_from_spawn, mh::orders::issue::promoted_arm::unit_order_scatter_from_spawn)
MH_EXPORT_REPLACE(llm_strat_unit_order_exit_storage, mh::orders::issue::promoted_arm::unit_order_exit_storage)
MH_EXPORT_REPLACE(llm_strat_unit_order_move_confirmed_with_bump, mh::orders::issue::promoted_arm::unit_order_move_confirmed_with_bump)
MH_EXPORT_REPLACE(llm_strat_unit_order_attack_target, mh::orders::issue::promoted_arm::unit_order_attack_target)
MH_EXPORT_REPLACE(llm_strat_unit_order_attack_target_alt, mh::orders::issue::promoted_arm::unit_order_attack_target_alt)
MH_EXPORT_REPLACE(llm_strat_unit_order_attack_unit, mh::orders::issue::promoted_arm::unit_order_attack_unit)
MH_EXPORT_REPLACE(llm_strat_unit_order_attack_building_reposition, mh::orders::issue::promoted_arm::unit_order_attack_building_reposition)
MH_EXPORT_REPLACE(llm_strat_unit_order_move_relative, mh::orders::issue::promoted_arm::unit_order_move_relative)
MH_EXPORT_REPLACE(llm_strat_unit_order_auto_launch_from_storage, mh::orders::issue::promoted_arm::unit_order_auto_launch_from_storage)
MH_EXPORT_REPLACE(llm_strat_order_create_unit_debug, mh::orders::issue::promoted_arm::order_create_unit_debug)
MH_EXPORT_REPLACE(llm_strat_order_queue_construction_debug, mh::orders::issue::promoted_arm::order_queue_construction_debug)
MH_EXPORT_REPLACE(llm_strat_bldg_order_production_add, mh::orders::issue::promoted_arm::bldg_order_production_add)
MH_EXPORT_REPLACE(llm_strat_bldg_order_production_remove, mh::orders::issue::promoted_arm::bldg_order_production_remove)
MH_EXPORT_REPLACE(llm_strat_turret_order_target_unit, mh::orders::issue::promoted_arm::turret_order_target_unit)
MH_EXPORT_REPLACE(llm_strat_turret_order_target_building, mh::orders::issue::promoted_arm::turret_order_target_building)
MH_EXPORT_REPLACE(llm_strat_bldg_order_assign_workers, mh::orders::issue::promoted_arm::bldg_order_assign_workers)
MH_EXPORT_REPLACE(llm_strat_bldg_order_unassign_workers, mh::orders::issue::promoted_arm::bldg_order_unassign_workers)
MH_EXPORT_REPLACE(llm_strat_bldg_order_activate, mh::orders::issue::promoted_arm::bldg_order_activate)
MH_EXPORT_REPLACE(llm_strat_bldg_order_deactivate, mh::orders::issue::promoted_arm::bldg_order_deactivate)
MH_EXPORT_REPLACE(llm_strat_bldg_order_deactivate_enqueue, mh::orders::issue::promoted_arm::bldg_order_deactivate_enqueue)
MH_EXPORT_REPLACE(llm_strat_bldg_order_toggle_active, mh::orders::issue::promoted_arm::bldg_order_toggle_active)
MH_EXPORT_REPLACE(llm_strat_bldg_order_upgrade, mh::orders::issue::promoted_arm::bldg_order_upgrade)
MH_EXPORT_REPLACE(llm_strat_bldg_order_repair_cycle_start, mh::orders::issue::promoted_arm::bldg_order_repair_cycle_start)
MH_EXPORT_REPLACE(llm_strat_bldg_order_hangar_recharge, mh::orders::issue::promoted_arm::bldg_order_hangar_recharge)
MH_EXPORT_REPLACE(llm_strat_bldg_order_restart_construction, mh::orders::issue::promoted_arm::bldg_order_restart_construction)
MH_EXPORT_REPLACE(llm_strat_bldg_order_purge_dead_docked, mh::orders::issue::promoted_arm::bldg_order_purge_dead_docked)
MH_EXPORT_REPLACE(llm_strat_bldg_order_shuttle_depart, mh::orders::issue::promoted_arm::bldg_order_shuttle_depart)
MH_EXPORT_REPLACE(llm_strat_bldg_order_shuttle_depart_enqueue, mh::orders::issue::promoted_arm::bldg_order_shuttle_depart_enqueue)
MH_EXPORT_REPLACE(llm_strat_bldg_order_flush_cargo_hold, mh::orders::issue::promoted_arm::bldg_order_flush_cargo_hold)
MH_EXPORT_REPLACE(llm_strat_bldg_order_load_passengers, mh::orders::issue::promoted_arm::bldg_order_load_passengers)
MH_EXPORT_REPLACE(llm_strat_bldg_order_unload_passengers, mh::orders::issue::promoted_arm::bldg_order_unload_passengers)
MH_EXPORT_REPLACE(llm_strat_bldg_order_load_resource, mh::orders::issue::promoted_arm::bldg_order_load_resource)
MH_EXPORT_REPLACE(llm_strat_bldg_order_unload_resource, mh::orders::issue::promoted_arm::bldg_order_unload_resource)
MH_EXPORT_REPLACE(llm_strat_bldg_order_port_depart, mh::orders::issue::promoted_arm::bldg_order_port_depart)
MH_EXPORT_REPLACE(llm_strat_bldg_order_port_depart_enqueue, mh::orders::issue::promoted_arm::bldg_order_port_depart_enqueue)
MH_EXPORT_REPLACE(llm_strat_bldg_order_mother_depart, mh::orders::issue::promoted_arm::bldg_order_mother_depart)
MH_EXPORT_REPLACE(llm_strat_bldg_order_mother_depart_enqueue, mh::orders::issue::promoted_arm::bldg_order_mother_depart_enqueue)
MH_EXPORT_REPLACE(llm_strat_bldg_order_depart_dispatch_by_type, mh::orders::issue::promoted_arm::bldg_order_depart_dispatch_by_type)
MH_EXPORT_REPLACE(llm_strat_bldg_order_start_research_project, mh::orders::issue::promoted_arm::bldg_order_start_research_project)
MH_EXPORT_REPLACE(llm_strat_order_grant_resource, mh::orders::issue::promoted_arm::order_grant_resource)
MH_EXPORT_REPLACE(llm_strat_order_population_delta_debug, mh::orders::issue::promoted_arm::order_population_delta_debug)
MH_EXPORT_REPLACE(llm_strat_order_collect_available_projects, mh::orders::issue::promoted_arm::order_collect_available_projects)
MH_EXPORT_REPLACE(llm_strat_order_recheck_projects, mh::orders::issue::promoted_arm::order_recheck_projects)
MH_EXPORT_REPLACE(llm_strat_order_recheck_buildings, mh::orders::issue::promoted_arm::order_recheck_buildings)
MH_EXPORT_REPLACE(llm_strat_order_recheck_planet_system_all_players, mh::orders::issue::promoted_arm::order_recheck_planet_system_all_players)
MH_EXPORT_REPLACE(llm_strat_order_credit_conquest_kills, mh::orders::issue::promoted_arm::order_credit_conquest_kills)
MH_EXPORT_REPLACE(llm_strat_order_set_player_relation, mh::orders::issue::promoted_arm::order_set_player_relation)
MH_EXPORT_REPLACE(llm_strat_order_set_player_control_mode, mh::orders::issue::promoted_arm::order_set_player_control_mode)
MH_EXPORT_REPLACE(llm_strat_order_debug_energy_refill_full, mh::orders::issue::promoted_arm::order_debug_energy_refill_full)
MH_EXPORT_REPLACE(llm_strat_order_debug_damage_scaled, mh::orders::issue::promoted_arm::order_debug_damage_scaled)
MH_EXPORT_REPLACE(llm_strat_order_debug_kill_group, mh::orders::issue::promoted_arm::order_debug_kill_group)
MH_EXPORT_REPLACE(llm_strat_order_fow_reveal_full, mh::orders::issue::promoted_arm::order_fow_reveal_full)
MH_EXPORT_REPLACE(llm_strat_ai_group_move_formation_rotating, mh::orders::issue::promoted_arm::group_move_formation_rotating)
MH_EXPORT_REPLACE(llm_strat_ai_unit_flag_and_move, mh::orders::issue::promoted_arm::unit_flag_and_move)
MH_EXPORT_REPLACE(llm_strat_ai_group_scatter_to_passable_tile, mh::orders::issue::promoted_arm::group_scatter_to_passable_tile)

namespace mh::orders::issue {

namespace {
bool g_any_installed = false;
} // namespace

bool promotion_active() { return g_any_installed; }

// O4-P: install this domain's 62-row closure as the LIVE implementation. Same shape as O3's
// mh::orders::install_promotion -- ONE switch for the whole domain (`[promote] orders_issue=1`),
// because Law 4's unit of replacement is the call-tree closure, not a function at a time -- plus a
// per-row escape hatch (`[promote_skip] <original name>=1`) for pulling ONE bad row back to stock
// without re-deriving a safe partial set for the other 61.
int install_promotion(int default_on) {
    if (default_on == 0) return 0;

    struct entry {
        const char *name;
        bool (*install)();
    };
    const entry seams[] = {
        {"llm_strat_bldg_order_depart_confirm_dispatch", mh_export_install_llm_strat_bldg_order_depart_confirm_dispatch},
        {"llm_strat_group_order_ack_voice", mh_export_install_llm_strat_group_order_ack_voice},
        {"llm_strat_group_issue_move_order_deferred", mh_export_install_llm_strat_group_issue_move_order_deferred},
        {"llm_strat_group_issue_move_order_confirmed", mh_export_install_llm_strat_group_issue_move_order_confirmed},
        {"llm_strat_bldg_footprint_random_point", mh_export_install_llm_strat_bldg_footprint_random_point},
        {"llm_strat_unit_order_move", mh_export_install_llm_strat_unit_order_move},
        {"llm_strat_unit_order_move_default", mh_export_install_llm_strat_unit_order_move_default},
        {"llm_strat_unit_order_scatter_from_spawn", mh_export_install_llm_strat_unit_order_scatter_from_spawn},
        {"llm_strat_unit_order_exit_storage", mh_export_install_llm_strat_unit_order_exit_storage},
        {"llm_strat_unit_order_move_confirmed_with_bump", mh_export_install_llm_strat_unit_order_move_confirmed_with_bump},
        {"llm_strat_unit_order_attack_target", mh_export_install_llm_strat_unit_order_attack_target},
        {"llm_strat_unit_order_attack_target_alt", mh_export_install_llm_strat_unit_order_attack_target_alt},
        {"llm_strat_unit_order_attack_unit", mh_export_install_llm_strat_unit_order_attack_unit},
        {"llm_strat_unit_order_attack_building_reposition", mh_export_install_llm_strat_unit_order_attack_building_reposition},
        {"llm_strat_unit_order_move_relative", mh_export_install_llm_strat_unit_order_move_relative},
        {"llm_strat_unit_order_auto_launch_from_storage", mh_export_install_llm_strat_unit_order_auto_launch_from_storage},
        {"llm_strat_order_create_unit_debug", mh_export_install_llm_strat_order_create_unit_debug},
        {"llm_strat_order_queue_construction_debug", mh_export_install_llm_strat_order_queue_construction_debug},
        {"llm_strat_bldg_order_production_add", mh_export_install_llm_strat_bldg_order_production_add},
        {"llm_strat_bldg_order_production_remove", mh_export_install_llm_strat_bldg_order_production_remove},
        {"llm_strat_turret_order_target_unit", mh_export_install_llm_strat_turret_order_target_unit},
        {"llm_strat_turret_order_target_building", mh_export_install_llm_strat_turret_order_target_building},
        {"llm_strat_bldg_order_assign_workers", mh_export_install_llm_strat_bldg_order_assign_workers},
        {"llm_strat_bldg_order_unassign_workers", mh_export_install_llm_strat_bldg_order_unassign_workers},
        {"llm_strat_bldg_order_activate", mh_export_install_llm_strat_bldg_order_activate},
        {"llm_strat_bldg_order_deactivate", mh_export_install_llm_strat_bldg_order_deactivate},
        {"llm_strat_bldg_order_deactivate_enqueue", mh_export_install_llm_strat_bldg_order_deactivate_enqueue},
        {"llm_strat_bldg_order_toggle_active", mh_export_install_llm_strat_bldg_order_toggle_active},
        {"llm_strat_bldg_order_upgrade", mh_export_install_llm_strat_bldg_order_upgrade},
        {"llm_strat_bldg_order_repair_cycle_start", mh_export_install_llm_strat_bldg_order_repair_cycle_start},
        {"llm_strat_bldg_order_hangar_recharge", mh_export_install_llm_strat_bldg_order_hangar_recharge},
        {"llm_strat_bldg_order_restart_construction", mh_export_install_llm_strat_bldg_order_restart_construction},
        {"llm_strat_bldg_order_purge_dead_docked", mh_export_install_llm_strat_bldg_order_purge_dead_docked},
        {"llm_strat_bldg_order_shuttle_depart", mh_export_install_llm_strat_bldg_order_shuttle_depart},
        {"llm_strat_bldg_order_shuttle_depart_enqueue", mh_export_install_llm_strat_bldg_order_shuttle_depart_enqueue},
        {"llm_strat_bldg_order_flush_cargo_hold", mh_export_install_llm_strat_bldg_order_flush_cargo_hold},
        {"llm_strat_bldg_order_load_passengers", mh_export_install_llm_strat_bldg_order_load_passengers},
        {"llm_strat_bldg_order_unload_passengers", mh_export_install_llm_strat_bldg_order_unload_passengers},
        {"llm_strat_bldg_order_load_resource", mh_export_install_llm_strat_bldg_order_load_resource},
        {"llm_strat_bldg_order_unload_resource", mh_export_install_llm_strat_bldg_order_unload_resource},
        {"llm_strat_bldg_order_port_depart", mh_export_install_llm_strat_bldg_order_port_depart},
        {"llm_strat_bldg_order_port_depart_enqueue", mh_export_install_llm_strat_bldg_order_port_depart_enqueue},
        {"llm_strat_bldg_order_mother_depart", mh_export_install_llm_strat_bldg_order_mother_depart},
        {"llm_strat_bldg_order_mother_depart_enqueue", mh_export_install_llm_strat_bldg_order_mother_depart_enqueue},
        {"llm_strat_bldg_order_depart_dispatch_by_type", mh_export_install_llm_strat_bldg_order_depart_dispatch_by_type},
        {"llm_strat_bldg_order_start_research_project", mh_export_install_llm_strat_bldg_order_start_research_project},
        {"llm_strat_order_grant_resource", mh_export_install_llm_strat_order_grant_resource},
        {"llm_strat_order_population_delta_debug", mh_export_install_llm_strat_order_population_delta_debug},
        {"llm_strat_order_collect_available_projects", mh_export_install_llm_strat_order_collect_available_projects},
        {"llm_strat_order_recheck_projects", mh_export_install_llm_strat_order_recheck_projects},
        {"llm_strat_order_recheck_buildings", mh_export_install_llm_strat_order_recheck_buildings},
        {"llm_strat_order_recheck_planet_system_all_players", mh_export_install_llm_strat_order_recheck_planet_system_all_players},
        {"llm_strat_order_credit_conquest_kills", mh_export_install_llm_strat_order_credit_conquest_kills},
        {"llm_strat_order_set_player_relation", mh_export_install_llm_strat_order_set_player_relation},
        {"llm_strat_order_set_player_control_mode", mh_export_install_llm_strat_order_set_player_control_mode},
        {"llm_strat_order_debug_energy_refill_full", mh_export_install_llm_strat_order_debug_energy_refill_full},
        {"llm_strat_order_debug_damage_scaled", mh_export_install_llm_strat_order_debug_damage_scaled},
        {"llm_strat_order_debug_kill_group", mh_export_install_llm_strat_order_debug_kill_group},
        {"llm_strat_order_fow_reveal_full", mh_export_install_llm_strat_order_fow_reveal_full},
        {"llm_strat_ai_group_move_formation_rotating", mh_export_install_llm_strat_ai_group_move_formation_rotating},
        {"llm_strat_ai_unit_flag_and_move", mh_export_install_llm_strat_ai_unit_flag_and_move},
        {"llm_strat_ai_group_scatter_to_passable_tile", mh_export_install_llm_strat_ai_group_scatter_to_passable_tile},
    };
    const int total = (int)(sizeof(seams) / sizeof(seams[0]));

    int attempted = 0, ok = 0;
    for (const entry &e : seams) {
        ++attempted;
        if (e.install()) {
            ++ok;
            say("; [promote] orders_issue: + %s\n", e.name);
        } else {
            // install_export_ok already logged WHY (entry-byte guard mismatch = the DLL was built
            // against a different image). Refusing loudly beats a half-promoted closure.
            say("; [promote] orders_issue: seam %s REFUSED -- NOT fully promoted\n", e.name);
        }
    }

    if (ok != attempted) {
        say("; [promote] orders_issue: %d/%d seams installed, %d skipped -- PARTIAL, treat this "
            "run as invalid\n",
            ok, attempted, total - attempted);
    } else {
        say("; [promote] orders_issue: ALL %d seams installed -- mh::orders::issue is LIVE\n", ok);
    }
    g_any_installed = ok > 0;
    return ok;
}

} // namespace mh::orders::issue
