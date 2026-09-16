//
// sim/sim_unit_set_state.cpp -- see sim_unit_set_state.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_set_state_004866c9.asm); the exported .c draft agrees with the
// assembly here (a single passthrough write to the current unit's `state` field), so this is a
// case where the draft's plate/logic were both worth re-deriving as asked, and both turned out
// correct.
//
#include "sim/sim_unit_set_state.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void unit_set_state(sim_store &own, uint16_t new_state) {
    // 0x004866e4-0x004866ed: EDX = _G_LLM_STRAT_CUR_UNIT; [EDX + 0x6] (word) = AX (new_state). The
    // sole effect of this function -- see sim_store::cur_unit() for the shared resolved pointer.
    own.cur_unit().state = new_state;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_set_state(uint16_t new_state) {
    sim_state st = state();
    detail::unit_set_state(st.own, new_state);
}


} // namespace mh::sim
