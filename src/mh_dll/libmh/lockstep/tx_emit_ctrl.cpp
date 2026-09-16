//
// lockstep/tx_emit_ctrl.cpp -- the eight emitters declared in tx_emit_ctrl.h (RI-WIRE / W6-B).
// UNREVIEWED DRAFT -- see the header's top comment.
//
// Translated from tmp/decomp_wire/llm_net_lockstep_send_presence_lost_0049e328.asm,
// tmp/decomp_wire/llm_net_lockstep_send_status_reset_0049e189.asm (now llm_net_lockstep_send_slot_
// reset -- the .asm filename predates the rename),
// tmp/decomp_wire/llm_net_lockstep_send_horizon_ack_0049e01f.asm,
// tmp/decomp_wire/llm_net_lockstep_send_horizon_desync_0049e0d4.asm,
// tmp/decomp_wire/llm_net_send_lockstep_peer_timeout_drop_0049dc16.asm (now llm_net_send_lockstep_
// kick -- ditto),
// tmp/decomp_wire/llm_net_send_lockstep_step_size_0049da72.asm,
// tmp/decomp_wire/llm_net_lockstep_broadcast_resync_state_0049d8ef.asm and
// tmp/decomp_wire/llm_net_send_lockstep_resync_resume_0049dbb4.asm -- NOT from the decompile beside
// each; see tx_emit_ctrl.h for the specific spots where the .c disagrees with the .asm.
//
#include "lockstep/tx_emit_ctrl.h"
#include "lockstep/lt_player_by_side_id.h" // LIB-TRANS-P direct edge

#include "addr/mh_calls.gen.h"
#include "state/host_api.h"
#include "lockstep/internal_call.h" // C8-d: MH_INTERNAL_CALL -- the intra-closure edges
#include "lockstep/turn_engine.h"   // C8-d: reset_player_horizon, the production entry point
#include "addr/mh_rebind.gen.h"     // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::lockstep {

const ctrl_emit_calls &live_ctrl_calls() {
    static const ctrl_emit_calls cc = {
        // Same adapter tx_emit.cpp uses: llm_net_transport_send takes `void *`, emit_ctrl's
        // transport_send_fn is spelled over `uint8_t *` (see ctrl_emit.h).
        [](uint8_t *buf, int32_t len) { mh::host().transport_send(buf, len); },
        MH_INTERNAL_CALL(llm_strat_player_by_side_id, mh::lockstep::player_by_side_id),
        // DIRECT since C8-d, reversing the W6-B binding the long note in tx_emit_ctrl.h argues for.
        // Its premise -- that the turn engine and the emitters must stay independently switchable --
        // is what C8-d retires; see the same reversal, argued at length, in tx_emit.cpp's
        // live_emit_calls().
        MH_INTERNAL_CALL(llm_net_lockstep_reset_player_horizon, mh::lockstep::reset_player_horizon),
        MH_CRT(utils_fill_data),
        // STILL THE RAW 0x00466790 THUNK, and this one is NOT reversed: llm_strat_order_pending_enqueue
        // is a mh::orders seam and C8's direct-call rule is scoped to net + lockstep. Matches
        // rx_dispatch.cpp's binding of this same callee for its CTL_RESYNC_BEGIN handler.
        &mh::call::llm_strat_order_pending_enqueue, // LIB-REF-SPLIT: NOT converted -- see below
        // THE ONE SITE MH_PROMOTED COULD NOT TAKE, and the type check is why. This slot is
        // `int32_t (*)(const game_order *)`, inherited from the generated prototype, while
        // mh::orders::pending_enqueue takes `order *` NON-const -- and that is the correct
        // signature, not an oversight: the body masks four ushort fields of *rec to their low
        // byte IN PLACE before copying, exactly as the original does at 0x004667b4-0x004667c6.
        // So the const on the slot is the wrong half, and fixing it changes a HOSTED
        // calls-struct member type -- which this item's acceptance puts out of bounds.
        // Left as the entry-routed thunk with the residue recorded rather than const_cast away.
    };
    return cc;
}

namespace detail {

// ---- 1. llm_net_lockstep_send_presence_lost @0x0049e328 --------------------------------------------
void send_presence_lost(const emit_state &es, const ctrl_emit_calls &calls) {
    // NOTE ON `es.packet`: emit_ctrl takes a mutable `packet_buffer &`, but every function here takes
    // `const emit_state &` (so a test can pass a `const` fixture) -- same idiom tx_emit.cpp documents
    // at length; not repeated per-function here.
    mh::net::packet_buffer pb = es.packet;
    mh::net::ctrl_record   rec{};
    rec.outer       = static_cast<uint8_t>(MSG_CONTROL); // 4 -- 0x0049e345
    rec.has_inner   = true;
    rec.inner       = static_cast<uint8_t>(CTL_PLAYER_LEFT); // 1 -- 0x0049e357
    rec.payload     = nullptr;
    rec.payload_len = 0;
    rec.reset_first = false; // UNGUARDED
    mh::net::emit_ctrl(pb, rec, calls.transport_send);
    // No tail -- the epilogue follows the reset-cursor store directly (0x0049e374..0x0049e387).
}

// ---- 2. llm_net_lockstep_send_slot_reset @0x0049e189 ------------------------------------------------
void send_slot_reset(const emit_state &es, const ctrl_emit_calls &calls, int32_t side_id) {
    // Computed FIRST in the original (0x0049e1a7), before any buffer write -- no reordering needed.
    const int32_t pidx = calls.player_by_side_id(side_id);

    mh::net::packet_buffer pb = es.packet;
    mh::net::ctrl_record   rec{};
    rec.outer       = static_cast<uint8_t>(MSG_CONTROL); // 4
    rec.has_inner   = true;
    rec.inner       = static_cast<uint8_t>(CTL_SLOT_RESET); // 4 -- 0x0049e1c6
    rec.payload     = &side_id;                             // the ORIGINAL side_id, not pidx -- 0x0049e1d8 sources EBP-0x1c
    rec.payload_len = static_cast<int32_t>(sizeof(int32_t));
    rec.reset_first = false; // UNGUARDED
    mh::net::emit_ctrl(pb, rec, calls.transport_send);

    // ---- the tail (0x0049e217..0x0049e225) ----
    *es.status_flags &= static_cast<uint8_t>(~LS_HORIZON_PENDING); // AND 0x7f -- clear bit 0x80
    // pidx, NOT side_id -- see the long discrepancy note in tx_emit_ctrl.h against this function's
    // own PLATE prose.
    calls.reset_player_horizon(pidx);
}

// ---- 3. llm_net_lockstep_send_horizon_ack @0x0049e01f -----------------------------------------------
void send_horizon_ack(const emit_state &es, const ctrl_emit_calls &calls, int32_t side_id) {
    const int32_t pidx = calls.player_by_side_id(side_id); // 0x0049e03d, before any buffer write

    mh::net::packet_buffer pb = es.packet;
    mh::net::ctrl_record   rec{};
    rec.outer     = static_cast<uint8_t>(MSG_CONTROL); // 4
    rec.has_inner = true;
    // Value 6 -- CTL_LEAVE_CONSENSUS is the only existing name for this byte, coined from the
    // RECEIVER's reading of it; see the long note in tx_emit_ctrl.h.
    rec.inner       = static_cast<uint8_t>(CTL_LEAVE_CONSENSUS); // 0x0049e05c
    rec.payload     = &side_id;                                  // side_id, not pidx -- same shape as slot_reset
    rec.payload_len = static_cast<int32_t>(sizeof(int32_t));
    rec.reset_first = false; // UNGUARDED
    mh::net::emit_ctrl(pb, rec, calls.transport_send);

    // ---- the tail (0x0049e0ad..0x0049e0c3) ----
    // peer_state[pidx][PlayerSide] = 2 ("ack"; no named constant in this tree -- see tx_emit_ctrl.h).
    es.peer_state[pidx * MAX_PLAYERS + *es.player_side] = 2;
    *es.status_flags |= LS_HORIZON_PENDING; // OR 0x80 -- SET, the opposite of slot_reset's tail
}

// ---- 4. llm_net_lockstep_send_horizon_desync @0x0049e0d4 --------------------------------------------
void send_horizon_desync(const emit_state &es, const ctrl_emit_calls &calls, int32_t side_id) {
    const int32_t pidx = calls.player_by_side_id(side_id); // 0x0049e0f2, before any buffer write

    mh::net::packet_buffer pb = es.packet;
    mh::net::ctrl_record   rec{};
    rec.outer     = static_cast<uint8_t>(MSG_CONTROL); // 4
    rec.has_inner = true;
    // Value 7 -- CTL_STATUS_RESET_REQ, same naming-tension note as tag 6 in send_horizon_ack.
    rec.inner       = static_cast<uint8_t>(CTL_STATUS_RESET_REQ); // 0x0049e111
    rec.payload     = &side_id;
    rec.payload_len = static_cast<int32_t>(sizeof(int32_t));
    rec.reset_first = false; // UNGUARDED
    mh::net::emit_ctrl(pb, rec, calls.transport_send);

    // ---- the tail (0x0049e162..0x0049e178) ----
    // peer_state[pidx][PlayerSide] = 3 ("desync"; no named constant -- see tx_emit_ctrl.h).
    es.peer_state[pidx * MAX_PLAYERS + *es.player_side] = 3;
    *es.status_flags |= LS_HORIZON_PENDING; // OR 0x80, same as horizon_ack's tail
}

// ---- 5. llm_net_send_lockstep_kick @0x0049dc16 ------------------------------------------------------
void send_lockstep_kick(const emit_state &es, const ctrl_emit_calls &calls, int32_t side_id) {
    // NO player_by_side_id call -- confirmed absent from the 0x8d-byte body (unlike the three
    // functions above). The payload is side_id straight from the argument.
    mh::net::packet_buffer pb = es.packet;
    mh::net::ctrl_record   rec{};
    rec.outer       = static_cast<uint8_t>(MSG_CONTROL); // 4
    rec.has_inner   = true;
    rec.inner       = static_cast<uint8_t>(CTL_KICK); // 10 -- 0x0049dc48
    rec.payload     = &side_id;
    rec.payload_len = static_cast<int32_t>(sizeof(int32_t));
    rec.reset_first = false; // UNGUARDED
    mh::net::emit_ctrl(pb, rec, calls.transport_send);
    // No tail -- the epilogue follows the reset-cursor store directly (0x0049dc8f..0x0049dca2).
}

// ---- 6. llm_net_send_lockstep_step_size @0x0049da72 -------------------------------------------------
void send_lockstep_step_size(const emit_state &es, const ctrl_emit_calls &calls, double step_size) {
    // SEE THE STALE-PROTOTYPE NOTE in tx_emit_ctrl.h: this really takes a stack double at [EBP+8],
    // not `void`. The payload IS that double, read verbatim (0x0049dab3: LEA ESI,[EBP+8]).
    mh::net::packet_buffer pb = es.packet;
    mh::net::ctrl_record   rec{};
    rec.outer       = static_cast<uint8_t>(MSG_CONTROL); // 4
    rec.has_inner   = true;
    rec.inner       = static_cast<uint8_t>(CTL_SET_STEP); // 11 -- 0x0049daa1
    rec.payload     = &step_size;
    rec.payload_len = static_cast<int32_t>(sizeof(double));
    rec.reset_first = false; // UNGUARDED
    mh::net::emit_ctrl(pb, rec, calls.transport_send);
    // No tail -- the epilogue follows the reset-cursor store directly (0x0049dae8..0x0049dafb).
}

// ---- 7. llm_net_lockstep_broadcast_resync_state @0x0049d8ef -----------------------------------------
void lockstep_broadcast_resync_state(const emit_state &es, const ctrl_emit_calls &calls,
                                     const reimpl_fixes &fx, double exec_time) {
    // MP D14, carried here by D17, corrected by D24. The clamp is applied ONCE, at the top, before
    // either consumer -- and that is the entire point of doing it at this function rather than at its
    // two tails: the exec_time is used TWICE below (the CTL_RESYNC_BEGIN wire payload and the local
    // mirror order), and clamping one without the other would put the peers further apart than the
    // bug does. OFF is the faithful original, which passes the caller's literal 2.0 straight through.
    //
    // D24 changed WHAT it clamps to, not where. The bare horizon is the barrier the peers are already
    // sitting at, so an order stamped there is a race the local mirror always wins; the barrier is one
    // step past it. The argument, including why reordering force_resync is not the fix, is at
    // mh::fix::resync_order_barrier (fix/resync_clamp.h).
    if (fx.resync_order_horizon)
        exec_time = detail::resync_order_exec_time(
            exec_time, detail::resync_order_barrier(*es.horizon, *es.game_clock, *es.step_size));

    mh::net::packet_buffer pb = es.packet;
    mh::net::ctrl_record   rec{};
    rec.outer       = static_cast<uint8_t>(MSG_CONTROL); // 4
    rec.has_inner   = true;
    rec.inner       = static_cast<uint8_t>(CTL_RESYNC_BEGIN); // 13 -- 0x0049d91e
    rec.payload     = &exec_time;
    rec.payload_len = static_cast<int32_t>(sizeof(double));
    rec.reset_first = false; // UNGUARDED
    mh::net::emit_ctrl(pb, rec, calls.transport_send);

    // ---- the tail (0x0049d96f..0x0049d9d3): the LOCAL mirror order -----------------------------
    // See tx_emit_ctrl.h for the field-by-field derivation and the decompiler-artifact warning at
    // 0x0049d97e/0x0049d965 (a CALL return address rendered as a fake `args[0xc]` write -- not
    // reproduced here, it never was a real field).
    game_order order;
    calls.fill_data(&order, sizeof(order), 0);
    order.param0         = static_cast<int16_t>(CTL_RESYNC_BEGIN);                     // 13, offset 0xc
    order.order_code     = static_cast<uint16_t>(static_cast<uint32_t>(order.param0)); // offset 0xe
    order.owner_and_kind = 0xf0;                                                       // "global event" -- offset 0xa, 0x0049d98b
    order.unit_index     = 0;                                                          // offset 0x8, 0x0049d991 -- explicit despite fill_data's zero
    order.exec_time      = exec_time;                                                  // offset 0x0 -- written LAST in the original; reordered here,
                                                                                       // see tx_emit_ctrl.h for why that is safe (disjoint fields).
    calls.order_pending_enqueue(&order);
}

// ---- 8. llm_net_send_lockstep_resync_resume @0x0049dbb4 ---------------------------------------------
void send_lockstep_resync_resume(const emit_state &es, const ctrl_emit_calls &calls) {
    // SEE THE STALE-PROTOTYPE NOTE in tx_emit_ctrl.h: `RET 0x8` like step_size, but this body never
    // reads the popped bytes at all. No parameter here -- there is nothing to pass through.
    mh::net::packet_buffer pb = es.packet;
    mh::net::ctrl_record   rec{};
    rec.outer       = static_cast<uint8_t>(MSG_CONTROL); // 4
    rec.has_inner   = true;
    rec.inner       = static_cast<uint8_t>(CTL_RESYNC_END); // 14 -- 0x0049dbe3
    rec.payload     = nullptr;
    rec.payload_len = 0;
    rec.reset_first = false; // UNGUARDED
    mh::net::emit_ctrl(pb, rec, calls.transport_send);
    // No tail -- the epilogue follows the reset-cursor store directly (0x0049dc00..0x0049dc13).
}

} // namespace detail

// ---- production entry points (bound to the live game state) ----------------------------------------
void send_presence_lost() {
    // C8-d: the liveness counter is at the PRODUCTION ENTRY, not the entry thunk -- see the long
    // note at turn_engine.cpp's production entry points. A seam reached only from inside the
    // closure never touches its entry once the internal edges are direct.
    promoted_wire::live(8, "wire/send_presence_lost");
    detail::send_presence_lost(estate(), live_ctrl_calls());
}

void send_slot_reset(int32_t side_id) {
    promoted_wire::live(9, "wire/send_slot_reset");
    detail::send_slot_reset(estate(), live_ctrl_calls(), side_id);
}

void send_horizon_ack(int32_t side_id) {
    promoted_wire::live(10, "wire/send_horizon_ack");
    detail::send_horizon_ack(estate(), live_ctrl_calls(), side_id);
}

void send_horizon_desync(int32_t side_id) {
    promoted_wire::live(11, "wire/send_horizon_desync");
    detail::send_horizon_desync(estate(), live_ctrl_calls(), side_id);
}

void send_lockstep_kick(int32_t side_id) {
    promoted_wire::live(12, "wire/send_lockstep_kick");
    detail::send_lockstep_kick(estate(), live_ctrl_calls(), side_id);
}

void send_lockstep_step_size(double step_size) {
    promoted_wire::live(13, "wire/send_lockstep_step_size");
    detail::send_lockstep_step_size(estate(), live_ctrl_calls(), step_size);
}

void lockstep_broadcast_resync_state(double exec_time) {
    promoted_wire::live(14, "wire/broadcast_resync_state");
    detail::lockstep_broadcast_resync_state(estate(), live_ctrl_calls(), fixes(), exec_time);
}

void send_lockstep_resync_resume() {
    promoted_wire::live(15, "wire/send_lockstep_resync_resume");
    detail::send_lockstep_resync_resume(estate(), live_ctrl_calls());
}

} // namespace mh::lockstep
