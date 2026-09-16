//
// orders/issue/issue_unit_attack.h -- the unit "attack" order-issue wrappers (RI-ORDERS / O4A-C,
// `unit_attack` unit of the 2026-08-28 sweep). Declares:
//
//   unit_order_attack_target                @0x0046af1e -- 4 golden sites, sim-rooted
//   unit_order_attack_target_alt            @0x0046b599 -- the `_alt` twin: SAME branch shape as
//                                            the above, ONE constant differs (0x1a -> 0x1b in two
//                                            of the three dispatch shapes). sim-rooted.
//   unit_order_attack_unit                  @0x0046bc14 -- the short sibling: no range/boarding/
//                                            type branching, a single unconditional dispatch.
//                                            sim-rooted.
//   unit_order_attack_building_reposition   @0x0046bf9c -- targets a BUILDING; a ground attacker
//                                            already in range and not boarding calls the sibling
//                                            unit's bldg_footprint_random_point to jitter the
//                                            attack point inside the building's footprint before
//                                            dispatching. ai-rooted.
//
// See tmp/decomp_orders_issue/_UNIT_unit_attack.md for the batch's own per-row hazard notes.
//
#pragma once
#include "orders/issue/issue_bldg_footprint.h" // detail::bldg_footprint_random_point -- OURS, called directly
#include "orders/issue/issue_state.h"

namespace mh::orders::issue {

namespace detail {

// ---- llm_strat_unit_order_attack_target @0x0046af1e ---------------------------------------------
// Orders (attacker_player, attacker_unit_idx) to attack unit (target_player, target_unit_idx).
// `weapon` is a 0-3 weapon-slot selector, or 0xffffffff (-1) to auto-select via the TARGET's
// elevation (airborne vs not). Emits ONE of three dispatch shapes depending on range, boarding
// state, and the attacker's unit-class fields -- see the .cpp for the full branch structure.
void unit_order_attack_target(const issue_view &v, const order_sink &s, const issue_calls &c,
                              uint32_t attacker_player, int32_t attacker_unit_idx,
                              uint32_t target_player, int32_t target_unit_idx, uint32_t weapon);

// ---- llm_strat_unit_order_attack_target_alt @0x0046b599 -----------------------------------------
// The `_alt` twin of the above, translated independently from its own listing per the unit spec's
// hazard note. Identical branch structure; differs from the original in exactly the order-code
// constants used by the "direct fire" and "other ground class" dispatch shapes (0x1a -> 0x1b) --
// see the .cpp for the precise two sites.
void unit_order_attack_target_alt(const issue_view &v, const order_sink &s, const issue_calls &c,
                                  uint32_t attacker_player, int32_t attacker_unit_idx,
                                  uint32_t target_player, int32_t target_unit_idx, uint32_t weapon);

// ---- llm_strat_unit_order_attack_unit @0x0046bc14 -----------------------------------------------
// The short sibling: no range check, no boarding check, no per-class branching -- a single
// unconditional dispatch (order 0x1e/0x1e) carrying the target's RAW (un-divided) fine coordinates
// rather than tile coordinates, at scratch slots 8/9/0xa/0xb (NOT the 0/1/3/4/5/6 the other three
// functions in this unit use).
void unit_order_attack_unit(const issue_view &v, const order_sink &s, const issue_calls &c,
                            uint32_t attacker_player, int32_t attacker_unit_idx,
                            uint32_t target_player, int32_t target_unit_idx, uint32_t weapon);

// ---- llm_strat_unit_order_attack_building_reposition @0x0046bf9c --------------------------------
// Targets a BUILDING. `weapon` auto-selects with a HARDCODED target-class of 1 (buildings carry no
// elevation field, unlike units). A ground attacker that is already in range and not boarding calls
// the sibling `bldg_footprint_random_point` (issue_bldg_footprint.h) to jitter the attack point
// inside the target building's footprint, then dispatches order 1/0x1c with the jittered point.
// Every OTHER case (not in range, boarding, or a non-ground attacker) falls to the same "direct
// fire" dispatch shape (order 0x1c/move_op_arg) the other three functions in this unit use.
void unit_order_attack_building_reposition(const issue_view &v, const order_sink &s,
                                           const issue_calls &c, uint32_t attacker_player,
                                           int32_t attacker_unit_idx, uint32_t target_player,
                                           int32_t target_bldg_idx, uint32_t weapon);


} // namespace detail

void unit_order_attack_target(uint32_t attacker_player, int32_t attacker_unit_idx,
                              uint32_t target_player, int32_t target_unit_idx, uint32_t weapon);
void unit_order_attack_target_alt(uint32_t attacker_player, int32_t attacker_unit_idx,
                                  uint32_t target_player, int32_t target_unit_idx, uint32_t weapon);
void unit_order_attack_unit(uint32_t attacker_player, int32_t attacker_unit_idx,
                            uint32_t target_player, int32_t target_unit_idx, uint32_t weapon);
void unit_order_attack_building_reposition(uint32_t attacker_player, int32_t attacker_unit_idx,
                                           uint32_t target_player, int32_t target_bldg_idx,
                                           uint32_t weapon);

} // namespace mh::orders::issue
