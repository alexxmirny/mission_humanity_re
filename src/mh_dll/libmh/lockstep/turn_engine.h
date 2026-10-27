//
// lockstep/turn_engine.h -- the TURN ENGINE, reimplemented (RI-LOCKSTEP / L1).
//
// The second reimplementation module. Closure analysis is in the turn-engine notes; the operating
// procedure is the `reimpl-loop` skill. Scope is the IN-BINARY half of lockstep only -- the horizon
// barrier, the peer horizon table, and the mode-3 clock. Transport, discovery and pacing are already
// ours and live in mh/seams/net_lockstep.cpp; this module MEETS that seam rather than duplicating
// it, and it must not disturb the two settled pacing behaviours that live over there (the sim-relative
// adaptive floor and the +1 ms clock-quantum epsilon).
//
// THE TWO HORIZONS ARE DIFFERENT VARIABLES AND MIXING THEM UP IS THE WHOLE BUG CLASS:
//
//   _G_LLM_STRAT_LOCKSTEP_HORIZON    (0x5d5594) -- what THIS peer has requested/advertised.
//   _G_LLM_STRAT_LOCKSTEP_COMMITTED_HORIZON (0x5d558c) -- the BARRIER: min(own request, every other
//                                    active peer's advertised horizon). The sim may not pass it.
//   _G_LLM_NET_PEER_HORIZON[8]       (0x5d54cc) -- what each peer advertised, as the barrier sees it.
//   _G_LLM_NET_LOCKSTEP_PEER_TIMING[8] (0xe58c40) -- a SECOND per-peer record {order_marker, horizon}
//                                    fed by the RX path, used to decide whether the peers are on the
//                                    same order stream. NOT the barrier's input.
//
// STRUCTURE, and it is the same shape order_queue.h uses for the same reason: every function's logic
// lives in `detail::` and takes the state as a PARAMETER, so `net_selftest.exe lockstest` can run it
// over heap buffers with no game and no rig. The production wrappers bind that parameter to the live
// addresses. This is what buys T1 evidence for branches no scripted match reaches (a peer that never
// advertises, an all-agreeing marker set, the pending-horizon promotion on a slot reset).
//
#pragma once
#include "fix/resync_clamp.h" // MP D14/D24: the resync-order clamp + barrier (R4 -- shared with the net seam)
#include <cstdint>

#include "addr/mh_structs.gen.h"
#include "include/packet_buffer.h"

namespace mh::lockstep {

using peer_timing    = mh::game::mh_llm_net_lockstep_peer_timing; // 0xc: {int marker; double horizon}
using player_profile = mh::game::mh_llm_strat_player_profile;     // 0x740
using game_order     = mh::game::mh_llm_strat_order;              // 0x44, passed BY VALUE on the wire

inline constexpr int32_t MAX_PLAYERS = 8; // CMP [i],0x8 in every loop in this module

// llm_strat_player_profile.status_flags. Every barrier loop in this module tests the same pair with
// two separate `TEST byte ptr [...],imm` instructions (0x0049c1ce/0x0049c1de and friends): a player
// participates in the barrier only if it is ALIVE **and** HUMAN. An AI player is ALIVE but not HUMAN,
// so it never gates the horizon -- which is why an AI opponent costs nothing in lockstep.
inline constexpr uint32_t PLAYER_ALIVE = 0x02;
inline constexpr uint32_t PLAYER_HUMAN = 0x04;

// peer_timing_reset (0x0049ff34) and reset_player_horizon (0x0049e2ad) both write this bit pattern:
// low dword 0, high dword 0xbff00000 = -1.0. It means "nothing advertised yet", and it is a real
// value rather than a flag, so it participates in the comparisons like any other number.
inline constexpr double HORIZON_NONE = -1.0;

// Everything the turn engine reads or writes, as typed pointers. Bound to the live game in
// production (see state()), and to plain arrays in lockstest.
struct engine_state {
    double               *horizon;              // _G_LLM_STRAT_LOCKSTEP_HORIZON
    double               *committed;            // _G_LLM_STRAT_LOCKSTEP_COMMITTED_HORIZON
    double               *peer_horizon;         // _G_LLM_NET_PEER_HORIZON[8]
    double               *peer_horizon_pending; // _G_LLM_NET_PEER_HORIZON_PENDING[8]
    peer_timing          *peer;                 // _G_LLM_NET_LOCKSTEP_PEER_TIMING[8]
    uint8_t              *peer_state;           // _G_LLM_NET_LOCKSTEP_PEER_STATE[8][8], row-major
    const player_profile *players;              // _G_LLM_STRAT_PLAYERS[8]
    const uint16_t       *player_side;          // PlayerSide -- read with MOVZX, so 16-bit unsigned
    const double         *game_clock;           // _G_LLM_STRAT_GAME_CLOCK
    const int32_t        *player_count;         // _G_LLM_NET_LOCKSTEP_PLAYER_COUNT
    const double         *extend_margin_1;      // _G_LLM_NET_LOCKSTEP_EXTEND_MARGIN_1  (0.0 in every run)
    const double         *extend_margin_2;      // _G_LLM_NET_LOCKSTEP_EXTEND_MARGIN_2  (0.0 in every run)
    const double         *keepalive_margin_mul; // _G_LLM_NET_LOCKSTEP_KEEPALIVE_MARGIN_MUL (.rdata const)
    const double         *keepalive_step_mul;   // _G_LLM_NET_LOCKSTEP_KEEPALIVE_STEP_MUL   (.rdata const)

    // ---- batch B: the clock --------------------------------------------------------------------
    double       *game_clock_w;       // _G_LLM_STRAT_GAME_CLOCK again, writable (batch A only reads it)
    double       *game_time_delta;    // GAME_TIME_DELTA -- what one sim step advances the clock by
    const double *total_game_time;    // TOTAL_GAME_TIME -- the clock's target; in mode 3 it is already
                                      // clamped to the committed horizon by llm_strat_time_tick
    const double  *sim_step_interval; // _G_LLM_STRAT_SIM_STEP_INTERVAL -- the mode-3 sub-step
    const int32_t *session_mode;      // _G_LLM_GAME_SESSION_MODE (3 = MP lockstep)
    // The two order counts. Bound from mh::orders::pending_count()/staging_count() since ST1, NOT
    // from an address of our own: this module and mh::orders used to resolve the same two globals
    // independently, which is a region with two owners and exactly the arrangement that lets a later
    // island move break the consumer nobody remembered. The owner answers now.
    const int32_t *order_pending_count; // gates release_due
    const int32_t *order_staging_count; // gates schedule, in pump
    const double  *lockstep_step_size;  // _G_LLM_STRAT_LOCKSTEP_STEP_SIZE -- how far the pump extends
    const double  *pump_refill_frac;    // _G_LLM_NET_LOCKSTEP_PUMP_REFILL_FRAC (0.5 in the image)
    int32_t       *floating_msg_active; // _G_LLM_STRAT_FLOATING_MSG_QUEUE_ACTIVE
    void          *text_tmp;            // G_TEXT_TMP -- the wide buffer the alert line is built in
};

inline constexpr int32_t SESSION_MP_LOCKSTEP = 3; // CMP [_G_LLM_GAME_SESSION_MODE],0x3

// Outward calls. Batch A contributed one; batch B contributes the rest, and the split between "run
// it for real" and "bind an inert one" is decided per call by the side-effect rule -- a call is safe
// in the shadow arm exactly when its whole write-set is inside the declared region set.
//
//   send_lockstep_extend   ESCAPES (socket)                            -> inert
//   sim_step               11 direct regions + a ~686-function closure -> inert. Declaring that is
//                          not viable, and it is why sim_tick's verdict is about the CLOCK only.
//   ambient_tick           writes one region AND plays audio           -> inert
//   advisor_tick           writes five regions AND calls PrintTextMessage -> inert
//   print_text_message     UI queue                                    -> inert
//   order_release_due      writes four ORDER regions, all ordinary .bss, all DECLARED via
//                          extra_regions                               -> REAL (stubbing it would
//                          guarantee a false divergence, since those regions are compared)
//   invasion_alert_poll    PURE: its only call is assert_stack_capacity and it has no
//                          absolute-address store                      -> REAL, and it must be, since
//                          its return picks a branch
//   invasion_due_check     writes zero regions                         -> REAL
//   sprintf_text           writes an untracked text scratch buffer, idempotent for equal inputs -> REAL
struct game_calls {
    void (*send_lockstep_extend)(double horizon);
    void (*sim_step)();
    void (*ambient_tick)();
    void (*advisor_tick)(double now);
    void (*print_text_message)(void *text);
    int32_t (*order_release_due)(double now);
    int32_t (*invasion_alert_poll)(double now);
    // Returns the original's int flag (1 = an invasion fired this call, 0 = not due). Retyped from
    // `void (*)()` on 2026-08-31 when the Ghidra prototype was corrected to `int __watcall (void)`
    // -- both original callers discard it (0x0044d14c, 0x0044d187) and so does this engine; the
    // member's type only has to match mh::call::llm_strat_invasion_due_check's.
    int32_t (*invasion_due_check)();
    void (*sprintf_alert_text)(int32_t planet_index);
    // sim_clock_advance's three, none of which it can be shadowed with -- see its body.
    int32_t (*time_tick)();
    void (*reload_snapshot_resync_clocks)(double now);
    void (*clock_resync_units_and_buildings)(double now);
    // the pump's RX drain -- batch C, and an escape (it reads the socket and answers peers)
    void (*dispatch)();
    // the pump's own first statement, kept OUT of pump_after_schedule; see the wrapper
    int32_t (*order_schedule)();
};

// llm_strat_sim_clock_advance's constants, both read off the disassembly at 0x0044d238.
inline constexpr double  CATCHUP_LIMIT_SECS   = 300.0; // 0x4072c000_00000000
inline constexpr double  DELTA_DIVISOR        = 100.0; // DAT_00500f34
inline constexpr int32_t CLOCK_ADVANCE_CAPPED = 100;   // the 0x64 it returns when delta >= sub-step

engine_state      state();
const game_calls &live_calls();
const game_calls &inert_calls();

void set_logger(void (*fn)(const char *));
void say(const char *fmt, ...);

// ================================ batch C: the RX dispatcher =======================================
//
// llm_net_lockstep_dispatch @0x0049c2cd drains the socket and interprets everything that arrives. It
// is the ONE function in this module that cannot be shadowed at all, and the reason is structural
// rather than incidental: its input is a STREAM, and reading consumes it. The shadow arm would call
// recv() on an already-drained socket, process zero messages, and differ on every declared region on
// every call -- and stubbing recv inert gives the identical result, because the premise of shadow mode
// (both arms see the same inputs) is simply false here. See the turn-engine notes.
//
// So the split below is forced by the analysis, not chosen for taste: the recv LOOP lives in the
// production wrapper `dispatch()`, and everything else is `detail::dispatch_packet`, pure over an
// EXPLICIT buffer. That is strictly better evidence than a site would have given -- lockstest can feed
// crafted packets of all nineteen kinds, including the ones no live scenario produces (a resync
// demand, a presence loss, and the provably dead tag 0x0f).

// _G_LLM_NET_LOCKSTEP_STATUS_FLAGS bits, named from their set/clear sites in the dispatcher. Only
// these three are touched here; the semantics are inferred from behaviour, not from a symbol.
inline constexpr uint8_t LS_HORIZON_PENDING = 0x80; // a slot is resetting: park incoming horizons in
                                                    // _G_LLM_NET_PEER_HORIZON_PENDING instead of live
inline constexpr uint8_t LS_RESYNC_WAIT   = 0x40;   // cleared by CTL_RESYNC_END; never set here
inline constexpr uint8_t LS_SESSION_ENDED = 0x20;   // set by CTL_SESSION_ENDED and by an unknown tag

// The outer message tag, taken from the buffer and decremented before a `<= 4` bounds check, so tags
// 1..5 are the whole valid set and anything else takes the "garbled stream" default.
enum outer_tag : uint8_t {
    MSG_ORDER     = 1, // a 0x44-byte llm_strat_order, plus the sender's horizon in its exec_time
    MSG_HORIZON   = 2, // a bare 8-byte horizon advertisement
    MSG_KEEPALIVE = 3, // a stall ping; drives the resync trigger and the emergency horizon bump
    MSG_CONTROL   = 4, // the inner switch below
    MSG_CHAT      = 5, // length byte + recipient bitmask + ANSI text
};

// The inner (MSG_CONTROL) tag, same decrement-then-`<= 0xe` shape: 1..15 valid, anything else is
// silently skipped rather than treated as a garbled stream. Names are inferred from each handler's
// behaviour; the wire never spells them.
enum control_tag : uint8_t {
    CTL_PLAYER_LEFT   = 1,     // sender is gone; tear the session down if it was the last peer
    CTL_SESSION_ENDED = 2,     // host ended it: eliminate every other human
    CTL_NOP           = 3,     // no handler body at all -- straight to the next message
    CTL_SLOT_RESET    = 4,     // clear LS_HORIZON_PENDING and re-init one player's horizon row
    CTL_HORIZON_CHECK = 5,     // "is my horizon yours?" -> ack (6) or desync (7). The name is a
                               // shade too narrow, kept for continuity: its ONLY builder is
                               // llm_net_lockstep_broadcast_player_leave, which also marks all eight
                               // peer slots and zeroes the peer-timeout clock -- so this is a
                               // DEPARTURE PROPOSAL that CARRIES a horizon, and the horizon compare
                               // IS the consensus test. That is why the two replies read as an
                               // ack/desync pair from the sender's end and as leave-consensus /
                               // status-reset from the receiver's: same bytes, two ends. (RI-WIRE W3)
    CTL_LEAVE_CONSENSUS  = 6,  // a peer votes that `side_id` has left; remove once every peer agrees
    CTL_STATUS_RESET_REQ = 7,  // answer with a status reset if we are the one still pending
    CTL_DROP_SYNCED      = 8,  // peer dropped, horizons agreed -> busywait then resync the clock
    CTL_DROP_UNSYNCED    = 9,  // identical to 8 EXCEPT it does not busywait -- see the handler
    CTL_KICK             = 10, // if it names us, eliminate everyone else and return
    CTL_SET_STEP         = 11, // absolute lockstep step size
    CTL_SCALE_STEP       = 12, // multiply our step size, if it names us
    CTL_RESYNC_BEGIN     = 13, // synthesises a kind-0xf0 global-event order and sets RESYNC_IN_PROGRESS
    CTL_RESYNC_END       = 14, // clears it again
    CTL_PEER_HORIZON     = 15, // UNREACHABLE: its only builder, llm_net_send_lockstep_peer_horizon
                               // @0x0049e834, is called from nowhere and address-taken nowhere in the
                               // shipped EN image. Translated and tested anyway; see the turn-engine notes.
};

// RX_BUF_CAP used to be spelled here as a second 0x3f8. It is now mh::net::packet_buffer::CAPACITY --
// one constant, because it was always one buffer (RI-WIRE W2).
inline constexpr uint8_t PLAYER_GONE        = 0x08; // status_flags bits the "mark a player gone" triple
inline constexpr uint8_t PLAYER_DEFEATED    = 0x10; // sets, alongside clearing PLAYER_HUMAN
inline constexpr uint8_t PEER_STATE_LEAVING = 4;    // written into the peer-state matrix by CTL_LEAVE_CONSENSUS
inline constexpr uint8_t PEER_STATE_PRESENT = 6;    // the value that VETOES the removal in that handler

// The dispatcher's own globals. Deliberately a second struct rather than more fields on engine_state:
// these are RX-path bookkeeping, and several of them are the WRITABLE alias of something engine_state
// already exposes read-only (the barrier only reads the step size and the player count; the dispatcher
// assigns them).
struct dispatch_state {
    mh::net::packet_buffer packet;         // the shared region + its cursor; see include/packet_buffer.h.
                                           // This path uses it as a recv target and a parse source,
                                           // with its OWN local read offset -- packet.cursor counts
                                           // bytes queued for SENDING and is not touched here.
    uint8_t        *status_flags;          // _G_LLM_NET_LOCKSTEP_STATUS_FLAGS (byte-wide AND/OR only)
    player_profile *players_w;             // _G_LLM_STRAT_PLAYERS, writable: this path edits status_flags
    const int32_t  *local_player_index;    // _G_LLM_NET_LOCAL_PLAYER_INDEX -- compared against wire side_ids
    const int32_t  *active_player_count;   // _G_LLM_NET_ACTIVE_PLAYER_COUNT
    int32_t        *lockstep_player_count; // _G_LLM_NET_LOCKSTEP_PLAYER_COUNT, decremented on departure
    int32_t        *lobby_scan_host_count; // _G_LLM_NET_LOBBY_SCAN_HOST_COUNT, decremented with it
    double         *lockstep_step_size_w;  // _G_LLM_STRAT_LOCKSTEP_STEP_SIZE, set/scaled by CTL_SET_STEP
    double         *adapt_next_time;       // _G_LLM_STRAT_LOCKSTEP_ADAPT_NEXT_TIME
    int32_t        *resync_trigger_count;  // _G_LLM_NET_LOCKSTEP_RESYNC_TRIGGER_COUNT
    const int32_t  *sync_retry_countdown;  // _G_LLM_NET_LOCKSTEP_SYNC_RETRY_COUNTDOWN -- READ-ONLY here.
                                           // The RX path never writes it (time_tick owns it); dispatch
                                           // only reads it for the C3 resync_trigger_gate predicate.
    int32_t       *stall_nag_count;        // _G_LLM_NET_LOCKSTEP_STALL_NAG_COUNT
    int32_t       *stall_count;            // _G_LLM_STRAT_LOCKSTEP_STALL_COUNT
    int32_t       *resync_in_progress;     // _G_LLM_NET_RESYNC_IN_PROGRESS
    const int32_t *debug_tap_flag;         // _G_LLM_DEBUG_TAP_FLAG -- print chat not addressed to us
    const double  *emergency_step_mul;     // _G_LLM_STRAT_LOCKSTEP_EMERGENCY_STEP_MUL (.rdata)
    void          *message_queue;          // MESSAGE_QUEUE -- the wide chat log concat() appends to
    void *const   *text_ptrs;              // cfg_G_TEXT_PTRS -- only index 171 is read directly here

    // FIX AUDIT ([net] fix_audit), and it is DIAGNOSTIC state rather than game state -- a tally of how
    // many times the migrated `resync_trigger_gate` predicate was EVALUATED at this site and how many
    // of those it ALLOWED. It exists because the byte patch and this body are two implementations of
    // one fix, and the only honest cross-implementation comparison is their DECISION STREAM: the
    // downstream observable (RESYNC_TRIGGER_COUNT) is flat zero on a healthy LAN in both arms, which
    // is exactly what a branch that never ran also produces. C3's equivalence run reads these.
    // BOTH NULLABLE. The LIVE state wires them unconditionally (two adds per keepalive is not a cost
    // worth an ini-dependent code path); null is what lockstest and any caller that does not care
    // pass, and the ini key gates only whether the tally is REPORTED.
    int32_t *audit_gate_evals   = nullptr;
    int32_t *audit_gate_allowed = nullptr;

    // SB-BIND T4, APPENDED AT THE END ON PURPOSE. This struct is aggregate-initialized positionally
    // in lockstep_state.cpp and in lockstep_selftest.cpp's fixture, so a member inserted anywhere
    // above silently re-pairs every initializer after it -- SB-BIND T2 hit exactly that and only
    // caught it because the shifted types happened to disagree.
    //
    // G_TEXT_TMP -- the wide scratch the format_* calls write and the print_floating_msg_* calls
    // read. It used to be spelled as a registry lookup at each of the six use sites; it is state
    // this dispatcher uses, so it belongs in the state the dispatcher is handed.
    void *text_scratch = nullptr;
    // s_NetgameRead @0x005017a8, the ANSI tag passed to llm_strat_order_integrity_check for a
    // wire-sourced order. BOUND rather than spelled as a C++ literal (which is what the wide FORMAT
    // strings became): this pointer is consumed by an ORIGINAL function we have not translated, so
    // "it is only ever printed" is an assumption about code we do not own, and binding costs a
    // member.
    char *tag_netgame_read = nullptr;
};

// The live tally behind dispatch_state::audit_gate_*. Read by the net seam's fix-audit report; there is
// no reset, so a run's last line is its total.
struct gate_audit {
    int32_t evals   = 0;
    int32_t allowed = 0;
};
gate_audit &dispatch_gate_audit();

struct dispatch_calls {
    int32_t (*transport_recv)(int32_t *sender_out, void *buf,
                              int32_t *len); // wrapper only, never in the body
    void (*order_integrity_check)(const game_order *order, char *tag);
    int32_t (*order_pending_enqueue)(const game_order *order);
    int32_t (*is_local_leader_peer)(int32_t exclude_side_id);
    void (*force_resync)();
    void (*send_lockstep_extend)(double horizon);
    int32_t (*player_by_side_id)(int32_t side_id);
    void (*send_horizon_ack)(int32_t side_id);
    void (*send_horizon_desync)(int32_t side_id);
    void (*send_slot_reset)(int32_t side_id);
    void (*send_peer_timeout_drop)(int32_t side_id);
    uint32_t (*presence_lost)(uint32_t player, uint32_t mode);
    int32_t (*count_active_players)();
    void (*player_remove)(int32_t side_id);
    int32_t (*sync_busywait)();
    void (*time_resync_and_tick)();
    void (*mp_leave_reset_game_mode)();
    void (*chat_recalc_target_mode)();
    // The original also calls two EMPTY hook points here -- llm_teardown_hook_stub @0x0049bc44 and
    // llm_teardown_hook_stub_b @0x0049bc66, both verified body-free (Watcom prologue + epilogue and
    // nothing between). NOT reproduced since SIMABI-HOOKS 2026-09-10: calling an empty function and
    // not calling it are the same machine state, and they were the last two hook entries on the sim
    // host table. Each removed call site keeps a comment naming the address it stood at.
    void *(*ansi_to_wide_scratch)(char *src);
    void *(*w_str_copy)(void *src, void *dst); // NOTE the declaration order is (src,dst) while the
                                               // STORAGE is src=EDX, dst=EAX -- custom-storage, so the
                                               // generated wrapper already gets it right. Do not "fix".
    void *(*concat)(void *dst, void *src);
    void *(*fill_data)(void *ptr, uint32_t size, uint8_t fill);
    int32_t (*outcome_dialog)(uint8_t outcome);
    void (*print_floating_msg_red)(void *text);
    void (*print_floating_msg_cyan)(void *text);
    // The three w_sprintf shapes, folded into fixed-arity injectables for the same reason batch B
    // folded its alert line: w_sprintf is varargs, which gen_dll_calls refuses by design, and the
    // formatting needs G_TEXT_PTRS, which is not turn-engine state.
    void (*format_player_line)(int32_t text_id, void *wide_name); // "<label> <name>" -> G_TEXT_TMP
    void (*format_chat_line)(void *wide_name, void *wide_text);   // u"%s: %s"        -> G_TEXT_TMP
    void (*format_plain_line)(int32_t text_id);                   // "<label>"        -> G_TEXT_TMP
    // LIB-ABI stage E hoist ops -- tail member, null-skipped by suites (see timekeeper_calls).
    const struct overlay_hoist_ops *hoist;
    // mp:MP-LANG -- the received chat BODY is UTF-8: decode it into dst (cap_units UTF-16 units incl.
    // the NUL). Tail member like `hoist`: every offline suite leaves it null, and a null keeps the
    // original ansi_to_wide_scratch widen of the body (rx_dispatch.cpp handle_chat).
    void (*chat_to_wide)(const char *utf8, void *dst, uint32_t cap_units);
};

dispatch_state        dstate();
const dispatch_calls &live_dispatch_calls();

// ================================ batch D: the frame timekeeper ===================================
//
// llm_strat_time_tick @0x0043eea3 is the per-frame front of the turn engine, and it does three jobs
// that only share a function because they all key off one wall-clock read:
//
//   1. ADVANCE GAME TIME.   TOTAL_GAME_TIME += game_speed * (now - previous now). Every mode.
//   2. THE LOCKSTEP CLAMP.  In SESSION_MODE 3 only: if TOTAL_GAME_TIME has run past the COMMITTED
//      horizon, the overshoot is subtracted straight back off TOTAL_GAME_TIME *and* GAME_TIME_DELTA.
//      That discard is the measured source of the sub-1.0x MP sim rate (the MP latency notes).
//      While clamped it also runs the sync-wait state machine -- retry acks, the wait overlay, and
//      the 5-second peer-timeout drop.
//   3. THE GAME'S OWN ADAPTIVE CONTROLLER + the 20-slot FPS ring. Every 60 game-seconds it retunes
//      _G_LLM_STRAT_LOCKSTEP_STEP_SIZE by stall count, clamped to an FPS-relative floor.
//
// IT CANNOT BE SHADOWED, and this is a FOURTH way to be un-shadowable rather than a repeat of the
// other three (the turn-engine notes, and the shadow-oracle-limits ledger row):
//   * batch B's `pump` escapes through its outward calls;
//   * batch C's `dispatch` consumes a STREAM, so the second arm reads an empty socket;
//   * batch B's `sim_tick` is un-shadowable because an INSTRUMENT owns its entry;
//   * time_tick's very first statement is `GetCurrentTime()`. The wall clock is not restorable
//     state -- the shadow arm reads a LATER value by construction, so GAME_TIME_DELTA, TOTAL_GAME_TIME
//     and the FPS ring differ on every call no matter how faithful the body is. A site there would
//     report divergence forever and mean nothing.
// So the split below is forced, exactly as it was for dispatch: the production wrapper owns the clock
// read, and `detail::time_tick` is pure over an explicit `now`. That is what `lockstest` drives.
//
// IT IS PROMOTED BY REBIND, NOT BY AN ENTRY PATCH (C4, 2026-07-29 -- this paragraph previously said it
// was not promoted at all, and the reason it gave was right while the conclusion was too pessimistic).
// `mh/seams/net_lockstep.cpp` installs its OWN trampoline on this entry
// (`install_trampoline(ADDR_TIME_TICK, time_tick_detour, ...)`) in every MP run, because the shipped
// adaptive lookahead, the game_speed / sim_step pins, the rx_spin drain and U17's graceful-drop latch
// all hang off `on_time_tick`. Same instrument conflict as sim_tick, except here the instrument is
// OURS and it ships. The earlier reading of that was "promoting this seam means folding on_time_tick
// into the promoted body". It does not: the detour is a RUN-BEFORE trampoline that falls THROUGH to
// the original, so promotion only has to change what it falls through to. `on_time_tick` keeps running
// untouched, ahead of our body, exactly as it runs ahead of the original today. The instrument stays
// the owner of the entry and the implementation becomes its destination -- which is why a second entry
// patch here is refused by name (hook/promoted.h) rather than racing the trampoline.
struct timekeeper_state {
    // ---- 1. the clock ----
    double *last_game_time;          // LAST_GAME_TIME -- the stamp carried ACROSS frames. Read FIRST
                                     // (previous frame's value), then overwritten with `now`; the name
                                     // reads backwards but 0x0043eebb-0x0043eed0 is unambiguous.
    double       *current_game_time; // CURRENT_GAME_TIME -- a copy of it, consumed only by the FPS ring
    const double *game_speed;        // game_speed -- the multiplier on real elapsed time
    double       *game_time_delta;   // GAME_TIME_DELTA
    double       *total_game_time;   // TOTAL_GAME_TIME -- WRITABLE here; engine_state only reads it
    const double *committed;         // _G_LLM_STRAT_LOCKSTEP_COMMITTED_HORIZON -- the clamp target
    double       *horizon;           // _G_LLM_STRAT_LOCKSTEP_HORIZON -- what we advertise
    const double *game_clock;        // _G_LLM_STRAT_GAME_CLOCK
    const double *sim_step_interval; // _G_LLM_STRAT_SIM_STEP_INTERVAL -- the "is there a step's worth
                                     // of delta left after the clamp?" test, i.e. the STALL predicate
    const int32_t *session_mode;     // _G_LLM_GAME_SESSION_MODE

    // ---- 2. the sync-wait machine ----
    int32_t *sync_wait_active;             // _G_LLM_NET_SYNC_WAIT_ACTIVE
    int32_t *sync_retry_countdown;         // _G_LLM_NET_LOCKSTEP_SYNC_RETRY_COUNTDOWN, reloaded to 60
    int32_t *resync_trigger_count;         // _G_LLM_NET_LOCKSTEP_RESYNC_TRIGGER_COUNT -- time_tick
                                           // never touches it here (the retired resync_trigger_reset
                                           // fix used to, in the recovery branch).
    double         *sync_wait_elapsed;     // _G_LLM_NET_LOCKSTEP_SYNC_WAIT_ELAPSED, reloaded to 1.0
    double         *peer_timeout_elapsed;  // _G_LLM_NET_LOCKSTEP_PEER_TIMEOUT_ELAPSED, -1.0 = DISARMED
    const double   *peer_timeout_secs;     // _G_LLM_NET_LOCKSTEP_PEER_TIMEOUT_SECS (.rdata 5.0)
    uint8_t        *status_flags;          // _G_LLM_NET_LOCKSTEP_STATUS_FLAGS (LS_HORIZON_PENDING here)
    int32_t        *lockstep_player_count; // _G_LLM_NET_LOCKSTEP_PLAYER_COUNT
    int32_t        *lobby_scan_host_count; // _G_LLM_NET_LOBBY_SCAN_HOST_COUNT
    const int32_t  *local_player_index;    // _G_LLM_NET_LOCAL_PLAYER_INDEX
    int32_t        *stall_nag_count;       // _G_LLM_NET_LOCKSTEP_STALL_NAG_COUNT
    const uint8_t  *peer_state;            // _G_LLM_NET_LOCKSTEP_PEER_STATE[8][8], row-major
    const uint16_t *player_side;           // PlayerSide -- read with MOVZX, so 16-bit unsigned

    // ---- 3. the game's own adaptive controller + the FPS ring ----
    double         *lockstep_step_size;   // _G_LLM_STRAT_LOCKSTEP_STEP_SIZE -- WRITTEN here
    double         *adapt_next_time;      // _G_LLM_STRAT_LOCKSTEP_ADAPT_NEXT_TIME
    int32_t        *stall_count;          // _G_LLM_STRAT_LOCKSTEP_STALL_COUNT (zeroed each retune)
    const uint32_t *active_player_count;  // _G_LLM_NET_ACTIVE_PLAYER_COUNT -- the *5 grow threshold
    const double   *step_max;             // .rdata 3.0
    const double   *step_grow_mul;        // .rdata 1.1
    const double   *step_shrink_div;      // .rdata 1.1 -- a DIVISOR, not a reciprocal multiply
    const double   *step_min_fps_num_cmp; // .rdata: numerator of the floor in the COMPARE
    const double   *step_min_fps_num_set; // .rdata: numerator of the floor in the ASSIGN. Two
                                          // distinct slots, so they are two fields (see the body).
    const double *adapt_interval_secs;    // .rdata 60.0
    double       *fps_estimate;           // _G_LLM_STRAT_FPS_ESTIMATE
    double       *frame_time_ring;        // _G_LLM_STRAT_FRAME_TIME_RING[20]
    int32_t      *frame_time_ring_idx;    // _G_LLM_STRAT_FRAME_TIME_RING_IDX
    const double *fps_window_num;         // .rdata 20.0
};

inline constexpr int32_t FRAME_TIME_RING_SLOTS = 20;   // MOV EBX,0x14 / IDIV at 0x0043f3cf
inline constexpr int32_t SYNC_RETRY_RELOAD     = 0x3c; // 60, written at four separate sites
inline constexpr int32_t SYNC_OVERLAY_AFTER    = 0x38; // show the overlay once countdown < this
inline constexpr int32_t STALL_NAG_FLOOR       = 0x1e; // the nag counter decays only while >= 30
inline constexpr int32_t LEAVE_CONSENSUS_MIN   = 2;    // > 2 active players -> broadcast, else remove
inline constexpr uint8_t PEER_STATE_NONE       = 0;    // the value the timeout drop requires

// MP U20b -- reimpl_fixes::stall_icon_delay_ms's predicate. SYNC_WAIT_ELAPSED is reloaded to 1.0 when a wait
// ends and grows by GAME_TIME_DELTA (real dt x game_speed) on every parked frame, before the wait-overlay
// call reads it, so (elapsed - 1.0) / speed is the real wait in seconds. Fails OPEN (NaN, speed <= 0 -> show).
// net_lockstep.cpp's icon_delay_due is the retail carrier's copy of the same arithmetic.
inline bool stall_icon_due(double sync_wait_elapsed, double game_speed, int32_t delay_ms) {
    if (delay_ms <= 0) return true;
    if (!(game_speed > 0.0)) game_speed = 1.0;
    const double waited_ms = (sync_wait_elapsed - 1.0) * 1000.0 / game_speed;
    return !(waited_ms < static_cast<double>(delay_ms));
}

// Everything time_tick calls out to. `get_current_time` is listed for completeness but is used by the
// WRAPPER only -- if the body could call it, the split above would be pointless.
struct timekeeper_calls {
    double (*get_current_time)();
    // Returns int32_t, not void: SIM-READY (2026-08-07) found llm_net_lockstep_overlay_dismiss
    // declared `void` while its body loads a constant 1 into EAX before RET (MOV [EBP-0x18],1
    // @0x004c7dac). Every caller here discards it, as every caller in the original does -- the
    // type is corrected so the field cannot drift from mh_calls.gen.h again.
    int32_t (*overlay_dismiss)();
    void (*send_lockstep_extend)(double horizon);
    void (*commit_horizon)();
    int32_t (*find_horizon_match_side)();
    int32_t (*player_by_side_id)(int32_t side_id);
    void (*player_remove_timeout)(int32_t side_id);
    void (*player_remove)(int32_t side_id);
    void (*time_resync_and_tick)();
    // U20: "the game WANTED the de-sync icon this frame". Called UNCONDITIONALLY, immediately BEFORE
    // desync_icon_gate is applied -- which is the whole reason it is a slot and not a line inside the
    // wait_player_overlay_show wrapper. A gated call never reaches that wrapper, so counting only
    // there would collapse icon_calls onto icon_shown and destroy the very delta the gate exists to
    // make visible. Both counters therefore live on the IMPURE edge, where side effects belong, and
    // the body stays a pure function of (state, calls, fixes).
    //
    // It is a call slot rather than a global read for the same reason every other side effect here is
    // one: `lockstest` drives this body with a recording binding, so "did the body want an icon?"
    // has to be observable without the DLL's counters existing at all.
    void (*note_overlay_wanted)();
    int32_t (*wait_player_overlay_show)(int32_t player_idx);
    void (*send_lockstep_ack)(int32_t side_id);
    // Void since LIFT-SCREEN: the kick modal is a screen REQUEST, and its answer is read from the
    // overlay-result slot at the call site (hoist_overlay_result) rather than returned here.
    void (*sync_overlay_show)();
    int32_t (*is_local_leader_peer)(int32_t exclude_side_id);
    int32_t (*count_active_players)();
    void (*broadcast_player_leave)(int32_t side_id);
    // (llm_net_lockstep_hook_stub @0x0049bc88 was a member here until SIMABI-HOOKS 2026-09-10 --
    // verified EMPTY, so the call is simply gone; see timekeeper.cpp's self-match branch.)
    // The "<player> (<name>) left" red floating line, folded into ONE call for the same reason the
    // invasion alert was: formatting it needs G_TEXT_PTRS and the 0x740-stride player roster, and
    // neither belongs in this struct. Both of the original's two emit sites are identical.
    void (*print_player_left_alert)(int32_t player_idx);
    // LIB-ABI stage E: the hoisted GAME_MODE/overlay-latch writes (overlay_hoist.h). TAIL member
    // on purpose -- a suite's aggregate initializer zero-fills it and the null-tolerant wrappers
    // skip the hoist, matching how the suites already stub these host callees.
    const struct overlay_hoist_ops *hoist;
};

timekeeper_state        timekeeper();
const timekeeper_calls &timekeeper_live_calls();

// U20: wire the two de-sync-icon counters that `timekeeper_live_calls()`'s wrappers feed. The seam
// layer owns the counters (they are mh_lockstep.log columns and the retired byte thunk's asm writes
// the same two longs), and mh/lockstep must not depend on mh/seams -- so this is the same
// sink-injection shape as desync_watch's log pointer. Both may be null; unwired means uncounted.
void set_icon_counters(void (*wanted)(), void (*shown)());

// mp:X3c-FIX: the seam layer's floor for an outgoing MSG_HORIZON. Every horizon writer in this closure
// (time_tick's advertise_horizon, the nag bump, order stamps via commit) reaches the wire through
// send_lockstep_extend; while a world-resync catch-up MIRRORS, a writer computing clock + step from the
// rewound clock would advertise BEHIND what the peers hold. The sink returns the horizon to actually send
// (identity when not mirroring). Null = no floor (the default, and every selftest).
void set_extend_floor(double (*floor_fn)(double horizon));

// ---- C3: fixes that used to be BYTE PATCHES and now live in our bodies ---------------------------
//
// A faithful reimplementation reproduces the UNPATCHED behaviour BY CONSTRUCTION. That puts
// bit-equivalence and fix-carrying in direct tension, and promotion resolves it silently in favour of
// the bug: the DLL's byte patch lands inside a body we have JMP'd away, so the fix is inert while the
// arming line still says "armed". C1's interlock makes that loud; this struct is where it stops being
// true at all.
//
// Three properties are deliberate:
//   * EVERY MEMBER DEFAULTS OFF, and off means the faithful stock behaviour. A reimplementation whose
//     default differed from the original would make the asymmetric oracle meaningless.
//   * It is a PARAMETER, not a global the pure functions reach for. That is the whole reason lockstest
//     can drive both flag states of every fix; a hidden input would cost exactly that.
//   * Each member keeps the NAME AND MEANING of the ini key that used to gate the byte patch, so the
//     knob a player or a run sets means one thing whichever implementation is live.
//
// SEQUENCING RULE that comes with them: enabling a reimpl-side fix makes our promoted body DELIBERATELY
// differ from the original, so the ASYMMETRIC oracle must run with every such fix OFF ON BOTH PEERS.
// Otherwise it compares ours-with-fix against original-without-fix, every difference is expected, and
// a real divergence gets waved through.
struct reimpl_fixes {
    // [net] resync_trigger_gate -- count a stall-nag toward a forced resync only on GENUINE sustained
    // silence (SYNC_RETRY_COUNTDOWN < SYNC_OVERLAY_AFTER), not on the routine at-horizon nags that are
    // the steady state at a tight lookahead. Was the call-splice at 0x0049c508 (RECV) / 0x0049d8cb
    // (SENT); only the RECV site is inside a body we promote.
    bool resync_trigger_gate = false;

    // 2026-10-16: THIS FIELD IS NOW ALSO THE PRODUCT'S SINGLE-PLAYER FIXED STEP. `[net] sp_sim_step_ms` (default 20,
    // 0 = retail) sets it from seams/net_lockstep.cpp, and on_time_tick pins SIM_STEP_INTERVAL to that value in every
    // session mode but 3 -- one meaning whichever body is live (the retail carrier is the 2-byte NOP
    // `install_sp_fixed_step`). The "NEVER SHIPS ON" paragraph below predates that and is now only true of the ini key
    // `rig_fixed_step_loop` itself, which stays as the rig's way to force the loop with an arbitrary interval.
    // [net] rig_fixed_step_loop -- RIG KNOB, NOT A GAMEPLAY FIX, and the odd one out in this
    // struct: every other field restores behaviour a retired byte patch used to carry, while this one
    // deliberately makes the sim run a branch the game would not have taken. It forces sim_tick's
    // MODE-3 fixed-timestep catch-up loop in single-player (SESSION_MODE 2), so the SP determinism
    // oracle integrates at the same fixed `SIM_STEP_INTERVAL` as multiplayer instead of taking one
    // variable step per frame.
    //
    // WHY IT IS WORTH HAVING (measured 2026-08-01): in mode 2 the step SIZE
    // scales with game_speed, so a fast run is a different integration and its goldens are not
    // comparable with a 1x run's. In mode 3 `GAME_TIME_DELTA` is pinned to the sub-step, so game_speed
    // changes only how MANY steps the loop runs per frame -- same step sequence, denser in wall time.
    // That makes goldens speed-portable and lifts the one-step-per-frame ceiling.
    //
    // SAFE ON THE ORDER PATH IN THE SP ORACLE'S SCENARIO, and this was checked rather than assumed:
    // llm_strat_order_release_due 0x0046652e has exactly ONE caller (sim_tick's mode-3 branch), so
    // stock SP never calls it, and forcing the loop would start calling it. It cannot double-release
    // here because `order_pending` never changes across 15000 steps in the SP oracle -- nothing
    // reaches the pending lane. That is one scenario, not a general proof: a mode-1 campaign or a
    // different order flow could differ, so re-check before using this shape somewhere new.
    //
    // NEVER SHIPS ON. It changes the sim's integration; it exists so a rig run can trade SP fidelity
    // for MP-regime parity, and the stock mode-2 shape stays the coverage oracle (P0-SPDET exists
    // because the MP gate cannot see single-player paths -- and mode-2 sim_tick IS one of them).
    bool rig_fixed_step_loop = false;

    // [net] sp_sim_step_ms catch-up CAP: the most fixed steps one sim_tick call may run in single-player (0 = no cap).
    // Retail has no cap at all (a 20 s stall is ONE 20 s step); the fixed loop would run 1000 steps in one frame, so
    // after `sp_step_cap` steps the remainder of TOTAL_GAME_TIME is DROPPED (TOTAL := GAME_CLOCK) rather than carried.
    // Never applies in session mode 3 (the committed horizon already bounds it). One meaning with the retail
    // carrier (seams/net_lockstep.cpp sp_step_cap_stub).
    int32_t sp_step_cap = 0;

    // [net] desync_icon_gate -- MP U20: show the de-sync corner icon only on GENUINE sustained
    // silence (SYNC_RETRY_COUNTDOWN < SYNC_OVERLAY_AFTER), not on the routine at-horizon waiting that
    // is the steady state at a tight lookahead. Same predicate, and the same argument, as
    // `resync_trigger_gate` two fields up -- and the same predicate `sync_overlay_show`, one screen
    // below the gated call in timekeeper.cpp, is ALREADY gated on.
    //
    // SAFE TO GATE HERE AND NOT ONE SCREEN DOWN. The body DISCARDS wait_player_overlay_show's return
    // (timekeeper.cpp), so suppressing the call cannot change control flow -- it is display-only in
    // the strict sense. `sync_overlay_show()`'s result is assigned to `overlay_result`, which note (8)
    // branches on, so gating THAT would change behaviour. Do not generalise this fix across both.
    //
    // NOT A RETIRED BYTE PATCH, unlike every other non-rig field here -- and that is the one thing
    // about it that needs watching. `defang_tt_wait` (0 = stock / 1 = suppress / 2 = gate) was the
    // nearest ancestor and C8-e dropped it; the naked `wait_overlay_gate_thunk` that carried mode 2
    // survives as a PURE COUNTER with its gate hardcoded off. So there is no manifest entry to
    // derive this knob from, which is exactly the hole tools/test_ui.py's `migrated_fix_knobs()` had
    // to be widened to cover -- see MIGRATED_REIMPL_ONLY_KNOBS there. A default-ON fix shipped
    // through that hole would have silently compared ours-with-fix against original-without-fix.
    bool desync_icon_gate = false;

    // [net] overlay_dialog_guard -- MP U44: the stall overlay may only dismiss (and so only rewrite
    // GAME_MODE for) a screen lockstep itself armed, and the kick modal stashes -- rather than draws
    // over -- an open mode-3 dialog. Retail forces mode 2 on every dismiss whose widget list is empty,
    // which closes the building/sell dialog (it runs with an empty list) without clearing
    // _G_LLM_UI_ACTIVE_DIALOG; the kick modal then arms over the dialog and loses its clicks to it.
    // Default false here = the faithful-stock rule (fixtures); the shipped ini default is 1.
    // overlay_hoist.cpp reads it; so does the host sink's viewport-dirty mark.
    bool overlay_dialog_guard = false;
    // MP U44b: dev-only companion of the knob above (overlay_dialog_guard=2, the rc9.3 reproduction arm) --
    // drop the dismiss SAFETY NET, i.e. restore a foreign mode 3 even when no dialog / widget list is left.
    // Retail UI code (llm_menu_finish_enter_gameplay 0x004b82a8) arms the wait icon outside this ledger, so
    // without the net the next dismiss put mode 3 back over a closed dialog and input died. False = net on.
    bool overlay_guard_rc93 = false;

    // [net] stall_icon_delay_ms -- MP U20b: show the de-sync corner icon (wait_player_overlay_show) only once
    // the sim has been parked at the horizon this long: (SYNC_WAIT_ELAPSED - 1.0) / game_speed, the game's
    // own wait accumulator in real seconds. 0 = stock (every wanted frame). Display-only like
    // desync_icon_gate above (the call's return is discarded) and LOCAL (each peer may differ). The retail
    // carrier is net_lockstep.cpp's wait_overlay_gate_thunk; the initialiser is stock, the ini default 1000.
    int32_t stall_icon_delay_ms = 0;

    // [net] gone_peer_frame_guard -- MP U19e: keep the INCOMING datagram intact across the leader's
    // re-broadcast of a peer drop (rx_dispatch.cpp's dispatch_packet head, 0x0049c311), whose emitter
    // builds into _G_LLM_NET_SEND_BUF -- the very buffer the packet being dispatched lives in. Without
    // it the re-broadcast overwrites the head of that datagram, the parse loop walks off the end of the
    // record it just wrote, and an arbitrary payload byte is read as an outer tag -> handle_garbled ->
    // outcome 7 (NETWORK_ERROR). Measured on the rig on a perfectly clean 2-player quit; the long
    // comment at that site carries the dumped bytes.
    //
    // Born reimpl-only (the corruption exists in the original too -- the dead `cursor = len` store at
    // 0x0049c335 is the author's own aborted handling of it). Since mp:U19i an UNPROMOTED run carries
    // the same fix as a byte patch (mh/seams/gone_peer_guard.h, splice at 0x0049c330), so
    // configuration (1) has it too; this member is the promoted body's half. The ini default is 1, while
    // the initialiser here stays the faithful-stock value a pure caller or a lockstest fixture gets.
    bool gone_peer_frame_guard = false;

    // [net] diplo_order_dedup_fix -- MP U45: release_due's same-unit/same-owner supersede rule must not
    // treat the diplomacy admin orders 0xf4 (set_player_relation) and 0xf5 (set_player_control_mode)
    // as revisions of one another. A diplomacy Apply issues both with unit 0 and the issuer as owner
    // in one frame, so stock drops 0xf4 (0xf5 has the higher code) on every peer and the relation only
    // applies on a second Apply. Reimpl-only (no byte patch; under [config] mode=original the retail
    // rule runs). SIM-AFFECTING: every peer must run the same value -- it is a shipped default like
    // sim_step_ms, not negotiated. The initialiser is the faithful-stock value; the ini default is 1.
    bool diplo_order_dedup_fix = false;

    // [net] order_wide_index -- O1c: the scheduled order lane carries unit_index in 12 bits instead of
    // the vanilla 8 (orders/order_index_codec.h), so building 256..499 is addressable. A strict superset
    // of the vanilla encoding for every index < 256, hence DEFAULT TRUE (unlike the other members): the
    // wire and every hash are unchanged until an index reaches 256. false = the vanilla 8-bit lane.
    bool order_wide_index = true;

    // [net] undock_reentry_fix -- MP U49: the queue dispatcher drops an undock order (code 0x20) whose
    // unit is not PARKED at apply time. A second 0x20 applied a pass after the first (the lockstep
    // scheduling delay makes a double-click do this) re-enters exit_storage_begin on a unit already
    // walking out; can_exit refuses the door's own holder and the unit sits in EXIT_WAIT forever with
    // the door mutex held. SIM-AFFECTING: every peer must run the same value (shipped default 1, not
    // negotiated). Carried for the unpromoted dispatcher by a byte patch (net_lockstep.cpp,
    // install_undock_reentry_fix); this member is the promoted body's half. Initialiser = faithful stock.
    bool undock_reentry_fix = false;

    // [net] turret_reload_owner_fix -- PT-FIX1: building_tick skips llm_strat_bldg_turret_reload_tick unless
    // turrets[player][sub_id].b_index is the ticking building's own index. A destroyed turret's rubble
    // (state 4, up to 20 s) keeps its sub_id; its slot is freed (unmap_footprint) and re-used by the next
    // turret (map_CreateBuilding), and the rubble's growing tick budget then drains the new turret's
    // reload_timer every step (~4x fire rate). SIM-AFFECTING (shipped default 1, not negotiated). Byte
    // splice for the unpromoted tick: net_lockstep.cpp install_turret_reload_owner_fix. Initialiser = faithful stock.
    bool turret_reload_owner_fix = false;

    // [net] bldg_pip_guard -- hotfix rc9.4 (field crashes 0x0047908b / 0x004793e1): building_tick (before the
    // pip-anim gate) and update_charge_pips (on entry) zero a building's pip state when any pip_level is outside
    // 0..4 or pip_active_count outside 0..16, instead of indexing the 4-entry A_OGIEN table with it. SIM-AFFECTING
    // only on an already-illegal state (shipped default 1, not negotiated). Byte splices for the unpromoted bodies:
    // seams/bldg_pip_guard.cpp. Initialiser = faithful stock.
    bool bldg_pip_guard = false;

    // [net] turret_reload_realtime_fix + turret_fire_rate_mult -- PT-FIX2: building_tick hands the turret
    // reload tick dt * turret_fire_rate_mult (dt = GAME_TIME_DELTA, one step's real length) instead of the
    // re-counted budget, which at our 10/20 ms steps ran dt..K*dt and fired ~2x fast. SIM-AFFECTING
    // (shipped default 1 / 2.0; C_DEV, so a shipped build ignores an ini override and every peer carries the
    // same values). Byte splice for the unpromoted tick: net_lockstep.cpp install_turret_reload_splice.
    // Initialiser = faithful stock.
    bool   turret_reload_realtime_fix = false;
    double turret_fire_rate_mult      = 2.0;

    // [net] turret_clip_fix -- PT-FIX4: a new turret starts with ammo = Weapon.ammo and
    // resupply_cycles_remaining = -1, and the reload tick refills at ammo <= 0 (retail: never initialised,
    // refill only at exactly 0, so the clip / long reload are dead). SIM-AFFECTING, shipped default OFF
    // (balance change, C_DEV). Byte splices: net_lockstep.cpp install_turret_clip_fix. Initialiser = faithful stock.
    bool turret_clip_fix = false;

    // [net] turret_fire_poll_fix -- PT-FIX5: a ready turret already within the aim tolerance skips the aim loop of
    // llm_strat_bldg_state_turret_attack (whose passes cost 0.05 s of tick budget each) and fires on the step its
    // reload completes (retail: fire interval = reload + 0..~0.06 s). SIM-AFFECTING, shipped default OFF (user ruling:
    // a Gameplay-tab setting, C_DEV). Byte splice: net_lockstep.cpp install_turret_fire_poll_fix.
    bool turret_fire_poll_fix = false;

    // [net] air_order_facing_fix -- PT-FIX3: llm_strat_unit_state_move_path's ms==31 && order_queued early
    // return snaps an air unit's facing_target/facing_current to MOVE_MICROSTEPS[heading][31].facing when
    // they differ, so the dispatcher's turn-completion gate lets the queued order through (retail: the
    // helicopter is stuck forever, RE-MISC10). SIM-AFFECTING (shipped default 1, not negotiated). Byte
    // splice for the unpromoted body: net_lockstep.cpp install_air_order_facing_fix. Initialiser = faithful stock.
    bool air_order_facing_fix = false;

    // [net] project_gate_fix -- mods:A2: llm_progress_propagate_unlocks withholds a research PROJECT whose
    // depend[] are all acquired once the current planet's own invention is acquired by PlayerSide (every
    // conquered planet: "lab built, no research"). With the key on the gate is dropped. Campaign-only
    // (MP/skirmish sit on pseudo planet 0x1f, invention_index 0, never acquired: gate open either way), so
    // NOT host-synced. SHIPPED DEFAULT OFF. Byte splice for the unpromoted body: net_lockstep.cpp
    // install_project_gate_fix. Initialiser = faithful stock.
    bool project_gate_fix = false;

    // [net] mother_free_build / mother_free_build_ai -- mods:A1: the primary-mother gate of the build sites reads OPEN
    // (game_SetEvent x4, llm_ui_bldg_panel_open's page, try_begin_placement, order 0x19) and a power plant roots the network
    // (link_to_network, recompute seed) while its owner has no primary mother on the planet. The _ai flag extends it to
    // computer players (ai_enabled != 0). Host-set (F_HOSTSYNC), SIM-AFFECTING, shipped default OFF. Byte splices for the
    // unpromoted bodies: net_lockstep.cpp install_mother_free_build. Initialiser = faithful stock.
    bool mother_free_build    = false;
    bool mother_free_build_ai = false;

    // [net] move_attack_heading_fix -- mods:M1X: llm_strat_pathfind_build_steps (the greedy path of a single-unit attack
    // wave) records the heading it actually stepped (the -3/+3 turn candidate) instead of the blocked direct heading, so
    // the unit no longer walks into the building it was routed around and gives up. Host-set (F_HOSTSYNC, dev key under
    // [net] improved_movement), SIM-AFFECTING. Retail carrier: move_proto_blocked.cpp's runtime-flipped byte at 0x0041ec1f.
    // Initialiser = faithful stock.
    bool move_attack_heading_fix = false;

    // [net] move_squad_merge_fix -- mods:M1C: llm_strat_unit_state_squad_merge accepts a target soldier that is firing
    // (state 0x1a/0x1b/0x1c) and settled on its tile when the merge would otherwise succeed: the target is released
    // (target_release_ref mode 1) and put idle first. Host-set (F_HOSTSYNC, dev key under [net] improved_movement),
    // SIM-AFFECTING. Retail carrier: combat_proto.cpp's splice on the state compare at 0x0047ebe1. Initialiser = stock.
    bool move_squad_merge_fix = false;
    // [net] move_merge_goal_fix -- mods:M1X: llm_strat_group_move_order_pathfind step 2 relocates a squad merge's goal (the
    // target soldier's occupied tile) to the nearest REACHABLE tile next to it (sim/sim_merge_goal.h) instead of the first
    // closer tile of llm_strat_pathfind_find_closer_visible_tile's spiral. Retail carrier: move_proto_arrival.cpp 0x0041cc69.
    bool move_merge_goal_fix = false;
    // [net] projectile_facing_fix -- mods:M1C: llm_strat_projectile_tick's per-tick sprite re-aim uses a facing difference
    // wrapped into -12..11 (0 for weapon types 5/6, whose frame is sector-based) -- sim/sim_projectile_facing.h. Retail
    // carrier: combat_proto.cpp at 0x00441424.
    bool projectile_facing_fix = false;
    // [net] soldier_origin_anim_fix -- mp:DSHEAL: llm_strat_unit_soldier_get_sprite_screen_pos (the shot origin) reads the
    // RENDER-ONLY soldier anim_change_count; with the key on it reads as 0 (sim/sim_unit_soldier_screen_pos.h). Host-set
    // (F_HOSTSYNC, default ON), SIM-AFFECTING. Retail carrier: combat_proto.cpp at 0x00491166. Initialiser = stock (off).
    bool soldier_origin_anim_fix = false;

    // [net] attack_range_footprint -- mods:M1C: weapon range to a BUILDING is measured to the footprint tile nearest
    // the attacker instead of the centre (sim/sim_attack_range_footprint.h), in the attack-building issuer, the move
    // walker's path-end check, unit_fire_weapon's auto mode, attack_building's re-aim and group_step_ground. Host-set
    // (F_HOSTSYNC, dev key under [net] improved_combat), SIM-AFFECTING. Retail carrier: combat_proto.cpp's splices.
    // Initialiser = faithful stock.
    bool attack_range_footprint = false;

    // mods:M1H lane A -- the rest of the improved_movement / group_movement keys, plumbed to libmh so a libmh body can carry them (the
    // twins land in lanes B / C; sim/sim_move_fixes.h holds the values they read). Each is the host-synced value of the [net] key of the
    // same name (F_HOSTSYNC, dev keys under the launcher switches). They do NOTHING in libmh until a twin reads them, and the retail
    // splices in mh.dll are what carries a key when libmh is absent. The initialisers are the faithful stock: flags off, numeric knobs at
    // their retail values (route_cost_base/ring4/ring8/ring16 = 1/4/8/16), so a build that never calls set_fixes is the original.
    bool move_centroid_wrap_fix         = false;
    bool move_detour_slot_guard         = false;
    bool move_flood_queue_guard         = false;
    int  route_cost_mode                = 0;
    int  route_cost_base                = 1;
    int  route_cost_ring4               = 4;
    int  route_cost_ring8               = 8;
    int  route_cost_ring16              = 16;
    int  route_gap_wave_full            = 0;
    int  route_corridor_mode            = 0;
    bool route_flood_exact              = false;
    int  move_block_wait_ms             = 0;
    bool move_keep_sidestep             = false;
    int  move_repath_cooldown_ms        = 0;
    bool move_distinct_goals            = false;
    int  move_near_enough               = 0;
    bool move_group_speed               = false;
    bool move_nudge_idle                = false;
    bool move_claim_leader              = false;
    bool move_swap_headon               = false;
    bool move_route_through_idle        = false;
    bool move_route_through_moving      = false; // mods:M1N 3e-6: a walking friend's tile is open to the planner
    int  move_route_through_moving_cost = 0;     // ... priced at this cost level (0..4)
    bool move_route_through_replanning  = false; // mods:M1DT fix 2: a re-planning (state 0x0a) friend's tile is open to the planner too
    bool move_block_reset_moving        = false; // mods:M1DT fix 4a: the block-wait budget restarts when the blocker changes to a walking friend
    int  move_replan_detour_cap         = 0;     // mods:M1DT fix 4b: percent of the straight distance a block-wait replan may take (0 = off)
    bool move_enter_queue               = false; // mods:M1BE: ORDER 0x24 units wait behind an entering friend and ignore the block-wait / sidestep / cooldown keys
    bool move_queue_flank_guard         = false; // mp:U75: the unit-queue shuffle and the idle nudge take a diagonal step only past an open flank tile; the shuffle search is depth-capped
    bool storage_door_hold              = false; // mods:M1BE: an open storage door stays open while an entering friend is within 2 tiles
    int  move_formation_click           = 0;     // mods:M1G 4b: 0 off, 1..2 = the old keep rule (N - 1 rings of slack), 3 = the formation moves as one
    bool move_stream_heat               = false; // mods:M1WL v2: a lone unit of a stream (>= 8 groupmates walking) prices the tiles of its mates' next 120 path steps (sim/move/stream_heat.h)
    int  move_stream_heat_cost          = 2;     // mods:M1WL v3: the cost per priced tile, a legal value (0, 2, 32, 34 .. 226; sim/move/stream_heat.h sh_legal_cost)
    int  move_stream_heat_window        = 120;   // mods:M1WL v3: path steps of each walking mate that are priced (1 .. 255)
    int  move_stream_heat_min_mates     = 8;     // mods:M1WL v3: groupmates needed before the rule fires (1 .. 255)
    int  move_stream_heat_scale         = 1;     // mods:M1WL v3: 1 = the cost x min(mates on the tile, scale_cap) (on by default since v5)
    int  move_stream_heat_retrace       = 0;     // mods:M1WL v4: 1 = a heated plan's route is re-traced charging the tile stepped onto (sim/move/stream_heat.h sh_retrace)
    int  move_stream_heat_turn          = 0;     // mods:M1WL v4: the re-trace's penalty for a turn of more than 45 degrees (0 .. 64)
    int  move_stream_heat_scale_cap     = 16;    // mods:M1WL v5: the mate count at which the scaled cost stops growing (1 .. 255)
    int  move_stream_heat_margin        = 0;     // mods:M1WL v5: percent; a heated route longer than (100 + margin)% of the unheated one is replaced by it (0 = off)
    bool move_reverse_now               = false; // mods:M1G 4c: slide back at once when a new goal lies behind
    int  move_turn_rate                 = 0;     // mods:M1G 4c: walker turn speed x 2^k (0 retail, 0..2)

    // [net] player_left_pin_fix -- MP U56: a NATURAL elimination of a human is observed by every peer's
    // own sim at the same step (llm_strat_player_presence_lost(p, 0)), but the eliminated peer's
    // CTL_PLAYER_LEFT used to flip that player's HUMAN/DEFEATED/GONE flags AT RECEIVE TIME -- a different
    // sim step on each survivor (the other client hears it through the host's forward), and when it
    // landed after the sim's own presence_lost it did nothing at all, so the survivors ended on
    // different flags (0x1B vs 0x05) for the rest of the match. With the fix ON the flip is made by the
    // sim at the elimination step on every peer (presence_lost, mode 0, a human other than the local
    // side), and the receipt of CTL_PLAYER_LEFT from a still-ALIVE, not-yet-GONE sender leaves the flags
    // alone whenever another human would remain (the last-peer teardown keeps the retail receive-time
    // flip). Carried for the unpromoted bodies by three byte patches (net_lockstep.cpp,
    // install_player_left_pin_fix, mp:U66), so the knob means one thing in both configurations.
    // SIM-AFFECTING: every peer must run the same value -- a shipped default, not negotiated. The
    // initialiser is the faithful-stock value; the ini default is 1.
    bool player_left_pin_fix = false;

    // [net] spectate_after_defeat -- MP U54: a human eliminated while at least TWO other humans still play (and
    // the match is not decided by the team-victory rule) is NOT dropped: it stays in the lockstep session as a
    // SPECTATOR -- no CTL_PLAYER_LEFT, no session downgrade, its own HUMAN bit cleared and DEFEATED|GONE set at
    // the elimination step exactly like every survivor already flips it (U56), its sim keeps stepping with the
    // survivors (per-step hash identical), its own orders are dropped before enqueue, and it ends its own match
    // when the survivors decide theirs (mh_spectate.h). Needs player_left_pin_fix. A 2-peer match ends as
    // today. Carried for the unpromoted bodies by byte patches (net_lockstep.cpp, install_spectate). SIM-AFFECTING:
    // every peer must run the same value -- a shipped default, not negotiated. Initialiser = faithful stock (off);
    // the ini default is 1.
    bool spectate_after_defeat = false;

    // [net] last_human_win_check -- MP U54-FW (the false win): retail presence_lost's "no other HUMAN alive" branch
    // (outcome 6 "you are the last player" / 8) prints "You have won! Congratulations" (text 0x7d) and plays the
    // victory fanfare (0xa5) without asking whether an ENEMY (AI) is still alive -- the last human of a team whose
    // mates died or dropped "won" against a live AI. With the fix ON the text and the fanfare are given only when
    // mh_spectate.h's no_enemy_alive says so; the outcome code (and so the dialog: "last player" + Continue) and
    // every sim write are unchanged. DISPLAY-ONLY: neither the print queue nor a sound is hashed state. Carried
    // for the unpromoted body by two byte patches (net_lockstep.cpp, install_last_human_win_check). Initialiser =
    // faithful stock (off); the ini default is 1.
    bool last_human_win_check = false;

    // [net] team_relations_fix -- MP U52: the lobby's TEAM column (slot +0x0c -> Players[].desc+7) and
    // MODE selector (host slot 0 +0x07) take effect. At MP match start, AFTER landing has reset the AI
    // relation mirror, every ordered pair of enabled players is set allied (same non-zero team) or enemy
    // (everything else; "-" is its own team) -- in FFA and Team mode alike, whenever any seated slot has
    // a team. Team mode additionally sets the ally-victory flag (_G_LLM_STRAT_MP_ALLY_VICTORY_RULE_FLAG:
    // last ALLIANCE standing wins) and LOCKS relations: an in-match relation order (0xf4) is a no-op on
    // every peer, and the diplomacy dialog greys its relation column. No team set = retail relations.
    // Also carried for the unpromoted sim by three retail byte patches (net_lockstep.cpp install_team_relations_fix: seed, lock). SIM-AFFECTING: every
    // peer must run the same value -- a shipped default, not negotiated. Initialiser = faithful stock.
    bool team_relations_fix = false;

    // [net] ally_damage_no_hostility -- MP U52: the AI's "you damaged my object" notification
    // (llm_strat_ai_bldg_register_visible_building) stamps ai_player_relation[aggressor] = -1 on the VICTIM --
    // unconditionally once _G_LLM_STRAT_AI_FOREIGN_BLDG_CHANGE_FLAG is set, which any foreign building
    // completion does and nothing clears. An ALLY that merely splashes or grazes a teammate's object thereby
    // turned hostile in the victim's mirror, and the victim's own units then auto-engaged it. With the fix ON
    // the stamp is skipped while the victim currently treats the aggressor as an ally (relation > 0). Applies
    // in every mode (FFA, Team, the campaign); an enemy hit still stamps. Carried by a byte patch for the
    // unpromoted function and by this field for the twin. SIM-AFFECTING: every peer must run the same value
    // (shipped default 1, not negotiated). Initialiser = faithful stock.
    bool ally_damage_no_hostility = false;

    // Not a knob: the lobby MODE byte (host slot 0 +0x07 != 0 = Team) as it stood when the session began,
    // PUSHED by the DLL's session-entry observer (the lobby slots are not a registered sim region, and
    // adding one would renumber every hash region id). Read only by the U52 seed.
    bool lobby_team_mode = false;

    // [ui] crew_shortage_reminder_s -- hotfix rc9.4 (garage report 01a13bb6): while a LOCAL-player vehicle waits
    // in EXIT_WAIT for lack of crew, exit_wait re-shows retail's "You have run out of people to drive vehicles"
    // (race_alert_text_emit) at most once per this many game-clock seconds. Retail shows it once, at exit begin.
    // DISPLAY-ONLY (the emitter's statics + the print queue; no hashed write). Carried by a splice for the retail
    // body (seams/ui_crew_reminder.cpp); mh_crew_reminder.h holds the shared throttle. Initialiser = faithful
    // stock (0 = off); the ini default is 30.
    int crew_shortage_reminder_s = 0;

    // The three `defang_*` fields were HERE and were removed by C8-e (2026-07-30) as a scope
    // decision: all three were default-off and `resync_trigger_gate` supersedes them as the
    // root-cause fix for the freeze they suppressed. Their byte patches went with them; the
    // reimplementation is back to stock behaviour at all five sites, which is what their default
    // already was. Archived, with the restore procedure, in the retired-patch log.
};

// The live knob values, read once from the ini at init. The pure functions never see this -- they take
// a `const reimpl_fixes &` so a test can pass its own.
const reimpl_fixes &fixes();
void                set_fixes(const reimpl_fixes &f);

// ---- the logic, over explicit state (unit-testable; see mh_nettest/lockstep_selftest.cpp) --------
namespace detail {

// Shared with rx_dispatch.cpp, which is why these are in the header rather than turn_engine.cpp's
// anonymous namespace. See the long comment in turn_engine.cpp for WHY the x87 helpers exist -- the
// short version is that a NaN compares as both "equal" and "below" at once, so the faithful answer
// depends on which Jcc the compiler picked, and C++ operators are uniformly false instead.
inline bool x87_equal_or_unordered(double a, double b) { return !(a < b) && !(a > b); }
inline bool x87_below_or_unordered(double a, double b) { return !(a >= b); }
// Batch D needs the other two directions of the same idiom. `JA` is the only one C++ already spells
// correctly (`>` is false on NaN, and JA is not taken when PF/CF/ZF are all set), but it is named here
// anyway so every FCOMP in timekeeper.cpp reads as the Jcc the disassembly actually has.
inline bool x87_above(double a, double b) { return a > b; }                       // JA
inline bool x87_below_equal_or_unordered(double a, double b) { return !(a > b); } // JBE
inline bool x87_above_equal(double a, double b) { return a >= b; }                // JNC / JAE

// ALIVE and HUMAN and not the local side -- the filter four batch-A functions and five dispatch loops
// all run inline.
bool participates(const engine_state &st, int32_t i);

// What the caller of dispatch_packet should do next. The original expresses this as two distinct jump
// targets, and conflating them would change behaviour: `drain_again` falls into the
// `len != 0 && SESSION_MODE == 3` re-recv test at 0x0049d31e, while `stop` is the direct return at
// 0x0049d331 that abandons the socket mid-drain.
//
// COUNTED, because "how many handlers bail out" is exactly the kind of claim that drifts: there are
// EIGHT `JMP 0x0049d331` sites across FOUR handlers -- CTL_PLAYER_LEFT (last peer), CTL_DROP_SYNCED
// and CTL_DROP_UNSYNCED (three each: the message names us / the horizons disagree / we were the last
// peer), and CTL_KICK. They map to five `return stop` statements here because handle_peer_drop is
// shared by tags 8 and 9. And only TWO jumps target 0x0049d31e, both from the drain loop itself, so
// there is no third exit hiding in a case body.
enum class packet_result { drain_again,
                           stop };


packet_result dispatch_packet(const engine_state &st, const dispatch_state &ds,
                              const dispatch_calls &calls, const reimpl_fixes &fx, int32_t sender_side_id,
                              uint32_t len);


void    commit_horizon(const engine_state &st);
void    extend_if_near_horizon(const engine_state &st, const game_calls &calls, double lookahead_scale);
int32_t find_horizon_match_side(const engine_state &st);
int32_t record_peer_horizon(const engine_state &st, int32_t player_index, double horizon, int32_t order_marker);
void    reset_player_horizon(const engine_state &st, int32_t player_idx);
int32_t no_players(const engine_state &st);

// ---- batch B: the clock ----
void sim_tick(const engine_state &st, const game_calls &calls, const reimpl_fixes &fx);
void advance_sim_clock(const engine_state &st, const game_calls &calls);

// The pump takes the schedule() result as a PARAMETER rather than calling it, which is what makes it
// testable at all -- see the note on its production wrapper in turn_engine.cpp.
void pump_after_schedule(const engine_state &st, const game_calls &calls, int32_t schedule_result);

// Likewise sim_clock_advance takes time_tick's outcome as data: it READS TOTAL_GAME_TIME, which
// time_tick has just written, so the two cannot be separated in a shadow arm (see turn_engine.cpp).
int32_t sim_clock_advance_after_time_tick(const engine_state &st, const game_calls &calls);

// ---- batch D: the frame timekeeper ----
// `now` is the wall clock, passed in rather than read, because reading it is the one thing that makes
// this function un-shadowable (see timekeeper_state). Returns the original's return: (idx+1)/20, i.e.
// 1 on every 20th call and 0 otherwise -- a 20-frame tick signal, NOT a status code.
int32_t time_tick(const timekeeper_state &s, const timekeeper_calls &c, const reimpl_fixes &fx,
                  double now);

// ---- MP D14 / D24: the resync-order scheduling arithmetic lives in fix/resync_clamp.h ----
//
// Both functions MOVED at fork F3D (F1A residue R4). They are the one piece of arithmetic the net
// seam and this closure genuinely share -- seams/net_lockstep.cpp's broadcast_resync_state detour
// carries the fix when the original body is live, tx_emit_ctrl.cpp carries it when ours is -- and
// while they lived here the net seam had to include a closure header and name a closure symbol to
// reach a pure double comparison. That was the one config-(1) residue neither the selector nor a
// guard could honestly remove: the original configuration really does run this arithmetic. So the
// FUNCTIONS moved to a neutral header instead, byte-identical, with their full derivations.
//
// Re-exported into `detail` so this closure's call sites (tx_emit_ctrl.cpp) and lockstest's
// test_d14_resync_order_exec_time / test_d24_resync_order_barrier keep spelling them where they
// always did. The alias is deliberate, not laziness: a NET TU that goes back to the
// `mh::lockstep::detail::` spelling is then caught by check_net_lockstep_refs as an UNRULED
// reference instead of passing quietly as a name that still resolves.
using mh::fix::resync_order_barrier;
using mh::fix::resync_order_exec_time;

} // namespace detail

// ---- production entry points (bound to the live game state) --------------------------------------
void    commit_horizon();
void    extend_if_near_horizon(double lookahead_scale);
int32_t find_horizon_match_side();
int32_t record_peer_horizon(int32_t player_index, double horizon, int32_t order_marker);
void    reset_player_horizon(int32_t player_idx);
int32_t no_players();

// ---- batch B ----
void    sim_tick();
void    advance_sim_clock();
void    pump();
int32_t sim_clock_advance();

// ---- batch C ----
void dispatch();

// ---- batch D ---- (owns the GetCurrentTime() read; the logic is detail::time_tick)
int32_t time_tick();


// ---- L1-P: promotion (run OURS for real) ---------------------------------------------------------
//
// Gated on `[promote] lockstep=1`, so a one-flag rollback returns to the original engine. Returns the
// number of seams installed; a PARTIAL install is logged loudly and means the run is invalid, because
// half a closure is neither engine.
//
// NINE SEAMS, and the two absentees are deliberate:
//   * llm_strat_sim_tick    -- THE DETERMINISM HARNESS OWNS ITS ENTRY. harness.cpp installs a
//     trampoline there unconditionally when it arms (install_trampoline(ADDR_SIM_TICK) ~line 1285)
//     and checks the prologue bytes first (~1271). Whichever patches second refuses -- and if the
//     export won that race, the HARNESS would be the one refusing, silently voiding the entire
//     determinism run. Exactly the instrument conflict that made sim_tick un-shadowable in batch B,
//     with a worse failure mode. It stays original in any harnessed run.
//   * llm_net_lockstep_no_players -- has no caller anywhere in the image, and every seam is an ABI
//     contract maintained forever (Law 4). A seam nothing can reach is pure cost.
//   * llm_strat_time_tick   -- NOT an absentee any more (C4). Its entry is owned by the shipped pacing
//     detour, so it cannot be INSTALLED here; install_promotion records the request and
//     mh/seams/net_lockstep.cpp REBINDS the detour's fall-through once that detour is armed. The two
//     accessors below are that handoff. This is the general model for any function that already
//     carries a DLL trampoline: the instrument keeps the entry, the implementation becomes its
//     destination -- rather than two writers racing for one entry and the loser vanishing quietly.
int  install_promotion(const char *ini_path, int default_on);
bool promotion_active();

// C4 handoff: was `time_tick` selected for promotion, and what should the detour fall through to?
bool  time_tick_requested();
void *time_tick_entry_thunk();
// C6: the same pair for sim_tick, which rebinds onto the DETERMINISM HARNESS's detour. Opt-in only --
// `[promote] sim_tick=1` on top of a promoted run; it is not in the default closure, because every
// L1-P promotion result was measured with sim_tick original.
bool  sim_tick_requested();
void *sim_tick_entry_thunk();

// ---- C5: per-seam gating is a DIAGNOSTIC facility, and the policy is enforced here ---------------
//
// Settled 2026-07-29: per-seam gating exists to BISECT a red run; whole-closure is the only SHIPPING
// shape. The danger the enforcement addresses is not that someone runs a subset -- that is the point
// of the feature -- but that a subset run is later read as a ship-config verdict. Three ways that
// happens, and all three are the L1-P P0 lesson (an asymmetric run passed with the promotion never
// installed; an asymmetric test whose asymmetry evaporates does not fail, it passes):
//
//   * the subset is silently narrowed by a TYPO, so the run promotes less than the author believes;
//   * the subset selects NOTHING, so the run is un-promoted while still printing a "SUBSET" banner;
//   * the run's partial-ness is recorded only in prose, so nothing downstream can act on it.
//
// Hence: unknown tokens and empty selections REFUSE the whole install rather than narrowing it, and
// every run emits a machine-readable `RUN-CONFIG:` line a runner can gate on.

// Every seam `lockstep_seams=` may name: the nine installed by install_promotion, plus `time_tick`,
// which arrives by rebind (C4). Terminated by nullptr.
extern const char *const SEAM_NAMES[];
// ---- C8-d: the seam liveness counter, declared for the PRODUCTION ENTRY POINTS -------------------
//
// It used to be called only from the `promoted::` entry-thunk wrappers, so it counted calls that
// ARRIVED AT THE ORIGINAL ENTRY ADDRESS. Once the intra-closure edges became direct C++ calls, a seam
// reached only from inside the closure stopped touching its entry -- `dispatch`, which runs every
// frame, read ZERO in a run where it plainly ran. "READ THE LIVENESS LINES BEFORE BELIEVING A PASS"
// is the standing defence against the O3 failure (an asymmetric run that passed with the promotion
// never installed), and a counter that cannot tell "ran" from "was never armed" is not that defence.
// So the production entries call it, and the wrappers are pure forwards. Declared here because two of
// those entries (dispatch, time_tick) live in sibling TUs.
namespace promoted {
void live(int slot, const char *name);
}

int seam_count();

namespace detail {

enum class subset_result {
    full,          // absent or empty string -- the shipping default, every seam
    ok,            // a valid non-empty subset -- a diagnostic run
    empty,         // syntactically present but selects nothing (e.g. ",,") -- REFUSE
    unknown_token, // names a seam that does not exist -- REFUSE, and say which
};

// Parse `want` (a comma-separated seam list) into `mask[0..n)` over SEAM_NAMES. Whitespace around a
// token is ignored and matching is case-insensitive, but a token must equal a whole name: prefix
// matching would let `pump` also select a hypothetical `pump_2`. On `unknown_token` the offending
// token is copied into `bad` (truncated to bad_cap-1) so the log can name it.
subset_result parse_seam_subset(const char *want, bool *mask, int n, char *bad, int bad_cap);

} // namespace detail

} // namespace mh::lockstep
