//
// sim/sim_storage_purge_dead_docked.cpp -- see sim_storage_purge_dead_docked.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_storage_purge_dead_docked_0049b8f5.asm), not from the Ghidra
// .c draft -- the draft's overall shape (single loop over docked_units, backlink repair then an
// energy<=0 removal, both independent per iteration) reads correctly and was used as a map, but every
// index expression, field offset, and width was independently re-walked against the raw
// IMUL/MOVZX/CMP/JZ/FLDZ/FCOMP/JC opcodes per house rules.
//
#include "sim/sim_storage_purge_dead_docked.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const storage_purge_dead_docked_calls &live_storage_purge_dead_docked_calls() {
    static const storage_purge_dead_docked_calls c = {
        MH_LIBMH_BIND(llm_strat_storage_remove_docked_unit),
    };
    return c;
}

namespace detail {

void storage_purge_dead_docked(const sim_view &v, sim_store &own, const storage_purge_dead_docked_calls &c,
                               int32_t player, int32_t storage_sub_id) {
    const uint32_t p = static_cast<uint32_t>(player);

    // 0x0049b919-0x0049b9c8: walk unit_storage[player][storage_sub_id].docked_units[0..docked_count).
    // Bound through a reference so `.docked_count`/`.docked_units[i]` are re-read from memory on every
    // visit (see header note: the outward call below is documented to mutate THIS exact slot mid-walk,
    // and the asm itself re-derives the row/slot base at both the bound check, 0x0049b919-0x0049b929,
    // and the element read, 0x0049b941-0x0049b959).
    const unit_storage &storage = storage_of(v, p, storage_sub_id);
    for (int32_t i = 0; i < storage.docked_count; ++i) {
        const int32_t unit_index = storage.docked_units[i]; // 0x0049b959

        // 0x0049b972-0x0049b997: repair the home_storage_slot backlink if it drifted from this slot.
        // The compare widens the byte field to 32 bits (0x0049b979); the write stores only the low
        // byte of storage_sub_id (0x0049b991).
        if (unit_of(v, p, unit_index).home_storage_slot != storage_sub_id) {
            own.unit_at(p, unit_index).home_storage_slot = static_cast<uint8_t>(storage_sub_id);
        }

        // 0x0049b9a7-0x0049b9be: energy <= 0.0 (x87 FLDZ/FCOMP/FNSTSW/SAHF/JC) -- release the dead
        // unit from this dock. Independent of the branch above (both are tested every visited
        // iteration in sequence, not else-if). `player` is narrowed to its low 16 bits for the call
        // (0x0049b9ba, MOVZX word), matching the callee's committed uint16_t first parameter.
        if (unit_of(v, p, unit_index).energy <= 0.0) {
            c.storage_remove_docked_unit(static_cast<uint16_t>(player), unit_index, storage_sub_id);
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void storage_purge_dead_docked(int32_t player, int32_t storage_sub_id) {
    sim_state st = state();
    detail::storage_purge_dead_docked(st.read, st.own, live_storage_purge_dead_docked_calls(), player,
                                      storage_sub_id);
}


} // namespace mh::sim
