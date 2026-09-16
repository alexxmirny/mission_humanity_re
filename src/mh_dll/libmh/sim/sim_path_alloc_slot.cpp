//
// sim/sim_path_alloc_slot.cpp -- see sim_path_alloc_slot.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_path_alloc_slot_0049a1ce.asm), not from any Ghidra `.c` draft.
//
#include "sim/sim_path_alloc_slot.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const path_alloc_slot_calls &live_path_alloc_slot_calls() {
    static const path_alloc_slot_calls c = {
        MH_LIBMH_BIND(llm_strat_path_attach_slot),
    };
    return c;
}

namespace detail {

int32_t path_alloc_slot(sim_store &own, const path_alloc_slot_calls &c, int32_t path_group_idx,
                        int32_t entity_id) {
    // 0x0049a1eb-0x0049a22a: linear scan slot in [0, PATH_SLOTS_PER_PLAYER) for the first one whose
    // flag != 1 (not busy). No early-exit shortcut beyond the first match -- unlike
    // claim_free_slots_within_dist's sibling scan, THIS loop DOES stop at the first hit (the asm jumps
    // straight to the shared epilogue at 0x0049a226 on a match, never falling through to the next
    // iteration).
    for (int32_t slot = 0; slot < PATH_SLOTS_PER_PLAYER; ++slot) {
        if (own.path_slot_flag_at(path_group_idx, slot) == 1) continue; // 0x0049a210: busy, try next

        // 0x0049a212-0x0049a21b: claim the slot. ALL state mutation
        // (PATH_SLOT_FLAGS[player][slot]=1, unit.path_slot_id=slot, PATH_FREE_SLOT_COUNT[player]--)
        // happens inside this ORIGINAL callee -- this function's own body writes nothing tracked.
        c.path_attach_slot(path_group_idx, entity_id, slot);

        return slot; // 0x0049a220-0x0049a226: EAX reloaded with the loop counter, not path_group_idx
                     // or entity_id.
    }
    return -1; // 0x0049a22a: all PATH_SLOTS_PER_PLAYER slots busy.
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t path_alloc_slot(int32_t path_group_idx, int32_t entity_id) {
    sim_state st = state();
    return detail::path_alloc_slot(st.own, live_path_alloc_slot_calls(), path_group_idx, entity_id);
}


} // namespace mh::sim
