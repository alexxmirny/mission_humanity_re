//
// sim/libtrans/sim_lt_scan_masked_table.cpp -- see sim_lt_scan_masked_table.h. Translated from the
// DISASSEMBLY (tmp/decomp_lib_trans/llm_scan_masked_table_for_empty_cell_004b4bdd.asm), not from
// Ghidra's C draft -- the draft's uVar1/uVar3 CONCAT31 threading is a correct, if opaque, model of
// the single packed cursor this file names explicitly (see the header banner).
//
#include "sim/libtrans/sim_lt_scan_masked_table.h"

#include "ai/ai_state.h" // mh::ai::grid_wrap_mask() (the OWNER accessor) / ai_say / trace_budget

namespace mh::sim {

namespace detail {

int32_t scan_masked_table_for_empty_cell(uint32_t *wrap_mask, const uint8_t *grid, int32_t grid_width,
                                         int32_t grid_height, const uint8_t *footprint_mask,
                                         int32_t span_x, int32_t span_y, int32_t start_x,
                                         int32_t start_y) {
    // 0x004b4be5-0x004b4bf2: two BYTE stores, low then high, bits 16-31 preserved -- the same
    // preserve-the-high-half idiom ai/ai_grid.cpp uses at its three sites.
    *wrap_mask = (*wrap_mask & 0xffff0000u) | (((uint32_t)(uint8_t)(grid_width - 1)) << 8) |
                 ((uint32_t)(uint8_t)(grid_height - 1));

    // 0x004b4bfd/0x004b4bff: the single packed cursor. BH (bits 8-15) = x, BL (bits 0-7) = y, bits
    // 16-31 permanently 0 (XOR EBX,EBX, and every later write below touches only one byte of it).
    // start_x is loaded ONCE, before the outer loop -- never reloaded on a later outer pass.
    uint32_t cursor = ((uint32_t)(uint8_t)start_x) << 8;

    // 0x004b4c02-0x004b4c2a: outer loop over span_x, DO-WHILE shaped in the original (tested at the
    // BOTTOM, 0x004b4c2a) -- reproduced as `for (;;) { ...; if (--span_x == 0) return 1; }` so a
    // span_x of 0 underflows to -1 (as int32_t) and keeps looping, exactly as `DEC dword ptr
    // [EBP+0x18]` / `JNZ` would.
    for (;;) {
        // 0x004b4c05: BL = start_y, RELOADED at the top of EVERY outer iteration -- this is what
        // stops the y-climb hazard (see header banner) from crossing an outer-loop boundary.
        cursor        = (cursor & 0xffffff00u) | ((uint32_t)(uint8_t)start_y);
        int32_t inner = span_y; // 0x004b4c02: ECX = span_y, reloaded every outer iteration too.

        // 0x004b4c08-0x004b4c23: inner loop over span_y, also DO-WHILE shaped (tested at the bottom,
        // 0x004b4c23) -- same underflow-on-zero reproduction as the outer loop.
        for (;;) {
            if (*footprint_mask != 0) {
                // 0x004b4c0d: AND EBX,[wrap_mask] -- DESTRUCTIVE: the masked value is written back
                // into the running cursor, not used as a one-off lookup temporary. See header banner.
                cursor &= *wrap_mask;
                const uint8_t cell = grid[cursor];
                if (cell == 0 || cell == 6) {
                    return 0; // 0x004b4c36: does not fit -- polarity kept as-is, see header banner.
                }
            }
            // 0x004b4c1f-0x004b4c23: INC BL (UNMASKED on the skip path -- the y cursor can climb past
            // height-1 here), advance footprint_mask, decrement the inner counter.
            cursor = (cursor & 0xffffff00u) | (uint32_t)(uint8_t)((cursor & 0xffu) + 1u);
            ++footprint_mask;
            if (--inner != 0) {
                continue;
            }
            break;
        }

        // 0x004b4c25: INC BH -- operates on whatever the cursor holds after the inner loop, masked or
        // not (see header banner).
        cursor = (cursor & 0xffff00ffu) | ((uint32_t)(uint8_t)(((cursor >> 8) & 0xffu) + 1u) << 8);
        if (--span_x == 0) {
            return 1; // 0x004b4c2c: every tested cell passed.
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t scan_masked_table_for_empty_cell(uint8_t *grid, int32_t grid_width, int32_t grid_height,
                                         uint8_t *footprint_mask, int32_t span_x, int32_t span_y,
                                         int32_t start_x, int32_t start_y) {
    return detail::scan_masked_table_for_empty_cell(
        mh::ai::grid_wrap_mask(), static_cast<const uint8_t *>(grid), grid_width, grid_height,
        static_cast<const uint8_t *>(footprint_mask), span_x, span_y, start_x, start_y);
}


} // namespace mh::sim
