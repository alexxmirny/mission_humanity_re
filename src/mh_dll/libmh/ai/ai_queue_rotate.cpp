//
// ai/ai_queue_rotate.cpp -- see ai_queue_rotate.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_queue_rotate_newest_to_front_004e2cac.asm), not from Ghidra's C.
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h rather than transcribed:
//   0xe935ec = player_data + 0x2572c = ai_bldg_queue_count  (player stride 166140, built by
//              SHL 2 / ADD / SHL 7 / SUB / SHL 2 / SHL 6 / ADD at 0x004e2cc1-0x004e2cd3)
//   0xe935f0 = player_data + 0x25730 = ai_bldg_queue[0]     (entry stride 0x12, IMUL ..,0x12)
// so no byte offset and no literal VA appears below (Law 1).
//
#include "ai/ai_queue_rotate.h"


namespace mh::ai {
namespace detail {

void queue_rotate_newest_to_front(const ai_store &own, int32_t player) {
    player_data &pd = own.players[player];
    // UNSIGNED: CMP dword ptr [.. + 0xe935ec],0x1 / JBE 0x004e2d48 @0x004e2cd5-0x004e2cdc.
    if ((uint32_t)pd.ai_bldg_queue_count <= 1u) return;

    // MOV EBX,count / DEC EBX @0x004e2cde-0x004e2ce4, then the 0x12-byte save into the frame
    // (MOVSD.REP x4 + MOVSW @0x004e2cf6-0x004e2cf8). The struct assignment is the same 18 bytes;
    // mh_structs.gen.h static_asserts the size, so a Ghidra retype breaks the build rather than
    // silently copying a different amount.
    int32_t    i   = pd.ai_bldg_queue_count - 1;
    const auto tmp = pd.ai_bldg_queue[i];

    // SIGNED: TEST EBX,EBX / JG 0x004e2cfc @0x004e2d33-0x004e2d35. Walking DOWN, so the shift is
    // safe in place -- q[i] is written only after q[i] has already been read into q[i+1].
    for (; i > 0; --i) pd.ai_bldg_queue[i] = pd.ai_bldg_queue[i - 1];

    pd.ai_bldg_queue[0] = tmp; // the second MOVSD.REP x4 + MOVSW @0x004e2d44-0x004e2d46
}

} // namespace detail

void queue_rotate_newest_to_front(int32_t player) {
    const ai_state st = state();
    detail::queue_rotate_newest_to_front(st.own, player);
}

// ---- the differential-oracle arm ----------------------------------------------------------------
//
// Nothing to stub -- no outward call. Unlike the two predicates in ai_queue_type_query.cpp this one
// does write: the whole queue row lives in player_data, which the site declares, so the restore
// between the arms undoes it and a wrong rotate is reported rather than applied.
//
// WHAT A VACUOUS GREEN LOOKS LIKE HERE. The early-out (count <= 1) writes nothing, and on an idle
// or nearly idle base that is most calls. `rotated` counts the ones that reached the shift, so a run
// with rotated == 0 has compared nothing but "we both declined". Read it before the divergence count.

} // namespace mh::ai
