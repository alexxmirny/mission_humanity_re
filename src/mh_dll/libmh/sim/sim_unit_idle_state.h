//
// sim/sim_unit_idle_state.h -- one small unit state-machine predicate from the strategic sim's SIM1A
// slice, its own TU because it is a pure predicate that calls nothing but the inert
// utils_assert_stack_capacity prologue (translator brief rule 6) and writes no sim state -- same
// shape as sim_unit_state_predicates.h's three siblings and sim_unit_type_predicates.h.
//
//   * llm_strat_unit_is_idle_or_patrolling @0x004ee4c2 (0x4c bytes)
//
// Reads only `units` (via sim_view/unit_of), so it takes a `sim_view` and no `sim_store`.
//
// ON THE llm_strat_unit_state DOMAIN (order/state fields, both uint16_t). This TU's plate
// (tmp/decomp/llm_strat_unit_is_idle_or_patrolling_004ee4c2.c) prints PATROL_SWAP / STOP_TO_DEFAULT /
// HOVER_ENGAGE for the compared values, and re-deriving the raw CMP/JZ/JNZ chain against
// sim_order_enqueue.h's own dumped values (UNIT_STATE_PATROL_SWAP=0x10, UNIT_STATE_STOP_TO_DEFAULT
// =0x01, UNIT_STATE_HOVER_ENGAGE=0x2e -- "Ghidra enum dump 2026-08-08, get-data-type-by-string
// llm_strat_unit_state") confirms an exact value match: order==0x10, state==0x1, state==0x2e, in that
// CMP order. Named locally from that same dump rather than by #include, matching sim_order_enqueue.h's
// own precedent of NOT sharing its UNIT_TYPE_*/UNIT_STATE_* constants across sibling sim/ files (its
// header note on ai/ai_state.h) -- pulling in the whole order-enqueue module for three constants would
// be a much heavier coupling than the duplication buys back. See the .cpp for the CONFLICTING claim in
// sim_unit_state_predicates.h (same field, says "not backed by any enum in this tree") that this TU
// does not resolve -- flagged for the conductor rather than silently picked between.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {
namespace detail {

// llm_strat_unit_is_idle_or_patrolling @0x004ee4c2.
//
// Returns 1 iff units[player][unit_index].order == PATROL_SWAP(0x10), OR .state ==
// STOP_TO_DEFAULT(0x01), OR .state == HOVER_ENGAGE(0x2e); else 0. Three plain CMP/JZ checks
// short-circuiting to the same "return 1" landing pad (0x004ee4ff); no AND/OR of state with order the
// way sim_unit_state_predicates.cpp's siblings do -- order and state are each tested against a
// DIFFERENT single value here, not the same value-set applied to both fields.
int32_t unit_is_idle_or_patrolling(const sim_view &v, int32_t player, int32_t unit_index);

} // namespace detail

// Live wrapper: the logic applied to state().read. Matches the original's committed
// __watcall(EAX=player, EDX=unit_index) shape.
int32_t unit_is_idle_or_patrolling(int32_t player, int32_t unit_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
