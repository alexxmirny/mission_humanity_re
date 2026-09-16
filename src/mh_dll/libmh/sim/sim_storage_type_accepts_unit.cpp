//
// sim/sim_storage_type_accepts_unit.cpp -- see sim_storage_type_accepts_unit.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_storage_type_accepts_unit_00497b91.asm), not from the Ghidra .c
// draft -- see the header banner for what was re-derived rather than trusted.
//
#include "sim/sim_storage_type_accepts_unit.h"

#include "ai/ai_state.h"                  // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_order_enqueue.h"        // BUILDING_TYPE_* / UNIT_TYPE_A_HELI.._H_HELI_CARGO
#include "sim/sim_unit_type_predicates.h" // UNIT_TYPE_UNDEFINED / _A_WALKER / _A_HELI_MOTHER

namespace mh::sim {

namespace detail {

int32_t storage_type_accepts_unit(const sim_view &v, uint32_t building_index, uint16_t unit_index) {
    // 0x00497bb5-0x00497c22 and every later building_index use: only the LOW WORD of the stack slot
    // holding this uint32_t param is ever read (`MOVZX EAX, word ptr [...]`) -- see header banner.
    const uint32_t      bldg_id = building_index & 0xffffu;
    const cfg_building &bldg    = v.cfg_buildings[bldg_id];

    if (bldg.type == BUILDING_TYPE_A_SHUTTLE || bldg.type == BUILDING_TYPE_H_SHUTTLE) {
        // 0x00497bd5-0x00497c22: the embedded per-soldier-slot capacity table. See header banner for
        // the offset-0x615 derivation (Building[building_index].unit_capacity[100]) and the
        // deliberately-unchecked index.
        const cfg_unit &proto = v.cfg_units[unit_index];
        const int32_t   crew_offset =
            (proto.soldier_count == 0) ? 0 : (proto.soldier_count - 1);             // 0x00497bae-0x00497bfe
        const int32_t capacity_slot = static_cast<int32_t>(unit_index) - crew_offset; // 0x00497c0f-0x00497c16
        return bldg.unit_capacity[capacity_slot];                                     // 0x00497c18-0x00497c1f, byte, unchecked
    }

    // 0x00497c27-0x00497ce9: the five-bucket building/unit tier match. See header banner for the full
    // bucket table and the fifth bucket's corrected bound.
    const uint32_t typ = v.cfg_units[unit_index].type; // 0x00497c2b-0x00497c37

    if (typ < UNIT_TYPE_A_HELI) {                 // 0x00497c3a/0x00497c3e
        if (typ == UNIT_TYPE_UNDEFINED) return 0; // 0x00497c6f-0x00497c73
        if (typ < UNIT_TYPE_A_WALKER) {           // 0x00497c75-0x00497c79
            // 0x00497c82-0x00497c95: unit type in [1, A_WALKER).
            return (bldg.type == BUILDING_TYPE_A_GARAGE || bldg.type == BUILDING_TYPE_H_GARAGE) ? 1 : 0;
        }
        // 0x00497c97-0x00497caa: unit type in [A_WALKER, A_HELI).
        return (bldg.type == BUILDING_TYPE_A_BARRAKS || bldg.type == BUILDING_TYPE_H_BARRACKS) ? 1 : 0;
    }
    if (typ < UNIT_TYPE_A_PLANE) { // 0x00497c40/0x00497c44
        // 0x00497cac-0x00497cbf: unit type in [A_HELI, A_PLANE].
        return (bldg.type == BUILDING_TYPE_A_HELIPAD || bldg.type == BUILDING_TYPE_H_HELIPAD) ? 1 : 0;
    }
    if (typ < UNIT_TYPE_A_HELI_MOTHER) { // 0x00497c46/0x00497c4a
        // 0x00497cc1-0x00497cd4: unit type in [A_PLANE, A_HELI_MOTHER).
        return (bldg.type == BUILDING_TYPE_A_AIRFIELD || bldg.type == BUILDING_TYPE_H_AIRFIELD) ? 1 : 0;
    }
    if (typ >= UNIT_TYPE_A_HELI_CARGO && typ <= UNIT_TYPE_H_HELI_CARGO) { // 0x00497c50/0x00497c56 (see header banner)
        // 0x00497cd6-0x00497ce9: unit type in [A_HELI_CARGO, H_HELI_CARGO].
        return (bldg.type == BUILDING_TYPE_A_PORT || bldg.type == BUILDING_TYPE_H_PORT) ? 1 : 0;
    }
    // 0x00497c65/0x00497c7d/0x00497c60: unit type in [A_HELI_MOTHER, A_HELI_CARGO) or > H_HELI_CARGO --
    // no building check at all.
    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t storage_type_accepts_unit(uint32_t building_index, uint16_t unit_index) {
    const sim_view v = state().read;
    return detail::storage_type_accepts_unit(v, building_index, unit_index);
}


} // namespace mh::sim
