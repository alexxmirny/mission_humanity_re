//
// sim/sim_unit_state_predicates.h -- three small unit state-machine predicates from the strategic
// sim's SIM1A slice, grouped into one TU because none of them writes anything and none calls out to
// another game function (the only CALL any of the three makes is the inert
// utils_assert_stack_capacity prologue -- see the translator brief's rule 6).
//
//   * llm_strat_unit_attack_target_is_dead   @0x004d4226 (0xf8 bytes)
//   * llm_strat_unit_state_is_in_transit     @0x004d431e (0xb6 bytes)
//   * llm_strat_unit_is_idle_or_parked       @0x004d4452 (0x7e bytes)
//
// All three read only `units` (via sim_view/unit_of) and, for the ATTACK_BUILDING arm of the first,
// the map occupancy plane (`tile_objects` via tile_at()). None writes sim state, so each takes a
// `sim_view` and no `sim_store`, matching sim_bldg_alive.h's shape.
//
// ON THE llm_strat_unit_state DOMAIN (state/order fields, both uint16_t, same enum-ish domain).
// Ghidra's own decompile of these three functions prints symbolic names for several of the compared
// values (ATTACK_UNIT, ATTACK_BUILDING, MOVE_PATH, PARKED, ...), which looks like an applied enum --
// but ai_hq_attack_commit.cpp independently investigated the SAME field and concluded those names
// are "not backed by any enum in this tree" (Ghidra decompiler names printed without a real Data
// Type Manager enum behind them). Literal values are used below, with the plate's own vocabulary
// kept as trailing comments, matching that file's established style. declared_needs: a real
// `llm_strat_unit_state` enum for this domain would remove the need for both.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {
namespace detail {

// llm_strat_unit_attack_target_is_dead @0x004d4226.
//
// Returns 1 iff the querying unit's CURRENT attack target is dead/gone, else 0. Two arms, selected
// by the unit's OWN state/order (checked state first, order only if state doesn't match -- same
// "state settles it, order is the fallback" shape as the other two predicates in this file):
//
//   ATTACK_UNIT (0x1a): target = unit_of(ref_owner(own.target_ref), (ushort)own.target_index).
//   Dead iff (target.energy - target.pending_damage) <= 0.0, using ORDERED `<=` rather than
//   `!(0.0 < diff)` so a NaN diff reads as "not dead" -- see the .cpp for the x87 derivation; the
//   same idiom sim_bldg_alive.cpp's header already documents for this exact FCOMPP/JC shape.
//
//   ATTACK_BUILDING (0x1c): target tile = (own.target_fine_x/32, own.target_fine_y/32) (truncating,
//   see fine_to_tile() in the .cpp). Dead iff that tile's occupancy-plane `building` field is 0.
//
//   Neither state matches (on both state AND order) -> 0 (not dead; matches nothing to abandon).
int32_t unit_attack_target_is_dead(const sim_view &v, int32_t player, int32_t unit_index);

// llm_strat_unit_state_is_in_transit @0x004d431e.
//
// Returns 1 iff units[player][unit_id].state OR .order is in the scattered set {GROUP_MARSHAL 0xa,
// MOVE_WALKER 0xf, MOVE_PATH 0x11, MOVE_PATH_12 0x12, EXIT_STORAGE_BEGIN..EXIT_WAIT 0x20-0x22,
// ENTER_STORAGE_BEGIN..PARKED_2B 0x24-0x2b}; state is checked first, order only settles the
// inconclusive case (same value set, same shape, different field offset in the assembly). All 4
// callers are AI group management and act on ==0 -- TRUE means "already busy transiting".
int32_t unit_state_is_in_transit(const sim_view &v, uint32_t player, uint32_t unit_id);

// llm_strat_unit_is_idle_or_parked @0x004d4452.
//
// Returns 1 iff units[player][unit_index].state OR .order is in {PARKED..EXIT_WAIT 0x1f-0x22,
// ENTER_STORAGE_BEGIN..PARKED_2B 0x24-0x2b}, else 0. Same "state first, order settles it" shape as
// the transit predicate above, over a different (overlapping) value set. NOTE: the exported .c
// draft's PLATE comment (tmp/decomp/llm_strat_unit_is_idle_or_parked_004d4452.c) is WRONG -- it
// describes the second operand as "proto id" combined via AND-outside-two-ranges; the assembly (and
// Ghidra's own decompiled BODY, which this translation matches) actually re-reads the unit's `order`
// field at the same +0x04 offset the transit predicate uses, ORed with the state check via De
// Morgan. The plate's PROSE is stale, not the logic.
int32_t unit_is_idle_or_parked(const sim_view &v, int32_t player, int32_t unit_index);

} // namespace detail

// Live wrappers: the logic applied to state().read. Each matches its original's __watcall shape (see
// mh_export.gen.h's sig_ typedefs -- note llm_strat_unit_state_is_in_transit's params are uint32_t,
// the other two int32_t).
int32_t unit_attack_target_is_dead(int32_t player, int32_t unit_index);
int32_t unit_state_is_in_transit(uint32_t player, uint32_t unit_id);
int32_t unit_is_idle_or_parked(int32_t player, int32_t unit_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
