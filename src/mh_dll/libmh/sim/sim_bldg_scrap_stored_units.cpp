//
// sim/sim_bldg_scrap_stored_units.cpp -- see sim_bldg_scrap_stored_units.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_bldg_scrap_stored_units_0048d7d5.asm), not from the Ghidra .c draft.
//
#include "sim/sim_bldg_scrap_stored_units.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const scrap_stored_units_calls &live_scrap_stored_units_calls() {
    static const scrap_stored_units_calls gc = {
        MH_LIBMH_BIND(llm_strat_unit_refund_build_cost_by_health),
        MH_LIBMH_BIND(llm_strat_unit_teardown),
    };
    return gc;
}

namespace detail {

void bldg_scrap_stored_units(const sim_view &v, const scrap_stored_units_calls &gc, uint32_t player,
                             int32_t building_index) {
    // sub_id is read ONCE before the loop, exactly like the asm (0x0048d7ec-0x0048d80f computes it
    // a single time into a stack local and every iteration reuses that local) -- this is not a
    // hoisted-read violation of translator brief rule 16, it is a faithful transcription of what
    // the original itself does once.
    const uint8_t sub_id = building_of(v, player, building_index).sub_id;

    // The loop bound is NOT cached: docked_count is re-read off the storage record at the top of
    // every iteration (0x0048d829-0x0048d832 loads dword ptr [storage_slot + 0xc727c4] and compares
    // against the running index fresh each pass), matching `const` on sim_view not being a promise
    // of stability (translator brief rule 16) -- so this is written as a live re-evaluation of
    // storage_of(...).docked_count in the loop condition rather than hoisted into a local.
    for (int32_t i = 0; i < storage_of(v, player, sub_id).docked_count; ++i) {
        // docked_units[i] read as a plain dword (0x0048d854 SHL EAX,2 / 0x0048d859 MOV, no
        // MOVZX/MOVSX) -- see the header's derivation of why this is int32_t, not a narrower type.
        const int32_t unit_index = storage_of(v, player, sub_id).docked_units[i];

        // Call 1: unit_index passed FULL WIDTH (0x0048d862 MOV EDX,dword ptr[...] -- plain 32-bit
        // load, no truncation).
        gc.refund_build_cost_by_health((int32_t)player, unit_index);

        // Call 2: unit_index passed TRUNCATED to 16 bits (0x0048d86e MOVZX EDX,word ptr[...] --
        // zero-extended from the low 16 bits of the SAME local the first call read full-width).
        gc.unit_teardown(player, (uint16_t)unit_index);
    }
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void bldg_scrap_stored_units(uint32_t player, int32_t building_index) {
    const sim_view v = state().read;
    detail::bldg_scrap_stored_units(v, live_scrap_stored_units_calls(), player, building_index);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
