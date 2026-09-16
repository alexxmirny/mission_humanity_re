//
// sim/sim_unit_storage_transit.h -- llm_strat_unit_state_is_in_storage_transit, SIM1D.
//
//   * llm_strat_unit_state_is_in_storage_transit @0x004d43d4 (0x7e bytes)
//
// Reads only `units` (via sim_view/unit_of), writes nothing, calls nothing but the inert
// utils_assert_stack_capacity prologue (translator brief rule 6) -- same "no sim_store, no
// mh::call::" shape as sim_unit_state_predicates.h's three siblings, which this function is a close
// relative of: SAME "state settles it, order is the fallback" structure over the SAME +0x04 order /
// +0x06(ish) state field pair, but a NARROWER value set (only the storage-transit ranges, none of
// unit_state_is_in_transit's GROUP_MARSHAL/MOVE_WALKER/MOVE_PATH members).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {
namespace detail {

// llm_strat_unit_state_is_in_storage_transit @0x004d43d4.
//
// Returns 1 iff units[player][unit_id].state OR .order falls in EXIT_STORAGE_BEGIN..EXIT_WAIT
// (0x20-0x22) or ENTER_STORAGE_BEGIN..PARKED_2B (0x24-0x2b); state is checked first (0x004d43f6-
// 0x004d4413), order only settles the inconclusive case (0x004d442b-0x004d4449, same field/offset
// pair sim_unit_state_predicates.cpp's siblings read). EXIT_CANCEL (0x23) is deliberately excluded
// from both ranges -- the gap between the two CMP pairs on each side. Returns 0 only when NEITHER
// field is in either range. Both callers (llm_strat_ai_group_reposition_members,
// llm_strat_ai_group_report_unit_states) proceed only on ==0, i.e. TRUE means "busy docking/
// undocking, skip this unit". No backing Ghidra enum on this state/order domain -- see
// sim_unit_state_predicates.h's identical finding on the same field; names above are the plate's own
// vocabulary, kept as comments only. declared_needs: a real `llm_strat_unit_state` enum would remove
// the need for the magic numbers here and in every sibling predicate.
int32_t unit_state_is_in_storage_transit(const sim_view &v, uint32_t player, uint32_t unit_id);

} // namespace detail

// Live wrapper: the logic applied to state().read. Matches the original's __watcall shape
// (mh_export.gen.h's sig_llm_strat_unit_state_is_in_storage_transit: int32_t(uint32_t, uint32_t)).
int32_t unit_state_is_in_storage_transit(uint32_t player, uint32_t unit_id);


} // namespace mh::sim
