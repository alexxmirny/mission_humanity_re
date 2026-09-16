//
// sim/sim_unit_state_budget_noop.cpp -- see sim_unit_state_budget_noop.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_unit_state_parked_noop_0047e276.asm and
// tmp/decomp/llm_strat_unit_state_default_noop_0047e2ac.asm), which are byte-for-byte identical
// bodies -- the exported .c drafts agree with the assembly (both fields), so this is a case where
// the drafts' logic was correct as written.
//
#include "sim/sim_unit_state_budget_noop.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void unit_state_parked_noop(sim_store &own) {
    // 0x0047e28e-0x0047e2a2: MOV [_G_LLM_STRAT_TICK_BUDGET],0 ; MOV [_G_LLM_STRAT_TICK_BUDGET+4],0 --
    // the whole 8-byte double zeroed via two dword stores (see the header derivation and
    // sim_state.h's tick_budget() comment for the identical idiom elsewhere in the closure).
    own.tick_budget() = 0.0;
}

void unit_state_default_noop(sim_store &own) {
    // 0x0047e2c4-0x0047e2d8: identical to unit_state_parked_noop above -- see the header note on why
    // this stays a separate detail:: function despite the identical body.
    own.tick_budget() = 0.0;
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------

void unit_state_parked_noop() {
    sim_state st = state();
    detail::unit_state_parked_noop(st.own);
}

void unit_state_default_noop() {
    sim_state st = state();
    detail::unit_state_default_noop(st.own);
}


} // namespace mh::sim
