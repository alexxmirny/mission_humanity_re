#ifndef MH_DIAG_COUNTER_H
#define MH_DIAG_COUNTER_H
//
// mh_diag_counter.h -- a counter that stays visible when the process is KILLED, not exited
// (dead-end G101, tooling:TL-SUITE-COUNTERS).
//
// G101: `g_dropped` counted the exact loss D24 spent three sessions hunting, and no artifact of any
// run ever showed it -- its only print was one line inside the transport's graceful-shutdown path,
// and every rig determinism run ends by KILLING the peers. "The number exists in a struct" is not
// instrumentation in a system whose runs never reach that line.
//
// Two behaviours make a counter visible under a KILL, and NEITHER is a new thread or timer:
//   1. FIRST-HIT -- say so the moment it happens, in full, with the counter's identity in the line.
//   2. PERIODIC ROLLUP -- report from a cadence that ALREADY runs (a per-N-seconds watchdog tick, a
//      lockstep/desync sampling pass, the frametime log, ...). Never spin a new one just for this.
//
// This header owns neither the log sink nor the message: every mh_dll module already has its own
// (net_transport's logf, desync_watch's say, harness's append_line, ...), so the two functions below
// only decide WHETHER to log, in one call each -- the caller supplies the format string and calls its
// own sink. That is exactly the shape mh_net/net_transport.cpp's `g_dropped` fix and `g_qhigh_band`
// band-crossing report already use by hand; this header is that pattern, named and shared, so the
// next counter does not have to reinvent it (or skip it under a deadline).
//
// NOT thread-safe by itself. A counter touched from more than one thread needs the same
// synchronization it already needs for the increment (InterlockedIncrement, or its owning lock) --
// mh_diag_counter_note()/_rollup_due() only read and write the plain fields below under whatever the
// caller already holds; they take no lock of their own.

typedef struct {
    long count;          // the value itself -- log THIS, not a private shadow of it
    long last_reported;  // count as of the last accepted rollup (0 == "never rolled up")
    int  first_hit_done; // 0 until mh_diag_counter_note() has returned nonzero once
} mh_diag_counter;

#define MH_DIAG_COUNTER_INIT \
    {0, 0, 0}

// Bump the counter by `delta` (usually 1) at the increment site. Returns nonzero exactly once -- the
// call where the count first moved off zero -- which is the caller's cue to log the first occurrence
// immediately and in full (this header does not know the message shape or the sink).
static inline int mh_diag_counter_note(mh_diag_counter *c, long delta) {
    const int was_zero = (c->count == 0);
    c->count += delta;
    if (was_zero && c->count != 0 && !c->first_hit_done) {
        c->first_hit_done = 1;
        return 1;
    }
    return 0;
}

// Call from an EXISTING periodic site. Returns nonzero when the count has moved since the last
// accepted rollup -- the caller's cue to print one line with `c->count` in it -- and marks the
// rollup taken either way. A caller that already logs `count` unconditionally on its own cadence
// (e.g. udp_endpoint.cpp's "net: udp counters ..." line) does not need this call at all; it exists
// for a rollup that should stay silent until something actually happened, the same "printed only
// when something moved" rule mp:T2's bulk-transfer rollup already documents.
static inline int mh_diag_counter_rollup_due(mh_diag_counter *c) {
    if (c->count == c->last_reported) return 0;
    c->last_reported = c->count;
    return 1;
}

// Per-match/per-session reset, same discipline as net_transport.cpp's `g_lanes.reset()` beside its
// own rollup: the next match reports its own numbers, not the carried-over previous one.
static inline void mh_diag_counter_reset(mh_diag_counter *c) {
    c->count          = 0;
    c->last_reported  = 0;
    c->first_hit_done = 0;
}

#endif // MH_DIAG_COUNTER_H
