//
// sim/sim_fx_anim_seq_cancel.cpp -- see sim_fx_anim_seq_cancel.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_fx_anim_seq_cancel_004539cc.asm), not from the Ghidra .c draft.
//
#include "sim/sim_fx_anim_seq_cancel.h"

#include "addr/mh_calls.gen.h"  // mh::call::llm_fx_anim_chain_find_tail -- bound live below
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const fx_anim_seq_cancel_calls &live_fx_anim_seq_cancel_calls() {
    static const fx_anim_seq_cancel_calls c = {
        MH_LIBMH_BIND(llm_fx_anim_chain_find_tail),
    };
    return c;
}

namespace detail {

void fx_anim_seq_cancel(const sim_view &v, sim_store &own, const fx_anim_seq_cancel_calls &c,
                        int32_t anim_seq_start_frame) {
    // 0x004539ea: the frame-chain tail for this anim sequence (the original's local_18).
    const int32_t tail = c.chain_find_tail(anim_seq_start_frame);

    // 0x004539f2/0x004539f7: local copy of the pool's live-count header (slot 0's `.live`), read ONCE
    // before the loop starts. This is an early-exit BUDGET, not the header itself -- see the header
    // banner: it is decremented once per live slot visited (whether or not that slot matches) and is
    // never written back. Once it hits 0 there is nothing left to find.
    int32_t remaining_live = v.fx_anim_pool[0].live;

    // 0x00453a01-0x00453a0e: loop condition -- index < 10000 AND remaining_live != 0, both checked
    // every iteration (index bound first).
    for (int32_t index = 1; index < 10000 && remaining_live != 0; ++index) {
        // 0x00453a1e/0x00453a25: a dead slot is skipped WITHOUT consuming the budget (JZ lands past
        // the DEC) -- matches every other roster walker's "hole costs nothing" idiom in this closure.
        if (v.fx_anim_pool[index].live == 0) continue;

        // 0x00453a27-0x00453a2a: consume one unit of the local budget for every LIVE slot, regardless
        // of whether its frame turns out to be in range.
        --remaining_live;

        // 0x00453a31-0x00453a49: [anim_seq_start_frame, tail] inclusive on both ends, both signed
        // int32 compares (re-reads `.anim_frame` twice, matching the asm's two fresh IMUL-indexed
        // loads rather than caching it in a register across the two CMPs).
        const int32_t frame = v.fx_anim_pool[index].anim_frame;
        if (frame < anim_seq_start_frame || tail < frame) continue;

        // 0x00453a51: clear this slot's own live flag.
        own.fx_anim_pool_at(index).live = 0;
        // 0x00453a5b: decrement the POOL's real live-count header (slot 0's `.live`) -- a DIFFERENT
        // dword from `remaining_live` above, touched only here, once per actual match.
        own.fx_anim_pool_at(0).live -= 1;
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void fx_anim_seq_cancel(int32_t anim_seq_start_frame) {
    sim_state st = state();
    detail::fx_anim_seq_cancel(st.read, st.own, live_fx_anim_seq_cancel_calls(), anim_seq_start_frame);
}


} // namespace mh::sim
