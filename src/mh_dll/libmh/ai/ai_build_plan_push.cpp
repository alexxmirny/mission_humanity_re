//
// ai/ai_build_plan_push.cpp -- see ai_build_plan_push.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_build_plan_push_004dc781.asm), not from Ghidra's C.
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h rather than transcribed: the player
// stride 0x288fc (166140) is built by the same SHL/ADD/SUB ladder as ai_queue_rotate.cpp's
// (SHL 2 / ADD / SHL 7 / SUB / SHL 2 / SHL 6 / ADD @0x004dc793-0x004dc7a9), landing on
// player_data.ai_build_plan_len_and_flag (+0x104c8, absolute 0xe7e388) and ai_build_plan[0]
// (+0x104cc, absolute 0xe7e38c) -- so no byte offset and no literal VA appears below (Law 1).
//
#include "ai/ai_build_plan_push.h"


namespace mh::ai {
namespace detail {

void build_plan_push(const ai_store &own, int32_t player, int32_t ai_build_id) {
    // CMP EDX,-0x1 / JZ LAB_004dc7bc @0x004dc78e-0x004dc791 -- the sentinel skips straight to the
    // epilogue, touching nothing.
    if (ai_build_id == -1) return;

    player_data &pd = own.players[player];

    // MOV EDX,[.. + 0xe7e388] @0x004dc7a9 loads the WHOLE dword (count in the low 31 bits, an
    // unrelated ring/spiral-scan flag in bit 0x80000000 -- see the field comment on
    // ai_build_plan_len_and_flag), and the very next instruction uses it as an array index
    // multiplied by 4 (`MOV [EAX + EDX*4 + 0xe7e38c],EBX` @0x004dc7af). The original never masks the
    // flag bit off before that multiply -- it doesn't need to, because a *4 on a 32-bit register
    // shifts bit 31 out of the low 32 bits entirely, so the raw dword and the masked count produce
    // an IDENTICAL address. This translation masks explicitly (matching the idiom
    // llm_strat_ai_plan_construction's own consumer-side comparison already uses,
    // `len_and_flag & 0x7fffffff`) rather than leaning on that overflow, which is not something
    // C++ array indexing is guaranteed to reproduce the way raw x86 address arithmetic does.
    const uint32_t len = pd.ai_build_plan_len_and_flag & 0x7fffffffu;

    // No cap check against the array's 32-entry extent -- matches the original, which trusts the
    // caller (<=6 entries seen in practice per the field comment). Do not add a bound the
    // disassembly does not have.
    pd.ai_build_plan[len] = ai_build_id;

    // INC dword ptr [.. + 0xe7e388] @0x004dc7b6 increments the WHOLE dword, count and flag bit
    // together, not just the low 31 bits -- reproduced as a plain +1 on the raw field rather than a
    // masked increment, since that is bit-for-bit what INC does.
    pd.ai_build_plan_len_and_flag = pd.ai_build_plan_len_and_flag + 1;
}

} // namespace detail

void build_plan_push(int32_t player, int32_t ai_build_id) {
    const ai_state st = state();
    detail::build_plan_push(st.own, player, ai_build_id);
}

// ---- the differential-oracle arm ----------------------------------------------------------------
//
// Nothing to stub -- no outward call (the opening `CALL utils_assert_stack_capacity` is the inert
// prologue, omitted per the translator brief). The write (one ai_build_plan[] slot plus the
// len_and_flag dword) lives entirely inside player_data, which is the site's one declared region, so
// the restore between the arms undoes it and a wrong push is reported rather than applied.
//
// WHAT A VACUOUS GREEN LOOKS LIKE HERE. The sentinel early-out (ai_build_id == -1) writes nothing;
// `pushed` counts the calls that actually appended, so a run with pushed == 0 has compared nothing
// but "we both declined" on every call.

} // namespace mh::ai
