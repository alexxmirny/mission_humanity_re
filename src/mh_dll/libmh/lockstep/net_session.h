//
// lockstep/net_session.h -- the two ORIGINAL netcode writers NET-SESSION owns, and the ONE
// implementation of the resync-completion transition they share with the RX path.
//
//   llm_net_session_globals_reset  @0x0049e4c3  the process-level lockstep pacing bootstrap
//   llm_wait_screen_frame          @0x0043ee38  the mode-8 resync-wait frame (LEADER half)
//
// WHY THESE TWO ARE HERE AND NOT EXCEPTED. SB-NET and SB-BOOT adjudicated 38 original writers of
// regions libmh also writes and excepted 36 of them; these are the two that could not be, and for
// two DIFFERENT reasons (writer_dispositions.json `owned_elsewhere`):
//
//   * session_globals_reset is the SOLE SETTER of the pacing baseline our turn engine then reads and
//     mutates every tick -- STEP_SIZE / HORIZON / COMMITTED_HORIZON = 10.0, ADAPT_NEXT_TIME = 70.0,
//     SYNC_WAIT_ELAPSED = 1.0, PEER_TIMEOUT_ELAPSED = -1.0, SYNC_RETRY_COUNTDOWN = 60,
//     GAME_SESSION_MODE = 2, plus 8 counters zeroed. In-process it PROVABLY cannot race (one call,
//     from llm_game_init_subsystems at WM_CREATE, before the boot-stage machine and long before a
//     sim tick), so the ownership argument is STANDALONE rather than a race: a default-constructed
//     0.0 is not 10.0, and a libmh that started the turn engine without this would pace silently
//     wrong with no exception able to cover it.
//   * wait_screen_frame performs INLINE the leader-local half of a protocol transition libmh already
//     owns for the receiving side (rx_dispatch's CTL_RESYNC_END arm, live by default). Two
//     implementations of one transition is precisely what SB-SOLE exists to stop.
//
// THE SPLICE-VS-TRANSLATE QUESTION, ANSWERED (the item required an explicit answer). The candidate
// splice was "put the local transition inside the already-promoted emitter
// llm_net_send_lockstep_resync_resume", whose ONLY caller is llm_wait_screen_frame (verified by
// xref, EN v386). It is REJECTED on evidence, not on taste:
//   1. It does not do the job. The two writes that make this function a frontier writer are INLINE
//      at 0x0043ee7e (RESYNC_IN_PROGRESS = 0) and 0x0043ee88 (clear LS_RESYNC_WAIT), in the ORIGINAL
//      body. Splicing into the emitter leaves those bytes executing, so the census keeps naming this
//      function and SB-SOLE's condition is not met -- the item would close on a ledger row while the
//      thing it was opened for stayed true.
//   2. It would make the emitter lie. `send_lockstep_resync_resume` puts one control packet on the
//      wire; a body that also tore down local resync state would be an emitter that is not one, and
//      the next reader of tx_emit_ctrl.cpp has no way to know.
// So: WHOLE-BODY TRANSLATION of the 107-byte frame function, with the transition itself factored
// into `resync_complete_local` below -- which the RX path calls too. That is what makes "the leader
// path and the peer path reach ONE implementation" true as a fact about the code rather than as a
// claim about two bodies that happen to agree.
//
// THE ONE DELIBERATE DEVIATION, and the evidence that it is unobservable. The original orders the
// transition differently on the two sides:
//
//   leader (0x0043ee74..0x0043ee8f):  mp_leave; busywait; RESYNC_IN_PROGRESS=0; clear 0x40; resync_and_tick
//   peer   (0x0049d098..0x0049d0b5):  mp_leave; busywait; clear 0x40; resync_and_tick; RESYNC_IN_PROGRESS=0
//
// Same five acts, same end state; only the position of the RESYNC_IN_PROGRESS store differs.
// `resync_complete_local` implements the PEER order (which rx_dispatch already shipped), so the
// leader's store moves across `time_resync_and_tick`. That is unobservable because NOTHING BETWEEN
// THEM READS IT: _G_LLM_NET_RESYNC_IN_PROGRESS has exactly six references in the whole EN image
// (measured 2026-09-01) -- writes in session_globals_reset, wait_screen_frame and lockstep_dispatch
// x2, and a single read+write pair in llm_net_lockstep_force_resync, which is not on this path.
// llm_strat_time_resync_and_tick reaches only llm_time_get_ticks_ms, time_GetCurrentTime and
// llm_strat_time_tick; time_tick touches STATUS_FLAGS bit 0x80 only (TEST/AND/OR at 0x0043efaa,
// 0x0043effa, 0x0043f209, 0x0043f226) and never the 0x40 bit or RESYNC_IN_PROGRESS. sync_busywait is
// a bare spin with no message pump (resync.h), so nothing re-enters either.
// `net_session_selftest.cpp` PINS this: the leader path and the peer path are driven to completion
// over the same fixture and asserted to reach an identical (STATUS_FLAGS, RESYNC_IN_PROGRESS) end
// state, and the ordering claim is asserted directly by a mock that records the call/store sequence.
//
// THE STACK PROBE IS DROPPED, as everywhere else in this closure: both bodies open with
// `PUSH n; CALL utils_assert_stack_capacity` (0x004cf46f), which touches no tracked state.
//
// Translated from tmp/decomp_net_session/llm_net_session_globals_reset_0049e4c3.asm and
// tmp/decomp_net_session/llm_wait_screen_frame_0043ee38.asm -- NOT from the .c beside either.
//
#pragma once

#include <cstdint>

namespace mh::lockstep {

// ---- 1. llm_net_session_globals_reset @0x0049e4c3 -----------------------------------------------
//
// THE MEASURED CONSTANTS, named. Each is the exact 64-bit pattern the original stores as two dword
// halves (low half 0, high half the value below), read out of the listing rather than guessed:
//   0x40240000_00000000 = 10.0    0x40518000_00000000 = 70.0
//   0x3ff00000_00000000 =  1.0    0xbff00000_00000000 = -1.0
// They are exported because the ADDRESS-TABLE check reads them: tools/check_net_session_addrs.py
// disassembles llm_net_session_globals_reset in the retail exe and requires its stores to carry
// exactly these values -- one table, so the check cannot drift from the body it checks. (Until fork
// F2F the reader was the live read-back probe, mh/seams/net_session_probe.cpp, for the same reason.)
inline constexpr double  SESSION_HORIZON_INIT              = 10.0; // COMMITTED_HORIZON, HORIZON, STEP_SIZE
inline constexpr double  SESSION_ADAPT_NEXT_TIME_INIT      = 70.0;
inline constexpr double  SESSION_SYNC_WAIT_ELAPSED_INIT    = 1.0;
inline constexpr double  SESSION_PEER_TIMEOUT_ELAPSED_INIT = -1.0;
inline constexpr int32_t SESSION_SYNC_RETRY_COUNTDOWN_INIT = 0x3c; // 60
inline constexpr int32_t SESSION_MODE_MP_LOCAL             = 2;    // _G_LLM_GAME_SESSION_MODE

// The 22 stores, over 16 regions -- the best regions-per-byte ratio on the whole SB-SOLE frontier.
// Order of the fields is the order of the stores, so the body reads against the listing top to
// bottom.
struct session_init_state {
    int32_t *local_player_index;    // _G_LLM_NET_LOCAL_PLAYER_INDEX            0x005d55ac
    int32_t *local_player_slot;     // _G_LLM_NET_LOCAL_PLAYER_SLOT             0x005d55b8
    int32_t *is_host;               // _G_LLM_NET_IS_HOST                       0x005d55b0
    int32_t *lockstep_player_count; // _G_LLM_NET_LOCKSTEP_PLAYER_COUNT         0x005d54c0
    int32_t *lobby_map_recv_done;   // _G_LLM_NET_LOBBY_MAP_RECV_DONE           0x005d54c8
    int32_t *send_buf_cursor;       // _G_LLM_NET_SEND_BUF_CURSOR               0x005d59c4
    int32_t *probe_server_count;    // _G_LLM_MP_PROBE_SERVER_COUNT             0x005d1358
    int32_t *session_count;         // _G_LLM_NET_SESSION_COUNT                 0x005d1364
    int32_t *lobby_scan_host_count; // _G_LLM_NET_LOBBY_SCAN_HOST_COUNT         0x005d54bc
    double  *committed_horizon;     // _G_LLM_STRAT_LOCKSTEP_COMMITTED_HORIZON  0x005d558c
    double  *horizon;               // _G_LLM_STRAT_LOCKSTEP_HORIZON            0x005d5594
    double  *step_size;             // _G_LLM_STRAT_LOCKSTEP_STEP_SIZE          0x005d55bc
    double  *adapt_next_time;       // _G_LLM_STRAT_LOCKSTEP_ADAPT_NEXT_TIME    0x005d55c4
    int32_t *game_session_mode;     // _G_LLM_GAME_SESSION_MODE                 0x00e58344
    int32_t *sync_retry_countdown;  // _G_LLM_NET_LOCKSTEP_SYNC_RETRY_COUNTDOWN 0x00e58789
    int32_t *stall_nag_count;       // _G_LLM_NET_LOCKSTEP_STALL_NAG_COUNT      0x00e5878d
    int32_t *resync_trigger_count;  // _G_LLM_NET_LOCKSTEP_RESYNC_TRIGGER_COUNT 0x00e58791
    int32_t *stall_count;           // _G_LLM_STRAT_LOCKSTEP_STALL_COUNT        0x00e58795
    double  *sync_wait_elapsed;     // _G_LLM_NET_LOCKSTEP_SYNC_WAIT_ELAPSED    0x00e58799
    double  *peer_timeout_elapsed;  // _G_LLM_NET_LOCKSTEP_PEER_TIMEOUT_ELAPSED 0x00e587a1
    int32_t *sync_wait_active;      // _G_LLM_NET_SYNC_WAIT_ACTIVE              0x00e587a9
    int32_t *resync_in_progress;    // _G_LLM_NET_RESYNC_IN_PROGRESS            0x00e587ad
};

session_init_state live_session_init_state();

void session_globals_reset(const session_init_state &st);

// ---- 2. the resync-completion transition (ONE implementation) ------------------------------------
//
// Called by BOTH sides: rx_dispatch's CTL_RESYNC_END arm (the peer that was told the leader is done)
// and wait_screen_frame's leader branch (the leader deciding it is done). Kept in its own tiny
// state/calls pair rather than reusing either caller's, so neither caller's struct has to grow a
// field for the other's sake.
struct resync_complete_state {
    uint8_t *status_flags;       // _G_LLM_NET_LOCKSTEP_STATUS_FLAGS -- LS_RESYNC_WAIT cleared here
    int32_t *resync_in_progress; // _G_LLM_NET_RESYNC_IN_PROGRESS
};

struct resync_complete_calls {
    void (*mp_leave_reset_game_mode)(); // restore GAME_MODE from GAME_MODE_SAVED + dismiss the overlay
    int32_t (*sync_busywait)();         // the "SYNCHRONIZING" spin (returns 1 under resync_wait_fix)
    void (*time_resync_and_tick)();     // re-baseline LAST_GAME_TIME, then one time_tick
    // LIB-ABI stage E hoist ops (overlay_hoist.h) -- tail member, null-skipped by suites.
    const struct overlay_hoist_ops *hoist;
};

void resync_complete_local(const resync_complete_state &st, const resync_complete_calls &calls);

// ---- 3. llm_wait_screen_frame @0x0043ee38 --------------------------------------------------------
//
// The mode-8 (resync-wait screen) per-frame body, reached from llm_frame_dispatch case 8 -- its only
// caller. Two guards, then the transition, then the RX pump unconditionally.
struct wait_screen_state {
    const uint32_t       *resync_deadline_ms; // _G_LLM_NET_LOCKSTEP_RESYNC_DEADLINE_MS 0x00e58c38
    resync_complete_state done;
};

struct wait_screen_calls {
    // exclude_side_id is -1 at this call site (MOV EAX,0xffffffff), i.e. "exclude nobody".
    int32_t (*is_local_leader_peer)(int32_t exclude_side_id);
    uint32_t (*ticks_ms)();
    // CTL_RESYNC_END on the wire -- an EFFECT, see below. It takes a `double` because the ORIGINAL
    // entry does: llm_net_send_lockstep_resync_resume ends `RET 0x8` and this call site pushes two
    // zero dwords for it (0x0043ee6b/0x0043ee6d), which the emitter never reads. The argument is
    // kept in the signature rather than dropped so that MH_INTERNAL_CALL's two arms have the one
    // type it requires -- i.e. so the entry-routed diagnostic build stays type-checked.
    void (*send_lockstep_resync_resume)(double unused_arg);
    void (*lockstep_dispatch)(); // the RX pump, called on EVERY frame including the quiet one
    resync_complete_calls done;
};

void wait_screen_frame(const wait_screen_state &st, const wait_screen_calls &calls);

wait_screen_state        live_wait_screen_state();
const wait_screen_calls &live_wait_screen_calls();

// ---- the production entry points, applied to live state ------------------------------------------
//
// EFFECTFUL, and that is why neither of these gets a shadow site: wait_screen_frame SENDS A PACKET
// and drains the socket, and session_globals_reset runs exactly once per process at WM_CREATE (a
// shadow arm would need a second call that does not exist). Their evidence is the offline oracle
// `net_selftest netsessiontest` plus the live read-back probe -- see the header banner.
void session_globals_reset();
void wait_screen_frame();

// How many times OUR session_globals_reset body has served a call. Read by the read-back probe, so
// its one line can say whether the 22 values it just read were written by us or by the original --
// the two configurations are otherwise indistinguishable in the log.
long session_globals_reset_calls();

// The non-static wrappers turn_engine.cpp's promotion table drives (same arrangement as C8-c's:
// MH_EXPORT_REPLACE defines a `static` installer that cannot leave this TU).
bool install_seam_session_globals_reset();
bool install_seam_wait_screen_frame();

} // namespace mh::lockstep
