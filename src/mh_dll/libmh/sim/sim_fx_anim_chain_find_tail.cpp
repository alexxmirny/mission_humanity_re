//
// sim/sim_fx_anim_chain_find_tail.cpp -- see sim_fx_anim_chain_find_tail.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_fx_anim_chain_find_tail_0045583c.asm), not from the Ghidra .c draft.
//
#include "sim/sim_fx_anim_chain_find_tail.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

int32_t fx_anim_chain_find_tail(const sim_view &v, int32_t start_frame) {
    int32_t idx = start_frame;
    for (;;) {
        // 0x00455863/0x0045587a: Anim[idx + 1].next -- see header banner for the address-arithmetic
        // identity confirming this indexing. Read once per iteration (the asm re-fetches it for the
        // add path, but nothing writes Anim between the two fetches -- boot-loaded cfg data, no
        // writer in the sim closure -- so this is behaviourally identical).
        const int32_t next = v.anim_frames[idx + 1].next;

        // 0x0045586c-0x00455872: TAIL FOUND. next==0 -- return the current index as-is.
        if (next == 0) return idx;

        // 0x00455880: idx += next.
        idx += next;

        // 0x00455886-0x00455891: CYCLE GUARD. Looped back to the original start_frame -- return it.
        if (idx == start_frame) return start_frame;

        // 0x00455893: otherwise, loop.
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t fx_anim_chain_find_tail(int32_t start_frame) {
    const sim_view v = state().read;
    return detail::fx_anim_chain_find_tail(v, start_frame);
}


} // namespace mh::sim
