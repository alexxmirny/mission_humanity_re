#ifndef MH_SEAMS_ADAPTIVE_WINDOW_H
#define MH_SEAMS_ADAPTIVE_WINDOW_H
//
// mh/seams/adaptive_window.h -- mp:P15 (wave 7): WHERE the adaptive controller's first window starts,
// and WHAT its starved figure counts. Pure and I/O-free, like mh_net_udp/lookahead_start.h, so
// `net_selftest.exe udpstatstest` section (p) can assert it without a rig. net_lockstep.cpp calls
// these functions (not a re-derivation of them) from lateness_tick / adaptive_tick / on_time_tick.
//
// THE TWO THINGS THIS FIXES IN THE MEASUREMENT (not in the controller's signal):
//
// (1) THE START. User decision 2026-09-24: the first window is measured from the moment ALL peers
//     are live in the match, not from the host's match start. "Live" is lateness_tick's own test --
//     a PEER_HORIZON slot whose value has MOVED this match (an unused slot holds retail's 10 s
//     sentinel forever). "All" is every other ALIVE && HUMAN player that
//     llm_net_lockstep_count_active_players counts (minus ourselves). The moment is logged once per
//     match on every peer (`; [adaptive] all-peers-live t=...`), the adaptive window is restarted
//     there, and T3c's 1.5 s warm-up is anchored on it -- so the first decided window can never
//     contain a frame from before every peer was advertising.
//     A FALLBACK, so a miscounted roster can never switch the controller off: if the count is not
//     reached within ALL_LIVE_TIMEOUT_MS of the first live peer, the latch closes anyway and the line
//     says `timeout`.
//
// (2) THE STARVED FIGURE. The `starved X/Y ms diag` figure has always been sampled at the TOP of
//     on_time_tick -- BEFORE rx_spin_until_horizon drains RX off-frame. Measured on wave 6's P15 run 3
//     (tmp/wave6_rig/shim/p15_180_r3, host): for 33.0-37.0 s every host frame ENDED with
//     COMMITTED - clock = 16 ms (< one 20 ms sub-step), so every frame OPENED "starved", rx_spin
//     then received the client's EXTEND and funded the step inside the same frame (COMMITTED - clock
//     = 36 ms at time_tick entry), and the clock ran at 1.00x wall. The diag read 1015/1610 ms
//     starved; frames that could not advance even after the spin: 0. Same on runs 1-2 (172/750 and
//     156/750 pre-spin, 0 post-spin). So the figure is now TWO numbers: the old pre-spin diag
//     (kept, every earlier P1/P5 measurement is expressed in it) and the POST-spin one -- time in
//     frames whose sim could not fund its next sub-step even after rx_spin had drained RX. The
//     post-spin figure is what "the sim was starved" means to a player.
//     Known bias: rx_spin waits at most 40 ms (RX_SPIN_TIMEOUT_MS), so a late EXTEND that lands
//     inside the spin is not counted even though that frame was delayed by up to 40 ms. It
//     under-counts by at most one spin per episode; the clock/wall ratio is the cross-check.
//
#include <stdint.h>

namespace mh {
namespace adwin {

// How many OTHER peers must be live. `active_humans` is llm_net_lockstep_count_active_players()'s
// answer (it counts ourselves). Never below 1: this is only asked in live lockstep with a peer.
inline int live_peers_expected(int active_humans) {
    const int others = active_humans - 1;
    return others > 0 ? others : 1;
}

enum AllLive { AL_NOT_YET  = 0,
               AL_ALL_LIVE = 1,
               AL_TIMEOUT  = 2 };

// `since_first_live_ms` = ms since the first peer went live this match (0 if none yet).
inline AllLive all_live_latch(int live_peers, int expected_peers, bool any_live, uint32_t since_first_live_ms,
                              uint32_t timeout_ms) {
    if (live_peers >= expected_peers && live_peers > 0) return AL_ALL_LIVE;
    if (any_live && since_first_live_ms >= timeout_ms) return AL_TIMEOUT;
    return AL_NOT_YET;
}

// T3c's warm-up clock may start only when a lateness tail exists AND every peer is live.
inline bool warm_anchor_ready(bool late_have, bool all_live) { return late_have && all_live; }

// One PEER_HORIZON slot's per-frame observation. Returns true when this read is a MOVE (the caller
// stamps g_late_last_move). THE FIRST READ OF A SLOT IN A MATCH ONLY SEEDS `last_h` -- it is not a
// move. Found by wave 7's rig run 1 (tmp/wave6_rig/shim/p15w7_180_r1): with `last_h` reset to 0.0 per
// match, the first read of every EMPTY slot's retail 10 s sentinel differed from 0.0 and stamped a
// "move", so all 7 non-local slots were live for LATE_LIVE_MS and `all-peers-live peers=7/1` fired
// on the first tick, before the real peer had advertised anything. `seen` is reset with the rest of
// the per-match state.
inline bool horizon_observe(bool &seen, double &last_h, double h) {
    if (!seen) {
        seen   = true;
        last_h = h;
        return false;
    }
    if (h == last_h) return false;
    last_h = h;
    return true;
}

// ---- (3) THE BLOCKED EPISODE, and where it ends (user decision 2026-09-25) ----------------------
// lateness_tick opens an episode when the TOP of on_time_tick finds COMMITTED short of the next
// sub-step, and emits an interim -split_ms sample (then re-arms) if it lasts split_ms. It used to END
// only at the top of a later frame that opened funded -- so a peer phase-locked to its partner's
// EXTENDs (every frame opens 16 ms short, rx_spin funds it inside the frame; wave 6 P15 run 3, G314)
// never ended one: a synthetic -2000 every 2 s, and no other sample. Now the episode also ends right
// after rx_spin_until_horizon when the spin FUNDED the step, charging the real wait (spin end - start).
// An episode the spin could not fund (a genuinely late peer) runs on and still splits at split_ms.

// Top of the frame, the sim is blocked: open an episode, or split one that has lasted split_ms.
// Returns true (with *sample, <= -split_ms) when an interim sample must be pushed.
inline bool episode_blocked_top(uint32_t &since, uint32_t now, uint32_t split_ms, int *sample) {
    if (since == 0) {
        since = now ? now : 1;
        return false;
    }
    if ((uint32_t)(now - since) < split_ms) return false;
    *sample = -(int)(now - since);
    since   = now ? now : 1;
    return true;
}

// After rx_spin: an open episode ENDS here if the spin funded the step. Returns true (with *sample =
// minus the real wait) when it ended; false leaves it running.
inline bool episode_end_post_spin(uint32_t &since, bool funded, uint32_t now, int *sample) {
    if (since == 0 || !funded) return false;
    *sample = -(int)(now - since);
    since   = 0;
    return true;
}

// lateness_tick's per-slot liveness test: not us, a positive horizon, and a MOVE within `live_ms`.
inline bool slot_live(bool is_me, double h, uint32_t last_move, uint32_t now, uint32_t live_ms) {
    return !is_me && h > 0.0 && last_move != 0 && (uint32_t)(now - last_move) <= live_ms;
}

// "Blocked" at one instant: COMMITTED cannot fund the next sub-step. The SAME predicate as the
// pre-spin diag (adaptive_tick's `com < clk + sim`); the two figures differ only in WHEN it is taken.
inline bool blocked(double clock_s, double committed_s, double sim_step_s) {
    return committed_s < clock_s + sim_step_s;
}

// The POST-spin starved accounting, one per adaptive window (reset with it). Each frame adds the wall
// interval adaptive_tick measured for it (already clamped to AD_MAX_SAMPLE_MS), judged by `blocked`
// sampled AFTER rx_spin_until_horizon. The pre-spin diag keeps its own legacy counters.
struct PostSpinWindow {
    uint32_t time_ms    = 0;
    uint32_t starved_ms = 0;
    void     add(uint32_t dt, bool post_blocked) {
        time_ms += dt;
        if (post_blocked) starved_ms += dt;
    }
    void reset() { time_ms = starved_ms = 0; }
};

} // namespace adwin
} // namespace mh

#endif // MH_SEAMS_ADAPTIVE_WINDOW_H
