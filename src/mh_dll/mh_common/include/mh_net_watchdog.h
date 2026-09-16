#ifndef MH_NET_WATCHDOG_H
#define MH_NET_WATCHDOG_H
//
// mh_net_watchdog.h -- the link watchdog's TIMING DECISIONS, as pure functions.
//
// Pure so they can be checked without a rig (`net_selftest.exe watchdogtest`), for the same reason
// ui_test's stall_abort is: the load-bearing half of this logic is a drop that must NOT happen, and
// no live run can demonstrate a thing that does not occur. A relayed internet game reproduces the
// bug in minutes and a LAN rig never does, so "it passed on the rig" was never evidence here.
//
// D16 (2026-08-06). The watchdog needs g_conn_cs both to ping and to judge silence, and MH_Net_Send
// holds that same lock ACROSS a blocking send() bounded only by SO_SNDTIMEO (5 s). So a stalled link
// starves the watchdog: it sends no keepalives, the peer counts that as OUR silence and drops us at
// rx_timeout -- and when we finally acquire the lock, every conn's last_rx has aged past the timeout
// too, so we drop the peer in the same pass. Both ends, mutually, on a link that had been carrying
// data fine. That is the 2026-08-02 relay signature.
//
// The correction: a watchdog may only charge a peer for time it was actually WATCHING.

// Milliseconds of a watchdog pass that were NOT observed, and must therefore be credited back to
// every peer instead of charged as silence.
//
// `elapsed_ms` is wall time since the previous pass, `tick_ms` the intended sleep, `late_ms` the
// slip at which a pass counts as blind. The threshold matters: without it, ordinary scheduling
// jitter would credit a few ms every pass and the deadline would never arrive -- a watchdog that
// forgives everything is exactly as useless as one that forgives nothing, and is the failure this
// function's own test has to rule out.
static inline unsigned mh_watchdog_blind_ms(unsigned elapsed_ms, unsigned tick_ms, unsigned late_ms) {
    const unsigned over = (elapsed_ms > tick_ms) ? (elapsed_ms - tick_ms) : 0u;
    return (over >= late_ms) ? over : 0u;
}

// How long a peer has been silent, given the watchdog's `now` and the conn's last_rx stamp.
//
// THIS IS NOT `now - last_rx`, and the difference is a peer dropped in the middle of a healthy
// match. Both values are GetTickCount() stamps written by DIFFERENT THREADS: the watchdog samples
// `now` before it takes g_conn_cs, and the recv thread stamps last_rx on every inbound frame with
// no lock at all. So a frame that arrives while the watchdog is waiting for the lock leaves
// last_rx AHEAD of now, the unsigned subtraction wraps to ~4.29e9, and that sails past any timeout.
// Measured 2026-08-30 on a LAN determinism run: `net: conn 0 silent 4294967280 ms` -- i.e. -16 ms --
// logged one millisecond BEFORE the log's next inbound frame from the peer it had just declared
// dead. The blind-time credit above can produce the same shape from the other direction, since it
// pushes last_rx forward and may push it past now.
//
// A negative difference means "heard from more recently than the watchdog's clock reading", which is
// zero silence, not infinite silence. Anything within half the tick range counts as the future; that
// is the standard GetTickCount wrap-safe comparison, and it keeps working across the 49.7-day wrap.
static inline unsigned mh_watchdog_silence_ms(unsigned now_ms, unsigned last_rx_ms) {
    const unsigned d = now_ms - last_rx_ms;
    return (d > 0x80000000u) ? 0u : d;
}

// Has this connection been silent long enough to drop? `timeout_ms == 0` disables the check (the
// documented "[net] rx_timeout_ms=-1" escape used when a peer is parked under a debugger).
static inline int mh_watchdog_should_drop(unsigned silent_ms, unsigned timeout_ms) {
    return timeout_ms > 0u && silent_ms >= timeout_ms;
}

#endif // MH_NET_WATCHDOG_H
