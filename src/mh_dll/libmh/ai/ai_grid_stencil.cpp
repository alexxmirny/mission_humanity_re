//
// ai/ai_grid_stencil.cpp -- see ai_grid_stencil.h. Translated from the DISASSEMBLY
// (tmp/decomp_a6/llm_strat_ai_grid_match_stencil_004b4b1c.asm and
// tmp/decomp_a6/llm_strat_ai_grid_stencil_all_near_unthreatened_004b4b7a.asm), not from Ghidra's C.
//
// ---- WHICH AXIS IS WHICH, and why the committed Ghidra names are right HERE ------------------------
//
// Their layer-2 cousins in ai_grid.cpp have SWAPPED committed names; these two do not, and the
// difference has to be derived rather than assumed. Both bodies open with
//
//     MOV EAX,[EBP+0x10] ; DEC EAX ; MOV [0x00603ec8],AL      <- wrap-mask byte 0
//     MOV EAX,[EBP+0x0c] ; DEC EAX ; MOV [0x00603ec9],AL      <- wrap-mask byte 1
//
// and the parameter slots are the storage the .asm header commits, shifted uniformly by +4 into the
// body's EBP frame (Stack[0x4] == EBP+0x8, the slot loaded into EDI as `grid`):
//
//     EBP+0x08 grid            EBP+0x18 span_x        (outer counter, DEC'd IN PLACE)
//     EBP+0x0c grid_width      EBP+0x1c span_y        (inner counter, into ECX)
//     EBP+0x10 grid_height     EBP+0x20 start_x       (into BH)
//     EBP+0x14 footprint_mask  EBP+0x24 start_y       (into BL, re-read every outer pass)
//     EBP+0x28 target_byte     (match_stencil only, into AL)
//
// Little-endian, so wrap-mask bits 0-7 mask BL and bits 8-15 mask BH. BL is therefore masked by
// (grid_height - 1) and BH by (grid_width - 1), and the byte index is `(BH << 8) | BL` -- which is
// tile_at()'s X-outer/Y-inner packing (docs/structs.md on ai_tile_flags_grid: 256x256 bytes indexed
// (x << 8) | y). So BH is the X axis and BL the Y axis, and the names line up: grid_width is the X
// extent, start_x seeds BH, span_x counts the outer pass. Cross-checked from the CALLER side, which
// is independent of any of the above: ai_site_scan.cpp and ai_resource_sites.cpp both call these as
// `(grid, *v.map_width, *v.map_height, footprint, FOOTPRINT_SPAN, FOOTPRINT_SPAN, x, y)` and __cdecl
// pushes right-to-left, so *v.map_width lands in EBP+0xc. Both derivations agree.
//
// (The layer-2 pair's names are swapped because turret_threat_rescan passes *v.map_width into the
// slot Ghidra called grid_height there. Same evidence, opposite outcome -- which is why it is redone
// here instead of inherited.)
//
// ---- THE WRAP IS DESTRUCTIVE AND CONDITIONAL, and that is the only real trap in these bodies -------
//
// The inner loop is
//
//     CMP byte ptr [ESI],0x0 ; JZ skip          <- the footprint mask: 0 means "don't care"
//     AND EBX,dword ptr [0x00603ec8]            <- masks BH *and* BL, IN PLACE
//     <predicate on grid[EBX]> ; J<fail> 0
//   skip:
//     INC BL ; INC ESI ; DEC ECX ; JNZ inner
//
// Three consequences a "clean" rewrite gets wrong:
//   1. The AND runs ONLY for care cells. A row of all-zero mask bytes leaves BH/BL unmasked, and
//      the following `INC BL` continues from the UNMASKED value. (It happens to be observationally
//      equivalent, because masking commutes with the increments here -- (v & m) + 1 and v + 1 agree
//      once masked again for m of the form 2^k - 1 -- but that is an argument, not an assumption, so
//      the structure is reproduced exactly rather than leaned on.)
//   2. The AND is IN PLACE: once a care cell masks BL down, the rest of that row walks from the
//      masked value. Same for BH, which is never reloaded -- so a masked BH persists into every
//      LATER row. Only BL is re-read from the stack per outer pass (`MOV BL,[EBP+0x24]`).
//   3. BH and BL are 8-BIT and wrap at 256 independently of the wrap mask.
// This is a torus walk, so all of it only becomes observable when the window crosses the seam --
// which the offline aitest case drives deliberately, because a rig scenario reaches it only by luck.
//
// EBX's bits 16-31 are provably 0 for the whole body (`XOR EBX,EBX` then only BH/BL writes), so the
// single 32-bit AND is split into two byte ANDs below with no loss: 0 AND anything is 0, whatever the
// wrap mask's own upper half holds.
//
#include "ai/ai_grid_stencil.h"


namespace mh::ai {
namespace detail {
namespace {

// The shared entry prologue of both bodies: two BYTE stores into the low two bytes of the torus wrap
// mask. Bits 16-31 are left EXACTLY as they were (the original only ever touches the low two), which
// is the same treatment ai_grid.cpp gives the same scratch.
inline void store_wrap_mask(const ai_store &own, int32_t grid_width, int32_t grid_height) {
    *own.grid_wrap_mask = (*own.grid_wrap_mask & 0xffff0000u) |
                          (((uint32_t)(uint8_t)(grid_width - 1)) << 8) |
                          ((uint32_t)(uint8_t)(grid_height - 1));
}

} // namespace

int32_t grid_match_stencil(const ai_view &v, const ai_store &own, const uint8_t *grid,
                           int32_t grid_width, int32_t grid_height, const uint8_t *footprint_mask,
                           int32_t span_x, int32_t span_y, int32_t start_x, int32_t start_y,
                           int32_t target_byte) {
    store_wrap_mask(own, grid_width, grid_height);

    const uint8_t *mask   = footprint_mask;       // ESI -- contiguous, never reset per row
    const uint8_t  target = (uint8_t)target_byte; // AL -- only the low byte is ever compared
    uint8_t        bh     = (uint8_t)start_x;     // BH, seeded once and never reloaded
    int32_t        outer  = span_x;               // the original DECs the stack slot in place
    do {
        uint8_t bl    = (uint8_t)start_y; // MOV BL,[EBP+0x24] -- re-read every outer pass
        int32_t inner = span_y;           // ECX
        do {
            if (*mask != 0) {
                const uint32_t wrap = *v.grid_wrap_mask; // re-read inside the loop, as the .asm does
                bh &= (uint8_t)((wrap >> 8) & 0xffu);
                bl &= (uint8_t)(wrap & 0xffu);
                if (grid[((uint32_t)bh << 8) | (uint32_t)bl] != target) return 0;
            }
            ++bl; // INC BL -- 8-bit, wraps at 256
            ++mask;
            --inner;
        } while (inner != 0); // DEC ECX/JNZ -- at least one inner pass even for span_y == 0
        ++bh; // INC BH -- 8-bit, and NOT reset between rows
        --outer;
    } while (outer != 0); // DEC [EBP+0x18]/JNZ -- at least one outer pass even for span_x == 0
    return 1;
}

int32_t grid_stencil_all_near_unthreatened(const ai_view &v, const ai_store &own,
                                           const uint8_t *grid, int32_t grid_width,
                                           int32_t grid_height, const uint8_t *footprint_mask,
                                           int32_t span_x, int32_t span_y, int32_t start_x,
                                           int32_t start_y) {
    store_wrap_mask(own, grid_width, grid_height);

    const uint8_t *mask  = footprint_mask;
    uint8_t        bh    = (uint8_t)start_x;
    int32_t        outer = span_x;
    do {
        uint8_t bl    = (uint8_t)start_y;
        int32_t inner = span_y;
        do {
            if (*mask != 0) {
                const uint32_t wrap = *v.grid_wrap_mask;
                bh &= (uint8_t)((wrap >> 8) & 0xffu);
                bl &= (uint8_t)(wrap & 0xffu);
                // The ONLY difference from the sibling above: a fixed two-part test instead of an
                // equality against target_byte. Order matters only for which branch reports the
                // failure -- both return 0 -- but it is kept as written.
                const uint8_t cell = grid[((uint32_t)bh << 8) | (uint32_t)bl];
                if ((cell & TILE_FLAG_TURRET_THREAT) != 0) return 0;              // TEST AL,0x40/JNZ
                if ((cell & TILE_INFLUENCE_DIST_MASK) > TILE_INFLUENCE_DIST_NEAR) // AND AL,0x1f
                    return 0;                                                     // CMP AL,3/JA
            }
            ++bl;
            ++mask;
            --inner;
        } while (inner != 0);
        ++bh;
        --outer;
    } while (outer != 0);
    return 1;
}

} // namespace detail

int32_t grid_match_stencil(const uint8_t *grid, int32_t grid_width, int32_t grid_height,
                           const uint8_t *footprint_mask, int32_t span_x, int32_t span_y,
                           int32_t start_x, int32_t start_y, int32_t target_byte) {
    const ai_state st = state();
    return detail::grid_match_stencil(st.read, st.own, grid, grid_width, grid_height, footprint_mask,
                                      span_x, span_y, start_x, start_y, target_byte);
}

int32_t grid_stencil_all_near_unthreatened(const uint8_t *grid, int32_t grid_width,
                                           int32_t grid_height, const uint8_t *footprint_mask,
                                           int32_t span_x, int32_t span_y, int32_t start_x,
                                           int32_t start_y) {
    const ai_state st = state();
    return detail::grid_stencil_all_near_unthreatened(st.read, st.own, grid, grid_width, grid_height,
                                                      footprint_mask, span_x, span_y, start_x,
                                                      start_y);
}


} // namespace mh::ai
