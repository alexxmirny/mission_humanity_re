//
// sim/sim_dist_out_of_range.cpp -- see sim_dist_out_of_range.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_dist_out_of_range_00449ac1.asm), cross-checked against the Ghidra .c draft
// (tmp/decomp/llm_strat_dist_out_of_range_00449ac1.c), which matched the assembly branch-for-branch
// and register-for-register on a full manual re-trace.
//
#include "sim/sim_dist_out_of_range.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const dist_out_of_range_calls &live_dist_out_of_range_calls() {
    static const dist_out_of_range_calls c = {
        MH_LIBMH_BIND(llm_strat_tile_dist_wrapped),
    };
    return c;
}

namespace detail {

uint32_t dist_out_of_range(const dist_out_of_range_calls &c, int32_t range_min, int32_t range_max,
                           int32_t x, int32_t y, int32_t target_tile_x, int32_t target_tile_y) {
    // 0x00449ae2-0x00449af3: EAX=x, EDX=y, EBX=target_tile_x, ECX=target_tile_y at the call site --
    // llm_strat_tile_dist_wrapped's own committed (x1,y1,x2,y2) order, applied point-then-target.
    const int32_t dist = c.tile_dist_wrapped(x, y, target_tile_x, target_tile_y);

    // 0x00449af6-0x00449b04: `(range_max < dist) || (dist < range_min)` -> 1 (out of range), else 0.
    // Two independent JG/JGE branches, both landing on the same two result stores -- reproduced as one
    // boolean expression (neither operand has a side effect, so evaluation order does not matter).
    return (dist > range_max || dist < range_min) ? 1u : 0u;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

uint32_t dist_out_of_range(int32_t range_min, int32_t range_max, int32_t x, int32_t y,
                           int32_t target_tile_x, int32_t target_tile_y) {
    return detail::dist_out_of_range(live_dist_out_of_range_calls(), range_min, range_max, x, y,
                                     target_tile_x, target_tile_y);
}


} // namespace mh::sim
