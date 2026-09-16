//
// ai/ai_scan_target_sort_cmp.cpp -- see ai_scan_target_sort_cmp.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_scan_target_sort_cmp_004ec792.asm), not the paired .c: the draft's
// `*(short *)((int)x + 4)` casts already read the field as signed, which happens to be the right
// call, so nothing there needed correcting -- the assembly is transcribed here only to replace the
// raw `+4` offset with the named `priority_score` field from mh_structs.gen.h, per the translator
// brief's "never a byte offset" rule.
//
#include "ai/ai_scan_target_sort_cmp.h"


namespace mh::ai {
namespace detail {

int32_t scan_target_sort_cmp(void *a, void *b) {
    // The opening `PUSH 0x8 / CALL utils_assert_stack_capacity` is the inert CRT prologue
    // (translator brief rule 6, touches zero tracked state) and is omitted.
    const auto *ea = static_cast<const scan_target_entry *>(a);
    const auto *eb = static_cast<const scan_target_entry *>(b);
    // priority_score is stored `uint16_t` (mh_structs.gen.h) but compared SIGNED in the original
    // (`CMP BX, word ptr [..]` followed by signed `JLE`/`JGE`) -- (int16_t) reinterprets the same
    // 16 bits the assembly compares, it does not narrow a wider value.
    const int16_t key_a = (int16_t)ea->priority_score;
    const int16_t key_b = (int16_t)eb->priority_score;
    // b's key strictly greater than a's -> a sorts first (descending by priority_score).
    if (key_b < key_a) return -1;
    if (key_a < key_b) return 1;
    return 0; // tie: equal-key order is whatever the game's own qsort (0x004de8a6) produces.
}

} // namespace detail

int32_t scan_target_sort_cmp(void *a, void *b) { return detail::scan_target_sort_cmp(a, b); }


} // namespace mh::ai
