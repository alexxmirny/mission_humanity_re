//
// sim/sim_unit_set_state_order_of.cpp -- see sim_unit_set_state_order_of.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_unit_set_state_order_of_00486913.asm); the exported .c draft
// agrees with the assembly here (two passthrough writes to units[player][unit_index].state/.order, via
// the row/column IMUL strides recomputed once per store), so this is another case, like
// sim_unit_set_state_of.cpp's, where the draft's plate/logic were both worth re-deriving as asked, and
// both turned out correct.
//
#include "sim/sim_unit_set_state_order_of.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void unit_set_state_order_of(sim_store &own, int32_t player, int32_t unit_index, int16_t state,
                             int16_t param) {
    // 0x00486934-0x0048694e: EAX = player*0x5b04, EDX = unit_index*0xe9, EDX += EAX (row+col offset
    // into units[]), then MOV word ptr [EDX + 0xdd8c4e],AX -- units[player][unit_index].state = state.
    // 0x0048694e-0x00486968: the SAME row/col offset is recomputed a second time (fresh IMULs, not a
    // register kept live across both stores), then MOV word ptr [EDX + 0xdd8c4c],AX --
    // units[player][unit_index].order = param. 0xdd8c4c is 2 bytes below 0xdd8c4e, matching
    // mh_map_object_unit's order@0x4/state@0x6 field layout. See sim_store::unit_at() for the
    // reference-not-pointer accessor (no base is ever kept, matching the original's fresh
    // IMUL-per-store).
    own.unit_at(static_cast<uint32_t>(player), unit_index).state = static_cast<uint16_t>(state);
    own.unit_at(static_cast<uint32_t>(player), unit_index).order = static_cast<uint16_t>(param);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------
//
// The parameter is named `state` to match the original's own naming (mh_calls.gen.h's committed
// signature) -- which shadows the free function `mh::sim::state()`, so the call below is qualified
// rather than bare, same fix sim_unit_set_state_of.cpp's own wrapper avoided only by picking a
// differently-spelled parameter name.

void unit_set_state_order_of(int32_t player, int32_t unit_index, int16_t state, int16_t param) {
    sim_state st = mh::sim::state();
    detail::unit_set_state_order_of(st.own, player, unit_index, state, param);
}


} // namespace mh::sim
