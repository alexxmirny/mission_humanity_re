//
// orders/issue/issue_unit_move.h -- the single-unit move-order wrappers (RI-ORDERS / O4-0, O4A-C
// sweep, batch B unit `unit_move`). Five wrappers:
//
//   unit_order_move                       per-unit-type op code, taken from the unit's own proto row
//   unit_order_move_default               same shape, but op_code is the constant 0x10
//   unit_order_move_confirmed_with_bump   op_code 0x18, PLUS a local-viewer bump/collision sound
//   unit_order_move_relative              picks a near (0x1/0x33) or far (0x33/0xa) order by the
//                                          toroidal Chebyshev distance to the target
//   unit_order_scatter_from_spawn         op_code 0x10, IMMEDIATE lane (enqueue, not dispatch)
//
// THIS UNIT OWNS A SHADOW SITE (`install_shadow_issue_unit_move`, below): it arms every SIM/AI-rooted
// row -- unit_order_move, _move_default, _move_confirmed_with_bump, _move_relative. It does NOT arm
// unit_order_scatter_from_spawn, which is debug-rooted and unreachable by the soak/sp rig vehicles;
// that row stays proven OFFLINE only, against the generated golden. See
// tmp/decomp_orders_issue/_UNIT_unit_move.md.
//
#pragma once
#include "orders/issue/issue_state.h"

namespace mh::orders::issue {

namespace detail {

// llm_strat_unit_order_move @0x00469fe6 (239 B). REPLICATED lane.
void unit_order_move(const issue_view &v, const order_sink &s, const issue_calls &c, uint32_t player,
                     int32_t unit_idx, int32_t target_x, int32_t target_y, int32_t group_id);

// llm_strat_unit_order_move_default @0x0046a402 (201 B). REPLICATED lane.
void unit_order_move_default(const issue_view &v, const order_sink &s, const issue_calls &c,
                             uint32_t player, int32_t unit_idx, int32_t target_x, int32_t target_y,
                             int32_t group_id);

// llm_strat_unit_order_move_confirmed_with_bump @0x0046accf (315 B). REPLICATED lane. Calls
// issue_calls.snd_play -- the domain's ONE effectful callee -- for the local-viewer bump sound.
void unit_order_move_confirmed_with_bump(const issue_view &v, const order_sink &s,
                                         const issue_calls &c, uint32_t player, int32_t unit_idx,
                                         int32_t target_x, int32_t target_y);

// llm_strat_unit_order_move_relative @0x0046c87a (291 B). REPLICATED lane (far branch only). Uses
// issue_calls.tile_delta_wrapped for the toroidal wrap -- NOT issue_view.geom directly.
void unit_order_move_relative(const issue_view &v, const order_sink &s, const issue_calls &c,
                              uint32_t player, int32_t unit_idx, int32_t target_x, int32_t target_y);

// llm_strat_unit_order_scatter_from_spawn @0x0046a626 (147 B). IMMEDIATE lane (enqueue) despite
// carrying no `_enqueue` suffix in its name -- transcribed from the CALL target, not the name.
void unit_order_scatter_from_spawn(const issue_view &v, const order_sink &s, const issue_calls &c,
                                   uint16_t player, int32_t unit_idx, int32_t target_x,
                                   int32_t target_y);

// This unit's shadow-site aggregator; see the file banner above for which rows it arms.

} // namespace detail

void unit_order_move(uint32_t player, int32_t unit_idx, int32_t target_x, int32_t target_y,
                     int32_t group_id);
void unit_order_move_default(uint32_t player, int32_t unit_idx, int32_t target_x, int32_t target_y,
                             int32_t group_id);
void unit_order_move_confirmed_with_bump(uint32_t player, int32_t unit_idx, int32_t target_x,
                                         int32_t target_y);
void unit_order_move_relative(uint32_t player, int32_t unit_idx, int32_t target_x, int32_t target_y);
void unit_order_scatter_from_spawn(uint16_t player, int32_t unit_idx, int32_t target_x,
                                   int32_t target_y);

} // namespace mh::orders::issue
