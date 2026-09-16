//
// ai/ai_group_membership.cpp -- see ai_group_membership.h. Translated from the DISASSEMBLY:
//   tmp/decomp/llm_strat_ai_group_has_split_group_link_004d3af2.asm
//   tmp/decomp/llm_strat_ai_group_member_move_004d4af5.asm
//   tmp/decomp/llm_strat_ai_group_create_004d4d85.asm
//
#include "ai/ai_group_membership.h"


namespace mh::ai {
namespace detail {

int32_t group_has_split_group_link(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                   int32_t player_id, uint32_t target_group_id) {
    (void)own;
    (void)gc;
    const player_data &pd = v.players[player_id];

    // uVar1 (the loop index) starts at 5 -- AI_SEED_GROUP_COUNT, the five seed groups
    // llm_strat_spawn_ai_base creates for every AI player and which are never scanned here -- and the
    // loop runs while it is UNSIGNED less than ai_group_count (`CMP EBX,[..+0xe7e424] / JC`: carry set
    // means EBX < count). Preserve both the start index and the `<` direction exactly.
    for (uint32_t i = (uint32_t)AI_SEED_GROUP_COUNT; i < (uint32_t)pd.ai_group_count; ++i) {
        const unit_group &g = pd.ai_groups[i];
        // goal == 7 first (word compare @0x004d3b12), THEN link_target_group == target_group_id
        // (zero-extended word compare @0x004d3b1c-0x004d3b23) -- both must hold for a match.
        if (g.goal == 7 && (uint32_t)g.link_target_group == target_group_id) return 1;
    }
    return 0;
}

void group_member_move(const ai_view &v, const ai_store &own, const ai_calls &gc, uint32_t player,
                       int32_t src_group, int32_t dst_group, int32_t unit_id) {
    (void)v;
    (void)own;
    // Unlink from src_group first, then link into dst_group -- the original's own call order
    // (0x004d4b07 then 0x004d4b12), and the two are separate functions with separate write sets:
    // group_member_unlink is what DECREMENTS the source group's member_count (and the doubly-linked
    // list fixup), group_member_link is a distinct callee that does the destination-side work. Do not
    // fuse or reorder them.
    gc.group_member_unlink((int32_t)player, src_group, unit_id);
    gc.group_member_link(player, dst_group, (uint32_t)unit_id);
}

int32_t group_create(const ai_view &v, const ai_store &own, const ai_calls &gc, int32_t player_id) {
    (void)v;
    player_data &pd = own.players[player_id];

    // The cap check is an EARLY-OUT: on the full path (@0x004d4db7-0x004d4dbe) the original returns
    // -1 without touching ai_group_count, without touching next_group_serial, and WITHOUT calling
    // group_task_enqueue. Preserve the early-out exactly -- do not enqueue on this path.
    if (pd.ai_group_count == 0x20) return -1;

    const int32_t new_index = pd.ai_group_count; // the slot claimed = the count BEFORE the bump
    pd.ai_group_count       = new_index + 1;     // @0x004d4dc9-0x004d4dcc

    const int32_t serial = pd.next_group_serial; // the serial stamped = the value BEFORE the bump
    pd.next_group_serial = serial + 1;           // @0x004d4de1-0x004d4de4

    unit_group &g = pd.ai_groups[new_index];
    g.serial_id   = serial; // @0x004d4ded, the OLD (pre-increment) next_group_serial
    // Every other header field the original zeroes explicitly (@0x004d4df3-0x004d4e44), in the same
    // order as the assembly's own stores. current_param is the one dword-width zero among them; the
    // rest (including reserved_0x16, the 2-byte slot between goal and link_target_group) are
    // word-width.
    // link_target_group itself, and everything after it in the struct, is left untouched -- the
    // original does not zero it either.
    g.member_count        = 0; // @0x004d4dfd
    g.reinforce_pending   = 0; // @0x004d4e06
    g.current_param       = 0; // @0x004d4df3 (dword)
    g.active_member_count = 0; // @0x004d4e18
    g.head_unit           = 0; // @0x004d4e21
    g.tail_unit           = 0; // @0x004d4e2a
    g.task_queue_count    = 0; // @0x004d4e0f
    g.goal                = 0; // @0x004d4e33
    g.reserved_0x16       = 0; // its OWN independent word store, distinct from goal's
                               // (MOV word ptr [..+0xe7e43e],0x0 @0x004d4e3c, offset 0x16). The
                               // slot is unread image-wide -- see the field comment in
                               // mh_structs.gen.h; zeroing it is the whole obligation.

    // Unconditional on the success path (@0x004d4e45-0x004d4e5a). Task code 10 is an instant-complete
    // placeholder task, not one of the two named task codes in ai_state.h (RECRUIT_FROM_STORAGE=9,
    // DISBAND=0x14); all eight remaining task_enqueue arguments are literal zero at this call site.
    gc.group_task_enqueue(player_id, new_index, 10, 0, 0, 0, 0, 0, 0);

    return new_index;
}

} // namespace detail

int32_t group_has_split_group_link(int32_t player_id, uint32_t target_group_id) {
    const ai_state st = state();
    return detail::group_has_split_group_link(st.read, st.own, live_calls(), player_id,
                                              target_group_id);
}

void group_member_move(uint32_t player, int32_t src_group, int32_t dst_group, int32_t unit_id) {
    const ai_state st = state();
    detail::group_member_move(st.read, st.own, live_calls(), player, src_group, dst_group, unit_id);
}

int32_t group_create(int32_t player_id) {
    const ai_state st = state();
    return detail::group_create(st.read, st.own, live_calls(), player_id);
}

// ---- the differential-oracle arms ----------------------------------------------------------------
//
// group_has_split_group_link writes nothing at all, so its site needs no declared region for the
// restore to be exact. group_member_move's two callees write `units` (the intrusive link words) and
// player_data (head/tail/count on both the source and destination group); group_create writes only
// player_data (its own new slot's header) and its one callee, group_task_enqueue, is documented
// player_data-only elsewhere in this header -- so both sites are exact under a restore that declares
// player_data (+ units for group_member_move).

} // namespace mh::ai
