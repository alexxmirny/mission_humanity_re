//
// lockstep/tx_emit.cpp -- the five emitters declared in tx_emit.h, reimplemented (RI-WIRE / L1).
//
// Translated from tmp/decomp_wire/llm_net_send_lockstep_extend_0049d33b.asm,
// tmp/decomp_wire/llm_net_send_lockstep_ack_0049d84c.asm (the function at that address is now named
// llm_net_send_lockstep_keepalive -- the .asm filename predates the rename),
// tmp/decomp_wire/llm_net_lockstep_broadcast_player_leave_0049dedf.asm,
// tmp/decomp_wire/llm_net_player_remove_0049dca3.asm and
// tmp/decomp_wire/llm_net_player_remove_timeout_0049ddc1.asm -- NOT from the decompile beside each,
// which folds every one of these functions' incremental frame-local memcpy's into more statements than
// instructions, and (for player_remove/_timeout) shows a THIRD zero-length memcpy per function that is
// the same REP MOVSB artifact ctrl_emit.h already documents.
//
#include "lockstep/tx_emit.h"
#include "lockstep/lt_chat_ally_mask.h"    // SIMABI-CHAT: the internalized chat_recalc_target_mode
#include "lockstep/lt_player_by_side_id.h" // LIB-TRANS-P direct edge

#include "lockstep/tx_emit_chat.h" // W6-A: the chat pair + the flush primitive
#include "lockstep/tx_emit_ctrl.h" // W6-B: the eight remaining MSG_CONTROL emitters

#include "addr/mh_addrs.gen.h"
#include "addr/mh_calls.gen.h"
#include "addr/mh_export.gen.h"
#include "lockstep/internal_call.h" // C8-d: MH_INTERNAL_CALL -- the intra-closure edges
#include "lockstep/resync.h"        // C8-d: count_active_players / is_local_leader_peer / force_resync
#include "lockstep/turn_engine.h"   // C8-d: commit_horizon, the production entry point

#include <windows.h>

#include <cstring>
#include "state/host_api.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::lockstep {

gate_audit &emit_gate_audit() {
    static gate_audit a;
    return a;
}

const emit_calls &live_emit_calls() {
    static const emit_calls ec = {
        // llm_net_transport_send takes `void *`; emit_ctrl's transport_send_fn is spelled over
        // `uint8_t *` (see ctrl_emit.h) -- a one-line adapter, not a behaviour change.
        [](uint8_t *buf, int32_t len) { mh::host().transport_send(buf, len); },
        MH_INTERNAL_CALL(llm_strat_player_by_side_id, mh::lockstep::player_by_side_id),
        MH_INTERNAL_CALL(llm_net_lockstep_count_active_players, mh::lockstep::count_active_players),
        MH_LIBMH_BIND(llm_strat_player_presence_lost),
        // SIMABI-CHAT 2026-09-10: INTERNALIZED -- see rx_dispatch.cpp's copy of this slot.
        &mh::lockstep::chat_recalc_target_mode,
        // DIRECT since C8-d, reversing W4.2's binding here -- and the reason the earlier decision
        // gave is worth reading, because it was correct at the time and it is its PREMISE that
        // C8-d removes, not its logic.
        //
        // W4.2 bound the RAW 0x0049c189 thunk so that `CALL 0x0049c189` would land wherever the
        // trampoline pointed and thus FOLLOW the turn engine's promotion state on its own. Binding
        // our C++ directly, it argued, would make these emitters run OUR barrier even in a run
        // where the turn engine was not promoted -- an asymmetry between two seams "supposed to be
        // independently switchable", surfacing as an unattributable red run rather than a build
        // error.
        //
        // C8-d retires that independence inside net + lockstep: `wire` folds into `lockstep`, the
        // per-seam subset goes, and the closure installs whole or not at all. With no configuration
        // left in which the turn engine is unpromoted while these emitters are promoted, the
        // coupling the raw thunk was avoiding cannot arise -- and the entry routing that expressed
        // it now costs a trampoline hop and blocks nothing. MH_INTERNAL_CALL keeps the old topology
        // one build flag away for a shadow campaign that needs it.
        MH_INTERNAL_CALL(llm_net_lockstep_commit_horizon, mh::lockstep::commit_horizon),
        MH_INTERNAL_CALL(llm_net_lockstep_is_local_leader_peer, mh::lockstep::is_local_leader_peer),
        MH_INTERNAL_CALL(llm_net_lockstep_force_resync, mh::lockstep::force_resync),
    };
    return ec;
}

namespace detail {

// ---- 1. llm_net_send_lockstep_extend @0x0049d33b ----------------------------------------------------
// NOTE ON `es.packet`: emit_ctrl takes a mutable `packet_buffer &`, but every function here takes
// `const emit_state &` (so a test can pass a `const` fixture). packet_buffer holds only the two
// POINTERS, not the buffer itself, so a local by-value copy aliases the identical memory -- mutating
// `*pb.cursor` through the copy is indistinguishable from mutating it through `es.packet`. Every
// call site below takes that local copy for exactly this reason; it is not a second buffer.
void send_lockstep_extend(const emit_state &es, const emit_calls &calls, double horizon) {
    mh::net::packet_buffer pb = es.packet;
    mh::net::ctrl_record   rec{};
    rec.outer       = static_cast<uint8_t>(MSG_HORIZON); // 2 -- 0x0049d366
    rec.has_inner   = false;
    rec.payload     = &horizon;
    rec.payload_len = static_cast<int32_t>(sizeof(double));
    rec.reset_first = true; // THE ONLY guarded emitter of the five -- 0x0049d353..0x0049d35c
    mh::net::emit_ctrl(pb, rec, calls.transport_send);
}

// ---- 2. llm_net_send_lockstep_keepalive @0x0049d84c -------------------------------------------------

// The split-out tail (0x0049d8bd..0x0049d8e0). EAX=-1 to is_local_leader_peer means "exclude nobody" --
// only the elected leader ever escalates. The threshold compare is UNSIGNED in the original (`IMUL
// EAX,[active_player_count],0x64` / `CMP EAX,[resync_trigger_count]` / `JNC done`, i.e. force_resync
// fires when resync_trigger_count, cast to a huge unsigned value if ever negative, exceeds the
// threshold) -- same shape, same casts, as rx_dispatch.cpp's RECV-side twin of this accounting.
void resync_trigger_tick(const emit_state &es, const emit_calls &calls, const reimpl_fixes &fx,
                         gate_audit *audit) {
    if (calls.is_local_leader_peer(-1) == 0) return;

    // W5: the migrated SENT-side `resync_trigger_gate`, the exact mirror of the RECV-side predicate
    // in rx_dispatch.cpp (C3). OFF is the STOCK behaviour -- the original increments unconditionally
    // (`INC dword ptr [0x00e58791]` @0x0049d8cb), which is why a tight lookahead, where "at horizon"
    // is the STEADY state, ratchets this cumulative counter to ACTIVE_PLAYERS*100 and force-fires a
    // spurious resync every few seconds. ON counts only genuine sustained silence, using the same
    // predicate sync_overlay_show uses.
    //
    // Determinism-safe to differ between peers for the same reason the RECV half is: leader-local
    // scratch that gates only WHEN the leader emits its already-synchronised resync broadcast, and
    // that feeds no per-peer sim computation.
    const bool count_it = !fx.resync_trigger_gate || *es.sync_retry_countdown < SYNC_OVERLAY_AFTER;

    // FIX AUDIT -- tally the DECISION, not its downstream effect. On a healthy LAN the gate
    // suppresses every increment, so RESYNC_TRIGGER_COUNT stays 0 whether this ran thousands of
    // times or never ran at all; only the eval count distinguishes those two. Telling them apart is
    // the entire evidence for this migration, because the rig cannot supply it: an idle LAN fires
    // ~zero resyncs, and this predicate only runs when a keepalive is actually emitted (1..99 times
    // in the 3000-step W4.6 run).
    if (audit) {
        ++audit->evals;
        if (count_it) ++audit->allowed;
    }
    if (!count_it) return;

    ++*es.resync_trigger_count;
    if (static_cast<uint32_t>(*es.active_player_count * 100) <
        static_cast<uint32_t>(*es.resync_trigger_count))
        calls.force_resync();
}

void send_lockstep_keepalive(const emit_state &es, const emit_calls &calls, int32_t side_id,
                             const reimpl_fixes &fx, gate_audit *audit) {
    mh::net::packet_buffer pb = es.packet; // see the note above send_lockstep_extend
    mh::net::ctrl_record   rec{};
    rec.outer       = static_cast<uint8_t>(MSG_KEEPALIVE); // 3
    rec.has_inner   = false;
    rec.payload     = &side_id;
    rec.payload_len = static_cast<int32_t>(sizeof(int32_t));
    rec.reset_first = false; // UNGUARDED -- the wire-emitter inventory row 2
    mh::net::emit_ctrl(pb, rec, calls.transport_send);
    resync_trigger_tick(es, calls, fx, audit);
}

// ---- 3. llm_net_lockstep_broadcast_player_leave @0x0049dedf -----------------------------------------
void lockstep_broadcast_player_leave(const emit_state &es, const emit_calls &calls, int32_t side_id) {
    // Computed BEFORE any buffer write in the original (0x0049defd) -- no reordering needed here.
    const int32_t idx = calls.player_by_side_id(side_id);

    // side_id (4B) then peer_horizon[idx] (8B), contiguous on the wire (0x0049df29.. / 0x0049df53..).
    // Built as raw bytes rather than a packed struct so there is no compiler-inserted padding between
    // an int32_t and a double (a naive {int32_t; double;} struct pads to a 4-byte gap at offset 4).
    uint8_t payload[12];
    std::memcpy(payload, &side_id, sizeof(int32_t));
    std::memcpy(payload + sizeof(int32_t), &es.peer_horizon[idx], sizeof(double));

    mh::net::packet_buffer pb = es.packet; // see the note above send_lockstep_extend
    mh::net::ctrl_record   rec{};
    rec.outer       = static_cast<uint8_t>(MSG_CONTROL); // 4
    rec.has_inner   = true;
    rec.inner       = static_cast<uint8_t>(CTL_HORIZON_CHECK); // 5
    rec.payload     = payload;
    rec.payload_len = static_cast<int32_t>(sizeof(payload));
    rec.reset_first = false; // UNGUARDED
    mh::net::emit_ctrl(pb, rec, calls.transport_send);

    // ---- the tail: LOCAL bookkeeping only -- one packet went out above, nothing more is sent ----
    // (0x0049dfa1..0x0049e00b). Mark idx's whole peer-state row PRESENT and park every peer's pending
    // horizon back to HORIZON_NONE, then clear OUR OWN column in that row back to NONE (see the
    // UNCERTAIN SEMANTICS note in tx_emit.h -- the writes below are read directly off the bytes; the
    // one-line gloss in this comment is the inferred reading, not a verified one).
    for (int32_t i = 0; i < MAX_PLAYERS; ++i) {
        es.peer_state[idx * MAX_PLAYERS + i] = PEER_STATE_PRESENT; // 0x0049dfc1: "presumed present"
        es.peer_horizon_pending[i]           = HORIZON_NONE;       // 0x0049dfce/d8
    }
    es.peer_state[idx * MAX_PLAYERS + *es.player_side] = PEER_STATE_NONE; // 0x0049dfea..dff3: our vote

    *es.status_flags |= LS_HORIZON_PENDING; // 0x0049dffa -- park incoming horizons until this resolves
    *es.peer_timeout_elapsed = 0.0;         // 0x0049e001/0x0049e00b -- (re)arm the 5s grace clock
}

// ---- 4 & 5. llm_net_player_remove / llm_net_player_remove_timeout -----------------------------------
int32_t send_removal_record(const emit_state &es, const emit_calls &calls, int32_t side_id,
                            control_tag inner_tag) {
    // REORDERED vs the original (see the long note in tx_emit.h): pidx computed first so the whole
    // 12-byte payload goes through one emit_ctrl call. player_by_side_id has no effect on the packet
    // buffer or any state this function touches, so this is observably identical to the original's
    // "write side_id, call player_by_side_id, write horizon" order.
    const int32_t pidx = calls.player_by_side_id(side_id);

    uint8_t payload[12];
    std::memcpy(payload, &side_id, sizeof(int32_t));
    std::memcpy(payload + sizeof(int32_t), &es.peer_horizon[pidx], sizeof(double));

    mh::net::packet_buffer pb = es.packet; // see the note above send_lockstep_extend
    mh::net::ctrl_record   rec{};
    rec.outer       = static_cast<uint8_t>(MSG_CONTROL); // 4
    rec.has_inner   = true;
    rec.inner       = static_cast<uint8_t>(inner_tag); // CTL_DROP_SYNCED (8) or CTL_DROP_UNSYNCED (9)
    rec.payload     = payload;
    rec.payload_len = static_cast<int32_t>(sizeof(payload));
    rec.reset_first = false; // UNGUARDED
    mh::net::emit_ctrl(pb, rec, calls.transport_send);
    return pidx;
}

// Byte-identical tail for both callers (0x0049dd65..ddb7 / 0x0049de83..ded5) -- see tx_emit.h for why
// this is factored once rather than parameterized the way rx_dispatch.cpp's handle_peer_drop is.
void apply_removal_and_branch(const emit_state &es, const emit_calls &calls, int32_t pidx) {
    // The status-flags triple: clear PLAYER_HUMAN, set PLAYER_DEFEATED, set PLAYER_GONE -- same three
    // bits, same order, as rx_dispatch.cpp's (anonymous-namespace, unexported) mark_player_gone.
    es.players_w[pidx].status_flags &= ~static_cast<uint32_t>(PLAYER_HUMAN);
    es.players_w[pidx].status_flags |= PLAYER_DEFEATED;
    es.players_w[pidx].status_flags |= PLAYER_GONE;

    // SIGNED compare (`CMP EAX,1` / `JLE`) -- more than one active survivor: fold the barrier back in.
    // One or fewer: this was the last-peer-standing drop, so end the session for real.
    if (calls.count_active_players() > 1) {
        calls.commit_horizon();
    } else {
        calls.presence_lost(static_cast<uint32_t>(pidx), 1);
        calls.chat_recalc_target_mode();
        // (empty llm_teardown_hook_stub @0x0049bc44 here in the original -- not reproduced)
    }
}

void player_remove(const emit_state &es, const emit_calls &calls, int32_t side_id) {
    const int32_t pidx = send_removal_record(es, calls, side_id, CTL_DROP_SYNCED);
    apply_removal_and_branch(es, calls, pidx);
}

void player_remove_timeout(const emit_state &es, const emit_calls &calls, int32_t side_id) {
    const int32_t pidx = send_removal_record(es, calls, side_id, CTL_DROP_UNSYNCED);
    apply_removal_and_branch(es, calls, pidx);
}

} // namespace detail

// ---- production entry points (bound to the live game state) ----------------------------------------
void send_lockstep_extend(double horizon) {
    // C8-d: the liveness counter is at the PRODUCTION ENTRY, not the entry thunk -- see the long
    // note at turn_engine.cpp's production entry points. A seam reached only from inside the
    // closure never touches its entry once the internal edges are direct.
    promoted_wire::live(0, "wire/send_lockstep_extend");
    detail::send_lockstep_extend(estate(), live_emit_calls(), horizon);
}

void keepalive_resync_accounting() {
    detail::resync_trigger_tick(estate(), live_emit_calls(), fixes(), &emit_gate_audit());
}

void send_lockstep_keepalive(int32_t side_id) {
    promoted_wire::live(1, "wire/send_lockstep_keepalive");
    detail::send_lockstep_keepalive(estate(), live_emit_calls(), side_id, fixes(), &emit_gate_audit());
}

void lockstep_broadcast_player_leave(int32_t side_id) {
    promoted_wire::live(2, "wire/broadcast_player_leave");
    detail::lockstep_broadcast_player_leave(estate(), live_emit_calls(), side_id);
}

void player_remove(int32_t side_id) {
    promoted_wire::live(3, "wire/player_remove");
    detail::player_remove(estate(), live_emit_calls(), side_id);
}

void player_remove_timeout(int32_t side_id) {
    promoted_wire::live(4, "wire/player_remove_timeout");
    detail::player_remove_timeout(estate(), live_emit_calls(), side_id);
}

// ==================================================================================================
// PROMOTION (RI-WIRE W4.4)
//
// The five emitters get their OWN gate -- `[promote] wire=1`, optionally narrowed by
// `wire_seams=` -- and deliberately do NOT join turn_engine.cpp's `lockstep=1` table.
//
// WHY A SEPARATE GATE. It is the same argument that moved emit_calls::commit_horizon onto the raw
// thunk: the emitters and the turn engine are meant to be INDEPENDENTLY switchable. Folding them
// into one key would make "promoted emitters over an ORIGINAL turn engine" unexpressible, and that
// configuration is exactly the bisect you need when an asymmetric run comes back red and you have
// to ask which half moved the state. One key per closure keeps the question answerable.
//
// The REFUSED semantics below are copied in spirit from turn_engine.cpp's install_promotion and
// exist for a recorded reason: an unknown token used to match nothing and silently shrink the
// install set, and an all-separator list used to select nothing -- both producing a run that READS
// as promoted and is not. Refusing loudly beats narrowing quietly.
// ==================================================================================================

namespace promoted_wire {

// One counter per emitter; the tail (`keepalive_resync_accounting`) is counted separately because it
// is separately promotable in principle and because W5 is going to care exactly how often it ran.
// 5 from W4 + 11 from W6 = 16, plus one spare slot. Sized by hand rather than derived, so adding a
// seam without adding a slot is a visible edit rather than a silent overwrite of a neighbour.
long g_calls[17];
bool g_any_installed = false;

// NOT `inline`: since C8-d the production entry points call this too, from this and sibling TUs.
void live(int slot, const char *name) {
    const long n = ++g_calls[slot];
    if (n == 1 || n == 100 || n == 1000 || n == 10000 || n == 100000)
        mh::lockstep::say("; [promote] %s: call #%ld (OURS is live)\n", name, n);
}

// clang-format off
void send_lockstep_extend(double h)             { mh::lockstep::send_lockstep_extend(h); }
void send_lockstep_keepalive(int32_t side)      { mh::lockstep::send_lockstep_keepalive(side); }
void broadcast_player_leave(int32_t side)       { mh::lockstep::lockstep_broadcast_player_leave(side); }
void player_remove(int32_t side)                { mh::lockstep::player_remove(side); }
void player_remove_timeout(int32_t side)        { mh::lockstep::player_remove_timeout(side); }

// ---- W6: the eleven remaining live emitters ----
// send_lockstep_resync_resume's EXPORTED signature carries a `double` because the original ends
// RET 0x8 (EN v165 prototype), but its body never reads it -- so this wrapper takes and discards
// the argument while our own entry point correctly has none.
void chat_send_team(void *text, uint32_t len)   { mh::lockstep::chat_send_team(text, len); }
void chat_send_all(void *text, uint32_t len)    { mh::lockstep::chat_send_all(text, len); }
int32_t send_buf_flush()                        { return mh::lockstep::send_buf_flush(); }
void send_presence_lost()                       { mh::lockstep::send_presence_lost(); }
void send_slot_reset(int32_t side)              { mh::lockstep::send_slot_reset(side); }
void send_horizon_ack(int32_t side)             { mh::lockstep::send_horizon_ack(side); }
void send_horizon_desync(int32_t side)          { mh::lockstep::send_horizon_desync(side); }
void send_lockstep_kick(int32_t side)           { mh::lockstep::send_lockstep_kick(side); }
void send_lockstep_step_size(double step)       { mh::lockstep::send_lockstep_step_size(step); }
void broadcast_resync_state(double exec_time)   { mh::lockstep::lockstep_broadcast_resync_state(exec_time); }
void send_lockstep_resync_resume(double)        { mh::lockstep::send_lockstep_resync_resume(); }
// clang-format on

} // namespace promoted_wire

// Instantiate the generated entry thunks + installers, exactly as turn_engine.cpp does for its nine.
// The macro defines a static mh_export_install_<fn>() in THIS translation unit, which is why these
// must sit here rather than in a header.
// clang-format off
MH_EXPORT_REPLACE(llm_net_send_lockstep_extend,            mh::lockstep::promoted_wire::send_lockstep_extend)
MH_EXPORT_REPLACE(llm_net_send_lockstep_keepalive,         mh::lockstep::promoted_wire::send_lockstep_keepalive)
MH_EXPORT_REPLACE(llm_net_lockstep_broadcast_player_leave, mh::lockstep::promoted_wire::broadcast_player_leave)
MH_EXPORT_REPLACE(llm_net_player_remove,                   mh::lockstep::promoted_wire::player_remove)
MH_EXPORT_REPLACE(llm_net_player_remove_timeout,           mh::lockstep::promoted_wire::player_remove_timeout)
MH_EXPORT_REPLACE(llm_net_chat_send_team,                  mh::lockstep::promoted_wire::chat_send_team)
MH_EXPORT_REPLACE(llm_net_chat_send_all,                   mh::lockstep::promoted_wire::chat_send_all)
MH_EXPORT_REPLACE(llm_net_send_buf_flush,                  mh::lockstep::promoted_wire::send_buf_flush)
MH_EXPORT_REPLACE(llm_net_lockstep_send_presence_lost,     mh::lockstep::promoted_wire::send_presence_lost)
MH_EXPORT_REPLACE(llm_net_lockstep_send_slot_reset,        mh::lockstep::promoted_wire::send_slot_reset)
MH_EXPORT_REPLACE(llm_net_lockstep_send_horizon_ack,       mh::lockstep::promoted_wire::send_horizon_ack)
MH_EXPORT_REPLACE(llm_net_lockstep_send_horizon_desync,    mh::lockstep::promoted_wire::send_horizon_desync)
MH_EXPORT_REPLACE(llm_net_send_lockstep_kick,              mh::lockstep::promoted_wire::send_lockstep_kick)
MH_EXPORT_REPLACE(llm_net_send_lockstep_step_size,         mh::lockstep::promoted_wire::send_lockstep_step_size)
MH_EXPORT_REPLACE(llm_net_lockstep_broadcast_resync_state, mh::lockstep::promoted_wire::broadcast_resync_state)
MH_EXPORT_REPLACE(llm_net_send_lockstep_resync_resume,     mh::lockstep::promoted_wire::send_lockstep_resync_resume)
// clang-format on

bool wire_promotion_active() { return promoted_wire::g_any_installed; }

long wire_promotion_calls(int slot) {
    return (slot >= 0 && slot < 17) ? promoted_wire::g_calls[slot] : -1;
}

// ---- C8-d: the sixteen wire seams joined the `[promote] lockstep` closure ------------------------
//
// `[promote] wire` is GONE, and with it install_wire_promotion, WIRE_SEAM_NAMES and the second
// subset parser. W4.4 gave the emitters their own key so that "promoted emitters over an ORIGINAL
// turn engine" -- the bisect a red asymmetric run needs -- stayed expressible. C8-d retires exactly
// that configuration: our promoted bodies now call each other DIRECTLY (internal_call.h), so a run
// in which one half is ours and the other is the original no longer describes what actually
// executes. One closure, one knob.
//
// WHAT REPLACES THE TABLE, and it is the C8-c pattern verbatim: MH_EXPORT_REPLACE defines a `static`
// installer in its OWN translation unit, so the macro and the thing that calls it cannot be split
// across files -- but the seam VOCABULARY must stay ONE list (turn_engine.h SEAM_NAMES) or a name
// becomes valid in the parser and unknown at the install site. So each seam gets a non-static
// one-line wrapper here, and turn_engine.cpp's SINGLE table drives them, keeping the ok/skipped
// accounting and the SHIP-vs-DIAGNOSTIC banner in one place.
//
// Each wrapper also maintains promoted_wire::g_any_installed, which install_wire_promotion used to
// set in one place at the end. wire_promotion_active() therefore keeps meaning exactly what it did.
namespace {
bool wire_seam(bool installed) {
    if (installed) promoted_wire::g_any_installed = true;
    return installed;
}
} // namespace

// clang-format off
bool install_seam_send_lockstep_extend()        { return wire_seam(mh_export_install_llm_net_send_lockstep_extend()); }
bool install_seam_send_lockstep_keepalive()     { return wire_seam(mh_export_install_llm_net_send_lockstep_keepalive()); }
bool install_seam_broadcast_player_leave()      { return wire_seam(mh_export_install_llm_net_lockstep_broadcast_player_leave()); }
bool install_seam_player_remove()               { return wire_seam(mh_export_install_llm_net_player_remove()); }
bool install_seam_player_remove_timeout()       { return wire_seam(mh_export_install_llm_net_player_remove_timeout()); }
bool install_seam_chat_send_team()              { return wire_seam(mh_export_install_llm_net_chat_send_team()); }
bool install_seam_chat_send_all()               { return wire_seam(mh_export_install_llm_net_chat_send_all()); }
bool install_seam_send_buf_flush()              { return wire_seam(mh_export_install_llm_net_send_buf_flush()); }
bool install_seam_send_presence_lost()          { return wire_seam(mh_export_install_llm_net_lockstep_send_presence_lost()); }
bool install_seam_send_slot_reset()             { return wire_seam(mh_export_install_llm_net_lockstep_send_slot_reset()); }
bool install_seam_send_horizon_ack()            { return wire_seam(mh_export_install_llm_net_lockstep_send_horizon_ack()); }
bool install_seam_send_horizon_desync()         { return wire_seam(mh_export_install_llm_net_lockstep_send_horizon_desync()); }
bool install_seam_send_lockstep_kick()          { return wire_seam(mh_export_install_llm_net_send_lockstep_kick()); }
bool install_seam_send_lockstep_step_size()     { return wire_seam(mh_export_install_llm_net_send_lockstep_step_size()); }
bool install_seam_broadcast_resync_state()      { return wire_seam(mh_export_install_llm_net_lockstep_broadcast_resync_state()); }
bool install_seam_send_lockstep_resync_resume() { return wire_seam(mh_export_install_llm_net_send_lockstep_resync_resume()); }
// clang-format on

} // namespace mh::lockstep
