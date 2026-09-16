//
// sim/sim_geom_toroidal.cpp -- see sim_geom_toroidal.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_toroidal_dist_sq_00669f90.asm,
// tmp/decomp/llm_strat_tile_midpoint_wrapped_00669fe8.asm), not from Ghidra's C drafts -- both drafts
// read correctly for the VALUE logic (independently re-verified below against the raw bytes), so
// nothing here corrects their arithmetic, but the abs-via-CDQ/XOR/SUB idiom, the SAR-vs-C-`/2` rounding
// direction (hazard #8), and the unsigned-DIV-as-modulo step were all re-derived from the listings
// rather than transcribed from the plates, per house rules.
//
#include "sim/sim_geom_toroidal.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

uint32_t toroidal_dist_sq(const sim_view &v, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    const uint32_t width  = (uint32_t)*v.map_width;
    const uint32_t height = (uint32_t)*v.map_height;

    // 0x00669f96-0x00669fa9: abs(x1-x2) via the classic CDQ/XOR/SUB idiom (sign-extend the diff into
    // an all-0s/all-1s mask, XOR then SUB it back out). Reproduced with the same bit ops rather than
    // std::abs() so a diff of INT_MIN behaves identically (wraps) instead of hitting std::abs's UB
    // there.
    const int32_t  dx_raw  = x1 - x2;
    const uint32_t dx_mask = (uint32_t)(dx_raw >> 31); // CDQ: all-1s if negative, else all-0s
    uint32_t       dx      = ((uint32_t)dx_raw ^ dx_mask) - dx_mask;
    // 0x00669fa1-0x00669fb3: fold to the shorter wrap path if the abs delta exceeds half the width.
    if (dx > (width >> 1)) dx = width - dx;

    const int32_t  dy_raw  = y1 - y2;
    const uint32_t dy_mask = (uint32_t)(dy_raw >> 31);
    uint32_t       dy      = ((uint32_t)dy_raw ^ dy_mask) - dy_mask;
    // 0x00669fc8-0x00669fda: same fold over height.
    if (dy > (height >> 1)) dy = height - dy;

    // 0x00669fb9/0x00669fe0: `MUL EAX` keeps only the low dword (EAX) of the 64-bit product -- a
    // truncating 32x32->32 multiply. uint32_t multiplication below has the identical wraparound.
    return dx * dx + dy * dy;
}

void tile_midpoint_wrapped(const sim_view &v, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                           int32_t *out_x, int32_t *out_y) {
    const int32_t width  = *v.map_width;
    const int32_t height = *v.map_height;

    // ---- X axis (0x00669fef-0x0066a023) ----
    {
        const int32_t half_w = width >> 1; // 0x00669ffa: SHR EDX,1 -- width is always positive here,
                                           // so the arithmetic/logical shift distinction is moot.
        int32_t dx = x1 - x0;              // 0x00669ffc
        if (dx > half_w) dx -= width;      // 0x00669fff/0x0066a001: CMP/JLE (signed)
        if (dx < -half_w) dx += width;     // 0x0066a009-0x0066a013: NEG EDX then CMP/JGE (signed)
        // 0x0066a015: SAR EAX,1 -- arithmetic (floor) right shift, NOT truncating C `/2` (hazard #8:
        // the two differ whenever dx is negative and odd).
        const int32_t numerator = (dx >> 1) + x0 + width; // 0x0066a017-0x0066a01c
        // 0x0066a01a/0x0066a01e: XOR EDX,EDX then unsigned DIV EBX -- the remainder EDX is numerator's
        // 32-bit pattern read as unsigned, taken mod width. numerator is non-negative by construction
        // here (the two folds above bound dx to roughly [-width/2, width/2], so dx>>1 is roughly
        // [-width/4, width/4]; x0 in [0,width) and the extra +width push the sum comfortably positive)
        // -- the original relies on the same assumption, never re-checking the sign before the DIV.
        *out_x = (int32_t)((uint32_t)numerator % (uint32_t)width); // 0x0066a01e-0x0066a023
    }

    // ---- Y axis (0x0066a025-0x0066a059), identical shape over height ----
    {
        const int32_t half_h = height >> 1;
        int32_t       dy     = y1 - y0;
        if (dy > half_h) dy -= height;
        if (dy < -half_h) dy += height;
        const int32_t numerator = (dy >> 1) + y0 + height;
        *out_y                  = (int32_t)((uint32_t)numerator % (uint32_t)height);
    }
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

uint32_t toroidal_dist_sq(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    const sim_view v = state().read;
    return detail::toroidal_dist_sq(v, x1, y1, x2, y2);
}

void tile_midpoint_wrapped(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t *out_x, int32_t *out_y) {
    const sim_view v = state().read;
    detail::tile_midpoint_wrapped(v, x0, y0, x1, y1, out_x, out_y);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
