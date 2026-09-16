//
// sim/sim_path_attach_slot.cpp -- see sim_path_attach_slot.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_path_attach_slot_00496a7b.asm), address-by-address.
//
#include "sim/sim_path_attach_slot.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void path_attach_slot(sim_store &own, int32_t player, int32_t unit_idx, int32_t slot) {
    own.path_slot_flag_at(player, slot)        = 1;                          // 0x00496aa1
    own.unit_at(player, unit_idx).path_slot_id = static_cast<uint8_t>(slot); // 0x00496abb
    --own.path_free_slot_count_at(player);                                   // 0x00496ac7
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void path_attach_slot(int32_t player, int32_t unit_idx, int32_t slot) {
    sim_state st = state();
    detail::path_attach_slot(st.own, player, unit_idx, slot);
}


} // namespace mh::sim
