//
// sim/sim_map_wrap_delta_row.cpp -- see sim_map_wrap_delta_row.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_map_wrap_delta_row_0049446e.asm); the exported .c draft agrees field-for-field
// (its `general.big_height` read is this TU's `v.geom->big_height`, and its `(int)general.big_height
// / 2` is exactly the asm's SAR-based signed-halving idiom at 0x004944b9-0x004944c9 -- C++ signed
// integer division already truncates toward zero, so the literal `/2` reproduces the idiom without
// needing to hand-roll the EDX sign-extend-and-subtract dance).
//
#include "sim/sim_map_wrap_delta_row.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

int32_t wrap_delta_row(const sim_view &v, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    (void)x1; // spilled to its stack home (0x00494483) but never read again -- see the header banner.
    (void)x2; // spilled to its stack home (0x00494489) but never read again -- see the header banner.

    // 0x00494496-0x004944b6: sign/magnitude split of the raw row delta.
    //   y1 > y2 (i.e. NOT y1<=y2, the JLE-not-taken arm): sign=-1, delta=y1-y2.
    //   y1 <= y2 (LAB_004944b0):                          sign=+1 (unchanged), delta=y2-y1.
    int32_t sign = 1;
    int32_t delta;
    if (y1 > y2) {
        sign  = -1;
        delta = y1 - y2;
    } else {
        delta = y2 - y1;
    }

    // 0x004944b9-0x004944df: fold across half the wrap extent. half = trunc(big_height/2) via the
    // SAR-based signed-halving idiom (CDQ-equivalent sign spill + SUB + SAR 1); C++'s `/2` on a
    // signed int already truncates toward zero, matching this idiom bit-for-bit for both signs.
    const int32_t big_height = static_cast<int32_t>(v.geom->big_height);
    const int32_t half       = big_height / 2;
    if (half < delta) {
        delta = big_height - delta;
        sign  = -sign;
    }

    // 0x004944e2-0x004944e9.
    return sign * delta;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t wrap_delta_row(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    sim_state st = state();
    return detail::wrap_delta_row(st.read, x1, y1, x2, y2);
}


} // namespace mh::sim
