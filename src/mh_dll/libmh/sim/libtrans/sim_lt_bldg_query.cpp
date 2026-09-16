//
// sim/libtrans/sim_lt_bldg_query.cpp -- see sim_lt_bldg_query.h. Translated from the DISASSEMBLY
// (tmp/decomp_lib_trans/llm_bldg_first_occupied_unit_slot_has_soldiers_0049a025.asm), not from
// Ghidra's C draft -- the draft indexes `Building[building_index].unit_quant` as a `double[100]`
// (the field's own, wrong, Ghidra type -- see the header banner's "GHIDRA GAP" note), which reads
// correctly for the VALUE comparisons but obscures that the asm never executes an x87 instruction
// against these bytes at all.
//
#include "sim/libtrans/sim_lt_bldg_query.h"

#include <cstddef> // offsetof
#include <cstring> // std::memcpy

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

int32_t first_occupied_unit_slot_has_soldiers(const sim_view &v, int32_t building_type_id) {
    // Byte base of Building[building_type_id].unit_quant. `offsetof`/`sizeof` come from the
    // committed mh_structs.gen.h layout (static_assert'd against Ghidra: sizeof(cfg_building)==0x842
    // matches the asm's `IMUL EDX,building_type_id,0x842` @0x0049a057;
    // offsetof(cfg_building,unit_quant)==0x27d matches `0xd9eefd - 0xd9ec80`) -- never a literal byte
    // offset. NO bounds check on `building_type_id` against Building[100]'s extent, matching the
    // original exactly: EAX is used directly in the IMUL with no upper-bound compare anywhere in the
    // body.
    const uint8_t *unit_quant_base = reinterpret_cast<const uint8_t *>(v.cfg_buildings) +
                                     static_cast<std::size_t>(building_type_id) * sizeof(cfg_building) +
                                     offsetof(cfg_building, unit_quant);

    // 0x0049a040 (seed 1, not 0) / 0x0049a047-0x0049a04b (`CMP ...,0x64 ; JL`, i.e. `slot < 100`):
    // unit-type slots 1..99 inclusive. Slot 0 is never examined.
    for (int32_t slot = 1; slot <= 99; ++slot) {
        const uint8_t *entry = unit_quant_base + static_cast<std::size_t>(slot) * 8;

        // GHIDRA GAP (header banner): read the 8-byte entry as two RAW int32 halves via memcpy off
        // the raw byte address, never through the (wrongly double-typed, and here never 4-aligned)
        // `.unit_quant[slot]` field -- see the header banner's "WHY std::memcpy" note.
        //
        // 0x0049a066/0x0049a070: TEST dword ptr [entry+4],0x7fffffff ; JNZ -- the +4 half, with its
        // top bit masked OUT, is tested FIRST. An entry whose only set bit is 0x80000000 in this half
        // must NOT read as occupied, hence the mask.
        int32_t flags_masked;
        std::memcpy(&flags_masked, entry + 4, sizeof(flags_masked));
        flags_masked &= 0x7fffffff;

        bool occupied;
        if (flags_masked != 0) {
            occupied = true;
        } else {
            // 0x0049a072/0x0049a079: CMP dword ptr [entry+0],0x0 ; JZ -- the +0 half only matters
            // when the masked +4 half was zero (both reads are side-effect-free, so this
            // short-circuit is unobservable either way; kept for shape fidelity per translator-brief
            // rule 4/10).
            int32_t qty;
            std::memcpy(&qty, entry + 0, sizeof(qty));
            occupied = (qty != 0);
        }

        if (!occupied) continue; // 0x0049a09d: JMP back to the loop increment.

        // 0x0049a07b-0x0049a089: the FIRST occupied slot decides the WHOLE result -- `slot` doubles
        // as the unit-type id into Unit[] (sim_view::cfg_units), the SAME index, no separate lookup.
        // `soldier_count` is already correctly typed (int32_t, offsetof==0x227, static_assert'd), so
        // this one reads through the struct field directly, not via raw bytes.
        return (v.cfg_units[slot].soldier_count != 0) ? 1 : 0;
    }

    // 0x0049a047(JL fails)/0x0049a04d/0x0049a09f: slots 1..99 all unoccupied -> 0. No Unit[] lookup
    // happens on this path (not an any-of scan -- see the header banner).
    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t first_occupied_unit_slot_has_soldiers(int32_t building_index) {
    const sim_view v = state().read;
    return detail::first_occupied_unit_slot_has_soldiers(v, building_index);
}


} // namespace mh::sim
