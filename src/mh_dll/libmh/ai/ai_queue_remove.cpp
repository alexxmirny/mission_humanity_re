//
// ai/ai_queue_remove.cpp -- see ai_queue_remove.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_queue_remove_at_004e5da2.asm), not from Ghidra's C: the decompile
// renders the trailing MOVSW half of the entry copy as a store into
// `ai_bldg_queue[i].building_index + 2`, which is just Ghidra mis-attributing the last 2 of the
// 18-byte MOVSD.REP-x4 + MOVSW pair to a field inside the struct it happens to land on -- the same
// artifact ai_queue_rotate.cpp's header comment already documents for the sibling function. It is
// one 0x12-byte struct copy, translated as one struct assignment.
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h rather than transcribed:
//   0xe935ec = player_data + 0x2572c = ai_bldg_queue_count  (player stride 166140, built by the
//              same SHL 2 / ADD / SHL 7 / SUB / SHL 2 / SHL 6 / ADD sequence as the sibling
//              function, at 0x004e5dd8-0x004e5dec)
//   0xe935f0 = player_data + 0x25730 = ai_bldg_queue[0]     (entry stride 0x12, IMUL ..,0x12)
// so no byte offset and no literal VA appears below (Law 1).
//
#include "ai/ai_queue_remove.h"


namespace mh::ai {
namespace detail {

void queue_remove_at(const ai_store &own, int32_t player, uint32_t slot_index) {
    player_data &pd = own.players[player];

    // The ONE comparison in the whole function: CMP ECX,EDX / JC LAB_004e5db4 @0x004e5df5-0x004e5df7,
    // where EDX is `count - 1` -- freshly reloaded from memory and decremented every pass
    // (0x004e5dee/0x004e5df4), not hoisted. Unsigned throughout (JC), matching the original's
    // total absence of a guard for a corrupt count or an out-of-range slot_index.
    uint32_t i = slot_index;
    while (i < (uint32_t)(pd.ai_bldg_queue_count - 1)) {
        // MOVSD.REP EDI,ESI (x4 dwords) + MOVSW ES:EDI,ESI @0x004e5dd2-0x004e5dd4 -- the full
        // 0x12-byte entry, one struct assignment; mh_structs.gen.h static_asserts the size, so a
        // Ghidra retype breaks the build rather than silently copying a different amount.
        pd.ai_bldg_queue[i] = pd.ai_bldg_queue[i + 1];
        ++i;
    }
    --pd.ai_bldg_queue_count; // DEC dword ptr [.. + 0xe935ec] @0x004e5df9
}

} // namespace detail

void queue_remove_at(int32_t player, uint32_t slot_index) {
    const ai_state st = state();
    detail::queue_remove_at(st.own, player, slot_index);
}

// ---- the differential-oracle arm ----------------------------------------------------------------
//
// Nothing to stub -- no outward call. The whole queue row lives in player_data, which the site
// declares, so the restore between the arms undoes a write and a wrong remove is reported rather
// than applied.
//
// WHAT A VACUOUS GREEN LOOKS LIKE HERE. When slot_index == count - 1 (removing the last live
// entry) the loop body never runs and only the count decrements. `shifted` counts the calls that
// actually moved at least one entry, so a run with shifted == 0 has compared nothing but "we both
// decremented".

} // namespace mh::ai
