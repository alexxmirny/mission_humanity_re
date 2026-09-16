//
// sim/sim_tile_pixel_wrap_delta.cpp -- see sim_tile_pixel_wrap_delta.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_tile_dist_wrapped_0049404e.asm,
// tmp/decomp/llm_strat_pixel_delta_wrapped_004944f6.asm,
// tmp/decomp/llm_strat_map_wrapped_delta_004945de.asm); the Ghidra .c drafts agree with the asm for
// all three bodies (confirmed branch-for-branch and idiom-for-idiom against the raw instructions, not
// trusted blindly per the batch context's caveat).
//
#include "sim/sim_tile_pixel_wrap_delta.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const tile_dist_wrapped_calls &live_tile_dist_wrapped_calls() {
    static const tile_dist_wrapped_calls c = {
        MH_LIBMH_BIND(llm_strat_tile_delta_wrapped),
    };
    return c;
}

namespace detail {

// ---- llm_strat_tile_dist_wrapped @0x0049404e --------------------------------------------------

int32_t tile_dist_wrapped(const tile_dist_wrapped_calls &c, int32_t x1, int32_t y1, int32_t x2,
                          int32_t y2) {
    // 0x0049406f-0x00494083: out_dx == &dx, out_dy == &dy -- see the header's push-order derivation.
    int32_t dx = 0, dy = 0;
    c.tile_delta_wrapped(x1, y1, x2, y2, &dx, &dy);

    // 0x00494088-0x0049409a: abs(dx) + abs(dy). REVIEW FINDING (reimpl-verify, 2026-08-17): the
    // original's CDQ/XOR/SUB idiom (MOV EAX,dx; CDQ; XOR EAX,EDX; SUB EAX,EDX) is fully-defined 32-bit
    // wraparound at dx==INT32_MIN (produces 0x80000000 unchanged), whereas a plain `-dx` there is
    // signed-overflow UB in C++ -- reproduce the original's own mask idiom instead of a conditional
    // negate, so this stays defined (and bit-identical) at every input, not just the ones a compiler
    // happens not to exploit.
    const int32_t dx_mask = dx >> 31;
    const int32_t dy_mask = dy >> 31;
    const int32_t abs_dx  = (dx ^ dx_mask) - dx_mask;
    const int32_t abs_dy  = (dy ^ dy_mask) - dy_mask;
    return abs_dx + abs_dy;
}

// ---- llm_strat_pixel_delta_wrapped @0x004944f6 / llm_strat_map_wrapped_delta @0x004945de -------
//
// Byte-for-byte the same control flow, over the same v.geom->big_width/big_height pair -- see the
// header banner. Kept as two separate bodies (not a shared template/helper) because they are two
// separate original functions with two separate .asm listings and two separate call-site sets, per
// the translator brief's "one function in, one function out" rule (no new shared helpers).

void pixel_delta_wrapped(const sim_view &v, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                         int32_t *out_dx, int32_t *out_dy) {
    // ---- X axis (0x00494517-0x00494567) ----
    int32_t sign_x, delta_x;
    if (x2 < x1) {
        sign_x  = -1;
        delta_x = x1 - x2;
    } else {
        sign_x  = 1;
        delta_x = x2 - x1;
    }
    const int32_t big_width = static_cast<int32_t>(v.geom->big_width);
    if (big_width / 2 < delta_x) {
        delta_x = big_width - delta_x;
        sign_x  = -sign_x;
    }
    *out_dx = sign_x * delta_x;

    // ---- Y axis (0x00494576-0x004945c6), same shape over big_height ----
    int32_t sign_y, delta_y;
    if (y2 < y1) {
        sign_y  = -1;
        delta_y = y1 - y2;
    } else {
        sign_y  = 1;
        delta_y = y2 - y1;
    }
    const int32_t big_height = static_cast<int32_t>(v.geom->big_height);
    if (big_height / 2 < delta_y) {
        delta_y = big_height - delta_y;
        sign_y  = -sign_y;
    }
    *out_dy = sign_y * delta_y;
}

void map_wrapped_delta(const sim_view &v, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                       double *out_dx, double *out_dy) {
    // Identical shape to pixel_delta_wrapped -- see that function's own inline comments; only the
    // final store differs (int->double widening, 0x0049465f-0x00494662 / 0x004946c4-0x004946c7's
    // FILD/FSTP, which a plain C++ `(double)` cast reproduces exactly since int32->double is exact,
    // no rounding).

    // ---- X axis (0x004945ff-0x00494659) ----
    int32_t sign_x, delta_x;
    if (x2 < x1) {
        sign_x  = -1;
        delta_x = x1 - x2;
    } else {
        sign_x  = 1;
        delta_x = x2 - x1;
    }
    const int32_t big_width = static_cast<int32_t>(v.geom->big_width);
    if (big_width / 2 < delta_x) {
        delta_x = big_width - delta_x;
        sign_x  = -sign_x;
    }
    *out_dx = static_cast<double>(sign_x * delta_x);

    // ---- Y axis (0x00494664-0x004946be) ----
    int32_t sign_y, delta_y;
    if (y2 < y1) {
        sign_y  = -1;
        delta_y = y1 - y2;
    } else {
        sign_y  = 1;
        delta_y = y2 - y1;
    }
    const int32_t big_height = static_cast<int32_t>(v.geom->big_height);
    if (big_height / 2 < delta_y) {
        delta_y = big_height - delta_y;
        sign_y  = -sign_y;
    }
    *out_dy = static_cast<double>(sign_y * delta_y);
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

int32_t tile_dist_wrapped(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    return detail::tile_dist_wrapped(live_tile_dist_wrapped_calls(), x1, y1, x2, y2);
}

void pixel_delta_wrapped(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t *out_dx,
                         int32_t *out_dy) {
    const sim_view v = state().read;
    detail::pixel_delta_wrapped(v, x1, y1, x2, y2, out_dx, out_dy);
}

void map_wrapped_delta(int32_t x1, int32_t y1, int32_t x2, int32_t y2, double *out_dx, double *out_dy) {
    const sim_view v = state().read;
    detail::map_wrapped_delta(v, x1, y1, x2, y2, out_dx, out_dy);
}


} // namespace mh::sim
