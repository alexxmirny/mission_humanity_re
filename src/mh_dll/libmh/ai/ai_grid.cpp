//
// ai/ai_grid.cpp -- see ai_grid.h. Translated from the DISASSEMBLY
// (tmp/decomp_a2/llm_strat_ai_grid_stamp_threat_ring_004b4c3f.asm and
// tmp/decomp_a2/llm_strat_ai_grid_clear_threat_bit_004b4c94.asm for the first two;
// tmp/decomp/llm_strat_ai_grid_fill_below_threshold_004b49da.asm and
// tmp/decomp/llm_strat_ai_grid_flood_step_004b4a10.asm for the last two, added RI-AI / AI1B batch B
// layer 2), not from Ghidra's C.
//
// THE COMMITTED GHIDRA PARAMETER NAMES ARE WRONG, and here is the derivation, not just the claim.
// `grid_stamp_threat_ring` is committed as
//   (byte *grid_base, int grid_height, int grid_width, int center_y, int center_x, int radius)
// with per-slot storage Stack[0x4..0x18], which is EBP+0x8..0x1c in the body (a uniform +4 shift,
// confirmed against grid_base itself: Stack[0x4] is the slot the body loads into EDI at
// `MOV EDI,[EBP+0x8]`). The one call site, `turret_threat_rescan` (ai_turret_threat.cpp), is
// `gc.grid_stamp_threat_ring(own_threat_grid, *v.map_width, *v.map_height, b.x, b.y, w.range_max[p])`
// -- and __cdecl args are pushed right-to-left, so the LEFTMOST call-site argument lands in the
// LOWEST stack slot (EBP+0x8), positionally:
//   EBP+0x8  = grid_base   = grid            (Ghidra's name agrees)
//   EBP+0xc  = grid_height = *v.map_width    (Ghidra's "height" is actually the caller's WIDTH)
//   EBP+0x10 = grid_width  = *v.map_height   (Ghidra's "width" is actually the caller's HEIGHT)
//   EBP+0x14 = center_y    = b.x             (Ghidra's "y" is actually the caller's X)
//   EBP+0x18 = center_x    = b.y             (Ghidra's "x" is actually the caller's Y)
//   EBP+0x1c = radius      = w.range_max[p]  (agrees)
// i.e. BOTH pairs are swapped relative to their Ghidra names. `grid_clear_threat_bit`'s
// `row_count`/`col_count` (EBP+0xc/EBP+0x10) are the same swap: EBP+0xc is the caller's WIDTH
// (turret_threat_rescan passes *v.map_width there), EBP+0x10 the caller's HEIGHT.
//
// The index arithmetic proves which axis is which, independent of any name: in stamp_threat_ring,
// `MOV DH,[EBP+0x14]` / `MOV DL,[EBP+0x18]` load (byte)x into DH and (byte)y into DL, so after
// `MOV EBX,EDX` and the spiral add (`ADD BH,[ESI]` = +dx, `ADD BL,[ESI+1]` = +dy -- confirmed against
// mh_llm_strat_ai_spiral_offset's field order and against ai_spiral_scan.cpp's identical
// `x + off.dx` / `y + off.dy` usage of the same table), BH carries the X value and BL the Y value.
// The wrap-mask bytes written just before (see the AND below) are `[0x603ec8] = [EBP+0x10]-1` (= the
// caller's HEIGHT) and `[0x603ec9] = [EBP+0xc]-1` (= the caller's WIDTH); little-endian, so the packed
// dword's bits0-7 = height-1 (masks BL/Y) and bits8-15 = width-1 (masks BH/X). Both pairings line up
// (X with width, Y with height) and the final index -- ((x<<8)|y) after the mask -- is exactly
// tile_at()'s X-outer/Y-inner packing. So: `width` truly is the map's X extent, `height` its Y
// extent, `x`/`y` truly are the ring anchor's X/Y, matching the names already committed for these
// slots in ai_calls (ai_state.h) from the call-site side.
//
// One documented discrepancy against ai_state.h's own prose (worth a second pair of eyes -- see
// `uncertainties` in this translation's report): that header's grid_wrap_mask comment reads
// "((y_extent-1) << 8) | (x_extent-1)"; the derivation above gives the opposite packing,
// "((width-1=x_extent-1) << 8) | (height-1=y_extent-1)", cross-checked twice against the raw byte
// stores and against the index formula's own AND. This file follows the assembly (per the translator
// brief's rule 1) rather than the header prose, which this translation may not edit.
//
#include "ai/ai_grid.h"


namespace mh::ai {
namespace detail {

void grid_stamp_threat_ring(const ai_view &v, const ai_store &own, uint8_t *grid, int32_t width,
                            int32_t height, int32_t x, int32_t y, int32_t radius) {
    // Two BYTE stores into the low two bytes of the shared wrap-mask scratch (0x00603ec8 / +1 in the
    // .asm). Bits 16-31 are left EXACTLY as they were -- documented always-zero in the image with no
    // other referrer, but the original genuinely only ever touches the low two bytes, so this
    // preserves rather than assumes that instead of a flat 4-byte overwrite.
    *own.grid_wrap_mask = (*own.grid_wrap_mask & 0xffff0000u) |
                          (((uint32_t)(uint8_t)(width - 1)) << 8) |
                          ((uint32_t)(uint8_t)(height - 1));

    // The `in_EDX` hazard in Ghidra's decompile (bits 16-31 of the packed index inherited from
    // whatever the caller last left in EDX) is real but inert: those bits are always ANDed against
    // wrap-mask bits 16-31, which are always 0, so the result is always 0 regardless of the garbage.
    // Computing the index directly from the truncated (x,y) below -- which likewise never sets bits
    // above 15 -- is therefore bit-for-bit equivalent to modelling the garbage and masking it.
    uint32_t             remaining = v.spiral_ring_cell_counts[radius];
    const spiral_offset *entry     = v.spiral_offsets;
    do {
        // `MOV DH,[EBP+0x14]` / `MOV DL,[EBP+0x18]` are BYTE loads -- only the low byte of x/y is
        // ever live -- and `ADD BH,.. / ADD BL,..` are 8-bit adds that wrap at 256, not int math.
        const uint8_t xi = (uint8_t)((uint8_t)x + entry->dx);
        const uint8_t yi = (uint8_t)((uint8_t)y + entry->dy);
        ++entry;

        const uint32_t idx = (((uint32_t)xi << 8) | (uint32_t)yi) & *v.grid_wrap_mask;
        grid[idx] |= TILE_FLAG_TURRET_THREAT;
        --remaining;
    } while (remaining != 0); // DEC/JNZ -- runs at least once even for a zero count.
}

void grid_clear_threat_bit(uint8_t *grid, int32_t width, int32_t height) {
    // BH (the X/outer counter) and BL (the Y/inner counter) are 8-BIT registers in the original and
    // WRAP at 256 -- a 32-bit rewrite would only coincide with this for width/height <= 256,
    // reproduced exactly with uint8_t counters rather than assumed safe.
    uint8_t  xi              = 0; // BH: persists and wraps across outer passes, never reset
    uint32_t outer_remaining = (uint32_t)width;
    do {
        uint8_t  yi              = 0; // BL: reset to 0 every outer pass (`XOR BL,BL`)
        uint32_t inner_remaining = (uint32_t)height;
        do {
            grid[((uint32_t)xi << 8) | (uint32_t)yi] &= (uint8_t)~TILE_FLAG_TURRET_THREAT;
            ++yi;
            --inner_remaining;
        } while (inner_remaining != 0); // DEC ECX/JNZ -- at least one inner pass per outer pass.
        ++xi;
        --outer_remaining;
    } while (outer_remaining != 0); // DEC ESI/JNZ -- at least one outer pass even for width == 0.
}

// llm_strat_ai_grid_fill_below_threshold @0x004b49da (RI-AI / AI1B batch B layer 2). Translated from
// tmp/decomp/llm_strat_ai_grid_fill_below_threshold_004b49da.asm.
//
// Same do-while / 8-bit-wrapping-index shape as grid_clear_threat_bit above (xi=BH outer/X,
// yi=BL inner/Y, both uint8_t so they wrap at 256 exactly as the original's BH/BL do; the 32-bit
// outer_remaining/inner_remaining counters reproduce ESI/ECX's own decrement-then-JNZ termination,
// including the same underflow-not-zero-check behaviour for a zero width/height -- not guarded,
// because the original isn't). CMP is unsigned and the store fires on NOT-ABOVE (JA skips the
// store), i.e. grid[idx] <= threshold, not <.
void grid_fill_below_threshold(uint8_t *grid, int32_t width, int32_t height, int32_t threshold,
                               int32_t fill_value) {
    uint8_t  xi              = 0; // BH
    uint32_t outer_remaining = (uint32_t)width;
    do {
        uint8_t  yi              = 0; // BL
        uint32_t inner_remaining = (uint32_t)height;
        do {
            const uint32_t idx = ((uint32_t)xi << 8) | (uint32_t)yi;
            if (grid[idx] <= (uint8_t)threshold)
                grid[idx] = (uint8_t)fill_value;
            ++yi;
            --inner_remaining;
        } while (inner_remaining != 0);
        ++xi;
        --outer_remaining;
    } while (outer_remaining != 0);
}

// llm_strat_ai_grid_flood_step @0x004b4a10 (RI-AI / AI1B batch B layer 2). Translated from
// tmp/decomp/llm_strat_ai_grid_flood_step_004b4a10.asm.
//
// Two passes, both the same outer-X/inner-Y do-while shape as the rest of this file:
//
// Pass 1 (source scan): packs own.grid_wrap_mask = ((width-1)<<8)|(height-1) -- confirmed from the
// two raw BYTE stores at 0x004b4a1f-0x004b4a2c (low byte = height-1, next byte = width-1), the
// OPPOSITE packing from what an unchecked reading of "wrap_mask" might assume, and the same
// derivation grid_stamp_threat_ring's file comment already walks through for its own two-byte store.
// For every cell < source_level, ORs (fill_value|0x20) in if ANY of its four toroidal neighbours
// (Y+1, Y-1, X+1, X-1 -- that exact order, reproduced from the four EDX/AND-mask/CMP blocks even
// though the boolean OR makes the order immaterial to the result) equals source_level. The 0x20 mark
// is LOAD-BEARING: three of the six real call shapes pass source_level == fill_value, so an unmarked
// just-filled cell would satisfy its own neighbour's == test and the wavefront would flood instead
// of stepping once.
//
// Pass 2 (clear): ANDs 0xdfdfdfdf (clear bit 0x20 in all 4 bytes) a dword at a time along Y, so
// height must be a multiple of 4 -- reproduced as-is (ECX = height>>2), not guarded.
void grid_flood_step(const ai_store &own, uint8_t *grid, int32_t width, int32_t height,
                     int32_t source_level, int32_t fill_value) {
    // The original only ever stores the LOW TWO bytes (two `MOV [addr],AL` at 0x004b4a1f-0x004b4a2c),
    // never touching bits 16-31 -- reproduced with the same masked read-modify-write as
    // grid_stamp_threat_ring's identical two-byte store, for style parity across this file's shared
    // scratch (reimpl-verify 2026-08-06: bits 16-31 are provably always zero image-wide, so this is
    // defensive-coding parity, not a correctness fix).
    *own.grid_wrap_mask = (*own.grid_wrap_mask & 0xffff0000u) |
                          (((uint32_t)(uint8_t)(width - 1)) << 8) | ((uint32_t)(uint8_t)(height - 1));

    const uint8_t src  = (uint8_t)source_level;
    const uint8_t fill = (uint8_t)fill_value | 0x20;

    uint8_t  xi              = 0; // BH
    uint32_t outer_remaining = (uint32_t)width;
    do {
        uint8_t  yi              = 0; // BL
        uint32_t inner_remaining = (uint32_t)height;
        do {
            const uint32_t idx = ((uint32_t)xi << 8) | (uint32_t)yi;
            if (grid[idx] < src) {
                const uint32_t mask    = *own.grid_wrap_mask;
                const uint8_t  y_plus  = (uint8_t)(yi + 1);
                const uint8_t  y_minus = (uint8_t)(yi - 1);
                const uint8_t  x_plus  = (uint8_t)(xi + 1);
                const uint8_t  x_minus = (uint8_t)(xi - 1);
                if (grid[(((uint32_t)xi << 8) | (uint32_t)y_plus) & mask] == src ||
                    grid[(((uint32_t)xi << 8) | (uint32_t)y_minus) & mask] == src ||
                    grid[(((uint32_t)x_plus << 8) | (uint32_t)yi) & mask] == src ||
                    grid[(((uint32_t)x_minus << 8) | (uint32_t)yi) & mask] == src)
                    grid[idx] = fill;
            }
            ++yi;
            --inner_remaining;
        } while (inner_remaining != 0);
        ++xi;
        --outer_remaining;
    } while (outer_remaining != 0);

    xi              = 0;
    outer_remaining = (uint32_t)width;
    do {
        uint8_t  yi              = 0;
        uint32_t inner_remaining = (uint32_t)height >> 2;
        do {
            const uint32_t idx = ((uint32_t)xi << 8) | (uint32_t)yi;
            *(uint32_t *)(grid + idx) &= 0xdfdfdfdfu;
            yi = (uint8_t)(yi + 4);
            --inner_remaining;
        } while (inner_remaining != 0);
        ++xi;
        --outer_remaining;
    } while (outer_remaining != 0);
}

// llm_strat_ai_grid_stamp_seeds @0x004b4ac2 (RI-AI batch B/C, 2026-08-07). Translated from
// tmp/decomp/llm_strat_ai_grid_stamp_seeds_004b4ac2.asm/.c -- the decompile is already faithful here
// (its own committed plate, from AI1C prep, derives the same formula used below; re-checked against
// the raw bytes rather than trusted blind).
//
// Same outer-X/inner-Y do-while shape and 8-bit-wrapping counters as the rest of this file (span_x
// is ALWAYS 10 at every real call site, but the original's do-while runs the body at least once even
// for span 0, reproduced as such). `stencil` is walked with a plain post-increment, one byte per
// (i,j) cell in X-outer/Y-inner order -- it does not itself need wrapping.
//
// POWER-OF-TWO PRECONDITION (reimpl-verify 2026-08-07): the original ANDs its own live x/y POSITION
// register with the wrap mask on every stencil hit and increments from the masked value on
// subsequent passes; this translation instead keeps xi/yi UNMASKED and masks only the freshly
// computed `idx`. The two are provably byte-identical only when map_width/map_height are powers of
// two -- true of every real call site (the AI notes already establishes this precondition for the
// whole grid module) but not asserted here, so do not reuse this body for a non-power-of-two extent
// without re-deriving the equivalence.
void grid_stamp_seeds(const ai_store &own, uint8_t *grid, int32_t map_width, int32_t map_height,
                      const uint8_t *stencil, int32_t span_x, int32_t span_y, int32_t origin_x,
                      int32_t origin_y, int32_t seed_value) {
    *own.grid_wrap_mask = (*own.grid_wrap_mask & 0xffff0000u) |
                          (((uint32_t)(uint8_t)(map_width - 1)) << 8) |
                          ((uint32_t)(uint8_t)(map_height - 1));

    const uint8_t seed_byte = (uint8_t)seed_value;

    uint8_t  xi              = (uint8_t)origin_x; // BH-equivalent outer/X counter
    uint32_t outer_remaining = (uint32_t)span_x;
    do {
        uint8_t  yi              = (uint8_t)origin_y; // BL-equivalent inner/Y counter, reset per pass
        uint32_t inner_remaining = (uint32_t)span_y;
        do {
            if (*stencil != 0) {
                const uint32_t idx = (((uint32_t)xi << 8) | (uint32_t)yi) & *own.grid_wrap_mask;
                // CONCAT11(grid[idx],(char)seed_value) & 0xc0ff: preserve the OLD byte's top 2 bits,
                // OR in seed_value's whole low byte (not masked to 0x3f).
                grid[idx] = (uint8_t)((grid[idx] & 0xc0u) | seed_byte);
            }
            ++stencil;
            ++yi;
            --inner_remaining;
        } while (inner_remaining != 0);
        ++xi;
        --outer_remaining;
    } while (outer_remaining != 0);
}

} // namespace detail

void grid_stamp_threat_ring(uint8_t *grid, int32_t width, int32_t height, int32_t x, int32_t y,
                            int32_t radius) {
    const ai_state st = state();
    detail::grid_stamp_threat_ring(st.read, st.own, grid, width, height, x, y, radius);
}

void grid_clear_threat_bit(uint8_t *grid, int32_t width, int32_t height) {
    detail::grid_clear_threat_bit(grid, width, height);
}

void grid_fill_below_threshold(uint8_t *grid, int32_t width, int32_t height, int32_t threshold,
                               int32_t fill_value) {
    detail::grid_fill_below_threshold(grid, width, height, threshold, fill_value);
}

void grid_flood_step(uint8_t *grid, int32_t width, int32_t height, int32_t source_level,
                     int32_t fill_value) {
    const ai_state st = state();
    detail::grid_flood_step(st.own, grid, width, height, source_level, fill_value);
}

void grid_stamp_seeds(uint8_t *grid, int32_t map_width, int32_t map_height, uint8_t *stencil,
                      int32_t span_x, int32_t span_y, int32_t origin_x, int32_t origin_y,
                      int32_t seed_value) {
    const ai_state st = state();
    detail::grid_stamp_seeds(st.own, grid, map_width, map_height,
                             stencil, span_x, span_y, origin_x,
                             origin_y, seed_value);
}


} // namespace mh::ai
