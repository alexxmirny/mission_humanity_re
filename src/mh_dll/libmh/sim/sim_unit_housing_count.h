#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // UNIT_TYPE_A_HELI, UNIT_TYPE_A_PLANE
#include "sim/sim_state.h"
#include "sim/sim_unit_type_predicates.h" // UNIT_TYPE_A_WALKER, UNIT_TYPE_A_HELI_MOTHER

namespace mh::sim {

// UNIT_TYPE_UNDEFINED (cfg_enum_E_UNIT_TYPE member 0) now lives in
// sim_unit_type_predicates.h, included above, with the derivation and the reason it was
// hoisted out of this file (two definitions in one TU once a second header needed it).

namespace detail {

// llm_strat_unit_housing_count_add @0x0049779b. See the header derivation above. No outward calls
// besides the inert Watcom stack probe (translator-brief rule 6), so there is no `_calls` struct --
// same shape as sim_unit_type_predicates.cpp's pure bodies.
void unit_housing_count_add(const sim_view &v, sim_store &own, int32_t player, int32_t unit_proto_id);

// llm_strat_unit_housing_count_remove @0x00497842.. Exact mirror of unit_housing_count_
// add over the same ladder, decrementing instead of incrementing. Same no-outward-calls shape.
void unit_housing_count_remove(const sim_view &v, sim_store &own, int32_t player, int32_t unit_proto_id);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the original's committed __watcall(EAX,EDX)
// shape (sig_llm_strat_unit_housing_count_add) with the corrected parameter names (see header banner).
void unit_housing_count_add(int32_t player, int32_t unit_proto_id);

// Live wrapper for unit_housing_count_remove, same shape (sig_llm_strat_unit_housing_count_remove).
void unit_housing_count_remove(int32_t player, int32_t unit_proto_id);

namespace detail {

} // namespace detail

} // namespace mh::sim
