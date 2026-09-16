#include "orders/issue/issue_order_player.h"

namespace mh::orders::issue {

// ---- llm_strat_order_set_player_relation @0x0046f8c6 ----------------------------------------------
// Unguarded. Order 0xf4/0xf4. It DOES call scratch_reset first (`CALL 0x00466812` @0x0046f8e5,
// before either scratch_set_field), so the other eleven slots go out zeroed rather than carrying
// whatever the previous wrapper left.
//
// CORRECTION, 2026-08-28: the first draft of this body omitted that reset and carried a comment
// asserting the original had none. It was wrong against the listing at the address above, and it
// was caught by the translation lint's callee diff -- `llm_strat_order_scratch_reset` present
// in the .asm, absent from the C++. The golden's `scratch_reset_before` flag for this site would
// have caught it a second time in `issuetest`; the point of recording it here is that the CLAIM in
// a header comment is not evidence, and this one read as confident. Owner is the plain 16-bit MOVZX of
// `player` (`MOVZX EDX,word ptr [EBP-0x18]` @0x0046f90f) -- no kind nibble OR'd in, and unit_index
// is a literal 0 (`XOR EAX,EAX` @0x0046f913): this order targets a player-to-player relation, not a
// building or unit. Scratch: field 2 = opponent_idx (the full int32 EDX value), field 3 =
// relation_value zero-extended from its byte storage (`MOVZX EDX,byte ptr [EBP-0x10]` @0x0046f8f7).
void detail::order_set_player_relation(const issue_view &, const order_sink &s, const issue_calls &,
                                       uint32_t player, int32_t opponent_idx,
                                       uint8_t relation_value) {
    s.scratch_reset();
    s.scratch_set_field(2, opponent_idx);
    s.scratch_set_field(3, (int32_t)relation_value);
    s.dispatch(0, player & 0xffffu, 0xf4, 0xf4);
}

// ---- llm_strat_order_set_player_control_mode @0x0046f97e ------------------------------------------
// scratch_reset() FIRST, then field 2 = target_player, field 3 = set_human zero-extended from its
// byte storage -- both writes happen BEFORE the branch below, unconditionally. Order 0xf5/0xf5, same
// owner/unit_index shape as order_set_player_relation above (plain 16-bit MOVZX of `player`, literal
// 0 unit_index).
//
// THE BRANCH is the hazard the unit spec calls out: it both READS and WRITES `v.player_control_mask`
// (its only accessor in this domain) and formats an (otherwise-unread) debug line via
// `c.sprintf_ii`, once per arm, using a DIFFERENT format string per arm but the SAME two format
// arguments in both.
//
//   set_human == 0 (`CMP byte [EBP-0x10],0x0` / `JNZ` @0x0046f9bd-c1):
//     *mask &= ~(1 << (shift))                          -- NOT+AND @0x0046f9c3-cc
//     sprintf_ii(text_tmp, fmt_control_mode_clear, PlayerSide, target_player)   @0x0046f9d2-ef
//   else (falls through to LAB_0046f9f4):
//     *mask |=  (1 << (shift))                          -- OR @0x0046f9f4-fb
//     sprintf_ii(text_tmp, fmt_control_mode_set,   PlayerSide, target_player)   @0x0046fa01-1e
//
// `shift` is `(byte)target_player` (`MOV CL,byte [EBP-0x14]` reads the LOW BYTE of the full
// `target_player` dword, both arms), and the x86 SHL instruction then masks that count to 5 bits in
// hardware regardless of operand width -- reproduced explicitly as `& 0x1f` since C++ shift-by->=32
// is undefined and the hardware's masking is the actual spec here, not an incidental detail (the
// byte is already <=0xff, so the extra & 0x1f is the ONLY thing that matters for values 0x20-0xff).
//
// The sprintf ARGUMENT ORDER is fixed by the push sequence, right-to-left per __cdecl: dst, format,
// then PlayerSide (`MOVZX EAX,word [PlayerSide]` -- zero-extended, matching the view's uint16_t),
// then target_player (the FULL int32 value, not the byte used for the shift). Its result is a dead
// store -- nothing in this domain reads `text_tmp` back -- and is transcribed anyway per the unit
// spec, because it is what the original does on every call.
void detail::order_set_player_control_mode(const issue_view &v, const order_sink &s,
                                           const issue_calls &c, uint32_t player,
                                           int32_t target_player, uint8_t set_human) {
    s.scratch_reset();
    s.scratch_set_field(2, target_player);
    s.scratch_set_field(3, (int32_t)set_human);

    const uint32_t bit = 1u << ((uint8_t)target_player & 0x1fu);
    if (set_human == 0) {
        *v.player_control_mask = (uint8_t)(*v.player_control_mask & ~bit);
        c.sprintf_ii(v.text_tmp, v.fmt_control_mode_clear, (int32_t)*v.player_side, target_player);
    } else {
        *v.player_control_mask = (uint8_t)(*v.player_control_mask | bit);
        c.sprintf_ii(v.text_tmp, v.fmt_control_mode_set, (int32_t)*v.player_side, target_player);
    }

    s.dispatch(0, player & 0xffffu, 0xf5, 0xf5);
}

// ---- the public forms -----------------------------------------------------------------------------

void order_set_player_relation(uint32_t player, int32_t opponent_idx, uint8_t relation_value) {
    detail::order_set_player_relation(live_view(), live_sink(), live_calls(), player, opponent_idx,
                                      relation_value);
}
void order_set_player_control_mode(uint32_t player, int32_t target_player, uint8_t set_human) {
    detail::order_set_player_control_mode(live_view(), live_sink(), live_calls(), player,
                                          target_player, set_human);
}

} // namespace mh::orders::issue
