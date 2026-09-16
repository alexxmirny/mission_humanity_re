//
// orders/issue/issue_turret_orders.h -- turret target-acquisition order wrappers (RI-ORDERS / O4A-C,
// batch B, unit `turret_orders`).
//
// Two wrappers, both SIM-rooted (the order matrix via order_matrix.json issue_domains_rooted),
// so this unit owns a shadow site (both rows are armed). See issue_turret_orders.cpp for the shared
// shape and the target-kind encoding both bodies write into order-scratch field 5.
//
#pragma once

#include "orders/issue/issue_state.h"

namespace mh::orders::issue {

namespace detail {

// llm_strat_turret_order_target_unit @0x0046dd90 (96 B). Order 0x7b/0x7b, REPLICATED lane. Tags
// order-scratch field 5 with (target_side | KIND_UNIT) and field 6 with the target unit's index,
// written in that order (5 before 6, matching the original's two scratch_set_field calls). No guard.
void turret_order_target_unit(const issue_view &v, const order_sink &s, const issue_calls &c,
                              uint16_t player, uint16_t turret_bldg_idx, uint32_t target_side,
                              uint32_t target_unit_idx);

// llm_strat_turret_order_target_building @0x0046de50 (96 B). Identical shape to the sibling above:
// same order 0x7b/0x7b, same lane, same field-write order. Field 5 = target_side | KIND_BLDG
// instead, field 6 = target_bldg_idx. No guard.
void turret_order_target_building(const issue_view &v, const order_sink &s, const issue_calls &c,
                                  uint16_t player, uint16_t turret_bldg_idx, uint32_t target_side,
                                  uint32_t target_bldg_idx);


} // namespace detail

void turret_order_target_unit(uint16_t player, uint16_t turret_bldg_idx, uint32_t target_side,
                              uint32_t target_unit_idx);
void turret_order_target_building(uint16_t player, uint16_t turret_bldg_idx, uint32_t target_side,
                                  uint32_t target_bldg_idx);

} // namespace mh::orders::issue
