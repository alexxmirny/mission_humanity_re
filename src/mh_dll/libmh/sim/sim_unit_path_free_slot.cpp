//
// sim/sim_unit_path_free_slot.cpp -- see sim_unit_path_free_slot.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_path_free_slot_004969e8.asm), not from Ghidra's C: the draft's overall shape
// (guarded release, three sub-writes) reads correctly, but every field width, the row-major roster
// index arithmetic, and the guard's re-materialised reads were independently re-walked against the
// raw CMP/MOVZX/IMUL sequence per the translator brief.
//
#include "sim/sim_unit_path_free_slot.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void path_free_slot(const sim_view &v, sim_store &own, uint16_t player, int32_t unit_index) {
    (void)v; // no const-view read in this body -- everything touched is the mutable roster/scratch

    // 0x00496a18: CMP byte ptr [units[player][unit_index].path_slot_id],0xff; JZ -> skip the whole
    // body. One C++ read standing in for the asm's three re-derivations of the same field address
    // (guard compare, flags-table index, overwrite) -- see the header note on why that is faithful.
    unit &u = own.unit_at(player, unit_index);
    if (u.path_slot_id != 0xff) {
        // 0x00496a44: _G_LLM_STRAT_PATH_SLOT_FLAGS[player][path_slot_id] = 0.
        own.path_slot_flag_at(player, u.path_slot_id) = 0;
        // 0x00496a5e: release the slot id itself.
        u.path_slot_id = 0xff;
        // 0x00496a6c: INC _G_LLM_STRAT_PATH_FREE_SLOT_COUNT[player].
        own.path_free_slot_count_at(player) += 1;
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void path_free_slot(uint16_t player, int32_t unit_index) {
    sim_state st = state();
    detail::path_free_slot(st.read, st.own, player, unit_index);
}


} // namespace mh::sim
