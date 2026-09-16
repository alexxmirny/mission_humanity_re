//
// orders/issue/issue_bldg_orders_b.h -- the "batch B" building order wrappers (RI-ORDERS / O4A-C).
//
// Thirteen wrappers: unit `bldg_orders_b` of the O4A-C sweep (tmp/decomp_orders_issue/_UNIT_bldg_orders_b.md).
// All UI-rooted (or, for toggle_active, calling only sibling wrappers), so none of the domain's
// soak/sp rig vehicles reaches them -- proven OFFLINE against order_issue_golden.gen.h instead.
// THIS UNIT DECLARES NO SHADOW SITE; see issue_bldg_orders_b.cpp's header comment.
//
// Every `detail::` form takes (view, sink, calls) as its first three parameters, always, even where
// a body uses only one or two of them (issue_state.h's rule, fixed by the O4A-C sweep) -- the unused
// ones are named away.
//
#pragma once
#include "orders/issue/issue_state.h"

namespace mh::orders::issue {

namespace detail {

// llm_strat_bldg_order_assign_workers @0x0046dfb6. Order 0x7e/0x7e, kind 0x40. One scratch slot
// (field 1 = count). Unguarded.
void bldg_order_assign_workers(const issue_view &v, const order_sink &s, const issue_calls &c,
                               uint16_t player, uint16_t building_index, uint32_t count);

// llm_strat_bldg_order_unassign_workers @0x0046e054. Order 0x7f/0x7f, kind 0x40. Same shape as
// assign_workers (one scratch slot, field 1 = count). Unguarded.
void bldg_order_unassign_workers(const issue_view &v, const order_sink &s, const issue_calls &c,
                                 uint16_t player, uint16_t building_index, uint32_t count);

// llm_strat_bldg_order_deactivate @0x0046e174. Order 0x81/0x81, kind 0x40. Constant triple, no
// guard, no scratch -- the "not _enqueue" twin of the pilot's bldg_order_deactivate_enqueue
// (REPLICATED lane here, IMMEDIATE lane there). Also the callee bldg_order_toggle_active below
// reaches on its "currently staffed" branch.
void bldg_order_deactivate(const issue_view &v, const order_sink &s, const issue_calls &c,
                           uint32_t player, uint16_t building_idx);

// llm_strat_bldg_order_toggle_active @0x0046e1f6. NO golden site -- emits nothing itself. Reads
// buildings[player][building_index].built_flags bit 0x2 and calls the pilot's
// detail::bldg_order_activate (old 2-param (v,s,...) shape -- called as declared, per the unit
// spec's hazard note) when the bit is clear, or this unit's own detail::bldg_order_deactivate above
// when it is set.
void bldg_order_toggle_active(const issue_view &v, const order_sink &s, const issue_calls &c,
                              uint16_t player, int32_t building_index);

// llm_strat_bldg_order_upgrade @0x0046e2ae. Order 0x82/0x82, kind 0x40. Constant triple, no guard,
// no scratch.
void bldg_order_upgrade(const issue_view &v, const order_sink &s, const issue_calls &c,
                        uint32_t player, uint32_t bldg_unit_id);

// llm_strat_bldg_order_hangar_recharge @0x0046e4e2. Order 0x79/0x79, kind 0x40. Constant triple, no
// guard, no scratch.
void bldg_order_hangar_recharge(const issue_view &v, const order_sink &s, const issue_calls &c,
                                uint32_t player, int32_t bldg_idx);

// llm_strat_bldg_order_restart_construction @0x0046e5e6. Order 0x6b/0x6b, kind 0x40. NOTE THE
// PARAMETER ROLES ARE SWAPPED relative to every other wrapper in this file: the FIRST parameter
// (`target_id`) is the one OR'd with the kind bit and sent as the owner, and the SECOND (`player`)
// is the one sent as the unit id. See the .cpp for the register trace and the golden-node
// cross-check; both the committed prototype and order_issue_golden.gen.h's N173-N176 agree.
void bldg_order_restart_construction(const issue_view &v, const order_sink &s, const issue_calls &c,
                                     uint32_t target_id, uint16_t player);

// llm_strat_bldg_order_purge_dead_docked @0x0046e6ea. Order 0xe6/0xe6, kind 0x40. Constant triple,
// no guard, no scratch.
void bldg_order_purge_dead_docked(const issue_view &v, const order_sink &s, const issue_calls &c,
                                  uint32_t player, uint16_t bldg_idx);

// llm_strat_bldg_order_flush_cargo_hold @0x0046ea1a. Order 0xdf/0xdf, kind 0x40. Constant triple,
// no guard, no scratch.
void bldg_order_flush_cargo_hold(const issue_view &v, const order_sink &s, const issue_calls &c,
                                 uint32_t player, uint32_t bldg_unit_id);

// llm_strat_bldg_order_load_passengers @0x0046ea9c. Order 0xdb/0xdb, kind 0x40. One scratch slot
// (field 1 = planet_idx). Unguarded.
void bldg_order_load_passengers(const issue_view &v, const order_sink &s, const issue_calls &c,
                                uint16_t player, uint16_t bldg_idx, int32_t planet_idx);

// llm_strat_bldg_order_unload_passengers @0x0046eb3a. Order 0xdc/0xdc, kind 0x40. One scratch slot
// (field 1 = passenger_count). Unguarded.
void bldg_order_unload_passengers(const issue_view &v, const order_sink &s, const issue_calls &c,
                                  uint32_t player, uint16_t bldg_idx, int32_t passenger_count);

// llm_strat_bldg_order_unload_resource @0x0046ec92. Order 0xde/0xde, kind 0x40. TWO scratch slots,
// written 3 (resource_slot) then 2 (count), in that order. Unguarded.
void bldg_order_unload_resource(const issue_view &v, const order_sink &s, const issue_calls &c,
                                uint32_t player, uint16_t bldg_idx, int32_t resource_slot,
                                int32_t count);

// llm_strat_bldg_order_start_research_project @0x0046f140. Order 0x89/0x89, kind 0x40. One scratch
// slot (field 7 = project id). Unguarded.
void bldg_order_start_research_project(const issue_view &v, const order_sink &s,
                                       const issue_calls &c, uint32_t player, uint16_t bldg_idx,
                                       int32_t project_id);

} // namespace detail

void bldg_order_assign_workers(uint16_t player, uint16_t building_index, uint32_t count);
void bldg_order_unassign_workers(uint16_t player, uint16_t building_index, uint32_t count);
void bldg_order_deactivate(uint32_t player, uint16_t building_idx);
void bldg_order_toggle_active(uint16_t player, int32_t building_index);
void bldg_order_upgrade(uint32_t player, uint32_t bldg_unit_id);
void bldg_order_hangar_recharge(uint32_t player, int32_t bldg_idx);
void bldg_order_restart_construction(uint32_t target_id, uint16_t player);
void bldg_order_purge_dead_docked(uint32_t player, uint16_t bldg_idx);
void bldg_order_flush_cargo_hold(uint32_t player, uint32_t bldg_unit_id);
void bldg_order_load_passengers(uint16_t player, uint16_t bldg_idx, int32_t planet_idx);
void bldg_order_unload_passengers(uint32_t player, uint16_t bldg_idx, int32_t passenger_count);
void bldg_order_unload_resource(uint32_t player, uint16_t bldg_idx, int32_t resource_slot,
                                int32_t count);
void bldg_order_start_research_project(uint32_t player, uint16_t bldg_idx, int32_t project_id);

} // namespace mh::orders::issue
