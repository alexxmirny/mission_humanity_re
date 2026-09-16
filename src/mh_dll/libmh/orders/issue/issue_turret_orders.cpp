//
// orders/issue/issue_turret_orders.cpp -- turret target-acquisition order wrappers (RI-ORDERS /
// O4A-C, batch B, unit `turret_orders`).
//
// Two wrappers, and both are the SAME shape: a turret (a BUILDING) issues order 0x7b/0x7b via the
// REPLICATED lane (llm_strat_order_dispatch @0x00465fdf) to acquire a target. Straight-line code in
// both originals -- no guard, no branch, no scratch_reset. The only difference between the two is
// which kind bit gets OR'd into order-scratch field 5:
//
//   turret_order_target_unit      @0x0046dd90  field 5 = target_side | KIND_UNIT (0x80)
//   turret_order_target_building  @0x0046de50  field 5 = target_side | KIND_BLDG (0x40)
//
// Per both functions' .c PLATE (already [llm-renamed] and CONFIRMED against the order-queue
// consumer -- an orphaned jump-table handler at 0x004676a9, reached only through
// llm_strat_order_queue_dispatch's byte-keyed function-pointer table at 0x004672c8, so it is not its
// own Ghidra Function): that handler tests `args[5] & 0x80` to pick
// map::units[][] (bit set) over map::g::buildings[][] (bit clear) when resolving the target, then --
// if in weapon range -- commits target-kind+id into the turret's own field_0x16/field_0x18 and flags
// the building's state (field_0xd) to 0x7b. Field 6 carries the raw target id either way, written
// UNCHANGED (no kind bit folded into it) -- confirmed by both listings, which OR only into the value
// that becomes field 5's argument.
//
// FIELD-5's TOP BIT REUSES THE DOMAIN'S OWNER-KIND NIBBLE (KIND_UNIT=0x80 / KIND_BLDG=0x40, both
// issue_state.h) for a SECOND, unrelated purpose: here it tags the TARGET's kind, not the owner's.
// Both functions ALSO tag the OWNER byte with KIND_BLDG at the dispatch call (the issuer is always
// the turret's own building, whatever it is targeting) -- so one function body uses the same two
// named constants at two different call sites, for two different fields, and that is a property of
// the encoding, not a translation choice: the literal immediates in both listings (`OR DL,0x80` /
// `OR DL,0x40` for field 5, `OR AL,0x40` for the owner byte) are exactly those two values.
//
// `player` HERE IS ALREADY ushort (storage AX:2), unlike the `bldg_order_*` family's `uint32_t
// player` -- so there is no owner-byte-vs-index-narrowing WIDTH split to preserve in this unit. The
// original still reloads it as a full 32-bit dword before `OR AL,<kind>` (`MOV EAX,dword [..]` /
// `OR AL,0x40` / `MOVZX EDX,AX`), but since the parameter's own committed storage is 16-bit, that
// reload cannot carry information the ushort didn't already have -- the trailing MOVZX truncates
// back to 16 bits regardless of what garbage sat in EAX's upper half. owner_of() below matches it by
// construction (it takes uint32_t and truncates at the end), without needing a second narrowed path.
//
#include "orders/issue/issue_turret_orders.h"


namespace mh::orders::issue {

namespace {

// The owner byte the container receives: the kind nibble OR'd into the low byte of `player`. Same
// shape as issue_bldg_orders.cpp's file-local helper of the same name -- not shared across TUs, per
// Law 4 (one function in, one function out; no cross-TU helper extraction for a two-line expression).
uint16_t owner_of(uint32_t player, uint32_t kind) { return (uint16_t)(player | kind); }

} // namespace

// ---- llm_strat_turret_order_target_unit @0x0046dd90 ---------------------------------------------
void detail::turret_order_target_unit(const issue_view &, const order_sink &s, const issue_calls &,
                                      uint16_t player, uint16_t turret_bldg_idx,
                                      uint32_t target_side, uint32_t target_unit_idx) {
    s.scratch_set_field(5, (int32_t)(target_side | KIND_UNIT));
    s.scratch_set_field(6, (int32_t)target_unit_idx);
    s.dispatch(turret_bldg_idx, owner_of(player, KIND_BLDG), 0x7b, 0x7b);
}

// ---- llm_strat_turret_order_target_building @0x0046de50 ------------------------------------------
void detail::turret_order_target_building(const issue_view &, const order_sink &s,
                                          const issue_calls &, uint16_t         player,
                                          uint16_t turret_bldg_idx, uint32_t target_side,
                                          uint32_t target_bldg_idx) {
    s.scratch_set_field(5, (int32_t)(target_side | KIND_BLDG));
    s.scratch_set_field(6, (int32_t)target_bldg_idx);
    s.dispatch(turret_bldg_idx, owner_of(player, KIND_BLDG), 0x7b, 0x7b);
}

// ---- the public forms ------------------------------------------------------------------------------

void turret_order_target_unit(uint16_t player, uint16_t turret_bldg_idx, uint32_t target_side,
                              uint32_t target_unit_idx) {
    detail::turret_order_target_unit(live_view(), live_sink(), live_calls(), player,
                                     turret_bldg_idx, target_side, target_unit_idx);
}
void turret_order_target_building(uint16_t player, uint16_t turret_bldg_idx, uint32_t target_side,
                                  uint32_t target_bldg_idx) {
    detail::turret_order_target_building(live_view(), live_sink(), live_calls(), player,
                                         turret_bldg_idx, target_side, target_bldg_idx);
}


} // namespace mh::orders::issue


namespace mh::orders::issue {
} // namespace mh::orders::issue
