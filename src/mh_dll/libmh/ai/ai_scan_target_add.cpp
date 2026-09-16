//
// ai/ai_scan_target_add.cpp -- see ai_scan_target_add.h. Translated from the DISASSEMBLY
// (tmp/decomp_a3/llm_strat_ai_scan_target_list_add_004ec2d0.asm), not from Ghidra's C
// (tmp/decomp_a3/llm_strat_ai_scan_target_list_add_004ec2d0.c), which mis-transcribes the function
// in three ways the .c's own local names hide:
//   - the dedup compare reads `extraout_ECX` for the stored-vs-argument target_ref compare (an
//     artifact of Ghidra assuming the __watcall callee clobbers ECX); the .asm shows ECX is simply
//     the argument, carried in a register for the whole function;
//   - `class_flags` and `reserved_0x08` are written as two separate C statements, but the .asm executes
//     ONE dword store (`MOV dword ptr [EAX+6],0`) that covers both fields in a single instruction;
// - the committed parameter names were wrong previously: EDX is `target_ref` (the packed
//     owner|kind ref), not `target_id`/`target_owner`, and EBX is `target_index`. The exported .asm
//     header now carries the corrected names; any stale comment elsewhere saying target_id/
//     target_owner refers to the same two arguments under the old names.
//
#include "ai/ai_scan_target_add.h"


namespace mh::ai {
namespace detail {

void scan_target_list_add(const ai_view &v, const ai_store &own, const ai_calls &gc, uint32_t player,
                          uint32_t target_ref, int32_t target_index) {
    // The alive test happens FIRST and rejects before anything else -- 0x004ec2ee JZ, straight to
    // the shared epilogue. No capacity check exists anywhere in this function.
    if (!gc.target_ref_is_alive(target_ref, target_index)) return;

    const uint32_t count = (uint32_t)*v.scan_target_count;

    // Linear dedupe over the whole live count (0x004ec2f4..0x004ec314, unsigned CMP/JC against
    // *scan_target_count). Both stored fields are 16-bit and the original zero-extends each
    // (MOVZX EBX/EAX) before comparing it against the full-width argument -- a target_ref or
    // target_index above 0xffff therefore dedupes only against a stored entry whose low 16 bits
    // happen to match, exactly like the original, not against a 16-bit-truncated copy of the
    // argument.
    for (uint32_t i = 0; i < count; ++i) {
        const scan_target_entry &e = v.scan_targets[i];
        if ((uint32_t)e.target_ref == target_ref &&
            (uint32_t)e.target_index == (uint32_t)target_index) {
            return;
        }
    }

    // Fresh entry at the current (not-yet-counted) slot, written BEFORE the classifier runs --
    // group_classify_target_object reads this same record through the pointer handed to it below, so
    // every field it does not itself set must already hold its initial value.
    scan_target_entry &entry = own.scan_targets[count];
    entry.target_ref         = (uint16_t)target_ref;
    entry.target_index       = (uint16_t)target_index;
    entry.priority_score     = 0;
    // The original zeroes class_flags (+6) and reserved_0x08 (+8) with ONE dword store
    // (0x004ec334); written as two word stores here to the identical effect, since both fields are
    // uint16_t and both end up zero either way.
    entry.class_flags             = 0;
    entry.reserved_0x08           = 0;
    entry.counter_target_ai_group = 0xffff;

    gc.group_classify_target_object(player, &entry);

    // The count increments only AFTER the classifier call returns (0x004ec355, past the CALL) --
    // order matters because the classifier is handed the not-yet-counted slot. Written as an
    // INCREMENT rather than `count + 1` because that is the instruction (`INC dword [count]`): the
    // two differ if the classifier ever appended an entry of its own, which it does not today but
    // which the assembly does not assume either.
    ++*own.scan_target_count;
}

} // namespace detail

void scan_target_list_add(uint32_t player, uint32_t target_ref, int32_t target_index) {
    const ai_state st = state();
    detail::scan_target_list_add(st.read, st.own, live_calls(), player, target_ref, target_index);
}


} // namespace mh::ai
