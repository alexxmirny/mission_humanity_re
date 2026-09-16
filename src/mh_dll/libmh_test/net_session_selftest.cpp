//
// net_session_selftest.cpp -- `net_selftest.exe netsessiontest`: NET-SESSION's two bodies over plain
// locals, with no game and no rig.
//
// WHY THIS IS THE PRIMARY EVIDENCE rather than a shadow site. Neither function is armable:
//
//   session_globals_reset runs EXACTLY ONCE per process, from llm_game_init_subsystems at WM_CREATE.
//     A shadow site compares an original arm and ours per call; with one call in the life of the
//     process there is nothing to compare against on a second pass, and the state it writes is
//     immediately consumed by everything that boots afterwards.
//   wait_screen_frame SENDS A PACKET (CTL_RESYNC_END) and DRAINS THE SOCKET. Shadow mode restores
//     state between the arms; it cannot un-send a datagram or un-consume a stream, so arming it
//     would double the resume broadcast while the state comparison still read clean.
//
// The other half of the evidence is the ADDRESS TABLE -- that the 22 pointers name the globals the
// ORIGINAL writes, which no fixture can show. That was a live read-back probe (`[netprobe]`, seams/
// net_session_probe.cpp) until fork F1B replaced it with an offline disassembly of the retail body
// (tools/check_net_session_addrs.py, in lint) and F2F deleted the probe. This file is where the
// branches, the boundaries and the ordering live.
//
// EVERY CASE BELOW WAS MUTATION-CHECKED: the production body was broken in the way the assertion
// names, the run was confirmed to fail on THAT assertion, and the break reverted.
//
// The LEADER-vs-PEER end-state comparison the item's acceptance requires is NOT here -- it is in
// lockstep_selftest.cpp (`test_dispatch_resync_leader_matches_peer`), because the peer arm is the RX
// dispatcher and that file already owns the crafted-packet harness for it. Driving both ends over
// one fixture is the point; duplicating a 150-line `world` here to avoid an #include would have
// meant two harnesses that could disagree.
//
#include "lockstep/net_session.h"
#include "lockstep/turn_engine.h" // LS_RESYNC_WAIT / LS_HORIZON_PENDING / LS_SESSION_ENDED

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_checks, g_fails;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s\n", what);
    }
}

using mh::lockstep::LS_HORIZON_PENDING;
using mh::lockstep::LS_RESYNC_WAIT;
using mh::lockstep::LS_SESSION_ENDED;
using mh::lockstep::session_init_state;
using mh::lockstep::wait_screen_calls;
using mh::lockstep::wait_screen_state;

// Bit-exact double compare. The originals store two dword halves, so "is it 10.0" is really "are
// those eight bytes 0x4024000000000000" -- and a body that computed 10.0 a different way would still
// have to produce the same bytes to be equivalent.
uint64_t bits(double d) {
    uint64_t u;
    std::memcpy(&u, &d, sizeof(u));
    return u;
}

// ---- 1. the session-init fixture -----------------------------------------------------------------
//
// Every global POISONED with a DISTINCT non-zero value, so that (a) a store the body forgot leaves
// its poison behind and is caught, and (b) two stores that were swapped land on the wrong field and
// are caught. All-zero or all-equal seeding would pass a body that swapped two of the nine counters.
struct init_fixture {
    int32_t local_player_index = 0x11, local_player_slot = 0x12, is_host = 0x13;
    int32_t lockstep_player_count = 0x14, lobby_map_recv_done = 0x15, send_buf_cursor = 0x16;
    int32_t probe_server_count = 0x17, session_count = 0x18, lobby_scan_host_count = 0x19;
    double  committed_horizon = -3.5, horizon = -4.5, step_size = -5.5, adapt_next_time = -6.5;
    int32_t game_session_mode = 0x21, sync_retry_countdown = 0x22, stall_nag_count = 0x23;
    int32_t resync_trigger_count = 0x24, stall_count = 0x25;
    double  sync_wait_elapsed = -7.5, peer_timeout_elapsed = -8.5;
    int32_t sync_wait_active = 0x26, resync_in_progress = 0x27;

    session_init_state st{};

    init_fixture() {
        st = session_init_state{&local_player_index,
                                &local_player_slot,
                                &is_host,
                                &lockstep_player_count,
                                &lobby_map_recv_done,
                                &send_buf_cursor,
                                &probe_server_count,
                                &session_count,
                                &lobby_scan_host_count,
                                &committed_horizon,
                                &horizon,
                                &step_size,
                                &adapt_next_time,
                                &game_session_mode,
                                &sync_retry_countdown,
                                &stall_nag_count,
                                &resync_trigger_count,
                                &stall_count,
                                &sync_wait_elapsed,
                                &peer_timeout_elapsed,
                                &sync_wait_active,
                                &resync_in_progress};
    }
};

// ---- 2. the wait-screen recorder ------------------------------------------------------------------
//
// Records ORDER as well as counts. A state-only check passes on any permutation of the five acts, and
// the one thing this translation deliberately reorders (RESYNC_IN_PROGRESS, see net_session.h) is
// exactly the kind of thing a state-only check cannot see -- so the sequence is asserted directly,
// and the mocks SAMPLE the state they run under so "the flag was already cleared when
// time_resync_and_tick ran" is a fact this file can state.
struct ws_log {
    std::vector<const char *> seq;
    int                       leader_calls = 0, tick_calls = 0, resume_calls = 0, dispatch_calls = 0;
    int                       leave_calls = 0, busywait_calls = 0, resync_tick_calls = 0;
    int32_t                   leader_arg    = 0x7fffffff; // poisoned: -1 is what the site passes
    double                    resume_arg    = 12345.0;    // poisoned: 0.0 is what the site pushes
    int32_t                   leader_result = 0;
    uint32_t                  ticks         = 0;
    // sampled INSIDE time_resync_and_tick
    uint8_t status_at_resync_tick      = 0xff;
    int32_t in_progress_at_resync_tick = -1;
    // the fixture the mocks observe
    uint8_t  status      = 0;
    int32_t  in_progress = 0;
    uint32_t deadline    = 0;

    void reset() { *this = ws_log{}; }
};

ws_log g_ws;

int32_t rec_is_local_leader_peer(int32_t exclude) {
    ++g_ws.leader_calls;
    g_ws.leader_arg = exclude;
    g_ws.seq.push_back("leader");
    return g_ws.leader_result;
}
uint32_t rec_ticks_ms() {
    ++g_ws.tick_calls;
    g_ws.seq.push_back("ticks");
    return g_ws.ticks;
}
void rec_resume(double arg) {
    ++g_ws.resume_calls;
    g_ws.resume_arg = arg;
    g_ws.seq.push_back("resume");
}
void rec_dispatch() {
    ++g_ws.dispatch_calls;
    g_ws.seq.push_back("dispatch");
}
void rec_leave() {
    ++g_ws.leave_calls;
    g_ws.seq.push_back("leave");
}
int32_t rec_busywait() {
    ++g_ws.busywait_calls;
    g_ws.seq.push_back("busywait");
    return 1;
}
void rec_resync_tick() {
    ++g_ws.resync_tick_calls;
    g_ws.seq.push_back("resync_tick");
    g_ws.status_at_resync_tick      = g_ws.status;
    g_ws.in_progress_at_resync_tick = g_ws.in_progress;
}

wait_screen_state ws_state() {
    return wait_screen_state{&g_ws.deadline, {&g_ws.status, &g_ws.in_progress}};
}

wait_screen_calls ws_calls() {
    return wait_screen_calls{rec_is_local_leader_peer, rec_ticks_ms, rec_resume, rec_dispatch, {rec_leave, rec_busywait, rec_resync_tick}};
}

bool seq_is(std::initializer_list<const char *> want) {
    if (g_ws.seq.size() != want.size()) return false;
    size_t i = 0;
    for (const char *w : want)
        if (std::strcmp(g_ws.seq[i++], w) != 0) return false;
    return true;
}

} // namespace

int run_netsessiontest() {
    printf("=== netsessiontest (NET-SESSION: the session bootstrap + the leader resync frame) ===\n");

    // ---- 1. llm_net_session_globals_reset @0x0049e4c3 --------------------------------------------
    {
        // ALL 22 STORES, each against the value the listing stores. This is the read-back probe's
        // offline twin: the probe proves the LIVE globals hold these after a real boot, this proves
        // the body writes them at all.
        // RULES OUT: a dropped store (its poison survives), two stores swapped (distinct poisons and
        // distinct expected values), and a double written as float or as the wrong constant.
        init_fixture f;
        mh::lockstep::session_globals_reset(f.st);

        check("reset: LOCAL_PLAYER_INDEX = 0, 0x0049e4db", f.local_player_index == 0);
        check("reset: LOCAL_PLAYER_SLOT = 0, 0x0049e4e5", f.local_player_slot == 0);
        check("reset: IS_HOST = 0, 0x0049e4ef", f.is_host == 0);
        check("reset: LOCKSTEP_PLAYER_COUNT = 0, 0x0049e4f9", f.lockstep_player_count == 0);
        check("reset: LOBBY_MAP_RECV_DONE = 0, 0x0049e503", f.lobby_map_recv_done == 0);
        check("reset: SEND_BUF_CURSOR = 0, 0x0049e50d", f.send_buf_cursor == 0);
        check("reset: MP_PROBE_SERVER_COUNT = 0, 0x0049e517", f.probe_server_count == 0);
        check("reset: NET_SESSION_COUNT = 0, 0x0049e521", f.session_count == 0);
        check("reset: LOBBY_SCAN_HOST_COUNT = 0, 0x0049e52b", f.lobby_scan_host_count == 0);

        // The pacing baseline -- the four that make this function OWNERSHIP rather than an
        // exception. Asserted as BIT PATTERNS, which is what the original stores as two immediates.
        check("reset: COMMITTED_HORIZON = 10.0 (0x4024000000000000), 0x0049e535",
              bits(f.committed_horizon) == 0x4024000000000000ull);
        check("reset: HORIZON = 10.0, 0x0049e549", bits(f.horizon) == 0x4024000000000000ull);
        check("reset: STEP_SIZE = 10.0, 0x0049e55d", bits(f.step_size) == 0x4024000000000000ull);
        check("reset: ADAPT_NEXT_TIME = 70.0 (0x4051800000000000), 0x0049e571",
              bits(f.adapt_next_time) == 0x4051800000000000ull);

        check("reset: GAME_SESSION_MODE = 2 (SESSION_MP_LOCAL), 0x0049e585", f.game_session_mode == 2);
        check("reset: SYNC_RETRY_COUNTDOWN = 0x3c, 0x0049e58f", f.sync_retry_countdown == 0x3c);
        check("reset: STALL_NAG_COUNT = 0, 0x0049e599", f.stall_nag_count == 0);
        check("reset: RESYNC_TRIGGER_COUNT = 0, 0x0049e5a3", f.resync_trigger_count == 0);
        check("reset: STALL_COUNT = 0, 0x0049e5ad", f.stall_count == 0);
        check("reset: SYNC_WAIT_ELAPSED = 1.0 (0x3ff0000000000000), 0x0049e5b7",
              bits(f.sync_wait_elapsed) == 0x3ff0000000000000ull);
        check("reset: PEER_TIMEOUT_ELAPSED = -1.0 (0xbff0000000000000), 0x0049e5c1",
              bits(f.peer_timeout_elapsed) == 0xbff0000000000000ull);
        check("reset: SYNC_WAIT_ACTIVE = 0, 0x0049e5df", f.sync_wait_active == 0);
        check("reset: RESYNC_IN_PROGRESS = 0, 0x0049e5e9", f.resync_in_progress == 0);
    }
    {
        // THE SIGN OF THE PEER TIMEOUT IS LOAD-BEARING, and it is the one constant a reader is most
        // likely to "clean up": -1.0 is a SENTINEL ("no peer timing yet"), not a duration. Asserted
        // separately from the bit check above so a failure names the concept, not the pattern.
        // RULES OUT: transcribing 0xbff00000 as +1.0 by dropping the sign bit.
        init_fixture f;
        mh::lockstep::session_globals_reset(f.st);
        check("reset: PEER_TIMEOUT_ELAPSED is NEGATIVE one, not one", f.peer_timeout_elapsed == -1.0);
    }
    {
        // Idempotent: the body reads nothing, so running it twice must land on the same values. This
        // is what makes it safe to promote over a function whose ONE call site we do not control.
        init_fixture f;
        mh::lockstep::session_globals_reset(f.st);
        const double h1 = f.horizon;
        mh::lockstep::session_globals_reset(f.st);
        check("reset is idempotent -- it reads nothing it writes", f.horizon == h1 && h1 == 10.0);
    }

    // ---- 2. llm_wait_screen_frame @0x0043ee38 ----------------------------------------------------
    {
        // NOT THE LEADER: the whole transition is skipped AND the clock is never read (the JZ at
        // 0x0043ee5c jumps before 0x0043ee5e). The dispatch still runs.
        // RULES OUT: evaluating both guards before branching -- which produces the identical end
        // state and would pass any state-only assertion.
        g_ws.reset();
        g_ws.leader_result = 0;
        g_ws.status        = LS_RESYNC_WAIT | LS_HORIZON_PENDING;
        g_ws.in_progress   = 1;
        g_ws.deadline      = 1000;
        g_ws.ticks         = 5000; // WOULD pass the deadline test, if it were reached
        mh::lockstep::wait_screen_frame(ws_state(), ws_calls());
        check("non-leader frame does not read the clock at all", g_ws.tick_calls == 0);
        check("non-leader frame sends nothing and completes nothing",
              g_ws.resume_calls == 0 && g_ws.leave_calls == 0 && g_ws.busywait_calls == 0 &&
                  g_ws.resync_tick_calls == 0);
        check("non-leader frame leaves the resync state alone",
              g_ws.status == (LS_RESYNC_WAIT | LS_HORIZON_PENDING) && g_ws.in_progress == 1);
        check("non-leader frame still pumps the socket", g_ws.dispatch_calls == 1);
        check("non-leader frame's call sequence is leader,dispatch", seq_is({"leader", "dispatch"}));
    }
    {
        // THE LEADER TEST PASSES -1, "exclude nobody" (MOV EAX,0xffffffff at 0x0043ee50). A body
        // that passed the local slot, or 0, would pick a different leader on a full lobby.
        g_ws.reset();
        g_ws.leader_result = 0;
        mh::lockstep::wait_screen_frame(ws_state(), ws_calls());
        check("the leader test excludes NOBODY -- the argument is -1, 0x0043ee50",
              g_ws.leader_calls == 1 && g_ws.leader_arg == -1);
    }
    {
        // THE DEADLINE BOUNDARY, from the side that must NOT act. 0x0043ee69 is JBE: equal is
        // below-or-equal, so ticks == deadline does nothing.
        // RULES OUT: translating JBE as JB (`>=`), which differs from the original on exactly this
        // one input and agrees everywhere else.
        g_ws.reset();
        g_ws.leader_result = 1;
        g_ws.status        = LS_RESYNC_WAIT;
        g_ws.in_progress   = 1;
        g_ws.deadline      = 4000;
        g_ws.ticks         = 4000;
        mh::lockstep::wait_screen_frame(ws_state(), ws_calls());
        check("ticks == deadline does NOT complete the resync (JBE, not JB)",
              g_ws.resume_calls == 0 && g_ws.status == LS_RESYNC_WAIT && g_ws.in_progress == 1);
        check("the clock IS read once when we are the leader", g_ws.tick_calls == 1);
        check("the socket is pumped on the quiet leader frame too", g_ws.dispatch_calls == 1);
    }
    {
        // THE DEADLINE BOUNDARY, from the side that must act: one tick past.
        g_ws.reset();
        g_ws.leader_result = 1;
        g_ws.status        = LS_RESYNC_WAIT;
        g_ws.in_progress   = 1;
        g_ws.deadline      = 4000;
        g_ws.ticks         = 4001;
        mh::lockstep::wait_screen_frame(ws_state(), ws_calls());
        check("ticks == deadline + 1 DOES complete the resync", g_ws.resume_calls == 1);
    }
    {
        // THE COMPARE IS UNSIGNED -- the one that separates the implementations. Deadline near the
        // top of the 32-bit range, clock just past the wrap: unsigned says 0x10 is BELOW
        // 0xFFFFFFF0 (do nothing), a signed reading says 16 > -16 (act). Every ordinary input agrees.
        // RULES OUT: `int32_t` operands, or a helpful "wrap-aware" comparison.
        g_ws.reset();
        g_ws.leader_result = 1;
        g_ws.status        = LS_RESYNC_WAIT;
        g_ws.in_progress   = 1;
        g_ws.deadline      = 0xFFFFFFF0u;
        g_ws.ticks         = 0x00000010u;
        mh::lockstep::wait_screen_frame(ws_state(), ws_calls());
        check("the deadline compare is UNSIGNED across the GetTickCount wrap (JBE on CMP)",
              g_ws.resume_calls == 0 && g_ws.status == LS_RESYNC_WAIT && g_ws.in_progress == 1);
    }
    {
        // THE FULL LEADER TRANSITION: every act once, in order, with the argument the site pushes.
        // The `seq` assertion is the one that can see the ordering -- and the two SAMPLED values are
        // what pin the deliberate deviation documented in net_session.h: when time_resync_and_tick
        // runs, LS_RESYNC_WAIT is ALREADY cleared and RESYNC_IN_PROGRESS is NOT yet zeroed (the RX
        // order). The leader's original zeroes it one step earlier; that is unobservable because
        // nothing in between reads it, and this is where the claim is made checkable rather than
        // asserted in a comment.
        // RULES OUT: dropping any of the five acts, sending after completing, and clearing the whole
        // status byte instead of one bit.
        g_ws.reset();
        g_ws.leader_result = 1;
        g_ws.status        = LS_RESYNC_WAIT | LS_HORIZON_PENDING | LS_SESSION_ENDED;
        g_ws.in_progress   = 1;
        g_ws.deadline      = 1000;
        g_ws.ticks         = 9999;
        mh::lockstep::wait_screen_frame(ws_state(), ws_calls());

        check("leader transition: RESUME goes on the wire first, with the pushed 0.0",
              g_ws.resume_calls == 1 && g_ws.resume_arg == 0.0);
        check("leader transition: each act runs exactly once",
              g_ws.leave_calls == 1 && g_ws.busywait_calls == 1 && g_ws.resync_tick_calls == 1 &&
                  g_ws.dispatch_calls == 1);
        check("leader transition: the order is leader,ticks,resume,leave,busywait,resync_tick,dispatch",
              seq_is({"leader", "ticks", "resume", "leave", "busywait", "resync_tick", "dispatch"}));
        check("leader transition: LS_RESYNC_WAIT is cleared and ONLY it (0x0043ee88, AND 0xbf)",
              (g_ws.status & LS_RESYNC_WAIT) == 0 &&
                  (g_ws.status & (LS_HORIZON_PENDING | LS_SESSION_ENDED)) ==
                      (LS_HORIZON_PENDING | LS_SESSION_ENDED));
        check("leader transition: RESYNC_IN_PROGRESS ends at 0 (0x0043ee7e)", g_ws.in_progress == 0);
        check("leader transition: the flag is ALREADY clear when time_resync_and_tick runs",
              (g_ws.status_at_resync_tick & LS_RESYNC_WAIT) == 0);
        check("leader transition: in-progress is still 1 at time_resync_and_tick (the RX order -- "
              "see net_session.h's deviation note)",
              g_ws.in_progress_at_resync_tick == 1);
    }
    {
        // The dispatch is OUTSIDE the branch (LAB_0043ee94 is the join), so it runs exactly once on
        // the acting path too -- not twice, and not instead of the transition.
        // RULES OUT: putting the pump in an else, or after an early return.
        g_ws.reset();
        g_ws.leader_result = 1;
        g_ws.deadline      = 0;
        g_ws.ticks         = 1;
        mh::lockstep::wait_screen_frame(ws_state(), ws_calls());
        check("the pump runs exactly once on the acting path, after the transition",
              g_ws.dispatch_calls == 1 && std::strcmp(g_ws.seq.back(), "dispatch") == 0);
    }

    printf("=== netsessiontest: %d check(s), %d failure(s) ===\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
