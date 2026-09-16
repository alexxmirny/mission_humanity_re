//
// orders/issue/issue_order_debug.cpp -- the `order_debug` unit's debug-console order wrappers
// (RI-ORDERS / O4A-C).
//
// Five wrappers, all debug-console-issued cheats. None reads game state and none calls out to
// another original -- every body is scratch_reset (sometimes), a fixed count of scratch_set_field
// writes, then one dispatch with a fixed order-code pair. THIS UNIT DECLARES NO SHADOW SITE (see
// the unit spec, tmp/decomp_orders_issue/_UNIT_order_debug.md): every row here is UI/debug-rooted,
// so the domain's soak/sp rig vehicles never reach them -- proven OFFLINE against
// order_issue_golden.gen.h instead. Every row below was checked against that golden's g_node
// expressions (N252-N255/SC262, N424-N427/SC434, N417-N420/SC423, N312-N315/SC320, N303-N306/SC311)
// before this file was written, and all five agree with the transcription here.
//
// DERIVED FROM THE .asm, not the .c drafts (which independently agree on every line for these five
// -- no discrepancy found).
//
#include "orders/issue/issue_order_debug.h"

namespace mh::orders::issue {

// ---- llm_strat_order_create_unit_debug @0x0046d800 -----------------------------------------------
// Unconditional. scratch_reset() first, then THREE writes in this order: field 4 = param_1, field 5
// = param_2, field 1 = a2. unit_index is a literal 0 (`XOR EAX,EAX`); owner is param_4 truncated to
// 16 bits by a plain MOVZX (`MOVZX EDX,word ptr [EBP-0xc]`) -- NO kind nibble OR'd in, unlike the
// building-order family. Order 0xeb/0xeb (both param0 and order_code are the same constant here).
//
// The epilogue stores a LITERAL 1 into the return slot AFTER the dispatch call and never reads
// dispatch's own return value (`CALL dispatch` / `MOV [EBP-0x10],0x1` / `MOV EAX,[EBP-0x10]`) --
// so the function's return is unconditionally 1, not "whatever dispatch returned".
int32_t detail::order_create_unit_debug(const issue_view &, const order_sink &s, const issue_calls &,
                                        int32_t param_1, int32_t param_2, int32_t a2,
                                        uint32_t param_4) {
    s.scratch_reset();
    s.scratch_set_field(4, param_1);
    s.scratch_set_field(5, param_2);
    s.scratch_set_field(1, a2);
    s.dispatch(0, (uint16_t)param_4, 0xeb, 0xeb);
    return 1;
}

// ---- llm_strat_order_queue_construction_debug @0x0046da00 -----------------------------------------
// Same shape as order_create_unit_debug: scratch_reset() first, unit_index a literal 0, owner a
// plain 16-bit MOVZX of param_4 (no kind nibble), constant-1 return regardless of dispatch's result.
// The THIRD scratch write differs: field 4 = param_1, field 5 = param_2, field 0 = a2 -- slot 0
// here, not slot 1. Order 0xf2/0xf2.
int32_t
detail::order_queue_construction_debug(const issue_view &, const order_sink &s, const issue_calls &,
                                       int32_t param_1, int32_t param_2, int32_t a2,
                                       uint32_t param_4) {
    s.scratch_reset();
    s.scratch_set_field(4, param_1);
    s.scratch_set_field(5, param_2);
    s.scratch_set_field(0, a2);
    s.dispatch(0, (uint16_t)param_4, 0xf2, 0xf2);
    return 1;
}

// ---- llm_strat_order_population_delta_debug @0x0046f5ed -------------------------------------------
// Unconditional. scratch_reset() first, ONE scratch write (field 1 = population_delta). unit_index
// a literal 0, owner a plain 16-bit MOVZX of player_id (no kind nibble). Order 0xec/0xec. Void
// return (the epilogue never touches EAX after the dispatch call).
void detail::order_population_delta_debug(const issue_view &, const order_sink &s,
                                          const issue_calls &, uint32_t         player_id,
                                          int32_t population_delta) {
    s.scratch_reset();
    s.scratch_set_field(1, population_delta);
    s.dispatch(0, (uint16_t)player_id, 0xec, 0xec);
}

// ---- llm_strat_order_debug_energy_refill_full @0x0046fa9a -----------------------------------------
// Unconditional. scratch_reset() first, TWO scratch writes in this order: field 2 = player_id, field
// 3 = target_id. unit_index a literal 0. Owner is `player_id & 0xf` -- an AND-then-MOVZX, NOT the
// plain 16-bit MOVZX the three functions above use (`MOV EAX,[EBP-0x18]` / `AND EAX,0xf` / `MOVZX
// EDX,AX`). Order 0xf6/0xf6. Void return.
void detail::order_debug_energy_refill_full(const issue_view &, const order_sink &s,
                                            const issue_calls &, uint32_t         player_id,
                                            int32_t target_id) {
    s.scratch_reset();
    s.scratch_set_field(2, player_id);
    s.scratch_set_field(3, target_id);
    s.dispatch(0, (uint16_t)(player_id & 0xfu), 0xf6, 0xf6);
}

// ---- llm_strat_order_debug_damage_scaled @0x0046fb5c ----------------------------------------------
// Identical shape to order_debug_energy_refill_full: scratch_reset() first, field 2 = player_id,
// field 3 = damage_amount, owner = player_id & 0xf (AND-then-MOVZX). Order 0xf7/0xf7. Void return.
void detail::order_debug_damage_scaled(const issue_view &, const order_sink &s, const issue_calls &,
                                       uint32_t player_id, int32_t damage_amount) {
    s.scratch_reset();
    s.scratch_set_field(2, player_id);
    s.scratch_set_field(3, damage_amount);
    s.dispatch(0, (uint16_t)(player_id & 0xfu), 0xf7, 0xf7);
}

// ---- the public forms -----------------------------------------------------------------------------

int32_t order_create_unit_debug(int32_t param_1, int32_t param_2, int32_t a2, uint32_t param_4) {
    return detail::order_create_unit_debug(live_view(), live_sink(), live_calls(), param_1, param_2,
                                           a2, param_4);
}
int32_t order_queue_construction_debug(int32_t param_1, int32_t param_2, int32_t a2,
                                       uint32_t param_4) {
    return detail::order_queue_construction_debug(live_view(), live_sink(), live_calls(), param_1,
                                                  param_2, a2, param_4);
}
void order_population_delta_debug(uint32_t player_id, int32_t population_delta) {
    detail::order_population_delta_debug(live_view(), live_sink(), live_calls(), player_id,
                                         population_delta);
}
void order_debug_energy_refill_full(uint32_t player_id, int32_t target_id) {
    detail::order_debug_energy_refill_full(live_view(), live_sink(), live_calls(), player_id,
                                           target_id);
}
void order_debug_damage_scaled(uint32_t player_id, int32_t damage_amount) {
    detail::order_debug_damage_scaled(live_view(), live_sink(), live_calls(), player_id,
                                      damage_amount);
}

} // namespace mh::orders::issue
