//
// sim/sim_storage_cancel_pending_docked.cpp -- see sim_storage_cancel_pending_docked.h. Translated
// from the DISASSEMBLY (tmp/decomp/llm_storage_cancel_pending_docked_0046ca99.asm), not from the
// Ghidra .c draft -- the draft's overall shape (single loop over docked_units, state==PARKED gate,
// unconditional llm_unit_force_disembark per hit) reads correctly and was used as a map, but every
// index expression and field offset was independently re-walked against the raw
// IMUL/MOVZX/CMP/JL/JNZ opcodes per house rules.
//
#include "sim/sim_storage_cancel_pending_docked.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const storage_cancel_pending_docked_calls &live_storage_cancel_pending_docked_calls() {
    static const storage_cancel_pending_docked_calls c = {
        MH_LIBMH_BIND(llm_unit_force_disembark),
    };
    return c;
}

namespace {
// map_object_unit.state -- docked, idle. Same value/field every other sim TU touching this domain
// re-derives locally (see the header banner); not shared across TUs by established convention.
constexpr uint16_t UNIT_STATE_PARKED = 0x1f;
} // namespace

namespace detail {

void storage_cancel_pending_docked(const sim_view &v, const storage_cancel_pending_docked_calls &c,
                                   uint32_t player, int32_t building_index) {
    // The asm narrows `player` to its low 16 bits before every row-index multiply (buildings row
    // 0x0046cab6, unit_storage row 0x0046cada/0x0046cb02, units row 0x0046cb26) -- cached once here,
    // same reasoning as sim_unit_force_disembark.cpp's own single narrowing.
    const uint16_t p = static_cast<uint16_t>(player);

    // 0x0046cac9: buildings[player][building_index].sub_id, a BYTE field, read ONCE before the loop
    // and reused for every unit_storage[player][sub_id] access below (see header note).
    const uint8_t sub_id = building_of(v, p, building_index).sub_id;

    // 0x0046cada-0x0046cb4f: walk unit_storage[player][sub_id].docked_units[0..docked_count).
    const unit_storage &storage = storage_of(v, p, sub_id);
    for (int32_t i = 0; i < storage.docked_count; ++i) {
        // 0x0046cb1d: docked_units[i], an int32_t unit index (DWORD read, i*4 stride).
        const int32_t unit_index = storage.docked_units[i];

        // 0x0046cb39-0x0046cb41: gate on the unit's OWN state (a WORD compare), not on anything about
        // the storage slot itself.
        if (unit_of(v, p, unit_index).state == UNIT_STATE_PARKED) {
            // 0x0046cb4a: force the unit out. Original callee -- see header on why this runs through
            // the `_calls` indirection rather than a bare `mh::call::`.
            c.unit_force_disembark(p, unit_index);
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void storage_cancel_pending_docked(uint32_t player, int32_t building_index) {
    const sim_view v = state().read;
    detail::storage_cancel_pending_docked(v, live_storage_cancel_pending_docked_calls(), player,
                                          building_index);
}


} // namespace mh::sim
