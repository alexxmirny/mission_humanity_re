//
// sim/sim_unit_set_state_of.cpp -- see sim_unit_set_state_of.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_set_state_of_004869b0.asm); the exported .c draft agrees with the
// assembly here (a single passthrough write to units[player][unit_index].state, via the row/column
// IMUL strides), so this is another case, like sim_unit_set_state.cpp's, where the draft's
// plate/logic were both worth re-deriving as asked, and both turned out correct.
//
#include "sim/sim_unit_set_state_of.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void unit_set_state_of(sim_store &own, int32_t player, int32_t unit_index, int16_t state) {
    // 0x004869cf-0x004869e2: EAX = player*0x5b04, EDX = unit_index*0xe9, EDX += EAX (row+col offset
    // into units[]), then MOV word ptr [EDX + units_base], AX -- units[player][unit_index].state =
    // state. The sole effect of this function; see sim_store::unit_at() for the reference-not-
    // pointer accessor (no base is ever kept, matching the original's fresh IMUL-per-call).
    own.unit_at(static_cast<uint32_t>(player), unit_index).state = static_cast<uint16_t>(state);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------
//
// The parameter is named `state` to match the original's own naming (mh_calls.gen.h's committed
// signature) -- which shadows the free function `mh::sim::state()`, so the call below is qualified
// rather than bare, same fix sim_unit_set_state.cpp's own wrapper avoided only by picking a
// differently-spelled parameter name.

void unit_set_state_of(int32_t player, int32_t unit_index, int16_t state) {
    sim_state st = mh::sim::state();
    detail::unit_set_state_of(st.own, player, unit_index, state);
}


} // namespace mh::sim
