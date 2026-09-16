//
// sim/sim_tile_delta_wrapped.cpp -- see sim_tile_delta_wrapped.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_tile_delta_wrapped_004941b9.asm), not the Ghidra C draft. The draft's VALUE
// logic reads correctly; the branch-selected sign, the positive-subtraction-per-branch (not abs),
// and the round-toward-zero dim/2 threshold were re-derived from the listing per house rules.
//
#include "sim/sim_tile_delta_wrapped.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

namespace {
// One axis of the shorter-of-two-wrap-paths signed delta (0x004941da-0x00494237 for X, the identical
// shape 0x00494239-0x00494294 for Y). `dim` is a positive map dimension.
int32_t axis_delta(int32_t start, int32_t end, int32_t dim) {
    int32_t sign, d;
    // 0x004941dd-0x00494201: CMP start,end / JLE -- if start > end take the negative direction, else
    // positive; `d` is the positive subtraction on whichever branch was chosen (never a separate abs).
    if (start > end) {
        sign = -1;
        d    = start - end;
    } else {
        sign = 1;
        d    = end - start;
    }
    // 0x00494204-0x00494214: dim/2 = (dim - (dim>>31)) >> 1, round-toward-zero halving of a positive
    // dimension (== dim>>1). 0x00494216/0x00494219: if dim/2 < d, fold to the shorter path and flip
    // the sign.
    const int32_t half = (dim - (dim >> 31)) >> 1;
    if (half < d) {
        d    = dim - d;
        sign = -sign;
    }
    // 0x0049422d-0x00494237: *out = sign * d.
    return sign * d;
}
} // namespace

void tile_delta_wrapped(const sim_view &v, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                        int32_t *out_dx, int32_t *out_dy) {
    *out_dx = axis_delta(x1, x2, *v.map_width);
    *out_dy = axis_delta(y1, y2, *v.map_height);
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void tile_delta_wrapped(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t *out_dx,
                        int32_t *out_dy) {
    const sim_view v = state().read;
    detail::tile_delta_wrapped(v, x1, y1, x2, y2, out_dx, out_dy);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
