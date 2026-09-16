//
// lockstep/tx_emit_ctrl.h -- eight of the nineteen control-message emitters, reimplemented
// (RI-WIRE / W6-B). UNREVIEWED DRAFT: written cold against the disassembly only, no adversarial
// pass yet (that is the reimpl-verify step W6 schedules next). Treat every claim below as "read off
// the bytes, not yet cross-checked by a second reader."
//
// Scope: the eight MSG_CONTROL builders with small, fixed-size payloads --
//
//   llm_net_lockstep_send_presence_lost      @0x0049e328  (CTL_PLAYER_LEFT,    no payload,  no tail)
//   llm_net_lockstep_send_slot_reset         @0x0049e189  (CTL_SLOT_RESET,     side_id,     TAIL)
//   llm_net_lockstep_send_horizon_ack        @0x0049e01f  (tag 6,              side_id,     TAIL)
//   llm_net_lockstep_send_horizon_desync     @0x0049e0d4  (tag 7,              side_id,     TAIL)
//   llm_net_send_lockstep_kick               @0x0049dc16  (CTL_KICK,           side_id,     no tail)
//   llm_net_send_lockstep_step_size          @0x0049da72  (CTL_SET_STEP,       double,      no tail)
//   llm_net_lockstep_broadcast_resync_state  @0x0049d8ef  (CTL_RESYNC_BEGIN,   double,      TAIL)
//   llm_net_send_lockstep_resync_resume      @0x0049dbb4  (CTL_RESYNC_END,     no payload,  no tail)
//
// The other eleven of the nineteen are out of scope here: five are done (tx_emit.h, W4), three are
// the chat pair + flush primitive (tx_emit_chat.h, W6-A, a different shape -- variable payload,
// GUARDED), and three (rate_scale, ctrl_sub2, peer_horizon) are provably dead per docs/wire-
// emitters.md and are deliberately NOT translated (W6: "DO NOT REIMPLEMENT THE
// THREE DEAD ONES").
//
// EVERYTHING WAS TRANSLATED FROM tmp/decomp_wire/*_00*.asm, NOT from the decompile beside each.
// TWO of the eight .asm FILENAMES PREDATE A RENAME -- match by ADDRESS, not filename:
//   tmp/decomp_wire/llm_net_send_lockstep_peer_timeout_drop_0049dc16.asm  is now llm_net_send_lockstep_kick
//   tmp/decomp_wire/llm_net_lockstep_send_status_reset_0049e189.asm      is now llm_net_lockstep_send_slot_reset
// Where the .c beside each disagrees with the .asm, the .asm wins -- see the per-function notes
// below for the one place that actually happened (slot_reset's own PLATE prose).
//
// THE ONE TEMPLATE, same as tx_emit.h: every function below builds its record through
// mh::net::emit_ctrl (include/ctrl_emit.h) over the ONE shared buffer (include/packet_buffer.h).
// This header adds nothing to either.
//
// THE CURSOR GUARD: ALL EIGHT OF THESE ARE UNGUARDED (`reset_first = false`) -- confirmed
// individually against each .asm below, not assumed. Per the wire-emitter inventory this is the BENIGN
// form (appends after any pending order batch rather than discarding it). Do not "tidy" this; see
// ctrl_emit.h's header comment for why a default would be a silent wire-format change.
//
// THE STACK PROBE IS DROPPED, same settled decision as tx_emit.h / the reimpl-loop skill's "settled
// once" list -- every one of these eight opens with `PUSH frame_size; CALL assert_stack_capacity
// (0x004cf46f)`, Watcom's stack-touch helper, no semantic effect. Not reproduced, not re-litigated.
//
// THE INLINED memcpy'S TRAILING `REP MOVSB` COPIES ZERO BYTES in every one of the SIX emitters here
// that have a fixed-size payload at all (slot_reset, horizon_ack, horizon_desync, kick, step_size,
// broadcast_resync_state): each payload is a multiple of 4 bytes (4 or 8), so `len & 3` is always 0.
// An inlining artifact, not a field -- emit_ctrl's single `std::memcpy` already reproduces the byte
// content correctly. presence_lost and resync_resume have NO payload at all (not even this artifact).
//
// STATE: NO NEW STRUCT. All four pieces of ambient state these eight functions touch --
// the packet buffer, the peer-state matrix, the status-flags byte, and PlayerSide -- already have a
// field in `mh::lockstep::emit_state` (tx_emit.h, W4), and `estate()` already binds every one of them
// to the live game. Reused verbatim; ZERO fields were added to it. (Contrast this with the new CALLS
// struct below, which genuinely needed three additions tx_emit.h's `emit_calls` does not carry.)
//
// CALLS: three outward calls these eight need that emit_calls (tx_emit.h) does not have --
// `reset_player_horizon` (slot_reset's tail), `fill_data` and `order_pending_enqueue` (both
// broadcast_resync_state's tail, building the local mirror order). Rather than grow emit_calls (which
// this task is explicitly forbidden from touching, and which would drag five unrelated functions'
// call set into scope for no reason) this header declares its OWN small `ctrl_emit_calls`, exactly
// the precedent turn_engine.h already set for its four per-batch call structs (game_calls,
// dispatch_calls, timekeeper_calls) -- one calls-struct per translation batch, not one universal
// struct that grows forever.
//
// WHY `reset_player_horizon` AND `order_pending_enqueue` ARE BOUND TO THE RAW THUNK, NOT TO OUR OWN
// REIMPLEMENTATION, even though BOTH already have one in this tree (turn_engine.cpp's
// `detail::reset_player_horizon`; the NET-delivered enqueue in orders/order_queue.cpp is a DIFFERENT
// function, `llm_strat_order_stage_scheduled`'s sibling, not this one -- `llm_strat_order_pending_
// enqueue` itself has no C++ body of its own anywhere in this tree yet, only the raw thunk). This is
// the SAME argument tx_emit.cpp already makes for `commit_horizon` and rx_dispatch.cpp already makes
// for its own `order_pending_enqueue` binding: the original's `CALL` lands wherever the trampoline
// currently points, so it follows THAT function's own promotion state automatically. Binding our C++
// directly here would make these two emitters run our reimplementation even when the callee's own
// promotion is OFF -- an unannounced coupling between two seams meant to be independently switchable,
// which would show up as an unattributable red run rather than a build error.
//
// HALF-SUPERSEDED BY C8-d (2026-07-30), and the split matters. `reset_player_horizon` is now bound
// DIRECT through MH_INTERNAL_CALL: C8-d folds `wire` into `lockstep` and drops per-seam subsets, so
// the configuration this paragraph guards against -- our emitter promoted while the turn engine is
// not -- is no longer expressible inside net + lockstep. `order_pending_enqueue` KEEPS the raw thunk,
// because the direct-call rule is deliberately scoped to net + lockstep and does NOT extend to
// mh::orders, 6 of whose 9 seams are live single-player logic with a real original behind them. Two
// slots in the same struct, two different verdicts, for the reason the scope line gives.
//
// TWO STALE PROTOTYPES, READ OFF THE `RET`, NOT ASSUMED (W6, confirmed
// independently here against the raw bytes and against the generated interop layer):
//
//   llm_net_send_lockstep_step_size     @0x0049da72  Ghidra's DB / the .c PLATE both say `void(void)`.
//                                        The bytes say otherwise: ECX=8, ESI=LEA[EBP+8] (a STACK
//                                        argument slot, not a register), and the function ends
//                                        `RET 0x8` -- it reads 8 bytes from [EBP+8] and uses them
//                                        AS ITS PAYLOAD. Translated here as `double step_size`.
//   llm_net_send_lockstep_resync_resume @0x0049dbb4  ALSO ends `RET 0x8`, ALSO a stale `void(void)`
//                                        prototype -- but unlike step_size, nothing in this body ever
//                                        reads [EBP+8] or [EBP+0xc]. It pops 8 stack bytes it never
//                                        touches. Translated here as truly `void()`, because nothing
//                                        observable depends on whatever the caller pushed; the
//                                        discrepancy is recorded, not silently normalised away.
//
// THIS STALE-PROTOTYPE PROBLEM IS NOT COSMETIC -- it currently POISONS THE GENERATED INTEROP LAYER.
// `mh_calls.gen.h` and `mh_export.gen.h` both refuse these two functions outright:
//
//   MH_UNAVAILABLE__prototype_contradicts_the_functions_own_RET llm_net_send_lockstep_step_size(...);
//   MH_UNAVAILABLE__prototype_contradicts_the_functions_own_RET llm_net_send_lockstep_resync_resume(...);
//   #define MH_EXPORT_REPLACE_llm_net_send_lockstep_step_size(IMPL)     static_assert(false, ...)
//   #define MH_EXPORT_REPLACE_llm_net_send_lockstep_resync_resume(IMPL) static_assert(false, ...)
//
// Neither is a problem FOR THIS FILE (nothing here calls the ORIGINAL step_size/resync_resume as an
// outward call -- we are implementing them, not calling them), so the `detail::`/production split
// below compiles and is testable regardless. It IS a problem for promotion: `install_wire_promotion`
// (tx_emit.cpp) cannot wire either of these two seams via `MH_EXPORT_REPLACE` until the Ghidra-side
// prototype is corrected (`the prototype-commit step` / `set-function-prototype`, both out of scope for
// this task) and the generated headers are regenerated. Flagged here so the promotion step (W6,
// remaining step (e)) does not discover it cold.
//
#pragma once
#include <cstdint>

#include "include/ctrl_emit.h"
#include "lockstep/tx_emit.h"     // mh::lockstep::emit_state, estate() -- reused verbatim, see above
#include "lockstep/turn_engine.h" // game_order, control_tag/outer_tag enums, MAX_PLAYERS

namespace mh::lockstep {

// The three outward calls these eight functions need beyond what emit_calls (tx_emit.h) already
// carries. transport_send and player_by_side_id are ALSO in emit_calls; they are repeated here
// rather than shared via inheritance/composition because every sibling batch in this module
// (game_calls/dispatch_calls/timekeeper_calls in turn_engine.h) already uses one flat struct per
// batch rather than a shared base -- consistency of shape over de-duplicating two pointers.
struct ctrl_emit_calls {
    mh::net::transport_send_fn transport_send; // llm_net_transport_send @0x0049ba8d -- a STUB in the
                                               // retail image; our DLL replaces it (see ctrl_emit.h).

    int32_t (*player_by_side_id)(int32_t side_id); // llm_strat_player_by_side_id @0x0049e388 --
                                                   // used by slot_reset/horizon_ack/horizon_desync to
                                                   // turn the wire side_id into a player-table index.
                                                   // NOTE kick does NOT call this (see its notes
                                                   // below) despite carrying the same side_id shape.

    void (*reset_player_horizon)(int32_t player_idx); // llm_net_lockstep_reset_player_horizon
                                                      // @0x0049e230 -- slot_reset's tail. Bound to the
                                                      // RAW thunk, not to turn_engine.cpp's own
                                                      // `mh::lockstep::reset_player_horizon`; see the
                                                      // long note above.

    void *(*fill_data)(void *ptr, uint32_t size, uint8_t fill); // fill_data @0x004d1780 --
                                                                // broadcast_resync_state's tail zeroes
                                                                // its local llm_strat_order through
                                                                // this before stamping four fields.

    int32_t (*order_pending_enqueue)(const game_order *order); // llm_strat_order_pending_enqueue
                                                               // @0x00466790 -- broadcast_resync_
                                                               // state's tail, the SAME function (and
                                                               // the SAME raw-thunk-not-ours binding
                                                               // choice) rx_dispatch.cpp's CTL_RESYNC_
                                                               // BEGIN handler already uses for its own
                                                               // copy of this exact order shape.
};

const ctrl_emit_calls &live_ctrl_calls();

namespace detail {

// ---- 1. llm_net_lockstep_send_presence_lost @0x0049e328 --------------------------------------------
//
// void __watcall(void) -- genuinely NO argument (confirmed: the body never reads EAX or any stack
// slot) and a plain `RET` (no stack cleanup, consistent with zero arguments). Builds MSG_CONTROL (4)
// / CTL_PLAYER_LEFT (1), NO payload, UNGUARDED, then sends. NO TAIL: the epilogue
// (0x0049e374..0x0049e387) follows the reset-cursor-to-0 store directly; nothing else happens.
void send_presence_lost(const emit_state &es, const ctrl_emit_calls &calls);

// ---- 2. llm_net_lockstep_send_slot_reset @0x0049e189 ------------------------------------------------
//
// void __watcall(EAX = side_id) -- plain `RET`. `pidx = player_by_side_id(side_id)` runs FIRST
// (0x0049e1a7), before any buffer write -- no reordering needed here, unlike tx_emit.h's
// send_removal_record. Builds MSG_CONTROL (4) / CTL_SLOT_RESET (4), UNGUARDED, payload = side_id
// itself (4B, the ORIGINAL side_id local, not pidx -- 0x0049e1d8 sources ESI from EBP-0x1c, the
// side_id copy, not EBP-0x18 where pidx lives).
//
// THE TAIL (0x0049e217..0x0049e225): `status_flags &= ~0x80` (clear LS_HORIZON_PENDING), THEN
// `reset_player_horizon(pidx)` -- **pidx, not side_id**. This is a real discrepancy between the
// bytes and this function's OWN plate comment in tmp/decomp_wire/llm_net_lockstep_send_status_reset_
// 0049e189.c ("resets that player's horizon-tracking state via llm_net_lockstep_reset_player_horizon
// (side_id)") -- both the .asm (`MOV EAX,[EBP-0x18]` = pidx, immediately before the CALL) and the
// decompiled body itself (`llm_net_lockstep_reset_player_horizon(local_1c)`, where local_1c is pidx,
// NOT local_20 which is side_id) pass the PLAYER INDEX, and the prose is simply wrong on this one
// point. Translated here from the bytes, per this task's standing rule that the .asm (and, doubly
// here, the .c's own CODE) wins over prose.
void send_slot_reset(const emit_state &es, const ctrl_emit_calls &calls, int32_t side_id);

// ---- 3. llm_net_lockstep_send_horizon_ack @0x0049e01f -----------------------------------------------
//
// void __watcall(EAX = side_id) -- plain `RET`. pidx computed first (0x0049e03d), same shape as
// slot_reset. Builds MSG_CONTROL (4) / inner tag 6, UNGUARDED, payload = side_id (4B, the local copy,
// same "side_id not pidx" shape as slot_reset).
//
// TAG 6 HAS NO NAMED CONSTANT OF ITS OWN HERE that reads naturally from the SENDER's side: turn_engine.h
// spells it `CTL_LEAVE_CONSENSUS` because that is what the RECEIVER's handler (handle_leave_consensus)
// does with it. The wire-emitter inventory's "SETTLED by W3" section is explicit that this is not a
// naming bug -- the same wire byte is genuinely "my horizon matches yours" from this function's end
// and "this peer agrees `side_id` has left" from the receiver's, and neither reading is wrong. Used
// here as `CTL_LEAVE_CONSENSUS` (the only existing named constant for the value 6) with this comment
// rather than a second, redundant name.
//
// THE TAIL (0x0049e0ad..0x0049e0c3): `peer_state[pidx*8 + PlayerSide] = 2`, then `status_flags |= 0x80`
// (SET LS_HORIZON_PENDING -- the OPPOSITE of slot_reset's tail, which clears it). The value 2 has no
// named constant anywhere in this tree's C++ (only PEER_STATE_NONE=0, PEER_STATE_LEAVING=4,
// PEER_STATE_PRESENT=6 exist); a fresh decompile of this exact function renders it as a
// decompiler-inferred enumerator `PEER_ACK`, which appears NOWHERE else in this codebase (not in
// docs/symbols.md, not in any prior annotation) -- an unverified, one-off label, not a confirmed
// project name. Used here as the literal `2` with this comment rather than silently adopting an
// unverified enumerator.
void send_horizon_ack(const emit_state &es, const ctrl_emit_calls &calls, int32_t side_id);

// ---- 4. llm_net_lockstep_send_horizon_desync @0x0049e0d4 --------------------------------------------
//
// BYTE-FOR-BYTE the same shape as send_horizon_ack (same pidx-first order, same UNGUARDED payload
// shape), differing only in the inner tag (7, `CTL_STATUS_RESET_REQ` -- same naming-tension note as
// tag 6 above, see the wire-emitter inventory) and the tail's stamped value (3, not 2 -- again unnamed in
// this tree's C++; a fresh decompile renders it `PEER_DESYNC`, same unverified-one-off caveat as
// `PEER_ACK` above). NOT factored into a shared helper with send_horizon_ack the way tx_emit.h
// factors player_remove/player_remove_timeout: those two share ALL FOUR bits of their status-flags
// triple identically, whereas these two differ in BOTH the inner tag AND the stamped value, so a
// shared helper would need two parameters doing all the work of two separate functions.
void send_horizon_desync(const emit_state &es, const ctrl_emit_calls &calls, int32_t side_id);

// ---- 5. llm_net_send_lockstep_kick @0x0049dc16 ------------------------------------------------------
//
// void __watcall(EAX = side_id) -- plain `RET`. UNLIKE the three functions above, this one does
// **NOT** call player_by_side_id at all -- confirmed by its absence from the .asm (no CALL to
// 0x0049e388 anywhere in the 0x8d-byte body) and by the .c decompile agreeing. Builds MSG_CONTROL (4)
// / CTL_KICK (10), UNGUARDED, payload = side_id (4B, straight from the argument, no lookup). NO TAIL:
// the epilogue follows the reset-cursor store directly (0x0049dc8f..0x0049dca2), same shape as
// presence_lost.
void send_lockstep_kick(const emit_state &es, const ctrl_emit_calls &calls, int32_t side_id);

// ---- 6. llm_net_send_lockstep_step_size @0x0049da72 -------------------------------------------------
//
// SEE THE STALE-PROTOTYPE NOTE AT THE TOP OF THIS FILE. Really `void __watcall(double step_size)`
// with the double on the STACK at [EBP+8] (ECX=8, ESI=LEA[EBP+8]), `RET 0x8`. Builds MSG_CONTROL (4)
// / CTL_SET_STEP (11), UNGUARDED, payload = the 8-byte step_size verbatim. NO TAIL: nothing after the
// reset-cursor store (0x0049dae8..0x0049dafb).
void send_lockstep_step_size(const emit_state &es, const ctrl_emit_calls &calls, double step_size);

// ---- 7. llm_net_lockstep_broadcast_resync_state @0x0049d8ef -----------------------------------------
//
// void __watcall(double exec_time), stack arg at [EBP+8], `RET 0x8` -- this one's OWN prototype is
// NOT stale (Ghidra/.c already have it right; only step_size and resync_resume are stale). Builds
// MSG_CONTROL (4) / CTL_RESYNC_BEGIN (13), UNGUARDED, payload = the 8-byte exec_time verbatim, THEN
// sends.
//
// DECOMPILER ARTIFACT, already flagged by a human reader in the .c PLATE and independently confirmed
// here against the raw bytes: at 0x0049d979 there is a `CALL 0x004d1780` (fill_data); Ghidra's
// decompile of THIS call site renders the return address it pushes as `order.args[0xc] =
// 0x49d97e` (and, after the send, `= 0x49d965` at the earlier call site). NEITHER is a real write to
// the order's args array -- args[0xc] is never touched by this function. Not reproduced.
//
// THE TAIL (0x0049d96f..0x0049d9d3): builds a zeroed 0x44-byte llm_strat_order and pushes it BY VALUE
// to llm_strat_order_pending_enqueue (0x00466790) -- the exact same "global event" shape rx_dispatch.
// cpp's CTL_RESYNC_BEGIN handler builds for the RECEIVE side of this same tag:
//   fill_data(&order, 0x44, 0);
//   order.param0         = 13;                      // 0x0049d97e, offset 0xc
//   order.order_code     = (uint16_t)(uint32_t)order.param0;  // 0x0049d984-0987, offset 0xe -- see below
//   order.owner_and_kind = 0xf0;                     // 0x0049d98b, offset 0xa -- "global event"
//   order.unit_index     = 0;                        // 0x0049d991, offset 0x8 -- explicit despite
//                                                     // fill_data already zeroing it
//   order.exec_time      = exec_time;                // 0x0049d997-0x0049d9a3, offset 0x0, written
//                                                     // LAST in the original -- reordered to the
//                                                     // struct's natural field order below since none
//                                                     // of these five writes touch an overlapping
//                                                     // byte; not a spec change.
// THE order_code READ-BACK (0x0049d984: `MOV EAX,[EBP-0x4c]` reads a DWORD starting at param0's own
// offset, i.e. param0's word PLUS order_code's word, which fill_data has already zeroed -- so the
// dword read is exactly `(uint32_t)param0`) is the identical construction rx_dispatch.cpp's own
// CTL_RESYNC_BEGIN comment documents for the receive side; reproduced here as the equivalent plain
// assignment rather than the literal read-back, for the same reason that comment gives.
// Takes `fx` because it carries a migrated fix (MP D14 / D17's resync_order_horizon) -- same shape
// as resync_trigger_tick's, and the reason is the same: a fix that is a PARAMETER can be driven both
// ways by lockstest, where a global could not.
void lockstep_broadcast_resync_state(const emit_state &es, const ctrl_emit_calls &calls,
                                     const reimpl_fixes &fx, double exec_time);

// ---- 8. llm_net_send_lockstep_resync_resume @0x0049dbb4 ---------------------------------------------
//
// SEE THE STALE-PROTOTYPE NOTE AT THE TOP OF THIS FILE. `RET 0x8` like step_size, but this body never
// reads [EBP+8]/[EBP+0xc] at all -- confirmed by their total absence from the 0x62-byte disassembly.
// Builds MSG_CONTROL (4) / CTL_RESYNC_END (14), NO payload, UNGUARDED, then sends. NO TAIL: the
// epilogue follows the reset-cursor store directly (0x0049dc00..0x0049dc13), same shape as
// presence_lost and kick. Translated here as a true `void()` -- the 8 popped stack bytes are real (a
// caller somewhere pushes them) but genuinely inert for this function's own behaviour.
void send_lockstep_resync_resume(const emit_state &es, const ctrl_emit_calls &calls);

} // namespace detail

// ---- production entry points (bound to the live game state via mh::lockstep::estate()) -------------
void send_presence_lost();
void send_slot_reset(int32_t side_id);
void send_horizon_ack(int32_t side_id);
void send_horizon_desync(int32_t side_id);
void send_lockstep_kick(int32_t side_id);
void send_lockstep_step_size(double step_size);
void lockstep_broadcast_resync_state(double exec_time);
void send_lockstep_resync_resume();

} // namespace mh::lockstep
