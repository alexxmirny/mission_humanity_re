#ifndef MH_NET_UDP_RELAY_PATH_H
#define MH_NET_UDP_RELAY_PATH_H
//
// mh_net_udp/relay_path.h -- mp:P16: the binding one-way delay on a STAR with three or more peers.
//
// I/O-free and header-only, like lookahead_start.h, so `net_selftest.exe udpstatstest` asserts the
// arithmetic without a rig. The caller (mh/seams/net_lockstep.cpp) gathers the transport's rows and
// the host's published table and hands them over; nothing here touches a socket.
//
// ---- THE BUG THIS REMOVES ------------------------------------------------------------------------
// The transport is a client-server star: a client holds ONE connection, to the host, so its
// MH_NetStats::lat[] is one row (player_id -1). Every other client's horizon reaches it RELAYED, i.e.
// over own->host + host->other. binding_peer_owd_ms() looked the binding peer up by player_id, missed
// (the row is the HOST link), and fell into the single-entry fallback that exists for the 2-player
// case -- so the controller corrected the slack by the HOST link's one-way delay while the horizon it
// was waiting for had flown the whole relayed path. First 3-peer internet match (2026-09-30):
// host<->s1 232 ms, host<->s2 83 ms, s1<->s2 ~315 ms; lookahead medians 168 / 95 ms (sum 263 < 315,
// the mp:P8 pair bound: L1 + L2 must cover the client<->client RTT) -> the clients starved each other, 0.72x realtime.
//
// ---- THE FIX -------------------------------------------------------------------------------------
// The host already measures every client (its lat[] holds one row per client) and already publishes
// the per-slot SRTT summary (ANNOUNCE_PING, mp:L1f). P16 makes the host keep publishing it DURING a
// match (it was lobby-only) and lets the client read the other client's host leg from it:
//
//   owd(self -> other client) = own_owd (the existing (SRTT + 4 RTTVAR)/2 on the host link)
//                             + RELAY_LEG_MULT * published_srtt(other)   (= 1.25 x the host leg's SRTT/2)
//
// The host leg carries no RTTVAR on the wire (the table is player_id + SRTT_ms + relay class), so the
// 1.25 stands in for it, the same bias lookahead_start.h fits for the seed; the error direction is the
// one P10 chose on purpose (an over-estimate returns some of the latency win, an under-estimate costs
// throughput). Only a CLIENT whose binding peer is another CLIENT takes this branch: a client bound by
// the host (the 2-player case, and the 3-peer case where the host is the late one) keeps the host link,
// and the host itself reads its own direct rows by player_id, exactly as before.
//
// DETERMINISM: nothing here feeds the sim. The result gates only this peer's own willingness to run
// ahead (committed = min over peers, pinned monotone by mp:D30), like every other controller input.
#include <stddef.h>

namespace mh {
namespace netstats {

constexpr double RELAY_LEG_MULT  = 0.625; // x the host leg's published SRTT = 1.25 x SRTT/2
constexpr int    RELAY_MAX_PEERS = 8;

struct RelayLatRow {
    int player_id; // -1 = the transport has not learnt it (a client's single host row)
    int samples;
    int srtt_us;
    int rttvar_us;
};

// One-way delay, ms, of the binding peer; -1 = not measured (the gate then stands down).
// `pub_srtt_ms[pid]` is the host's published SRTT of player `pid` (<= 0 or null = nothing fresh).
// `relay_gate` false = the pre-P16 behaviour, bit for bit (the negative arm).
// `hub_id` (mp:U62): the player id of the HUB. It was the literal 0 -- the host -- until the hub could move: a
// migrated match's hub is whichever survivor took over, and "the binding peer IS the hub" must follow it.
// The default keeps every pre-U62 caller (and udpstatstest) bit for bit.
inline double binding_owd_ms(const RelayLatRow *rows, int n, int binding_peer, const double *pub_srtt_ms,
                             bool relay_gate, int hub_id = 0) {
    if (binding_peer < 0 || !rows || n <= 0) return -1.0;
    const RelayLatRow *hit      = 0;
    bool               fallback = false;
    for (int i = 0; i < n && i < RELAY_MAX_PEERS; ++i) {
        if (rows[i].player_id == binding_peer) {
            hit = &rows[i];
            break;
        }
    }
    if (!hit && n == 1 && rows[0].player_id < 0) {
        hit      = &rows[0];
        fallback = true;
    }
    if (!hit || hit->samples <= 0) return -1.0;
    const double srtt_ms   = (double)hit->srtt_us / 1000.0;
    const double rttvar_ms = (double)hit->rttvar_us / 1000.0;
    double       owd       = (srtt_ms + 4.0 * rttvar_ms) * 0.5;
    if (relay_gate && fallback && binding_peer != hub_id && binding_peer < RELAY_MAX_PEERS && pub_srtt_ms &&
        pub_srtt_ms[binding_peer] > 0.0)
        owd += RELAY_LEG_MULT * pub_srtt_ms[binding_peer];
    return (owd > 0.0) ? owd : -1.0;
}

// The lookahead SEED's extra candidates: on a client with one host row, the path RTT to every OTHER
// client is own SRTT + that client's published host-leg SRTT. Appends them to srtt[]/smp[] (capacity
// `cap`, current count `n`) and returns the new count; lookahead_start() takes the max, as it already
// does for a host with several direct rows. `local_id` is skipped (the host publishes our own row to
// us) and so is the host (id 0: that IS the own row). Appends nothing unless the transport reports
// exactly one row (the client shape) and `relay_gate` is on.
inline int seed_with_relay_paths(double *srtt_ms, int *samples, int n, int cap, int local_id,
                                 const double *pub_srtt_ms, bool relay_gate, int hub_id = 0) {
    if (!relay_gate || !pub_srtt_ms || n != 1 || !srtt_ms || !samples) return n;
    const double own = srtt_ms[0];
    const int    smp = samples[0];
    if (own <= 0.0) return n;
    for (int pid = 0; pid < RELAY_MAX_PEERS && n < cap; ++pid) {
        if (pid == hub_id || pid == local_id || pub_srtt_ms[pid] <= 0.0) continue; // the hub's row IS the own row
        srtt_ms[n] = own + pub_srtt_ms[pid];
        samples[n] = smp;
        ++n;
    }
    return n;
}

} // namespace netstats
} // namespace mh

#endif // MH_NET_UDP_RELAY_PATH_H
