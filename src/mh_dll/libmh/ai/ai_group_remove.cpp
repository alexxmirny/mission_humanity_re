//
// ai/ai_group_remove.cpp -- see ai_group_remove.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_group_remove_004d4f16.asm), not the Ghidra `.c` draft.
//
// PROOF: OFFLINE ONLY. No shadow wiring in this file and none planned -- see the header banner's
// "PROOF PATH" section for why arming a shadow site here would double-fire the one callee's
// order-enqueue closure while the region-diff comparison stayed clean.
//
#include "ai/ai_group_remove.h"

namespace mh::ai {
namespace detail {

void group_remove(const ai_view &v, const ai_store &own, const ai_calls &gc, int32_t player,
                  uint32_t group_index) {
    (void)v;
    player_data &pd = own.players[player];

    // 0x004d4f46-0x004d4f50: last_index = ai_group_count - 1 (read BEFORE the decrement in step
    // 3 below); if it already equals group_index -- the removed group IS the last one -- the
    // whole copy+restamp step is skipped and control falls straight through to the decrement.
    const uint32_t last_index = static_cast<uint32_t>(pd.ai_group_count) - 1u;
    if (last_index != group_index) {
        // 0x004d4f66-0x004d4f7b: whole-record copy of the LAST group over the freed slot.
        // sizeof(unit_group) == 0xa66, matching `MOV ECX,0x299 / REP MOVSD / MOVSW` exactly
        // (0x299*4 + 2 = 0xa66) -- a plain struct assignment, not a memcpy of a literal size.
        pd.ai_groups[group_index] = pd.ai_groups[last_index];

        // 0x004d4f7d-0x004d4fa6: walk the MOVED group's member list from its (post-copy)
        // head_unit, terminated at ai_group_next == 0, restamping every member's ai_group_index
        // to the slot the group was just moved into (group_index -- also the freed slot, since
        // that IS where the copy landed above). Reached only through the roster window: there is
        // no `unit *` in ai_store.
        uint16_t member = pd.ai_groups[group_index].head_unit;
        while (member != 0) {
            own.roster.ai_group_index(static_cast<uint32_t>(player), member) =
                static_cast<uint16_t>(group_index);                                   // 0x004d4f98
            member = own.roster.ai_group_next(static_cast<uint32_t>(player), member); // 0x004d4f9f
        }
    }

    // 0x004d4fc5: unconditional, reached from both the copy path and the "already last" path
    // (which jumps straight here via 0x004d4faa).
    --pd.ai_group_count;

    // 0x004d4fce-0x004d502e: sweep 1, DRAIN. `i` is compared against `pd.ai_group_count` fresh on
    // every iteration (see header banner) rather than a cached bound.
    for (uint32_t i = static_cast<uint32_t>(AI_SEED_GROUP_COUNT); i < static_cast<uint32_t>(pd.ai_group_count);
         ++i) {
        unit_group &g = pd.ai_groups[i];
        // 0x004d4fd6 / 0x004d4fe0-0x004d4fea: goal == 7 AND link_target_group == the ORIGINALLY
        // REMOVED index (group_index, held constant for the whole function -- NOT the mutated
        // sweep loop variable, even though the original starts both stack copies at the same
        // value; see the header banner's collapse note).
        if (g.goal == 7 && static_cast<uint32_t>(g.link_target_group) == group_index) {
            // 0x004d4fec-0x004d5023: NO iteration bound in the original -- see the header
            // banner's PRESERVE-BUG note. None is added here.
            while (g.member_count != 0) {
                gc.route_unit_to_home_storage(static_cast<uint32_t>(player), g.head_unit); // 0x004d501e
            }
            g.reinforce_pending = 0; // 0x004d5025
        }
    }

    // 0x004d505f-0x004d508a: sweep 2, RELINK. Same index range and goal gate; compares against
    // the CURRENT (post-decrement) ai_group_count -- the index the moved group just vacated --
    // and retargets to group_index, the slot that group now occupies.
    for (uint32_t i = static_cast<uint32_t>(AI_SEED_GROUP_COUNT); i < static_cast<uint32_t>(pd.ai_group_count);
         ++i) {
        unit_group &g = pd.ai_groups[i];
        if (g.goal == 7 &&
            static_cast<uint32_t>(g.link_target_group) == static_cast<uint32_t>(pd.ai_group_count)) {
            g.link_target_group = static_cast<uint16_t>(group_index); // 0x004d5083
        }
    }

    // 0x004d50ae: JMP into the shared Watcom epilogue -- a plain return, not a call.
}

} // namespace detail

void group_remove(int32_t player, uint32_t group_index) {
    const ai_state st = state();
    detail::group_remove(st.read, st.own, live_calls(), player, group_index);
}

} // namespace mh::ai
