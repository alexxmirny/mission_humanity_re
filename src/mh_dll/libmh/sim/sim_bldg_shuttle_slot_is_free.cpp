//
// sim/sim_bldg_shuttle_slot_is_free.cpp -- see sim_bldg_shuttle_slot_is_free.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_bldg_shuttle_slot_is_free_0048fa97.asm), not from the Ghidra
// .c draft.
//
#include "sim/sim_bldg_shuttle_slot_is_free.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

int32_t bldg_shuttle_slot_is_free(const sim_view &v, int32_t player, int32_t building_id) {
    // 0x0048faa4-0x0048fb4e: buildings[player][building_id], re-read here once -- all four gate
    // blocks in the assembly recompute the IDENTICAL address (same literal offsets, no write in
    // between), so one fetch stands in for all four re-reads. See header banner for the byte-level
    // confirmation.
    const building &b    = building_of(v, static_cast<uint32_t>(player), building_id);
    const uint8_t   type = v.cfg_buildings[b.building_id].type;

    // 0x0048faa4-0x0048fb52: the four-way OR gate. A miss on all four returns -1 (not shuttle-capable)
    // immediately, without touching the shuttle slot at all.
    if (type != BUILDING_TYPE_A_PORT && type != BUILDING_TYPE_H_PORT && type != BUILDING_TYPE_A_MOTHER &&
        type != BUILDING_TYPE_H_MOTHER) {
        return -1;
    }

    // 0x0048fb5e-0x0048fb75: shuttle_slot = buildings[player][building_id].shuttle_slot -- same
    // building record `b` already names (no write happened between the type gate and here).
    const uint32_t shuttle_slot = b.shuttle_slot;

    // 0x0048fb78: shuttle_slot == 0 (unbound) skips both reservation checks and returns 1 (free).
    if (shuttle_slot != 0) {
        const prod_shuttle_slot &slot = v.prod_shuttle_slots[player * PROD_SHUTTLE_SLOTS_PER_PLAYER + shuttle_slot];

        // 0x0048fb92-0x0048fba2: any reserved passenger makes the slot busy.
        if (slot.passengers_reserved != 0) return 0;

        // 0x0048fbab-0x0048fbe5: resources_reserved[1..9] (loop starts at 1, bound 10 -- index 0 is
        // never visited, reproduced exactly rather than "cleaned up" to a 9-iteration loop).
        for (int32_t r = 1; r < 10; ++r) {
            if (slot.resources_reserved[r] != 0) return 0;
        }
    }

    // 0x0048fbe7: shuttle-capable and unbound/unreserved.
    return 1;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t bldg_shuttle_slot_is_free(int32_t player, int32_t building_id) {
    const sim_view v = state().read;
    return detail::bldg_shuttle_slot_is_free(v, player, building_id);
}


} // namespace mh::sim
