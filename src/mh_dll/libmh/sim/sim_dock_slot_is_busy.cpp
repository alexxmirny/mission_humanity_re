//
// sim/sim_dock_slot_is_busy.cpp -- see sim_dock_slot_is_busy.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_dock_slot_is_busy_004d3b55.asm), not from the Ghidra .c draft.
//
#include "sim/sim_dock_slot_is_busy.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// The 12-value membership test both the .order and .state checks below apply, transcribed once and
// applied to both fields per the header banner's derivation (0x004d3b9a-0x004d3cc4): the contiguous
// range 0x1f..0x2b with 0x23 excluded. Plate vocabulary (all confirmed against the branch order in
// the assembly) kept as trailing comments; no backing Ghidra enum exists on this order/state domain
// (same finding sim_unit_state_predicates.cpp's identical field pair already made).
bool dock_unit_busy_state(uint16_t v) {
    return (v >= 0x1f && v <= 0x22) || (v >= 0x24 && v <= 0x2b);
    // 0x1f PARKED, 0x20 EXIT_STORAGE_BEGIN, 0x21 EXIT_WALK_OUT, 0x22 EXIT_WAIT,
    // 0x24 ENTER_STORAGE_BEGIN, 0x25 ENTER_WAIT, 0x26 ENTER_WALK_IN, 0x27 TAKEOFF_TAXI,
    // 0x28 PARKED_28, 0x29 LANDING_REQUEST, 0x2a DOCK_TAXI_IN, 0x2b PARKED_2B
}

} // namespace

namespace detail {

int32_t dock_slot_is_busy(const sim_view &v, int32_t player, int32_t slot) {
    // 0x004d3b5f-0x004d3b8d: head_unit = v.players[player].ai_groups[slot].head_unit -- see header
    // banner for the confirmed address-arithmetic identity.
    uint16_t unit_id = player_of(v, static_cast<uint32_t>(player)).ai_groups[slot].head_unit;

    // 0x004d3b92-0x004d3cdf: walk the AI-group member chain via ai_group_next; `unit_id == 0` is the
    // tail (0x004d3cd5-0x004d3cdf, TEST/JNZ), reached both on entry and after each reload.
    while (unit_id != 0) {
        const unit &u = unit_of(v, static_cast<uint32_t>(player), static_cast<int32_t>(unit_id));

        // 0x004d3b9a-0x004d3cc4: order-then-state, pure OR -- any single match returns 1 immediately.
        if (dock_unit_busy_state(u.order) || dock_unit_busy_state(u.state)) return 1;

        // 0x004d3cce: no match on either field -- advance to the next member and re-test.
        unit_id = u.ai_group_next;
    }
    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t dock_slot_is_busy(int32_t player, int32_t slot) {
    const sim_view v = state().read;
    return detail::dock_slot_is_busy(v, player, slot);
}


} // namespace mh::sim
