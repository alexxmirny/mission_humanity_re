//
// orders/issue/issue_order_core.cpp -- the `llm_strat_order_*` core wrappers (RI-ORDERS / O4C).
//
// ONE function so far, and it is here for an ORACLE reason rather than a scheduling one, so the
// reason is worth stating: the six batch-A pilots O4-0 translated share a shape that leaves three
// things the oracle claims to check UNEXERCISED --
//
//   * `param0` and `order_code` are the SAME constant at all six sites, so no pilot could tell the
//     two apart; a suite that had them swapped would have printed green.
//   * all six OR a kind nibble into the owner byte, so the "no kind at all" shape was untested.
//   * none of them calls `scratch_reset`, so the golden's `scratch_reset_before` flag was compared
//     against `false` seven times and never against `true`.
//
// `llm_strat_order_debug_kill_group` closes all three in 97 bytes: param0 0xf7 vs order_code 0xf8,
// an owner built with `AND EAX,0xf` and no OR, a scratch_reset, and a unit_index that is a literal
// zero rather than a parameter. That is why a batch-C row is translated before batch C runs.
//
// IT DOES NOT PREJUDGE O4C'S OPEN DECISION. Whether the 14 debug-console-issued wrappers are in
// scope for the domain at all is O4C's to settle, and translating one of them for oracle coverage
// is not that decision. If the answer comes back OUT, this file keeps one function that happens to
// be proven and the ledger row records the disposition; nothing here presumes the other 13.
//
// DERIVED FROM THE .asm (llm_strat_order_debug_kill_group_0046fc1e.asm), not the .c.
//
#include "orders/issue/issue_state.h"

namespace mh::orders::issue {

// ---- llm_strat_order_debug_kill_group @0x0046fc1e -----------------------------------------------
// Unguarded. Order param0 0xf7, order_code 0xf8 -- note they DIFFER, unlike every building wrapper.
//
// THE OWNER IS `player_id & 0xf`, NOT `player_id | kind`. `AND EAX,0xf` @0x0046fc67 then
// `MOVZX EDX,AX`: the low nibble only, with no kind bits set. The container reads the kind nibble
// out of the high half of that byte, so this order arrives with kind 0 -- which is what routes it
// down the admin lane rather than the building or unit one.
//
// THE UNIT INDEX IS A LITERAL ZERO (`XOR EAX,EAX` @0x0046fc6f), not a parameter. The order targets
// a player, not an object, so slot 0 is a placeholder the handler ignores; passing `player_id`
// there -- the plausible mistake -- would be a different order.
//
// scratch_reset() FIRST, then args[2] = player_id and args[3] = damage_amount. The reset matters:
// the scratch buffer is a global that keeps whatever the previous order left in it, so a wrapper
// that skipped it would ship the previous order's arguments in the other eleven slots.
void detail::order_debug_kill_group(const issue_view &, const order_sink &s, uint32_t player_id,
                                    int32_t damage_amount) {
    s.scratch_reset();
    s.scratch_set_field(2, (int32_t)player_id);
    s.scratch_set_field(3, damage_amount);
    s.dispatch(0, (uint16_t)(player_id & 0xfu), 0xf7, 0xf8);
}

void order_debug_kill_group(uint32_t player_id, int32_t damage_amount) {
    detail::order_debug_kill_group(live_view(), live_sink(), player_id, damage_amount);
}

} // namespace mh::orders::issue
