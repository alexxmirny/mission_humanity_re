//
// orders/issue/issue_bldg_orders_b.cpp -- the "batch B" building order wrappers (RI-ORDERS / O4A-C).
//
// Thirteen wrappers translated for unit `bldg_orders_b`
// (tmp/decomp_orders_issue/_UNIT_bldg_orders_b.md): assign/unassign workers, deactivate,
// toggle_active, upgrade, hangar_recharge, restart_construction, purge_dead_docked,
// flush_cargo_hold, load_passengers, unload_passengers, unload_resource, start_research_project.
//
// DERIVED FROM THE .asm, not the .c -- two of the thirteen drafts carry a stale/mismatched PLATE
// comment (see the per-function notes below); the code paths themselves matched the assembly.
//
// THE ARGUMENT SHAPE IS THE SAME AS THE PILOT'S SIX at eleven of these thirteen sites: the original
// loads ECX = order_code, EBX = param0, EAX = player; OR AL,0x40; MOVZX EDX,AX (owner_and_kind),
// then EAX = MOVZX <bldg index> (unit_id), and calls
// dispatch(unit_id, owner_and_kind, op_code=param0, arg=order_code). param0 == order_code at every
// site in this file (no order_debug_kill_group-style split). ONE site --
// bldg_order_restart_construction -- swaps which parameter plays which role; see its own comment.
//
// PLAYER IS NARROWED TO 16 BITS BEFORE THE index MOVZX (`MOVZX EAX,word ptr [...]`) but the OWNER
// byte is built from the FULL dword the parameter arrived in (`MOV EAX,dword ptr [...]` / `OR
// AL,0x40`), exactly as issue_bldg_orders.cpp documents for the pilot six. Preserved here the same
// way, via the same `owner_of`/`bldg_at` shape (re-declared locally -- anonymous-namespace helpers
// don't cross translation units).
//
#include "orders/issue/issue_bldg_orders_b.h"

namespace mh::orders::issue {

namespace {

// buildings[player][index], the original's own arithmetic: player narrowed to 16 bits before the
// index multiply, index not. `0x6aa4` == BUILDINGS_PER_PLAYER * sizeof(building). Identical to the
// pilot's `bldg_at()` in issue_bldg_orders.cpp; redeclared here because this is a separate TU.
const mh::game::mh_map_object_building &bldg_at(const issue_view &v, uint32_t player,
                                                int32_t index) {
    return v.buildings[(int32_t)(uint16_t)player * v.caps.buildings + index];
}

// The owner byte the container receives: the kind nibble OR'd into the low byte of `player`.
uint16_t owner_of(uint32_t player, uint32_t kind) { return (uint16_t)(player | kind); }

} // namespace

// ---- llm_strat_bldg_order_assign_workers @0x0046dfb6 --------------------------------------------
// Order 0x7e/0x7e, kind 0x40. Unguarded. One scratch slot: field 1 = count (the requested worker
// headcount -- 10000 / 1 from the two callers per the .c draft's plate, which matches the
// assembly here).
void detail::bldg_order_assign_workers(const issue_view &, const order_sink &s, const issue_calls &,
                                       uint16_t player, uint16_t building_index, uint32_t count) {
    s.scratch_set_field(1, (int32_t)count);
    s.dispatch(building_index, owner_of(player, KIND_BLDG), 0x7e, 0x7e);
}

// ---- llm_strat_bldg_order_unassign_workers @0x0046e054 ------------------------------------------
// Order 0x7f/0x7f, kind 0x40. Same shape as assign_workers -- one scratch slot, field 1 = count.
void detail::bldg_order_unassign_workers(const issue_view &, const order_sink &s,
                                         const issue_calls &, uint16_t         player,
                                         uint16_t building_index, uint32_t count) {
    s.scratch_set_field(1, (int32_t)count);
    s.dispatch(building_index, owner_of(player, KIND_BLDG), 0x7f, 0x7f);
}

// ---- llm_strat_bldg_order_deactivate @0x0046e174 ------------------------------------------------
// Order 0x81/0x81, kind 0x40. Unconditional: no guard, no scratch. The REPLICATED-lane twin of the
// pilot's bldg_order_deactivate_enqueue (which calls llm_strat_order_enqueue instead) -- same order
// code, different lane. Also the callee bldg_order_toggle_active reaches below when the building is
// currently staffed.
void detail::bldg_order_deactivate(const issue_view &, const order_sink &s, const issue_calls &,
                                   uint32_t player, uint16_t building_idx) {
    s.dispatch(building_idx, owner_of(player, KIND_BLDG), 0x81, 0x81);
}

// ---- llm_strat_bldg_order_toggle_active @0x0046e1f6 ----------------------------------------------
// NO golden site -- this function emits no order itself (order_matrix has no entry for it); it only
// reads state and calls one of its two siblings. Per the unit spec's hazard note.
//
// GUARD: `TEST byte [buildings[player][building_index].built_flags],0x2` / `JZ`. When bit 0x2 is
// CLEAR (JZ taken), it calls llm_strat_bldg_order_activate (the pilot's detail::bldg_order_activate,
// OLD two-parameter (v,s,...) shape -- called exactly as declared in issue_state.h, per the unit
// spec). When bit 0x2 is SET (fall-through), it calls THIS unit's own detail::bldg_order_deactivate
// above (three-parameter shape, since it is one of ours). mh_structs.gen.h documents built_flags bit
// 0x2 as "staffed/has-workers" (set via llm_strat_bldg_set_staffed_flag) -- the .c draft's plate
// speculates a "power-connection" reading instead; the struct's own field comment is the more
// specific source and this comment defers to it without asserting a new interpretation either way,
// since it is not load-bearing for the translation (the bit test is identical regardless of name).
void detail::bldg_order_toggle_active(const issue_view &v, const order_sink &s,
                                      const issue_calls &c, uint16_t player,
                                      int32_t building_index) {
    if ((bldg_at(v, player, building_index).built_flags & 2) == 0) {
        detail::bldg_order_activate(v, s, player, (uint16_t)building_index); // pilot's 2-param form
    } else {
        detail::bldg_order_deactivate(v, s, c, player, (uint16_t)building_index);
    }
}

// ---- llm_strat_bldg_order_upgrade @0x0046e2ae ----------------------------------------------------
// Order 0x82/0x82, kind 0x40. Unconditional: no guard, no scratch.
void detail::bldg_order_upgrade(const issue_view &, const order_sink &s, const issue_calls &,
                                uint32_t player, uint32_t bldg_unit_id) {
    s.dispatch((uint16_t)bldg_unit_id, owner_of(player, KIND_BLDG), 0x82, 0x82);
}

// ---- llm_strat_bldg_order_hangar_recharge @0x0046e4e2 --------------------------------------------
// Order 0x79/0x79, kind 0x40. Unconditional: no guard, no scratch.
void detail::bldg_order_hangar_recharge(const issue_view &, const order_sink &s,
                                        const issue_calls &, uint32_t player, int32_t bldg_idx) {
    s.dispatch((uint16_t)bldg_idx, owner_of(player, KIND_BLDG), 0x79, 0x79);
}

// ---- llm_strat_bldg_order_restart_construction @0x0046e5e6 ---------------------------------------
// Order 0x6b/0x6b, kind 0x40. Unconditional: no guard, no scratch. THE .c DRAFT'S PLATE IS WRONG --
// it claims "order 0x83" (that is bldg_order_cancel_reset's code, a different sibling function); the
// assembly here loads 0x6b (`MOV ECX,0x6b` / `MOV EBX,0x6b`) at both 0x0046e603 and 0x0046e608.
//
// PARAMETER ROLES ARE SWAPPED relative to every other wrapper in this file. The register trace:
//   MOV EAX,[EBP-0x14]   ; EAX = target_id (param 0, storage EAX:4)
//   OR AL,0x40
//   MOVZX EDX,AX          ; EDX (owner_and_kind) = target_id | 0x40
//   MOVZX EAX,word[EBP-0x18]  ; EAX (unit_id) = player (param 1, storage DX:2)
//   CALL llm_strat_order_dispatch
// so `target_id` (param 0) is the one OR'd with the kind bit and sent as the OWNER, while `player`
// (param 1) is sent as the UNIT ID -- backwards from assign_workers/deactivate/upgrade/etc., where
// the low-numbered "player" parameter becomes the owner and the building index becomes the unit id.
// Cross-checked against order_issue_golden.gen.h's N173-N176 (llm_strat_bldg_order_restart_
// construction's row): N173 = MOVZX16(PARAM[1]) -> unit_id, N174 = MOVZX16(PARAM[0] OR8 0x40) ->
// owner_and_kind, N175/N176 = IMM 107 (0x6b). Both sources agree; this is not left as an
// uncertainty.
void detail::bldg_order_restart_construction(const issue_view &, const order_sink &s,
                                             const issue_calls &, uint32_t         target_id,
                                             uint16_t player) {
    s.dispatch(player, owner_of(target_id, KIND_BLDG), 0x6b, 0x6b);
}

// ---- llm_strat_bldg_order_purge_dead_docked @0x0046e6ea ------------------------------------------
// Order 0xe6/0xe6, kind 0x40. Unconditional: no guard, no scratch.
void detail::bldg_order_purge_dead_docked(const issue_view &, const order_sink &s,
                                          const issue_calls &, uint32_t         player,
                                          uint16_t bldg_idx) {
    s.dispatch(bldg_idx, owner_of(player, KIND_BLDG), 0xe6, 0xe6);
}

// ---- llm_strat_bldg_order_flush_cargo_hold @0x0046ea1a -------------------------------------------
// Order 0xdf/0xdf, kind 0x40. Unconditional: no guard, no scratch.
void detail::bldg_order_flush_cargo_hold(const issue_view &, const order_sink &s,
                                         const issue_calls &, uint32_t         player,
                                         uint32_t bldg_unit_id) {
    s.dispatch((uint16_t)bldg_unit_id, owner_of(player, KIND_BLDG), 0xdf, 0xdf);
}

// ---- llm_strat_bldg_order_load_passengers @0x0046ea9c --------------------------------------------
// Order 0xdb/0xdb, kind 0x40. Unguarded. One scratch slot: field 1 = planet_idx.
void detail::bldg_order_load_passengers(const issue_view &, const order_sink &s,
                                        const issue_calls &, uint16_t player, uint16_t bldg_idx,
                                        int32_t planet_idx) {
    s.scratch_set_field(1, planet_idx);
    s.dispatch(bldg_idx, owner_of(player, KIND_BLDG), 0xdb, 0xdb);
}

// ---- llm_strat_bldg_order_unload_passengers @0x0046eb3a ------------------------------------------
// Order 0xdc/0xdc, kind 0x40. Unguarded. One scratch slot: field 1 = passenger_count.
void detail::bldg_order_unload_passengers(const issue_view &, const order_sink &s,
                                          const issue_calls &, uint32_t player, uint16_t bldg_idx,
                                          int32_t passenger_count) {
    s.scratch_set_field(1, passenger_count);
    s.dispatch(bldg_idx, owner_of(player, KIND_BLDG), 0xdc, 0xdc);
}

// ---- llm_strat_bldg_order_unload_resource @0x0046ec92 --------------------------------------------
// Order 0xde/0xde, kind 0x40. Unguarded. THE .c DRAFT'S PLATE IS WRONG -- it claims this is "the
// enqueue variant of the unconfirmed order 0xd5" (that is llm_strat_order_issue_0xd5's pair, a
// different sibling entirely, at 0x0046e8de/0x0046e93b); the assembly here loads 0xde
// (`MOV ECX,0xde` / `MOV EBX,0xde` @0x0046eccd/0x0046ecd2) and this IS the dispatch (non-enqueue)
// variant -- `llm_strat_bldg_order_unload_resource_enqueue` at 0x0046ecef is its actual sibling.
// TWO scratch slots, written 3 THEN 2, in that order (`MOV EAX,0x3` precedes `MOV EAX,0x2` in the
// listing) -- args[0] param is `resource_slot` (-> slot 3), args[1] param is `count` (-> slot 2).
// Cross-checked against order_issue_golden.gen.h SC234 (slot 3 = PARAM[2], slot 2 = PARAM[3]).
void detail::bldg_order_unload_resource(const issue_view &, const order_sink &s,
                                        const issue_calls &, uint32_t player, uint16_t bldg_idx,
                                        int32_t resource_slot, int32_t count) {
    s.scratch_set_field(3, resource_slot);
    s.scratch_set_field(2, count);
    s.dispatch(bldg_idx, owner_of(player, KIND_BLDG), 0xde, 0xde);
}

// ---- llm_strat_bldg_order_start_research_project @0x0046f140 -------------------------------------
// Order 0x89/0x89, kind 0x40. Unguarded. One scratch slot: field 7 = project_id.
void detail::bldg_order_start_research_project(const issue_view &, const order_sink &s,
                                               const issue_calls &, uint32_t         player,
                                               uint16_t bldg_idx, int32_t project_id) {
    s.scratch_set_field(7, project_id);
    s.dispatch(bldg_idx, owner_of(player, KIND_BLDG), 0x89, 0x89);
}

// ---- the public forms -----------------------------------------------------------------------------

void bldg_order_assign_workers(uint16_t player, uint16_t building_index, uint32_t count) {
    detail::bldg_order_assign_workers(live_view(), live_sink(), live_calls(), player,
                                      building_index, count);
}
void bldg_order_unassign_workers(uint16_t player, uint16_t building_index, uint32_t count) {
    detail::bldg_order_unassign_workers(live_view(), live_sink(), live_calls(), player,
                                        building_index, count);
}
void bldg_order_deactivate(uint32_t player, uint16_t building_idx) {
    detail::bldg_order_deactivate(live_view(), live_sink(), live_calls(), player, building_idx);
}
void bldg_order_toggle_active(uint16_t player, int32_t building_index) {
    detail::bldg_order_toggle_active(live_view(), live_sink(), live_calls(), player,
                                     building_index);
}
void bldg_order_upgrade(uint32_t player, uint32_t bldg_unit_id) {
    detail::bldg_order_upgrade(live_view(), live_sink(), live_calls(), player, bldg_unit_id);
}
void bldg_order_hangar_recharge(uint32_t player, int32_t bldg_idx) {
    detail::bldg_order_hangar_recharge(live_view(), live_sink(), live_calls(), player, bldg_idx);
}
void bldg_order_restart_construction(uint32_t target_id, uint16_t player) {
    detail::bldg_order_restart_construction(live_view(), live_sink(), live_calls(), target_id,
                                            player);
}
void bldg_order_purge_dead_docked(uint32_t player, uint16_t bldg_idx) {
    detail::bldg_order_purge_dead_docked(live_view(), live_sink(), live_calls(), player, bldg_idx);
}
void bldg_order_flush_cargo_hold(uint32_t player, uint32_t bldg_unit_id) {
    detail::bldg_order_flush_cargo_hold(live_view(), live_sink(), live_calls(), player,
                                        bldg_unit_id);
}
void bldg_order_load_passengers(uint16_t player, uint16_t bldg_idx, int32_t planet_idx) {
    detail::bldg_order_load_passengers(live_view(), live_sink(), live_calls(), player, bldg_idx,
                                       planet_idx);
}
void bldg_order_unload_passengers(uint32_t player, uint16_t bldg_idx, int32_t passenger_count) {
    detail::bldg_order_unload_passengers(live_view(), live_sink(), live_calls(), player, bldg_idx,
                                         passenger_count);
}
void bldg_order_unload_resource(uint32_t player, uint16_t bldg_idx, int32_t resource_slot,
                                int32_t count) {
    detail::bldg_order_unload_resource(live_view(), live_sink(), live_calls(), player, bldg_idx,
                                       resource_slot, count);
}
void bldg_order_start_research_project(uint32_t player, uint16_t bldg_idx, int32_t project_id) {
    detail::bldg_order_start_research_project(live_view(), live_sink(), live_calls(), player,
                                              bldg_idx, project_id);
}

} // namespace mh::orders::issue
