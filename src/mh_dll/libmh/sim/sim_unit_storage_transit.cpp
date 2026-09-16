//
// sim/sim_unit_storage_transit.cpp -- see sim_unit_storage_transit.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_unit_state_is_in_storage_transit_004d43d4.asm), not from
// Ghidra's C draft.
//
#include "sim/sim_unit_storage_transit.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// Membership test over the storage-transit ranges {EXIT_STORAGE_BEGIN..EXIT_WAIT 0x20-0x22,
// ENTER_STORAGE_BEGIN..PARKED_2B 0x24-0x2b}. Re-derived from the two 4-CMP branch trees the
// assembly repeats verbatim for `state` (0x004d43fd-0x004d4413) and `order` (0x004d4433-0x004d4449)
// -- differing only in which field is loaded, same as sim_unit_state_predicates.cpp's local set
// functions for the sibling predicates. EXIT_CANCEL (0x23) falls in neither CMP pair and so is
// deliberately excluded.
bool in_storage_transit_set(uint16_t v) {
    if (v < 0x20) return false;
    if (v <= 0x22) return true; // EXIT_STORAGE_BEGIN..EXIT_WAIT
    if (v < 0x24) return false; // covers v == EXIT_CANCEL (0x23)
    return v <= 0x2b;           // ENTER_STORAGE_BEGIN..PARKED_2B
}

} // namespace

namespace detail {

int32_t unit_state_is_in_storage_transit(const sim_view &v, uint32_t player, uint32_t unit_id) {
    // 0x004d43e2-0x004d43f6: unit = units[player][unit_id] (row stride 0x5b04, element stride 0xe9);
    // unit_of() implements the identical arithmetic (UNITS_PER_PLAYER * sizeof(unit) == 0x5b04).
    const unit &u = unit_of(v, player, (int32_t)unit_id);

    // 0x004d43f6-0x004d4413: state first.
    if (in_storage_transit_set(u.state)) return 1;

    // 0x004d441f-0x004d4449: order settles the inconclusive case (state matched neither range).
    return in_storage_transit_set(u.order) ? 1 : 0;
}

} // namespace detail

int32_t unit_state_is_in_storage_transit(uint32_t player, uint32_t unit_id) {
    const sim_view v = state().read;
    return detail::unit_state_is_in_storage_transit(v, player, unit_id);
}


} // namespace mh::sim
