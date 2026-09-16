//
// sim/sim_map_wrap_delta_xy.cpp -- see sim_map_wrap_delta_xy.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_map_wrap_delta_x_004940a9.asm, tmp/decomp_sim/llm_map_wrap_delta_y_00494131.asm),
// address-by-address; both bodies mirror sim_map_wrap_delta_row.cpp's own SAR-based signed-halving
// idiom, which C++'s signed `/2` already reproduces bit-for-bit (truncation toward zero).
//
// No entry-point seam: both functions have 0 tracked write cells (pure, no callee but the
// inert stack probe) per tmp/state_matrix.json -- matches this slice's write-set preflight (NOT
// SHADOWABLE). Evidence is the offline oracle (sim_map_wrap_delta_xy_selftest.cpp) plus adversarial
// review.
//
#include "sim/sim_map_wrap_delta_xy.h"

namespace mh::sim {

namespace detail {

int32_t wrap_delta_x(const sim_view &v, int32_t pos_a, uint32_t unused_param, int32_t pos_b) {
    (void)unused_param; // spilled to its stack home (0x004940c1) but never read again -- header banner.

    // 0x004940ca-0x004940f1: sign/magnitude split of the raw column delta.
    int32_t sign = 1;
    int32_t delta;
    if (pos_a > pos_b) {
        sign  = -1;
        delta = pos_a - pos_b;
    } else {
        delta = pos_b - pos_a;
    }

    // 0x004940f4-0x0049411a: fold across half the wrap extent (map width).
    const int32_t width = static_cast<int32_t>(*v.map_width);
    const int32_t half  = width / 2;
    if (half < delta) {
        delta = width - delta;
        sign  = -sign;
    }

    // 0x0049411d-0x00494124.
    return sign * delta;
}

int32_t wrap_delta_y(const sim_view &v, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    (void)x1; // spilled to its stack home (0x0049414c... via EBX) but never read again -- header banner.
    (void)x2; // spilled to its stack home but never read again -- header banner.

    // 0x00494152-0x00494179: sign/magnitude split of the raw row delta.
    int32_t sign = 1;
    int32_t delta;
    if (y1 > y2) {
        sign  = -1;
        delta = y1 - y2;
    } else {
        delta = y2 - y1;
    }

    // 0x0049417c-0x004941a2: fold across half the wrap extent (map height).
    const int32_t height = static_cast<int32_t>(*v.map_height);
    const int32_t half   = height / 2;
    if (half < delta) {
        delta = height - delta;
        sign  = -sign;
    }

    // 0x004941a5-0x004941ac.
    return sign * delta;
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

int32_t wrap_delta_x(int32_t pos_a, uint32_t unused_param, int32_t pos_b) {
    sim_state st = state();
    return detail::wrap_delta_x(st.read, pos_a, unused_param, pos_b);
}

int32_t wrap_delta_y(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    sim_state st = state();
    return detail::wrap_delta_y(st.read, x1, y1, x2, y2);
}

} // namespace mh::sim
