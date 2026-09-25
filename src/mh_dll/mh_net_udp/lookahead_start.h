#ifndef MH_NET_UDP_LOOKAHEAD_START_H
#define MH_NET_UDP_LOOKAHEAD_START_H
//
// mh_net_udp/lookahead_start.h -- mp:P14: the adaptive lookahead's START value, seeded from the RTT
// the UDP endpoint already measured in the lobby, instead of the fixed 100 ms guess.
//
// I/O-free and header-only for the same reason udp_stats.h's lookahead_decide is: the claim is
// arithmetic, so `net_selftest.exe udpstatstest` asserts it without a rig. The caller
// (mh/seams/net_lockstep.cpp adaptive_tick -- an mh.dll seam, so configuration (1) runs it with no
// libmh) gathers the per-peer SRTT from MH_Net_GetStats and applies the answer once, at the first
// lockstep frame of the process's first match.
//
// ---- WHY A SEED AT ALL (the row's evidence) --------------------------------------------------------
// On a slow link the visible opening stall is T3c's 1.5 s warm-up plus the first ~2 s decision window,
// both run ENTIRELY at the start value: at --shim-delay 200 (SRTT ~405 ms) the first window was 73-83%
// starved on both peers from a 100 ms start (mp:P12's rig runs: host `starved 1657/2000`, client
// `1329/2000` on the first `[adaptive]` line). No change to the controller's STEP reaches that window --
// it ends before the first decision. mp:P11 ruled out a blind higher start (300 on a LAN settles at
// 84 ms against 60), so the objection is to the GUESS; the link is already measured before Start: the
// endpoint pings every PING_MS_DEFAULT (1000 ms) and folds each echo into an RFC 6298 SRTT per peer
// (udp_stats.h RttEstimator), the same number the lobby's ping column (mh/ui/lobby_ping.cpp, mp:L1b)
// shows.
//
// ---- THE FORMULA:  start = floor + AD_START_RTT_MULT * max peer SRTT, clamped to [floor, ceil] --------
// Read it as `floor + 1.25 x (SRTT / 2)`: cover the advert's one-way flight (SRTT/2), with 25% on the
// flight for the frame / clock-quantum overhead that grows with it, plus the controller's own floor
// (AD_SIM_FLOOR_MULT = 3 sub-steps, 60 ms at the shipping sim_step 20) as the buffer a peer needs to
// run concurrently at all. Fitted to the SETTLED bands the controller itself found on the rig, which
// are the only measurement of "what this link needs" that exists:
//   LAN        SRTT ~1 ms    -> start 60-61   settled 60 / 60 (P11, both peers on the floor)
//   shim 100   SRTT ~205 ms  -> start 188     settled 144-206 across 9 P10/P11 peer samples
//   shim 200   SRTT ~405 ms  -> start 313     settled 223-313; the host's tail95 reaches the hold band
//                                              at ~300-312 (+21 at 312, +20 at 300; -31 at 250;
//                                              mp:P12's Wave-2 rig runs, and its scope's 341/223)
// The plain one-way form (floor + SRTT/2) lands at 262 on the 405 ms link, UNDER that band: the rig's
// window at 250 ms was still 11% starved on the host. The user's ruling (2026-09-23) is that starving
// is worse than input delay, so the fit is taken from the TOP of each band rather than its middle: the
// cost of a start a little high is a few windows of shrink (half the surplus per window after three
// clean ones), the cost of one a little low is the stall this row exists to remove. On a LAN the two
// forms agree to the millisecond, so the bias costs a LAN nothing -- it cannot leave the floor band.
//
// WHY SRTT ALONE, NOT (SRTT + 4*RTTVAR) LIKE mp:P10's owd. At match start RTTVAR is NOT a jitter
// measurement yet: RFC 6298 initialises it to R/2 and decays it by 3/4 per sample, and the rig's
// lobbies hand over after only 3-5 pings (shim200 host log: sample 3 rttvar 114 ms, sample 5 65 ms, on
// a link whose converged RTTVAR is 1-2 ms -- mp:P12's shim-200 host log). (SRTT + 4*RTTVAR)/2 at sample 4 would read 375 ms of
// one-way on a 202 ms link and seed the ceiling. SRTT on the other hand is exact from the first sample
// on a steady link (initialised to R) and within 0.3% by sample 3 (405.5 vs a converged 404.5).
//
// THE SAMPLE FLOOR, AD_START_MIN_RTT_SAMPLES = 3: the fewest the rig's own lobbies accrued before Start
// (3-5 at 1 ping/s -- the harness lobby is the fastest one there is; a human lobby takes longer), and
// enough that one outlier echo (the first ping can race the handshake's own work) is averaged at 1/8
// weight instead of standing alone. Below it, or when the transport cannot measure a round trip at all
// (TCP: lat_supported = 0), the start is the configured one (SHIP_LOOKAHEAD_MS, 100) and the caller
// logs that it fell back.
//
// WHICH PEER: the WORST (largest SRTT). The binding peer is the one whose horizon arrives latest, and
// covering an average link is how a mixed game stalls on the bad one (net_lockstep.cpp's
// lateness_snapshot note). If some peers are measured and others are not, the seed is not trusted to
// speak for the unmeasured ones, so it can only RAISE the fallback: max(fallback, seed).
//
// BOTH PEERS NEED NOT AGREE, and the existing machinery is why. Each side measures the same link, so
// the two seeds differ only by the two estimators' noise (the shim200 logs: 404.8 vs 405.1 ms at the
// same moment -> seeds 313 / 313). And a difference is not a determinism question: the lookahead is a
// peer's willingness to run ahead, gated by `committed = min(own horizon, the peers' advertised
// horizons)` (libmh turn_engine's commit, the original's llm_net_lockstep_commit_horizon) and pinned
// monotone by mp:D30 -- orders
// are stamped at or above every horizon already sent, so they carry their own exec time whatever each
// side's value. Every P10/P11 run already ended with the two peers at DIFFERENT values (e.g. 223/313)
// and ALL PAIRS IDENTICAL.
//
// THE RELAYED PATH measures END TO END by construction: the RTT probe is a REC_PING record inside a
// sealed PKT_DATA frame on the peer connection (udp_endpoint.cpp's watchdog loop), encrypted with the
// pair's own keys -- the relay forwards it and cannot answer it. The relay leg's own OP_PING
// (udp_relay.cpp) feeds nothing here.
#include <stddef.h>

namespace mh {
namespace netstats {

constexpr int    AD_START_MIN_RTT_SAMPLES = 3;
constexpr double AD_START_RTT_MULT        = 0.625; // = 1.25 x the one-way SRTT/2 -- see the table above

enum LookaheadStartReason {
    LS_START_SEEDED  = 0, // every peer measured: start = the formula
    LS_START_NO_RTT  = 1, // no peer has AD_START_MIN_RTT_SAMPLES (or the transport cannot measure)
    LS_START_PARTIAL = 2  // some peers measured: max(fallback, the formula over those)
};

struct LookaheadStartOut {
    double start_ms;
    double rtt_ms;  // the SRTT the start was derived from (-1 when none)
    int    peer;    // the index into the caller's arrays it came from (-1 when none)
    int    samples; // that peer's sample count (0 when none)
    int    reason;  // LookaheadStartReason
};

// `srtt_ms[i]` / `samples[i]` per transport peer, `n` of them. `measurable` false = the transport has
// no latency instrument (TCP), which is LS_START_NO_RTT whatever the arrays hold.
inline LookaheadStartOut lookahead_start(const double *srtt_ms, const int *samples, int n, bool measurable,
                                         double floor_ms, double ceil_ms, double fallback_ms) {
    LookaheadStartOut out;
    out.start_ms = fallback_ms;
    out.rtt_ms   = -1.0;
    out.peer     = -1;
    out.samples  = 0;
    out.reason   = LS_START_NO_RTT;
    int measured = 0, unmeasured = 0;
    if (measurable && srtt_ms && samples) {
        for (int i = 0; i < n; ++i) {
            if (samples[i] >= AD_START_MIN_RTT_SAMPLES && srtt_ms[i] > 0.0) {
                ++measured;
                if (out.peer < 0 || srtt_ms[i] > out.rtt_ms) {
                    out.peer    = i;
                    out.rtt_ms  = srtt_ms[i];
                    out.samples = samples[i];
                }
            } else {
                ++unmeasured;
            }
        }
    }
    if (measured == 0) return out; // fallback, unclamped: it is the configured start, as before P14
    double s = floor_ms + AD_START_RTT_MULT * out.rtt_ms;
    if (unmeasured > 0) {
        out.reason = LS_START_PARTIAL;
        if (s < fallback_ms) s = fallback_ms;
    } else {
        out.reason = LS_START_SEEDED;
    }
    if (s < floor_ms) s = floor_ms;
    if (s > ceil_ms) s = ceil_ms;
    out.start_ms = s;
    return out;
}

} // namespace netstats
} // namespace mh

#endif // MH_NET_UDP_LOOKAHEAD_START_H
