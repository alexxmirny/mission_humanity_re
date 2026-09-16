//
// orders/issue/issue_unit_storage.h -- the unit-storage order wrappers (RI-ORDERS / O4-0, O4A-C
// sweep, batch B unit `unit_storage`). Two wrappers, both UI-rooted:
//
//   unit_order_exit_storage               sends a docked unit out of its storage building
//   unit_order_auto_launch_from_storage    the per-tick hangar auto-launch trigger
//
// THIS UNIT DECLARES NO SHADOW SITE -- every row is UI/debug-rooted with no order_matrix entry, so
// the domain's soak/sp rig vehicles never call it; both rows are proven OFFLINE against the
// generated golden only. See tmp/decomp_orders_issue/_UNIT_unit_storage.md.
//
#pragma once
#include "orders/issue/issue_state.h"
#include "orders/issue/issue_unit_move.h" // detail::unit_order_move -- exit_storage's fallback sibling call

namespace mh::orders::issue {

namespace detail {

// llm_strat_unit_order_exit_storage @0x0046a6b9 (607 B). Sends a docked unit out of storage.
//
// GUARD (the whole function is one if/else on this pair): if the storage building is not
// operational (buildings[..].online_state == 0) OR does not accept this unit's type
// (issue_calls.storage_type_accepts_unit(building_id, unit_proto_id) == 0) -- evaluated
// short-circuit, exactly as the original only calls storage_type_accepts_unit when online_state
// passed -- the function FALLS BACK to a plain move order (detail::unit_order_move, the sibling
// wrapper in issue_unit_move.h) to the storage's recorded exit tile, then stamps
// order_seq_id_by_player[player]: increment, and increment again if that wrapped to 0 (never emit
// seq id 0). This is one of the writers _G_LLM_STRAT_ORDER_SEQ_ID_BY_PLAYER's stamped-field
// discipline is waiting on -- get the wrap arithmetic exactly right.
//
// Otherwise (accepted) it dispatches directly: scratch slot 2 = storage_index always, then either
//   - order 0x24/0x38 to the storage's own default exit tile, when
//     cfg_units[unit_proto_id].move_op_arg == 0xa (elevation-unaffected unit classes), or
//   - order 0x29/0xb to an approach tile from issue_calls.storage_get_approach_tile, for
//     elevation-restricted classes -- but ONLY if units[..].elevation >= cfg_units[..].elevation;
//     that guard's failure is a full early return with NO dispatch and NO session-mode branch below.
// Either dispatch is followed by the domain's usual session-mode branch: a 0xfb/0xfb resync marker
// in the MP lockstep regime (session_mode == 3), else issue_calls.unit_notify_status(player,
// unit_index, 0).
void unit_order_exit_storage(const issue_view &v, const order_sink &s, const issue_calls &c,
                             uint32_t player, uint32_t unit_index, int32_t storage_index,
                             int32_t param_4, int32_t param_5);

// llm_strat_unit_order_auto_launch_from_storage @0x0046cd8b (417 B). The per-tick hangar
// auto-launch trigger. `state` (units[..].state) has NO Ghidra enum applied -- it is a raw dispatch
// index into a 255-entry function table (_G_LLM_STRAT_UNIT_STATE_FUNCS) -- so the two states this
// function discriminates on are transcribed as the literals the assembly compares, 0x1f and 0x22;
// see the .cpp's declared_needs note (this is also why the .c draft's `PARKED`/`EXIT_WAIT` names are
// NOT used here -- they are not backed by any Ghidra symbol; verified absent from mh_structs.gen.h).
//
//   state == 0x1f: if the unit's home storage's building is fully operational (built_flags == 3),
//     dispatch the unit's own cfg_units[..].default_op_code / arg 0x20, targeting either the
//     caller's explicit (x, y) when x != 0xffffffff, or the storage's own recorded exit tile + 2 on
//     each axis, in both cases masked by the map's toroidal width_mask/height_mask.
//   state == 0x22: unconditionally re-dispatch a fixed order 0x1f/0x23.
//   any other state: no-op (no golden site outside these two).
void unit_order_auto_launch_from_storage(const issue_view &v, const order_sink &s,
                                         const issue_calls &c, uint32_t player, int32_t unit_index,
                                         uint32_t x, uint32_t y);

} // namespace detail

void unit_order_exit_storage(uint32_t player, uint32_t unit_index, int32_t storage_index,
                             int32_t param_4, int32_t param_5);
void unit_order_auto_launch_from_storage(uint32_t player, int32_t unit_index, uint32_t x, uint32_t y);

} // namespace mh::orders::issue
