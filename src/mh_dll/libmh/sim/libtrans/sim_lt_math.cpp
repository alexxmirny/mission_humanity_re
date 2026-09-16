//
// sim/libtrans/sim_lt_math.cpp -- see sim_lt_math.h. Translated from the DISASSEMBLY
// (tmp/decomp_lib_trans/llm_math_manhattan_dist_00489164.asm,
// tmp/decomp_lib_trans/llm_math_scale_pct_0044b3b3.asm), not from the Ghidra .c drafts beside them.
//
#include "sim/libtrans/sim_lt_math.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

// llm_math_manhattan_dist @0x00489164. See the header banner for the subtrahend-order and abs-idiom
// derivation; re-checked against the asm immediately before writing this body.
int32_t manhattan_dist(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    // 0x00489185-0x0048918b / 0x0048918e-0x00489194: x1-x0, y1-y0 -- NOT the other subtrahend order.
    int32_t dx = x1 - x0;
    int32_t dy = y1 - y0;
    // 0x00489197-0x004891a6 / 0x004891ae-0x004891bd: `if (d < 0) d = -d;` on plain int32_t reproduces
    // the original's CMP/JGE/NEG idiom bit-for-bit, INCLUDING at d == INT32_MIN: NEG on the two's-
    // complement bit pattern 0x80000000 yields 0x80000000 unchanged, and negating an int32_t already at
    // its own width (no promotion to a wider type happens here) reproduces exactly that wraparound, so
    // this stays bit-identical at every input, not just the ones a compiler happens not to exploit.
    if (dx < 0)
        dx = -dx;
    if (dy < 0)
        dy = -dy;
    return dx + dy;
}

// llm_math_scale_pct @0x0044b3b3. See the header banner for the x87 operation-order derivation.
double scale_pct(const sim_view &v, double value, int32_t pct) {
    // 0x0044b3cc-0x0044b3d2: FILD pct -> FMUL value -> FDIV [_G_LLM_MATH_PERCENT_DIVISOR]. `pct` is
    // the LEFT operand of the multiply, matching the FILD/FMUL order -- do NOT reassociate to
    // `value * pct` (translator-brief rule 9: a plausible-looking reassociation is a different x87
    // rounding). The divisor is read from the view, never spelled as a 100.0 literal.
    return (static_cast<double>(pct) * value) / *v.math_percent_divisor;
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

int32_t manhattan_dist(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    return detail::manhattan_dist(x0, y0, x1, y1);
}

double scale_pct(double value, int32_t pct) {
    const sim_view v = state().read;
    return detail::scale_pct(v, value, pct);
}


} // namespace mh::sim
