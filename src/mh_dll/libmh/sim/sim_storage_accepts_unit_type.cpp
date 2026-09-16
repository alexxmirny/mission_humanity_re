//
// sim/sim_storage_accepts_unit_type.cpp -- see sim_storage_accepts_unit_type.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_bldg_storage_accepts_unit_type_00497ac4.asm), cross-checked
// branch-by-branch against the Ghidra .c draft (which reads correctly here).
//
#include "sim/sim_storage_accepts_unit_type.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

int32_t storage_accepts_unit_type(int16_t building_type, uint16_t unit_type) {
    // 0x00497aee-0x00497b0a: four-way unsigned range dispatch on unit_type. See the header banner
    // for the full derivation of the bucket boundaries and building_type pairs.
    if (unit_type < 0xb) {
        // 0x00497b19-0x00497b20: unit_type==0 is its own carve-out (JNC unit_type>=1 else fall to
        // the shared false tail) -- the [1,0xa] bucket, not [0,0xa].
        if (unit_type != 0 && (building_type == 8 || building_type == 0x1c)) return 1;
        return 0;
    }
    if (unit_type <= 0xe) { // [0xb, 0xe]
        if (building_type == 7 || building_type == 0x1b) return 1;
        return 0;
    }
    if (unit_type <= 0x10) { // [0xf, 0x10]
        if (building_type == 0xa || building_type == 0x1e) return 1;
        return 0;
    }
    if (unit_type <= 0x12) { // [0x11, 0x12]
        if (building_type == 9 || building_type == 0x1d) return 1;
        return 0;
    }
    // unit_type >= 0x13: falls straight to the shared false tail (0x00497b0a JMP 0x00497b7f).
    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t storage_accepts_unit_type(int16_t building_type, uint16_t unit_type) {
    return detail::storage_accepts_unit_type(building_type, unit_type);
}


} // namespace mh::sim
