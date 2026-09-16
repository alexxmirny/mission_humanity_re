//
// ai/ai_group_member_list.cpp -- see ai_group_member_list.h. Translated from the DISASSEMBLY:
//   tmp/decomp_ai/llm_strat_ai_group_member_link_004d4a2e.asm
//   tmp/decomp_ai/llm_strat_ai_group_member_unlink_004d4919.asm
//
#include "ai/ai_group_member_list.h"

namespace mh::ai {
namespace detail {

void group_member_link(const ai_view &v, const ai_store &own, const ai_calls &gc, uint32_t player,
                       int32_t ai_group_index, uint32_t unit_id) {
    (void)v;  // no reads through the read-only view -- see header banner
    (void)gc; // no callees -- see header banner; must NOT call gc.group_member_link (bound to the
              // ORIGINAL) from inside its own reimplementation

    // 0x004d4a4e: UNCONDITIONAL, evaluated before the list-empty test below. Truncating 16-bit
    // store -- ai_group_index arrives as int32_t, the field is uint16_t.
    own.roster.ai_group_index(player, static_cast<int32_t>(unit_id)) =
        static_cast<uint16_t>(ai_group_index);

    player_data &pd = own.players[player];
    unit_group  &g  = pd.ai_groups[ai_group_index];

    // 0x004d4a71: `CMP word[..+0xe7e434],0` tests TAIL_UNIT (+0xc), not head_unit (+0xa) -- see the
    // header banner's CORRECTION note. tail_unit == 0 is the actual empty-list test read off the
    // opcodes.
    if (g.tail_unit == 0) {
        // 0x004d4aac / 0x004d4ab3: sole member -- head and tail both become this unit.
        g.head_unit = static_cast<uint16_t>(unit_id);
        g.tail_unit = static_cast<uint16_t>(unit_id);
        // 0x004d4aba / 0x004d4ac3: its own links both terminate the list.
        own.roster.ai_group_next(player, static_cast<int32_t>(unit_id)) = 0;
        own.roster.ai_group_prev(player, static_cast<int32_t>(unit_id)) = 0;
    } else {
        // 0x004d4a7b: the new unit becomes the new tail, so its own next terminates the list.
        own.roster.ai_group_next(player, static_cast<int32_t>(unit_id)) = 0;
        // 0x004d4a84 / 0x004d4a8b: its own prev is the OLD tail, read fresh before being
        // overwritten below.
        const uint16_t old_tail                                         = g.tail_unit;
        own.roster.ai_group_prev(player, static_cast<int32_t>(unit_id)) = old_tail;
        // 0x004d4a92-0x004d4a9b: the OLD tail's next becomes the new unit (append).
        own.roster.ai_group_next(player, static_cast<int32_t>(old_tail)) =
            static_cast<uint16_t>(unit_id);
        // 0x004d4aa3: the group's tail becomes the new unit.
        g.tail_unit = static_cast<uint16_t>(unit_id);
    }

    // 0x004d4ae8: UNCONDITIONAL, in the shared tail reached by either branch above.
    ++g.member_count;
}

void group_member_unlink(const ai_view &v, const ai_store &own, const ai_calls &gc, uint32_t player,
                         int32_t ai_group_index, uint32_t unit_id) {
    (void)v;  // no reads through the read-only view -- see header banner
    (void)gc; // no callees -- see header banner; must NOT call gc.group_member_unlink (bound to the
              // ORIGINAL) from inside its own reimplementation

    player_data &pd = own.players[player];
    unit_group  &g  = pd.ai_groups[ai_group_index];

    // 0x004d493a: `CMP word[..+0xdd8d1e],0` -- this unit's own ai_group_prev, read once and reused
    // (nothing writes it before the reuse below).
    const uint16_t prev = own.roster.ai_group_prev(player, static_cast<int32_t>(unit_id));
    if (prev == 0) {
        // 0x004d4980 / 0x004d4987: no previous member -- this unit WAS the group's head; the
        // group's head_unit becomes this unit's own ai_group_next (re-read fresh, unmodified so
        // far).
        g.head_unit = own.roster.ai_group_next(player, static_cast<int32_t>(unit_id));
    } else {
        // 0x004d4944-0x004d4958: prev.next = this.next -- ordinary middle-of-list splice on the
        // backward side.
        own.roster.ai_group_next(player, static_cast<int32_t>(prev)) =
            own.roster.ai_group_next(player, static_cast<int32_t>(unit_id));
    }

    // 0x004d499d: `CMP word[..+0xdd8d1c],0` -- this unit's own ai_group_next, re-derived fresh
    // (the prev-side branch above never writes this unit's own next/prev fields -- see the header
    // banner). Independent of the prev-side test; not an else-branch of it.
    const uint16_t next = own.roster.ai_group_next(player, static_cast<int32_t>(unit_id));
    if (next == 0) {
        // 0x004d49e1 / 0x004d49e8: no next member -- this unit WAS the group's tail; the group's
        // tail_unit becomes this unit's own ai_group_prev (re-read fresh).
        g.tail_unit = own.roster.ai_group_prev(player, static_cast<int32_t>(unit_id));
    } else {
        // 0x004d49a7-0x004d49bb: next.prev = this.prev -- ordinary middle-of-list splice on the
        // forward side.
        own.roster.ai_group_prev(player, static_cast<int32_t>(next)) =
            own.roster.ai_group_prev(player, static_cast<int32_t>(unit_id));
    }

    // 0x004d49fc: UNCONDITIONAL sentinel -- "in no group" -- regardless of which arms above fired.
    own.roster.ai_group_index(player, static_cast<int32_t>(unit_id)) = static_cast<uint16_t>(0xffff);
    // 0x004d4a22: UNCONDITIONAL decrement, NOT gated behind either branch above -- the ground truth
    // this batch was handed down explicitly warns a naive "decrement only inside a branch" oracle
    // would pass vacuously; this translation does not gate it.
    --g.member_count;
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void group_member_link(uint32_t player, int32_t ai_group_index, uint32_t unit_id) {
    const ai_state st = state();
    detail::group_member_link(st.read, st.own, live_calls(), player, ai_group_index, unit_id);
}

void group_member_unlink(uint32_t player, int32_t ai_group_index, uint32_t unit_id) {
    const ai_state st = state();
    detail::group_member_unlink(st.read, st.own, live_calls(), player, ai_group_index, unit_id);
}

} // namespace mh::ai
