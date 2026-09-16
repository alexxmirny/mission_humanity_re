//
// sim/sim_bldg_side_has_aircraft_producer.cpp -- see sim_bldg_side_has_aircraft_producer.h.
// Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_side_has_aircraft_producer_004d8b89.asm), not from the Ghidra .c draft.
//
#include "sim/sim_bldg_side_has_aircraft_producer.h"

#include "ai/ai_state.h"           // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_order_enqueue.h" // UNIT_TYPE_A_PLANE, UNIT_TYPE_H_PLANE

namespace mh::sim {

namespace detail {

int32_t bldg_side_has_aircraft_producer(const sim_view &v, int32_t player) {
    // 0x004d8b9b/0x004d8ba0-0x004d8bb4: buildings[player][0].index, zero-extended from ushort,
    // doubles as "how many occupied building slots remain to be checked" for this player -- see
    // header banner for the slot-0-doubles-as-count derivation.
    int32_t buildings_remaining =
        static_cast<uint16_t>(v.buildings[player * v.caps.buildings + 0].index);

    // 0x004d8b9b: slot cursor. Slot 0 is the count above and is never itself scanned as a building.
    int32_t slot = 1;

    // 0x004d8c70/0x004d8c72: outer do-while, condition checked BEFORE each slot -- exits (return 0,
    // 0x004d8b80) the instant buildings_remaining hits 0. UNBOUNDED on `slot` in the original (no
    // comparison against BUILDINGS_PER_PLAYER anywhere in this function) -- transcribed as-is; see
    // the translation report's uncertainties.
    while (buildings_remaining != 0) {
        // 0x004d8bc1-0x004d8bd9: player*0x6aa4 + slot*0x111 + 0xc3d2a2 ==
        // v.buildings[player][slot].building_id's address.
        const building &b = v.buildings[player * v.caps.buildings + slot];

        // 0x004d8be5/0x004d8bee: an empty slot (building_id == 0) skips straight to the slot
        // increment without touching buildings_remaining.
        if (b.building_id != 0) {
            // 0x004d8bf4-0x004d8c66: for (unit = 1; unit <= cfg_unit_sec->total; ++unit) -- unsigned,
            // INCLUSIVE (JBE) per mh_addrs.gen.h's own comment on G_UNIT_COUNT_TOTAL.
            for (uint32_t unit = 1; unit <= v.cfg_unit_sec->total; ++unit) {
                // 0x004d8c27-0x004d8c3b (x87 FLDZ/FCOMP/FNSTSW/SAHF/JNC): 0.0 <
                // cfg_buildings[building_id].unit_quant[unit] -- a strictly positive
                // production-capacity/queue entry for this unit type at this building type
                // (CONFIRMED identity, see header banner).
                // 0x004d8c49-0x004d8c59: cfg_units[unit].type == A_PLANE || == H_PLANE.
                if (0.0 < v.cfg_buildings[b.building_id].unit_quant[unit] &&
                    (v.cfg_units[unit].type == UNIT_TYPE_A_PLANE ||
                     v.cfg_units[unit].type == UNIT_TYPE_H_PLANE)) {
                    return 1; // 0x004d8c5b: MOV EAX,1; JMP 0x004d8b82
                }
            }
            // 0x004d8c6e: reached only once the unit-type scan for this occupied slot exhausted
            // without a match.
            --buildings_remaining;
        }
        ++slot; // 0x004d8c6f: unconditional every iteration, occupied slot or not.
    }
    return 0; // 0x004d8c70 -> JMP 0x004d8b80 (see header banner's shared-epilogue inference)
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t bldg_side_has_aircraft_producer(int32_t player) {
    const sim_view v = state().read;
    return detail::bldg_side_has_aircraft_producer(v, player);
}


} // namespace mh::sim
