//
// ai/ai_scan_target_sort_cmp.h -- the scan-target-list qsort comparator (RI-AI batch E).
//
// llm_strat_ai_scan_target_sort_cmp @0x004ec792. Companion to ai_scan_visible.h's
// scan_target_list_sort, which hands this function's ADDRESS to the game's own qsort
// (`gc.qsort(base, count, 0x16, gc.scan_target_sort_cmp)`) as the comparison callback. That is
// also why static analysis shows it with zero callers -- it is reached only through the function
// pointer qsort is given, never by a direct call (a reference manager cannot see one). Once this site is
// shadow-armed, the callback address the trampoline captured still resolves here.
//
// A qsort comparator's signature is fixed by its caller (the CRT qsort): two raw `void *` element
// pointers in, an `int` in {-1, 0, 1} out. There is no channel through which it could take an
// ai_view/ai_store/ai_calls the way every other function in this cluster does -- it genuinely
// reads nothing but the two records it is handed, and the const view / store / calls triple is
// omitted from its signature for that reason, not by oversight.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an explicit pair of element pointers -- there is no "state()" to bind here, so
// unlike the rest of this cluster the detail:: function and the public wrapper have the identical
// signature; the wrapper exists only for shape consistency with every other TU.
namespace detail {

// llm_strat_ai_scan_target_sort_cmp @0x004ec792.
//
// Compares the SIGNED 16-bit `priority_score` field (offset +0x4 of the 0x16-byte
// mh_llm_strat_ai_scan_target_entry record) of `*a` against `*b`, even though the field is typed
// `uint16_t` in mh_structs.gen.h -- the assembly's `CMP BX, word ptr [..]` is followed by signed
// `JLE`/`JGE`, so the comparison is over the signed reinterpretation of those bits, not the
// unsigned field value. Returns -1 when a's key is strictly GREATER than b's, +1 when it is
// strictly LESS, 0 on a tie -- i.e. this sorts DESCENDING by priority_score. The 0 return on a tie
// is load-bearing: it hands equal-key ordering to the game's OWN qsort (0x004de8a6), so a
// reimplementation must call THAT qsort with THIS comparator (as scan_target_list_sort already
// does) rather than a host std::sort/qsort, or equal-priority entries permute differently.
int32_t scan_target_sort_cmp(void *a, void *b);

} // namespace detail

int32_t scan_target_sort_cmp(void *a, void *b);

} // namespace mh::ai
