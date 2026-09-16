//
// ai/ai_spiral_table_init.cpp -- see ai_spiral_table_init.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_spiral_table_init_004dc597.asm), not from Ghidra's C. The draft's `for`
// loops read faithfully against the listing (traced instruction-by-instruction, including the
// second loop's "increment the array index every iteration regardless of whether a ring boundary
// was committed" shape, which is easy to mis-simplify into a cleaner two-pointer merge that is NOT
// what the original does), so this is a straight transcription with the loop control kept literal
// rather than "cleaned up."
//
#include "ai/ai_spiral_table_init.h"


namespace mh::ai {
namespace detail {

// ---- LAB_004dc546: the spiral-offset comparator (LIB-CRT) -------------------------------------
//
// Transcribed 2026-09-08. Ghidra never made this a function -- it is a bare label one instruction
// before spiral_table_init's own entry -- so it had no decompilation to read; the bytes were
// disassembled directly. The opening `PUSH 0xc / CALL utils_assert_stack_capacity` is the inert CRT
// prologue (translator-brief rule 6) and is omitted.
//
//   0x004dc554  MOVSX EBX, byte [EAX+1]   ; a->dy, SIGN-extended -- the fields are int8, and reading
//   0x004dc558  IMUL  EBX, EBX            ;   them unsigned would put every negative offset in the
//   0x004dc55b  MOVSX EAX, byte [EAX]     ;   wrong ring
//   0x004dc55e  IMUL  EAX, EAX
//   0x004dc561  ADD   EBX, EAX            ; ra = dy*dy + dx*dx   (dy FIRST -- irrelevant to the sum,
//                                         ;   kept so the transcription reads against the listing)
//   ...same for b into EAX
//   0x004dc576  CMP EBX,EAX / JGE / JLE   ; ra < rb -> -1 ; ra == rb -> 0 ; ra > rb -> +1
int32_t spiral_offset_sort_cmp(void *a, void *b) {
    const auto   *pa = static_cast<const signed char *>(a);
    const auto   *pb = static_cast<const signed char *>(b);
    const int32_t ra = static_cast<int32_t>(pa[1]) * pa[1] + static_cast<int32_t>(pa[0]) * pa[0];
    const int32_t rb = static_cast<int32_t>(pb[1]) * pb[1] + static_cast<int32_t>(pb[0]) * pb[0];
    if (ra < rb) return -1; // 0x004dc57a
    if (ra > rb) return 1;  // 0x004dc586
    return 0;               // 0x004dc590
}

void spiral_table_init(const ai_store &own, const ai_calls &gc) {
    // ---- pass 1: generate every offset within the disc, dx outer / dy inner (0x004dc5a4-0x004dc5ee) ----
    //
    // Both loops run dx, dy over [-127, 127] inclusive (EBX/EDX start at 0xffffff81 == -127, loop
    // while <= 0x7f == 127 -- 255 values each, the full square). A cell is kept when
    // dx*dx + dy*dy <= 0x3f01 (`CMP EAX,0x3f01 ; JA skip`), i.e. strictly less than 0x3f02. No cap is
    // ever compared against the write index -- the original relies on the array being sized for the
    // worst case (it is: the declared shadow region is 131072 bytes / 2 bytes-per-entry = 65536
    // entries, comfortably above the ~50.6k cells a radius-127 disc actually has), so this
    // reimplementation does not invent a bounds check either.
    *own.ai_tile_spiral_cell_count = 0;
    for (int32_t dx = -127; dx <= 127; ++dx) {
        for (int32_t dy = -127; dy <= 127; ++dy) {
            if ((uint32_t)(dx * dx + dy * dy) < 0x3f02u) {
                const int32_t count            = *own.ai_tile_spiral_cell_count;
                own.spiral_offsets[count].dx   = (int8_t)dx;
                own.spiral_offsets[count].dy   = (int8_t)dy;
                *own.ai_tile_spiral_cell_count = count + 1;
            }
        }
    }

    // ---- sort the whole run by squared radius, ascending (0x004dc5f4-0x004dc60e) ----
    //
    // THE GAME's qsort with THE GAME's own comparator (address 0x004dc546, immediately before this
    // function in the image) -- gc.spiral_offset_sort_cmp, a declared need (see the translator's
    // report): that comparator is not exposed anywhere in ai_calls yet, unlike its sibling
    // gc.scan_target_sort_cmp (ai_state.h's comment on ai_calls::qsort explains why a host sort would
    // be a real divergence -- equal-radius ties keep whatever permutation Watcom's qsort produces).
    gc.qsort(own.spiral_offsets, (uint32_t)*own.ai_tile_spiral_cell_count, 2u,
             gc.spiral_offset_sort_cmp);

    // ---- pass 2: derive the per-ring cumulative cell count (0x004dc60e-0x004dc656) ----
    //
    // TRANSCRIBED LITERALLY, not simplified into a cleaner merge. `array_idx` (EDX in the assembly)
    // advances by exactly one EVERY outer-loop iteration, whether or not a ring boundary was
    // committed that iteration; `ring` (EBX) advances only on a commit. A commit fires when the
    // current entry's squared distance exceeds ring*ring (`ring*ring < dist_sq`, sorted-ascending so
    // this is the first index past the ring), OR unconditionally once array_idx has reached
    // cell_count (the sentinel pass that lets the loop terminate via the index-past-cell_count exit
    // rather than by exhausting all 128 rings -- in practice cell_count is tens of thousands, so the
    // loop always exits via `ring == 128` first and the sentinel branch is dead in normal data, but it
    // is in the original and is reproduced rather than pruned).
    {
        const int32_t cell_count = *own.ai_tile_spiral_cell_count;
        int32_t       ring       = 0;
        int32_t       array_idx  = 0;
        while (array_idx <= cell_count && ring < 128) {
            const spiral_offset &e       = own.spiral_offsets[array_idx];
            const int32_t        dist_sq = (int32_t)e.dx * (int32_t)e.dx + (int32_t)e.dy * (int32_t)e.dy;
            if (ring * ring < dist_sq || array_idx == cell_count) {
                own.spiral_ring_cell_counts[ring] = (uint32_t)array_idx;
                ++ring;
            }
            ++array_idx;
        }
    }
    // The original's tail `POP EDX / POP ECX / POP EBX / RET` is the ordinary epilogue -- it means
    // `return`.
}

} // namespace detail

void spiral_table_init() {
    const ai_state st = state();
    detail::spiral_table_init(st.own, live_calls());
}


} // namespace mh::ai
