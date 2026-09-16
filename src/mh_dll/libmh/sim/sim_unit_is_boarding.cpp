//
// sim/sim_unit_is_boarding.cpp -- see sim_unit_is_boarding.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_unit_state_is_boarding_004967ce.asm); the Ghidra .c draft agrees exactly (same
// `state < 0x1f || 0x2b < state` shape), so there was nothing to correct.
//
#include "sim/sim_unit_is_boarding.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

int32_t unit_state_is_boarding(int32_t state) {
    // 0x004967e9-0x004967f3: state < 0x1f -> false; else state <= 0x2b -> true, else false. Both
    // ends inclusive, read straight off the two CMP/Jcc pairs -- see the header banner.
    return (state >= UNIT_STATE_BOARDING_RANGE_LO && state <= UNIT_STATE_BOARDING_RANGE_HI) ? 1 : 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t unit_state_is_boarding(int32_t state) { return detail::unit_state_is_boarding(state); }


} // namespace mh::sim
