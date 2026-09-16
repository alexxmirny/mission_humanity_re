//
// sim/sim_storage_launch_parked_to_orbit.cpp -- see sim_storage_launch_parked_to_orbit.h. Translated
// from the DISASSEMBLY (tmp/decomp_sim/llm_strat_storage_launch_parked_to_orbit_0048f952.asm), not
// from the Ghidra .c draft -- the draft's overall shape (loop over docked_units, state==PARKED gate,
// force-disembark + set order + return the index, or 0 on exhaustion) reads correctly and was used as
// a map, but every index expression and field offset was independently re-walked against the raw
// IMUL/MOVZX/CMP/JL/JNZ opcodes per house rules.
//
#include "sim/sim_storage_launch_parked_to_orbit.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const storage_launch_parked_to_orbit_calls &live_storage_launch_parked_to_orbit_calls() {
    static const storage_launch_parked_to_orbit_calls c = {
        MH_LIBMH_BIND(llm_unit_force_disembark),
    };
    return c;
}

namespace {
// llm_strat_unit_state members this function tests/sets (map_object_unit.state / .order, both
// uint16_t). Same value/field every other sim TU touching this domain re-derives locally (see the
// header banner); not shared across TUs by established convention -- no backing Ghidra enum exists.
constexpr uint16_t UNIT_STATE_PARKED          = 0x1f; // map_object_unit.state -- docked, idle
constexpr uint16_t UNIT_STATE_ASCEND_TO_ORBIT = 0x31; // map_object_unit.order -- launch to orbit
} // namespace

namespace detail {

int32_t storage_launch_parked_to_orbit(const sim_view &v, sim_store &own,
                                       const storage_launch_parked_to_orbit_calls &c, uint16_t player,
                                       int32_t building_index) {
    // 0x0048f971-0x0048f98b: sub_id = buildings[player][building_index].sub_id, a BYTE field, read
    // ONCE and reused for every unit_storage[player][sub_id] row-base recomputation below (see header
    // note).
    const uint8_t       sub_id  = building_of(v, player, building_index).sub_id;
    const unit_storage &storage = storage_of(v, player, sub_id);

    // 0x0048f995-0x0048fa33: scan docked_units[0..docked_count) for the first PARKED unit. Structured
    // as a for-loop here; the assembly threads the identical control shape through gotos (see header
    // banner) -- same head test, same body test, same two exits.
    for (int32_t i = 0; i < storage.docked_count; ++i) {
        // 0x0048f9db: docked_units[i], an int32_t unit index (DWORD read, i*4 stride).
        const int32_t unit_index = storage.docked_units[i];

        // 0x0048f9f7-0x0048f9ff: gate on the unit's OWN state (a WORD compare).
        if (unit_of(v, player, unit_index).state == UNIT_STATE_PARKED) {
            // 0x0048fa01-0x0048fa0d: force the unit out. Original callee -- see header on why this
            // runs through the `_calls` indirection rather than a bare `mh::call::`.
            c.unit_force_disembark(player, unit_index);

            // 0x0048fa11-0x0048fa29: direct write, distinct from the callee's own writes -- set the
            // unit's order to depart for orbit.
            own.unit_at(player, unit_index).order = UNIT_STATE_ASCEND_TO_ORBIT;

            return unit_index; // 0x0048fa2c-0x0048fa44
        }
    }
    return 0; // 0x0048fa33-0x0048fa44: docked list exhausted, nothing was PARKED
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t storage_launch_parked_to_orbit(uint16_t player, int32_t building_index) {
    sim_state st = state();
    return detail::storage_launch_parked_to_orbit(st.read, st.own, live_storage_launch_parked_to_orbit_calls(),
                                                  player, building_index);
}


} // namespace mh::sim
