#include "lockstep/lockstep_state.h" // SB-BIND T4: host_binds() -- the production-arm addresses
#include "lockstep/turn_engine.h"
#include "lockstep/lt_chat_ally_mask.h"    // SIMABI-CHAT: the internalized chat_recalc_target_mode
#include "lockstep/lt_player_by_side_id.h" // LIB-TRANS-P direct edge

#include "addr/mh_addrs.gen.h"
#include "addr/mh_calls.gen.h"
#include "lockstep/internal_call.h" // MH_INTERNAL_CALL -- C8-d, the intra-closure edges
#include "lockstep/net_session.h"   // resync_complete_local -- the ONE resync-completion transition
#include "lockstep/overlay_hoist.h" // LIB-ABI stage E: outcome_dialog's hoisted GAME_MODE=3
#include "lockstep/resync.h"        // is_local_leader_peer / force_resync / count_active_players /
                                    // sync_busywait -- the production entry points
#include "lockstep/tx_emit.h"       // send_lockstep_extend, player_remove
#include "lockstep/tx_emit_ctrl.h"  // send_horizon_ack / _desync, send_slot_reset, send_lockstep_kick
#include "orders/order_codec.h"     // ST5: the ONE order-record layout, shared with the save

#include <cstring>
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::lockstep {

gate_audit &dispatch_gate_audit() {
    static gate_audit a;
    return a;
}

namespace {

// The three w_sprintf shapes, folded to fixed arity for the same reason batch B folded its alert
// line: w_sprintf is varargs, gen_dll_calls refuses varargs by design (a shape table cannot describe
// an open argument list), and it needs no marshalling anyway -- every argument is on the stack and the
// CALLER cleans (`ADD ESP,0x10` / `ADD ESP,0xc`), which is __cdecl however Ghidra labels it.
// The three format strings, as C++ literals rather than registry-bound .rdata pointers -- libmh/sim/'s
// precedent (sim_bldg_completion_dispatch.cpp's TEXT_FMT_NAME_REASON and the two TUs its comment
// cites). These are sprintf format PUNCTUATION; the localized text arrives through G_TEXT_PTRS as an
// argument, so an identical literal produces identical output. The addresses stay in the comments
// because they are what a reader checks against the disassembly.
//
// KEEP THE 0x005017bc / 0x005009e8 NOTE: the two hold the SAME text and are DIFFERENT objects in the
// binary (timekeeper.cpp/turn_engine.cpp use the other one). Nothing here collapses them; the
// duplication is the original's, and a later reader who notices it should not "fix" the binary.
constexpr const wchar_t *TEXT_FMT_LABEL_PAREN_NAME = L"%s (%s)"; // 0x005017bc
constexpr const wchar_t *TEXT_FMT_NAME_COLON_TEXT  = L"%s: %s";  // 0x005017cc
constexpr const wchar_t *TEXT_FMT_PLAIN            = L"%s";      // 0x005017da

// w_sprintf(&G_TEXT_TMP, u"%s (%s)", G_TEXT_PTRS[text_id], wide_name) -- five sites, all with the
// SAME format string 0x005017bc.
void live_format_player_line(int32_t text_id, void *wide_name) {
    const host_bind_state hb = host_binds();
    MH_CRT(w_sprintf__vss)(hb.text_scratch, TEXT_FMT_LABEL_PAREN_NAME,
                           static_cast<const wchar_t *>(hb.text_ptrs[text_id]),
                           static_cast<const wchar_t *>(wide_name));
}

// w_sprintf(&G_TEXT_TMP, u"%s: %s", wide_name, wide_text) -- the chat line, 0x0049d1e6.
void live_format_chat_line(void *wide_name, void *wide_text) {
    MH_CRT(w_sprintf__vss)(host_binds().text_scratch, TEXT_FMT_NAME_COLON_TEXT,
                           static_cast<const wchar_t *>(wide_name),
                           static_cast<const wchar_t *>(wide_text));
}

// w_sprintf(&G_TEXT_TMP, u"%s", G_TEXT_PTRS[text_id]) -- THREE arguments, not four, and the original
// cleans 0xc not 0x10 (0x0049d280): mh::call::w_sprintf__vs is the matching arity. The unknown-tag
// warning, 0x0049d27b.
void live_format_plain_line(int32_t text_id) {
    const host_bind_state hb = host_binds();
    MH_CRT(w_sprintf__vs)(hb.text_scratch, TEXT_FMT_PLAIN,
                          static_cast<const wchar_t *>(hb.text_ptrs[text_id]));
}

// The G_TEXT_PTRS indices the handlers name. The original spells each as a folded absolute address
// (e.g. 0x005846a0 = cfg_G_TEXT_PTRS + 165*4); these are those addresses divided back out.
constexpr int32_t TXT_SESSION_ENDED  = 164; // 0x0058469c -- CTL_SESSION_ENDED
constexpr int32_t TXT_PLAYER_LEFT    = 165; // 0x005846a0 -- CTL_PLAYER_LEFT
constexpr int32_t TXT_PLAYER_DROPPED = 166; // 0x005846a4 -- CTL_LEAVE_CONSENSUS / DROP_SYNCED / _UNSYNCED
constexpr int32_t TXT_CHAT_SUFFIX    = 171; // 0x005846b8 -- appended to MESSAGE_QUEUE for a message
                                            //               that was not addressed to us
constexpr int32_t TXT_STREAM_GARBLED = 781; // 0x00585040 -- the unknown-tag warning

// The outcome code the unknown-tag path hands the end-of-game dialog (`MOV EAX,0x7` at 0x0049d30f).
constexpr uint8_t OUTCOME_NETWORK_ERROR = 7;

// ---- little readers ------------------------------------------------------------------------------
//
// Every field read in the original is `REP MOVSD/MOVSB` from _G_LLM_NET_SEND_BUF + cursor into a
// frame local, followed by `ADD cursor,n`. memcpy is the same operation; the cursor arithmetic is
// spelled out at each call site rather than hidden, because several handlers advance it at a
// different point than they read (MSG_HORIZON writes straight into the global and only then adds 8).
template <typename T>
T take(const dispatch_state &ds, uint32_t &cursor) {
    T v;
    std::memcpy(&v, ds.packet.bytes + cursor, sizeof(T));
    cursor += sizeof(T);
    return v;
}

// ---- the two sweeps every departure handler runs -------------------------------------------------

// The "mark this player gone" triple: `AND byte [p],0xfb` / `OR byte [p],0x10` / `OR byte [p],0x08`,
// emitted as three separate read-modify-writes at nine sites. They are disjoint bits, so the order
// cannot matter; it is preserved because there is no reason to depart from it.
void mark_player_gone(const dispatch_state &ds, int32_t i) {
    ds.players_w[i].status_flags &= ~static_cast<uint32_t>(PLAYER_HUMAN);
    ds.players_w[i].status_flags |= PLAYER_DEFEATED;
    ds.players_w[i].status_flags |= PLAYER_GONE;
}

// "The session is over for everyone else": five sites run this eight-slot sweep over the same
// ALIVE && HUMAN && !local filter the barrier uses.
//
// `notify` is the ONE difference between them and it is not cosmetic: the four CONTROL handlers call
// llm_strat_player_presence_lost(i, 1) per eliminated slot, and the unknown-tag default (0x0049d29e)
// deliberately does NOT -- it only edits the flags. Collapsing the two would either fire eight
// spurious presence callbacks on a garbled packet or swallow them on a real teardown.
void eliminate_other_humans(const engine_state &st, const dispatch_state &ds,
                            const dispatch_calls &calls, bool notify) {
    for (int32_t i = 0; i < MAX_PLAYERS; ++i) {
        if (!detail::participates(st, i)) continue;
        mark_player_gone(ds, i);
        if (notify) calls.presence_lost(static_cast<uint32_t>(i), 1);
    }
}

// The tail of the CTL_PLAYER_LEFT / CTL_DROP_* handlers when we were the LAST peer standing: tell
// the game the player is gone, re-point the chat target, and abandon the drain. Written once because
// three handlers end this way byte for byte. (The original ends it with llm_teardown_hook_stub
// @0x0049bc44 as well -- an empty body, dropped at SIMABI-HOOKS; see turn_engine.h.)
void last_peer_teardown(const dispatch_state &ds, const dispatch_calls &calls,
                        int32_t player_idx) {
    calls.presence_lost(static_cast<uint32_t>(player_idx), 1);
    calls.chat_recalc_target_mode();
}

// ---- handlers ------------------------------------------------------------------------------------

// CTL_DROP_SYNCED (inner tag 8, 0x0049ca0a) and CTL_DROP_UNSYNCED (inner tag 9, 0x0049cc54) are 137
// and 136 instructions and are IDENTICAL except for one call. Factored into one body with the
// difference as a parameter precisely so that difference cannot be lost in a copy: tag 8 calls
// llm_net_lockstep_sync_busywait before llm_strat_time_resync_and_tick (0x0049cc45), tag 9 calls only
// the latter (0x0049ce8f).
detail::packet_result handle_peer_drop(const engine_state &st, const dispatch_state &ds,
                                       const dispatch_calls &calls, uint32_t &cursor,
                                       bool busywait_before_resync) {
    const int32_t side_id = take<int32_t>(ds, cursor);
    const double  horizon = take<double>(ds, cursor);
    const int32_t pidx    = calls.player_by_side_id(side_id);

    calls.format_player_line(TXT_PLAYER_DROPPED,
                             calls.ansi_to_wide_scratch(ds.players_w[pidx].name));
    calls.print_floating_msg_red(ds.text_scratch);
    *ds.status_flags &= static_cast<uint8_t>(~LS_HORIZON_PENDING);

    // The message names US. Nothing left to stay in step with: eliminate everyone and stop draining.
    if (side_id == *ds.local_player_index) {
        eliminate_other_humans(st, ds, calls, /*notify=*/true);
        // (empty llm_teardown_hook_stub_b @0x0049bc66 here in the original -- not reproduced)
        return detail::packet_result::stop;
    }

    mark_player_gone(ds, pidx);

    // `FLD horizon` / `FCOMP peer_horizon[pidx]` / `JZ`: an UNORDERED compare (either side NaN) sets
    // ZF and so counts as EQUAL, taking the "we agree" path. A plain `==` would take the disagree
    // path instead -- and the disagree path tears the whole session down, so getting this backwards
    // turns one NaN into a dropped match.
    if (!detail::x87_equal_or_unordered(horizon, st.peer_horizon[pidx])) {
        eliminate_other_humans(st, ds, calls, /*notify=*/true);
        // (empty llm_teardown_hook_stub_b @0x0049bc66 here in the original -- not reproduced)
        return detail::packet_result::stop;
    }

    // Horizons agreed. If anyone else is still playing, carry on a peer lighter.
    if (calls.count_active_players() > 1) {
        detail::commit_horizon(st);
        --*ds.lockstep_player_count;
        --*ds.lobby_scan_host_count;
        if (busywait_before_resync) calls.sync_busywait();
        calls.time_resync_and_tick();
        return detail::packet_result::drain_again;
    }
    last_peer_teardown(ds, calls, pidx);
    return detail::packet_result::stop;
}

// CTL_LEAVE_CONSENSUS (inner tag 6, 0x0049c804). A peer reports that `side_id` is leaving. We record
// that vote in the peer-state matrix and only actually remove the player once NO OTHER participating
// peer still has it marked PRESENT.
detail::packet_result handle_leave_consensus(const engine_state &st, const dispatch_state &ds,
                                             const dispatch_calls &calls, uint32_t &cursor,
                                             int32_t sender_idx) {
    const int32_t side_id = take<int32_t>(ds, cursor);
    const int32_t pidx    = calls.player_by_side_id(side_id);

    // The gate is an AND of two conditions written as two jumps (0x0049c842..0x0049c852): our own
    // column of that player's row must still be clear AND we must be in the pending-horizon state.
    if (st.peer_state[pidx * MAX_PLAYERS + *st.player_side] != 0 ||
        (*ds.status_flags & LS_HORIZON_PENDING) == 0)
        return detail::packet_result::drain_again;

    st.peer_state[pidx * MAX_PLAYERS + sender_idx] = PEER_STATE_LEAVING;

    // Starts TRUE and is cleared by any OTHER participating slot that still reads PRESENT. Note the
    // `pidx != j` guard (0x0049c8db): the departing player's own column never vetoes its removal.
    bool everyone_agrees = true;
    for (int32_t j = 0; j < MAX_PLAYERS; ++j) {
        const uint32_t f = ds.players_w[j].status_flags;
        if ((f & PLAYER_ALIVE) == 0 || (f & PLAYER_HUMAN) == 0) continue;
        if (st.peer_state[pidx * MAX_PLAYERS + j] != PEER_STATE_PRESENT) continue;
        if (pidx != j) everyone_agrees = false;
    }
    if (!everyone_agrees) return detail::packet_result::drain_again;

    calls.player_remove(side_id);
    calls.format_player_line(TXT_PLAYER_DROPPED,
                             calls.ansi_to_wide_scratch(ds.players_w[pidx].name));
    calls.print_floating_msg_red(ds.text_scratch);
    *ds.status_flags &= static_cast<uint8_t>(~LS_HORIZON_PENDING);
    --*ds.lockstep_player_count;
    --*ds.lobby_scan_host_count;
    calls.sync_busywait();
    calls.time_resync_and_tick();
    return detail::packet_result::drain_again;
}

// MSG_CHAT (outer tag 5, 0x0049d128). Length byte, recipient bitmask byte, then that many ANSI bytes.
detail::packet_result handle_chat(const engine_state &st, const dispatch_state &ds,
                                  const dispatch_calls &calls, uint32_t &cursor, int32_t sender_idx) {
    // The 0x3f8 recv cap bounds this: `len` is one byte, so at most 255 into a frame buffer the
    // original sizes at 0x400 (EBP+0xfffffb74 .. EBP+0xfffffc74). Reproduced with the same headroom.
    char text[0x100];

    const uint32_t n    = ds.packet.bytes[cursor++];
    const uint8_t  mask = ds.packet.bytes[cursor++];
    std::memcpy(text, ds.packet.bytes + cursor, n);
    cursor += n;
    text[n] = 0; // ONE byte, at [n] -- the string is ANSI here and only widened below

    const uint32_t addressed_to_us = static_cast<uint32_t>(mask) & (1u << *st.player_side);
    if (addressed_to_us == 0 && *ds.debug_tap_flag == 0) return detail::packet_result::drain_again;

    // The sender's name is widened into a FRAME buffer first, because the very next widen reuses the
    // shared scratch that llm_str_ansi_to_wide_scratch returns. Losing this copy would print the
    // message body as the speaker's name.
    wchar_t name[0x100];
    // Declared param order is (src, dst) while the STORAGE is src=EDX, dst=EAX -- custom storage, so
    // the generated wrapper places them correctly and the arguments here read (src, dst).
    calls.w_str_copy(calls.ansi_to_wide_scratch(ds.players_w[sender_idx].name), name);
    calls.format_chat_line(name, calls.ansi_to_wide_scratch(text));

    *st.floating_msg_active = 0;
    calls.print_floating_msg_cyan(ds.text_scratch);

    // Re-tested rather than reused (0x0049d202 recomputes `1 << PlayerSide` and re-reads the mask):
    // a message we only saw because of the debug tap also gets logged to the chat backlog.
    if ((static_cast<uint32_t>(mask) & (1u << *st.player_side)) == 0)
        calls.concat(ds.message_queue, ds.text_ptrs[TXT_CHAT_SUFFIX]);
    return detail::packet_result::drain_again;
}

// The unknown-outer-tag path (0x0049d22d). Abandons the rest of the buffer, warns, eliminates every
// other human WITHOUT a presence callback, and opens the end-of-game dialog. It does NOT stop the
// drain -- it falls into the same "next message" jump every other handler uses, and the drain ends
// only because the cursor has been walked to `len`.
void handle_garbled(const engine_state &st, const dispatch_state &ds, const dispatch_calls &calls,
                    uint32_t &cursor, uint32_t len) {
    // The original walks the remaining bytes one at a time computing `n % 10` and DISCARDING it
    // (0x0049d252: DIV EBX / TEST EDX,EDX with no consumer) -- a debug remnant. The only effect is
    // that the cursor ends at `len`. It backs the cursor up over the tag byte first, so a
    // zero-length remainder is still handled by the same loop.
    --cursor;
    while (cursor < len) ++cursor;

    *ds.status_flags |= LS_SESSION_ENDED;
    calls.format_plain_line(TXT_STREAM_GARBLED);
    *st.floating_msg_active = 0;
    calls.print_floating_msg_red(ds.text_scratch);
    eliminate_other_humans(st, ds, calls, /*notify=*/false);
    // (empty llm_teardown_hook_stub @0x0049bc44 here in the original -- not reproduced)
    calls.outcome_dialog(OUTCOME_NETWORK_ERROR);
    // LIB-ABI stage E hoist: outcome_dialog's GAME_MODE=3 @0x004c6cec is unconditional and
    // precedes every branch of the callee -- the one row with no predicate to latch.
    hoist_mode_set(calls.hoist, 3);
    // R3b: outcome_dialog's gated panel pair -- see overlay_hoist.h.
    mh::lockstep::hoist_outcome_panel(calls.hoist);
}

// ---- the inner (MSG_CONTROL) switch --------------------------------------------------------------
detail::packet_result dispatch_control(const engine_state &st, const dispatch_state &ds,
                                       const dispatch_calls &calls, uint32_t &cursor,
                                       int32_t sender_idx) {
    // The RAW tag is kept as well as the decremented selector: CTL_RESYNC_BEGIN stamps the raw value
    // into the order it synthesises (0x0049d034 reads the same local the selector was derived from).
    const uint8_t raw_tag = ds.packet.bytes[cursor++];
    const uint8_t sel     = static_cast<uint8_t>(raw_tag - 1);

    // Out of range is SILENTLY SKIPPED here (`JA 0x0049d123`), unlike the outer switch, which treats
    // an unknown tag as a garbled stream and tears the session down. Do not unify the two.
    if (sel > 0x0e) return detail::packet_result::drain_again;

    switch (static_cast<control_tag>(raw_tag)) {

        case CTL_PLAYER_LEFT: { // 0x0049c60c
            if (ds.players_w[sender_idx].status_flags & PLAYER_ALIVE) {
                mark_player_gone(ds, sender_idx);
                calls.format_player_line(TXT_PLAYER_LEFT,
                                         calls.ansi_to_wide_scratch(ds.players_w[sender_idx].name));
                calls.print_floating_msg_red(ds.text_scratch);
            }
            if (calls.count_active_players() > 1) {
                detail::commit_horizon(st);
                return detail::packet_result::drain_again;
            }
            last_peer_teardown(ds, calls, sender_idx);
            return detail::packet_result::stop;
        }

        case CTL_SESSION_ENDED: { // 0x0049c6b5
            *ds.status_flags |= LS_SESSION_ENDED;
            calls.format_player_line(TXT_SESSION_ENDED,
                                     calls.ansi_to_wide_scratch(ds.players_w[sender_idx].name));
            calls.print_floating_msg_red(ds.text_scratch);
            eliminate_other_humans(st, ds, calls, /*notify=*/true);
            // (empty llm_teardown_hook_stub @0x0049bc44 here in the original -- not reproduced)
            return detail::packet_result::drain_again;
        }

        case CTL_NOP: // 0x0049d123 -- the case body IS the jump to the next message
            return detail::packet_result::drain_again;

        case CTL_SLOT_RESET: { // 0x0049c9c7
            const int32_t side_id = take<int32_t>(ds, cursor);
            const int32_t pidx    = calls.player_by_side_id(side_id);
            *ds.status_flags &= static_cast<uint8_t>(~LS_HORIZON_PENDING);
            detail::reset_player_horizon(st, pidx);
            return detail::packet_result::drain_again;
        }

        case CTL_HORIZON_CHECK: { // 0x0049c786
            const int32_t side_id = take<int32_t>(ds, cursor);
            const double  horizon = take<double>(ds, cursor);
            const int32_t pidx    = calls.player_by_side_id(side_id);
            // `FCOMP` + `JNZ`: unordered counts as a MATCH and gets an ACK. `==` would send a desync.
            if (detail::x87_equal_or_unordered(st.peer_horizon[pidx], horizon))
                calls.send_horizon_ack(side_id);
            else
                calls.send_horizon_desync(side_id);
            return detail::packet_result::drain_again;
        }

        case CTL_LEAVE_CONSENSUS: // 0x0049c804
            return handle_leave_consensus(st, ds, calls, cursor, sender_idx);

        case CTL_STATUS_RESET_REQ: { // 0x0049c961 -- same two-part gate as CTL_LEAVE_CONSENSUS
            const int32_t side_id = take<int32_t>(ds, cursor);
            const int32_t pidx    = calls.player_by_side_id(side_id);
            if (st.peer_state[pidx * MAX_PLAYERS + *st.player_side] == 0 &&
                (*ds.status_flags & LS_HORIZON_PENDING) != 0) {
                calls.send_slot_reset(side_id);
                *ds.status_flags &= static_cast<uint8_t>(~LS_HORIZON_PENDING);
            }
            return detail::packet_result::drain_again;
        }

        case CTL_DROP_SYNCED: // 0x0049ca0a
            return handle_peer_drop(st, ds, calls, cursor, /*busywait_before_resync=*/true);

        case CTL_DROP_UNSYNCED: // 0x0049cc54 -- the ONLY difference is the missing busywait
            return handle_peer_drop(st, ds, calls, cursor, /*busywait_before_resync=*/false);

        case CTL_KICK: { // 0x0049ce99
            const int32_t side_id = take<int32_t>(ds, cursor);
            // player_by_side_id runs unconditionally and its result is UNUSED unless the kick names us.
            // Kept because it is an outward call, not a pure lookup we may elide.
            (void)calls.player_by_side_id(side_id);
            if (side_id != *ds.local_player_index) return detail::packet_result::drain_again;
            eliminate_other_humans(st, ds, calls, /*notify=*/true);
            // (empty llm_teardown_hook_stub_b @0x0049bc66 here in the original -- not reproduced)
            return detail::packet_result::stop;
        }

        case CTL_SET_STEP: // 0x0049cf69 -- straight into the global, no local
            std::memcpy(ds.lockstep_step_size_w, ds.packet.bytes + cursor, sizeof(double));
            cursor += sizeof(double);
            return detail::packet_result::drain_again;

        case CTL_SCALE_STEP: { // 0x0049cf94 -- factor FIRST, then the side_id
            const double  factor  = take<double>(ds, cursor);
            const int32_t side_id = take<int32_t>(ds, cursor);
            if (side_id == *ds.local_player_index) *ds.lockstep_step_size_w *= factor;
            return detail::packet_result::drain_again;
        }

        case CTL_RESYNC_BEGIN: { // 0x0049cffb
            game_order order;
            calls.fill_data(&order, sizeof(order), 0);
            std::memcpy(&order, ds.packet.bytes + cursor, sizeof(double)); // exec_time only
            cursor += sizeof(double);

            // 0x0049d034..0x0049d056, and the middle step is a genuine oddity worth spelling out: the
            // original writes the raw tag into param0 as a WORD, then re-reads that field as a DWORD
            // and stores its low word into order_code. Net effect: both fields get the tag.
            //
            // PRECISELY: the re-read's UPPER half is the old order_code, which fill_data has just
            // zeroed -- but only the LOW half is kept, and that is param0 either way. So the memset
            // makes the read well-defined without changing this field's observable value. The memset
            // still is not droppable: it is the only thing that zeroes args[], which nothing here
            // writes and which travels with the order into the pending queue. (An earlier version of
            // this comment claimed order_code itself depended on the memset; mutation-testing the
            // lockstest assertion showed it does not.)
            order.param0         = static_cast<int16_t>(raw_tag);
            order.order_code     = static_cast<uint16_t>(static_cast<uint32_t>(order.param0));
            order.owner_and_kind = 0xf0; // "global event" -- see the struct comment; param0 is its id
            order.unit_index     = 0;

            calls.order_pending_enqueue(&order);
            *ds.resync_in_progress = 1;
            return detail::packet_result::drain_again;
        }

        case CTL_RESYNC_END: // 0x0049d098
            // NET-SESSION, 2026-09-01: the five acts of this arm are now `resync_complete_local`,
            // because the LEADER performs the same transition inline in llm_wait_screen_frame and
            // one transition may not have two implementations (net_session.h carries the argument).
            // The body below is unchanged, moved -- the store order here is the one the shared
            // function implements.
            resync_complete_local({ds.status_flags, ds.resync_in_progress},
                                  {calls.mp_leave_reset_game_mode, calls.sync_busywait,
                                   calls.time_resync_and_tick, calls.hoist});
            return detail::packet_result::drain_again;

        case CTL_PEER_HORIZON: { // 0x0049d0ba -- UNREACHABLE in the shipped image; see turn_engine.h
            const double  horizon = take<double>(ds, cursor);
            const int32_t marker  = take<int32_t>(ds, cursor);
            detail::record_peer_horizon(st, sender_idx, horizon, marker);
            return detail::packet_result::drain_again;
        }
    }
    return detail::packet_result::drain_again;
}

} // namespace

namespace detail {

packet_result dispatch_packet(const engine_state &st, const dispatch_state &ds,
                              const dispatch_calls &calls, const reimpl_fixes &fx, int32_t sender_side_id,
                              uint32_t len) {
    const int32_t sender_idx = calls.player_by_side_id(sender_side_id);

    // 0x0049c311: a packet from a peer we have already written off. If we are the peer responsible
    // for saying so, re-broadcast the drop.
    //
    // AND THEN NOTHING ELSE HAPPENS. 0x0049c335 stores `cursor = len` -- which would skip the whole
    // packet -- and 0x0049c33b overwrites it with 0 two instructions later, unconditionally, on BOTH
    // paths. The store is dead. That is almost certainly a bug in the original (the intent reads as
    // "ignore this packet"), but it is the original's behaviour, so the packet IS parsed. Recorded
    // here so the next reader does not "restore" the skip and change what the game does.
    if (ds.players_w[sender_idx].status_flags & PLAYER_GONE) {
        if (calls.is_local_leader_peer(sender_side_id)) calls.send_peer_timeout_drop(sender_side_id);
    }

    uint32_t cursor = 0;
    // `CMP cursor,len` + `JC` -- an UNSIGNED compare, matching the unsigned emptiness test in the
    // wrapper. Both are modelled as uint32 rather than int32 on purpose.
    while (cursor < len) {
        const uint8_t sel = static_cast<uint8_t>(ds.packet.bytes[cursor++] - 1);
        if (sel > 4) {
            handle_garbled(st, ds, calls, cursor, len);
            continue;
        }

        packet_result r = packet_result::drain_again;
        switch (static_cast<outer_tag>(sel + 1)) {

            case MSG_ORDER: { // 0x0049c397
                // The order arrives whole, and its exec_time doubles as the sender's horizon.
                // ST5: decoded through the shared codec rather than memcpy'd, so a layout change
                // cannot update the wire's read side without also updating the save's.
                game_order order{};
                mh::orders::codec::decode(ds.packet.bytes + cursor, order);
                cursor += mh::orders::codec::RECORD_BYTES;
                calls.order_integrity_check(&order,
                                            ds.tag_netgame_read);
                if (*ds.status_flags & LS_HORIZON_PENDING) {
                    st.peer_horizon_pending[sender_idx] = order.exec_time;
                } else {
                    st.peer_horizon[sender_idx] = order.exec_time;
                    commit_horizon(st);
                }
                calls.order_pending_enqueue(&order);
                break;
            }

            case MSG_HORIZON: // 0x0049c469 -- copied STRAIGHT into the table, and the cursor advances
                              // once, after both branches (0x0049c4cd)
                if (*ds.status_flags & LS_HORIZON_PENDING) {
                    std::memcpy(&st.peer_horizon_pending[sender_idx], ds.packet.bytes + cursor, sizeof(double));
                } else {
                    std::memcpy(&st.peer_horizon[sender_idx], ds.packet.bytes + cursor, sizeof(double));
                    commit_horizon(st);
                }
                cursor += sizeof(double);
                break;

            case MSG_KEEPALIVE: { // 0x0049c4d6 -- the stall detector
                const int32_t side_id = take<int32_t>(ds, cursor);

                // -1 means "exclude nobody". Only the leader escalates to a full resync, and the
                // threshold compare is UNSIGNED (`JNC`), which is why both sides are cast.
                if (calls.is_local_leader_peer(-1)) {
                    // C3: the migrated `resync_trigger_gate`. OFF is the stock behaviour -- the original
                    // increments unconditionally, which is why a tight lookahead (where "at horizon" is
                    // the STEADY state) ratchets this cumulative counter to ACTIVE_PLAYERS*100 and
                    // force-fires a spurious resync every few seconds. ON counts only genuine sustained
                    // silence, the same predicate sync_overlay_show uses.
                    //
                    // Leader-local scratch: it gates only WHEN the leader emits its already-synchronised
                    // resync broadcast and feeds no per-peer sim computation, which is what makes the
                    // knob determinism-safe to differ between implementations.
                    const bool count_it =
                        !fx.resync_trigger_gate || *ds.sync_retry_countdown < SYNC_OVERLAY_AFTER;
                    // FIX AUDIT (C3's equivalence run) -- tally the DECISION, not its downstream
                    // effect. On a healthy LAN the gate suppresses every increment, so
                    // RESYNC_TRIGGER_COUNT stays 0 whether this branch ran thousands of times or
                    // never ran at all; only the eval count distinguishes those.
                    if (ds.audit_gate_evals) {
                        ++*ds.audit_gate_evals;
                        if (count_it && ds.audit_gate_allowed) ++*ds.audit_gate_allowed;
                    }
                    if (count_it) ++*ds.resync_trigger_count;
                    if (static_cast<uint32_t>(*ds.active_player_count * 100) <
                        static_cast<uint32_t>(*ds.resync_trigger_count))
                        calls.force_resync();
                }

                if (side_id != *ds.local_player_index) break;

                ++*ds.stall_nag_count;
                ++*ds.stall_count;
                // `LEA EAX,[EAX+EAX*4]` then `CMP` + `JA`: the store runs when count*5 <= stall_count.
                if (static_cast<uint32_t>(*ds.active_player_count * 5) <=
                    static_cast<uint32_t>(*ds.stall_count))
                    *ds.adapt_next_time = *st.game_clock;

                // EXACTLY five, not five-or-more (`CMP ...,0x5` + `JNZ`): the emergency bump fires once
                // on the fifth consecutive nag and then never again until the counter is reset elsewhere.
                if (*ds.stall_nag_count == 5) {
                    *st.horizon = *ds.lockstep_step_size_w * *ds.emergency_step_mul + *st.game_clock;
                    calls.send_lockstep_extend(*st.horizon);
                    commit_horizon(st);
                }
                break;
            }

            case MSG_CONTROL: // 0x0049c5d1
                r = dispatch_control(st, ds, calls, cursor, sender_idx);
                break;

            case MSG_CHAT: // 0x0049d128
                r = handle_chat(st, ds, calls, cursor, sender_idx);
                break;
        }

        if (r == packet_result::stop) return r;
    }
    return packet_result::drain_again;
}

} // namespace detail

const dispatch_calls &live_dispatch_calls() {
    static const dispatch_calls dc = {
        mh::host().transport_recv,
        MH_LIBMH_BIND(llm_strat_order_integrity_check),
        // STAYS ENTRY-ROUTED, and not by omission: llm_strat_order_pending_enqueue is a mh::orders
        // seam, and C8's direct-call rule is scoped to net + lockstep precisely because orders has a
        // real original to compare against. Same binding tx_emit_ctrl.cpp uses for this callee.
        &mh::call::llm_strat_order_pending_enqueue, // LIB-REF-SPLIT: NOT converted -- see below
        // THE ONE SITE MH_PROMOTED COULD NOT TAKE, and the type check is why. This slot is
        // `int32_t (*)(const game_order *)`, inherited from the generated prototype, while
        // mh::orders::pending_enqueue takes `order *` NON-const -- and that is the correct
        // signature, not an oversight: the body masks four ushort fields of *rec to their low
        // byte IN PLACE before copying, exactly as the original does at 0x004667b4-0x004667c6.
        // So the const on the slot is the wrong half, and fixing it changes a HOSTED
        // calls-struct member type -- which this item's acceptance puts out of bounds.
        // Left as the entry-routed thunk with the residue recorded rather than const_cast away.
        // ---- C8-d: the intra-closure edges. Ten of the 17, the largest group. ----
        MH_INTERNAL_CALL(llm_net_lockstep_is_local_leader_peer, mh::lockstep::is_local_leader_peer),
        MH_INTERNAL_CALL(llm_net_lockstep_force_resync, mh::lockstep::force_resync),
        MH_INTERNAL_CALL(llm_net_send_lockstep_extend, mh::lockstep::send_lockstep_extend),
        MH_INTERNAL_CALL(llm_strat_player_by_side_id, mh::lockstep::player_by_side_id),
        MH_INTERNAL_CALL(llm_net_lockstep_send_horizon_ack, mh::lockstep::send_horizon_ack),
        MH_INTERNAL_CALL(llm_net_lockstep_send_horizon_desync, mh::lockstep::send_horizon_desync),
        MH_INTERNAL_CALL(llm_net_lockstep_send_slot_reset, mh::lockstep::send_slot_reset),
        MH_INTERNAL_CALL(llm_net_send_lockstep_kick, mh::lockstep::send_lockstep_kick),
        MH_LIBMH_BIND(llm_strat_player_presence_lost),
        MH_INTERNAL_CALL(llm_net_lockstep_count_active_players, mh::lockstep::count_active_players),
        MH_INTERNAL_CALL(llm_net_player_remove, mh::lockstep::player_remove),
        MH_INTERNAL_CALL(llm_net_lockstep_sync_busywait, mh::lockstep::sync_busywait),
        MH_LIBMH_BIND(llm_strat_time_resync_and_tick),
        mh::state::evt::mp_leave_reset_game_mode,
        // SIMABI-CHAT 2026-09-10: INTERNALIZED. Was mh::host().llm_ui_chat_recalc_target_mode -- the
        // entry left the sim host table when we took the body (lockstep/lt_chat_ally_mask.cpp), which
        // is what dissolves its R3b readback: the writer and lt_chat_ally_mask's reader are now one
        // synchronous call graph. Binds directly, the shape sim_diplomacy_set_relation.cpp:39 uses.
        &mh::lockstep::chat_recalc_target_mode,
        [](char *s) { return mh::host().ansi_to_wide_scratch(s); },
        MH_CRT(utils_w_str_copy),
        MH_CRT(utils_concat),
        MH_CRT(utils_fill_data),
        mh::state::evt::outcome_dialog_i32,
        mh::state::evt::text_float_red,
        mh::state::evt::text_float_cyan,
        live_format_player_line,
        live_format_chat_line,
        live_format_plain_line,
        &live_overlay_hoist_ops(), // LIB-ABI stage E
    };
    return dc;
}

// The production entry point: the recv LOOP, which is the part that cannot be a pure function.
//
//   do { len = 0x3f8; recv(&sender, buf, &len); if (!len) return;
//        process the whole buffer;
//   } while (len && SESSION_MODE == 3);
//
// TWO THINGS THAT LOOK LIKE TYPOS AND ARE NOT:
//  * `len` IS UNSIGNED. The emptiness test is `CMP dword [len],0` + `JBE`, and the per-message bound
//    is `JC` -- both unsigned. A negative length from a failed recv would therefore be treated as an
//    enormous one by the original, and we reproduce that rather than quietly hardening it.
//  * The re-drain test re-reads `len`, which no handler writes, so it is only ever "did we receive
//    anything" -- the loop repeats because a single recv returns one datagram and there may be more
//    queued, and it stops as soon as one comes back empty or we have left lockstep.
void dispatch() {
    // C8-d: the liveness counter moved here from the promoted:: entry thunk -- see the long note at
    // turn_engine.cpp's production entry points. This function is called ONLY from inside the closure,
    // so after the direct-call conversion the entry-level counter read zero in a run where it plainly
    // ran every frame.
    promoted::live(6, "dispatch");
    const engine_state   &st = state();
    const dispatch_state &ds = dstate();
    const dispatch_calls &dc = live_dispatch_calls();

    for (;;) {
        // `len` is int32_t because llm_net_transport_recv's committed out-param is `int *`
        // (TACT1-P C6 -- the boundary carries the committed pointee). dispatch_packet still takes
        // the length unsigned; the recv contract never returns a negative.
        int32_t len    = static_cast<int32_t>(mh::net::packet_buffer::CAPACITY);
        int32_t sender = 0;
        dc.transport_recv(&sender, ds.packet.bytes, &len);
        if (len == 0) return;
        if (detail::dispatch_packet(st, ds, dc, fixes(), sender, static_cast<uint32_t>(len)) ==
            detail::packet_result::stop)
            return;
        if (*st.session_mode != SESSION_MP_LOCKSTEP) return;
    }
}

} // namespace mh::lockstep
