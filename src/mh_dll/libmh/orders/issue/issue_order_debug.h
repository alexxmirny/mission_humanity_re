//
// orders/issue/issue_order_debug.h -- the `order_debug` unit's debug-console order wrappers
// (RI-ORDERS / O4A-C).
//
// Five wrappers: unit `order_debug` of the O4A-C sweep
// (tmp/decomp_orders_issue/_UNIT_order_debug.md). Every row is UI-rooted (debug-console-issued) or
// has no order_matrix entry at all -- none of the domain's soak/sp rig vehicles reaches them, so
// THIS UNIT DECLARES NO SHADOW SITE. They are proven OFFLINE against order_issue_golden.gen.h
// instead (all five confirmed row-for-row against the golden's g_node expressions before writing
// the .cpp -- see the .cpp for the addresses).
//
// None of the five reads game state or calls out to another original, so every `detail::` form
// below takes `(const issue_view &, const order_sink &s, const issue_calls &)` with the first and
// third named away -- issue_state.h's three-parameter rule (fixed by the O4A-C sweep) applies even
// though this unit's bodies use only `s`.
//
#pragma once
#include "orders/issue/issue_state.h"

namespace mh::orders::issue {

namespace detail {

// llm_strat_order_create_unit_debug @0x0046d800. Order 0xeb/0xeb ("spawn-unit-at-cursor" debug
// cheat, the game-mode notes). Owner is the FULL 16-bit MOVZX of param_4, no kind nibble OR'd in.
// unit_index is a literal 0. Scratch: field 4 = param_1, field 5 = param_2, field 1 = a2 (param
// names kept as Ghidra's own committed prototype -- the order 0xeb handler's field roles are not
// yet RE'd, so inventing names for them would be inventing semantics; see
// issue_bldg_orders.cpp's load_resource for the same call). Returns a CONSTANT 1, discarding
// dispatch's actual return value -- the original overwrites EAX with a literal immediately after
// the call and never reads dispatch's result.
int32_t order_create_unit_debug(const issue_view &, const order_sink &s, const issue_calls &,
                                int32_t param_1, int32_t param_2, int32_t a2, uint32_t param_4);

// llm_strat_order_queue_construction_debug @0x0046da00. Order 0xf2/0xf2 (debug-console
// queue-construction cheat -> llm_bldg_queue_construction). Same shape as order_create_unit_debug:
// owner is the full 16-bit MOVZX of param_4, unit_index a literal 0, constant-1 return. Scratch:
// field 4 = param_1, field 5 = param_2, field 0 = a2 -- note the THIRD write lands in slot 0 here,
// not slot 1 as in create_unit_debug.
int32_t order_queue_construction_debug(const issue_view &, const order_sink &s, const issue_calls &,
                                       int32_t param_1, int32_t param_2, int32_t a2,
                                       uint32_t param_4);

// llm_strat_order_population_delta_debug @0x0046f5ed. Order 0xec/0xec (debug-console population
// add/remove cheat, dispatches to llm_strat_population_add for positive, _remove for negative --
// NOT a building worker-count setter). Owner is the full 16-bit MOVZX of player_id. One scratch
// slot, field 1 = population_delta. Void return.
void order_population_delta_debug(const issue_view &, const order_sink &s, const issue_calls &,
                                  uint32_t player_id, int32_t population_delta);

// llm_strat_order_debug_energy_refill_full @0x0046fa9a. Order 0xf6/0xf6 (debug-console '_REPAIR'
// cheat -> llm_unit_bldg_energy_refill_full, a full HP/charge heal; ENERGY here is the HP-like
// stat, not the POWER resource -- docs/conventions.md#energy-is-not-power). Owner is player_id & 0xf, NO kind nibble -- unlike
// the two above, this one masks to the low nibble rather than a plain 16-bit MOVZX. Scratch: field
// 2 = player_id, field 3 = target_id. Void return.
void order_debug_energy_refill_full(const issue_view &, const order_sink &s, const issue_calls &,
                                    uint32_t player_id, int32_t target_id);

// llm_strat_order_debug_damage_scaled @0x0046fb5c. Order 0xf7/0xf7 (debug-console '_DESTROY' cheat
// -> llm_unit_bldg_apply_scaled_damage, a scaled damage add to .pending_damage -- it deals damage,
// it does not heal; the game-mode notes' 2026-07-05 correction). Same shape as
// order_debug_energy_refill_full: owner is player_id & 0xf. Scratch: field 2 = player_id, field 3 =
// damage_amount. Void return.
void order_debug_damage_scaled(const issue_view &, const order_sink &s, const issue_calls &,
                               uint32_t player_id, int32_t damage_amount);

} // namespace detail

int32_t order_create_unit_debug(int32_t param_1, int32_t param_2, int32_t a2, uint32_t param_4);
int32_t order_queue_construction_debug(int32_t param_1, int32_t param_2, int32_t a2,
                                       uint32_t param_4);
void    order_population_delta_debug(uint32_t player_id, int32_t population_delta);
void    order_debug_energy_refill_full(uint32_t player_id, int32_t target_id);
void    order_debug_damage_scaled(uint32_t player_id, int32_t damage_amount);

} // namespace mh::orders::issue
