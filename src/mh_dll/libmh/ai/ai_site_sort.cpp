//
// ai/ai_site_sort.cpp -- see ai_site_sort.h. Translated from the DISASSEMBLY
// (tmp/decomp_a5/llm_strat_ai_sort_site_candidates_by_dist_004e6228.asm), not from Ghidra's C
// (tmp/decomp_a5/llm_strat_ai_sort_site_candidates_by_dist_004e6228.c). The .c's `bVar1` /
// `!bVar1` spelling of the early-exit flag reads as the obvious "keep going while a pass swapped
// something" idiom, and it happens to reproduce the right OBSERVABLE behaviour, but the register it
// is standing in for -- the original's ECX -- means the opposite of what that spelling suggests: it
// is 0 on entry, set to 1 at the top of EVERY pass (assume no swap yet), and cleared to 0 the moment
// any swap happens; the pass loop's own top (`TEST ECX,ECX ; JNZ exit`) leaves only when ECX is
// STILL 1, i.e. the PREVIOUS pass made no swap at all. Get this backwards -- e.g. exit when a swap
// DID happen -- and the sort runs at most one pass instead of running to a fixed point.
//
#include "ai/ai_site_sort.h"


namespace mh::ai {
namespace detail {

void sort_site_candidates_by_dist(const ai_view &v, const ai_store &own) {
    // count == 0: the very first instruction after the prologue (0x004e623a
    // `CMP [site_candidate_count],0` ; `JZ` straight to the shared epilogue) leaves before any pass
    // runs at all.
    const int32_t count = *v.site_candidate_count;
    if (count == 0) return;

    // `no_swap` is the original's ECX (see the file comment above for the polarity derivation):
    // false (0) on entry -- guaranteeing the outer loop's own top-of-loop TEST/JNZ falls through the
    // first time, so at least one full pass always runs for a nonzero count -- then set true at the
    // top of every pass and cleared by any swap within it.
    bool no_swap = false;
    while (!no_swap) {
        no_swap = true;

        // ONE DELIBERATE INSTRUCTION-LEVEL ASYMMETRY, kept rather than papered over (adversarial
        // review, 2026-08-01): the original's inner bound test is UNSIGNED (`CMP EDX,EAX ; JC` at
        // 0x004e629f) and this is a signed int32_t compare. They differ only for a NEGATIVE count,
        // where the original would read count - 1 as a huge unsigned bound and run far off the end.
        // The count is append-only from zero -- nothing in the image or in this tree ever decrements
        // it, and llm_strat_ai_plan_construction only ever zeroes it -- so no call path can produce
        // one. Recorded because "unreachable" is a claim about the WRITERS, and it stops being true
        // the moment a writer is added.
        //
        // count == 1: this pass's inner bound is count - 1 == 0, so the inner loop body runs zero
        // times (matches the .asm's inner-condition check at LAB_004e6299 failing on its very first
        // visit, EDX=0 vs EAX=count-1=0, JC not taken) and the sort exits on the pass loop's SECOND
        // visit to its top, having never touched a single record.
        for (int32_t i = 0; i < count - 1; ++i) {
            // UNSIGNED comparison on dist_sq (`CMP EBX,[next] ; JBE skip`): swap only when the
            // current record's key is STRICTLY GREATER than the next record's, so records with
            // equal keys are never swapped against each other and keep their relative order.
            if (own.site_candidates[i].dist_sq > own.site_candidates[i + 1].dist_sq) {
                // The swap moves the WHOLE 16-byte record -- all four fields (tile_x, tile_y, kind,
                // dist_sq) -- through a stack temp via three REP MOVSD runs in the original, not
                // just the sort key. site_candidate is a trivially-copyable 16-byte POD, so a
                // struct-value copy reproduces that byte-for-byte.
                const site_candidate tmp   = own.site_candidates[i + 1];
                own.site_candidates[i + 1] = own.site_candidates[i];
                own.site_candidates[i]     = tmp;
                no_swap                    = false;
            }
        }
    }
    // The original's tail (`JMP 0x004e2d4c`) is a shared Watcom epilogue, not a call -- an ordinary
    // return, not a tail call into another function.
}

} // namespace detail

void sort_site_candidates_by_dist() {
    const ai_state st = state();
    detail::sort_site_candidates_by_dist(st.read, st.own);
}


} // namespace mh::ai
