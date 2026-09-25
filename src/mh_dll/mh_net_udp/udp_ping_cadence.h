#ifndef MH_NET_UDP_PING_CADENCE_H
#define MH_NET_UDP_PING_CADENCE_H
//
// mh_net_udp/udp_ping_cadence.h -- mp:P15: the PER-PEER warm-up ping cadence, as a pure I/O-free
// decision function, for the same reason lookahead_start.h and udp_stats.h are: the claim is
// arithmetic over two clocks, so `net_selftest.exe udpstatstest` can assert it without a rig or a
// socket. udp_endpoint.cpp's timer_loop calls THIS function (not a re-derivation of it) for every
// active conn every tick, so an offline arm that drives it proves the real wiring, not a parallel
// formula that could drift from what timer_loop actually does.
//
// See udp_endpoint.h's FAST_PING_MS / FAST_PING_WINDOW_MS for the constants and their rationale
// (mp:P15's row: P14's seed needs AD_START_MIN_RTT_SAMPLES=3 pings, and at the shipped 1 Hz cadence
// that is a 3 s floor a quick lobby may not clear).
//
#include <stdint.h>

namespace mh {
namespace netudp {

// Is `now` (GetTickCount()-space, like every caller in this transport) a tick THIS peer should ping
// on? `since_admit_ms` = now - conn.admitted_ms (set once, at admission); `since_ping_ms` = now -
// conn.last_ping_ms (this peer's own last-ping clock, updated by the caller on a true return).
// `ping_ms <= 0` means pings are off entirely (the R-live three-state convention). The warm-up rate
// is clamped to never exceed a slower CONFIGURED steady rate: this can only ADD samples early, never
// override a deliberately fast `[net] ping_ms`.
inline bool ping_due(int ping_ms, uint32_t since_admit_ms, uint32_t since_ping_ms, uint32_t fast_ms,
                     uint32_t fast_window_ms) {
    if (ping_ms <= 0) return false;
    const uint32_t warm_ms    = ((uint32_t)ping_ms < fast_ms) ? (uint32_t)ping_ms : fast_ms;
    const uint32_t cadence_ms = (since_admit_ms < fast_window_ms) ? warm_ms : (uint32_t)ping_ms;
    return since_ping_ms >= cadence_ms;
}

} // namespace netudp
} // namespace mh

#endif // MH_NET_UDP_PING_CADENCE_H
