//
// ai/ai_unit_move_bump.cpp -- see ai_unit_move_bump.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_unit_order_move_with_bump_0046ae0a.asm), not from Ghidra's C: the
// draft's bare `local_14`/`local_18`/`local_20` locals hide the one thing worth reading the raw
// bytes for -- order_arg0/order_arg1 are STORED in this function's own frame at entry and, for the
// placement-preview tail, RELOADED into the OPPOSITE physical registers from the ones they arrived
// in (EDX/EBX at 0x0046aea9/0x0046aeac, having arrived in EBX/ECX). Their SEMANTIC role does not
// move with the swap: order_arg0 is still the value passed to order_scratch_set_field(0, ...) and
// still lands in the `center_x` slot below; order_arg1 is still field 1 and still `center_y`. A
// translation that assumed "EBX still means order_arg0 because that's where it came in" would swap
// x and y in the preview call.
//
#include "ai/ai_unit_move_bump.h"


namespace mh::ai {
namespace detail {

move_bump_report unit_order_move_with_bump(const ai_view &v, const ai_store &own,
                                           const ai_calls &gc, uint32_t player, int32_t unit_idx,
                                           uint32_t order_arg0, uint32_t order_arg1) {
    move_bump_report rep{};
    (void)own; // this function writes nothing directly; every write happens inside its callees

    // The order scratch always carries (order_arg0, order_arg1) in fields 0/1, unconditionally --
    // this runs regardless of the PlayerSide gate below.
    gc.order_scratch_reset();
    gc.order_scratch_set_field(0, (int32_t)order_arg0);
    gc.order_scratch_set_field(1, (int32_t)order_arg1);

    // THE ROSTER ROW IS INDEXED BY THE LOW 16 BITS OF `player`, not by all 32. All three of the
    // original's roster-address computations start `MOVZX EDX, word ptr [EBP-0xc]` / `MOVZX EAX,
    // word ptr [EBP-0xc]` (0x0046ae47, 0x0046aeaf, 0x0046aece), so a player index above 0xffff
    // wraps rather than running off the array. Unreachable with the game's 0..7 player ids, and
    // matched anyway because matching costs nothing and leaves no standing question. (Raised as a
    // divergence by the reimpl-verify width lens, 2026-08-05.)
    const uint32_t roster_player = (uint16_t)player;
    // unit_proto_id is read ONCE and reused, where the original re-reads it at 0x0046ae5a,
    // 0x0046aec2 and 0x0046aee1. Equivalent, and checked rather than assumed: the only callee that
    // runs between those reads and writes `units` at all is llm_strat_unit_notify_status, whose
    // whole write set is unit +0xe3 and +0xe8 (0x004dae71-0x004daeb8) -- neither is unit_proto_id at
    // +0x2. llm_strat_order_enqueue writes only the order queue and its count.
    const uint16_t  unit_proto_id = unit_of(v, roster_player, unit_idx).unit_proto_id;
    const cfg_unit &proto         = v.cfg_units[unit_proto_id];

    // op_code is the LITERAL 0x18 (0x0046ae72: MOV EBX,0x18) -- this function's own order, NOT
    // proto.move_op_code. `arg` is the unit's cfg move_op_arg, zero-extended from the byte field
    // (0x0046ae67-0x0046ae6f). This call site is the one ai_state.h's field comment on move_op_arg
    // cites as proof of the field's meaning -- do not swap in move_op_code here.
    gc.order_enqueue((uint16_t)unit_idx, (uint16_t)player | 0x80u, 0x18,
                     (uint16_t)proto.move_op_arg);
    gc.unit_notify_status(player & 0xffffu, unit_idx, 0);

    // NOT a "the human did this" test -- under --soak (all-AI), PlayerSide names an AI player and
    // this tail runs for it too (ai_state.h's field comment on player_side). The shadow arm counts
    // this branch (a `local` counter) so a run can say whether it was reached, rather than reading
    // a clean pass as evidence it was not.
    if ((uint16_t)player == *v.player_side) {
        rep.local_tail = true;
        // order_arg0/order_arg1 reloaded here from THIS function's own frame, into the OPPOSITE
        // registers from the ones they arrived in (see the file header comment) -- their semantic
        // role (arg0 -> center_x, arg1 -> center_y) is unchanged from the order_scratch_set_field
        // calls above.
        uint32_t out_col = 0; // committed llm_strat_bldg_calc_placement_corner_from_center out-params
        uint32_t out_row = 0;
        gc.bldg_calc_placement_corner_from_center(unit_proto_id, (int32_t)order_arg0,
                                                  (int32_t)order_arg1, &out_col,
                                                  &out_row);

        const int32_t blocked =
            gc.bldg_placement_check_and_preview(out_col, out_row, proto.equivalent);
        if (blocked != 0) {
            gc.snd_play(0xb3, 100);
            rep.bump_played = true;
        }
    }
    // The original's tail `LEA ESP,[EBP-8] / POP EDI / POP ESI / POP EBP / RET` is the function's
    // own ordinary epilogue, not a shared one -- a plain `return`.
    return rep;
}

} // namespace detail

void unit_order_move_with_bump(uint32_t player, int32_t unit_idx, uint32_t order_arg0,
                               uint32_t order_arg1) {
    const ai_state st = state();
    (void)detail::unit_order_move_with_bump(st.read, st.own, live_calls(), player, unit_idx,
                                            order_arg0, order_arg1);
}


} // namespace mh::ai
