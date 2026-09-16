//
// sim/libtrans/sim_lt_anim_time.cpp -- see sim_lt_anim_time.h. Translated from the DISASSEMBLY
// (tmp/decomp_lib_trans/cfg_GetAnimTime_004616fd.asm), cross-checked against the Ghidra .c draft,
// which matched the branch structure and index arithmetic exactly.
//
#include "sim/libtrans/sim_lt_anim_time.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

double cfg_get_anim_time(const sim_view &v, int32_t index_) {
    // 0x00461718-0x0046172f (setup): accumulator seeded 0.0, cursor seeded to the frame_index param.
    double  total  = 0.0;
    int32_t cursor = index_;

    // 0x0046172c-0x00461760: DO-WHILE -- the body always runs at least once (the test is at the
    // BOTTOM, at 0x00461759-0x00461760), so an animation whose very first node already has
    // `.next == 0` still contributes its `.time` once before exiting.
    int32_t next;
    do {
        // 0x00461732-0x0046173b: total += Anim[cursor+1].time (x87 FLD/FADD/FSTP, round-tripped
        // through memory every iteration -- see header for why a plain `double` accumulator under
        // /fp:precise reproduces this). See header for the `+1` index-arithmetic derivation
        // (0xae4c88 - Anim_base(0xae4c70) = 0x18 = 1*0x10 + 0x8, i.e. element (cursor+1)'s `.time`
        // at its own +0x8).
        total += v.anim_frames[cursor + 1].time;

        // 0x0046173e-0x00461750: `.next` is a RELATIVE DELTA added onto the running cursor (ADD, not
        // MOV) -- 0xae4c84 - 0xae4c70 = 0x14 = 1*0x10 + 0x4, i.e. element (cursor+1)'s `.next` at its
        // own +0x4. 0x00461753-0x00461760 re-reads this SAME element's `.next` (saved cursor, no
        // writer to Anim[] in between) to decide whether to loop again -- caching the one load here
        // and testing it below reproduces that re-read's value exactly, since nothing in this function
        // writes the region between the two reads.
        next = v.anim_frames[cursor + 1].next;
        cursor += next;

        // 0x00461759-0x00461760: no bounds check and no termination guard beyond `next != 0` -- a
        // zero-length loop back to the same node, or any cycle in the `.next` chain, spins forever in
        // the original. Reproduced as-is (translator-brief rule 12): no guard added.
    } while (next != 0);

    // 0x00461762-0x0046176e: return the accumulator, in ST0 in the original.
    return total;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

double cfg_get_anim_time(int32_t index_) {
    const sim_view v = state().read;
    return detail::cfg_get_anim_time(v, index_);
}


} // namespace mh::sim
