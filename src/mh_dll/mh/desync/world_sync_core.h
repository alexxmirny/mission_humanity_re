//
// desync/world_sync_core.h -- mp:X3c: the PURE decision core of the host-authoritative world resync.
//
// No I/O, no globals, no game memory, no clock: every rule is a function of its arguments, so
// `net_selftest.exe wstest` drives each gate and each transition with a positive AND a negative case.
// The product wiring (mh/desync/world_sync.cpp) only gathers the inputs and performs the actions
// these functions return.
//
// Log prefix for anything built on this is `; [worldsync]` -- NOT `[resync]`, which is the retail mode-8
// barrier's prefix that check_resync_storm.py greps.
//
//   1. WHO DIVERGED     pick_diverged()      -- host authority; 2 peers: the client; N>=3: outside the largest
//                                               hash group that contains the host, else host-minority.
//   2. MAY WE ACT       gate_check()         -- confirm steps, roster quiet, mode 3, per-peer state, resync_max,
//                                               libmh/promote/transport availability. First failure = SKIP line.
//   3. THE MACHINE      fsm_step()           -- IDLE -> BEGUN -> SENT -> COOLDOWN -> IDLE, plus the ABORT cases.
//   4. MINORITY SIDE    ring_covers(), import_check(), ff_* -- what the diverged peer may do with META + blob.
//
#pragma once
#include <cstdint>
#include <cstring>

#include "desync/desync_wire2.h"

namespace mh::desync::ws {

using namespace mh::desync::v2; // ws_abort_reason enumerators, WS_SRC_SLOTS, T_WS_*

inline constexpr int MAX_PEERS = 8; // player slots

// ---- configuration (the [desync] ini keys; defaults are the design's) -------------------------------
struct config {
    int action              = 0;      // [desync] action: 1 = resync. Default stays 0 (report-only).
    int confirm_steps       = 50;     // resync_confirm_steps: consecutive mismatching steps before acting (1 s)
    int roster_quiet_steps  = 250;    // resync_roster_quiet_steps: no roster change this long before the incident
    int resync_max          = 3;      // resyncs per match, per peer
    int timeout_ms          = 180000; // resync_timeout_ms: BEGIN -> DONE deadline
    int ff_steps            = 20;     // resync_ff_steps: catch-up cap per frame
    int cooldown_steps      = 500;    // COOLDOWN length
    int capture_retry_every = 25;     // steps between capture attempts (-8 hashed-slice-missing is early-match)
    int capture_retry_max   = 20;     // attempts before ABORT(CAPTURE)
};

// ---- 1. who diverged -------------------------------------------------------------------------------
// One hash per peer at the incident step. have_mask bit i = peer i's state hash at that step is known
// (the host's own is always known when the host runs this). Peers with no hash take no part.
enum verdict_kind : int {
    V_NO_DATA = 0,   // the host's own hash is missing, or fewer than two peers reported
    V_AGREE,         // every reporting peer holds the host's hash: nothing to do
    V_RESYNC,        // `resync_mask` names the peers to resync
    V_HOST_MINORITY, // the host's group is not a largest group: SKIP (row X3g)
};

struct diverge_verdict {
    verdict_kind kind;
    uint32_t     resync_mask; // V_RESYNC: bit i = peer i to resync
    int          groups;      // distinct hashes among the reporting peers
    int          host_group;  // size of the host's group
    int          best_group;  // size of the largest group
};

// The host's group is the reference unless another group is STRICTLY larger (ties go to the host: with two
// peers the client is always the minority by host authority). Everyone outside the host's group is resynced.
inline diverge_verdict pick_diverged(const uint64_t *state, uint32_t have_mask, int host, int n) {
    diverge_verdict v = {V_NO_DATA, 0, 0, 0, 0};
    if (n > MAX_PEERS) n = MAX_PEERS;
    if (host < 0 || host >= n || !((have_mask >> host) & 1u)) return v;
    int reporting = 0;
    for (int i = 0; i < n; ++i)
        if ((have_mask >> i) & 1u) ++reporting;
    if (reporting < 2) return v;
    for (int i = 0; i < n; ++i) {
        if (!((have_mask >> i) & 1u)) continue;
        int  size  = 0;
        bool first = true; // count each distinct hash once, at its first holder
        for (int j = 0; j < n; ++j) {
            if (!((have_mask >> j) & 1u) || state[j] != state[i]) continue;
            if (j < i) first = false;
            ++size;
        }
        if (first) ++v.groups;
        if (size > v.best_group) v.best_group = size;
        if (state[i] == state[host]) v.host_group = size;
    }
    if (v.groups <= 1) {
        v.kind = V_AGREE;
        return v;
    }
    if (v.best_group > v.host_group) {
        v.kind = V_HOST_MINORITY;
        return v;
    }
    v.kind = V_RESYNC;
    for (int i = 0; i < n; ++i)
        if (((have_mask >> i) & 1u) && state[i] != state[host]) v.resync_mask |= 1u << i;
    return v;
}

// ---- roster tracking (the D26 exclusion) -----------------------------------------------------------
// The word is one bit per player slot: the ALIVE bit (0x02) of _G_LLM_STRAT_PLAYERS[i], all 8 slots, AI
// included. A change means a player was eliminated / removed / lost presence: RX-direct writes that a
// minority would never replay (risk R2).
inline uint32_t roster_word(const uint8_t *player_flags /*[8]*/) {
    uint32_t w = 0;
    for (int i = 0; i < MAX_PEERS; ++i)
        if (player_flags[i] & 0x02) w |= 1u << i;
    return w;
}

// The WIDER word the product wiring feeds the tracker (R2): ALIVE (0x02), HUMAN (0x04), GONE (0x08) and
// DEFEATED (0x10) of `STRAT_PLAYERS[i]` byte 0, four bits per slot. ws::roster_word watches ALIVE alone, but
// a clean quit in a 3-peer game runs mark_player_gone (clears HUMAN, sets GONE + DEFEATED) and leaves ALIVE
// alone until presence_lost, so an ALIVE-only word would miss the RX-direct write the exclusion exists for.
inline uint32_t roster_word_wide(const uint8_t *player_flags /*[8]*/) {
    uint32_t w = 0;
    for (int i = 0; i < MAX_PEERS; ++i) w |= (uint32_t)((player_flags[i] >> 1) & 0xFu) << (4 * i);
    return w;
}

struct roster_tracker {
    bool     seen         = false;
    uint32_t word         = 0;
    uint32_t changed_step = 0; // the step of the most recent change (0 = never observed changing)
    // Feed once per step. Returns true when the word differs from the previous observation.
    bool observe(uint32_t step, uint32_t w) {
        if (!seen) {
            seen = true;
            word = w;
            return false;
        }
        if (w == word) return false;
        word         = w;
        changed_step = step;
        return true;
    }
    // Quiet = no change within `quiet_steps` before `step`. A tracker that never saw a change is quiet.
    bool quiet(uint32_t step, int quiet_steps) const {
        if (changed_step == 0) return true;
        return step >= changed_step && (step - changed_step) >= (uint32_t)quiet_steps;
    }
};

// ---- per-sender hash history --------------------------------------------------------------------------
// The host groups the peers' hashes at ONE step (pick_diverged) and each verdict only carries one sender's
// value, so every judged comparison is remembered here: 128 steps per slot, slot 8 = the host's own hash.
// A step evicted by a newer one (step % 128) reads as unknown, never as a stale value.
struct hash_hist {
    static constexpr int N = 128;
    uint32_t             step[MAX_PEERS + 1][N];
    uint64_t             st[MAX_PEERS + 1][N];
    void                 clear() { memset(this, 0, sizeof(*this)); }
    void                 put(int who, uint32_t s, uint64_t v) {
        if (who < 0 || who > MAX_PEERS || s == 0) return;
        step[who][s % N] = s;
        st[who][s % N]   = v;
    }
    bool get(int who, uint32_t s, uint64_t *v) const {
        if (who < 0 || who > MAX_PEERS || s == 0 || step[who][s % N] != s) return false;
        *v = st[who][s % N];
        return true;
    }
};

// ---- 2. the gates ----------------------------------------------------------------------------------
enum peer_state : int { P_IDLE = 0,
                        P_BEGUN,
                        P_SENT,
                        P_COOLDOWN };

enum skip_reason : int {
    SKIP_NONE = 0,
    SKIP_ACTION_OFF,    // [desync] action != 1
    SKIP_NOT_HOST,      // clients never act
    SKIP_NO_LIBMH,      // libmh / contract rows / [promote] orders not live
    SKIP_TRANSPORT,     // MH_NetSnapshotStatus.supported != 1 (TCP)
    SKIP_MODE,          // SESSION_MODE != 3
    SKIP_PEER,          // the peer is not ALIVE|HUMAN, or not in MH_Net_ActivePeerIds
    SKIP_CONFIRM,       // fewer than confirm_steps consecutive mismatching steps
    SKIP_ROSTER,        // roster changed inside the quiet window
    SKIP_BUSY,          // the peer is not IDLE
    SKIP_MAX,           // resync_max reached
    SKIP_HOST_MINORITY, // N>=3 and the host is outside the largest group
    SKIP_COUNT
};

inline const char *skip_name(skip_reason r) {
    static const char *const n[SKIP_COUNT] = {"none", "action", "not host", "libmh", "transport", "mode",
                                              "peer", "confirm", "roster", "busy", "max", "host is the minority"};
    return (r >= 0 && r < SKIP_COUNT) ? n[r] : "?";
}

struct gate_input {
    bool           is_host;
    bool           libmh_ok;         // configuration 2, contract rows resolved, [promote] orders live
    bool           transport_ok;     // MH_NetSnapshotStatus.supported == 1
    int            session_mode;     // SESSION_MODE (3 = running)
    bool           peer_human_alive; // the peer is ALIVE|HUMAN
    bool           peer_active;      // and in MH_Net_ActivePeerIds
    int            consecutive;      // consecutive mismatching steps for this peer (tick verdict)
    uint32_t       step;             // the host's current step
    roster_tracker roster;           // the host's roster tracker
    peer_state     state;            // this peer's machine
    int            resyncs_done;     // resyncs begun so far for this peer this match
};

// The first failing gate, in a fixed order (cheapest/most fundamental first), or SKIP_NONE. Host-minority is
// decided by pick_diverged, not here.
inline skip_reason gate_check(const config &c, const gate_input &g) {
    if (c.action != 1) return SKIP_ACTION_OFF;
    if (!g.is_host) return SKIP_NOT_HOST;
    if (!g.libmh_ok) return SKIP_NO_LIBMH;
    if (!g.transport_ok) return SKIP_TRANSPORT;
    if (g.session_mode != 3) return SKIP_MODE;
    if (!g.peer_human_alive || !g.peer_active) return SKIP_PEER;
    if (g.consecutive < c.confirm_steps) return SKIP_CONFIRM;
    if (!g.roster.quiet(g.step, c.roster_quiet_steps)) return SKIP_ROSTER;
    if (g.state != P_IDLE) return SKIP_BUSY;
    if (g.resyncs_done >= c.resync_max) return SKIP_MAX;
    return SKIP_NONE;
}

// ---- 3. the per-peer state machine (host side) -------------------------------------------------------
//   IDLE --INCIDENT(gate ok)--> BEGUN --CAPTURED--> SENT --DONE--> COOLDOWN --(cooldown_steps)--> IDLE
// with ABORT out of BEGUN / SENT into COOLDOWN. `resyncs` counts BEGINs. Every input is an event value.
struct fsm {
    peer_state state           = P_IDLE;
    int        resyncs         = 0; // BEGINs so far
    uint32_t   begin_step      = 0; // incident step D
    uint32_t   capture_step    = 0; // S (0 until captured)
    int        attempts        = 0; // capture attempts made
    int        elapsed_ms      = 0; // since BEGIN, as reported by the caller
    int        cooldown_left   = 0;
    uint32_t   roster_at_begin = 0; // roster word when BEGIN was sent
};

enum event_kind : int {
    E_INCIDENT = 0,    // the gate passed for this peer (a = incident step D, roster = current word)
    E_CAPTURE_OK,      // capture done + blob queued + META sent (a = S)
    E_CAPTURE_FAIL,    // this attempt failed (e.g. -8 hashed slice missing)
    E_DONE,            // T_WS_DONE received (a = live step, rc in `rc`)
    E_STEP,            // one host step elapsed (elapsed_ms = ms since BEGIN, roster = current word,
                       //   session_mode = SESSION_MODE, peer_active = still an active human)
    E_PENDING_OVERFLOW // ORDER_PENDING overflowed on the host (R5)
};

struct event {
    event_kind kind;
    uint32_t   a            = 0;
    int        rc           = 0;
    int        elapsed_ms   = 0;
    uint32_t   roster       = 0;
    int        session_mode = 3;
    bool       peer_active  = true;
};

enum act_kind : int {
    A_NONE = 0,
    A_SEND_BEGIN,    // send T_WS_BEGIN; the capture happens at the top of the next step
    A_RETRY_CAPTURE, // try the capture again after cfg.capture_retry_every steps
    A_SEND_ABORT,    // send T_WS_ABORT(reason) and cancel the transfer (MH_Net_SnapshotCancel)
    A_LOG_DONE,      // DONE accepted (rc >= 0 or not): log `; [worldsync] DONE`
    A_COOLDOWN_END   // COOLDOWN elapsed: back to IDLE
};

struct action {
    act_kind kind   = A_NONE;
    uint32_t reason = WS_ABORT_NONE; // A_SEND_ABORT
};

inline action fsm_abort(fsm &f, const config &c, uint32_t reason) {
    f.state         = P_COOLDOWN;
    f.cooldown_left = c.cooldown_steps;
    return {A_SEND_ABORT, reason};
}

// Pure transition. `f` is updated in place; the returned action is what the wiring must perform.
inline action fsm_step(const config &c, fsm &f, const event &e) {
    switch (e.kind) {
        case E_INCIDENT:
            if (f.state != P_IDLE || f.resyncs >= c.resync_max) return {};
            f.state = P_BEGUN;
            ++f.resyncs;
            f.begin_step      = e.a;
            f.capture_step    = 0;
            f.attempts        = 0;
            f.elapsed_ms      = 0;
            f.roster_at_begin = e.roster;
            return {A_SEND_BEGIN, 0};
        case E_CAPTURE_OK:
            if (f.state != P_BEGUN) return {};
            f.state        = P_SENT;
            f.capture_step = e.a;
            return {};
        case E_CAPTURE_FAIL:
            if (f.state != P_BEGUN) return {};
            if (++f.attempts >= c.capture_retry_max) return fsm_abort(f, c, WS_ABORT_CAPTURE);
            return {A_RETRY_CAPTURE, 0};
        case E_DONE:
            if (f.state != P_SENT) return {}; // a stray DONE (no transfer in flight) is ignored
            f.state         = P_COOLDOWN;
            f.cooldown_left = c.cooldown_steps;
            return {A_LOG_DONE, 0};
        case E_PENDING_OVERFLOW:
            if (f.state == P_BEGUN || f.state == P_SENT) return fsm_abort(f, c, WS_ABORT_PENDING_OVFL);
            return {};
        case E_STEP:
            if (f.state == P_COOLDOWN) {
                if (--f.cooldown_left <= 0) {
                    f.state = P_IDLE;
                    return {A_COOLDOWN_END, 0};
                }
                return {};
            }
            if (f.state == P_BEGUN || f.state == P_SENT) {
                f.elapsed_ms = e.elapsed_ms;
                // Order of precedence: roster (the D26 exclusion), match ending (R4), peer gone, deadline.
                if (e.roster != f.roster_at_begin) return fsm_abort(f, c, WS_ABORT_ROSTER);
                if (e.session_mode != 3) return fsm_abort(f, c, WS_ABORT_GAMEOVER);
                if (!e.peer_active) return fsm_abort(f, c, WS_ABORT_PEER_GONE);
                if (e.elapsed_ms >= c.timeout_ms) return fsm_abort(f, c, WS_ABORT_TIMEOUT);
            }
            return {};
    }
    return {};
}

// ---- 4. the minority side ---------------------------------------------------------------------------
// The admission ring keeps the last RING records. A per-source counter `have[s]` (records admitted so far)
// and `oldest[s]` (the lowest index still held for source s, 0 = none held). To replay indices
// n_src[s]+1 .. have[s] the ring must still hold n_src[s]+1. A source with have[s] <= n_src[s] has nothing
// to replay. Returns 0 = covered, else -30 (the DONE rc: "ring too short", report-only).
inline constexpr int RC_RING_SHORT = -30;
inline int           ring_covers(const uint32_t *have, const uint32_t *oldest, const uint32_t *n_src) {
    for (int s = 0; s < v2::WS_SRC_SLOTS; ++s) {
        if (have[s] <= n_src[s]) continue;
        if (oldest[s] == 0 || oldest[s] > n_src[s] + 1) return RC_RING_SHORT;
    }
    return 0;
}

enum import_verdict : int {
    IMP_WAIT = 0,  // blob or META not here yet
    IMP_GO,        // import now
    IMP_BAD_STEP,  // blob header step != META.S
    IMP_BAD_ROOT,  // blob root != META.root
    IMP_BAD_LEN,   // blob length != META.blob_len
    IMP_RING_SHORT // the admission ring cannot reach n_src
};

struct import_input {
    bool     blob_ready;
    bool     meta_seen;
    uint32_t blob_step;
    uint32_t meta_step;
    uint32_t blob_len, meta_len;
    bool     root_match; // MH_NetSnapshotStatus.root_hex == META.root
    int      ring_rc;    // ring_covers()
};

inline import_verdict import_check(const import_input &i) {
    if (!i.blob_ready || !i.meta_seen) return IMP_WAIT;
    if (i.blob_step != i.meta_step) return IMP_BAD_STEP;
    if (i.blob_len != i.meta_len) return IMP_BAD_LEN;
    if (!i.root_match) return IMP_BAD_ROOT;
    if (i.ring_rc != 0) return IMP_RING_SHORT;
    return IMP_GO;
}

// The peer refuses to start/continue an import when its own match is ending (R4) or it was told to abort.
inline bool minority_must_abort(int session_mode, bool abort_received) { return abort_received || session_mode != 3; }

// ---- fast-forward arithmetic (game-time seconds; `sub` = the sim sub-step) --------------------------------
// At import, TOTAL_GAME_TIME = min(T_live, clock_S + ff_steps*sub); each frame the live target advances by
// dt*speed (clamped to committed) and TOTAL = min(g_ff_live, clock + ff_steps*sub). Exit when the backlog
// is within two caps.
inline double ff_cap(double clock, int ff_steps, double sub) { return clock + (double)ff_steps * sub; }
inline double ff_total(double t_live, double clock, int ff_steps, double sub) {
    const double cap = ff_cap(clock, ff_steps, sub);
    return t_live < cap ? t_live : cap;
}
inline double ff_live_advance(double live, double dt, double speed, double committed) {
    double v = live + dt * speed;
    if (committed > 0.0 && v > committed) v = committed;
    return v < live ? live : v; // never backwards
}
inline bool ff_done(double t_live, double clock, int ff_steps, double sub) {
    return (t_live - clock) <= 2.0 * (double)ff_steps * sub + 1e-9; // eps: double rounding at the edge
}

} // namespace mh::desync::ws
