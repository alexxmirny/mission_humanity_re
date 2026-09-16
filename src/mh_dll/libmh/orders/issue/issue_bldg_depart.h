//
// orders/issue/issue_bldg_depart.h -- the building "depart" order family (RI-ORDERS / O4A-C).
//
// Eight originals: three DEPART wrappers (shuttle/port/mother), their _enqueue twins (six rows, one
// golden site each), the type-dispatcher that routes to all six by the target building's cfg TYPE,
// and the UI-facing confirm-dispatch that drains the two staged UI hand-off slots into the
// dispatcher. The last two have no golden site of their own -- see
// tmp/decomp_orders_issue/_UNIT_bldg_depart.md and the .cpp for why.
//
#pragma once
#include "orders/issue/issue_state.h"

namespace mh::orders::issue {

namespace detail {

// ---- llm_strat_bldg_order_shuttle_depart @0x0046e840 -- REPLICATED lane. Order 0xd2/0xd2, scratch
// slot 6 = dest_planet_idx. Unconditional (no guard, no other scratch write). Owner byte built from
// the FULL dword `player` (OR AL,0x40); `bldg_idx` is narrowed to 16 bits before the dispatch call,
// the same two-widths-of-one-parameter shape issue_bldg_orders.cpp documents for bldg_at().
void bldg_order_shuttle_depart(const issue_view &v, const order_sink &s, const issue_calls &c,
                               uint32_t player, uint16_t bldg_idx, int32_t dest_planet_idx);
// llm_strat_bldg_order_shuttle_depart_enqueue @0x0046e88f -- the IMMEDIATE-lane twin: calls
// llm_strat_order_enqueue DIRECTLY, bypassing the replicated-lane router. Same order/scratch shape.
void bldg_order_shuttle_depart_enqueue(const issue_view &v, const order_sink &s,
                                       const issue_calls &c, uint32_t player, uint16_t bldg_idx,
                                       int32_t dest_planet_idx);

// llm_strat_bldg_order_port_depart @0x0046ed4c -- REPLICATED lane. Order 0xd3/0xd3, scratch slot 6.
// Unconditional AT THIS FUNCTION'S OWN LEVEL -- the "not the currently-viewed planet" guard belongs
// to its caller, bldg_order_depart_dispatch_by_type, not to this wrapper. See the .cpp.
void bldg_order_port_depart(const issue_view &v, const order_sink &s, const issue_calls &c,
                            uint32_t player, uint16_t bldg_idx, int32_t dest_planet_idx);
// llm_strat_bldg_order_port_depart_enqueue @0x0046ed9b -- IMMEDIATE-lane twin.
void bldg_order_port_depart_enqueue(const issue_view &v, const order_sink &s, const issue_calls &c,
                                    uint32_t player, uint16_t bldg_idx, int32_t dest_planet_idx);

// llm_strat_bldg_order_mother_depart @0x0046edea -- REPLICATED lane. Order 0xd4/0xd4, scratch slot 6.
// Unconditional, same shape as shuttle_depart above.
void bldg_order_mother_depart(const issue_view &v, const order_sink &s, const issue_calls &c,
                              uint32_t player, uint16_t bldg_idx, int32_t dest_planet_idx);
// llm_strat_bldg_order_mother_depart_enqueue @0x0046ee39 -- IMMEDIATE-lane twin.
void bldg_order_mother_depart_enqueue(const issue_view &v, const order_sink &s,
                                      const issue_calls &c, uint32_t player, uint16_t bldg_idx,
                                      int32_t dest_planet_idx);

// llm_strat_bldg_order_depart_dispatch_by_type @0x0046ee88 -- NOT a thin order thunk: routes to one
// of the six wrappers above by cfg_buildings[buildings[player][bldg_idx].building_id].type. No
// golden site of its own (it emits no order directly). See the .cpp for the exact type values and
// the port-only "not the current planet" guard this function owns.
void bldg_order_depart_dispatch_by_type(const issue_view &v, const order_sink &s,
                                        const issue_calls &c, uint32_t player, int32_t bldg_idx,
                                        int32_t dest_planet_idx);

// llm_strat_bldg_order_depart_confirm_dispatch @0x00415b41 -- the UI hand-off: drains whichever of
// the two staged building slots (ui_depart_pending_bldg_a, tested first, else _b) is nonzero into
// bldg_order_depart_dispatch_by_type, then clears the slot it used; ui_planet_sel_action_target is
// cleared UNCONDITIONALLY at the end regardless of which branch ran, or whether either did. No
// golden site of its own (it emits no order directly; dispatch_by_type does, or does not). See the
// .cpp for the exact read-then-clear ordering.
void bldg_order_depart_confirm_dispatch(const issue_view &v, const order_sink &s,
                                        const issue_calls &c);

// inert_calls() is required here, NOT live_calls(): the three plain (UI-rooted) rows and the two
// routers have no golden site and are not order_matrix entries, so their evidence is the offline
// oracle. Recorded O4A-C 2026-08-28; the arming half of that note went with the oracle at F2D.

} // namespace detail

void bldg_order_shuttle_depart(uint32_t player, uint16_t bldg_idx, int32_t dest_planet_idx);
void bldg_order_shuttle_depart_enqueue(uint32_t player, uint16_t bldg_idx, int32_t dest_planet_idx);
void bldg_order_port_depart(uint32_t player, uint16_t bldg_idx, int32_t dest_planet_idx);
void bldg_order_port_depart_enqueue(uint32_t player, uint16_t bldg_idx, int32_t dest_planet_idx);
void bldg_order_mother_depart(uint32_t player, uint16_t bldg_idx, int32_t dest_planet_idx);
void bldg_order_mother_depart_enqueue(uint32_t player, uint16_t bldg_idx, int32_t dest_planet_idx);
void bldg_order_depart_dispatch_by_type(uint32_t player, int32_t bldg_idx, int32_t dest_planet_idx);
void bldg_order_depart_confirm_dispatch();

} // namespace mh::orders::issue
