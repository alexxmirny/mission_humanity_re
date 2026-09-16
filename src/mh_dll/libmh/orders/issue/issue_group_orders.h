//
// orders/issue/issue_group_orders.h -- the control-group move/ack-voice wrappers (RI-ORDERS / O4-0,
// O4A-C sweep, batch B unit `group_orders`). Three functions, NONE of which has a golden site: all
// three either emit no order of their own (ack_voice) or emit only by calling SIBLING wrappers in
// `issue_unit_move.h` (the two `group_issue_move_order_*`), so `order_issue_golden.gen.h` has no case
// for any row in this file -- these header comments are the spec the conductor's hand-authored oracle
// cases get written against. See tmp/decomp_orders_issue/_UNIT_group_orders.md.
//
// THIS UNIT DECLARES NO SHADOW SITE. All three rows are UI-rooted, debug-rooted, or have no
// order_matrix entry at all (per the unit spec), so the domain's soak/sp rig vehicles never reach
// them -- an arm would read ZERO CALLS. Proven OFFLINE against the golden's hand-authored cases only.
//
// Every `detail::` form takes (view, sink, calls) as its first three parameters, always, even where a
// body uses only one or two of them (issue_state.h's rule, fixed by the O4A-C sweep).
//
#pragma once
#include "orders/issue/issue_state.h"
#include "orders/issue/issue_unit_move.h" // detail::unit_order_move / _default / _confirmed_with_bump
                                          // -- OURS, called directly as sibling detail:: calls, never
                                          // through issue_calls and never through mh::call::.

namespace mh::orders::issue {

namespace detail {

// llm_strat_group_order_ack_voice @0x004259b2 (188 B). void(void) in the original -- no arguments,
// takes only the three state parameters here. NO golden site: it emits no order, only a sound.
//
// Rate-limited, race-specific order-acknowledgement voice line (one of 3 per race bucket, over
// v.ack_voice_snd_id_by_race). EMIT GATE, from the x87 compare at 0x004259ca-0x004259df (FLD
// last_play_time / FADD cooldown_sec / FCOMP game_clock / FNSTSW / SAHF / JNC 0x00425a3c): JNC is
// taken -- i.e. the SUPPRESSED arm runs -- when (last_play_time + cooldown_sec) is ORDERED and >=
// game_clock. Written below as the literal mirror of that condition, `sum >= game_clock`: on an
// unordered (NaN) input x87 does NOT take the JNC (falls through to the emit arm), and plain C `>=`
// with a NaN operand is also false (so the `if` is also not taken) -- the two agree on NaN by
// construction of this particular direction, unlike issue_bldg_orders.cpp's repair_cycle_start, where
// the mirror direction would NOT have agreed. See the unit spec's hazard note.
//
// SUPPRESSED arm: bumps v.ack_voice_suppressed_count; the CMP/JLE at 0x00425a42/0x00425a49 fires the
// generic sound 0x44 only once the count exceeds 5, i.e. on the 6th consecutive refusal (off-by-one:
// `5 < count`, not `<=`), then resets the counter to 0.
// EMIT arm: race_bucket = (v.player_race == 2) ? 3 : 0 (CMP/SETcc-shape at 0x004259e8-0x004259f8);
// picks v.ack_voice_snd_id_by_race[c.rand_below_fx(3) + race_bucket], plays it, stamps
// v.ack_voice_last_play_time = v.game_clock, and resets the suppressed counter.
//
// c.snd_play is the domain's ONE effectful call, reached on BOTH arms here.
void group_order_ack_voice(const issue_view &v, const order_sink &s, const issue_calls &c);

// llm_strat_group_issue_move_order_deferred @0x00444e5f (364 B). void __mh_watcall_ebx_volatile
// (int op_code, int op_arg, int modifier) -- EAX/EDX/EBX. NO golden site: emits only via the sibling
// wrappers below, never through `s` directly.
//
// `mod` (the modifier ultimately passed to the sibling calls) starts as `modifier` and, only when
// `modifier != 0`, gets OR'd with v.order_seq_id_by_player[*v.player_side] (0x00444e85-0x00444e99).
//
// Iterates v.ctrl_groups[0]'s member units (v.ctrl_groups[0].count of them). For each member whose
// cfg type is NEITHER UNIT_TYPE_A_HELI_CARGO(0x17) NOR UNIT_TYPE_H_HELI_CARGO(0x18) (0x00444ee9-
// 0x00444f27; the two skipped types share JUST that pair, not a range), it calls:
//   - detail::unit_order_move_default  when (v.key_lalt_held & 1) != 0 OR (v.key_lalt_held & 2) != 0
//   - detail::unit_order_move          otherwise
// both as (player=*v.player_side, unit_id, op_code, op_arg, mod) -- register trace at 0x00444f3d-
// 0x00444f7e: EAX=player, EDX=unit_id, EBX=op_code, ECX=op_arg, mod pushed on the stack. Any dispatch
// at all sets `did_move`.
//
// After the loop (0x00444f8f-0x00444fc3): if `did_move`, calls detail::group_order_ack_voice FIRST;
// THEN, if `mod != 0`, stamps v.order_seq_id_by_player[*v.player_side]: increment, and increment AGAIN
// if that wrapped to 0 (0 is never emitted as a seq id). This function's write ordering is
// ack-voice-then-seq-stamp; its `_confirmed` sibling below does the two in the OPPOSITE order -- both
// transcribed exactly as the assembly has them.
void group_issue_move_order_deferred(const issue_view &v, const order_sink &s, const issue_calls &c,
                                     int32_t op_code, int32_t op_arg, int32_t modifier);

// llm_strat_group_issue_move_order_confirmed @0x00444fcb (519 B). void __watcall(int dst_x, int
// dst_y) -- EAX/EDX. NO golden site: emits only via the sibling wrappers below.
//
// Same v.ctrl_groups[0] iteration and the same A_HELI_CARGO/H_HELI_CARGO skip as `_deferred` above
// (0x00445015-0x00445085 -- IDENTICAL skip test, independently transcribed). For every surviving
// member, reads cfg_units[proto].equivalent and cfg_units[proto].type ONCE (the .asm re-loads both
// from scratch at 0x004450f5-0x00445123 for a SECOND, value-identical check with nothing written in
// between -- a codegen artifact, not a second real condition; written once here per
// issue_bldg_orders.cpp's repair_cycle_start precedent for the same pattern) and branches:
//   - `equivalent == 0 || (int32_t)type < 0xf` (0x004450b3/0x004450ea, SIGNED compare on type):
//     computes `mod = (0x40 << 8) | v.order_seq_id_by_player[*v.player_side]` (0x00445153-0x00445164,
//     `OR AH,0x40` on the zero-extended seq byte -- CONCAT11(0x40, seq)) and calls
//     detail::unit_order_move(player, unit_id, dst_x, dst_y, mod).
//   - otherwise (equivalent != 0 && type >= 0xf): calls
//     detail::unit_order_move_confirmed_with_bump(player, unit_id, dst_x, dst_y) -- FOUR args, no
//     modifier; register trace at 0x0044512c-0x00445145 has no stack push.
// Either call sets `did_move`; `mod` stays 0 unless the first arm ran at least once.
//
// After the loop (0x00445195-0x004451c9): if `mod != 0`, stamps v.order_seq_id_by_player[*v.player_side]
// with the SAME increment/re-increment-on-wrap shape as `_deferred`, THEN, if `did_move`, calls
// detail::group_order_ack_voice -- seq-stamp BEFORE ack-voice, the opposite order from `_deferred`.
void group_issue_move_order_confirmed(const issue_view &v, const order_sink &s, const issue_calls &c,
                                      int32_t dst_x, int32_t dst_y);

} // namespace detail

void group_order_ack_voice();
void group_issue_move_order_deferred(int32_t op_code, int32_t op_arg, int32_t modifier);
void group_issue_move_order_confirmed(int32_t dst_x, int32_t dst_y);

} // namespace mh::orders::issue
