//
// lockstep/net_session.cpp -- the two functions declared in net_session.h (NET-SESSION).
//
// Translated from tmp/decomp_net_session/llm_net_session_globals_reset_0049e4c3.asm and
// tmp/decomp_net_session/llm_wait_screen_frame_0043ee38.asm -- NOT from the .c beside either. The
// argument for owning them, the rejected splice, and the one deliberate ordering deviation are all
// in the header; this file carries only what a reader needs at the code.
//
#include "lockstep/net_session.h"

#include "addr/mh_addrs.gen.h"
#include "addr/mh_calls.gen.h"
#include "addr/mh_export.gen.h"     // MH_EXPORT_REPLACE -- the two promotion seams, at the tail
#include "lockstep/internal_call.h" // MH_INTERNAL_CALL -- C8-d, the intra-closure edges
#include "lockstep/overlay_hoist.h" // LIB-ABI stage E: mp_leave's restore + dismiss-rule hoist
#include "lockstep/resync.h"        // sync_busywait / is_local_leader_peer, the production entries
#include "lockstep/turn_engine.h"   // LS_RESYNC_WAIT, dispatch() -- one definition of each
#include "lockstep/tx_emit_ctrl.h"  // send_lockstep_resync_resume, likewise
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::lockstep {

// ---- 1. llm_net_session_globals_reset @0x0049e4c3 -----------------------------------------------

// A straight-line store list -- no branch, no read, no call. The order below is the order of the
// stores at 0x0049e4db..0x0049e5e9; nothing here depends on it (the 22 targets are distinct), but a
// reader diffing against the listing should not have to hunt.
//
// THE FOUR DOUBLES ARE WRITTEN AS DOUBLES, not as two dword halves. The original has no FPU here at
// all: it stores the low half 0 and the high half as an immediate (0x40240000 = 10.0). Same 8 bytes,
// and the constants are named in the header so the read-back probe compares against ONE table.
void session_globals_reset(const session_init_state &st) {
    *st.local_player_index    = 0;
    *st.local_player_slot     = 0;
    *st.is_host               = 0;
    *st.lockstep_player_count = 0;
    *st.lobby_map_recv_done   = 0;
    *st.send_buf_cursor       = 0;
    *st.probe_server_count    = 0;
    *st.session_count         = 0;
    *st.lobby_scan_host_count = 0;

    // The pacing baseline. This is the whole reason the function is ownership rather than an
    // exception: our turn engine reads and mutates these every tick, and a standalone libmh that
    // never ran this would start at 0.0 instead of 10.0.
    *st.committed_horizon = SESSION_HORIZON_INIT;
    *st.horizon           = SESSION_HORIZON_INIT;
    *st.step_size         = SESSION_HORIZON_INIT;
    *st.adapt_next_time   = SESSION_ADAPT_NEXT_TIME_INIT;

    *st.game_session_mode    = SESSION_MODE_MP_LOCAL;
    *st.sync_retry_countdown = SESSION_SYNC_RETRY_COUNTDOWN_INIT;
    *st.stall_nag_count      = 0;
    *st.resync_trigger_count = 0;
    *st.stall_count          = 0;
    *st.sync_wait_elapsed    = SESSION_SYNC_WAIT_ELAPSED_INIT;
    *st.peer_timeout_elapsed = SESSION_PEER_TIMEOUT_ELAPSED_INIT;
    *st.sync_wait_active     = 0;
    *st.resync_in_progress   = 0;
}

// ---- 2. the resync-completion transition ---------------------------------------------------------
//
// The RX order (rx_dispatch's CTL_RESYNC_END, 0x0049d098). The leader's original stores
// resync_in_progress one step earlier; see the header for why that is unobservable and where it is
// pinned. sync_busywait's return is discarded by BOTH original sites.
void resync_complete_local(const resync_complete_state &st, const resync_complete_calls &calls) {
    // LIB-ABI stage E hoist (mp_leave = the saved-mode restore + a tail overlay_dismiss): the
    // restore goes BEFORE the call (the callee re-does the identical store, and its dismiss
    // guard reads the restored value), the dismiss rule AFTER with not4 latched on that value.
    const uint8_t saved = hoist_mode_saved(calls.hoist);
    hoist_mode_set(calls.hoist, saved);
    calls.mp_leave_reset_game_mode();
    hoist_dismiss(calls.hoist, saved != 4);
    calls.sync_busywait();
    *st.status_flags &= static_cast<uint8_t>(~LS_RESYNC_WAIT);
    calls.time_resync_and_tick();
    *st.resync_in_progress = 0;
}

// ---- 3. llm_wait_screen_frame @0x0043ee38 --------------------------------------------------------
//
// `if (is_local_leader_peer(-1) && ticks_ms() > deadline) { resume; complete; } dispatch();`
//
// THE SHORT CIRCUIT IS THE ORIGINAL'S. 0x0043ee5a `TEST EAX,EAX / JZ` skips the clock read entirely
// when we are not the leader, so a non-leader frame reads the clock ZERO times -- asserted by call
// count in the oracle, because an implementation that evaluated both guards would still produce the
// right end state and pass any state-only check.
//
// THE DEADLINE COMPARE IS UNSIGNED. 0x0043ee69 is `JBE`, not `JLE`: the frame acts when the tick
// count is strictly ABOVE the deadline, and the comparison survives the 49.7-day GetTickCount wrap
// the same way sync_busywait's does.
//
// THE DISPATCH IS UNCONDITIONAL and outside the branch (LAB_0043ee94 is the join): the wait screen
// still pumps the socket on every frame, which is how the peer that is NOT the leader ever sees the
// CTL_RESYNC_END this function sends.
void wait_screen_frame(const wait_screen_state &st, const wait_screen_calls &calls) {
    if (calls.is_local_leader_peer(-1) != 0 && calls.ticks_ms() > *st.resync_deadline_ms) {
        calls.send_lockstep_resync_resume(0.0); // the two PUSH 0s at 0x0043ee6b -- never read
        resync_complete_local(st.done, calls.done);
    }
    calls.lockstep_dispatch();
}

namespace {

// The generated wrapper for the emitter takes a `double` -- the original ends `RET 0x8` and the two
// `PUSH 0`s at 0x0043ee6b/0x0043ee6d are that 8-byte argument, which the emitter never reads (it has
// no payload; tx_emit_ctrl.h). Our production entry takes none, so this adapter gives the edge the
// one function-pointer type MH_INTERNAL_CALL requires both arms to share.
void resync_resume_edge(double /*unused_arg -- the original pushes two zero dwords*/) {
    mh::lockstep::send_lockstep_resync_resume();
}

} // namespace

const wait_screen_calls &live_wait_screen_calls() {
    static const wait_screen_calls cc = {
        // Four of the six edges are OURS (C8-d: direct by default, entry-routed in the diagnostic
        // build). time_resync_and_tick is still original code -- the time base, outside this
        // closure. mp_leave_reset_game_mode was SPLIT at LIB-ABI stage E (2026-09-03, reversing
        // the earlier "presentation, outside this closure" note by the user's call): its
        // load-bearing writes (the saved-mode restore + the dismiss rule) are hoisted in
        // resync_complete_local above, and the visual teardown remainder is a host-callback
        // table entry.
        MH_INTERNAL_CALL(llm_net_lockstep_is_local_leader_peer, mh::lockstep::is_local_leader_peer),
        mh::host().ticks_ms,
        MH_INTERNAL_CALL(llm_net_send_lockstep_resync_resume, resync_resume_edge),
        MH_INTERNAL_CALL(llm_net_lockstep_dispatch, mh::lockstep::dispatch),
        {
            mh::state::evt::mp_leave_reset_game_mode,
            MH_INTERNAL_CALL(llm_net_lockstep_sync_busywait, mh::lockstep::sync_busywait),
            MH_LIBMH_BIND(llm_strat_time_resync_and_tick),
            &live_overlay_hoist_ops(), // LIB-ABI stage E
        },
    };
    return cc;
}

} // namespace mh::lockstep

// ---- promotion -----------------------------------------------------------------------------------
//
// PROVE OUR CODE ACTUALLY RAN, the same rule as the neighbours -- and it matters more here than
// anywhere: session_globals_reset is called EXACTLY ONCE per process, and wait_screen_frame only
// while a resync is in flight, so "zero calls" and "clean" look identical in every log the rig
// produces. The read-back probe is the other half of that answer.
namespace mh::lockstep::promoted_session {

long g_calls[2]; // exactly two, sized by hand like the neighbours

void live(int slot, const char *name) {
    const long n = ++g_calls[slot];
    if (n == 1 || n == 100 || n == 1000 || n == 10000)
        mh::lockstep::say("; [promote] %s: call #%ld (OURS is live)\n", name, n);
}

void session_globals_reset() {
    live(0, "net_session/session_globals_reset");
    mh::lockstep::session_globals_reset(mh::lockstep::live_session_init_state());
}

void wait_screen_frame() {
    live(1, "net_session/wait_screen_frame");
    mh::lockstep::wait_screen_frame(mh::lockstep::live_wait_screen_state(),
                                    mh::lockstep::live_wait_screen_calls());
}

} // namespace mh::lockstep::promoted_session

// clang-format off
MH_EXPORT_REPLACE(llm_net_session_globals_reset, mh::lockstep::promoted_session::session_globals_reset)
MH_EXPORT_REPLACE(llm_wait_screen_frame,         mh::lockstep::promoted_session::wait_screen_frame)
// clang-format on

namespace mh::lockstep {

// The production entry points. They are what the SELFTEST and any in-DLL caller use; the promotion
// thunks above add only the call counter.
void session_globals_reset() { session_globals_reset(live_session_init_state()); }
void wait_screen_frame() { wait_screen_frame(live_wait_screen_state(), live_wait_screen_calls()); }

// The non-static wrappers turn_engine.cpp's table drives -- MH_EXPORT_REPLACE's installers are
// `static` and cannot leave this TU.
bool install_seam_session_globals_reset() {
    return mh_export_install_llm_net_session_globals_reset();
}
bool install_seam_wait_screen_frame() { return mh_export_install_llm_wait_screen_frame(); }

long session_globals_reset_calls() { return promoted_session::g_calls[0]; }

} // namespace mh::lockstep
