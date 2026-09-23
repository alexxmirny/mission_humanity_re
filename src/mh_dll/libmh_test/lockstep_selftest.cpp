//
// lockstep_selftest.cpp -- `net_selftest.exe lockstest`: the turn engine's horizon/barrier logic over
// heap buffers, with no game and no rig.
//
// WHY THIS EXISTS, in the same terms as orderstest. Shadow mode proves equivalence only on paths a
// run actually reaches, and this batch has several that no scripted match does:
//   * extend_if_near_horizon is called from ONE site, llm_ui_paged_list_frame -- the local player
//     sitting in a paged list during a live match. A determinism run never opens one.
//   * reset_player_horizon's PENDING-PROMOTION branch needs a horizon to have arrived while a slot
//     was resetting, i.e. a join/leave race.
//   * record_peer_horizon's second loop (markers agree?) only runs when the first one found every
//     active peer at the same horizon, and its interesting answer is the DISAGREEING case.
//   * The barrier's behaviour with an AI player in the lobby (ALIVE but not HUMAN, so it must NOT
//     gate the horizon) is a claim about a filter, and the cheapest way to test a filter is to feed
//     it every combination.
//
// It works because mh::lockstep::detail::* takes the engine state as a parameter; the live entry
// points are those same functions applied to state(). Same code, different buffers.
//
#include "include/ctrl_emit.h"
#include "lockstep/net_session.h" // NET-SESSION: the leader end of the resync transition
#include "lockstep/turn_engine.h"
#include "state/host_events.h" // the kick modal's pushed answer (R9)
#include "lockstep/tx_emit.h"
#include "lockstep/tx_emit_chat.h"
#include "lockstep/tx_emit_ctrl.h"

// tx_emit.cpp (the five RI-WIRE emitters this file adds tests for -- see the "RI-WIRE: tests for..."
// block below) is not yet a compile item in mh_nettest.vcxproj; 92f22a7's message says it registered
// tx_emit in both projects, but only mh.vcxproj actually gained it. Pulling in the translation unit
// with an #include here gives net_selftest.exe the same object code without touching a .vcxproj, which
// keeps this task's edit surface to the one file it was scoped to (see the task brief). #pragma once in
// tx_emit.h makes the second inclusion of it (its own #include, right below) a no-op.

#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>
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

using mh::lockstep::dispatch_calls;
using mh::lockstep::dispatch_state;
using mh::lockstep::engine_state;
using mh::lockstep::game_calls;
using mh::lockstep::peer_timing;
using order   = mh::game::mh_llm_strat_order;
using profile = mh::game::mh_llm_strat_player_profile;

constexpr int32_t  N          = mh::lockstep::MAX_PLAYERS;
constexpr uint32_t ALIVE      = mh::lockstep::PLAYER_ALIVE;
constexpr uint32_t HUMAN      = mh::lockstep::PLAYER_HUMAN;
constexpr double   NO_HORIZON = mh::lockstep::HORIZON_NONE;

// The outward calls, recorded. game_calls holds plain function pointers (in production they are the
// generated thunks), so the recorder is a file-scope singleton rather than a capture.
struct call_log {
    int                 sends             = 0;
    double              last_sent         = 0.0;
    int                 sim_steps         = 0; // how many times the sim was stepped -- the catch-up loop's real output
    int                 ambient           = 0;
    int                 advisors          = 0;
    int                 prints            = 0;
    int                 sprintfs          = 0;
    int                 dispatches        = 0;
    int                 due_checks        = 0;
    int                 schedules         = 0;
    int                 time_ticks        = 0;
    int                 resync_snapshot   = 0;
    int                 resync_units      = 0;
    double              last_resync_units = 0.0;
    std::vector<double> released;            // the `now` of every order_release_due call, in order
    int32_t             alert_planet   = -1; // what invasion_alert_poll is rigged to answer
    int32_t             sprintf_planet = -1; // what sprintf_alert_text was actually handed
    void                reset() { *this = call_log{}; }
};
call_log g_log;

const game_calls &recording_calls() {
    static const game_calls gc = {
        [](double h) { ++g_log.sends; g_log.last_sent = h; },
        []() { ++g_log.sim_steps; },
        []() { ++g_log.ambient; },
        [](double) { ++g_log.advisors; },
        [](void *) { ++g_log.prints; },
        [](double now) -> int32_t { g_log.released.push_back(now); return 0; },
        [](double) -> int32_t { return g_log.alert_planet; },
        []() -> int32_t { ++g_log.due_checks; return 0; },
        [](int32_t p) { ++g_log.sprintfs; g_log.sprintf_planet = p; },
        []() -> int32_t { ++g_log.time_ticks; return 0; },
        [](double) { ++g_log.resync_snapshot; },
        [](double now) { ++g_log.resync_units; g_log.last_resync_units = now; },
        []() { ++g_log.dispatches; },
        []() -> int32_t { ++g_log.schedules; return 0; },
    };
    return gc;
}

// ---- batch C: the RX dispatcher's outward calls, recorded ----------------------------------------
//
// Every handler's observable effect is either a state write or one of these calls, so the recorder is
// half the oracle. Two entries are RIGGED rather than counted, because the original BRANCHES on them:
// is_local_leader_peer and count_active_players.
struct rx_log {
    std::vector<int32_t> acks, desyncs, status_resets, timeout_drops, removed, leader_asked;
    std::vector<double>  extends;
    std::vector<int32_t> presence_lost; // player index per call
    // ...and its SECOND argument, recorded rather than discarded (U19h). The comment this vector
    // replaced asserted "mode is always 1 in this function" in prose; U19h's chain turns on that 1 --
    // sim_player_presence_lost picks outcome 8 for mode != 0 and 6 for mode == 0 -- so the claim now
    // has to be checkable from a test rather than trusted from a comment.
    std::vector<uint32_t> presence_modes;
    std::vector<order>    enqueued, integrity_checked;
    std::vector<int32_t>  outcomes;
    int                   busywaits = 0, time_resyncs = 0, force_resyncs = 0;
    int                   chat_recalcs = 0, leave_resets = 0;
    int                   prints_red = 0, prints_cyan = 0, concats = 0;
    int                   player_lines = 0, chat_lines = 0, plain_lines = 0;
    int32_t               last_text_id = -1;
    int                   fills        = 0;

    // rigged answers
    int32_t leader_answer = 0; // what is_local_leader_peer returns
    int32_t active_answer = 2; // what count_active_players returns
    // player_by_side_id is modelled, not rigged: side_id 100+i -> i (matching world's fixture), and
    // anything else -> 0, which is what makes a handler that passes the WRONG id visibly wrong.
    void reset() { *this = rx_log{}; }
};
rx_log g_rx;

int32_t side_to_index(int32_t side_id) {
    return (side_id >= 100 && side_id < 100 + N) ? side_id - 100 : 0;
}

const dispatch_calls &recording_dispatch_calls() {
    static const dispatch_calls dc = {
        // transport_recv is never called from dispatch_packet -- only from the dispatch() wrapper,
        // which these tests deliberately do not exercise (it is the un-testable half).
        [](int32_t *, void *, int32_t *) -> int32_t { return 0; },
        [](const order *o, char *) { g_rx.integrity_checked.push_back(*o); },
        [](const order *o) -> int32_t { g_rx.enqueued.push_back(*o); return 0; },
        [](int32_t excl) -> int32_t { g_rx.leader_asked.push_back(excl); return g_rx.leader_answer; },
        []() { ++g_rx.force_resyncs; },
        [](double h) { g_rx.extends.push_back(h); },
        [](int32_t s) -> int32_t { return side_to_index(s); },
        [](int32_t s) { g_rx.acks.push_back(s); },
        [](int32_t s) { g_rx.desyncs.push_back(s); },
        [](int32_t s) { g_rx.status_resets.push_back(s); },
        [](int32_t s) { g_rx.timeout_drops.push_back(s); },
        [](uint32_t p, uint32_t mode) -> uint32_t {
            g_rx.presence_lost.push_back(static_cast<int32_t>(p));
            g_rx.presence_modes.push_back(mode);
            return 0;
        },
        []() -> int32_t { return g_rx.active_answer; },
        [](int32_t s) { g_rx.removed.push_back(s); },
        []() -> int32_t { ++g_rx.busywaits; return 0; },
        []() { ++g_rx.time_resyncs; },
        []() { ++g_rx.leave_resets; },
        []() { ++g_rx.chat_recalcs; },
        [](char *s) -> void * { return s; }, // the "wide" scratch: identity, so a test can still tell
                                             // WHICH string was widened
        [](void *src, void *dst) -> void * { return dst ? dst : src; },
        [](void *dst, void *) -> void * { ++g_rx.concats; return dst; },
        [](void *p, uint32_t n, uint8_t f) -> void * { ++g_rx.fills; std::memset(p, f, n); return p; },
        [](uint8_t o) -> int32_t { g_rx.outcomes.push_back(o); return 1; },
        [](void *) { ++g_rx.prints_red; },
        [](void *) { ++g_rx.prints_cyan; },
        [](int32_t id, void *) { ++g_rx.player_lines; g_rx.last_text_id = id; },
        [](void *, void *) { ++g_rx.chat_lines; },
        [](int32_t id) { ++g_rx.plain_lines; g_rx.last_text_id = id; },
    };
    return dc;
}

// A whole turn engine on the heap. Deliberately NOT zero-initialised where the game would not be:
// the horizon tables start at the -1.0 sentinel peer_timing_reset writes.
struct world {
    double               horizon = 0.0, committed = 0.0;
    double               peer_horizon[N]{}, peer_pending[N]{};
    peer_timing          peer[N]{};
    uint8_t              peer_state[N * N]{};
    std::vector<profile> players{static_cast<size_t>(N)};
    uint16_t             side    = 0;
    double               clock   = 0.0;
    int32_t              count   = 0;
    double               margin1 = 0.0, margin2 = 0.0;
    double               margin_mul = 1.0, step_mul = 1.0;
    // ---- batch B ----
    double  delta = 0.0, total = 0.0, sub_step = 1.0, step_size = 0.0, refill = 0.5;
    int32_t mode = 0, pending = 0, staging = 0, msg_active = 0;
    char    text_buf[16]{};

    world() {
        std::memset(players.data(), 0, players.size() * sizeof(profile));
        for (int32_t i = 0; i < N; ++i) {
            peer_horizon[i]      = NO_HORIZON;
            peer_pending[i]      = NO_HORIZON;
            peer[i].horizon      = NO_HORIZON;
            peer[i].order_marker = 0;
            players[i].side_id   = 100 + i; // distinguishable from any index
        }
    }
    void human(int32_t i, bool on = true) { players[i].status_flags = on ? (ALIVE | HUMAN) : 0; }
    void ai(int32_t i) { players[i].status_flags = ALIVE; } // ALIVE but not HUMAN

    engine_state st() {
        return engine_state{&horizon, &committed, peer_horizon, peer_pending, peer,
                            peer_state, players.data(), &side, &clock, &count,
                            &margin1, &margin2, &margin_mul, &step_mul,
                            // batch B
                            &clock, &delta, &total, &sub_step, &mode,
                            &pending, &staging, &step_size, &refill, &msg_active,
                            text_buf};
    }

    // ---- batch C: the RX dispatcher's own globals -------------------------------------------------
    //
    // These carry MORE weight than the batch A/B fixtures do, because llm_net_lockstep_dispatch has no
    // shadow site: it drains the socket, so its input is consumed by reading and the two arms can
    // never see the same input. Crafted packets fed through detail::dispatch_packet are the only
    // evidence this function will ever have.
    uint8_t buf[0x400]{};
    int32_t send_cursor = 0; // the RX path never touches it; bound so packet_buffer is whole
    uint8_t status      = 0;
    int32_t local_side  = 0;
    int32_t active      = 2;
    int32_t ls_players = 2, lobby_hosts = 2;
    double  adapt_next     = 0.0;
    int32_t resync_trigger = 0, stall_nag = 0, stalls = 0, resync_busy = 0;
    // SYNC_RETRY_COUNTDOWN. 0x3c (60) is its reloaded value, i.e. the ROUTINE at-horizon state --
    // the one the C3 resync_trigger_gate must NOT count. A test that wants the gate to let a nag
    // through drops it below SYNC_OVERLAY_AFTER (0x38).
    int32_t retry_countdown = 0x3c;
    int32_t debug_tap       = 0;
    // The FIX AUDIT tally: how many times the gate predicate was EVALUATED, and how many of those it
    // ALLOWED. Diagnostic, not game state -- see dispatch_state.
    int32_t audit_ev = 0, audit_ok = 0;
    double  emergency_mul = 2.0;
    uint8_t msg_queue[0x200]{};
    void   *text_ptr_slots[1024]{};
    // SB-BIND T4. The two members appended to dispatch_state. Bound to real storage rather than left
    // to their `= nullptr` defaults for the same reason the audit tally below is bound: a handler
    // that formats into the scratch and prints it would hand the stub a null and every assertion
    // about the message would still hold, because none of them look at the buffer. A default that
    // makes a test vacuous is worse than one that makes it crash.
    uint8_t text_scratch[0x200]{};
    char    netgame_tag[16] = "NetgameRead";

    dispatch_state ds() {
        // players_w MUST alias engine_state::players -- participates() reads through the const view
        // while the eliminate sweeps write through this one, so a second buffer would make every
        // sweep test stale flags and pass for the wrong reason.
        // The last two are the FIX AUDIT tally. Bound here rather than left null on purpose: the C3
        // equivalence run reads those counters, so an audit that silently failed to count would make
        // the run vacuous while looking like agreement -- the same shape as every other gate this
        // project has watched pass for the wrong reason.
        return dispatch_state{mh::net::packet_buffer{buf, &send_cursor},
                              &status, players.data(), &local_side,
                              &active, &ls_players, &lobby_hosts, &step_size,
                              &adapt_next, &resync_trigger, &retry_countdown, &stall_nag,
                              &stalls, &resync_busy, &debug_tap, &emergency_mul,
                              msg_queue, text_ptr_slots, &audit_ev, &audit_ok,
                              text_scratch, netgame_tag};
    }
};

// A wire packet, built field by field in the order the handlers read them. Deliberately NOT a struct
// per message kind: the point of these tests is the CURSOR ARITHMETIC, so the bytes are laid down
// explicitly and a handler that reads a field in the wrong order or advances by the wrong width shows
// up as garbage in the NEXT message rather than as a silently-tolerated mis-parse.
struct packet {
    std::vector<uint8_t> b;

    packet &u8(uint8_t v) {
        b.push_back(v);
        return *this;
    }
    packet &i32(int32_t v) { return blob(&v, sizeof(v)); }
    packet &f64(double v) { return blob(&v, sizeof(v)); }
    packet &blob(const void *p, size_t n) {
        const auto *q = static_cast<const uint8_t *>(p);
        b.insert(b.end(), q, q + n);
        return *this;
    }
    // tag 4 + an inner control tag, the two-byte header every CTL_* message opens with
    packet &ctl(uint8_t inner) { return u8(mh::lockstep::MSG_CONTROL).u8(inner); }
};

// Copy a crafted packet into the world's buffer and run the dispatcher over it, as the recv wrapper
// would. `sender_side` is the transport's sender id, NOT a player index -- several handlers use the
// two for different things and swapping them is one of the mistakes these tests exist to catch.
bool feed(world &w, const packet &p, int32_t sender_side = 100,
          const mh::lockstep::reimpl_fixes &fx = mh::lockstep::reimpl_fixes{}) {
    std::memset(w.buf, 0xcd, sizeof(w.buf)); // poison, so a handler that over-reads its field shows up
    std::memcpy(w.buf, p.b.data(), p.b.size());
    // The DEFAULT argument is an all-off reimpl_fixes, i.e. the faithful stock behaviour -- so every
    // pre-existing test keeps asserting what the ORIGINAL does, and only the tests that pass a fix
    // explicitly are asserting our deliberate divergence (C3).
    const auto r = mh::lockstep::detail::dispatch_packet(w.st(), w.ds(), recording_dispatch_calls(), fx,
                                                         sender_side,
                                                         static_cast<uint32_t>(p.b.size()));
    return r == mh::lockstep::detail::packet_result::drain_again;
}

// ---- the barrier ---------------------------------------------------------------------------------

void test_commit_horizon() {
    { // solo: nothing else participates, so the barrier is our own request
        world w;
        w.human(0);
        w.side    = 0;
        w.horizon = 12.5;
        mh::lockstep::detail::commit_horizon(w.st());
        check("solo commits own horizon", w.committed == 12.5);
    }
    { // two humans: the barrier is the MINIMUM, and it is the peer that holds it back
        world w;
        w.human(0);
        w.human(1);
        w.side            = 0;
        w.horizon         = 20.0;
        w.peer_horizon[1] = 8.0;
        mh::lockstep::detail::commit_horizon(w.st());
        check("barrier takes the lagging peer", w.committed == 8.0);

        w.peer_horizon[1] = 30.0;
        mh::lockstep::detail::commit_horizon(w.st());
        check("barrier never exceeds our own request", w.committed == 20.0);
    }
    { // the SELF slot is skipped -- it is never written, so it still holds the sentinel. If the
        // filter forgot `!= PlayerSide`, committed would collapse to -1.0 and the sim would stop dead.
        world w;
        w.human(0);
        w.human(1);
        w.side            = 0;
        w.horizon         = 20.0;
        w.peer_horizon[0] = NO_HORIZON; // our own slot, as the game leaves it
        w.peer_horizon[1] = 15.0;
        mh::lockstep::detail::commit_horizon(w.st());
        check("self slot is not part of the barrier", w.committed == 15.0);
    }
    { // an AI player is ALIVE but not HUMAN and must NOT gate the horizon
        world w;
        w.human(0);
        w.ai(2);
        w.side            = 0;
        w.horizon         = 20.0;
        w.peer_horizon[2] = 1.0;
        mh::lockstep::detail::commit_horizon(w.st());
        check("an AI player does not gate the barrier", w.committed == 20.0);
    }
    { // a dead (not ALIVE) human is likewise out, which is what lets a match continue after a drop
        world w;
        w.human(0);
        w.human(1);
        w.players[1].status_flags = HUMAN; // dropped: HUMAN kept, ALIVE cleared
        w.side                    = 0;
        w.horizon                 = 20.0;
        w.peer_horizon[1]         = 1.0;
        mh::lockstep::detail::commit_horizon(w.st());
        check("an eliminated peer does not gate the barrier", w.committed == 20.0);
    }
}

// ---- the keepalive -------------------------------------------------------------------------------

void test_extend_if_near_horizon() {
    { // clock is far behind the horizon: nothing happens, and nothing is advertised
        world w;
        w.human(0);
        w.clock      = 1.0;
        w.horizon    = 100.0;
        w.margin_mul = 2.0;
        w.step_mul   = 5.0;
        g_log.reset();
        mh::lockstep::detail::extend_if_near_horizon(w.st(), recording_calls(), 3.0);
        check("no extend while the horizon is far", w.horizon == 100.0 && g_log.sends == 0);
    }
    { // clock + scale*margin_mul passes the horizon: bump by scale*STEP_MUL, advertise, re-commit.
        // The two multipliers are DIFFERENT globals; using margin_mul for the bump would give 106.
        world w;
        w.human(0);
        w.side       = 0;
        w.clock      = 95.0;
        w.horizon    = 100.0;
        w.margin_mul = 2.0;
        w.step_mul   = 5.0;
        g_log.reset();
        mh::lockstep::detail::extend_if_near_horizon(w.st(), recording_calls(), 3.0);
        check("extend bumps by scale * STEP_MUL", w.horizon == 115.0);
        check("extend advertises the NEW horizon", g_log.sends == 1 && g_log.last_sent == 115.0);
        check("extend re-commits the barrier", w.committed == 115.0);
    }
    { // the test is strictly-greater: exactly reaching the horizon does NOT extend
        world w;
        w.human(0);
        w.clock      = 94.0;
        w.horizon    = 100.0;
        w.margin_mul = 2.0;
        w.step_mul   = 5.0;
        g_log.reset();
        mh::lockstep::detail::extend_if_near_horizon(w.st(), recording_calls(), 3.0);
        check("exact reach does not extend", w.horizon == 100.0 && g_log.sends == 0);
    }
    { // both margins participate in the sum. They are 0.0 in the shipped image; this is the test
        // that would catch a reimplementation that dropped them if that ever stopped being true.
        world w;
        w.human(0);
        w.clock      = 90.0;
        w.horizon    = 100.0;
        w.margin1    = 3.0;
        w.margin2    = 4.0;
        w.margin_mul = 1.0;
        w.step_mul   = 1.0;
        g_log.reset();
        mh::lockstep::detail::extend_if_near_horizon(w.st(), recording_calls(), 4.0);
        check("both margins count toward the reach", w.horizon == 104.0 && g_log.sends == 1);
    }
}

// ---- who is holding the barrier ------------------------------------------------------------------

void test_find_horizon_match_side() {
    world w;
    w.human(0);
    w.human(1);
    w.human(3);
    w.side            = 0;
    w.committed       = 42.0;
    w.peer_horizon[1] = 99.0;
    w.peer_horizon[3] = 42.0;
    check("finds the peer sitting exactly on the barrier",
          mh::lockstep::detail::find_horizon_match_side(w.st()) == w.players[3].side_id);

    w.peer_horizon[3] = 43.0;
    check("no match answers 0", mh::lockstep::detail::find_horizon_match_side(w.st()) == 0);

    // it returns side_id, NOT the slot index -- the two are equal in most real sessions, which is
    // exactly why a mix-up would survive a rig run
    w.peer_horizon[1]    = 42.0;
    w.players[1].side_id = 7;
    check("returns side_id, not the slot index",
          mh::lockstep::detail::find_horizon_match_side(w.st()) == 7);

    // self is skipped even when its slot happens to match
    world v;
    v.human(0);
    v.side            = 0;
    v.committed       = 5.0;
    v.peer_horizon[0] = 5.0;
    check("self is never reported as holding the barrier",
          mh::lockstep::detail::find_horizon_match_side(v.st()) == 0);
}

// ---- the RX side ---------------------------------------------------------------------------------

void test_record_peer_horizon() {
    { // A stale or REPEATED horizon is rejected without storing, and answers 1.
        //
        // THE SETUP IS RIGGED so the RETURN VALUE separates the two implementations, which the first
        // version of this test did not do: peer 0 sits at the same horizon with a DISAGREEING marker,
        // so an implementation that wrongly ACCEPTED the equal horizon would store it, find every peer
        // at that horizon, hit the marker loop and answer 0. Rejecting answers 1. (Learned from the
        // mutation run: with `<` for `<=` the old assertion still passed and only the neighbouring
        // "stores nothing" check went red -- the mutation worked, the assertion was the wrong
        // instrument.)
        world w;
        w.human(0);
        w.human(1);
        w.side                 = 0;
        w.peer[0].horizon      = 10.0;
        w.peer[0].order_marker = 7;
        w.peer[1].horizon      = 10.0;
        w.peer[1].order_marker = 4;
        const int32_t r        = mh::lockstep::detail::record_peer_horizon(w.st(), 1, 10.0, 9);
        check("an equal horizon is rejected", r == 1);
        check("a rejected horizon stores nothing",
              w.peer[1].horizon == 10.0 && w.peer[1].order_marker == 4);
        check("an older horizon is rejected too",
              mh::lockstep::detail::record_peer_horizon(w.st(), 1, 3.0, 9) == 1 &&
                  w.peer[1].horizon == 10.0);
    }
    { // everyone at the same horizon and the same marker -> in sync
        world w;
        w.human(0);
        w.human(1);
        w.side                 = 0;
        w.peer[0].horizon      = 20.0;
        w.peer[0].order_marker = 7;
        w.peer[1].horizon      = 10.0;
        w.peer[1].order_marker = 7;
        check("all agreed answers 1", mh::lockstep::detail::record_peer_horizon(w.st(), 1, 20.0, 7) == 1);
        check("the new horizon is stored", w.peer[1].horizon == 20.0);
    }
    { // same horizon, DIFFERENT order marker -> the desync case the second loop exists for
        world w;
        w.human(0);
        w.human(1);
        w.side                 = 0;
        w.peer[0].horizon      = 20.0;
        w.peer[0].order_marker = 7;
        w.peer[1].horizon      = 10.0;
        w.peer[1].order_marker = 7;
        check("same time but a different order stream answers 0",
              mh::lockstep::detail::record_peer_horizon(w.st(), 1, 20.0, 99) == 0);
        check("the marker is stored even so", w.peer[1].order_marker == 99);
    }
    { // one peer NOT yet at the horizon -> the marker loop is skipped entirely and the answer is 1,
        // even though the markers disagree. Short-circuit, not an accident.
        world w;
        w.human(0);
        w.human(1);
        w.human(2);
        w.side                 = 0;
        w.peer[0].horizon      = 20.0;
        w.peer[0].order_marker = 7;
        w.peer[1].horizon      = 5.0;
        w.peer[1].order_marker = 7;
        w.peer[2].horizon      = 1.0; // still behind
        w.peer[2].order_marker = 123; // and disagreeing
        check("a lagging peer suppresses the marker check",
              mh::lockstep::detail::record_peer_horizon(w.st(), 1, 20.0, 7) == 1);
    }
    { // an AI slot is neither at the horizon nor consulted
        world w;
        w.human(0);
        w.ai(2);
        w.side                 = 0;
        w.peer[0].horizon      = 20.0;
        w.peer[0].order_marker = 7;
        w.peer[2].horizon      = NO_HORIZON;
        w.peer[2].order_marker = 555;
        check("an AI slot is ignored by both loops",
              mh::lockstep::detail::record_peer_horizon(w.st(), 0, 20.0, 7) == 1);
    }
}

// ---- slot reset ----------------------------------------------------------------------------------

void test_reset_player_horizon() {
    world w;
    w.human(0);
    w.human(1);
    w.side    = 0;
    w.horizon = 50.0;
    for (int32_t i = 0; i < N * N; ++i) w.peer_state[i] = 0xAB;
    w.peer_pending[1] = 33.0; // arrived while the slot was resetting -> promote it
    w.peer_pending[2] = 0.0;  // not positive -> not promoted
    w.peer_horizon[1] = 9.0;
    w.peer_horizon[2] = 9.0;

    mh::lockstep::detail::reset_player_horizon(w.st(), 3);

    check("a positive pending horizon is promoted", w.peer_horizon[1] == 33.0);
    check("a zero pending horizon is not promoted", w.peer_horizon[2] == 9.0);
    bool rearmed = true;
    for (int32_t i = 0; i < N; ++i) rearmed = rearmed && w.peer_pending[i] == NO_HORIZON;
    check("every pending slot is re-armed to the sentinel", rearmed);

    // ONLY the argument's row is cleared. Getting this wrong (clearing the column, or the whole
    // matrix) is invisible in a 2-player run where row 1 and column 1 look alike.
    bool row_cleared = true, others_intact = true;
    for (int32_t i = 0; i < N; ++i) {
        row_cleared   = row_cleared && w.peer_state[3 * N + i] == 0;
        others_intact = others_intact && w.peer_state[2 * N + i] == 0xAB;
    }
    check("the argument's peer-state ROW is cleared", row_cleared);
    check("other rows are untouched", others_intact);
    check("the barrier is re-run over the promoted horizons", w.committed == 33.0);
}

// ---- x87 unordered-compare semantics -------------------------------------------------------------
//
// The three cases the adversarial review found (two of them live divergences). Every double compare
// in the original is FCOMP + SAHF + a Jcc, and the UNORDERED result sets ZF=CF=PF at once, so a NaN
// counts as "equal" under JZ and as "below" under JNC while C++ comparisons are false on all of them.
// These tests exist to pin which way each branch goes, because the answer is per-Jcc and not
// guessable from the C.
void test_nan_semantics() {
    const double nan = std::numeric_limits<double>::quiet_NaN();

    { // JNZ in find_horizon_match_side: unordered counts as a MATCH
        world w;
        w.human(0);
        w.human(1);
        w.side            = 0;
        w.committed       = nan;
        w.peer_horizon[1] = 5.0;
        check("NaN committed still reports a peer holding the barrier (JNZ: unordered == equal)",
              mh::lockstep::detail::find_horizon_match_side(w.st()) == w.players[1].side_id);
    }
    { // JZ in record_peer_horizon's first loop: unordered does NOT clear all_at_horizon, so the
        // marker loop still runs and can still report the desync
        world w;
        w.human(0);
        w.human(1);
        w.side                 = 0;
        w.peer[0].horizon      = 1.0;
        w.peer[0].order_marker = 7;
        w.peer[1].horizon      = 0.0;
        w.peer[1].order_marker = 7;
        check("a NaN horizon does not suppress the marker check (JZ: unordered == equal)",
              mh::lockstep::detail::record_peer_horizon(w.st(), 1, nan, 99) == 0);
    }
    { // JNC in reset_player_horizon: unordered TAKES the promotion branch
        world w;
        w.human(0);
        w.peer_pending[1] = nan;
        w.peer_horizon[1] = 9.0;
        mh::lockstep::detail::reset_player_horizon(w.st(), 0);
        check("a NaN pending horizon IS promoted (JNC: unordered is 'below')",
              w.peer_horizon[1] != w.peer_horizon[1]); // i.e. it is now NaN
    }
    { // JBE in commit_horizon and extend_if_near_horizon: unordered SKIPS, which is what plain `>`
        // already does -- the case that needed no helper, asserted so a future "consistency" edit that
        // wrapped these in a helper too would fail here.
        world w;
        w.human(0);
        w.human(1);
        w.side            = 0;
        w.horizon         = 20.0;
        w.peer_horizon[1] = nan;
        mh::lockstep::detail::commit_horizon(w.st());
        check("a NaN peer horizon does NOT lower the barrier (JBE: unordered skips)", w.committed == 20.0);
    }
}

// ---- batch B: the clock --------------------------------------------------------------------------
//
// sim_tick and advance_sim_clock also have shadow sites; pump and sim_clock_advance do NOT and cannot
// (each branches on, or reads the output of, a call their shadow arm is forbidden to make), so for
// those two these tests are the ONLY evidence there will ever be. They are written accordingly.

// All-off fixes = faithful stock behaviour, which is what every pre-existing sim_tick case asserts.
const mh::lockstep::reimpl_fixes &no_fixes() {
    static const mh::lockstep::reimpl_fixes f{};
    return f;
}

void test_sim_tick_lockstep() {
    { // the catch-up loop takes WHOLE sub-steps and leaves the remainder on the clock. This is the
        // clamp-discard: TOTAL_GAME_TIME has already been clipped to the committed horizon, so the
        // leftover fraction is carried rather than simulated.
        world w;
        w.mode     = mh::lockstep::SESSION_MP_LOCKSTEP;
        w.clock    = 0.0;
        w.sub_step = 0.02;
        w.total    = 0.07; // three whole sub-steps fit, 0.01 is left over
        g_log.reset();
        mh::lockstep::detail::sim_tick(w.st(), recording_calls(), no_fixes());
        check("mode 3 takes only WHOLE sub-steps", g_log.sim_steps == 3);
        check("mode 3 leaves the remainder on the clock", w.clock > 0.0599 && w.clock < 0.0601);
        check("mode 3 sets the delta to one sub-step", w.delta == 0.02);
    }
    { // exactly on the boundary: `clock + sub_step <= total` is <=, so a step that lands exactly on
        // the target still runs. An implementation using `<` would run one fewer.
        world w;
        w.mode     = mh::lockstep::SESSION_MP_LOCKSTEP;
        w.sub_step = 0.25;
        w.total    = 0.5;
        g_log.reset();
        mh::lockstep::detail::sim_tick(w.st(), recording_calls(), no_fixes());
        check("an exactly-fitting sub-step still runs", g_log.sim_steps == 2 && w.clock == 0.5);
    }
    { // nothing owed -> no steps at all, and the delta is NOT touched in mode 3
        world w;
        w.mode     = mh::lockstep::SESSION_MP_LOCKSTEP;
        w.clock    = 5.0;
        w.total    = 5.0;
        w.sub_step = 0.02;
        w.delta    = 1234.0;
        g_log.reset();
        mh::lockstep::detail::sim_tick(w.st(), recording_calls(), no_fixes());
        check("no time owed -> no sim steps", g_log.sim_steps == 0);
        check("mode 3 leaves the delta alone when it does not step", w.delta == 1234.0);
    }
    { // release_due is called ONCE PER ITERATION, with the ALREADY-ADVANCED clock, and only while
        // there are pending orders. Both halves matter: the order lands at the step it is due at.
        world w;
        w.mode     = mh::lockstep::SESSION_MP_LOCKSTEP;
        w.sub_step = 1.0;
        w.total    = 3.0;
        w.pending  = 1;
        g_log.reset();
        mh::lockstep::detail::sim_tick(w.st(), recording_calls(), no_fixes());
        check("release_due runs once per iteration", g_log.released.size() == 3);
        check("release_due sees the ADVANCED clock",
              g_log.released.size() == 3 && g_log.released[0] == 1.0 && g_log.released[2] == 3.0);

        world v;
        v.mode     = mh::lockstep::SESSION_MP_LOCKSTEP;
        v.sub_step = 1.0;
        v.total    = 3.0;
        v.pending  = 0; // nothing queued
        g_log.reset();
        mh::lockstep::detail::sim_tick(v.st(), recording_calls(), no_fixes());
        check("release_due is skipped when nothing is pending", g_log.released.empty());
        check("...but the sim still steps", g_log.sim_steps == 3);
    }
}

// The RIG shape: [harness] rig_fixed_step_loop forces the mode-3 catch-up loop in single-player, so
// the SP oracle integrates at the same fixed sub-step as MP. These cases are the only evidence for it
// that does not need a game -- and the LAST one is the point of the whole knob.
void test_sim_tick_rig_fixed_step_loop() {
    mh::lockstep::reimpl_fixes rig;
    rig.rig_fixed_step_loop = true;

    { // mode 2 + the knob behaves EXACTLY like mode 3: whole sub-steps, remainder carried.
        world w;
        w.mode     = 2;
        w.clock    = 0.0;
        w.sub_step = 0.02;
        w.total    = 0.07;
        g_log.reset();
        mh::lockstep::detail::sim_tick(w.st(), recording_calls(), rig);
        check("rig loop takes only WHOLE sub-steps in SP", g_log.sim_steps == 3);
        check("rig loop carries the remainder", w.clock > 0.0599 && w.clock < 0.0601);
        check("rig loop pins the delta to one sub-step", w.delta == 0.02);
    }
    { // AND the same fixture WITHOUT the knob still takes the single variable step. Without this the
        // three checks above would pass against a build where mode 2 had been broken outright.
        world w;
        w.mode     = 2;
        w.clock    = 0.0;
        w.sub_step = 0.02;
        w.total    = 0.07;
        g_log.reset();
        mh::lockstep::detail::sim_tick(w.st(), recording_calls(), no_fixes());
        check("knob OFF leaves SP on the one-variable-step branch", g_log.sim_steps == 1);
        check("knob OFF still jumps straight to the target", w.clock == 0.07);
    }
    { // the knob must not disturb mode 3, which already takes that branch.
        world w;
        w.mode     = mh::lockstep::SESSION_MP_LOCKSTEP;
        w.sub_step = 0.25;
        w.total    = 0.5;
        g_log.reset();
        mh::lockstep::detail::sim_tick(w.st(), recording_calls(), rig);
        check("rig loop is a no-op in mode 3", g_log.sim_steps == 2 && w.clock == 0.5);
    }
    { // release_due: stock SP NEVER calls it (sim_tick's mode-3 branch is its only caller in the whole
        // image), so forcing the loop starts calling it. Measured on the SP oracle, `order_pending`
        // never changes over 15000 steps -- nothing reaches the pending lane -- so the guard keeps it
        // inert. Assert BOTH halves: inert when nothing is pending, and still correct if something is.
        world w;
        w.mode     = 2;
        w.sub_step = 1.0;
        w.total    = 3.0;
        w.pending  = 0;
        g_log.reset();
        mh::lockstep::detail::sim_tick(w.st(), recording_calls(), rig);
        check("rig loop does not release when nothing is pending", g_log.released.empty());
        check("...but it still steps", g_log.sim_steps == 3);

        world v;
        v.mode     = 2;
        v.sub_step = 1.0;
        v.total    = 3.0;
        v.pending  = 1;
        g_log.reset();
        mh::lockstep::detail::sim_tick(v.st(), recording_calls(), rig);
        check("rig loop releases once per iteration when orders ARE pending",
              g_log.released.size() == 3);
        check("rig loop releases against the ADVANCED clock",
              g_log.released.size() == 3 && g_log.released[0] == 1.0 && g_log.released[2] == 3.0);
    }
    { // THE REASON THE KNOB EXISTS: in the forced loop the step SEQUENCE is invariant to game speed --
        // a 10x-faster clock produces 10x MORE steps of the SAME size, not 10 steps of 10x the size.
        // That is what makes a fast run's golden comparable with a 1x run's, which mode 2 cannot do.
        world slow;
        slow.mode     = 2;
        slow.sub_step = 0.1;
        slow.total    = 1.0; // one frame's worth of clock at 1x
        g_log.reset();
        mh::lockstep::detail::sim_tick(slow.st(), recording_calls(), rig);
        const int    slow_steps = g_log.sim_steps;
        const double slow_delta = slow.delta;

        world fast;
        fast.mode     = 2;
        fast.sub_step = 0.1;
        fast.total    = 10.0; // the same frame at 10x game speed
        g_log.reset();
        mh::lockstep::detail::sim_tick(fast.st(), recording_calls(), rig);
        check("10x runs 10x MORE steps", g_log.sim_steps == slow_steps * 10);
        check("...each of the SAME size", fast.delta == slow_delta && fast.delta == 0.1);
    }
}

void test_sim_tick_singleplayer() {
    { // the non-lockstep branch takes ONE variable step straight to the target
        world w;
        w.mode  = 2; // anything but SESSION_MP_LOCKSTEP
        w.clock = 1.0;
        w.total = 9.5;
        g_log.reset();
        mh::lockstep::detail::sim_tick(w.st(), recording_calls(), no_fixes());
        check("single-player takes one variable step", g_log.sim_steps == 1);
        check("single-player jumps straight to the target", w.clock == 9.5);
        check("single-player delta is the whole remainder", w.delta == 8.5);
        check("single-player never calls release_due", g_log.released.empty());
    }
    { // a non-positive delta steps nothing -- and the delta is still written, which is the part a
        // "tidier" implementation would move inside the if.
        //
        // delta is SEEDED with a marker rather than left at 0: the first version of this check asserted
        // `delta == 0.0` against a fixture that already held 0.0, so "wrote 0" and "did not write" were
        // the same observation and the mutation that moved the write inside the if went UNCAUGHT.
        world w;
        w.mode  = 2;
        w.clock = 9.5;
        w.total = 9.5;
        w.delta = 777.0; // must be overwritten with 0.0
        g_log.reset();
        mh::lockstep::detail::sim_tick(w.st(), recording_calls(), no_fixes());
        check("a zero delta does not step", g_log.sim_steps == 0);
        check("the delta is written even when it does not step", w.delta == 0.0);
        check("a zero delta leaves the clock alone", w.clock == 9.5);
    }
}

void test_sim_tick_tail() {
    { // the tail runs on EVERY call, stepping or not, and in order
        world w;
        w.mode  = 2;
        w.clock = 5.0;
        w.total = 5.0;
        g_log.reset();
        g_log.alert_planet = -1;
        mh::lockstep::detail::sim_tick(w.st(), recording_calls(), no_fixes());
        check("ambient tick always runs", g_log.ambient == 1);
        check("advisor tick always runs", g_log.advisors == 1);
        check("no alert -> nothing printed", g_log.prints == 0 && g_log.sprintfs == 0);
        check("no alert -> the floating-message flag is untouched", w.msg_active == 0);
    }
    { // planet 0 is a VALID alert: the test is `-1 < planet`, not `0 < planet`. Using the wrong one
        // would silently drop every alert for the first planet.
        world w;
        w.mode  = 2;
        w.clock = 5.0;
        w.total = 5.0;
        g_log.reset();
        g_log.alert_planet = 0;
        mh::lockstep::detail::sim_tick(w.st(), recording_calls(), no_fixes());
        check("planet 0 is a valid alert", g_log.prints == 1 && g_log.sprintfs == 1);
        check("the alert is formatted for the polled planet", g_log.sprintf_planet == 0);
        check("an alert raises the floating-message flag", w.msg_active == 1);
    }
}

void test_advance_sim_clock() {
    { // ordinary step: clock += delta, no clamp
        world w;
        w.clock = 1.0;
        w.delta = 0.5;
        w.total = 100.0;
        g_log.reset();
        mh::lockstep::detail::advance_sim_clock(w.st(), recording_calls());
        check("advance adds the delta to the clock", w.clock == 1.5);
        check("advance leaves an unclamped delta alone", w.delta == 0.5);
        check("advance steps the sim once", g_log.sim_steps == 1);
        check("advance runs the invasion due check", g_log.due_checks == 1);
    }
    { // overshoot: the clock is pinned AND the overshoot is GIVEN BACK to the delta, so the caller's
        // next step starts from a delta that already accounts for the clamp. Pinning the clock without
        // adjusting the delta is the plausible-but-wrong version.
        world w;
        w.clock = 9.0;
        w.delta = 5.0;
        w.total = 10.0;
        g_log.reset();
        mh::lockstep::detail::advance_sim_clock(w.st(), recording_calls());
        check("an overshoot pins the clock to the target", w.clock == 10.0);
        check("an overshoot is subtracted from the delta too", w.delta == 1.0);
    }
    { // Landing EXACTLY on the target. The original's test is `<`, not `<=`, and this check does NOT
        // discriminate between them -- deliberately, because nothing can: at equality the clamp body is
        // `delta -= (clock - total)` with a zero difference and `clock = total` with clock already
        // equal, i.e. an identity on both variables. The two spellings are behaviourally the same
        // function, so faithfulness here rests on reading the disassembly (`FCOMP`/`JBE` at
        // 0x0044d126), not on a test. Asserted anyway to pin the OUTCOME at the boundary.
        // (Found by mutation: swapping `<` for `<=` left the whole suite green, which is the correct
        // result for an unobservable difference and would have been a false alarm to "fix".)
        world w;
        w.clock = 9.0;
        w.delta = 1.0;
        w.total = 10.0;
        mh::lockstep::detail::advance_sim_clock(w.st(), recording_calls());
        check("landing exactly on the target leaves clock and delta at the target",
              w.clock == 10.0 && w.delta == 1.0);
    }
}

void test_pump() {
    { // schedule flushed something -> the keepalive stands down for this frame, but the RX drain
        // still runs. This is the branch an inert schedule() stub would get wrong, which is exactly
        // why the pump is not shadowable.
        world w;
        w.clock     = 10.0;
        w.horizon   = 10.0; // starved: the keepalive WOULD fire if it were allowed to
        w.step_size = 4.0;
        w.refill    = 0.5;
        g_log.reset();
        mh::lockstep::detail::pump_after_schedule(w.st(), recording_calls(), 1);
        check("a flushing frame suppresses the keepalive", g_log.sends == 0 && w.horizon == 10.0);
        check("...but still drains RX", g_log.dispatches == 1);
    }
    { // nothing flushed and the horizon is nearly used up -> extend by a WHOLE step, advertise it,
        // and re-commit the barrier
        world w;
        w.human(0);
        w.side      = 0;
        w.clock     = 10.0;
        w.horizon   = 11.0; // clock + 0.5*4 = 12 > 11 -> starved
        w.step_size = 4.0;
        w.refill    = 0.5;
        g_log.reset();
        mh::lockstep::detail::pump_after_schedule(w.st(), recording_calls(), 0);
        check("a starved horizon is extended by a whole step", w.horizon == 14.0);
        check("the extension is advertised", g_log.sends == 1 && g_log.last_sent == 14.0);
        check("the barrier is re-committed after extending", w.committed == 14.0);
        check("RX is drained after extending", g_log.dispatches == 1);
    }
    { // plenty of horizon left -> nothing but the RX drain
        world w;
        w.clock     = 10.0;
        w.horizon   = 99.0;
        w.step_size = 4.0;
        w.refill    = 0.5;
        g_log.reset();
        mh::lockstep::detail::pump_after_schedule(w.st(), recording_calls(), 0);
        check("a healthy horizon is left alone", w.horizon == 99.0 && g_log.sends == 0);
        check("RX is drained anyway", g_log.dispatches == 1);
    }
    { // the REFILL FRACTION is what decides "nearly used up". At frac 0.5 a horizon 2.5 steps out is
        // fine; at 3.0 the same state is starved. Hard-coding either would pass one of these only.
        world w;
        w.clock     = 0.0;
        w.horizon   = 10.0;
        w.step_size = 4.0;
        w.refill    = 0.5; // reach = 2.0, not past 10
        g_log.reset();
        mh::lockstep::detail::pump_after_schedule(w.st(), recording_calls(), 0);
        check("a small refill fraction leaves the horizon alone", g_log.sends == 0);

        world v;
        v.clock     = 0.0;
        v.horizon   = 10.0;
        v.step_size = 4.0;
        v.refill    = 3.0; // reach = 12.0, past 10 -> starved
        g_log.reset();
        mh::lockstep::detail::pump_after_schedule(v.st(), recording_calls(), 0);
        check("a large refill fraction starves the same horizon", g_log.sends == 1);
    }
}

void test_sim_clock_advance() {
    { // nothing owed -> zero sub-steps, and none of the resync work runs
        world w;
        w.clock    = 5.0;
        w.total    = 5.0;
        w.sub_step = 1.0;
        g_log.reset();
        check("no time owed -> 0 steps",
              mh::lockstep::detail::sim_clock_advance_after_time_tick(w.st(), recording_calls()) == 0);
        check("no time owed -> no unit/building resync", g_log.resync_units == 0);
    }
    { // a small debt: delta = owed/100, and if that is below one sub-step the answer is the WHOLE
        // NUMBER of sub-steps that fit -- and the delta is pinned to one sub-step.
        world w;
        w.clock    = 0.0;
        w.total    = 10.0; // owed = 10 -> delta = 0.1
        w.sub_step = 3.0;  // 0.1 < 3.0 -> the counted branch; 10/3 truncates to 3
        g_log.reset();
        const int32_t n = mh::lockstep::detail::sim_clock_advance_after_time_tick(w.st(), recording_calls());
        check("a small debt returns the whole sub-steps that fit", n == 3);
        check("a small debt pins the delta to one sub-step", w.delta == 3.0);
    }
    { // The sub-step count TRUNCATES; it does not round to nearest. The case above (10/3 = 3.33) does
        // NOT separate the two -- both answer 3 -- so this one is chosen with a fraction >= 0.5, which
        // is the only place the two differ. Added because the batch-B review reported the truncation as
        // a divergence on the strength of the callee's NAME (`round`), and the check that should have
        // settled it could not: it agreed with both implementations.
        world w;
        w.clock    = 0.0;
        w.total    = 3.7; // owed = 3.7, delta = 0.037 < sub_step -> the counted branch
        w.sub_step = 1.0; // 3.7 / 1.0 -> truncate 3, round-to-nearest 4
        g_log.reset();
        const int32_t n = mh::lockstep::detail::sim_clock_advance_after_time_tick(w.st(), recording_calls());
        check("the sub-step count truncates rather than rounding", n == 3);
    }
    { // a large per-step debt: delta >= sub_step -> the CAP, 100, and the computed delta is kept.
        // The debt is deliberately kept UNDER CATCHUP_LIMIT_SECS so this case isolates the cap; the
        // first draft of this test used owed=1000, which trips the 300 s skip first and therefore
        // measured the skip, not the cap. (The code was right and the fixture was wrong -- worth
        // recording, since a fixture that agrees with a wrong implementation is the real hazard.)
        world w;
        w.clock    = 0.0;
        w.total    = 100.0; // owed = 100 < 300 -> no skip; delta = 1.0
        w.sub_step = 0.5;   // 1.0 >= 0.5 -> capped
        g_log.reset();
        const int32_t n = mh::lockstep::detail::sim_clock_advance_after_time_tick(w.st(), recording_calls());
        check("a large per-step debt returns the 100 cap", n == mh::lockstep::CLOCK_ADVANCE_CAPPED);
        check("the capped branch keeps the computed delta", w.delta == 1.0);
        check("a debt under the catch-up limit is not skipped", g_log.resync_units == 0);
        check("a debt under the catch-up limit leaves the clock alone", w.clock == 0.0);
    }
    { // past CATCHUP_LIMIT_SECS the excess is SKIPPED, not simulated: the clock jumps forward by it
        // and the unit/building resync is told about the jump.
        world w;
        w.clock    = 0.0;
        w.total    = 500.0; // owed 500, limit 300 -> 200 skipped
        w.sub_step = 1.0;
        g_log.reset();
        mh::lockstep::detail::sim_clock_advance_after_time_tick(w.st(), recording_calls());
        check("time past the catch-up limit is skipped on the clock", w.clock == 200.0);
        check("the skip resyncs units and buildings at the new clock",
              g_log.resync_units == 1 && g_log.last_resync_units == 200.0);
        check("only the limit is left to simulate", w.delta == mh::lockstep::CATCHUP_LIMIT_SECS / 100.0);
    }
    { // the snapshot resync runs unconditionally, before any of the above
        world w;
        w.clock = 5.0;
        w.total = 5.0;
        g_log.reset();
        mh::lockstep::detail::sim_clock_advance_after_time_tick(w.st(), recording_calls());
        check("the snapshot clock resync always runs", g_log.resync_snapshot == 1);
    }
}


// A world set up as a live 2-human match: we are side 100 (index 0), the peer is side 101 (index 1).
world mp_world() {
    world w;
    w.human(0);
    w.human(1);
    w.side       = 0;
    w.local_side = 100;
    w.clock      = 50.0;
    w.horizon    = 60.0;
    for (int32_t i = 0; i < N; ++i) w.peer_horizon[i] = NO_HORIZON;
    g_rx.reset();
    return w;
}


namespace w3 {

constexpr double PROBE = 4242.5; // distinctive: no handler produces it, so it can only come from us

std::vector<uint8_t> g_sent; // whatever the emitter handed the transport
void                 capture_send(uint8_t *b, int32_t n) { g_sent.assign(b, b + n); }

// A standalone buffer + cursor, so an emit never touches the world under test.
struct emitter {
    uint8_t buf[0x400]{};
    int32_t cursor = 0;

    mh::net::packet_buffer pb() { return mh::net::packet_buffer{buf, &cursor}; }

    int32_t emit(const mh::net::ctrl_record &r) {
        g_sent.clear();
        auto p = pb();
        return mh::net::emit_ctrl(p, r, capture_send);
    }
};

// The payload shapes. The original copies side_id and horizon with two separate inlined memcpys
// (4 bytes then 8); handing emit_ctrl one packed 12-byte blob is byte-identical, and `packed` is
// asserted below rather than assumed.
#pragma pack(push, 1)
struct side_and_horizon {
    int32_t side_id;
    double  horizon;
};
#pragma pack(pop)
static_assert(sizeof(side_and_horizon) == 12, "the wire has no padding between side_id and horizon");

// A control record, spelled the way the table reads.
mh::net::ctrl_record ctl(uint8_t inner, const void *p, int32_t n, bool reset_first = false) {
    mh::net::ctrl_record r;
    r.outer       = mh::lockstep::MSG_CONTROL;
    r.has_inner   = true;
    r.inner       = inner;
    r.payload     = p;
    r.payload_len = n;
    r.reset_first = reset_first;
    return r;
}
mh::net::ctrl_record outer_only(uint8_t outer, const void *p, int32_t n, bool reset_first = false) {
    mh::net::ctrl_record r;
    r.outer       = outer;
    r.payload     = p;
    r.payload_len = n;
    r.reset_first = reset_first;
    return r;
}

// Every kind W1 marked LIVE, with the wire length its own bytes produce (the wire-emitter inventory).
// MSG_ORDER(1) is deliberately absent: its records are appended by the original send_order(), which
// is not one of the emit_ctrl family. The three unreachable builders -- CTL_SESSION_ENDED(2),
// CTL_SCALE_STEP(12), CTL_PEER_HORIZON(15) -- are absent for the reason W1 established, and
// CTL_NOP(3) has no builder at all in the image.
struct live_kind {
    const char *name;
    uint8_t     outer;
    bool        has_inner;
    uint8_t     inner;
    int32_t     payload_len;
    int32_t     wire; // what the table says the whole record measures
    bool        reset_first;
};

const live_kind KINDS[] = {
    {"MSG_HORIZON", mh::lockstep::MSG_HORIZON, false, 0, 8, 9, true},
    {"MSG_KEEPALIVE", mh::lockstep::MSG_KEEPALIVE, false, 0, 4, 5, false},
    {"CTL_PLAYER_LEFT", mh::lockstep::MSG_CONTROL, true, mh::lockstep::CTL_PLAYER_LEFT, 0, 2, false},
    {"CTL_SLOT_RESET", mh::lockstep::MSG_CONTROL, true, mh::lockstep::CTL_SLOT_RESET, 4, 6, false},
    {"CTL_HORIZON_CHECK", mh::lockstep::MSG_CONTROL, true, mh::lockstep::CTL_HORIZON_CHECK, 12, 14, false},
    {"CTL_LEAVE_CONSENSUS", mh::lockstep::MSG_CONTROL, true, mh::lockstep::CTL_LEAVE_CONSENSUS, 4, 6, false},
    {"CTL_STATUS_RESET_REQ", mh::lockstep::MSG_CONTROL, true, mh::lockstep::CTL_STATUS_RESET_REQ, 4, 6, false},
    {"CTL_DROP_SYNCED", mh::lockstep::MSG_CONTROL, true, mh::lockstep::CTL_DROP_SYNCED, 12, 14, false},
    {"CTL_DROP_UNSYNCED", mh::lockstep::MSG_CONTROL, true, mh::lockstep::CTL_DROP_UNSYNCED, 12, 14, false},
    {"CTL_KICK", mh::lockstep::MSG_CONTROL, true, mh::lockstep::CTL_KICK, 4, 6, false},
    {"CTL_SET_STEP", mh::lockstep::MSG_CONTROL, true, mh::lockstep::CTL_SET_STEP, 8, 10, false},
    {"CTL_RESYNC_BEGIN", mh::lockstep::MSG_CONTROL, true, mh::lockstep::CTL_RESYNC_BEGIN, 8, 10, false},
    {"CTL_RESYNC_END", mh::lockstep::MSG_CONTROL, true, mh::lockstep::CTL_RESYNC_END, 0, 2, false},
};

// A payload big enough for the widest kind, with recognisable content.
struct payload_bytes {
    uint8_t b[16]{};
    payload_bytes() {
        for (int i = 0; i < 16; ++i) b[i] = static_cast<uint8_t>(0x40 + i);
    }
};

mh::net::ctrl_record record_for(const live_kind &k, const void *payload) {
    return k.has_inner ? ctl(k.inner, k.payload_len ? payload : nullptr, k.payload_len, k.reset_first)
                       : outer_only(k.outer, k.payload_len ? payload : nullptr, k.payload_len,
                                    k.reset_first);
}

} // namespace w3

// ==================================================================================================
// RI-WIRE: tests for the five emitters in lockstep/tx_emit.{h,cpp} -- send_lockstep_extend,
// send_lockstep_keepalive, lockstep_broadcast_player_leave, player_remove, player_remove_timeout.
//
// W3's round trip above is an oracle for emit_ctrl() itself, using HAND-BUILT payloads. It does not
// prove any of these five actually call emit_ctrl with the right tag / payload / guard, and it says
// nothing about their TAILS -- the state writes and outward calls that run after the send, which is
// where four of these five do their real work (the wire-emitter inventory "Tail logic"). That is this
// block's job: drive the actual detail:: functions tx_emit.cpp defines, over a fixture of our own.
namespace tx5 {

// The transport capture, kept separate from w3::g_sent (these two test groups never interleave, but a
// shared vector would make a future reordering of the runner list silently read the wrong bytes).
std::vector<uint8_t> g_sent;
void                 capture_send(uint8_t *b, int32_t n) { g_sent.assign(b, b + n); }

// player_by_side_id, MODELLED exactly like w3/tk's side_to_index: side 100+i -> player i, anything
// else -> 0. Modelling rather than rigging means an emitter that mixed up a side_id and an
// already-resolved index reads as a wrong array slot instead of being silently indistinguishable.
int32_t side_to_index(int32_t side_id) {
    return (side_id >= 100 && side_id < 100 + N) ? side_id - 100 : 0;
}

// The seven remaining outward calls (emit_calls minus transport_send and player_by_side_id), recorded
// or rigged the same way g_rx/g_log do for their own modules.
struct call_log {
    int32_t              count_active_players_answer = 2; // rigged: >1 by default (the "carry on" side)
    std::vector<int32_t> presence_lost;                   // player index per call
    int32_t              chat_recalcs  = 0;
    int32_t              commits       = 0;
    int32_t              leader_answer = 0; // rigged is_local_leader_peer(-1)
    std::vector<int32_t> leader_asked;
    int32_t              force_resyncs = 0;
    void                 reset() { *this = call_log{}; }
};
call_log g_log;

const mh::lockstep::emit_calls &recording_calls() {
    static const mh::lockstep::emit_calls ec = {
        capture_send,
        side_to_index,
        []() -> int32_t { return g_log.count_active_players_answer; },
        [](uint32_t p, uint32_t) -> uint32_t {
            g_log.presence_lost.push_back(static_cast<int32_t>(p));
            return 0;
        },
        []() { ++g_log.chat_recalcs; },
        []() { ++g_log.commits; },
        [](int32_t excl) -> int32_t {
            g_log.leader_asked.push_back(excl);
            return g_log.leader_answer;
        },
        []() { ++g_log.force_resyncs; },
    };
    return ec;
}

// A standalone emit_state fixture on the heap. Deliberately its OWN buffers rather than `world`'s: this
// module reads/writes a narrower slice of the same global memory than the turn engine does, and a
// shared fixture would risk a bug in one test being masked by state a different module's setup happens
// to leave in a working shape.
struct fixture {
    uint8_t buf[0x400]{};
    int32_t cursor = 0;

    double  peer_horizon[N]{};
    double  peer_horizon_pending[N]{};
    uint8_t peer_state[N * N]{};
    uint8_t status_flags = 0;
    // A sentinel that is neither 0.0 nor HORIZON_NONE (-1.0), so "was this (re)armed to 0.0 by the
    // tail" stays a real question instead of one a zero-initialised field would answer by accident --
    // the same trap the batch-B review flagged for sim_tick's delta (see test_sim_tick_singleplayer).
    double               peer_timeout_elapsed = 12345.0;
    uint16_t             player_side          = 0;
    std::vector<profile> players{static_cast<size_t>(N)};
    int32_t              resync_trigger_count = 0;
    int32_t              active_player_count  = 2;
    // W5: the SENT-side gate's predicate input. Default 0x38 == SYNC_OVERLAY_AFTER, i.e. NOT yet in
    // the "sustained silence" window -- so a gate-ON run SUPPRESSES by default and a test that wants
    // the allowing branch has to say so. Defaulting the other way would make the gate look inert.
    int32_t sync_retry_countdown = 0x38;
    // D17: the local lockstep horizon the migrated resync_order_horizon clamp reads. A value
    // well AHEAD of the 2.0 the original passes, so the clamp has something to do.
    double horizon = 10.1884824473598;
    // D24: the other two clamp inputs. NON-ZERO on purpose -- with step_size 0.0 the D24 barrier
    // collapses back onto the bare horizon and every assertion below would pass against the
    // pre-D24 code, which is exactly the way a fixture default hides the change it is meant to
    // prove. game_clock sits BEHIND the horizon, as it does in a healthy match.
    double game_clock = 9.5;
    double step_size  = 0.25;

    fixture() {
        std::memset(players.data(), 0, players.size() * sizeof(profile));
        for (int32_t i = 0; i < N; ++i) {
            peer_horizon[i]         = NO_HORIZON;
            peer_horizon_pending[i] = NO_HORIZON;
            players[i].side_id      = 100 + i; // matches side_to_index above
        }
    }

    mh::lockstep::emit_state st() {
        return mh::lockstep::emit_state{
            mh::net::packet_buffer{buf, &cursor},
            peer_horizon,
            peer_horizon_pending,
            peer_state,
            &status_flags,
            &peer_timeout_elapsed,
            &player_side,
            players.data(),
            &resync_trigger_count,
            &sync_retry_countdown,
            &active_player_count,
            &horizon,
            &game_clock,
            &step_size,
        };
    }
};

} // namespace tx5

// (1) THE LAYOUT + THE GUARD, send_lockstep_extend. The wire-emitter inventory row 1: MSG_HORIZON, no
// inner tag, an 8-byte horizon payload, wire = 9, reset? = YES -- the ONLY guarded emitter of the five.
void test_tx5_extend_layout_and_guard() {
    {
        tx5::fixture f;
        tx5::g_log.reset();
        const double horizon = 123.5;
        mh::lockstep::detail::send_lockstep_extend(f.st(), tx5::recording_calls(), horizon);
        check("send_lockstep_extend emits the table wire length (9)",
              static_cast<int32_t>(tx5::g_sent.size()) == 9);
        check("send_lockstep_extend's outer tag is MSG_HORIZON (0x0049d366)",
              !tx5::g_sent.empty() && tx5::g_sent[0] == mh::lockstep::MSG_HORIZON);
        double got = 0.0;
        if (tx5::g_sent.size() == 9) std::memcpy(&got, tx5::g_sent.data() + 1, sizeof(double));
        check("send_lockstep_extend's payload is the horizon argument verbatim", got == horizon);
        check("send_lockstep_extend leaves the cursor at 0 after sending", f.cursor == 0);
    }
    { // THE GUARD, made observable: dirty the cursor first, as a mid-batch order-schedule call would.
        tx5::fixture f;
        f.cursor = 6;
        std::memset(f.buf, 0xAB, 6); // stand-in for 6 bytes of a pending order batch
        tx5::g_log.reset();
        mh::lockstep::detail::send_lockstep_extend(f.st(), tx5::recording_calls(), 9.5);
        check("send_lockstep_extend is GUARDED (0x0049d353..0x0049d35c): a pending batch is DISCARDED, "
              "and only its own 9-byte record is sent, from offset 0",
              static_cast<int32_t>(tx5::g_sent.size()) == 9 && tx5::g_sent[0] == mh::lockstep::MSG_HORIZON);
    }
}

// (2) THE LAYOUT + THE GUARD, send_lockstep_keepalive. Row 2: MSG_KEEPALIVE, no inner tag, a 4-byte
// side_id payload, wire = 5, reset? = no -- UNGUARDED, unlike extend, and that asymmetry is the point.
void test_tx5_keepalive_layout_and_guard() {
    {
        tx5::fixture f;
        tx5::g_log.reset();
        mh::lockstep::detail::send_lockstep_keepalive(f.st(), tx5::recording_calls(), 777, mh::lockstep::reimpl_fixes{}, nullptr);
        check("send_lockstep_keepalive emits the table wire length (5)",
              static_cast<int32_t>(tx5::g_sent.size()) == 5);
        check("send_lockstep_keepalive's outer tag is MSG_KEEPALIVE",
              !tx5::g_sent.empty() && tx5::g_sent[0] == mh::lockstep::MSG_KEEPALIVE);
        int32_t got = 0;
        if (tx5::g_sent.size() == 5) std::memcpy(&got, tx5::g_sent.data() + 1, sizeof(int32_t));
        check("send_lockstep_keepalive's payload is side_id verbatim", got == 777);
    }
    { // UNGUARDED: the pending batch survives, and the record is appended after it.
        tx5::fixture f;
        f.cursor = 6;
        std::memset(f.buf, 0xAB, 6);
        tx5::g_log.reset();
        mh::lockstep::detail::send_lockstep_keepalive(f.st(), tx5::recording_calls(), 5, mh::lockstep::reimpl_fixes{}, nullptr);
        check("send_lockstep_keepalive is UNGUARDED: it APPENDS after a pending batch rather than "
              "discarding it, unlike send_lockstep_extend",
              static_cast<int32_t>(tx5::g_sent.size()) == 6 + 5 &&
                  tx5::g_sent[6] == mh::lockstep::MSG_KEEPALIVE);
    }
}

// (3) THE SPLIT-OUT TAIL: resync_trigger_tick on its own, both leader states and both sides of the
// threshold (0x0049d8bd..0x0049d8e0). EAX=-1 to is_local_leader_peer means "exclude nobody"; the
// threshold compare is UNSIGNED in the original, reproduced with the same casts here.
// (W5) THE MIGRATED SENT-SIDE resync_trigger_gate. The mirror of C3's RECV-side migration, and the
// place the real evidence for it has to live: the rig CANNOT supply this. An idle LAN fires ~zero
// resyncs, and the gate only evaluates when a keepalive is actually emitted -- which in the W4.6
// 3000-step run happened between 1 and 99 times. So both flag states are proved here, by name.
void test_w5_sent_gate_both_states() {
    using mh::lockstep::reimpl_fixes;
    const int32_t BELOW = mh::lockstep::SYNC_OVERLAY_AFTER - 1; // genuine sustained silence
    const int32_t AT    = mh::lockstep::SYNC_OVERLAY_AFTER;     // the routine at-horizon nag

    { // OFF is the STOCK behaviour: increment unconditionally, whatever the countdown says.
        tx5::fixture f;
        f.sync_retry_countdown = AT; // the value the gate would SUPPRESS on
        tx5::g_log.reset();
        tx5::g_log.leader_answer = 1;
        reimpl_fixes fx; // resync_trigger_gate defaults false
        mh::lockstep::detail::resync_trigger_tick(f.st(), tx5::recording_calls(), fx, nullptr);
        check("W5 gate OFF increments even at the routine nag (stock, 0x0049d8cb unconditional)",
              f.resync_trigger_count == 1);
    }
    { // ON + routine nag -> SUPPRESSED. This is the whole point of the fix.
        tx5::fixture f;
        f.sync_retry_countdown = AT;
        tx5::g_log.reset();
        tx5::g_log.leader_answer = 1;
        reimpl_fixes fx;
        fx.resync_trigger_gate = true;
        mh::lockstep::detail::resync_trigger_tick(f.st(), tx5::recording_calls(), fx, nullptr);
        check("W5 gate ON suppresses the routine at-horizon nag", f.resync_trigger_count == 0);
    }
    { // ON + genuine sustained silence -> still counts. A gate that suppressed EVERYTHING would pass
        // the previous check and break the feature; this is the check that stops that.
        tx5::fixture f;
        f.sync_retry_countdown = BELOW;
        tx5::g_log.reset();
        tx5::g_log.leader_answer = 1;
        reimpl_fixes fx;
        fx.resync_trigger_gate = true;
        mh::lockstep::detail::resync_trigger_tick(f.st(), tx5::recording_calls(), fx, nullptr);
        check("W5 gate ON still counts genuine sustained silence", f.resync_trigger_count == 1);
    }
    { // Not the leader -> nothing happens in EITHER state, and the audit must not tick either: the
        // leader test precedes the gate in the original (0x0049d8c2/0x0049d8c9).
        tx5::fixture f;
        f.sync_retry_countdown = BELOW;
        reimpl_fixes fx;
        fx.resync_trigger_gate = true;
        mh::lockstep::gate_audit a;
        tx5::g_log.reset();
        tx5::g_log.leader_answer = 0; // NOT the leader
        mh::lockstep::detail::resync_trigger_tick(f.st(), tx5::recording_calls(), fx, &a);
        check("W5 a non-leader neither counts nor evaluates the gate",
              f.resync_trigger_count == 0 && a.evals == 0);
    }
}

// The FIX AUDIT is not decoration -- it is the only thing that separates "the predicate ran and said
// no" from "the predicate never ran". On a healthy LAN RESYNC_TRIGGER_COUNT stays 0 in both cases,
// which is exactly how a migrated fix gets certified without ever having executed.
void test_w5_sent_gate_audit_tally() {
    using mh::lockstep::reimpl_fixes;
    tx5::fixture f;
    tx5::g_log.reset();
    tx5::g_log.leader_answer = 1;
    reimpl_fixes fx;
    fx.resync_trigger_gate = true;
    mh::lockstep::gate_audit a;

    f.sync_retry_countdown = mh::lockstep::SYNC_OVERLAY_AFTER; // suppress
    mh::lockstep::detail::resync_trigger_tick(f.st(), tx5::recording_calls(), fx, &a);
    check("W5 audit counts a SUPPRESSED evaluation", a.evals == 1 && a.allowed == 0);
    check("W5 a suppressed evaluation leaves the counter alone", f.resync_trigger_count == 0);

    f.sync_retry_countdown = mh::lockstep::SYNC_OVERLAY_AFTER - 1; // allow
    mh::lockstep::detail::resync_trigger_tick(f.st(), tx5::recording_calls(), fx, &a);
    check("W5 audit counts an ALLOWED evaluation", a.evals == 2 && a.allowed == 1);
    check("W5 an allowed evaluation does increment", f.resync_trigger_count == 1);
}

void test_tx5_resync_trigger_tick() {
    { // not the leader: the counter is untouched and nothing escalates
        tx5::fixture f;
        tx5::g_log.reset();
        tx5::g_log.leader_answer = 0;
        f.resync_trigger_count   = 5;
        mh::lockstep::detail::resync_trigger_tick(f.st(), tx5::recording_calls(),
                                                  mh::lockstep::reimpl_fixes{}, nullptr);
        check("resync_trigger_tick asks is_local_leader_peer(-1) -- exclude nobody",
              tx5::g_log.leader_asked.size() == 1 && tx5::g_log.leader_asked[0] == -1);
        check("a non-leader's counter is untouched", f.resync_trigger_count == 5);
        check("a non-leader never escalates", tx5::g_log.force_resyncs == 0);
    }
    { // leader, well below the threshold: counts, does not escalate
        tx5::fixture f;
        tx5::g_log.reset();
        tx5::g_log.leader_answer = 1;
        f.active_player_count    = 1; // threshold = active*100 = 100
        f.resync_trigger_count   = 5;
        mh::lockstep::detail::resync_trigger_tick(f.st(), tx5::recording_calls(),
                                                  mh::lockstep::reimpl_fixes{}, nullptr);
        check("the leader's counter increments", f.resync_trigger_count == 6);
        check("well below the threshold: no escalation", tx5::g_log.force_resyncs == 0);
    }
    { // the boundary, low side: counter lands EXACTLY on the threshold -- `<` means this does NOT fire
        tx5::fixture f;
        tx5::g_log.reset();
        tx5::g_log.leader_answer = 1;
        f.active_player_count    = 1;  // threshold 100
        f.resync_trigger_count   = 99; // becomes 100 after the increment
        mh::lockstep::detail::resync_trigger_tick(f.st(), tx5::recording_calls(),
                                                  mh::lockstep::reimpl_fixes{}, nullptr);
        check("counter == threshold does not yet escalate (strict <)", tx5::g_log.force_resyncs == 0);
        check("...but it did increment to exactly the threshold", f.resync_trigger_count == 100);
    }
    { // the boundary, high side: one past the threshold fires
        tx5::fixture f;
        tx5::g_log.reset();
        tx5::g_log.leader_answer = 1;
        f.active_player_count    = 1;
        f.resync_trigger_count   = 100; // becomes 101
        mh::lockstep::detail::resync_trigger_tick(f.st(), tx5::recording_calls(),
                                                  mh::lockstep::reimpl_fixes{}, nullptr);
        check("one past the threshold escalates", tx5::g_log.force_resyncs == 1);
        check("counter still incremented normally", f.resync_trigger_count == 101);
    }
}

// (4) THE SAME BOUNDARY, reached THROUGH send_lockstep_keepalive rather than the split-out function
// directly -- proving the wrapper actually calls the tail. A translation that dropped the trailing
// `resync_trigger_tick(es, calls);` statement from send_lockstep_keepalive would pass every check above
// and only fail here.
void test_tx5_keepalive_calls_resync_tail() {
    {
        tx5::fixture f;
        tx5::g_log.reset();
        tx5::g_log.leader_answer = 1;
        f.active_player_count    = 1;
        f.resync_trigger_count   = 99;
        mh::lockstep::detail::send_lockstep_keepalive(f.st(), tx5::recording_calls(), 100, mh::lockstep::reimpl_fixes{}, nullptr);
        check("send_lockstep_keepalive still emits its own 5-byte record while the tail runs",
              static_cast<int32_t>(tx5::g_sent.size()) == 5);
        check("...and the tail's boundary holds through the wrapper: at the threshold, no escalation",
              f.resync_trigger_count == 100 && tx5::g_log.force_resyncs == 0);
    }
    {
        tx5::fixture f;
        tx5::g_log.reset();
        tx5::g_log.leader_answer = 1;
        f.active_player_count    = 1;
        f.resync_trigger_count   = 100;
        mh::lockstep::detail::send_lockstep_keepalive(f.st(), tx5::recording_calls(), 100, mh::lockstep::reimpl_fixes{}, nullptr);
        check("...one call later, the wrapper's tail escalates too", tx5::g_log.force_resyncs == 1);
    }
}

// (5) THE LAYOUT + THE GUARD, lockstep_broadcast_player_leave. Row 8: MSG_CONTROL / CTL_HORIZON_CHECK,
// payload = side_id (4B) + the DEPARTING peer's own advertised horizon (8B), wire = 14, reset? = no.
void test_tx5_broadcast_player_leave_layout_and_guard() {
    {
        tx5::fixture f;
        f.peer_horizon[2] = 77.5; // side 102 -> index 2 (side_to_index)
        tx5::g_log.reset();
        mh::lockstep::detail::lockstep_broadcast_player_leave(f.st(), tx5::recording_calls(), 102);
        check("broadcast_player_leave emits the table wire length (14)",
              static_cast<int32_t>(tx5::g_sent.size()) == 14);
        check("its outer tag is MSG_CONTROL", tx5::g_sent[0] == mh::lockstep::MSG_CONTROL);
        check("its inner tag is CTL_HORIZON_CHECK (tag 5)",
              tx5::g_sent[1] == mh::lockstep::CTL_HORIZON_CHECK);
        int32_t got_side = 0;
        double  got_h    = 0.0;
        std::memcpy(&got_side, tx5::g_sent.data() + 2, sizeof(int32_t));
        std::memcpy(&got_h, tx5::g_sent.data() + 6, sizeof(double));
        check("the payload's side_id is the argument verbatim", got_side == 102);
        check("the payload's horizon is the departing peer's own peer_horizon[idx], with NO padding "
              "between the two fields (a naive {int32_t;double;} struct would insert 4 bytes here)",
              got_h == 77.5);
    }
    { // UNGUARDED
        tx5::fixture f;
        f.cursor = 6;
        std::memset(f.buf, 0xAB, 6);
        tx5::g_log.reset();
        mh::lockstep::detail::lockstep_broadcast_player_leave(f.st(), tx5::recording_calls(), 100);
        check("broadcast_player_leave is UNGUARDED: it appends after any pending batch",
              static_cast<int32_t>(tx5::g_sent.size()) == 6 + 14 &&
                  tx5::g_sent[6] == mh::lockstep::MSG_CONTROL);
    }
}

// (6) THE TAIL: the 8-slot local sweep that runs AFTER the send (0x0049dfa1..0x0049e00b). Asserted as
// whole-array state, not one element -- the row-vs-column-vs-other-rows distinction is exactly the
// mistake a single-slot check would miss (mirroring test_reset_player_horizon's own reasoning above).
void test_tx5_broadcast_player_leave_tail() {
    tx5::fixture f;
    for (int32_t i = 0; i < N * N; ++i) f.peer_state[i] = 0xAB;                             // poison every row
    for (int32_t i = 0; i < N; ++i) f.peer_horizon_pending[i] = static_cast<double>(i + 1); // != NO_HORIZON
    f.status_flags         = 0;
    f.peer_timeout_elapsed = 999.0;
    f.player_side          = 5; // OUR OWN column within the departing player's row
    tx5::g_log.reset();

    mh::lockstep::detail::lockstep_broadcast_player_leave(f.st(), tx5::recording_calls(), 103); // idx 3

    bool row_present = true;
    for (int32_t i = 0; i < N; ++i)
        if (i != 5)
            row_present = row_present && f.peer_state[3 * N + i] == mh::lockstep::PEER_STATE_PRESENT;
    check("the departing player's whole peer-state row is marked PRESENT (6)", row_present);
    check("...EXCEPT our own column in that row, cleared back to NONE (0x0049dfea..dff3)",
          f.peer_state[3 * N + 5] == mh::lockstep::PEER_STATE_NONE);

    bool other_rows_intact = true;
    for (int32_t i = 0; i < N * N; ++i)
        if (i < 3 * N || i >= 4 * N) other_rows_intact = other_rows_intact && f.peer_state[i] == 0xAB;
    check("every OTHER row is untouched", other_rows_intact);

    bool pending_cleared = true;
    for (int32_t i = 0; i < N; ++i)
        pending_cleared = pending_cleared && f.peer_horizon_pending[i] == NO_HORIZON;
    check("every peer's pending horizon is reset to the sentinel", pending_cleared);

    check("LS_HORIZON_PENDING is raised (0x0049dffa)",
          (f.status_flags & mh::lockstep::LS_HORIZON_PENDING) != 0);
    check("the peer-timeout grace clock is (re)armed to 0.0 (0x0049e001/0x0049e00b)",
          f.peer_timeout_elapsed == 0.0);
}

// (7) THE LAYOUT + THE GUARD, both removal emitters. Rows 11/12: MSG_CONTROL / CTL_DROP_SYNCED (8)
// resp. CTL_DROP_UNSYNCED (9), payload = side_id (4B) + the target's peer_horizon (8B), wire = 14,
// reset? = no for BOTH. send_removal_record is shared between them, so this is also the test that the
// inner tag really is the ONE byte that differs between the two callers.
void test_tx5_player_remove_layout_and_guard() {
    {
        tx5::fixture f;
        f.peer_horizon[1] = 42.25;
        tx5::g_log.reset();
        mh::lockstep::detail::player_remove(f.st(), tx5::recording_calls(), 101);
        check("player_remove emits the table wire length (14)", static_cast<int32_t>(tx5::g_sent.size()) == 14);
        check("player_remove's outer tag is MSG_CONTROL", tx5::g_sent[0] == mh::lockstep::MSG_CONTROL);
        check("player_remove's inner tag is CTL_DROP_SYNCED (8)",
              tx5::g_sent[1] == mh::lockstep::CTL_DROP_SYNCED);
        int32_t got_side = 0;
        double  got_h    = 0.0;
        std::memcpy(&got_side, tx5::g_sent.data() + 2, sizeof(int32_t));
        std::memcpy(&got_h, tx5::g_sent.data() + 6, sizeof(double));
        check("player_remove's payload side_id is the argument verbatim", got_side == 101);
        check("player_remove's payload horizon is the target's advertised horizon, no padding",
              got_h == 42.25);
    }
    {
        tx5::fixture f;
        f.peer_horizon[1] = 11.5;
        tx5::g_log.reset();
        mh::lockstep::detail::player_remove_timeout(f.st(), tx5::recording_calls(), 101);
        check("player_remove_timeout emits the SAME table wire length (14)",
              static_cast<int32_t>(tx5::g_sent.size()) == 14);
        check("player_remove_timeout's inner tag is CTL_DROP_UNSYNCED (9) -- the ONE byte that differs "
              "from player_remove",
              tx5::g_sent[1] == mh::lockstep::CTL_DROP_UNSYNCED);
        double got_h = 0.0;
        std::memcpy(&got_h, tx5::g_sent.data() + 6, sizeof(double));
        check("...and the payload shape is otherwise identical", got_h == 11.5);
    }
    { // UNGUARDED, both
        tx5::fixture f;
        f.cursor = 6;
        std::memset(f.buf, 0xAB, 6);
        tx5::g_log.reset();
        mh::lockstep::detail::player_remove(f.st(), tx5::recording_calls(), 100);
        check("player_remove is UNGUARDED: it appends after any pending batch",
              static_cast<int32_t>(tx5::g_sent.size()) == 6 + 14 &&
                  tx5::g_sent[6] == mh::lockstep::MSG_CONTROL);
    }
    {
        tx5::fixture f;
        f.cursor = 6;
        std::memset(f.buf, 0xAB, 6);
        tx5::g_log.reset();
        mh::lockstep::detail::player_remove_timeout(f.st(), tx5::recording_calls(), 100);
        check("player_remove_timeout is UNGUARDED too",
              static_cast<int32_t>(tx5::g_sent.size()) == 6 + 14 &&
                  tx5::g_sent[6] == mh::lockstep::MSG_CONTROL);
    }
}

// (8) THE TAIL: the status-flags triple, then the branch on count_active_players(). Both branches, on
// BOTH callers, because apply_removal_and_branch is shared and a factoring mistake there would hit both
// at once. PLAYER_ALIVE is deliberately asserted UNTOUCHED on the drop-only path -- it is a DIFFERENT
// trigger (presence_lost, on the last-peer branch only) that clears it; see mh_structs.gen.h's
// status_flags field comment on the b1/b2-b3-b4 ownership split, and the ENERGY-vs-POWER-style
// warning against conflating two named concepts that happen to share a struct.
void test_tx5_player_remove_tail_status_flags_and_branch() {
    { // more than one survivor -> commit_horizon(); no presence-lost/chat pair
        tx5::fixture f;
        f.players[1].status_flags = mh::lockstep::PLAYER_ALIVE | mh::lockstep::PLAYER_HUMAN;
        tx5::g_log.reset();
        tx5::g_log.count_active_players_answer = 2;
        mh::lockstep::detail::player_remove(f.st(), tx5::recording_calls(), 101);
        check("player_remove clears PLAYER_HUMAN (AND 0xfb)",
              (f.players[1].status_flags & mh::lockstep::PLAYER_HUMAN) == 0);
        check("player_remove sets PLAYER_DEFEATED (OR 0x10)",
              (f.players[1].status_flags & mh::lockstep::PLAYER_DEFEATED) != 0);
        check("player_remove sets PLAYER_GONE (OR 0x08)",
              (f.players[1].status_flags & mh::lockstep::PLAYER_GONE) != 0);
        check("the drop-only path leaves PLAYER_ALIVE untouched",
              (f.players[1].status_flags & mh::lockstep::PLAYER_ALIVE) != 0);
        check(">1 survivor: commit_horizon runs", tx5::g_log.commits == 1);
        check(">1 survivor: presence_lost does NOT run", tx5::g_log.presence_lost.empty());
        check(">1 survivor: chat_recalc_target_mode does NOT run", tx5::g_log.chat_recalcs == 0);
    }
    // One or fewer survivors -> presence_lost(pidx,1), chat_recalc_target_mode(). The original's
    // empty teardown_hook_stub beside them is no longer called (SIMABI-HOOKS).
    {
        tx5::fixture f;
        f.players[1].status_flags = mh::lockstep::PLAYER_ALIVE | mh::lockstep::PLAYER_HUMAN;
        tx5::g_log.reset();
        tx5::g_log.count_active_players_answer = 1;
        mh::lockstep::detail::player_remove(f.st(), tx5::recording_calls(), 101);
        check("<=1 survivor: commit_horizon does NOT run", tx5::g_log.commits == 0);
        check("<=1 survivor: presence_lost runs with the target's pidx",
              tx5::g_log.presence_lost.size() == 1 && tx5::g_log.presence_lost[0] == 1);
        check("<=1 survivor: chat_recalc_target_mode runs", tx5::g_log.chat_recalcs == 1);
    }
    { // the SAME branch, reached through player_remove_timeout: exactly 1 survivor
        tx5::fixture f;
        tx5::g_log.reset();
        tx5::g_log.count_active_players_answer = 1;
        mh::lockstep::detail::player_remove_timeout(f.st(), tx5::recording_calls(), 100);
        check("player_remove_timeout shares the SAME branch: 1 survivor takes the presence-lost side",
              tx5::g_log.commits == 0 && tx5::g_log.presence_lost.size() == 1);
    }
    { // the boundary on the other side, through player_remove_timeout: exactly 2 survivors
        tx5::fixture f;
        tx5::g_log.reset();
        tx5::g_log.count_active_players_answer = 2;
        mh::lockstep::detail::player_remove_timeout(f.st(), tx5::recording_calls(), 100);
        check("player_remove_timeout: 2 survivors (the SIGNED > 1 boundary) takes commit_horizon instead",
              tx5::g_log.commits == 1 && tx5::g_log.presence_lost.empty());
    }
}

// (9) THE ROUND TRIP, applied to the real functions rather than to raw emit_ctrl calls: feed each of
// the five emitters' OWN output through the real dispatcher with a trailing probe. test_w3_emit_round_trip
// proves emit_ctrl's byte layout agrees with the parser using HAND-BUILT payloads; a bug entirely inside
// tx_emit.cpp's own field order, byte count, or argument wiring (not emit_ctrl's) would not show up
// there. Here it would.
void test_tx5_round_trip() {
    { // 1. send_lockstep_extend -- MSG_HORIZON, no inner tag, no branch to steer around
        tx5::fixture f;
        tx5::g_log.reset();
        mh::lockstep::detail::send_lockstep_extend(f.st(), tx5::recording_calls(), 91.25);
        world  wo = mp_world();
        packet p;
        p.blob(tx5::g_sent.data(), tx5::g_sent.size());
        p.u8(mh::lockstep::MSG_HORIZON).f64(w3::PROBE);
        feed(wo, p, 101);
        check("the parser consumes send_lockstep_extend's own record at exactly its emitted length",
              wo.peer_horizon[1] == w3::PROBE);
    }
    { // 2. send_lockstep_keepalive -- MSG_KEEPALIVE; any side_id keeps the dispatcher draining
        tx5::fixture f;
        tx5::g_log.reset();
        mh::lockstep::detail::send_lockstep_keepalive(f.st(), tx5::recording_calls(), 999, mh::lockstep::reimpl_fixes{}, nullptr);
        world  wo = mp_world();
        packet p;
        p.blob(tx5::g_sent.data(), tx5::g_sent.size());
        p.u8(mh::lockstep::MSG_HORIZON).f64(w3::PROBE);
        feed(wo, p, 101);
        check("the parser consumes send_lockstep_keepalive's own record at exactly its emitted length",
              wo.peer_horizon[1] == w3::PROBE);
    }
    { // 3. lockstep_broadcast_player_leave -- MSG_CONTROL/CTL_HORIZON_CHECK. That handler never
        // returns `stop` (turn_engine.h's own tally of the eight stop sites does not include tag 5), so
        // unlike the drop pair below no horizon needs to be pre-armed to keep the drain going.
        tx5::fixture f;
        f.peer_horizon[2] = 55.5;
        tx5::g_log.reset();
        mh::lockstep::detail::lockstep_broadcast_player_leave(f.st(), tx5::recording_calls(), 102);
        world  wo = mp_world();
        packet p;
        p.blob(tx5::g_sent.data(), tx5::g_sent.size());
        p.u8(mh::lockstep::MSG_HORIZON).f64(w3::PROBE);
        feed(wo, p, 101);
        check("the parser consumes broadcast_player_leave's own record at exactly its emitted length",
              wo.peer_horizon[1] == w3::PROBE);
    }
    { // 4. player_remove -- MSG_CONTROL/CTL_DROP_SYNCED, through handle_peer_drop. THE TRAP (already
        // noted on test_w3_emit_round_trip): target side 102, never the sender (101) or the local side
        // (100), and its horizon must AGREE with what handle_peer_drop compares against
        // (st.peer_horizon[target]) or the handler tears the session down and never reaches the probe.
        tx5::fixture f;
        f.peer_horizon[2] = 63.0;
        tx5::g_log.reset();
        tx5::g_log.count_active_players_answer = 2;
        mh::lockstep::detail::player_remove(f.st(), tx5::recording_calls(), 102);
        world wo           = mp_world();
        wo.peer_horizon[2] = 63.0; // matches what was just emitted
        g_rx.active_answer = 2;    // >1 survivor -> the "carry on" side of handle_peer_drop too
        packet p;
        p.blob(tx5::g_sent.data(), tx5::g_sent.size());
        p.u8(mh::lockstep::MSG_HORIZON).f64(w3::PROBE);
        feed(wo, p, 101);
        check("the parser consumes player_remove's own record at exactly its emitted length",
              wo.peer_horizon[1] == w3::PROBE);
    }
    { // 5. player_remove_timeout -- identical shape, CTL_DROP_UNSYNCED
        tx5::fixture f;
        f.peer_horizon[2] = 12.0;
        tx5::g_log.reset();
        tx5::g_log.count_active_players_answer = 2;
        mh::lockstep::detail::player_remove_timeout(f.st(), tx5::recording_calls(), 102);
        world wo           = mp_world();
        wo.peer_horizon[2] = 12.0;
        g_rx.active_answer = 2;
        packet p;
        p.blob(tx5::g_sent.data(), tx5::g_sent.size());
        p.u8(mh::lockstep::MSG_HORIZON).f64(w3::PROBE);
        feed(wo, p, 101);
        check("the parser consumes player_remove_timeout's own record at exactly its emitted length",
              wo.peer_horizon[1] == w3::PROBE);
    }
}

// ==================================================================================================
// RI-WIRE W6-A -- tests for the three functions in lockstep/tx_emit_chat.{h,cpp}: chat_send_team,
// chat_send_all, send_buf_flush. The wire-emitter inventory rows 3, 4, 19; tx_emit_chat.h's own header
// comment carries the length-byte double-duty derivation and the one-instruction diff between the two
// chat senders -- these tests are the assertions for both.
// ==================================================================================================
namespace tx_chat {

// Kept separate from w3::g_sent / tx5::g_sent / tx_ctrl::g_sent (below): four capture vectors already
// coexist in this file's different test groups, and a shared one would make a reordered runner list
// silently read the wrong bytes.
std::vector<uint8_t> g_sent;
int32_t              g_sent_calls = 0; // send_buf_flush's "sends even at cursor 0" claim needs a CALL
                                       // COUNT -- an empty g_sent looks identical to "never called".
void capture_send(uint8_t *b, int32_t n) {
    ++g_sent_calls;
    g_sent.assign(b, b + n);
}

// A standalone fixture: its own buffer/cursor, plus the one extra field emit_chat_state carries beyond
// tx_emit.h's emit_state -- the chat-target-mask byte chat_send_team reads and chat_send_all never does.
struct fixture {
    uint8_t buf[0x400]{};
    int32_t cursor           = 0;
    uint8_t chat_target_mask = 0;

    mh::lockstep::emit_chat_state st() {
        return mh::lockstep::emit_chat_state{mh::net::packet_buffer{buf, &cursor}, &chat_target_mask};
    }
};

const mh::lockstep::emit_chat_calls &recording_calls() {
    static const mh::lockstep::emit_chat_calls cc = {capture_send};
    return cc;
}

} // namespace tx_chat

// (1) THE LAYOUT + THE GUARD + THE RECIPIENT SOURCE, chat_send_team @0x0049d450 (row 3). MSG_CHAT (5),
// no inner tag, payload = len byte + recipient byte (*chat_target_mask) + text, wire = 3+n. GUARDED --
// one of only three guarded emitters in the whole 19-emitter inventory.
void test_tx_chat_team_layout_and_guard() {
    {
        tx_chat::fixture f;
        f.chat_target_mask = 0x07;
        const char text[]  = "abc";
        mh::lockstep::detail::chat_send_team(f.st(), tx_chat::recording_calls(), text, 3);
        check("chat_send_team emits the table wire length (3+n, n=3 -> 6)",
              static_cast<int32_t>(tx_chat::g_sent.size()) == 6);
        check("chat_send_team's outer tag is MSG_CHAT (0x0049d485)",
              tx_chat::g_sent[0] == mh::lockstep::MSG_CHAT);
        check("chat_send_team's length-prefix byte is the argument (3, no truncation needed here)",
              tx_chat::g_sent[1] == 3);
        check("chat_send_team's recipient byte is *chat_target_mask (0x0049d4ac), NOT a hardcoded value",
              tx_chat::g_sent[2] == 0x07);
        check("chat_send_team's text follows the two header bytes verbatim",
              std::memcmp(tx_chat::g_sent.data() + 3, text, 3) == 0);
        check("chat_send_team leaves the cursor at 0 after sending", f.cursor == 0);
    }
    { // THE GUARD (0x0049d46d..0x0049d480).
        tx_chat::fixture f;
        f.cursor = 6;
        std::memset(f.buf, 0xAB, 6); // a pending order batch
        const char text[] = "x";
        mh::lockstep::detail::chat_send_team(f.st(), tx_chat::recording_calls(), text, 1);
        check("chat_send_team is GUARDED: a pending batch is DISCARDED and only its own 4-byte record "
              "is sent, from offset 0",
              static_cast<int32_t>(tx_chat::g_sent.size()) == 4 &&
                  tx_chat::g_sent[0] == mh::lockstep::MSG_CHAT);
    }
}

// (2) THE SAME SHAPE, chat_send_all @0x0049d50b (row 4) -- the ONE byte that differs is the recipient:
// a hardcoded 0xff sentinel (0x0049d567), not a global read. The mask is deliberately set to something
// else here, so a translation that accidentally shared the global read would be caught.
void test_tx_chat_all_layout_and_guard() {
    {
        tx_chat::fixture f;
        f.chat_target_mask = 0x33; // NOT 0xff -- proves _all ignores this global entirely
        const char text[]  = "abc";
        mh::lockstep::detail::chat_send_all(f.st(), tx_chat::recording_calls(), text, 3);
        check("chat_send_all emits the table wire length (6)",
              static_cast<int32_t>(tx_chat::g_sent.size()) == 6);
        check("chat_send_all's outer tag is MSG_CHAT", tx_chat::g_sent[0] == mh::lockstep::MSG_CHAT);
        check("chat_send_all's recipient byte is the hardcoded 0xff sentinel, NOT the target-mask global",
              tx_chat::g_sent[2] == mh::lockstep::CHAT_RECIPIENT_ALL);
        check("chat_send_all's text follows verbatim", std::memcmp(tx_chat::g_sent.data() + 3, text, 3) == 0);
    }
    { // GUARDED, same as _team
        tx_chat::fixture f;
        f.cursor = 6;
        std::memset(f.buf, 0xAB, 6);
        const char text[] = "x";
        mh::lockstep::detail::chat_send_all(f.st(), tx_chat::recording_calls(), text, 1);
        check("chat_send_all is GUARDED too: the pending batch is discarded",
              static_cast<int32_t>(tx_chat::g_sent.size()) == 4 &&
                  tx_chat::g_sent[0] == mh::lockstep::MSG_CHAT);
    }
}

// (3) THE LENGTH BYTE'S DOUBLE DUTY (tx_emit_chat.h's long comment). For len > 255 the wire's own
// length-prefix byte UNDERSTATES the text that follows, while the copy count AND the cursor advance
// both use the FULL len. Both roles are asserted: the truncated prefix byte, and the untruncated copy.
void test_tx_chat_length_double_duty() {
    tx_chat::fixture     f;
    constexpr uint32_t   len = 257; // 0x101 -- low byte truncates to 1
    std::vector<uint8_t> text(len);
    for (uint32_t i = 0; i < len; ++i)
        text[i] = static_cast<uint8_t>(0xAA + (i % 7)); // recognisable, never a valid MSG_* tag byte
    mh::lockstep::detail::chat_send_team(f.st(), tx_chat::recording_calls(), text.data(), len);

    check("len=257's wire size is 3+257 (the FULL len is copied, not the truncated byte)",
          static_cast<int32_t>(tx_chat::g_sent.size()) == 3 + static_cast<int32_t>(len));
    check("len=257's length-prefix byte is len & 0xff == 1, understating the real text length",
          tx_chat::g_sent[1] == static_cast<uint8_t>(len));
    check("...but the copy count used the FULL 257 bytes, not the truncated 1",
          tx_chat::g_sent.size() == 3 + len && std::memcmp(tx_chat::g_sent.data() + 3, text.data(), len) == 0);
}

// (4) THE ROUND TRIP. handle_chat (rx_dispatch.cpp) ALWAYS returns drain_again regardless of whether
// the message is addressed to us, so -- unlike the trap test_w3_emit_round_trip's own comment warns
// about for the drop pair -- there is no "keep draining" precondition to arrange for the ordinary case.
void test_tx_chat_round_trip() {
    { // ordinary length: the probe lands exactly where a correct length prefix says it should
        tx_chat::fixture f;
        const char       text[] = "hello";
        mh::lockstep::detail::chat_send_team(f.st(), tx_chat::recording_calls(), text, 5);

        world  wo = mp_world();
        packet p;
        p.blob(tx_chat::g_sent.data(), tx_chat::g_sent.size());
        p.u8(mh::lockstep::MSG_HORIZON).f64(w3::PROBE);
        feed(wo, p, 101);
        check("the parser consumes chat_send_team's own record at exactly its emitted length",
              wo.peer_horizon[1] == w3::PROBE);
    }
    { // THE REPRODUCED HAZARD: len > 255 makes the wire's length-prefix byte LIE about how much text
        // follows. handle_chat trusts that single byte, so it consumes far FEWER bytes than were
        // actually sent -- the probe therefore does NOT land, and the next byte the parser reads is
        // mid-text garbage (which routes into the garbled-stream path, consuming the rest of the
        // buffer). This is not a defect in tx_emit_chat.cpp: it is the ORIGINAL's own quirk
        // (the wire-emitter inventory / tx_emit_chat.h "THE LENGTH BYTE'S DOUBLE DUTY"), reproduced
        // faithfully rather than "fixed" -- this test is the proof that the reproduction is
        // byte-for-byte, not just prose.
        tx_chat::fixture     f;
        constexpr uint32_t   len = 257;
        std::vector<uint8_t> text(len, 0xEE); // 0xEE - 1 = 0xED > 4 as a "sel" byte -> never misread as
                                              // a valid outer tag, so the desync is unambiguous
        mh::lockstep::detail::chat_send_team(f.st(), tx_chat::recording_calls(), text.data(), len);

        world  wo = mp_world();
        packet p;
        p.blob(tx_chat::g_sent.data(), tx_chat::g_sent.size());
        p.u8(mh::lockstep::MSG_HORIZON).f64(w3::PROBE);
        feed(wo, p, 101);
        check("a len>255 chat record desyncs the real parser: the probe does NOT land where a "
              "correct-length record would put it",
              wo.peer_horizon[1] != w3::PROBE);
    }
}

// (5) llm_net_send_buf_flush @0x0049d7ff (row 19) is NOT an emitter -- see tx_emit_chat.h's header. It
// sends [bytes, cursor), zeroes the cursor, and returns the OLD cursor -- UNCONDITIONALLY, even when
// cursor == 0 (there is no dirty-check guard here, unlike the chat pair above).
void test_send_buf_flush() {
    {
        tx_chat::fixture f;
        f.cursor = 10;
        for (int i = 0; i < 10; ++i) f.buf[i] = static_cast<uint8_t>(0x50 + i);
        tx_chat::g_sent.clear();
        tx_chat::g_sent_calls = 0;
        const int32_t ret     = mh::lockstep::detail::send_buf_flush(f.st(), tx_chat::recording_calls());
        check("send_buf_flush sends exactly [bytes, cursor)",
              static_cast<int32_t>(tx_chat::g_sent.size()) == 10 &&
                  std::memcmp(tx_chat::g_sent.data(), f.buf, 10) == 0);
        check("send_buf_flush returns the OLD cursor (0x0049d817/0x0049d83f)", ret == 10);
        check("send_buf_flush zeroes the cursor (0x0049d82f)", f.cursor == 0);
    }
    { // no dirty-check guard: it sends even when there is nothing queued
        tx_chat::fixture f;
        f.cursor = 0;
        tx_chat::g_sent.clear();
        tx_chat::g_sent_calls = 0;
        const int32_t ret     = mh::lockstep::detail::send_buf_flush(f.st(), tx_chat::recording_calls());
        check("send_buf_flush calls transport_send even at cursor 0 (unconditional, unlike the chat "
              "pair's reset_first guard)",
              tx_chat::g_sent_calls == 1);
        check("...with a zero length", tx_chat::g_sent.empty());
        check("send_buf_flush returns 0 when nothing was queued", ret == 0);
    }
}

// ==================================================================================================
// RI-WIRE W6-B -- tests for the eight emitters in lockstep/tx_emit_ctrl.{h,cpp}. All eight are
// UNGUARDED (the wire-emitter inventory) -- the benign form, see ctrl_emit.h's header comment on why the
// split must not be normalised. STATE: `estate()`'s emit_state is reused verbatim via tx5::fixture
// (tx_emit_ctrl.h's own header note: "NO NEW STRUCT"), so only a new ctrl_emit_calls recorder is needed.
// ==================================================================================================
namespace tx_ctrl {

std::vector<uint8_t> g_sent;
void                 capture_send(uint8_t *b, int32_t n) { g_sent.assign(b, b + n); }

struct call_log {
    std::vector<int32_t> pbs_calls;                  // player_by_side_id args, in call order
    std::vector<int32_t> reset_player_horizon_calls; // slot_reset's tail
    std::vector<order>   enqueued;                   // broadcast_resync_state's tail
    int32_t              fill_calls = 0;             // broadcast_resync_state's tail
    void                 reset() { *this = call_log{}; }
};
call_log g_log;

// Modelled, not rigged, same mapping as tx5::side_to_index / rx's own side_to_index: side 100+i ->
// player i. Recorded on top of the model so kick's "does NOT call this at all" claim is a real
// assertion (an empty log), not an absence of evidence.
int32_t player_by_side_id(int32_t side_id) {
    g_log.pbs_calls.push_back(side_id);
    return tx5::side_to_index(side_id);
}

const mh::lockstep::ctrl_emit_calls &recording_calls() {
    static const mh::lockstep::ctrl_emit_calls cc = {
        capture_send,
        player_by_side_id,
        [](int32_t p) { g_log.reset_player_horizon_calls.push_back(p); },
        [](void *p, uint32_t n, uint8_t f) -> void * {
            ++g_log.fill_calls;
            std::memset(p, f, n);
            return p;
        },
        [](const order *o) -> int32_t {
            g_log.enqueued.push_back(*o);
            return 0;
        },
    };
    return cc;
}

} // namespace tx_ctrl

// (1) llm_net_lockstep_send_presence_lost @0x0049e328 (row 5): MSG_CONTROL/CTL_PLAYER_LEFT, NO
// payload, UNGUARDED, NO tail.
void test_tx_ctrl_presence_lost() {
    {
        tx5::fixture f;
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_presence_lost(f.st(), tx_ctrl::recording_calls());
        check("presence_lost emits the table wire length (2)",
              static_cast<int32_t>(tx_ctrl::g_sent.size()) == 2);
        check("presence_lost's outer tag is MSG_CONTROL", tx_ctrl::g_sent[0] == mh::lockstep::MSG_CONTROL);
        check("presence_lost's inner tag is CTL_PLAYER_LEFT (1)",
              tx_ctrl::g_sent[1] == mh::lockstep::CTL_PLAYER_LEFT);
        check("presence_lost leaves the cursor at 0", f.cursor == 0);
    }
    { // UNGUARDED
        tx5::fixture f;
        f.cursor = 6;
        std::memset(f.buf, 0xAB, 6);
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_presence_lost(f.st(), tx_ctrl::recording_calls());
        check("presence_lost is UNGUARDED: it appends after a pending batch rather than discarding it",
              static_cast<int32_t>(tx_ctrl::g_sent.size()) == 6 + 2 &&
                  tx_ctrl::g_sent[6] == mh::lockstep::MSG_CONTROL);
    }
    { // NO TAIL: an emitter that only sends is silently wrong for the emitters the wire-emitter inventory's
        // "Tail logic" section lists -- this one is deliberately NOT among them, so nothing else may move.
        tx5::fixture f;
        f.status_flags  = 0xFF; // sentinel: must survive untouched
        f.peer_state[0] = 0xAB; // sentinel
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_presence_lost(f.st(), tx_ctrl::recording_calls());
        check("presence_lost has no tail: status_flags is untouched", f.status_flags == 0xFF);
        check("presence_lost has no tail: peer_state is untouched", f.peer_state[0] == 0xAB);
        check("presence_lost has no tail: no outward call besides the send happened",
              tx_ctrl::g_log.pbs_calls.empty() && tx_ctrl::g_log.reset_player_horizon_calls.empty() &&
                  tx_ctrl::g_log.enqueued.empty() && tx_ctrl::g_log.fill_calls == 0);
    }
}

// (2) llm_net_lockstep_send_slot_reset @0x0049e189 (row 7): MSG_CONTROL/CTL_SLOT_RESET, payload =
// side_id (4B, the WIRE side_id, not pidx), UNGUARDED. THE TAIL (0x0049e217..0x0049e225) clears
// LS_HORIZON_PENDING and calls reset_player_horizon(pidx) -- the PLAYER INDEX, not the wire side_id --
// which is the one place this function's OWN Ghidra plate prose gets it wrong (tx_emit_ctrl.h's long
// note); this is the assertion that catches a translation that "fixed" it back to the prose's reading.
void test_tx_ctrl_slot_reset() {
    {
        tx5::fixture f;
        f.status_flags = 0xFF; // every bit set, incl. LS_HORIZON_PENDING(0x80) -- must clear ONLY that one
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_slot_reset(f.st(), tx_ctrl::recording_calls(), 105); // side 105 -> pidx 5
        check("slot_reset emits the table wire length (6)", static_cast<int32_t>(tx_ctrl::g_sent.size()) == 6);
        check("slot_reset's outer tag is MSG_CONTROL", tx_ctrl::g_sent[0] == mh::lockstep::MSG_CONTROL);
        check("slot_reset's inner tag is CTL_SLOT_RESET (4)", tx_ctrl::g_sent[1] == mh::lockstep::CTL_SLOT_RESET);
        int32_t got = 0;
        std::memcpy(&got, tx_ctrl::g_sent.data() + 2, sizeof(int32_t));
        check("slot_reset's payload is the WIRE side_id verbatim (105), NOT pidx (5) -- 0x0049e1d8 "
              "sources EBP-0x1c, the side_id copy",
              got == 105);

        check("slot_reset's tail clears LS_HORIZON_PENDING and nothing else (0xFF & ~0x80 = 0x7F)",
              f.status_flags == 0x7F);
        check("slot_reset's tail calls reset_player_horizon with the PLAYER INDEX (5), NOT the wire "
              "side_id (105) -- the discrepancy tx_emit_ctrl.h documents against this function's own plate",
              tx_ctrl::g_log.reset_player_horizon_calls.size() == 1 &&
                  tx_ctrl::g_log.reset_player_horizon_calls[0] == 5);
        check("slot_reset resolves pidx via player_by_side_id(105) before any buffer write (0x0049e1a7)",
              tx_ctrl::g_log.pbs_calls.size() == 1 && tx_ctrl::g_log.pbs_calls[0] == 105);
    }
    { // UNGUARDED
        tx5::fixture f;
        f.cursor = 6;
        std::memset(f.buf, 0xAB, 6);
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_slot_reset(f.st(), tx_ctrl::recording_calls(), 100);
        check("slot_reset is UNGUARDED: it appends after a pending batch",
              static_cast<int32_t>(tx_ctrl::g_sent.size()) == 6 + 6 &&
                  tx_ctrl::g_sent[6] == mh::lockstep::MSG_CONTROL);
    }
}

// (3) llm_net_lockstep_send_horizon_ack @0x0049e01f (row 9): MSG_CONTROL/tag 6 (CTL_LEAVE_CONSENSUS,
// the parser's own name for this byte -- see tx_emit_ctrl.h), payload = side_id (4B), UNGUARDED. THE
// TAIL (0x0049e0ad..0x0049e0c3) writes 2 ("ack") into peer_state[pidx][PlayerSide] and SETS
// LS_HORIZON_PENDING -- the opposite direction of slot_reset's tail.
void test_tx_ctrl_horizon_ack() {
    {
        tx5::fixture f;
        f.player_side = 3;
        for (int32_t i = 0; i < N * N; ++i) f.peer_state[i] = 0xAB;
        f.status_flags = 0; // must become 0x80
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_horizon_ack(f.st(), tx_ctrl::recording_calls(), 102); // side 102 -> pidx 2
        check("horizon_ack emits the table wire length (6)", static_cast<int32_t>(tx_ctrl::g_sent.size()) == 6);
        check("horizon_ack's inner tag is 6 (CTL_LEAVE_CONSENSUS, the receiver's name for this byte)",
              tx_ctrl::g_sent[1] == mh::lockstep::CTL_LEAVE_CONSENSUS);
        int32_t got = 0;
        std::memcpy(&got, tx_ctrl::g_sent.data() + 2, sizeof(int32_t));
        check("horizon_ack's payload is side_id (102), the same shape as slot_reset", got == 102);

        check("horizon_ack's tail writes 2 at row pidx(2), column PlayerSide(3) (0x0049e0ad..c3)",
              f.peer_state[2 * N + 3] == 2);
        bool others_untouched = true;
        for (int32_t i = 0; i < N * N; ++i)
            if (i != 2 * N + 3) others_untouched = others_untouched && f.peer_state[i] == 0xAB;
        check("horizon_ack's tail touches only that one cell", others_untouched);
        check("horizon_ack's tail SETS LS_HORIZON_PENDING (0x80)", (f.status_flags & 0x80) != 0);
    }
    { // UNGUARDED
        tx5::fixture f;
        f.cursor = 6;
        std::memset(f.buf, 0xAB, 6);
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_horizon_ack(f.st(), tx_ctrl::recording_calls(), 100);
        check("horizon_ack is UNGUARDED: it appends after a pending batch",
              static_cast<int32_t>(tx_ctrl::g_sent.size()) == 6 + 6 &&
                  tx_ctrl::g_sent[6] == mh::lockstep::MSG_CONTROL);
    }
}

// (4) llm_net_lockstep_send_horizon_desync @0x0049e0d4 (row 10): BYTE-FOR-BYTE the same shape as
// horizon_ack -- differs only in the inner tag (7, CTL_STATUS_RESET_REQ) and the tail's stamped value
// (3, "desync", not 2). Not factored into a shared helper: the two differ in BOTH the tag and the
// value, so a shared helper would need two parameters doing all the work of two functions.
void test_tx_ctrl_horizon_desync() {
    {
        tx5::fixture f;
        f.player_side = 4;
        for (int32_t i = 0; i < N * N; ++i) f.peer_state[i] = 0xAB;
        f.status_flags = 0;
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_horizon_desync(f.st(), tx_ctrl::recording_calls(), 103); // pidx 3
        check("horizon_desync emits the table wire length (6)",
              static_cast<int32_t>(tx_ctrl::g_sent.size()) == 6);
        check("horizon_desync's inner tag is 7 (CTL_STATUS_RESET_REQ)",
              tx_ctrl::g_sent[1] == mh::lockstep::CTL_STATUS_RESET_REQ);
        int32_t got = 0;
        std::memcpy(&got, tx_ctrl::g_sent.data() + 2, sizeof(int32_t));
        check("horizon_desync's payload is side_id (103)", got == 103);

        check("horizon_desync's tail writes 3 ('desync') at row pidx(3), column PlayerSide(4), NOT the "
              "ack value 2 send_horizon_ack's tail writes",
              f.peer_state[3 * N + 4] == 3);
        bool others_untouched = true;
        for (int32_t i = 0; i < N * N; ++i)
            if (i != 3 * N + 4) others_untouched = others_untouched && f.peer_state[i] == 0xAB;
        check("horizon_desync's tail touches only that one cell", others_untouched);
        check("horizon_desync's tail also SETS LS_HORIZON_PENDING, same as horizon_ack's",
              (f.status_flags & 0x80) != 0);
    }
    { // UNGUARDED
        tx5::fixture f;
        f.cursor = 6;
        std::memset(f.buf, 0xAB, 6);
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_horizon_desync(f.st(), tx_ctrl::recording_calls(), 100);
        check("horizon_desync is UNGUARDED too",
              static_cast<int32_t>(tx_ctrl::g_sent.size()) == 6 + 6 &&
                  tx_ctrl::g_sent[6] == mh::lockstep::MSG_CONTROL);
    }
}

// (5) llm_net_send_lockstep_kick @0x0049dc16 (row 13): MSG_CONTROL/CTL_KICK, payload = side_id
// straight from the argument, UNGUARDED, NO tail -- and UNLIKE the three tests above, it does NOT call
// player_by_side_id at all (confirmed absent from the 0x8d-byte body; tx_emit_ctrl.h's own note).
void test_tx_ctrl_kick() {
    {
        tx5::fixture f;
        f.status_flags = 0xFF;
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_lockstep_kick(f.st(), tx_ctrl::recording_calls(), 107);
        check("kick emits the table wire length (6)", static_cast<int32_t>(tx_ctrl::g_sent.size()) == 6);
        check("kick's outer tag is MSG_CONTROL, inner is CTL_KICK (10)",
              tx_ctrl::g_sent[0] == mh::lockstep::MSG_CONTROL && tx_ctrl::g_sent[1] == mh::lockstep::CTL_KICK);
        int32_t got = 0;
        std::memcpy(&got, tx_ctrl::g_sent.data() + 2, sizeof(int32_t));
        check("kick's payload is side_id straight from the argument (107), no lookup", got == 107);
        check("kick does NOT call player_by_side_id, unlike slot_reset/horizon_ack/horizon_desync",
              tx_ctrl::g_log.pbs_calls.empty());
        check("kick has no tail: status_flags is untouched", f.status_flags == 0xFF);
    }
    { // UNGUARDED
        tx5::fixture f;
        f.cursor = 6;
        std::memset(f.buf, 0xAB, 6);
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_lockstep_kick(f.st(), tx_ctrl::recording_calls(), 100);
        check("kick is UNGUARDED: it appends after a pending batch",
              static_cast<int32_t>(tx_ctrl::g_sent.size()) == 6 + 6 &&
                  tx_ctrl::g_sent[6] == mh::lockstep::MSG_CONTROL);
    }
}

// (6) llm_net_send_lockstep_step_size @0x0049da72 (row 14): the STALE-PROTOTYPE function
// (tx_emit_ctrl.h's note) -- really `double step_size` on the stack, not `void`. MSG_CONTROL/
// CTL_SET_STEP, payload = the 8-byte double verbatim, UNGUARDED, NO tail.
void test_tx_ctrl_step_size() {
    {
        tx5::fixture f;
        f.status_flags = 0xFF;
        tx_ctrl::g_log.reset();
        const double step = 3.75;
        mh::lockstep::detail::send_lockstep_step_size(f.st(), tx_ctrl::recording_calls(), step);
        check("step_size emits the table wire length (10)", static_cast<int32_t>(tx_ctrl::g_sent.size()) == 10);
        check("step_size's outer tag is MSG_CONTROL, inner is CTL_SET_STEP (11)",
              tx_ctrl::g_sent[0] == mh::lockstep::MSG_CONTROL &&
                  tx_ctrl::g_sent[1] == mh::lockstep::CTL_SET_STEP);
        double got = 0.0;
        std::memcpy(&got, tx_ctrl::g_sent.data() + 2, sizeof(double));
        check("step_size's payload is the double argument verbatim", got == step);
        check("step_size has no tail: status_flags is untouched", f.status_flags == 0xFF);
        check("step_size has no tail: no outward call besides the send",
              tx_ctrl::g_log.pbs_calls.empty() && tx_ctrl::g_log.enqueued.empty());
    }
    { // UNGUARDED
        tx5::fixture f;
        f.cursor = 6;
        std::memset(f.buf, 0xAB, 6);
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_lockstep_step_size(f.st(), tx_ctrl::recording_calls(), 1.5);
        check("step_size is UNGUARDED: it appends after a pending batch",
              static_cast<int32_t>(tx_ctrl::g_sent.size()) == 6 + 10 &&
                  tx_ctrl::g_sent[6] == mh::lockstep::MSG_CONTROL);
    }
}

// (7) llm_net_lockstep_broadcast_resync_state @0x0049d8ef (row 16): MSG_CONTROL/CTL_RESYNC_BEGIN,
// payload = exec_time (8B), UNGUARDED. THE TAIL (0x0049d96f..0x0049d9d3) builds a zeroed 0x44-byte
// llm_strat_order and pushes it BY VALUE -- the "an emitter that only sends is silently wrong" case
// The wire-emitter inventory names first, so every field is asserted, not just that a call happened.
void test_tx_ctrl_broadcast_resync_state() {
    {
        tx5::fixture f;
        tx_ctrl::g_log.reset();
        const double exec_time = 88.0;
        mh::lockstep::detail::lockstep_broadcast_resync_state(f.st(), tx_ctrl::recording_calls(),
                                                              mh::lockstep::reimpl_fixes{}, exec_time);
        check("broadcast_resync_state emits the table wire length (10)",
              static_cast<int32_t>(tx_ctrl::g_sent.size()) == 10);
        check("broadcast_resync_state's outer/inner tags are MSG_CONTROL / CTL_RESYNC_BEGIN (13)",
              tx_ctrl::g_sent[0] == mh::lockstep::MSG_CONTROL &&
                  tx_ctrl::g_sent[1] == mh::lockstep::CTL_RESYNC_BEGIN);
        double got = 0.0;
        std::memcpy(&got, tx_ctrl::g_sent.data() + 2, sizeof(double));
        check("broadcast_resync_state's payload is exec_time verbatim", got == exec_time);

        check("broadcast_resync_state's tail zeroes the local order first (fill_data called once)",
              tx_ctrl::g_log.fill_calls == 1);
        check("broadcast_resync_state's tail enqueues exactly one order", tx_ctrl::g_log.enqueued.size() == 1);
        const order &o = tx_ctrl::g_log.enqueued[0];
        check("the enqueued order carries the SAME exec_time as the wire payload", o.exec_time == exec_time);
        check("the enqueued order is a global event (owner_and_kind=0xf0)", o.owner_and_kind == 0xf0);
        check("the enqueued order's param0 is the order code 13 (CTL_RESYNC_BEGIN)",
              o.param0 == mh::lockstep::CTL_RESYNC_BEGIN);
        check("the enqueued order_code mirrors param0 via the dword read-back", o.order_code == 13);
        check("the enqueued order's unit_index is explicitly cleared", o.unit_index == 0);
        bool args_clear = true;
        for (int i = 0; i < 13; ++i) args_clear = args_clear && o.args[i] == 0;
        check("the enqueued order's args[] are zeroed -- the fill_data evidence", args_clear);
    }
    { // UNGUARDED
        tx5::fixture f;
        f.cursor = 6;
        std::memset(f.buf, 0xAB, 6);
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::lockstep_broadcast_resync_state(f.st(), tx_ctrl::recording_calls(),
                                                              mh::lockstep::reimpl_fixes{}, 1.0);
        check("broadcast_resync_state is UNGUARDED: it appends after a pending batch",
              static_cast<int32_t>(tx_ctrl::g_sent.size()) == 6 + 10 &&
                  tx_ctrl::g_sent[6] == mh::lockstep::MSG_CONTROL);
    }

    // ---- D17: the WIRING of the MP D14 clamp, which is the part nobody was testing ---------------
    //
    // test_d14_resync_order_exec_time has covered the clamp FUNCTION since the day it shipped, eight
    // cases including NaN, and it passed every single run while the fix was reaching no ship build at
    // all. That is not a gap in those cases; it is what a pure-function test structurally cannot see.
    // It answers "does this arithmetic work", and the bug was "does anything call it". So these
    // assertions are about the CALL, and they are written against BOTH consumers because exec_time is
    // used twice and clamping one of them would be worse than clamping neither.
    {
        tx5::fixture               f;
        mh::lockstep::reimpl_fixes fx;
        fx.resync_order_horizon = true;
        tx_ctrl::g_log.reset();
        // 2.0 is the literal the original passes (`PUSH 0x40000000 / PUSH 0x0` @0x0049da0b) and is
        // permanently in the past, which is the whole defect.
        mh::lockstep::detail::lockstep_broadcast_resync_state(f.st(), tx_ctrl::recording_calls(), fx, 2.0);
        double got = 0.0;
        std::memcpy(&got, tx_ctrl::g_sent.data() + 2, sizeof(double));
        // D24 CHANGED THIS EXPECTATION, and the change IS the fix: it used to be `== f.horizon`.
        // The bare horizon is the barrier every peer is already sitting at, so an order stamped there
        // is released the instant each peer arrives -- a race the leader's local mirror always wins.
        // The value must now be one step PAST it. If this reads `f.horizon` again, D24 has regressed.
        const double barrier = f.horizon + f.step_size; // max(horizon, clock) is the horizon here
        check("D24 wiring: with the fix ON, the WIRE payload clears the barrier by one step",
              got == barrier);
        check("D24 wiring: with the fix ON, the LOCAL order carries the same clamped value",
              tx_ctrl::g_log.enqueued.size() == 1 && tx_ctrl::g_log.enqueued[0].exec_time == barrier);
        // The pre-D24 value stated separately, so a reviewer can see WHICH way this moved and a
        // silent revert cannot pass by being "some clamped number".
        check("D24 wiring: and it is NOT the bare horizon D17 stamped", got != f.horizon);
        // The two must not merely both be clamped -- they must be EQUAL, since a peer takes the wire
        // value and the leader keeps the local one. This is the assertion that would fail if a later
        // edit clamped at one of the two tails instead of at the top.
        check("D17 wiring: wire payload and local order agree", got == tx_ctrl::g_log.enqueued[0].exec_time);
    }
    {
        // D24, the property that actually makes the peers agree, asserted through the CALL: the
        // stamped time is STRICTLY GREATER than the horizon, which is the highest clock any peer can
        // have reached (peer_clock <= peer_committed <= our advertised horizon). Strictly, not >=,
        // because release_due fires on equality.
        tx5::fixture               f;
        mh::lockstep::reimpl_fixes fx;
        fx.resync_order_horizon = true;
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::lockstep_broadcast_resync_state(f.st(), tx_ctrl::recording_calls(), fx, 2.0);
        double got = 0.0;
        std::memcpy(&got, tx_ctrl::g_sent.data() + 2, sizeof(double));
        check("D24: the stamped exec_time is STRICTLY past the barrier every peer is pinned at",
              got > f.horizon);
    }
    {
        // D24: the pathological floor. If the clock has somehow outrun the horizon, clamping to the
        // horizon would stamp the order IN THE PAST for everyone -- the original D14 defect exactly.
        // The max() floor is what stops that, and this is the only case that exercises it.
        tx5::fixture f;
        f.horizon    = 3.0;
        f.game_clock = 12.0;
        f.step_size  = 0.5;
        mh::lockstep::reimpl_fixes fx;
        fx.resync_order_horizon = true;
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::lockstep_broadcast_resync_state(f.st(), tx_ctrl::recording_calls(), fx, 2.0);
        double got = 0.0;
        std::memcpy(&got, tx_ctrl::g_sent.data() + 2, sizeof(double));
        check("D24: a clock ahead of the horizon becomes the floor, not the horizon", got == 12.5);
    }
    {
        // OFF is the faithful original, and it has to stay that way or the asymmetric oracle compares
        // ours-with-fix against original-without-fix and waves real divergence through.
        tx5::fixture f;
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::lockstep_broadcast_resync_state(f.st(), tx_ctrl::recording_calls(),
                                                              mh::lockstep::reimpl_fixes{}, 2.0);
        double got = 0.0;
        std::memcpy(&got, tx_ctrl::g_sent.data() + 2, sizeof(double));
        check("D17: fix OFF passes 2.0 through unclamped (the stock behaviour)", got == 2.0);
        check("D17: fix OFF leaves the local order at 2.0 too",
              tx_ctrl::g_log.enqueued.size() == 1 && tx_ctrl::g_log.enqueued[0].exec_time == 2.0);
    }
    {
        // ON must not RETARD an exec_time that is already ahead of the barrier -- the clamp raises,
        // it does not overwrite. Same direction the pure function's own cases assert, checked here
        // through the call so a wrong argument ORDER (exec_time/barrier swapped) cannot pass.
        // The whole fixture is early-match here: D24 raised the comparison target by one step, so
        // the clock and step have to be early too or 2.0 is no longer ahead of anything.
        tx5::fixture f;
        f.horizon    = 1.5;
        f.game_clock = 1.4;
        f.step_size  = 0.25;
        mh::lockstep::reimpl_fixes fx;
        fx.resync_order_horizon = true;
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::lockstep_broadcast_resync_state(f.st(), tx_ctrl::recording_calls(), fx, 2.0);
        double got = 0.0;
        std::memcpy(&got, tx_ctrl::g_sent.data() + 2, sizeof(double));
        check("D17: the clamp RAISES only -- an exec_time already past the barrier is untouched",
              got == 2.0);
    }
}

// (8) llm_net_send_lockstep_resync_resume @0x0049dbb4 (row 17): the OTHER stale-prototype function --
// `RET 0x8` like step_size, but this body never reads the popped stack bytes at all. MSG_CONTROL/
// CTL_RESYNC_END, NO payload, UNGUARDED, NO tail.
void test_tx_ctrl_resync_resume() {
    {
        tx5::fixture f;
        f.status_flags = 0xFF;
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_lockstep_resync_resume(f.st(), tx_ctrl::recording_calls());
        check("resync_resume emits the table wire length (2)", static_cast<int32_t>(tx_ctrl::g_sent.size()) == 2);
        check("resync_resume's outer/inner tags are MSG_CONTROL / CTL_RESYNC_END (14)",
              tx_ctrl::g_sent[0] == mh::lockstep::MSG_CONTROL &&
                  tx_ctrl::g_sent[1] == mh::lockstep::CTL_RESYNC_END);
        check("resync_resume has no tail: status_flags is untouched", f.status_flags == 0xFF);
        check("resync_resume has no tail: no outward call besides the send",
              tx_ctrl::g_log.pbs_calls.empty() && tx_ctrl::g_log.enqueued.empty() &&
                  tx_ctrl::g_log.fill_calls == 0 && tx_ctrl::g_log.reset_player_horizon_calls.empty());
    }
    { // UNGUARDED
        tx5::fixture f;
        f.cursor = 6;
        std::memset(f.buf, 0xAB, 6);
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_lockstep_resync_resume(f.st(), tx_ctrl::recording_calls());
        check("resync_resume is UNGUARDED: it appends after a pending batch",
              static_cast<int32_t>(tx_ctrl::g_sent.size()) == 6 + 2 &&
                  tx_ctrl::g_sent[6] == mh::lockstep::MSG_CONTROL);
    }
}

// (9) THE ROUND TRIP: each of the eight's own output fed through the real dispatcher with a trailing
// probe. TWO of the eight need their "keep draining" precondition arranged deliberately -- the same
// trap test_w3_emit_round_trip's own comment names -- CTL_PLAYER_LEFT and CTL_KICK both have a `stop`
// arm (rx_dispatch.cpp lines 344/408); the other six never do.
void test_tx_ctrl_round_trip() {
    { // presence_lost -> CTL_PLAYER_LEFT. Stops UNLESS count_active_players() > 1; mp_world()'s rx_log
        // already defaults active_answer to 2, but it is set explicitly here so the precondition this
        // handler needs is visible rather than accidental.
        tx5::fixture f;
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_presence_lost(f.st(), tx_ctrl::recording_calls());
        world wo           = mp_world();
        g_rx.active_answer = 2;
        packet p;
        p.blob(tx_ctrl::g_sent.data(), tx_ctrl::g_sent.size());
        p.u8(mh::lockstep::MSG_HORIZON).f64(w3::PROBE);
        feed(wo, p, 101);
        check("the parser consumes presence_lost's own record at exactly its emitted length",
              wo.peer_horizon[1] == w3::PROBE);
    }
    { // slot_reset -> CTL_SLOT_RESET, always drains
        tx5::fixture f;
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_slot_reset(f.st(), tx_ctrl::recording_calls(), 102);
        world  wo = mp_world();
        packet p;
        p.blob(tx_ctrl::g_sent.data(), tx_ctrl::g_sent.size());
        p.u8(mh::lockstep::MSG_HORIZON).f64(w3::PROBE);
        feed(wo, p, 101);
        check("the parser consumes slot_reset's own record at exactly its emitted length",
              wo.peer_horizon[1] == w3::PROBE);
    }
    { // horizon_ack -> tag 6, always drains (handle_leave_consensus returns drain_again on every arm)
        tx5::fixture f;
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_horizon_ack(f.st(), tx_ctrl::recording_calls(), 102);
        world  wo = mp_world();
        packet p;
        p.blob(tx_ctrl::g_sent.data(), tx_ctrl::g_sent.size());
        p.u8(mh::lockstep::MSG_HORIZON).f64(w3::PROBE);
        feed(wo, p, 101);
        check("the parser consumes horizon_ack's own record at exactly its emitted length",
              wo.peer_horizon[1] == w3::PROBE);
    }
    { // horizon_desync -> tag 7, always drains
        tx5::fixture f;
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_horizon_desync(f.st(), tx_ctrl::recording_calls(), 102);
        world  wo = mp_world();
        packet p;
        p.blob(tx_ctrl::g_sent.data(), tx_ctrl::g_sent.size());
        p.u8(mh::lockstep::MSG_HORIZON).f64(w3::PROBE);
        feed(wo, p, 101);
        check("the parser consumes horizon_desync's own record at exactly its emitted length",
              wo.peer_horizon[1] == w3::PROBE);
    }
    { // kick -> CTL_KICK. STOPS if side_id == local_player_index(100); named side must be someone else.
        tx5::fixture f;
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_lockstep_kick(f.st(), tx_ctrl::recording_calls(), 101); // not local (100)
        world  wo = mp_world();
        packet p;
        p.blob(tx_ctrl::g_sent.data(), tx_ctrl::g_sent.size());
        p.u8(mh::lockstep::MSG_HORIZON).f64(w3::PROBE);
        feed(wo, p, 101);
        check("the parser consumes kick's own record at exactly its emitted length",
              wo.peer_horizon[1] == w3::PROBE);
    }
    { // step_size -> CTL_SET_STEP, always drains; also observable end to end, not just a length probe
        tx5::fixture f;
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_lockstep_step_size(f.st(), tx_ctrl::recording_calls(), 0.375);
        world  wo = mp_world();
        packet p;
        p.blob(tx_ctrl::g_sent.data(), tx_ctrl::g_sent.size());
        p.u8(mh::lockstep::MSG_HORIZON).f64(w3::PROBE);
        feed(wo, p, 101);
        check("the parser consumes step_size's own record at exactly its emitted length",
              wo.peer_horizon[1] == w3::PROBE);
        check("...and the step size itself round-trips", wo.step_size == 0.375);
    }
    { // broadcast_resync_state -> CTL_RESYNC_BEGIN, always drains; only the wire half of the split
        // operation is exercised here -- the local-enqueue half is test_tx_ctrl_broadcast_resync_state's job.
        tx5::fixture f;
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::lockstep_broadcast_resync_state(f.st(), tx_ctrl::recording_calls(),
                                                              mh::lockstep::reimpl_fixes{}, 42.0);
        world  wo = mp_world();
        packet p;
        p.blob(tx_ctrl::g_sent.data(), tx_ctrl::g_sent.size());
        p.u8(mh::lockstep::MSG_HORIZON).f64(w3::PROBE);
        feed(wo, p, 101);
        check("the parser consumes broadcast_resync_state's own record at exactly its emitted length",
              wo.peer_horizon[1] == w3::PROBE);
    }
    { // resync_resume -> CTL_RESYNC_END, always drains
        tx5::fixture f;
        tx_ctrl::g_log.reset();
        mh::lockstep::detail::send_lockstep_resync_resume(f.st(), tx_ctrl::recording_calls());
        world  wo = mp_world();
        packet p;
        p.blob(tx_ctrl::g_sent.data(), tx_ctrl::g_sent.size());
        p.u8(mh::lockstep::MSG_HORIZON).f64(w3::PROBE);
        feed(wo, p, 101);
        check("the parser consumes resync_resume's own record at exactly its emitted length",
              wo.peer_horizon[1] == w3::PROBE);
    }
}

// (1) THE ANCHOR. The emitted bytes must match what the original's own instructions produce: outer
// tag at [0], inner tag at [1] when the outer is MSG_CONTROL, payload immediately after, and a total
// equal to the table's `wire` column. This is the test that keeps the round trip honest.
void test_w3_emit_layout() {
    const w3::payload_bytes pay;
    for (const auto &k : w3::KINDS) {
        w3::emitter   e;
        const int32_t len = e.emit(w3::record_for(k, pay.b));

        char msg[160];
        std::snprintf(msg, sizeof(msg), "%s emits its table wire length (%d)", k.name, k.wire);
        check(msg, len == k.wire && static_cast<int32_t>(w3::g_sent.size()) == k.wire);

        std::snprintf(msg, sizeof(msg), "%s puts its outer tag at [0]", k.name);
        check(msg, !w3::g_sent.empty() && w3::g_sent[0] == k.outer);

        if (k.has_inner) {
            std::snprintf(msg, sizeof(msg), "%s puts its inner tag at [1]", k.name);
            check(msg, w3::g_sent.size() > 1 && w3::g_sent[1] == k.inner);
        }

        const int32_t off = k.has_inner ? 2 : 1;
        std::snprintf(msg, sizeof(msg), "%s puts its payload at [%d] unchanged", k.name, off);
        check(msg, k.payload_len == 0 ||
                       std::memcmp(w3::g_sent.data() + off, pay.b,
                                   static_cast<size_t>(k.payload_len)) == 0);

        std::snprintf(msg, sizeof(msg), "%s leaves the cursor at 0 after sending", k.name);
        check(msg, e.cursor == 0);
    }
}

// (2) THE ROUND TRIP. Feed each emitted record to our own parser with a probe behind it. The probe
// is applied exactly when the parser consumed the record at the length the emitter wrote.
void test_w3_emit_round_trip() {
    for (const auto &k : w3::KINDS) {
        // Target side 102 (player 2), never the sender (101) -- so the departure and kick handlers
        // cannot remove the peer whose horizon the probe is about to set.
        w3::side_and_horizon sh{102, 77.5};
        double               d       = 33.25;
        int32_t              side    = 102;
        const void          *payload = nullptr;
        switch (k.payload_len) {
            case 12: payload = &sh; break;
            case 8: payload = &d; break;
            case 4: payload = &side; break;
            default: payload = nullptr; break;
        }

        w3::emitter w;
        w.emit(w3::record_for(k, payload));

        world wo = mp_world();
        // THE PROBE ORACLE ONLY READS A LENGTH IF THE HANDLER KEEPS DRAINING. A handler that
        // legitimately returns `stop` never reaches the probe, and that looks identical to a
        // one-byte length disagreement -- so each handler's "carry on" precondition has to be set
        // up deliberately. The drop pair is the one that bites: handle_peer_drop compares the
        // message's horizon against st.peer_horizon[target] and TEARS THE SESSION DOWN when they
        // differ, and mp_world() starts every peer at NO_HORIZON. Matching them here is what makes
        // this a test of the record's LENGTH rather than of the drop policy, which
        // test_dispatch_drop_synced_vs_unsynced already owns.
        wo.peer_horizon[2] = sh.horizon; // player 2 == side 102; not human, so the barrier is unmoved
        packet p;
        p.blob(w3::g_sent.data(), w3::g_sent.size());
        p.u8(mh::lockstep::MSG_HORIZON).f64(w3::PROBE); // the trailing probe
        feed(wo, p, 101);

        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "the parser consumes %s at exactly the emitted length", k.name);
        check(msg, wo.peer_horizon[1] == w3::PROBE);
    }

    // Two payloads that ARE observable end to end, so the round trip asserts a value and not only a
    // length: the step size the parser writes, and the horizon it records.
    {
        double      step = 0.125;
        w3::emitter w;
        w.emit(w3::ctl(mh::lockstep::CTL_SET_STEP, &step, sizeof(step)));
        world  wo = mp_world();
        packet p;
        p.blob(w3::g_sent.data(), w3::g_sent.size());
        feed(wo, p, 101);
        check("CTL_SET_STEP round-trips its double through the parser", wo.step_size == 0.125);
    }
    {
        double      h = 88.5;
        w3::emitter w;
        w.emit(w3::outer_only(mh::lockstep::MSG_HORIZON, &h, sizeof(h), true));
        world  wo = mp_world();
        packet p;
        p.blob(w3::g_sent.data(), w3::g_sent.size());
        feed(wo, p, 101);
        check("MSG_HORIZON round-trips its double through the parser", wo.peer_horizon[1] == 88.5);
    }
}

// (3) MUTATION CHECKS. done_when requires that a wrong tag, a wrong payload offset and a missing
// cursor reset each be caught BY NAME -- i.e. that these tests can actually fail. Each block builds
// the DEFECT deliberately and asserts the property the correct emitter has does NOT hold.
void test_w3_emit_mutations() {
    const w3::payload_bytes pay;

    { // a wrong OUTER tag: the layout check's [0] assertion is what catches it
        w3::emitter e;
        auto        r = w3::ctl(mh::lockstep::CTL_SET_STEP, pay.b, 8);
        r.outer       = mh::lockstep::MSG_KEEPALIVE; // defect
        e.emit(r);
        check("mutation: a wrong outer tag is visible at [0]",
              w3::g_sent[0] != mh::lockstep::MSG_CONTROL);
    }
    { // a wrong INNER tag
        w3::emitter e;
        auto        r = w3::ctl(mh::lockstep::CTL_SET_STEP, pay.b, 8);
        r.inner       = mh::lockstep::CTL_KICK; // defect
        e.emit(r);
        check("mutation: a wrong inner tag is visible at [1]",
              w3::g_sent[1] != mh::lockstep::CTL_SET_STEP);
    }
    { // a wrong PAYLOAD OFFSET: emitting the inner tag when the record has none shifts the payload
        // by one, which is exactly the off-by-one the layout test is built to catch.
        w3::emitter e;
        auto        r = w3::outer_only(mh::lockstep::MSG_HORIZON, pay.b, 8);
        r.has_inner   = true; // defect: an extra header byte
        r.inner       = 0;
        e.emit(r);
        check("mutation: an extra header byte moves the payload off [1]",
              std::memcmp(w3::g_sent.data() + 1, pay.b, 8) != 0);
        check("mutation: and it makes the record one byte too long",
              static_cast<int32_t>(w3::g_sent.size()) == 10); // table says MSG_HORIZON is 9
    }
    { // a MISSING cursor reset. emit_ctrl always resets after sending, so the defect is modelled by
        // dirtying the cursor and NOT asking for the guard -- which is the real, deliberate
        // behaviour of the fifteen unguarded emitters and must remain distinguishable from the
        // three guarded ones.
        double      h = 5.0;
        w3::emitter e;
        e.cursor = 6; // a pending order batch, mid-accumulation
        auto p   = e.pb();
        mh::net::emit_ctrl(p, w3::outer_only(mh::lockstep::MSG_HORIZON, &h, 8, /*reset_first=*/false),
                           w3::capture_send);
        check("mutation: without the guard the record is APPENDED after the pending batch",
              static_cast<int32_t>(w3::g_sent.size()) == 6 + 9 && w3::g_sent[6] == mh::lockstep::MSG_HORIZON);
    }
    { // ...and WITH the guard the pending batch is discarded. This pair is the W1 finding expressed
        // as a test: the two disciplines differ observably, so nobody can "tidy" one into the other
        // without a named failure here.
        double      h = 5.0;
        w3::emitter e;
        e.cursor = 6;
        auto p   = e.pb();
        mh::net::emit_ctrl(p, w3::outer_only(mh::lockstep::MSG_HORIZON, &h, 8, /*reset_first=*/true),
                           w3::capture_send);
        check("the guard DISCARDS the pending batch and sends only the record",
              static_cast<int32_t>(w3::g_sent.size()) == 9 && w3::g_sent[0] == mh::lockstep::MSG_HORIZON);
    }
}

void test_dispatch_cursor_walk() {
    // THE CURSOR IS THE THING MOST LIKELY TO BE WRONG, so it gets tested by CONSEQUENCE: three
    // messages back to back in one buffer. If any handler advances by the wrong width the next tag
    // byte lands mid-field, the outer switch sees garbage, and the run ends in the garbled path.
    world  w = mp_world();
    packet p;
    p.u8(mh::lockstep::MSG_HORIZON).f64(70.0);                 // 1 + 8
    p.ctl(mh::lockstep::CTL_SET_STEP).f64(0.25);               // 2 + 8
    p.ctl(mh::lockstep::CTL_HORIZON_CHECK).i32(101).f64(70.0); // 2 + 4 + 8
    check("three chained messages parse", feed(w, p, 101));
    check("chained: horizon landed", w.peer_horizon[1] == 70.0);
    check("chained: step size landed", w.step_size == 0.25);
    check("chained: the third handler ran", g_rx.acks.size() == 1);
    // The decisive one: a cursor slip anywhere upstream ends in the garbled handler instead.
    check("chained: no message was mis-parsed", g_rx.plain_lines == 0 && g_rx.outcomes.empty());
}

void test_dispatch_horizon_and_pending() {
    { // LS_HORIZON_PENDING routes the SAME message to a different table, and only the live table
        // re-runs the barrier. Getting the flag backwards silently stalls or races the barrier.
        world  w = mp_world();
        packet p;
        p.u8(mh::lockstep::MSG_HORIZON).f64(70.0);
        feed(w, p, 101);
        check("horizon goes live when not pending", w.peer_horizon[1] == 70.0);
        check("live horizon re-runs the barrier", w.committed == 60.0); // min(own 60, peer 70)
    }
    {
        world w     = mp_world();
        w.status    = mh::lockstep::LS_HORIZON_PENDING;
        w.committed = 999.0;
        packet p;
        p.u8(mh::lockstep::MSG_HORIZON).f64(70.0);
        feed(w, p, 101);
        check("horizon parks in PENDING when the flag is set", w.peer_pending[1] == 70.0);
        check("pending horizon does NOT touch the live table", w.peer_horizon[1] == NO_HORIZON);
        check("pending horizon does NOT re-run the barrier", w.committed == 999.0);
    }
}

void test_dispatch_order() {
    // The order message's exec_time DOUBLES as the sender's horizon -- the same eight bytes are both
    // the order's schedule and the barrier input. A translation that enqueued the order without
    // extracting the horizon would look completely fine until the barrier stopped advancing.
    world w = mp_world();
    order o{};
    o.exec_time      = 65.0;
    o.unit_index     = 7;
    o.owner_and_kind = 0x21;
    packet p;
    p.u8(mh::lockstep::MSG_ORDER).blob(&o, sizeof(o));
    feed(w, p, 101);
    check("order is integrity-checked", g_rx.integrity_checked.size() == 1);
    check("order is enqueued", g_rx.enqueued.size() == 1);
    check("the enqueued order survives intact", g_rx.enqueued[0].unit_index == 7);
    check("the order's exec_time IS the sender's horizon", w.peer_horizon[1] == 65.0);
    check("and it re-ran the barrier", w.committed == 60.0);
}

void test_dispatch_keepalive() {
    { // The emergency bump fires on EXACTLY the fifth nag, not the fifth-or-later.
        world w         = mp_world();
        w.step_size     = 2.0;
        w.emergency_mul = 3.0;
        for (int i = 1; i <= 7; ++i) {
            packet p;
            p.u8(mh::lockstep::MSG_KEEPALIVE).i32(100); // addressed to us
            feed(w, p, 101);
            if (i == 4) check("no emergency bump before the fifth nag", g_rx.extends.empty());
            if (i == 5) check("the emergency bump fires on the fifth nag", g_rx.extends.size() == 1);
        }
        check("and it fires ONCE, not on every nag past five", g_rx.extends.size() == 1);
        check("the bump is step*mul + clock", g_rx.extends[0] == 2.0 * 3.0 + 50.0);
        check("the bump is what landed in HORIZON", w.horizon == 56.0);
    }
    { // A keepalive naming somebody ELSE must not touch our stall counters at all.
        world  w = mp_world();
        packet p;
        p.u8(mh::lockstep::MSG_KEEPALIVE).i32(101); // not our local_side
        feed(w, p, 101);
        check("a keepalive for another side does not nag us", w.stall_nag == 0 && w.stalls == 0);
    }
    { // The leader escalation is a SEPARATE branch that runs before the side_id test, and it asks
        // is_local_leader_peer(-1) -- "exclude nobody".
        world w            = mp_world();
        g_rx.leader_answer = 1;
        w.active           = 0; // so active*100 = 0 < trigger after the first increment
        packet p;
        p.u8(mh::lockstep::MSG_KEEPALIVE).i32(101);
        feed(w, p, 101);
        check("the leader check excludes nobody", g_rx.leader_asked.size() == 1 && g_rx.leader_asked[0] == -1);
        check("the resync trigger counted", w.resync_trigger == 1);
        check("and it escalated past the threshold", g_rx.force_resyncs == 1);
    }
    {
        world w            = mp_world();
        g_rx.leader_answer = 0;
        packet p;
        p.u8(mh::lockstep::MSG_KEEPALIVE).i32(101);
        feed(w, p, 101);
        check("a non-leader never escalates", g_rx.force_resyncs == 0 && w.resync_trigger == 0);
    }

    // ---- C3: the MIGRATED resync_trigger_gate. Both flag states, because "off" is a claim too: it
    // asserts our body still reproduces the ORIGINAL, which is what the asymmetric oracle rests on.
    // This is the fix that used to be the byte splice at 0x0049c508, inside a body we now promote.
    {
        mh::lockstep::reimpl_fixes on;
        on.resync_trigger_gate = true;

        // GATE ON, routine at-horizon nag: countdown at its 0x3c reload means "no sustained silence",

        {
            // so the nag must NOT count. This is the whole point of the fix -- at a tight lookahead these
            // stream as normal play and the cumulative counter used to ratchet into a spurious resync.
            world w            = mp_world();
            g_rx.leader_answer = 1;
            w.active           = 0;
            w.retry_countdown  = 0x3c;
            packet p;
            p.u8(mh::lockstep::MSG_KEEPALIVE).i32(101);
            feed(w, p, 101, on);
            check("gate ON: a routine at-horizon nag does not count", w.resync_trigger == 0);
            check("gate ON: and therefore does not escalate", g_rx.force_resyncs == 0);
            // FIX AUDIT: the branch RAN and SUPPRESSED. On the rig this is the whole difference
            // between "the fix agreed with the byte patch" and "our body never reached the site" --
            // both of which leave RESYNC_TRIGGER_COUNT at 0.
            check("gate ON: the audit records an evaluation that was suppressed",
                  w.audit_ev == 1 && w.audit_ok == 0);
        }
        // GATE ON, GENUINE sustained silence: the countdown has drained past SYNC_OVERLAY_AFTER, so
        {
            // the nag counts and still escalates. A gate that suppressed real stalls would trade a
            // spurious resync for a session that never recovers.
            world w            = mp_world();
            g_rx.leader_answer = 1;
            w.active           = 0;
            w.retry_countdown  = 0x37; // one below SYNC_OVERLAY_AFTER
            packet p;
            p.u8(mh::lockstep::MSG_KEEPALIVE).i32(101);
            feed(w, p, 101, on);
            check("gate ON: a genuine sustained stall still counts", w.resync_trigger == 1);
            check("gate ON: and still escalates", g_rx.force_resyncs == 1);
            check("gate ON: the audit records an evaluation that was ALLOWED",
                  w.audit_ev == 1 && w.audit_ok == 1);
        }
        { // The BOUNDARY, and it matters: the predicate is `< 0x38`, so 0x38 itself is still routine.
            world w            = mp_world();
            g_rx.leader_answer = 1;
            w.active           = 0;
            w.retry_countdown  = 0x38;
            packet p;
            p.u8(mh::lockstep::MSG_KEEPALIVE).i32(101);
            feed(w, p, 101, on);
            check("gate ON: countdown == SYNC_OVERLAY_AFTER is still routine (strict <)",
                  w.resync_trigger == 0);
        }
        // GATE OFF at the SAME countdown that the gate would have suppressed. Off must be the stock
        {
            // behaviour -- unconditional -- or every asymmetric run is comparing two different engines.
            world w            = mp_world();
            g_rx.leader_answer = 1;
            w.active           = 0;
            w.retry_countdown  = 0x3c;
            packet p;
            p.u8(mh::lockstep::MSG_KEEPALIVE).i32(101);
            feed(w, p, 101); // default = all fixes off
            check("gate OFF: counts unconditionally, exactly as the original does",
                  w.resync_trigger == 1);
            check("gate OFF: and escalates", g_rx.force_resyncs == 1);
        }
        // The gate must not touch the OTHER half of the handler. The stall-nag bookkeeping below the
        {
            // leader branch is addressed to us and is not what the fix is about; suppressing it too would
            // be a silent behaviour change riding along with a knob that never claimed it.
            world w            = mp_world();
            g_rx.leader_answer = 1;
            w.active           = 0;
            w.retry_countdown  = 0x3c;
            packet p;
            p.u8(mh::lockstep::MSG_KEEPALIVE).i32(w.local_side);
            feed(w, p, 101, on);
            check("gate ON: the stall-nag bookkeeping for our own side is untouched",
                  w.stall_nag == 1 && w.stalls == 1);
        }
    }
}

void test_dispatch_control_basics() {
    { // tag 3 has NO body at all -- it must consume its two header bytes and nothing else
        world  w = mp_world();
        packet p;
        p.ctl(mh::lockstep::CTL_NOP);
        p.u8(mh::lockstep::MSG_HORIZON).f64(70.0);
        check("CTL_NOP keeps draining", feed(w, p, 101));
        check("CTL_NOP consumed exactly its header", w.peer_horizon[1] == 70.0);
    }
    { // An out-of-range INNER tag is silently skipped; an out-of-range OUTER tag is a garbled stream.
        // Same-looking bounds check, opposite consequence -- this pair is the test for that.
        world  w = mp_world();
        packet p;
        p.ctl(0x10); // > 0xf
        check("an unknown inner tag keeps draining", feed(w, p, 101));
        check("an unknown inner tag warns about nothing", g_rx.plain_lines == 0);
        check("an unknown inner tag ends no session", g_rx.outcomes.empty());
    }
    {
        world  w = mp_world();
        packet p;
        p.u8(6); // > 5
        check("an unknown outer tag still returns drain_again", feed(w, p, 101));
        check("an unknown outer tag warns", g_rx.plain_lines == 1);
        check("an unknown outer tag opens the outcome dialog with 7",
              g_rx.outcomes.size() == 1 && g_rx.outcomes[0] == 7);
        check("an unknown outer tag flags the session ended",
              (w.status & mh::lockstep::LS_SESSION_ENDED) != 0);
        // THE ONE THAT SEPARATES IT FROM THE CONTROL HANDLERS: no presence callbacks.
        check("the garbled path eliminates WITHOUT presence_lost", g_rx.presence_lost.empty());
        check("but it did eliminate the other human",
              (w.players[1].status_flags & HUMAN) == 0);
    }
    { // A tag byte of 0 underflows to 0xff, which is > 4 -- it must take the garbled path, not wrap
        world  w = mp_world();
        packet p;
        p.u8(0);
        feed(w, p, 101);
        check("tag 0 underflows into the garbled path", g_rx.outcomes.size() == 1);
    }
}

void test_dispatch_departures() {
    { // CTL_PLAYER_LEFT with somebody still playing: barrier re-runs, drain continues
        world w = mp_world();
        w.human(2);
        g_rx.active_answer = 2;
        packet p;
        p.ctl(mh::lockstep::CTL_PLAYER_LEFT);
        check("player-left keeps draining while others remain", feed(w, p, 101));
        check("the leaver is marked gone", (w.players[1].status_flags & HUMAN) == 0);
        check("player-left names the leaver", g_rx.player_lines == 1 && g_rx.last_text_id == 165);
        check("nobody was told about a presence loss yet", g_rx.presence_lost.empty());
    }
    { // ...and when it was the LAST peer, the same handler abandons the drain instead
        world w            = mp_world();
        g_rx.active_answer = 1;
        packet p;
        p.ctl(mh::lockstep::CTL_PLAYER_LEFT);
        check("player-left STOPS when it was the last peer", !feed(w, p, 101));
        check("the last-peer path reports the presence loss",
              g_rx.presence_lost.size() == 1 && g_rx.presence_lost[0] == 1);
        check("the last-peer path re-points chat", g_rx.chat_recalcs == 1);
    }
    { // CTL_SESSION_ENDED: every other human goes, WITH a presence callback each
        world w = mp_world();
        w.human(2);
        w.human(3);
        packet p;
        p.ctl(mh::lockstep::CTL_SESSION_ENDED);
        check("session-ended keeps draining", feed(w, p, 101));
        check("session-ended flags the status bit",
              (w.status & mh::lockstep::LS_SESSION_ENDED) != 0);
        check("session-ended eliminates every OTHER human", g_rx.presence_lost.size() == 3);
        check("session-ended spares the local side",
              (w.players[0].status_flags & HUMAN) != 0);
    }
}

void test_dispatch_drop_synced_vs_unsynced() {
    // THE PAIR THE DRAFT FACTORED INTO ONE BODY. If the factoring lost the difference, the two tags
    // become indistinguishable -- so the test is precisely that they differ in exactly one call and
    // agree on everything else.
    auto run_drop = [](uint8_t tag, double wire_horizon, int32_t named_side, int32_t active) {
        world w            = mp_world();
        w.peer_horizon[1]  = 70.0;
        g_rx.active_answer = active;
        packet p;
        p.ctl(tag).i32(named_side).f64(wire_horizon);
        const bool drained = feed(w, p, 101);
        return std::make_pair(drained, w.ls_players);
    };

    const auto a           = run_drop(mh::lockstep::CTL_DROP_SYNCED, 70.0, 101, 2);
    const int  busy_synced = g_rx.busywaits, resync_synced = g_rx.time_resyncs;
    const auto b             = run_drop(mh::lockstep::CTL_DROP_UNSYNCED, 70.0, 101, 2);
    const int  busy_unsynced = g_rx.busywaits, resync_unsynced = g_rx.time_resyncs;

    check("both drop tags keep draining when horizons agree", a.first && b.first);
    check("both drop tags decrement the player count", a.second == 1 && b.second == 1);
    check("both drop tags resync the clock", resync_synced == 1 && resync_unsynced == 1);
    check("DROP_SYNCED busy-waits", busy_synced == 1);
    check("DROP_UNSYNCED does NOT busy-wait -- the ONE difference", busy_unsynced == 0);

    { // Horizons DISAGREE -> the session is torn down and the drain stops.
        //
        // THE FIXTURE NEEDS A THIRD HUMAN, and finding that out is the point of this test. The original
        // clears the dropped peer's HUMAN bit at 0x0049cb44 -- BEFORE the FCOMP and before the
        // elimination sweep -- so the dropped peer is already excluded from its own sweep. With only
        // two humans the sweep therefore has nobody to act on and reports ZERO presence losses, which
        // is correct and looks exactly like a broken sweep. Player 2 exists so the two mechanisms
        // (marked-gone directly vs. eliminated by the sweep) can be told apart.
        world w = mp_world();
        w.human(2);
        w.peer_horizon[1]  = 70.0;
        g_rx.active_answer = 2;
        packet p;
        p.ctl(mh::lockstep::CTL_DROP_SYNCED).i32(101).f64(71.0); // != 70.0
        check("a disagreeing horizon STOPS the drain", !feed(w, p, 101));
        check("a disagreeing horizon marks the DROPPED peer gone directly",
              (w.players[1].status_flags & HUMAN) == 0);
        check("the dropped peer is NOT also swept (it was excluded before the loop)",
              g_rx.presence_lost.size() == 1 && g_rx.presence_lost[0] == 2);
        check("a disagreeing horizon sweeps the remaining humans",
              (w.players[2].status_flags & HUMAN) == 0);
    }
    { // A NaN on the wire compares UNORDERED, which the original's FCOMP/JZ treats as EQUAL. The
        // C++-natural `==` would take the disagree path -- i.e. would drop the match. This assertion
        // is the whole reason x87_equal_or_unordered exists in handle_peer_drop.
        world w            = mp_world();
        w.peer_horizon[1]  = std::numeric_limits<double>::quiet_NaN();
        g_rx.active_answer = 2;
        packet p;
        p.ctl(mh::lockstep::CTL_DROP_SYNCED).i32(101).f64(70.0);
        check("a NaN horizon counts as AGREEING, so the drain continues", feed(w, p, 101));
        check("a NaN horizon does not sweep the remaining humans", g_rx.presence_lost.empty());
    }
    { // the drop names US -> eliminate everyone else and stop, before any horizon compare
        world  w = mp_world();
        packet p;
        p.ctl(mh::lockstep::CTL_DROP_SYNCED).i32(100).f64(0.0); // 100 == local_side
        check("a drop naming the local side STOPS", !feed(w, p, 101));
        check("a drop naming us eliminates the others", g_rx.presence_lost.size() == 1);
    }
    { // horizons agree but we were the last peer -> teardown, not a decrement
        world w            = mp_world();
        w.peer_horizon[1]  = 70.0;
        g_rx.active_answer = 1;
        packet p;
        p.ctl(mh::lockstep::CTL_DROP_SYNCED).i32(101).f64(70.0);
        check("last peer + agreeing horizon STOPS", !feed(w, p, 101));
        check("last peer re-points chat", g_rx.chat_recalcs == 1);
        check("last peer does NOT decrement the count", w.ls_players == 2);
    }
}

// ==================================================================================================
// mp:U19h -- THE GRACEFUL-LEAVE HORIZON RACE, reproduced offline.
//
// THE FAILURE. A 3-peer lockstep match; one peer quits cleanly; roughly one run in six BOTH survivors
// wrongly end their match (`; on_gameover ENTER sess=2 outcome=8`) on a peer nobody eliminated. The
// interleaving, measured on the rig 2026-09-22 (the quitter's mh_temporal.log shows LOCAL_HORIZON and
// COMMITTED stepping 3260/3290 -> 3320/3320 across a 255 ms park gap with no frame in between; both
// survivors' lockstep rows pin PEER_HORIZON[2] = 3320 five times while the record carried 3290):
//
//   1. graceful_leave_park (mh/seams/net_lockstep.cpp) freezes the quitter's horizon at
//      H_d = GAME_CLOCK + lookahead, broadcasts it once, sets g_leave_frozen, and then SPINS waiting
//      for the survivors to park -- calling llm_net_lockstep_dispatch() every iteration.
//   2. Inside that drain the MSG_KEEPALIVE arm fires its emergency horizon bump on the FIFTH
//      consecutive nag naming us: `horizon = step * EMERGENCY_STEP_MUL + clock`, EMERGENCY_STEP_MUL
//      being 2.0 (/eng/mh.exe .rdata @0x005017b4). The quitter's clock is frozen by the park, so that
//      is clk + 2*step -- one whole lookahead step ABOVE the H_d it just froze. This is libmh code and
//      it knows nothing about g_leave_frozen; the freeze lives in the seam.
//   3. Every survivor applies that advert, so PEER_HORIZON[quitter] = clk + 2*step, while the removal
//      record send_removal_record (lockstep/tx_emit.cpp) builds next carries PEER_HORIZON[quitter]
//      from the QUITTER's own table -- the H_d the park's step (3) stamped there.
//   4. handle_peer_drop compares the two, they differ, and the DISAGREE arm runs
//      eliminate_other_humans(notify=true): PLAYER_HUMAN cleared on every participating slot plus a
//      presence_lost(i, 1) each. mode 1 is what makes sim_player_presence_lost select outcome 8
//      rather than 6 (sim/sim_player_presence_lost.cpp, `outcome = (mode == 0) ? 6 : 8`) -- see
//      MP-U19H in sim_player_presence_lost_selftest.cpp for that last link, driven over its own
//      fixture with the exact roster this sweep leaves behind.
//
// WHY IT IS HERE RATHER THAN ON THE RIG. The bug is a RACE: it needs the fifth nag to land inside the
// park window, which is why twenty green rig runs prove nothing and one red one proves only that the
// dice came up. Every step of the chain above is pure translated logic over explicit state, so the
// interleaving can simply be BUILT -- deterministically, in milliseconds, with the wire bytes emitted
// by the real emitters rather than hand-assembled.
//
// WHAT EACH ARM IS WORTH. (B2) is the one that must stay green forever: it is the invariant "the record
// agrees => the match goes on", i.e. the property any fix has to establish. (A) pins the producer that
// breaks it and is expected to go RED the moment the bump's arithmetic changes.
// ==================================================================================================

namespace u19h {

// The measured run's numbers. The frozen clock and the 30 ms step are the rig's; EMERGENCY_MUL is the
// binary's own constant, spelled out rather than inherited from world's default so that a later change
// to that default cannot silently make this arm assert something else.
constexpr double FROZEN_CLOCK  = 3.260;
constexpr double STEP          = 0.030;
constexpr double EMERGENCY_MUL = 2.0;

// H_d exactly as graceful_leave_park spells it (`clk + look`) and the bump exactly as the keepalive arm
// spells it (`step * mul + clock`). Written as the two PRODUCTION expressions, not as `H_D + STEP`:
// the association is the production one, and a re-associated expectation could agree by luck on values
// where the real pair does not.
constexpr double H_D  = FROZEN_CLOCK + STEP;
constexpr double BUMP = STEP * EMERGENCY_MUL + FROZEN_CLOCK;

constexpr int32_t QUITTER_SIDE = 102; // -> index 2 under the fixture's side_to_index
constexpr int32_t QUITTER_IDX  = 2;
constexpr int32_t SURVIVOR_B   = 1; // side 101, the human this bug eliminates for nothing

// The two survivors' own horizons, both AHEAD of the quitter's H_d/bump -- which is the whole reason
// the quitter is the one holding the barrier while it parks. Distinct values so the committed barrier
// below identifies WHICH slot it came from instead of matching several at once.
constexpr double LOCAL_HORIZON      = 3.400;
constexpr double SURVIVOR_B_HORIZON = 3.500;

// A 3-human match seen from SURVIVOR A: we are side 100 (index 0), survivor B is 101 (index 1), the
// quitter is 102 (index 2). The same shape stands in for the quitter's own engine in arm (A) -- the
// keepalive handler reads none of the roster.
world three_peer_world() {
    world w = mp_world();
    w.human(2);
    w.clock                    = FROZEN_CLOCK;
    w.step_size                = STEP;
    w.emergency_mul            = EMERGENCY_MUL;
    w.horizon                  = LOCAL_HORIZON;
    w.peer_horizon[SURVIVOR_B] = SURVIVOR_B_HORIZON;
    return w;
}

// The quitter's extend advert, built by the REAL emitter so the bytes the survivor parses are the ones
// production would have sent.
void feed_extend_from_quitter(world &wo, double horizon) {
    tx5::fixture f;
    tx5::g_log.reset();
    mh::lockstep::detail::send_lockstep_extend(f.st(), tx5::recording_calls(), horizon);
    packet p;
    p.blob(tx5::g_sent.data(), tx5::g_sent.size());
    feed(wo, p, QUITTER_SIDE);
}

// The quitter's removal record, likewise built by the real emitter. `stamped` is what park step (3)
// left in the quitter's PEER_HORIZON[own slot] -- the single value this whole bug turns on.
bool feed_removal_from_quitter(world &wo, double stamped) {
    tx5::fixture f;
    f.peer_horizon[QUITTER_IDX] = stamped;
    tx5::g_log.reset();
    tx5::g_log.count_active_players_answer = 2;
    mh::lockstep::detail::player_remove(f.st(), tx5::recording_calls(), QUITTER_SIDE);
    packet p;
    p.blob(tx5::g_sent.data(), tx5::g_sent.size());
    return feed(wo, p, QUITTER_SIDE);
}

} // namespace u19h

void test_u19h_leave_park_horizon_race() {
    double bump_on_the_wire = 0.0;

    // ---- (A) THE PRODUCER: the emergency bump climbs off the frozen H_d --------------------------
    {
        world w                   = u19h::three_peer_world();
        w.horizon                 = u19h::H_D; // park step (1): the frozen advert, already broadcast
        const double clock_before = w.clock;

        for (int nag = 1; nag <= 4; ++nag) {
            packet p;
            p.u8(mh::lockstep::MSG_KEEPALIVE).i32(w.local_side); // the nag names US
            feed(w, p, 101);
        }
        check("U19h(A): four nags inside the park leave the frozen horizon alone",
              g_rx.extends.empty() && w.horizon == u19h::H_D);

        packet fifth;
        fifth.u8(mh::lockstep::MSG_KEEPALIVE).i32(w.local_side);
        feed(w, fifth, 101);

        check("U19h(A): the fifth nag fires exactly one emergency bump", g_rx.extends.size() == 1);
        // THE VALUE, asserted as the production expression over the fixture's own state rather than
        // against a literal -- a literal would still pass if the handler read the wrong globals.
        check("U19h(A): the bump is step * EMERGENCY_STEP_MUL + clock",
              g_rx.extends.size() == 1 &&
                  g_rx.extends[0] == w.step_size * w.emergency_mul + w.clock);
        check("U19h(A): and what went on the wire is what landed in LOCAL_HORIZON",
              g_rx.extends.size() == 1 && w.horizon == g_rx.extends[0]);
        // THE BUG, stated: with the clock frozen the bump is a whole step ABOVE the H_d the park froze
        // and advertised. The seam's freeze flag is invisible from here, which is exactly why it fires.
        check("U19h(A): the park did not move the clock", w.clock == clock_before);
        check("U19h(A): so the bump lands one lookahead step ABOVE the frozen H_d",
              g_rx.extends.size() == 1 && g_rx.extends[0] > u19h::H_D &&
                  g_rx.extends[0] == u19h::BUMP);

        bump_on_the_wire = g_rx.extends[0];
    }

    // ---- (B) THE CONSEQUENCE: the survivor's removal-record compare ------------------------------
    //
    // Both halves run the SAME two datagrams over the same world, in the order the survivor sees them:
    // the quitter's advert, then its removal record. The only difference is which value park step (3)
    // stamped -- the racing H_d or the value the survivors actually hold.
    { // B1 -- the LOSING interleaving. The record carries H_d; we hold the bump.
        world wo = u19h::three_peer_world();
        u19h::feed_extend_from_quitter(wo, bump_on_the_wire);
        check("U19h(B1): the survivor applied the quitter's bumped advert",
              wo.peer_horizon[u19h::QUITTER_IDX] == bump_on_the_wire &&
                  wo.peer_horizon[u19h::QUITTER_IDX] != u19h::H_D);
        // ...and the quitter, lowest of the three, is what the barrier currently sits on -- the state
        // the park is waiting for and the thing the removal is supposed to lift.
        check("U19h(B1): the parked quitter holds the barrier", wo.committed == bump_on_the_wire);

        const bool drained = u19h::feed_removal_from_quitter(wo, u19h::H_D);

        check("U19h(B1): a record that disagrees with PEER_HORIZON abandons the drain", !drained);
        // The quitter is marked gone BEFORE the compare, on both arms -- so it is not the discriminator
        // and is asserted here only so a reader does not mistake it for one.
        check("U19h(B1): the quitter is marked gone either way",
              (wo.players[u19h::QUITTER_IDX].status_flags & HUMAN) == 0);
        // THE DAMAGE: a survivor nobody eliminated loses PLAYER_HUMAN and gets a FORCED presence loss.
        check("U19h(B1): the OTHER SURVIVOR's PLAYER_HUMAN is cleared by the sweep",
              (wo.players[u19h::SURVIVOR_B].status_flags & HUMAN) == 0);
        check("U19h(B1): eliminate_other_humans notified exactly that slot",
              g_rx.presence_lost.size() == 1 && g_rx.presence_lost[0] == u19h::SURVIVOR_B);
        // mode 1 == FORCED, which is the half that picks outcome 8 over 6 downstream.
        check("U19h(B1): and it notified it as FORCED (mode 1), which selects outcome 8",
              g_rx.presence_modes.size() == 1 && g_rx.presence_modes[0] == 1u);
        check("U19h(B1): the torn-down session never reaches the carry-on tail",
              g_rx.time_resyncs == 0 && wo.ls_players == 2);
    }
    { // B2 -- THE INVARIANT. Same two datagrams; park step (3) stamped the value the survivors hold.
        // This is what any fix must make true, and it must stay green forever.
        world wo = u19h::three_peer_world();
        u19h::feed_extend_from_quitter(wo, bump_on_the_wire);

        const bool drained = u19h::feed_removal_from_quitter(wo, bump_on_the_wire);

        check("U19h(B2): an AGREEING record keeps the drain alive", drained);
        check("U19h(B2): the other survivor keeps PLAYER_HUMAN",
              (wo.players[u19h::SURVIVOR_B].status_flags & HUMAN) != 0);
        check("U19h(B2): no presence callback fires at all", g_rx.presence_lost.empty());
        // The carry-on tail, which is how "the match goes on" is observable: the barrier re-committed,
        // a peer counted out of both counters, the clock resynced, and (tag 8) the busy-wait.
        // The barrier was pinned to the quitter's bump before the record landed; afterwards the
        // quitter no longer participates, so it is OUR horizon (the lower of the two survivors') that
        // holds it. That transition is what "the match goes on" means in state terms.
        check("U19h(B2): it re-commits the barrier without the departed peer",
              wo.committed == u19h::LOCAL_HORIZON && wo.committed != bump_on_the_wire);
        check("U19h(B2): it counts the quitter out of both peer counters",
              wo.ls_players == 1 && wo.lobby_hosts == 1);
        check("U19h(B2): and it takes CTL_DROP_SYNCED's resync tail",
              g_rx.time_resyncs == 1 && g_rx.busywaits == 1);
    }
}

void test_dispatch_kick_and_resets() {
    {
        world  w = mp_world();
        packet p;
        p.ctl(mh::lockstep::CTL_KICK).i32(100); // names us
        check("a kick naming us STOPS the drain", !feed(w, p, 101));
        check("a kick naming us eliminates the others", g_rx.presence_lost.size() == 1);
    }
    {
        world  w = mp_world();
        packet p;
        p.ctl(mh::lockstep::CTL_KICK).i32(101); // names somebody else
        check("a kick naming somebody else keeps draining", feed(w, p, 101));
        check("a kick naming somebody else changes nothing", g_rx.presence_lost.empty());
    }
    { // CTL_SLOT_RESET clears the pending flag AND re-inits the horizon row (batch A's function)
        world w           = mp_world();
        w.status          = mh::lockstep::LS_HORIZON_PENDING;
        w.peer_pending[1] = 77.0;
        packet p;
        p.ctl(mh::lockstep::CTL_SLOT_RESET).i32(101);
        feed(w, p, 101);
        check("slot reset clears the pending flag",
              (w.status & mh::lockstep::LS_HORIZON_PENDING) == 0);
        check("slot reset promotes the parked horizon", w.peer_horizon[1] == 77.0);
    }
    { // CTL_STATUS_RESET_REQ answers only when BOTH gate halves hold
        world w                 = mp_world();
        w.status                = mh::lockstep::LS_HORIZON_PENDING;
        w.peer_state[1 * N + 0] = 0;
        packet p;
        p.ctl(mh::lockstep::CTL_STATUS_RESET_REQ).i32(101);
        feed(w, p, 101);
        check("status-reset answers when the gate holds", g_rx.status_resets.size() == 1);
        check("answering clears the pending flag",
              (w.status & mh::lockstep::LS_HORIZON_PENDING) == 0);
    }
    {
        world w                 = mp_world();
        w.status                = 0; // second half of the gate fails
        w.peer_state[1 * N + 0] = 0;
        packet p;
        p.ctl(mh::lockstep::CTL_STATUS_RESET_REQ).i32(101);
        feed(w, p, 101);
        check("status-reset stays silent without the pending flag", g_rx.status_resets.empty());
    }
    {
        world w                 = mp_world();
        w.status                = mh::lockstep::LS_HORIZON_PENDING;
        w.peer_state[1 * N + 0] = 3; // first half of the gate fails
        packet p;
        p.ctl(mh::lockstep::CTL_STATUS_RESET_REQ).i32(101);
        feed(w, p, 101);
        check("status-reset stays silent when our column is already set", g_rx.status_resets.empty());
    }
}

void test_dispatch_leave_consensus() {
    // Removal happens only once NO OTHER participating peer still reads PRESENT(6) for the leaver.
    {
        world w = mp_world();
        w.human(2);
        w.status                = mh::lockstep::LS_HORIZON_PENDING;
        w.peer_state[1 * N + 0] = 0;
        w.peer_state[1 * N + 2] = mh::lockstep::PEER_STATE_PRESENT; // player 2 still says present
        packet p;
        p.ctl(mh::lockstep::CTL_LEAVE_CONSENSUS).i32(101);
        feed(w, p, 101);
        check("consensus records the sender's vote",
              w.peer_state[1 * N + 1] == mh::lockstep::PEER_STATE_LEAVING);
        check("one dissenting peer VETOES the removal", g_rx.removed.empty());
    }
    {
        world w = mp_world();
        w.human(2);
        w.status                = mh::lockstep::LS_HORIZON_PENDING;
        w.peer_state[1 * N + 0] = 0;
        w.peer_state[1 * N + 2] = 0; // nobody still says present
        packet p;
        p.ctl(mh::lockstep::CTL_LEAVE_CONSENSUS).i32(101);
        feed(w, p, 101);
        check("full consensus removes the player",
              g_rx.removed.size() == 1 && g_rx.removed[0] == 101);
        check("removal decrements both counts", w.ls_players == 1 && w.lobby_hosts == 1);
        check("removal busy-waits then resyncs", g_rx.busywaits == 1 && g_rx.time_resyncs == 1);
    }
    { // THE SELF-VETO GUARD: the departing player's OWN column must not block its own removal.
        //
        // THE SENDER MUST NOT BE THE DEPARTING PLAYER, and mutation-testing is what forced that out.
        // The handler writes peer_state[pidx][sender_idx] = LEAVING BEFORE the loop, so when the peer
        // reports its own departure (sender_idx == pidx) that write lands on the very cell this test
        // is trying to plant PRESENT in. The first version of this test did exactly that, never
        // reached the guard, and passed just as happily with the guard deleted. Here player 2 reports
        // that player 1 left, so peer_state[1][1] survives to hit the `pidx != j` branch.
        world w = mp_world();
        w.human(2);
        w.status                = mh::lockstep::LS_HORIZON_PENDING;
        w.peer_state[1 * N + 0] = 0;
        w.peer_state[1 * N + 1] = mh::lockstep::PEER_STATE_PRESENT; // pidx == j: the leaver itself
        packet p;
        p.ctl(mh::lockstep::CTL_LEAVE_CONSENSUS).i32(101);
        feed(w, p, /*sender_side=*/102);
        check("the reporter's vote lands in ITS OWN column",
              w.peer_state[1 * N + 2] == mh::lockstep::PEER_STATE_LEAVING);
        check("a leaver's own PRESENT column does not veto its removal", g_rx.removed.size() == 1);
    }
}

void test_dispatch_step_size_and_resync() {
    {
        world w     = mp_world();
        w.step_size = 4.0;
        packet p;
        p.ctl(mh::lockstep::CTL_SCALE_STEP).f64(0.5).i32(100); // factor FIRST, then the side
        feed(w, p, 101);
        check("scale-step multiplies when it names us", w.step_size == 2.0);
    }
    {
        world w     = mp_world();
        w.step_size = 4.0;
        packet p;
        p.ctl(mh::lockstep::CTL_SCALE_STEP).f64(0.5).i32(101);
        feed(w, p, 101);
        check("scale-step is a no-op when it names somebody else", w.step_size == 4.0);
    }
    { // CTL_RESYNC_BEGIN synthesises a kind-0xf0 global-event order carrying the RAW tag
        world  w = mp_world();
        packet p;
        p.ctl(mh::lockstep::CTL_RESYNC_BEGIN).f64(88.0);
        feed(w, p, 101);
        check("resync-begin zeroes the order first", g_rx.fills == 1);
        check("resync-begin enqueues one order", g_rx.enqueued.size() == 1);
        const order &o = g_rx.enqueued[0];
        check("resync order carries the wire exec_time", o.exec_time == 88.0);
        check("resync order is a global event", o.owner_and_kind == 0xf0);
        check("resync order's event id is the RAW tag", o.param0 == mh::lockstep::CTL_RESYNC_BEGIN);
        check("resync order_code mirrors param0 via the dword read-back",
              o.order_code == mh::lockstep::CTL_RESYNC_BEGIN);
        check("resync order clears unit_index", o.unit_index == 0);
        // THE MEMSET'S OWN EVIDENCE IS args[], and mutation-testing is what established that: the
        // three obvious candidates all prove nothing. unit_index is written explicitly right after,
        // order_code's observable low half is param0 whether or not 0x0e..0x0f were zeroed, and
        // param0 is written outright. args[] is the only part of the 0x44 that fill_data zeroes and
        // nothing else touches -- so it is the only assertion that can actually fail when the memset
        // is removed.
        bool args_clear = true;
        for (int i = 0; i < 13; ++i) args_clear = args_clear && o.args[i] == 0;
        check("resync order is zeroed first -- args[] is the proof", args_clear);
        check("resync-begin latches the in-progress flag", w.resync_busy == 1);
    }
    {
        world w       = mp_world();
        w.resync_busy = 1;
        w.status      = mh::lockstep::LS_RESYNC_WAIT | mh::lockstep::LS_HORIZON_PENDING;
        packet p;
        p.ctl(mh::lockstep::CTL_RESYNC_END);
        feed(w, p, 101);
        check("resync-end clears the in-progress flag", w.resync_busy == 0);
        check("resync-end clears ONLY the resync-wait bit",
              (w.status & mh::lockstep::LS_RESYNC_WAIT) == 0 &&
                  (w.status & mh::lockstep::LS_HORIZON_PENDING) != 0);
        check("resync-end leaves the lobby and resyncs",
              g_rx.leave_resets == 1 && g_rx.busywaits == 1 && g_rx.time_resyncs == 1);
    }
    {
        // NET-SESSION's acceptance clause: THE LEADER PATH AND THE PEER PATH REACH ONE
        // IMPLEMENTATION. The peer arrives here through CTL_RESYNC_END (above); the leader performs
        // the same transition from llm_wait_screen_frame's deadline branch. Both now call
        // mh::lockstep::resync_complete_local, and this is the check that the two ENDS agree --
        // driven over the same world fixture, with the same recorders, so nothing but the entry
        // path differs.
        //
        // It is not vacuous just because both call one function: what it pins is that the RX arm
        // still ROUTES through it (a future edit that re-inlines the five acts here diverges from
        // the leader end state or the call tally) and that the leader arm performs all five.
        world w_peer       = mp_world();
        w_peer.resync_busy = 1;
        w_peer.status      = mh::lockstep::LS_RESYNC_WAIT | mh::lockstep::LS_HORIZON_PENDING;
        packet p;
        p.ctl(mh::lockstep::CTL_RESYNC_END);
        g_rx.reset();
        feed(w_peer, p, 101);
        const uint8_t peer_status = w_peer.status;
        const int32_t peer_busy   = w_peer.resync_busy;
        const int     peer_leaves = g_rx.leave_resets, peer_waits = g_rx.busywaits,
                  peer_resyncs = g_rx.time_resyncs;

        world w_lead       = mp_world();
        w_lead.resync_busy = 1;
        w_lead.status      = mh::lockstep::LS_RESYNC_WAIT | mh::lockstep::LS_HORIZON_PENDING;
        uint32_t deadline  = 1000;
        g_rx.reset();
        int         resumes = 0, pumps = 0;
        static int *s_resumes = nullptr;
        static int *s_pumps   = nullptr;
        s_resumes             = &resumes;
        s_pumps               = &pumps;
        const mh::lockstep::wait_screen_state ws{
            &deadline, {&w_lead.status, &w_lead.resync_busy}};
        const mh::lockstep::wait_screen_calls wc{
            [](int32_t) -> int32_t { return 1; }, // we ARE the leader
            []() -> uint32_t { return 9999u; },   // and the deadline has expired
            [](double) { ++*s_resumes; },         // CTL_RESYNC_END onto the wire
            []() { ++*s_pumps; },                 // the RX pump
            {[]() { ++g_rx.leave_resets; }, []() -> int32_t { ++g_rx.busywaits; return 0; },
             []() { ++g_rx.time_resyncs; }}};
        mh::lockstep::wait_screen_frame(ws, wc);

        check("leader and peer reach the SAME resync end state",
              w_lead.status == peer_status && w_lead.resync_busy == peer_busy);
        check("leader and peer perform the same three local acts, once each",
              g_rx.leave_resets == peer_leaves && g_rx.busywaits == peer_waits &&
                  g_rx.time_resyncs == peer_resyncs && peer_leaves == 1);
        check("only the LEADER puts the resume packet on the wire, and it still pumps",
              resumes == 1 && pumps == 1);
        check("the shared transition clears ONLY the resync-wait bit on both ends",
              (w_lead.status & mh::lockstep::LS_RESYNC_WAIT) == 0 &&
                  (w_lead.status & mh::lockstep::LS_HORIZON_PENDING) != 0 &&
                  w_lead.resync_busy == 0);
    }
}

void test_dispatch_chat_and_dead_tag() {
    { // Addressed to us -> printed. The mask is a bitmask over PlayerSide, not a side_id.
        world      w     = mp_world();
        const char msg[] = "hi";
        packet     p;
        p.u8(mh::lockstep::MSG_CHAT).u8(2).u8(1u << 0).blob(msg, 2);
        check("chat keeps draining", feed(w, p, 101));
        check("chat addressed to us prints", g_rx.prints_cyan == 1 && g_rx.chat_lines == 1);
        check("chat addressed to us is NOT appended to the backlog", g_rx.concats == 0);
    }
    { // Not addressed to us and no debug tap -> nothing at all, but the cursor still advanced
        world      w     = mp_world();
        const char msg[] = "hi";
        packet     p;
        p.u8(mh::lockstep::MSG_CHAT).u8(2).u8(1u << 1).blob(msg, 2);
        p.u8(mh::lockstep::MSG_HORIZON).f64(70.0);
        feed(w, p, 101);
        check("chat not addressed to us is silent", g_rx.prints_cyan == 0);
        check("chat still consumed length+mask+body", w.peer_horizon[1] == 70.0);
    }
    { // The debug tap makes us print it AND append it to the backlog -- the second mask test is
        // recomputed, not reused, which is what makes those two outcomes differ.
        world w          = mp_world();
        w.debug_tap      = 1;
        const char msg[] = "hi";
        packet     p;
        p.u8(mh::lockstep::MSG_CHAT).u8(2).u8(1u << 1).blob(msg, 2);
        feed(w, p, 101);
        check("the debug tap prints a message meant for someone else", g_rx.prints_cyan == 1);
        check("a tapped message IS appended to the backlog", g_rx.concats == 1);
    }
    { // tag 0x0f: UNREACHABLE in the shipped image (its only builder is called from nowhere), so a
        // crafted packet is the ONLY way it will ever execute. Tested because "unreachable today" is an
        // observation about this build, not a guarantee.
        world  w = mp_world();
        packet p;
        p.ctl(mh::lockstep::CTL_PEER_HORIZON).f64(90.0).i32(1234);
        check("the dead tag 0x0f keeps draining", feed(w, p, 101));
        check("the dead tag records the sender's horizon", w.peer[1].horizon == 90.0);
        check("the dead tag records the order marker", w.peer[1].order_marker == 1234);
    }
}

void test_dispatch_sender_already_gone() {
    // A packet from a peer already flagged PLAYER_GONE: the leader re-broadcasts the drop -- and then
    // the packet IS STILL PARSED, because the original's "skip" store is dead (0x0049c335 is
    // overwritten by 0x0049c33b on both paths). This asserts the BUG, deliberately.
    world w = mp_world();
    w.players[1].status_flags |= mh::lockstep::PLAYER_GONE;
    g_rx.leader_answer = 1;
    packet p;
    p.u8(mh::lockstep::MSG_HORIZON).f64(70.0);
    feed(w, p, 101);
    check("a packet from a gone peer re-broadcasts the drop",
          g_rx.timeout_drops.size() == 1 && g_rx.timeout_drops[0] == 101);
    check("...and the packet is STILL parsed (the skip store is dead)", w.peer_horizon[1] == 70.0);

    world w2 = mp_world();
    w2.players[1].status_flags |= mh::lockstep::PLAYER_GONE;
    g_rx.leader_answer = 0;
    packet p2;
    p2.u8(mh::lockstep::MSG_HORIZON).f64(70.0);
    feed(w2, p2, 101);
    check("a non-leader does not re-broadcast the drop", g_rx.timeout_drops.empty());
}

void test_no_players() {
    world w;
    check("no_players is 1 when the count is zero", mh::lockstep::detail::no_players(w.st()) == 1);
    w.count = 2;
    check("no_players is 0 otherwise", mh::lockstep::detail::no_players(w.st()) == 0);
}

// ================================ batch D: the frame timekeeper ===================================
//
// llm_strat_time_tick's ONLY evidence, and by construction rather than by omission: its first
// statement reads the wall clock, which no shadow arm can restore, so a shadow site there would
// diverge on every call however faithful the body is (turn_engine.h documents this as the fourth way
// to be un-shadowable). `detail::time_tick` takes `now` as a parameter precisely so this file can be
// the oracle instead.
//
// The tests below are organised by the ten translation traps listed at the top of timekeeper.cpp --
// each of those has at least one test whose failure names it.

using mh::lockstep::timekeeper_calls;
using mh::lockstep::timekeeper_state;

struct tk_log {
    int dismisses = 0, commits = 0, resyncs = 0, wait_overlays = 0;
    // U20: the `icon_calls` edge. Counted separately from wait_overlays (the `icon_shown` edge)
    // because the whole point of the extra slot is that the two DIVERGE once the gate is on.
    int                  overlay_wanted = 0;
    std::vector<double>  extends;
    std::vector<int32_t> acks, removed_timeout, removed, broadcast_left, alerts, leader_asked;
    int                  overlay_shows = 0;

    // rigged answers -- the original BRANCHES on all four
    int32_t match_side = 100; // find_horizon_match_side
    // The kick modal's answer. Since LIFT-SCREEN it is not a stub return -- tick() PUSHES it
    // through libmh_submit_screen_answer, the same call a real host makes, and the body reads
    // libmh's own slot. Tests set it exactly as before.
    int32_t overlay_reply = -1;
    int32_t leader_answer = 0; // is_local_leader_peer
    int32_t active_answer = 2; // count_active_players
    void    reset() { *this = tk_log{}; }
};
tk_log g_tk;

const timekeeper_calls &recording_tk_calls() {
    static const timekeeper_calls tc = {
        []() -> double { return 0.0; }, // get_current_time: wrapper-only, never reached from the body
        // Returns int32_t since SIM-READY 2026-08-07 corrected llm_net_lockstep_overlay_dismiss's
        // prototype (it always leaves 1 in EAX). The recorder still only counts calls.
        []() -> int32_t {
            ++g_tk.dismisses;
            return 1;
        },
        [](double h) { g_tk.extends.push_back(h); },
        []() { ++g_tk.commits; },
        []() -> int32_t { return g_tk.match_side; },
        side_to_index, // player_by_side_id, MODELLED not rigged: side 100+i -> i, so a handler that
                       // passes the wrong id is visibly wrong rather than silently right
        [](int32_t s) { g_tk.removed_timeout.push_back(s); },
        [](int32_t s) { g_tk.removed.push_back(s); },
        []() { ++g_tk.resyncs; },
        []() { ++g_tk.overlay_wanted; }, // U20 note_overlay_wanted -- fires before the gate
        [](int32_t p) -> int32_t { ++g_tk.wait_overlays; return p; },
        [](int32_t s) { g_tk.acks.push_back(s); },
        []() { ++g_tk.overlay_shows; },
        [](int32_t s) -> int32_t { g_tk.leader_asked.push_back(s); return g_tk.leader_answer; },
        []() -> int32_t { return g_tk.active_answer; },
        [](int32_t s) { g_tk.broadcast_left.push_back(s); },
        [](int32_t p) { g_tk.alerts.push_back(p); },
    };
    return tc;
}

// Every global time_tick touches, as plain storage. Defaults are a live mode-3 match that is NOT
// parked, so each test perturbs exactly the one thing it is about.
struct tk_world {
    double  last = 100.0, current = 0.0, speed = 1.0, delta = 0.0, total = 10.0;
    double  committed = 1000.0, horizon = 0.0, clock = 50.0, sim_step = 0.02;
    int32_t mode = mh::lockstep::SESSION_MP_LOCKSTEP;

    int32_t wait_active = 0, retry = 60, nag = 0, ls_count = 4, lobby_count = 4, local_idx = 999;
    // RESYNC_TRIGGER_COUNT. The ORIGINAL time_tick never touches it -- only C3's migrated
    // resync_trigger_reset does, in the recovery branch. Seeded non-zero so "was it zeroed?" is a real
    // question rather than one a zero-initialised field would answer by accident.
    int32_t  trigger_count = 77;
    double   wait_elapsed = 1.0, timeout_elapsed = -1.0, timeout_secs = 5.0;
    uint8_t  flags             = 0;
    uint8_t  peer_state[N * N] = {0};
    uint16_t side              = 0;

    double   step = 0.1, adapt_next = 1.0e30, step_max = 3.0, grow = 1.1, shrink = 1.1;
    double   floor_cmp = 0.5, floor_set = 0.5, adapt_interval = 60.0, fps_num = 20.0, fps = 49.0;
    int32_t  stalls                                    = 0;
    uint32_t active                                    = 2;
    double   ring[mh::lockstep::FRAME_TIME_RING_SLOTS] = {0};
    int32_t  ring_idx                                  = 0;

    timekeeper_state s() {
        timekeeper_state st{};
        st.last_game_time        = &last;
        st.current_game_time     = &current;
        st.game_speed            = &speed;
        st.game_time_delta       = &delta;
        st.total_game_time       = &total;
        st.committed             = &committed;
        st.horizon               = &horizon;
        st.game_clock            = &clock;
        st.sim_step_interval     = &sim_step;
        st.session_mode          = &mode;
        st.sync_wait_active      = &wait_active;
        st.sync_retry_countdown  = &retry;
        st.resync_trigger_count  = &trigger_count;
        st.sync_wait_elapsed     = &wait_elapsed;
        st.peer_timeout_elapsed  = &timeout_elapsed;
        st.peer_timeout_secs     = &timeout_secs;
        st.status_flags          = &flags;
        st.lockstep_player_count = &ls_count;
        st.lobby_scan_host_count = &lobby_count;
        st.local_player_index    = &local_idx;
        st.stall_nag_count       = &nag;
        st.peer_state            = peer_state;
        st.player_side           = &side;
        st.lockstep_step_size    = &step;
        st.adapt_next_time       = &adapt_next;
        st.stall_count           = &stalls;
        st.active_player_count   = &active;
        st.step_max              = &step_max;
        st.step_grow_mul         = &grow;
        st.step_shrink_div       = &shrink;
        st.step_min_fps_num_cmp  = &floor_cmp;
        st.step_min_fps_num_set  = &floor_set;
        st.adapt_interval_secs   = &adapt_interval;
        st.fps_estimate          = &fps;
        st.frame_time_ring       = ring;
        st.frame_time_ring_idx   = &ring_idx;
        st.fps_window_num        = &fps_num;
        return st;
    }
    // All-off fixes by DEFAULT, so every pre-existing timekeeper test keeps asserting what the
    // ORIGINAL does and only the C3 tests pass a fix explicitly.
    int32_t tick(double now, const mh::lockstep::reimpl_fixes &fx = mh::lockstep::reimpl_fixes{}) {
        libmh_submit_screen_answer(LIBMH_SCR_ANS_LOCKSTEP_KICK, g_tk.overlay_reply);
        timekeeper_state st = s();
        return mh::lockstep::detail::time_tick(st, recording_tk_calls(), fx, now);
    }
};

bool near_eq(double a, double b) { return a > b ? a - b < 1e-9 : b - a < 1e-9; }

// ---- trap (1): which clock global carries across frames, and which is the ring's copy ----
void test_tk_clock_advance() {
    g_tk.reset();
    tk_world w;
    w.mode  = 1; // single player -- isolate the clock arithmetic from the whole mode-3 block
    w.last  = 100.0;
    w.speed = 2.0;
    w.total = 10.0;
    w.tick(100.5);
    check("D: LAST_GAME_TIME is the cross-frame stamp and takes `now`", near_eq(w.last, 100.5));
    check("D: CURRENT_GAME_TIME is the copy made after it", near_eq(w.current, 100.5));
    check("D: delta = (now - previous) * game_speed", near_eq(w.delta, 1.0));
    check("D: total accumulates the delta", near_eq(w.total, 11.0));
}

void test_tk_singleplayer_skips_lockstep() {
    g_tk.reset();
    tk_world w;
    w.mode                   = 1;
    w.total                  = 5000.0;
    w.committed              = 10.0; // massively "over", but not mode 3
    w.adapt_next             = 0.0;  // would retune if the block ran
    const double step_before = w.step;
    w.tick(101.0);
    check("D: mode != 3 does not clamp TOTAL to the horizon", w.total > 4000.0);
    check("D: mode != 3 does not retune the step size", near_eq(w.step, step_before));
    check("D: mode != 3 still runs the FPS ring", near_eq(w.ring[0], 101.0));
}

// ---- traps (2)(3)(4)(6): the clamp, and the two stand-down paths that differ ----
void test_tk_clamp_and_standdown() {
    g_tk.reset();
    { // NOT parked (over < 0): stand down AND disarm the peer timeout
        tk_world w;
        w.total           = 10.0;
        w.committed       = 1000.0;
        w.wait_active     = 1;
        w.retry           = 3;
        w.wait_elapsed    = 40.0;
        w.timeout_elapsed = 2.5;
        w.tick(100.1);
        check("D: not-parked clears SYNC_WAIT_ACTIVE", w.wait_active == 0);
        check("D: not-parked reloads the retry countdown", w.retry == 60);
        check("D: not-parked reloads the elapsed timer to 1.0", near_eq(w.wait_elapsed, 1.0));
        check("D: not-parked DISARMS the peer timeout to -1.0", near_eq(w.timeout_elapsed, -1.0));
        check("D: not-parked dismisses the overlay", g_tk.dismisses == 1);
    }
    { // PARKED but with a step's worth of delta left: stand down WITHOUT disarming (trap 4)
        g_tk.reset();
        tk_world w;
        w.last            = 100.0;
        w.speed           = 1.0;
        w.total           = 100.0;
        w.committed       = 100.05;
        w.sim_step        = 0.02;
        w.wait_active     = 1;
        w.timeout_elapsed = 2.5;
        w.tick(100.1); // delta 0.1 -> total 100.1, over = 0.05 -> delta 0.05 >= sim_step
        check("D: the clamp takes the overshoot off TOTAL", near_eq(w.total, 100.05));
        check("D: the clamp takes the SAME overshoot off DELTA", near_eq(w.delta, 0.05));
        check("D: parked-but-unstalled clears SYNC_WAIT_ACTIVE", w.wait_active == 0);
        // It ACCUMULATED rather than resetting -- and by the PRE-clamp delta (0.1), because the
        // accumulate at 0x0043ef58 happens before the clamp at 0x0043f012. Writing 2.55 here (the
        // post-clamp 0.05) is the natural mistake and this assertion is what caught it.
        check("D: parked-but-unstalled does NOT disarm the peer timeout",
              near_eq(w.timeout_elapsed, 2.6));
    }
}

// ---- the peer-timeout drop, and its three gates ----
void test_tk_peer_timeout() {
    { // disarmed: -1.0 must not accumulate
        g_tk.reset();
        tk_world w;
        w.last            = 100.0;
        w.total           = 100.0;
        w.committed       = 100.0;
        w.timeout_elapsed = -1.0;
        w.tick(100.1);
        check("D: a disarmed peer timeout (-1.0) does not accumulate",
              near_eq(w.timeout_elapsed, -1.0));
    }
    { // armed and past the deadline, with both gates open -> the drop
        g_tk.reset();
        tk_world w;
        w.last                  = 100.0;
        w.total                 = 100.0;
        w.committed             = 100.0;
        w.timeout_elapsed       = 4.95;
        w.timeout_secs          = 5.0;
        w.flags                 = mh::lockstep::LS_HORIZON_PENDING;
        g_tk.match_side         = 103; // -> player index 3
        w.peer_state[3 * N + 0] = 0;   // row 3, column PlayerSide 0 == PEER_STATE_NONE
        w.tick(100.1);
        check("D: the timeout drop removes the side the horizon match named",
              g_tk.removed_timeout.size() == 1 && g_tk.removed_timeout[0] == 103);
        check("D: the timeout drop alerts with the PLAYER INDEX, not the side id",
              g_tk.alerts.size() == 1 && g_tk.alerts[0] == 3);
        check("D: the timeout drop clears LS_HORIZON_PENDING", (w.flags & 0x80) == 0);
        check("D: the timeout drop decrements BOTH counters", w.ls_count == 3 && w.lobby_count == 3);
        check("D: the timeout drop resyncs the clock", g_tk.resyncs == 1);
    }
    { // the flag gate closed -> nothing happens even though the deadline passed
        g_tk.reset();
        tk_world w;
        w.last            = 100.0;
        w.total           = 100.0;
        w.committed       = 100.0;
        w.timeout_elapsed = 4.95;
        w.flags           = 0;
        g_tk.match_side   = 103;
        w.tick(100.1);
        check("D: no LS_HORIZON_PENDING -> no timeout drop", g_tk.removed_timeout.empty());
    }
    { // peer_state not NONE -> vetoed
        g_tk.reset();
        tk_world w;
        w.last                  = 100.0;
        w.total                 = 100.0;
        w.committed             = 100.0;
        w.timeout_elapsed       = 4.95;
        w.flags                 = mh::lockstep::LS_HORIZON_PENDING;
        g_tk.match_side         = 103;
        w.peer_state[3 * N + 0] = 6;
        w.tick(100.1);
        check("D: a non-NONE peer_state vetoes the timeout drop", g_tk.removed_timeout.empty());
    }
    { // exactly AT the deadline -> not yet (the compare is a strict, ordered >)
        g_tk.reset();
        tk_world w;
        w.last            = 100.0;
        w.speed           = 1.0;
        w.total           = 100.0;
        w.committed       = 100.0;
        w.timeout_elapsed = 4.9;
        w.timeout_secs    = 5.0;
        w.flags           = mh::lockstep::LS_HORIZON_PENDING;
        w.tick(100.1); // -> exactly 5.0
        check("D: the peer timeout fires on a STRICT >, not >=", g_tk.removed_timeout.empty());
    }
}

// ---- the parked/stalled machine: arming, the retry pacer, the overlay, the reset ----
void test_tk_parked_machine() {
    { // first entry: arm, advertise a fresh horizon
        g_tk.reset();
        tk_world w;
        w.last        = 100.0;
        w.total       = 100.0;
        w.committed   = 100.0;
        w.wait_active = 0;
        w.clock       = 50.0;
        w.step        = 0.25;
        w.retry       = 0; // retry 0 -> skip the pacer entirely
        w.tick(100.001);   // delta ~0.001 < sim_step 0.02 -> STALLED
        check("D: the stall arms the sync wait", w.wait_active == 1);
        check("D: arming advertises clock + step", near_eq(w.horizon, 50.25));
        check("D: arming sends exactly one extend", g_tk.extends.size() == 1);
        check("D: arming commits the horizon", g_tk.commits == 1);
        check("D: arming shows no wait overlay", g_tk.wait_overlays == 0);
    }
    { // already armed: commit, then show the wait overlay for the matched peer. No extend.
        g_tk.reset();
        tk_world w;
        w.last          = 100.0;
        w.total         = 100.0;
        w.committed     = 100.0;
        w.wait_active   = 1;
        w.retry         = 0;
        g_tk.match_side = 102;
        w.tick(100.001);
        check("D: an armed stall commits without advertising", g_tk.commits == 1 && g_tk.extends.empty());
        check("D: an armed stall shows the wait overlay", g_tk.wait_overlays == 1);
    }

    // ---- U20: the de-sync icon counter edge, and desync_icon_gate in BOTH states ------------------
    //
    // The whole item rests on icon_calls and icon_shown staying TWO numbers, so that is what these
    // assert -- not just "the gate suppresses". A design that counted inside the show-wrapper alone
    // would pass every "suppressed" check here and silently collapse the two counters, which is the
    // measurement U20 exists to produce.
    //
    // The two arms are distinguished ONLY by SYNC_RETRY_COUNTDOWN: 60 (= SYNC_RETRY_RELOAD, nothing
    // has drained it) is the routine at-horizon wait the icon should not announce; 0 is genuine
    // multi-second silence, which it must still announce. Both use the armed-stall path above.
    {
        auto armed_stall = [](tk_world &w) {
            w.last        = 100.0;
            w.total       = 100.0;
            w.committed   = 100.0;
            w.wait_active = 1;
        };
        // OFF = stock: the icon is wanted AND drawn on a routine at-horizon frame. This is the
        // storm the item measured -- 62-85 icon-frames per 1k on a healthy link -- and asserting
        // it is what makes the ON arm below a difference rather than a coincidence.
        {
            g_tk.reset();
            tk_world w;
            armed_stall(w);
            w.retry = mh::lockstep::SYNC_RETRY_RELOAD; // routine wait: nothing has drained it
            w.tick(100.001);
            check("U20 gate OFF: the body wants an icon", g_tk.overlay_wanted == 1);
            check("U20 gate OFF: and draws it -- stock behaviour", g_tk.wait_overlays == 1);
        }
        // ON: wanted but NOT drawn. `icon_calls > icon_shown` is done_when (a)'s acceptance, and it
        // is only expressible because note_overlay_wanted fires before the gate.
        {
            g_tk.reset();
            tk_world                   w;
            mh::lockstep::reimpl_fixes fx;
            fx.desync_icon_gate = true;
            armed_stall(w);
            w.retry = mh::lockstep::SYNC_RETRY_RELOAD;
            w.tick(100.001, fx);
            check("U20 gate ON: the body still WANTS an icon (icon_calls counts the frame)",
                  g_tk.overlay_wanted == 1);
            check("U20 gate ON: and the routine at-horizon icon is suppressed", g_tk.wait_overlays == 0);
            check("U20 gate ON: so the two counters DIVERGE -- icon_calls > icon_shown",
                  g_tk.overlay_wanted > g_tk.wait_overlays);
        }
        // ON, but genuine sustained silence: the gate must NOT hide a real stall. This is the
        // direction that matters -- a gate that suppressed everything would pass the arm above.
        {
            g_tk.reset();
            tk_world                   w;
            mh::lockstep::reimpl_fixes fx;
            fx.desync_icon_gate = true;
            armed_stall(w);
            w.retry = 0; // drained: multi-second silence, below SYNC_OVERLAY_AFTER
            w.tick(100.001, fx);
            check("U20 gate ON: a genuinely silent peer STILL shows the icon",
                  g_tk.wait_overlays == 1 && g_tk.overlay_wanted == 1);
        }
        // The boundary, because `<` versus `<=` here is a one-frame difference nothing else would
        // catch: SYNC_OVERLAY_AFTER itself is still "routine", one below it is not.
        {
            g_tk.reset();
            tk_world                   w;
            mh::lockstep::reimpl_fixes fx;
            fx.desync_icon_gate = true;
            armed_stall(w);
            w.retry = mh::lockstep::SYNC_OVERLAY_AFTER; // == the threshold -> NOT below it
            w.tick(100.001, fx);
            check("U20 gate ON: countdown == SYNC_OVERLAY_AFTER is still suppressed",
                  g_tk.wait_overlays == 0);

            g_tk.reset();
            tk_world w2;
            armed_stall(w2);
            w2.retry = mh::lockstep::SYNC_OVERLAY_AFTER - 1; // one below -> shown
            w2.tick(100.001, fx);
            check("U20 gate ON: one below the threshold IS shown", g_tk.wait_overlays == 1);
        }
        // done_when (e), argued in the unit test rather than only in prose: the gate is
        // DISPLAY-ONLY. The body discards wait_player_overlay_show's return, so every other
        // observable of the frame must be identical with the gate on and off. Compare the whole
        // recorded frame, not a chosen field.
        {
            tk_world off_w;
            armed_stall(off_w);
            off_w.retry = mh::lockstep::SYNC_RETRY_RELOAD;
            g_tk.reset();
            const int32_t off_ret     = off_w.tick(100.001);
            const int     off_commits = g_tk.commits, off_extends = (int)g_tk.extends.size();
            const int     off_acks = (int)g_tk.acks.size(), off_shows = g_tk.overlay_shows;
            const int     off_resyncs = g_tk.resyncs, off_removed = (int)g_tk.removed.size();

            mh::lockstep::reimpl_fixes fx;
            fx.desync_icon_gate = true;
            tk_world on_w;
            armed_stall(on_w);
            on_w.retry = mh::lockstep::SYNC_RETRY_RELOAD;
            g_tk.reset();
            const int32_t on_ret = on_w.tick(100.001, fx);

            check("U20 display-only: same return value", off_ret == on_ret);
            check("U20 display-only: same calls out (commit/extend/ack/sync-overlay/resync/remove)",
                  g_tk.commits == off_commits && (int)g_tk.extends.size() == off_extends &&
                      (int)g_tk.acks.size() == off_acks && g_tk.overlay_shows == off_shows &&
                      g_tk.resyncs == off_resyncs && (int)g_tk.removed.size() == off_removed);
            check("U20 display-only: same state left behind",
                  on_w.wait_active == off_w.wait_active && on_w.retry == off_w.retry &&
                      near_eq(on_w.horizon, off_w.horizon) && near_eq(on_w.total, off_w.total) &&
                      near_eq(on_w.delta, off_w.delta) &&
                      near_eq(on_w.wait_elapsed, off_w.wait_elapsed) &&
                      on_w.trigger_count == off_w.trigger_count);
        }
    }
    { // trap (7): the pacer. elapsed - (60 - countdown) must exceed 1.0.
        g_tk.reset();
        tk_world w;
        w.last         = 100.0;
        w.total        = 100.0;
        w.committed    = 100.0;
        w.wait_active  = 1;
        w.retry        = 58;
        w.wait_elapsed = 2.5; // acks_sent = 2 -> 0.5, not > 1.0
        w.tick(100.001);
        check("D: the retry pacer does not fire below one elapsed second", g_tk.acks.empty());
        check("D: a non-firing pacer leaves the countdown alone", w.retry == 58);

        g_tk.reset();
        tk_world v;
        v.last          = 100.0;
        v.total         = 100.0;
        v.committed     = 100.0;
        v.wait_active   = 1;
        v.retry         = 58;
        v.wait_elapsed  = 3.5; // acks_sent = 2 -> 1.5 > 1.0
        g_tk.match_side = 101;
        v.tick(100.001);
        check("D: the retry pacer fires past one elapsed second",
              g_tk.acks.size() == 1 && g_tk.acks[0] == 101);
        check("D: a firing pacer decrements the countdown", v.retry == 57);
    }

    // ---- C3: the four fixes migrated out of byte patches inside llm_strat_time_tick. Each is tested
    // in BOTH states, because "off" is a claim too -- it asserts our body still reproduces the
    // ORIGINAL, which is the entire basis of the asymmetric oracle.
    {
        // The RECOVERY branch: parked -> not parked with the sync-wait armed. This is the `over < 0`
        // path, the one the 0x0043f2a5 splice sat in, identifiable because it alone disarms the peer
        // timeout.
        auto recover = [](tk_world &w) {
            w.last            = 100.0;
            w.total           = 100.0;
            w.committed       = 200.0; // room before the barrier -> over < 0 -> recovery
            w.wait_active     = 1;
            w.timeout_elapsed = 4.0; // armed, so the disarm below is observable
        };

        { // resync_trigger_reset OFF = stock: the original never touches the counter here.
            g_tk.reset();
            tk_world w;
            recover(w);
            w.tick(100.001);
            check("C3 reset OFF: the trigger count is untouched (stock)", w.trigger_count == 77);
            check("C3 reset OFF: recovery still happened", w.wait_active == 0 && w.timeout_elapsed < 0);
        }
        { // ON: zeroed, so the counter measures CONSECUTIVE stalls rather than cumulative ones.
            g_tk.reset();
            tk_world                   w;
            mh::lockstep::reimpl_fixes fx;
            fx.resync_trigger_reset = true;
            recover(w);
            w.tick(100.001, fx);
            check("C3 reset ON: the trigger count is zeroed on recovery", w.trigger_count == 0);
            check("C3 reset ON: and recovery is otherwise unchanged",
                  w.wait_active == 0 && w.timeout_elapsed < 0);
        }
        // The OTHER stand_down site -- "un-stalled while parked" (timekeeper.cpp's `else if
        // (*s.sync_wait_active == 1)` inside the parked branch). The byte splice was at ONE address,
        // so the reset must NOT fire here; applying it at both sites would change behaviour the patch
        // never touched.
        //
        // HOW THE PATH IS REACHED, since a previous attempt did not reach it and the check would then
        // have passed vacuously: GAME_TIME_DELTA is RECOMPUTED at the top of time_tick, so seeding it
        // is useless -- it has to be produced. delta = (now - last) * speed = 1.0; total becomes 1.0
        // and committed is 0.5, so over = +0.5 and we take the PARKED branch. The clamp then takes
        // the overshoot off delta, leaving 0.5, and the stall predicate is `delta < sim_step_interval`
        // -- with sim_step 0.02 we are NOT stalled, which is the only way to reach the else-if.
        //
        // AND HOW WE KNOW IT IS THE OTHER SITE and not the recovery one: note (4). The recovery exit
        // disarms the peer timeout (`peer_timeout_elapsed = -1.0`); this one does not. Arming it at
        // 0.0 makes that the discriminator -- it must come out POSITIVE.
        auto unstall_while_parked = [](tk_world &w) {
            w.last            = 100.0;
            w.speed           = 1.0;
            w.total           = 0.0;
            w.committed       = 0.5;  // over = +0.5 -> parked
            w.sim_step        = 0.02; // 0.5 >= 0.02 -> NOT stalled -> the else-if
            w.wait_active     = 1;
            w.retry           = 30;  // != SYNC_RETRY_RELOAD, so stand_down's reload is observable
            w.timeout_elapsed = 0.0; // armed; this path must leave it armed
        };
        { // sanity: the path really is the un-stalled-while-parked exit, in the STOCK build
            g_tk.reset();
            tk_world w;
            unstall_while_parked(w);
            w.tick(101.0);
            check("C3 other-site: stand_down ran (the un-stalled-while-parked exit)",
                  w.wait_active == 0 && w.retry == mh::lockstep::SYNC_RETRY_RELOAD);
            check("C3 other-site: and it is NOT the recovery exit -- the peer timeout stays armed",
                  w.timeout_elapsed > 0.0);
            check("C3 other-site: the clamp took the overshoot off the delta", near_eq(w.delta, 0.5));
            check("C3 other-site: stock leaves the trigger count alone", w.trigger_count == 77);
        }
        { // ON: the reset is scoped to the recovery exit, so this site must STILL not zero it
            g_tk.reset();
            tk_world                   w;
            mh::lockstep::reimpl_fixes fx;
            fx.resync_trigger_reset = true;
            unstall_while_parked(w);
            w.tick(101.0, fx);
            check("C3 other-site: reached the same exit with the fix ON",
                  w.wait_active == 0 && w.timeout_elapsed > 0.0);
            check("C3 reset ON: does NOT fire at the other stand_down site", w.trigger_count == 77);
        }
        // ... nor at the THIRD episode-exit: NOTE (8)'s inline reload. Same three stores as
        // stand_down, written out rather than called, so it is easy to treat as "another stand_down"
        // and reset there too -- which is again broader than the byte patch. Setup is the "a
        // non-negative overlay reply forces the reset" case below; `retry == 60` is what proves the
        // reload really ran, so the trigger-count assertion is not vacuous.
        {
            g_tk.reset();
            tk_world                   w;
            mh::lockstep::reimpl_fixes fx;
            fx.resync_trigger_reset = true;
            w.last                  = 100.0;
            w.total                 = 100.0;
            w.committed             = 100.0;
            w.wait_active           = 1;
            w.retry                 = 56;
            w.wait_elapsed          = 6.0;
            w.local_idx             = 999;
            g_tk.overlay_reply      = 0;
            g_tk.match_side         = 102;
            g_tk.leader_answer      = 0;
            w.tick(100.001, fx);
            check("C3 third-exit: NOTE (8)'s reload really ran", w.retry == 60);
            check("C3 reset ON: does NOT fire at NOTE (8)'s reload either", w.trigger_count == 77);
        }
        { // C8-e: `defang_dismiss` is GONE, so the two-flag-state case above it went too. What is
            // kept is the STOCK half -- the one assertion that still describes shipped behaviour --
            // because the deletion's risk is not "the suppression stopped working", it is "the
            // dismiss stopped happening at all", and nothing else in this file watches for that.
            g_tk.reset();
            tk_world w;
            recover(w);
            w.tick(100.001);
            check("C8-e: overlay_dismiss is called on recovery, unconditionally (defang_dismiss gone)",
                  g_tk.dismisses == 1);
        }
    }
    { // re-advertise only on a strict increase
        g_tk.reset();
        tk_world w;
        w.last         = 100.0;
        w.total        = 100.0;
        w.committed    = 100.0;
        w.wait_active  = 1;
        w.retry        = 58;
        w.wait_elapsed = 3.5;
        w.clock        = 50.0;
        w.step         = 0.25;
        w.horizon      = 50.25; // clock+step == horizon -> JBE -> no re-advertise
        w.tick(100.001);
        check("D: the retry does not re-advertise an unchanged horizon", g_tk.extends.empty());

        g_tk.reset();
        tk_world v;
        v.last         = 100.0;
        v.total        = 100.0;
        v.committed    = 100.0;
        v.wait_active  = 1;
        v.retry        = 58;
        v.wait_elapsed = 3.5;
        v.clock        = 50.0;
        v.step         = 0.25;
        v.horizon      = 50.0;
        v.tick(100.001);
        check("D: the retry re-advertises a strictly higher horizon",
              g_tk.extends.size() == 1 && near_eq(g_tk.extends[0], 50.25));
    }
    { // the overlay gate: only once the countdown has dropped below 0x38
        g_tk.reset();
        tk_world w;
        w.last         = 100.0;
        w.total        = 100.0;
        w.committed    = 100.0;
        w.wait_active  = 1;
        w.retry        = 57;
        w.wait_elapsed = 5.0; // -> 56 == 0x38, NOT below it
        w.tick(100.001);
        check("D: no sync overlay while the countdown is still 0x38", g_tk.overlay_shows == 0);

        g_tk.reset();
        tk_world v;
        v.last         = 100.0;
        v.total        = 100.0;
        v.committed    = 100.0;
        v.wait_active  = 1;
        v.retry        = 56;
        v.wait_elapsed = 6.0; // -> 55 < 0x38
        v.tick(100.001);
        check("D: the sync overlay shows once the countdown drops below 0x38",
              g_tk.overlay_shows == 1);
    }
}

// ---- trap (8): the reset condition, and what it does about the stalled peer ----
void test_tk_retry_reset() {
    // Countdown hits 0 -> reset fires, and the matched side is US -> the self-match branch, which
    // does nothing at all now that the original's empty hook call is not reproduced.
    {
        g_tk.reset();
        tk_world w;
        w.last             = 100.0;
        w.total            = 100.0;
        w.committed        = 100.0;
        w.wait_active      = 1;
        w.retry            = 1;
        w.wait_elapsed     = 61.0;
        w.clock            = 50.0;
        w.step             = 0.25;
        w.local_idx        = 101;
        g_tk.match_side    = 101;
        g_tk.overlay_reply = -1;
        w.tick(100.001);
        check("D: the reset reloads the countdown to 60", w.retry == 60);
        check("D: the reset reloads the elapsed timer to 1.0", near_eq(w.wait_elapsed, 1.0));
        check("D: the reset re-advertises", near_eq(w.horizon, 50.25));
        check("D: a self-match removes nobody", g_tk.removed.empty() && g_tk.broadcast_left.empty());
        check("D: the reset dismisses the overlay", g_tk.dismisses == 1);
    }
    { // countdown still positive AND the overlay answered < 0 -> NO reset
        g_tk.reset();
        tk_world w;
        w.last             = 100.0;
        w.total            = 100.0;
        w.committed        = 100.0;
        w.wait_active      = 1;
        w.retry            = 56;
        w.wait_elapsed     = 6.0;
        w.local_idx        = 999;
        g_tk.overlay_reply = -1;
        w.tick(100.001);
        check("D: a negative overlay reply with countdown left does NOT reset", w.retry == 55);
        check("D: ... and dismisses nothing", g_tk.dismisses == 0);
    }
    { // countdown still positive but the overlay answered >= 0 -> reset anyway
        g_tk.reset();
        tk_world w;
        w.last             = 100.0;
        w.total            = 100.0;
        w.committed        = 100.0;
        w.wait_active      = 1;
        w.retry            = 56;
        w.wait_elapsed     = 6.0;
        w.local_idx        = 999;
        g_tk.overlay_reply = 0;
        g_tk.match_side    = 102;
        g_tk.leader_answer = 0;
        w.tick(100.001);
        check("D: a non-negative overlay reply forces the reset", w.retry == 60);
    }
    { // leader + >2 active -> broadcast the departure and latch the flag
        g_tk.reset();
        tk_world w;
        w.last             = 100.0;
        w.total            = 100.0;
        w.committed        = 100.0;
        w.wait_active      = 1;
        w.retry            = 1;
        w.wait_elapsed     = 61.0;
        w.local_idx        = 999;
        w.flags            = 0;
        g_tk.match_side    = 102;
        g_tk.leader_answer = 1;
        g_tk.active_answer = 3;
        w.tick(100.001);
        check("D: leader with >2 active broadcasts the leave",
              g_tk.broadcast_left.size() == 1 && g_tk.broadcast_left[0] == 102);
        check("D: ... and latches LS_HORIZON_PENDING", (w.flags & 0x80) != 0);
        check("D: ... and removes nobody locally", g_tk.removed.empty());
    }
    { // leader + exactly 2 active -> remove locally and alert
        g_tk.reset();
        tk_world w;
        w.last             = 100.0;
        w.total            = 100.0;
        w.committed        = 100.0;
        w.wait_active      = 1;
        w.retry            = 1;
        w.wait_elapsed     = 61.0;
        w.local_idx        = 999;
        w.flags            = 0;
        g_tk.match_side    = 102;
        g_tk.leader_answer = 1;
        g_tk.active_answer = 2;
        w.tick(100.001);
        check("D: leader with <=2 active removes locally",
              g_tk.removed.size() == 1 && g_tk.removed[0] == 102);
        check("D: ... and alerts with the PLAYER INDEX", g_tk.alerts.size() == 1 && g_tk.alerts[0] == 2);
        check("D: ... and does not broadcast", g_tk.broadcast_left.empty());
    }
    { // LS_HORIZON_PENDING already latched -> the leader branch is skipped entirely
        g_tk.reset();
        tk_world w;
        w.last             = 100.0;
        w.total            = 100.0;
        w.committed        = 100.0;
        w.wait_active      = 1;
        w.retry            = 1;
        w.wait_elapsed     = 61.0;
        w.local_idx        = 999;
        w.flags            = mh::lockstep::LS_HORIZON_PENDING;
        g_tk.match_side    = 102;
        g_tk.leader_answer = 1;
        g_tk.active_answer = 3;
        w.tick(100.001);
        check("D: an already-latched LS_HORIZON_PENDING suppresses the departure",
              g_tk.broadcast_left.empty() && g_tk.removed.empty());
    }
}

// ---- the nag counter decays only above its floor ----
void test_tk_stall_nag() {
    g_tk.reset();
    tk_world w;
    w.last      = 100.0;
    w.total     = 100.0;
    w.committed = 100.0;
    w.retry     = 0;
    w.nag       = 30;
    w.tick(100.001);
    check("D: the nag counter decays at exactly the floor", w.nag == 29);
    w.nag       = 29;
    w.last      = 100.001;
    w.total     = 100.0;
    w.committed = 100.0;
    w.tick(100.002);
    check("D: the nag counter does NOT decay below the floor", w.nag == 29);
}

// ---- trap (9): the game's own adaptive controller ----
void test_tk_adaptive() {
    { // stalls at the threshold -> grow, capped
        g_tk.reset();
        tk_world w;
        w.mode       = mh::lockstep::SESSION_MP_LOCKSTEP;
        w.total      = 10.0;
        w.committed  = 1000.0; // not parked, so only the retune runs
        w.adapt_next = 0.0;
        w.clock      = 50.0;
        w.active     = 2;
        w.stalls     = 10; // 2*5 == 10, so NOT (10 > 10) -> grow
        w.step       = 1.0;
        w.step_max   = 3.0;
        w.grow       = 1.1;
        w.floor_cmp = w.floor_set = 0.0;
        w.tick(100.1);
        check("D: stalls at exactly count*5 GROW the step", near_eq(w.step, 1.1));
        check("D: the retune zeroes the stall count", w.stalls == 0);
        check("D: the retune schedules the next one at clock + interval", near_eq(w.adapt_next, 110.0));
    }
    { // at the cap -> no growth
        g_tk.reset();
        tk_world w;
        w.total      = 10.0;
        w.committed  = 1000.0;
        w.adapt_next = 0.0;
        w.active     = 2;
        w.stalls     = 10;
        w.step       = 3.0;
        w.step_max   = 3.0;
        w.floor_cmp = w.floor_set = 0.0;
        w.tick(100.1);
        check("D: a step already at STEP_MAX does not grow", near_eq(w.step, 3.0));
    }
    { // no stalls -> shrink by DIVISION
        g_tk.reset();
        tk_world w;
        w.total      = 10.0;
        w.committed  = 1000.0;
        w.adapt_next = 0.0;
        w.active     = 2;
        w.stalls     = 0;
        w.step       = 1.1;
        w.shrink     = 1.1;
        w.floor_cmp = w.floor_set = 0.0;
        w.tick(100.1);
        check("D: zero stalls shrink the step by DIVISION", near_eq(w.step, 1.0));
    }
    { // between the two: some stalls but below the threshold -> neither grows nor shrinks
        g_tk.reset();
        tk_world w;
        w.total      = 10.0;
        w.committed  = 1000.0;
        w.adapt_next = 0.0;
        w.active     = 2;
        w.stalls     = 3;
        w.step       = 1.0;
        w.floor_cmp = w.floor_set = 0.0;
        w.tick(100.1);
        check("D: stalls between 1 and count*5-1 leave the step alone", near_eq(w.step, 1.0));
    }
    { // trap (9): the floor uses TWO distinct numerators, so a test must be able to tell them apart
        g_tk.reset();
        tk_world w;
        w.total      = 10.0;
        w.committed  = 1000.0;
        w.adapt_next = 0.0;
        w.active     = 2;
        w.stalls     = 3;
        w.step       = 0.001;
        w.fps        = 49.0;
        w.floor_cmp  = 5.0;  // 5/50 = 0.1 > 0.001 -> the floor applies
        w.floor_set  = 25.0; // 25/50 = 0.5 -> and it is the SET numerator that lands
        w.tick(100.1);
        check("D: the floor clamp uses the SET numerator, not the COMPARE one", near_eq(w.step, 0.5));
    }
    { // not yet due -> nothing at all
        g_tk.reset();
        tk_world w;
        w.total      = 10.0;
        w.committed  = 1000.0;
        w.adapt_next = 1.0e30;
        w.active     = 2;
        w.stalls     = 99;
        w.step       = 1.0;
        w.tick(100.1);
        check("D: the retune does not run before its scheduled time", near_eq(w.step, 1.0));
        check("D: ... and leaves the stall count intact", w.stalls == 99);
    }
}

// ---- trap (10): the FPS ring and the return value ----
void test_tk_fps_ring() {
    g_tk.reset();
    tk_world w;
    w.mode          = 1;
    w.ring_idx      = 0;
    w.ring[0]       = 99.0;
    w.fps_num       = 20.0;
    const int32_t r = w.tick(100.0);
    check("D: the FPS estimate is window / (now - ring[idx])", near_eq(w.fps, 20.0));
    check("D: the ring slot takes the new stamp", near_eq(w.ring[0], 100.0));
    check("D: the ring index advances", w.ring_idx == 1);
    check("D: a non-wrapping tick returns 0", r == 0);

    w.ring_idx       = 19;
    const int32_t r2 = w.tick(101.0);
    check("D: the ring index wraps to 0", w.ring_idx == 0);
    check("D: the 20th tick returns 1", r2 == 1);
}

// ---- trap (5): the unordered direction, site by site ----
void test_tk_nan_semantics() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    { // a NaN overshoot must take the PARKED branch (JA is not taken when unordered)
        g_tk.reset();
        tk_world w;
        w.last        = 100.0;
        w.total       = nan;
        w.committed   = 100.0;
        w.wait_active = 0;
        w.retry       = 0;
        w.tick(100.001);
        check("D/NaN: a NaN overshoot parks rather than standing down", w.wait_active == 1);
    }
    { // a NaN peer-timeout keeps counting rather than reading as disarmed
        g_tk.reset();
        tk_world w;
        w.last            = 100.0;
        w.total           = 100.0;
        w.committed       = 100.0;
        w.timeout_elapsed = nan;
        w.timeout_secs    = 5.0;
        w.flags           = mh::lockstep::LS_HORIZON_PENDING;
        w.retry           = 0;
        w.tick(100.1);
        check("D/NaN: a NaN peer timeout is not treated as disarmed, but never fires (JBE takes it)",
              g_tk.removed_timeout.empty());
    }
    { // a NaN delta counts as STALLED (JC is taken when unordered)
        g_tk.reset();
        tk_world w;
        w.last        = nan;
        w.total       = 100.0;
        w.committed   = 100.0;
        w.wait_active = 0;
        w.retry       = 0;
        w.tick(100.0);
        check("D/NaN: a NaN delta reads as stalled", w.wait_active == 1);
    }
    { // a NaN adapt_next_time RUNS the retune (JA is not taken when unordered)
        g_tk.reset();
        tk_world w;
        w.total      = 10.0;
        w.committed  = 1000.0;
        w.adapt_next = nan;
        w.active     = 2;
        w.stalls     = 0;
        w.step       = 1.1;
        w.shrink     = 1.1;
        w.floor_cmp = w.floor_set = 0.0;
        w.tick(100.1);
        check("D/NaN: a NaN adapt deadline runs the retune", near_eq(w.step, 1.0));
    }
    { // a NaN step size must NOT grow (JNC is not taken when unordered -> the grow is skipped...
        // ...no: JNC skips, so unordered CF=1 means it FALLS THROUGH and grows. Pin the real answer.)
        g_tk.reset();
        tk_world w;
        w.total      = 10.0;
        w.committed  = 1000.0;
        w.adapt_next = 0.0;
        w.active     = 2;
        w.stalls     = 10;
        w.step       = nan;
        w.step_max   = 3.0;
        w.grow       = 1.1;
        w.floor_cmp = w.floor_set = 0.0;
        w.tick(100.1);
        check("D/NaN: a NaN step is still multiplied by the grow factor (JNC not taken)",
              w.step != w.step); // NaN * 1.1 is NaN -- the point is it took the grow branch
    }
}

// ---- MP D14: the resync-begin order's exec_time clamp --------------------------------------------
//
// WHY THIS IS UNIT-TESTED RATHER THAN LEFT TO THE RIG, stated plainly because the rig WAS tried first
// (tmp/l1p/d14, d14sat, d14long -- 16 runs). The rig can prove the clamp is LIVE: with the fix on, the
// recorded exec_time stops being the hardcoded 2.0 and becomes a real horizon (10.188482447359874,
// replicated bit-identically to both peers, both scheduling on the same step). What the rig could NOT
// be made to do is reproduce the PRE-fix failure on demand. At ship pacing a match fires exactly one
// resync, at around step 100 -- where the game clock is ~2.0 s, so the stock 2.0 is not yet in the past
// and the bug simply cannot manifest. Longer runs (9000 steps) did not re-arm the trigger; an injected
// 100 ms link delay did not either. The pre-fix skew reproduced once in three saturated runs and not at
// all in nine ship-config runs.
//
// So the interesting inputs -- horizon far ahead, horizon behind, exactly equal, NaN -- are reachable
// ONLY here. A pure function of two doubles, tested directly, is better evidence about the LOGIC than
// any number of runs that never enter the regime where the logic matters.
void test_d14_resync_order_exec_time() {
    using mh::lockstep::detail::resync_order_exec_time;

    // The case the whole fix exists for: the stock 2.0 against a horizon the clock has long passed.
    check("D14: a past exec_time is raised to the horizon", resync_order_exec_time(2.0, 10.1884824473598) == 10.1884824473598);

    // The case that made two thirds of the ON runs uninformative, and it must stay a NO-OP: very early
    // in a match the horizon has not yet reached 2.0, so the stock value is already at/ahead of it.
    // Raising here would move the order EARLIER than the original scheduled it.
    check("D14: an exec_time already ahead of the horizon is left alone", resync_order_exec_time(2.0, 1.5) == 2.0);
    check("D14: equal exec_time and horizon is a no-op", resync_order_exec_time(2.0, 2.0) == 2.0);

    // The clamped value must BE the horizon, not something recomputed near it -- an off-by-a-step here
    // would put the two peers back on different sides of a step boundary.
    const double h = 7.25;
    check("D14: the clamped value IS the horizon, not a recomputation", resync_order_exec_time(0.0, h) == h);

    // Honest note about what these checks do NOT pin, established by mutating them (tmp/l1p/mutate_d14.py):
    // writing the comparison as `<=` instead of `<` is a BEHAVIOURALLY IDENTICAL program -- for equal
    // finite values both yield the same number, and for NaN both compares are false either way. Likewise
    // `horizon > exec_time ? ...` is just `<` with the operands swapped. Those two mutations are not
    // caught because there is nothing to catch. What DOES matter is argument ORDER around NaN, which the
    // next two checks pin: std::max(exec_time, horizon) is exactly this function, while
    // std::max(horizon, exec_time) would return the horizon for a NaN exec_time instead of the NaN.

    // Negative / zero exec_time: nothing special, but it is the shape a zeroed record would carry, and
    // "it silently stayed 0 and released on step 0 everywhere" is the failure this must not allow.
    check("D14: a zero exec_time is raised like any other past value", resync_order_exec_time(0.0, 3.0) == 3.0);

    // NaN. Written as `<` deliberately: an unordered compare is FALSE, so a NaN horizon leaves exec_time
    // untouched instead of poisoning the order the peers are about to agree on -- and a NaN exec_time
    // stays NaN rather than being quietly swapped for the horizon.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    check("D14: a NaN horizon leaves exec_time untouched (unordered compare is false)", resync_order_exec_time(2.0, nan) == 2.0);
    check("D14: a NaN exec_time is NOT silently replaced by the horizon", resync_order_exec_time(nan, 5.0) != resync_order_exec_time(nan, 5.0));

    // Ordering property, which is the thing that actually makes the peers agree: whatever comes out is
    // never BEFORE the horizon, for any finite input. That is the invariant release_due depends on.
    const double horizons[]   = {0.0, 1.5, 2.0, 10.1884824473598, 1e6};
    const double times[]      = {-1.0, 0.0, 2.0, 9.9, 1e7};
    bool         never_before = true;
    for (double hz : horizons)
        for (double t : times)
            if (resync_order_exec_time(t, hz) < hz) never_before = false;
    check("D14: the result is never before the horizon, over the whole grid", never_before);
}

// MP D24 -- the BARRIER the clamp above raises to. Split from test_d14_resync_order_exec_time on
// purpose: that function tests "does the raise work", this one tests "is it raising to a value that
// actually clears every peer", which is the question D17's version got wrong.
//
// The rig cannot reach any of this. D17 spent six 2-peer runs failing to fire a resync at all, and
// the reason was measured (SYNC_RETRY_COUNTDOWN never leaves its 0x3c reload at two peers, so the
// gated trigger count never moves). A pure function of three doubles is the only place the regime
// exists.
void test_d24_resync_order_barrier() {
    using mh::lockstep::detail::resync_order_barrier;
    using mh::lockstep::detail::resync_order_exec_time;

    // THE DEFECT, stated as an assertion. Every peer's clock can reach the horizon exactly (its
    // committed barrier is <= our advertised horizon and the sim runs right up to it), and
    // release_due fires on `!(exec_time > now)` -- equality included. So the value must be STRICTLY
    // greater than the horizon or the order is due the moment each peer arrives, which is the race
    // the leader's zero-latency local mirror always wins.
    // RULES OUT: the pre-D24 code, which returned the horizon itself.
    const double h = 10.1884824473598, step = 0.25;
    check("D24: the barrier is strictly past the horizon", resync_order_barrier(h, 9.5, step) > h);
    check("D24: and it is exactly one lockstep step past it", resync_order_barrier(h, 9.5, step) == h + step);

    // The floor. `game_clock` is in the max only for the case where the clock has outrun the
    // horizon; clamping to the horizon there would stamp the order in the PAST for everyone, which
    // is the original D14 defect with extra steps.
    check("D24: a clock ahead of the horizon wins the max", resync_order_barrier(3.0, 12.0, 0.5) == 12.5);
    check("D24: a clock behind the horizon does not", resync_order_barrier(12.0, 3.0, 0.5) == 12.5);
    check("D24: equal clock and horizon is unambiguous", resync_order_barrier(7.0, 7.0, 0.5) == 7.5);

    // A zero step size is the degenerate configuration, and it must NOT be papered over with a
    // fabricated epsilon: with no step there is no "next step" to put the order on, and the honest
    // answer is the horizon itself. Recorded so a later reader does not add a magic constant.
    check("D24: a zero step size degenerates to the bare horizon, no invented epsilon",
          resync_order_barrier(5.0, 1.0, 0.0) == 5.0);

    // NaN transparency, which the composition has to preserve or the clamp poisons the order the
    // peers are about to agree on. A NaN barrier makes `exec_time < barrier` false, so exec_time
    // survives untouched -- the same property test_d14_* pins for a NaN horizon, checked through
    // the composition because that is what production calls.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    check("D24: a NaN horizon leaves exec_time untouched through the composition",
          resync_order_exec_time(2.0, resync_order_barrier(nan, 1.0, 0.5)) == 2.0);
    check("D24: a NaN step size does too",
          resync_order_exec_time(2.0, resync_order_barrier(5.0, 1.0, nan)) == 2.0);

    // The invariant release_due depends on, over a grid: whatever comes out of the composition is
    // never at-or-before the horizon for any finite input where the clamp actually fired. The
    // "actually fired" qualifier is load-bearing -- an exec_time already past the barrier is left
    // alone by design and is not a counter-example.
    const double horizons[]         = {0.0, 1.5, 2.0, 10.1884824473598, 1e6};
    const double clocks[]           = {-1.0, 0.0, 1.9, 9.9, 1e7};
    const double steps[]            = {0.03, 0.25, 1.0};
    bool         never_at_or_before = true;
    for (double hz : horizons)
        for (double ck : clocks)
            for (double sp : steps) {
                const double out = resync_order_exec_time(2.0, resync_order_barrier(hz, ck, sp));
                if (out != 2.0 && out <= hz) never_at_or_before = false;
            }
    check("D24: a clamped result is never at-or-before the horizon, over the whole grid",
          never_at_or_before);
}

} // namespace

int run_lockstest() {
    printf("=== lockstest (turn-engine horizon/barrier logic, no game) ===\n");
    g_checks = g_fails = 0;
    test_commit_horizon();
    test_extend_if_near_horizon();
    test_find_horizon_match_side();
    test_record_peer_horizon();
    test_reset_player_horizon();
    test_nan_semantics();
    test_no_players();
    test_sim_tick_lockstep();
    test_sim_tick_singleplayer();
    test_sim_tick_rig_fixed_step_loop();
    test_sim_tick_tail();
    test_advance_sim_clock();
    test_pump();
    test_sim_clock_advance();
    test_dispatch_cursor_walk();
    test_dispatch_horizon_and_pending();
    test_dispatch_order();
    test_dispatch_keepalive();
    test_dispatch_control_basics();
    test_dispatch_departures();
    test_dispatch_drop_synced_vs_unsynced();
    test_u19h_leave_park_horizon_race(); // mp:U19h -- the graceful-leave horizon race
    test_dispatch_kick_and_resets();
    test_dispatch_leave_consensus();
    test_dispatch_step_size_and_resync();
    test_dispatch_chat_and_dead_tag();
    test_dispatch_sender_already_gone();
    // ---- batch D: the frame timekeeper (its ONLY evidence -- the wall clock is unrestorable) ----
    test_tk_clock_advance();
    test_tk_singleplayer_skips_lockstep();
    test_tk_clamp_and_standdown();
    test_tk_peer_timeout();
    test_tk_parked_machine();
    test_tk_retry_reset();
    test_tk_stall_nag();
    test_tk_adaptive();
    test_tk_fps_ring();
    test_tk_nan_semantics();
    // ---- MP D14: the resync-begin exec_time clamp ----
    test_d14_resync_order_exec_time();
    test_d24_resync_order_barrier();
    // ---- RI-WIRE W3: the emit -> parse round trip ----
    test_w3_emit_layout();
    test_w3_emit_round_trip();
    test_w3_emit_mutations();
    // ---- RI-WIRE: the five tx_emit.cpp emitters (layout, cursor guard, tails, round trip) ----
    test_tx5_extend_layout_and_guard();
    test_tx5_keepalive_layout_and_guard();
    test_tx5_resync_trigger_tick();
    test_w5_sent_gate_both_states();
    test_w5_sent_gate_audit_tally();
    test_tx5_keepalive_calls_resync_tail();
    test_tx5_broadcast_player_leave_layout_and_guard();
    test_tx5_broadcast_player_leave_tail();
    test_tx5_player_remove_layout_and_guard();
    test_tx5_player_remove_tail_status_flags_and_branch();
    test_tx5_round_trip();
    // ---- RI-WIRE W6-A: the chat pair + send_buf_flush (tx_emit_chat.cpp) ----
    test_tx_chat_team_layout_and_guard();
    test_tx_chat_all_layout_and_guard();
    test_tx_chat_length_double_duty();
    test_tx_chat_round_trip();
    test_send_buf_flush();
    // ---- RI-WIRE W6-B: the eight ctrl emitters (tx_emit_ctrl.cpp) ----
    test_tx_ctrl_presence_lost();
    test_tx_ctrl_slot_reset();
    test_tx_ctrl_horizon_ack();
    test_tx_ctrl_horizon_desync();
    test_tx_ctrl_kick();
    test_tx_ctrl_step_size();
    test_tx_ctrl_broadcast_resync_state();
    test_tx_ctrl_resync_resume();
    test_tx_ctrl_round_trip();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
