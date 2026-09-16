//
// lockstep/tx_emit.h -- five of the nineteen control-message emitters, reimplemented (RI-WIRE / L1).
//
// Scope: the five wire builders whose disassembly is the spec for this module --
//
//   llm_net_send_lockstep_extend            @0x0049d33b  (MSG_HORIZON, GUARDED)
//   llm_net_send_lockstep_keepalive          @0x0049d84c  (MSG_KEEPALIVE + the leader resync tail)
//   llm_net_lockstep_broadcast_player_leave  @0x0049dedf  (CTL_HORIZON_CHECK + an 8-slot local sweep)
//   llm_net_player_remove                    @0x0049dca3  (CTL_DROP_SYNCED + the removal tail)
//   llm_net_player_remove_timeout            @0x0049ddc1  (CTL_DROP_UNSYNCED + the SAME removal tail)
//
// EVERYTHING WAS TRANSLATED FROM tmp/decomp_wire/*_00*.asm, NOT from the decompile beside each --
// Ghidra's C for these five is a lossy draft (folded frame-local memcpy's with zero-length remainder
// copies it materialises as real statements, the two-part payload writes read as more calls than they
// are). Where the two disagree the .asm wins; see the per-function notes below for the specific spots.
//
// THE ONE TEMPLATE. All five build their record through mh::net::emit_ctrl (include/ctrl_emit.h) over
// the ONE shared buffer (include/packet_buffer.h) -- that generalisation is W2/W3, already landed. This
// header adds nothing to either; it only supplies the per-function state and outward calls.
//
// THE CURSOR GUARD IS PER-EMITTER, NOT A DEFAULT (the wire-emitter inventory "The cursor guard, and why it
// must not be normalised"). Of these five, `send_lockstep_extend` is the ONLY one that resets
// (`reset_first = true`); the other four are UNGUARDED (`reset_first = false`), which is the BENIGN
// form -- it appends after any pending order batch rather than discarding it. Do not "tidy" this.
//
// THE STACK PROBE IS DROPPED. Every one of these five opens with `PUSH frame_size; CALL
// assert_stack_capacity (0x004cf46f)` -- Watcom's stack-touch helper, no semantic effect, already the
// settled decision in ctrl_emit.h and the reimpl-loop skill's "settled once" list. Not reproduced, not
// re-litigated here.
//
// THE INLINED memcpy'S TRAILING `REP MOVSB` COPIES ZERO BYTES in every one of these five (the original
// splits `len` into a dword count via `SHR ECX,2` and a `len & 3` remainder via `AND CL,3`; every payload
// here is a multiple of 4 bytes, so the remainder is always 0) -- an artifact of the inlining, not a
// field. `emit_ctrl`'s single `std::memcpy` already reproduces the byte content correctly; there is
// nothing left over to translate.
//
// STRUCTURE matches the sibling batches in this module: logic lives in `detail::`, takes its state as a
// PARAMETER, and is bound to the live game only in the production wrapper -- see turn_engine.h's own
// preamble and rx_dispatch.cpp's for the rationale (a net_selftest.exe target can drive these five over
// heap buffers with no game and no rig).
//
#pragma once
#include <cstdint>

#include "include/ctrl_emit.h"
#include "lockstep/turn_engine.h"

namespace mh::lockstep {

// Everything these five functions read or write, as typed pointers -- the same idiom engine_state /
// dispatch_state / timekeeper_state use, and DELIBERATELY its own struct rather than a re-use of one of
// those three wholesale: every field below already has a same-address twin somewhere in this module
// (packet in dispatch_state, peer_horizon/peer_horizon_pending/peer_state in engine_state, status_flags
// in dispatch_state, peer_timeout_elapsed in timekeeper_state, players_w in dispatch_state,
// resync_trigger_count/active_player_count in dispatch_state) -- but pulling in three unrelated structs
// (twenty-plus fields with nothing to do with these five emitters, e.g. message_queue, text_ptrs,
// sync_retry_countdown) for the sake of five small functions would make this module depend on batch C
// and batch D's shape for no reason. So: a fourth, minimal, purpose-built view onto the SAME memory,
// exactly the "one region, several typed views, deliberately" pattern rx_dispatch.cpp already uses for
// _G_LLM_STRAT_PLAYERS (engine_state::players is const, dispatch_state::players_w is the writable
// alias of the identical array). Production binds every field here to the identical address its twin
// uses elsewhere in this module; a test binds all of them to one set of heap buffers, same as lockstest
// already does for players/peer_state.
struct emit_state {
    mh::net::packet_buffer packet; // _G_LLM_NET_SEND_BUF (0x005d55cc) + _CURSOR (0x005d59c4) -- the
                                   // SAME object dispatch_state::packet wraps; see include/packet_buffer.h.

    const double *peer_horizon; // _G_LLM_NET_PEER_HORIZON[8] (0x005d54cc) -- read-only here: the
                                // departing peer's own advertised horizon, carried in the
                                // broadcast_player_leave / player_remove / player_remove_timeout
                                // payloads. Same address as engine_state::peer_horizon.

    double *peer_horizon_pending; // _G_LLM_NET_PEER_HORIZON_PENDING[8] (0x005d550c) -- WRITTEN by
                                  // broadcast_player_leave's tail loop (reset to HORIZON_NONE for all
                                  // eight slots). Same address as engine_state::peer_horizon_pending.

    uint8_t *peer_state; // _G_LLM_NET_LOCKSTEP_PEER_STATE[8][8] (0x00e58bd8), row-major -- WRITTEN by
                         // broadcast_player_leave's tail. Same address as engine_state::peer_state.

    uint8_t *status_flags; // _G_LLM_NET_LOCKSTEP_STATUS_FLAGS (0x005d55b4) -- WRITTEN (OR
                           // LS_HORIZON_PENDING) by broadcast_player_leave's tail. Same address as
                           // dispatch_state::status_flags / timekeeper_state::status_flags.

    double *peer_timeout_elapsed; // _G_LLM_NET_LOCKSTEP_PEER_TIMEOUT_ELAPSED (0x00e587a1) -- WRITTEN
                                  // (zeroed) by broadcast_player_leave's tail. Same address as
                                  // timekeeper_state::peer_timeout_elapsed.

    const uint16_t *player_side; // PlayerSide (0x00e58354), read with MOVZX like every other consumer
                                 // in this module -- broadcast_player_leave uses it to find ITS OWN
                                 // column in the departing player's peer-state row. Same address as
                                 // engine_state::player_side.

    player_profile *players_w; // _G_LLM_STRAT_PLAYERS[8] (0x00cff060), stride 0x740 -- WRITABLE here:
                               // player_remove / player_remove_timeout edit status_flags directly. The
                               // SAME array engine_state::players (const) and dispatch_state::players_w
                               // alias, deliberately -- see rx_dispatch.cpp's comment on that pattern.

    int32_t *resync_trigger_count; // _G_LLM_NET_LOCKSTEP_RESYNC_TRIGGER_COUNT (0x00e58791) -- WRITTEN
                                   // (incremented) by the keepalive's split-out leader accounting.
                                   // Same address as dispatch_state::resync_trigger_count -- this is
                                   // the SEND-side twin of the RECV-side accounting rx_dispatch.cpp
                                   // already performs for an incoming MSG_KEEPALIVE (its own
                                   // `dispatch_state::resync_trigger_count`); the two are DIFFERENT
                                   // code paths in the original that happen to touch the same counter.

    // W5: the SENT-side resync_trigger_gate's predicate input. READ-ONLY here, exactly as it is in
    // dispatch_state -- llm_strat_time_tick owns this counter and the gate only ever reads it.
    const int32_t *sync_retry_countdown; // _G_LLM_NET_LOCKSTEP_SYNC_RETRY_COUNTDOWN

    const int32_t *active_player_count; // _G_LLM_NET_ACTIVE_PLAYER_COUNT (0x005d54c4) -- read-only
                                        // here, the threshold multiplicand. Same address as
                                        // dispatch_state::active_player_count.

    const double *horizon; // _G_LLM_STRAT_LOCKSTEP_HORIZON (0x005d5594) -- read-only, and read by
                           // NOTHING the original does in this module. It is here for the migrated
                           // `resync_order_horizon` fix (MP D14 / D17): the clamp needs the local
                           // horizon at the one choke point both the wire copy and the local order
                           // record pass through. Same address as engine_state::horizon and
                           // timekeeper_state::horizon.

    // MP D24: the other two inputs the same clamp needs, and for the same reason the horizon is here
    // -- nothing the ORIGINAL does in this module reads either. D17 clamped the synthetic order to
    // the bare horizon, which is the barrier every peer is ALREADY pinned at; clearing it takes a
    // step. See mh::fix::resync_order_barrier (fix/resync_clamp.h) for the argument.
    const double *game_clock; // _G_LLM_STRAT_GAME_CLOCK (0x005d0198). Same address as
                              // engine_state::game_clock and resync_state::game_clock.
    const double *step_size;  // _G_LLM_STRAT_LOCKSTEP_STEP_SIZE (0x005d55bc). Same address as
                              // dispatch_state::lockstep_step_size_w and resync_state::step_size.
};

// Outward calls. Every one of these is a function this task is NOT reimplementing -- either a plain
// original helper (player lookup, the count, the two UI/teardown calls) or `commit_horizon`, which IS
// already reimplemented in this same module (turn_engine.cpp, batch A) but as a function of its OWN
// `engine_state`, not this one. Rather than take a second, mostly-unrelated `engine_state` parameter
// just to call `detail::commit_horizon(st)` directly (the convention rx_dispatch.cpp uses for ITS
// commit_horizon call, where it already carries an `engine_state&` for other reasons), this module
// treats it uniformly with the other seven: a calls-struct entry. It is bound in production to the RAW
// `mh::call::llm_net_lockstep_commit_horizon` thunk -- see live_emit_calls() in the .cpp. The draft
// originally bound our own free wrapper for signature uniformity; that was CHANGED after W4.2,
// because the two bindings are not equivalent in every configuration. See the field comment below.
struct emit_calls {
    mh::net::transport_send_fn transport_send; // llm_net_transport_send @0x0049ba8d -- a STUB in the
                                               // retail image; our DLL replaces it (see ctrl_emit.h).

    int32_t (*player_by_side_id)(int32_t side_id); // llm_strat_player_by_side_id @0x0049e388

    int32_t (*count_active_players)(); // llm_net_lockstep_count_active_players @0x0049e3ea

    uint32_t (*presence_lost)(uint32_t player, uint32_t mode); // llm_strat_player_presence_lost
                                                               // @0x00498089

    void (*chat_recalc_target_mode)(); // llm_ui_chat_recalc_target_mode @0x0049d631

    // (llm_teardown_hook_stub @0x0049bc44 was a member here until SIMABI-HOOKS 2026-09-10: verified
    // EMPTY -- Watcom prologue+epilogue, nothing between -- so the call is gone and the sim host
    // table is one entry smaller. See turn_engine.h.)

    void (*commit_horizon)(); // the barrier recompute. Bound to the RAW address-0x0049c189 thunk
                              // (`mh::call::llm_net_lockstep_commit_horizon`), NOT to our own
                              // `mh::lockstep::commit_horizon`, even though the barrier IS already
                              // reimplemented in this module (turn_engine.cpp, batch A).
                              //
                              // WHY, and it is not style: the original's `CALL 0x0049c189` lands
                              // wherever the trampoline currently points, so it FOLLOWS the turn
                              // engine's promotion state on its own. Binding our C++ directly would
                              // make these emitters run OUR barrier even when the turn engine is
                              // NOT promoted -- coupling two seams that are meant to be
                              // independently switchable. That failure would surface as an
                              // unattributable red determinism run, not as a build error, which is
                              // the worst shape a bug can have here. rx_dispatch.cpp uses the raw
                              // thunk for the same reason.
                              //
                              // SUPERSEDED BY C8-d (2026-07-30) -- kept because the argument is
                              // right and only its PREMISE expired. C8-d folds `wire` into
                              // `lockstep` and drops per-seam subsets, so "the turn engine is NOT
                              // promoted while these emitters are" is no longer an expressible
                              // configuration and the coupling has nothing to couple. This slot,
                              // and every other intra-closure edge, is now bound through
                              // MH_INTERNAL_CALL (lockstep/internal_call.h): DIRECT by default,
                              // entry-routed again under MH_INTERNAL_EDGES_ENTRY_ROUTED=1.

    int32_t (*is_local_leader_peer)(int32_t exclude_side_id); // llm_net_lockstep_is_local_leader_peer
                                                              // @0x0049e653

    void (*force_resync)(); // llm_net_lockstep_force_resync @0x0049d9d6
};

emit_state        estate();
const emit_calls &live_emit_calls();

namespace detail {

// ---- 1. llm_net_send_lockstep_extend @0x0049d33b ---------------------------------------------------
//
// void __watcall(double horizon) -- the ONE stack-double argument, hence the original's `RET 0x8`
// (callee pops its own 8-byte stack arg). Builds MSG_HORIZON (2), no inner tag, payload = the 8-byte
// horizon verbatim, THEN sends. THE ONLY GUARDED EMITTER OF THE FIVE (`reset_first = true`,
// 0x0049d353..0x0049d35c) -- see the wire-emitter inventory; this is the LOSSY form, kept faithfully.
// No tail: nothing after the send in the original.
void send_lockstep_extend(const emit_state &es, const emit_calls &calls, double horizon);

// ---- 2. llm_net_send_lockstep_keepalive @0x0049d84c -------------------------------------------------
//
// void __watcall(EAX = side_id) -- plain `RET`, no stack cleanup (register arg only). Builds
// MSG_KEEPALIVE (3), no inner tag, payload = the 4-byte side_id, UNGUARDED, then sends.
//
// SPLIT IN TWO, as required by the task: 0x0049d84c is two things welded together in the original --
// the emit above, and a leader-only "should we force a resync" tail (0x0049d8bd..0x0049d8e0) that has
// NOTHING to do with sending a packet. Carved into its own function, `resync_trigger_tick`, so a later
// item can retire the byte patch that used to splice into 0x0049d8cb (the RECV-side twin of this same
// accounting already lives behind `reimpl_fixes::resync_trigger_gate` in turn_engine.h / rx_dispatch.cpp
// -- this SEND-side copy is the other half W5 names, and it could not be gated at all while it was
// fused into the emitter). This draft keeps it FAITHFUL/unconditional -- the original increments on
// every call where we are the leader, with no gate -- because applying the not-yet-landed fix is out of
// this task's scope; only the split is required here.
void resync_trigger_tick(const emit_state &es, const emit_calls &calls, const reimpl_fixes &fx,
                         gate_audit *audit);
void send_lockstep_keepalive(const emit_state &es, const emit_calls &calls, int32_t side_id,
                             const reimpl_fixes &fx, gate_audit *audit);

// ---- 3. llm_net_lockstep_broadcast_player_leave @0x0049dedf -----------------------------------------
//
// void __watcall(EAX = side_id) -- plain `RET`. Despite the name, ONE packet goes out (see
// The wire-emitter inventory): MSG_CONTROL (4) / CTL_HORIZON_CHECK (5), UNGUARDED, payload = side_id (4B) +
// _G_LLM_NET_PEER_HORIZON[idx] (8B), where idx = player_by_side_id(side_id) computed BEFORE any buffer
// write (0x0049defd) -- unlike player_remove/_timeout below, no reordering was needed here.
//
// THE TAIL is an 8-slot local sweep, run AFTER the send, that arms the grace-drop machinery:
//   for i in 0..7: peer_state[idx][i] = PEER_STATE_PRESENT (6); peer_horizon_pending[i] = HORIZON_NONE;
//   peer_state[idx][PlayerSide] = PEER_STATE_NONE (0);   // OUR OWN column in that row, cleared back
//   status_flags |= LS_HORIZON_PENDING (0x80);
//   peer_timeout_elapsed = 0.0;
// UNCERTAIN SEMANTICS, stated rather than hidden: the MECHANICAL effect (write these bytes to these
// addresses) is read directly off the disassembly and is not in doubt. The INTERPRETATION -- "mark the
// departing player PRESENT everywhere except our own vote, which starts undecided" -- is inferred from
// the constants' names elsewhere in this module (PEER_STATE_PRESENT is the value handle_leave_consensus
// treats as a veto) and from LS_HORIZON_PENDING's use in rx_dispatch.cpp (MSG_ORDER/MSG_HORIZON route
// into peer_horizon_pending while it is set), not verified against the receive side of THIS message.
void lockstep_broadcast_player_leave(const emit_state &es, const emit_calls &calls, int32_t side_id);

// ---- 4 & 5. llm_net_player_remove @0x0049dca3 / llm_net_player_remove_timeout @0x0049ddc1 -----------
//
// Both: void __watcall(EAX = side_id), plain `RET`. Build+send are BYTE-IDENTICAL except the inner tag
// (CTL_DROP_SYNCED=8 vs CTL_DROP_UNSYNCED=9); the tail that follows is INSTRUCTION-FOR-INSTRUCTION
// identical between the two (unlike rx_dispatch.cpp's handle_peer_drop, which factors CTL_DROP_SYNCED/
// _UNSYNCED with a `busywait_before_resync` bool because they genuinely differ by one call -- there is
// no such difference here to parameterize). So both build+send and tail are factored ONCE, called twice.
//
// send_removal_record: MSG_CONTROL (4) / inner_tag, UNGUARDED, payload = side_id (4B) +
// _G_LLM_NET_PEER_HORIZON[pidx] (8B). REORDERED from the original's instruction order: the original
// writes the header + side_id (0x0049dce2), THEN calls player_by_side_id (0x0049dd0f), THEN writes the
// horizon (0x0049dd17) -- three separate steps because it built the record incrementally. Computing
// pidx FIRST here (so the whole 12-byte payload can go through ONE emit_ctrl call) is an intentional
// reordering, not a spec change: player_by_side_id has no effect on the packet buffer, the cursor, or
// any state either function touches, so the two orders are observably identical -- same bytes, same one
// transport_send call, same length. Returns pidx for the tail.
//
// apply_removal_and_branch: the status-flags triple (AND 0xfb clear PLAYER_HUMAN, OR 0x10 PLAYER_
// DEFEATED, OR 0x08 PLAYER_GONE -- the exact same three bits, same order, as rx_dispatch.cpp's
// `mark_player_gone`, reused here by NAME via turn_engine.h's constants rather than by calling that
// function, which lives in rx_dispatch.cpp's anonymous namespace and is not exported), then branches on
// `count_active_players() > 1` (a SIGNED compare, `CMP EAX,1`/`JLE` -- unlike the keepalive's threshold
// compare, this one is signed in the original): more than one survivor -> commit_horizon(); one or
// fewer -> presence_lost(pidx, 1), chat_recalc_target_mode() (and, in the original, the empty
// teardown_hook_stub).
int32_t send_removal_record(const emit_state &es, const emit_calls &calls, int32_t side_id,
                            control_tag inner_tag);
void    apply_removal_and_branch(const emit_state &es, const emit_calls &calls, int32_t pidx);

void player_remove(const emit_state &es, const emit_calls &calls, int32_t side_id);
void player_remove_timeout(const emit_state &es, const emit_calls &calls, int32_t side_id);

} // namespace detail

// ---- production entry points (bound to the live game state) ----------------------------------------
void send_lockstep_extend(double horizon);
void keepalive_resync_accounting(); // the split-out (b) half of llm_net_send_lockstep_keepalive's tail
void send_lockstep_keepalive(int32_t side_id);
void lockstep_broadcast_player_leave(int32_t side_id);
void player_remove(int32_t side_id);
void player_remove_timeout(int32_t side_id);


// ---- the SENT-side gate's fix audit (RI-WIRE W5) --------------------------------------------------
// The mirror of dispatch_gate_audit(). Tally the DECISION, not its downstream effect: on a healthy
// LAN the gate suppresses every increment, so RESYNC_TRIGGER_COUNT stays 0 whether the predicate ran
// thousands of times or never ran at all. Only the eval count tells those apart -- which is the whole
// reason C3 could prove the RECV half, and the only way this half gets proved too.
gate_audit &emit_gate_audit();

// ---- promotion (RI-WIRE W4.4, folded into the lockstep closure by C8-d) ---------------------------
//
// `[promote] wire=1` and `wire_seams=` ARE RETIRED. W4.4 gave the emitters a key of their own so that
// "promoted emitters over an ORIGINAL turn engine" stayed expressible; C8-d makes the intra-closure
// edges DIRECT C++ calls (lockstep/internal_call.h), so that configuration no longer describes what
// executes, and the sixteen seams join `[promote] lockstep`. install_promotion REFUSES an ini that
// still carries either key rather than ignoring it -- a stale fragment must not read as a promoted
// run that quietly promoted less than it named.
//
// These are the one-line non-static wrappers turn_engine.cpp's single install table drives; the
// MH_EXPORT_REPLACE installers they forward to are `static` in tx_emit.cpp and cannot leave it. Same
// arrangement C8-c introduced for the resync seams, and for the same reason.
// clang-format off
bool install_seam_send_lockstep_extend();
bool install_seam_send_lockstep_keepalive();
bool install_seam_broadcast_player_leave();
bool install_seam_player_remove();
bool install_seam_player_remove_timeout();
bool install_seam_chat_send_team();
bool install_seam_chat_send_all();
bool install_seam_send_buf_flush();
bool install_seam_send_presence_lost();
bool install_seam_send_slot_reset();
bool install_seam_send_horizon_ack();
bool install_seam_send_horizon_desync();
bool install_seam_send_lockstep_kick();
bool install_seam_send_lockstep_step_size();
bool install_seam_broadcast_resync_state();
bool install_seam_send_lockstep_resync_resume();
// clang-format on

// ---- C8-d: the seam liveness counter, declared for the PRODUCTION ENTRY POINTS -------------------
//
// It used to be called only from the `promoted_wire::` entry-thunk wrappers, so it counted calls that
// ARRIVED AT THE ORIGINAL ENTRY ADDRESS. Once the intra-closure edges became direct C++ calls, a seam
// reached only from inside the closure stopped touching its entry -- and read zero in a run where it
// plainly ran. "READ THE LIVENESS LINES BEFORE BELIEVING A PASS" is the standing defence against the
// O3 failure (an asymmetric run that passed with the promotion never installed), and a counter that
// cannot tell "ran" from "was never armed" is not that defence. So the production entries call it,
// and the wrappers are pure forwards. Declared here because the entries live in sibling TUs.
namespace promoted_wire {
void live(int slot, const char *name);
}

bool wire_promotion_active();
// Per-seam call counts, in the wire seams' own order: slots 0..15 are the sixteen emitters and slot
// 16 is keepalive_resync_accounting, the separately-counted tail (see promoted_wire in tx_emit.cpp).
// -1 for an out-of-range slot. Exposed so a test or a run report can assert an emitter ACTUALLY
// FIRED rather than that a run was merely green.
long wire_promotion_calls(int slot);

} // namespace mh::lockstep
