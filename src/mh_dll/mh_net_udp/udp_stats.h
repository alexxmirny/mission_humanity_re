#pragma once
//
// udp_stats.h -- THE LATENCY INSTRUMENT (tracker mp:T3, plan D1/D2 channel B).
//
// WHAT THIS IS. The arithmetic behind five numbers the game did not previously know about its own
// link: SRTT, RTTVAR, IPDV, loss, and arrival lateness. It is deliberately ALL ARITHMETIC AND NO
// I/O -- no socket, no clock, no lock, no Windows header -- because that is what makes it testable
// offline (`net_selftest.exe udpstatstest`, RFC 6298's own worked constants) rather than only
// provable on a two-VM rig with a delay injector in front of it. Every caller supplies the samples;
// this file only turns them into statistics and one control decision.
//
// The vocabulary is the latency page of the maintainer docs (tracker mp:L0) and is NOT re-invented here:
//
//   RTT      RFC 2681 round-trip delay, measured at the sender with ONE clock.
//   SRTT     RFC 6298 §2 smoothed RTT, alpha = 1/8. The per-peer "ping" number.
//   RTTVAR   RFC 6298 §2 smoothed mean absolute deviation, beta = 1/4.
//   IPDV     RFC 3393 delay variation -- here over CONSECUTIVE ROUND-TRIP samples, see the note on
//            RttEstimator::sample for why that is the honest form without clock sync.
//   loss     RFC 7680 ratio over a 256-packet sequence window (the transport's own ring width).
//   lateness ms before its deadline a peer's step input arrived. POSITIVE = margin, per L0 §2 --
//            so the dangerous tail is the LOW end, which is why the reductions below are named
//            `tail95`/`tail99` rather than `p95`/`p99`. See LatenessWindow.
//
// WHY THE LOOKAHEAD DECISION IS IN HERE TOO. `lookahead_decide` is the adaptive controller's whole
// judgement, as a pure function of (current lookahead, sim step, floor/ceiling, the binding peer's
// lateness tail, our own unused horizon, the join warm-up, the clean-window count). It lived inside
// net_lockstep.cpp's adaptive_tick as
// straight-line code against globals, where the only way to test "does it climb within one window
// at 200 ms, and does it reach the floor on a clean link" was to run the rig. Extracting the
// decision costs nothing at the call site and makes those two claims offline assertions.
//
#ifndef MH_NET_UDP_STATS_H
#define MH_NET_UDP_STATS_H

#include <stdint.h>

namespace mh {
namespace netstats {

// ---- RFC 6298 §2 constants -- the RFC's own fixed values, NOT tunables ---------------------------
// "alpha = 1/8 and beta = 1/4" (RFC 6298 §2, rule 2.3). They are written as the literal fractions so
// a reader can check them against the RFC without arithmetic.
constexpr double RFC6298_ALPHA = 1.0 / 8.0;
constexpr double RFC6298_BETA  = 1.0 / 4.0;

// ---- SRTT / RTTVAR / IPDV ------------------------------------------------------------------------
struct RttEstimator {
    double srtt_ms;   // RFC 6298 smoothed RTT
    double rttvar_ms; // RFC 6298 smoothed deviation
    double ipdv_ms;   // smoothed |R(n) - R(n-1)| over consecutive samples (see sample())
    double last_ms;   // the previous raw sample, for the IPDV pair
    long   samples;   // how many raw samples have been folded in (0 = nothing measured yet)

    void reset() {
        srtt_ms = rttvar_ms = ipdv_ms = last_ms = 0.0;
        samples                                 = 0;
    }

    // Fold in one raw RTT measurement, in milliseconds.
    //
    // RFC 6298 §2 rule 2.2 (first measurement) then rule 2.3 (every later one). The ORDER of the two
    // assignments matters and is the RFC's: RTTVAR uses the OLD SRTT, so computing SRTT first would
    // quietly shrink the variance term toward zero on a link whose delay is climbing -- exactly when
    // the controller needs it to be large.
    //
    // IPDV IS COMPUTED OVER THE ROUND TRIP, NOT ONE WAY, and that is a deliberate limitation rather
    // than an approximation nobody noticed. RFC 3393 defines IPDV as the difference of two packets'
    // ONE-WAY delays, which needs comparable clocks at the two ends. Channel B's echo timestamp is
    // the peer's GetTickCount -- a different epoch at ~15.6 ms granularity -- so a one-way IPDV read
    // off it would be mostly the peer's timer quantisation. Consecutive ROUND-TRIP differences need
    // no shared clock at all (both stamps are ours), and they carry the same "is this link steady or
    // spiky" signal the L1 stability indicator is for. A true one-way IPDV needs the NTP
    // four-timestamp exchange (the latency page, section 1), which L0 already rules out of v1.
    void sample(double r_ms) {
        if (r_ms < 0.0) r_ms = 0.0;
        if (samples == 0) {
            srtt_ms   = r_ms;
            rttvar_ms = r_ms / 2.0;
            ipdv_ms   = 0.0;
        } else {
            double d = srtt_ms - r_ms;
            if (d < 0.0) d = -d;
            rttvar_ms = (1.0 - RFC6298_BETA) * rttvar_ms + RFC6298_BETA * d;
            srtt_ms   = (1.0 - RFC6298_ALPHA) * srtt_ms + RFC6298_ALPHA * r_ms;
            double p  = r_ms - last_ms;
            if (p < 0.0) p = -p;
            ipdv_ms = (1.0 - RFC6298_BETA) * ipdv_ms + RFC6298_BETA * p;
        }
        last_ms = r_ms;
        ++samples;
    }
};

// ---- the outstanding-probe table ------------------------------------------------------------------
//
// WHY THIS EXISTS AT ALL, given that the PING record already carries `t_origin_ms` the peer echoes
// back: because that field is a uint32 of GetTickCount, so an RTT computed as
// `GetTickCount() - echoed_origin` is quantised to the ~15.6 ms system tick at BOTH ends of the
// subtraction. The done_when this serves asks for SRTT within 10% of 80 ms -- an 8 ms budget -- and
// a 15.6 ms quantum eats it. So the wire field stays exactly as T0 specified it (no format change,
// the udpwiretest fixtures are untouched) and is used only as a MATCH KEY: the sender remembers the
// high-resolution stamp it actually sent at, and the echo selects it back.
//
// PING_TRACK is 8 because pings go out on a ~1 s timer and no reply is interesting after 8 s (the
// watchdog has given up by then anyway). A wrapped-over entry is simply unmatchable, which reads as
// "no sample", not as a wrong sample.
constexpr int PING_TRACK = 8;

// What `on_echo` matched, for the caller that wants to SAY so in a log rather than only act on the
// number. mp:T3b: the ~19 ms under-read was chased for a session on the theory that a pong could be
// matched against a LATER probe's stamp (which is the only arithmetic by which a stopwatch started
// before a send can report less than the path delivers). It cannot -- the origin key advances once
// per ping interval -- but "it cannot" is a claim about code, and the run that settles it has to be
// able to print the slot it used. `outstanding` is the same evidence from the other side: probes
// that never matched are the shape a lost or re-keyed echo makes.
struct EchoMatch {
    int slot;        // the table index the echo matched, or -1
    int outstanding; // probes still unmatched AFTER this one was taken
};

struct PingTracker {
    uint32_t origin_ms[PING_TRACK];
    int64_t  stamp[PING_TRACK]; // the caller's own high-resolution counter at send time
    uint8_t  used[PING_TRACK];
    int      next;

    void reset() {
        for (int i = 0; i < PING_TRACK; ++i) {
            origin_ms[i] = 0;
            stamp[i]     = 0;
            used[i]      = 0;
        }
        next = 0;
    }

    void on_sent(uint32_t origin, int64_t now_stamp) {
        origin_ms[next] = origin;
        stamp[next]     = now_stamp;
        used[next]      = 1;
        next            = (next + 1) % PING_TRACK;
    }

    // Match an echoed origin stamp and return the round trip in milliseconds, or -1.0 when the probe
    // is unknown (wrapped out of the table, or a peer echoing something it was never sent).
    // `freq` is the caller's counter frequency in ticks per second; 0 means "stamps are already ms".
    double on_echo(uint32_t origin, int64_t now_stamp, int64_t freq, EchoMatch *m = 0) {
        double r   = -1.0;
        int    hit = -1;
        for (int i = 0; i < PING_TRACK; ++i) {
            if (!used[i] || origin_ms[i] != origin) continue;
            used[i]       = 0;
            hit           = i;
            int64_t delta = now_stamp - stamp[i];
            if (delta >= 0) // a negative delta is a counter reset, not a measurement
                r = (freq > 0) ? ((double)delta * 1000.0 / (double)freq) : (double)delta;
            break;
        }
        if (m) {
            m->slot        = hit;
            m->outstanding = 0;
            for (int i = 0; i < PING_TRACK; ++i)
                if (used[i]) ++m->outstanding;
        }
        return r;
    }
};

// ---- RFC 7680 loss over a 256-packet sequence window ----------------------------------------------
//
// The window is 256 because that is the width of the transport's own shared inbound ring and of its
// replay window (the latency page, section 1): a loss ratio computed over a different span than the one
// the receiver can actually resolve would not correspond to anything it could act on.
//
// The sequence being counted is the T0 PACKET sequence (the header's per-direction, never-reused
// nonce counter), not a channel-B record number. That choice is what makes loss measurable WITHOUT
// a wire change: the sender already increments it on every datagram, so a hole in what arrives is a
// datagram that did not.
//
// LOSS_GUARD is the reason a freshly-arrived hole is not immediately called a loss: reordering is
// normal on a UDP path (net_shim reorders under jitter deliberately), so the newest few slots are
// still in flight as far as this statistic is concerned and are excluded from the judged range.
constexpr int LOSS_WINDOW     = 256;
constexpr int LOSS_GUARD      = 8;
constexpr int LOSS_MIN_JUDGED = 32; // below this the ratio is noise; loss_pm() answers -1 instead

class LossWindow {
public:
    void reset() {
        m_top   = 0;
        m_first = 0;
        m_any   = false;
        for (int i = 0; i < LOSS_WINDOW / 32; ++i) m_bits[i] = 0;
    }

    void on_rx(uint64_t seq) {
        if (!m_any) {
            m_any   = true;
            m_first = seq;
            m_top   = seq;
            set(seq);
            return;
        }
        if (seq > m_top) {
            // Everything strictly between the old top and the new one enters the window UNSEEN, and
            // anything that scrolled out of it must be cleared or it would be counted twice at the
            // same ring index one wrap later.
            uint64_t advance = seq - m_top;
            if (advance >= (uint64_t)LOSS_WINDOW) {
                for (int i = 0; i < LOSS_WINDOW / 32; ++i) m_bits[i] = 0;
            } else {
                for (uint64_t s = m_top + 1; s <= seq; ++s) clear(s);
            }
            m_top = seq;
            set(seq);
            return;
        }
        if (m_top - seq < (uint64_t)LOSS_WINDOW) set(seq); // an in-window reorder: it DID arrive
        // older than the window: unjudgeable, and already retired. Ignored.
    }

    // Loss over the judged span, in parts per mille, or -1 when too little has settled to say.
    int loss_pm() const {
        if (!m_any) return -1;
        if (m_top < m_first + (uint64_t)LOSS_GUARD) return -1;
        uint64_t hi = m_top - (uint64_t)LOSS_GUARD;
        uint64_t lo = m_first;
        if (m_top + 1 >= (uint64_t)LOSS_WINDOW && m_top + 1 - (uint64_t)LOSS_WINDOW > lo)
            lo = m_top + 1 - (uint64_t)LOSS_WINDOW;
        if (hi < lo) return -1;
        uint64_t span = hi - lo + 1;
        if (span < (uint64_t)LOSS_MIN_JUDGED) return -1;
        uint64_t seen = 0;
        for (uint64_t s = lo; s <= hi; ++s)
            if (get(s)) ++seen;
        uint64_t lost = span - seen;
        return (int)((lost * 1000u + span / 2u) / span);
    }

private:
    void set(uint64_t s) { m_bits[(s % LOSS_WINDOW) / 32] |= (1u << ((s % LOSS_WINDOW) % 32)); }
    void clear(uint64_t s) { m_bits[(s % LOSS_WINDOW) / 32] &= ~(1u << ((s % LOSS_WINDOW) % 32)); }
    bool get(uint64_t s) const {
        return (m_bits[(s % LOSS_WINDOW) / 32] & (1u << ((s % LOSS_WINDOW) % 32))) != 0;
    }

    uint64_t m_top;
    uint64_t m_first;
    bool     m_any;
    uint32_t m_bits[LOSS_WINDOW / 32];
};

// ---- the arrival-lateness distribution -------------------------------------------------------------
//
// Samples are ARRIVAL LATENESS in the latency page's section-2 sign: POSITIVE = the peer's step input was
// there with that many ms of margin before its deadline, NEGATIVE = the sim sat blocked that long
// waiting for it. So the dangerous end of this distribution is the LOW one, and the two reductions
// the controller wants are the 5th and 1st percentiles.
//
// THE NAMES ARE `tail95` / `tail99`, NOT `p95` / `p99`, and the difference is not cosmetic. The plan
// row says "raise fast on p95 lateness", which means the 95th-percentile-WORST case; with L0's sign
// that is the 5th percentile of the value. Calling the field `p95` would have every later reader
// take the 95th percentile of a positive-is-good quantity, which is the BEST case -- a controller
// driven off it would relax exactly when the link got spiky. The name says which tail.
//
// It is a bounded ring of raw samples rather than bucketed counts: at 256 samples an exact
// percentile costs one sort of 256 ints once per control window (microseconds, on the frame thread
// that is already doing this arithmetic), and bucket edges chosen in advance would have to guess the
// range -- which spans -2000 ms (a stall) to +400 ms (the lookahead ceiling).
constexpr int LATE_WINDOW = 256;

class LatenessWindow {
public:
    void reset() {
        m_n    = 0;
        m_next = 0;
    }

    void push(int ms) {
        if (ms > 30000) ms = 30000; // a paused VM is not a lateness measurement
        if (ms < -30000) ms = -30000;
        m_v[m_next] = ms;
        m_next      = (m_next + 1) % LATE_WINDOW;
        if (m_n < LATE_WINDOW) ++m_n;
    }

    int count() const { return m_n; }

    // p50 = the median; tail95/tail99 = the 5th/1st percentile (95%/99% of samples are at or above
    // them). Returns false when there is nothing to reduce.
    bool percentiles(int &p50, int &tail95, int &tail99) const {
        if (m_n <= 0) return false;
        int s[LATE_WINDOW];
        for (int i = 0; i < m_n; ++i) s[i] = m_v[i];
        sort_asc(s, m_n);
        p50    = s[idx_at(50)];
        tail95 = s[idx_at(5)];
        tail99 = s[idx_at(1)];
        return true;
    }

private:
    int idx_at(int pct) const {
        // Nearest-rank on the ASCENDING order, matching tools/mp_pacing_report.py's `pct()` so a
        // number read off the log and a number computed by the DLL mean the same thing.
        int i = (int)(((double)pct / 100.0) * (double)(m_n - 1) + 0.5);
        if (i < 0) i = 0;
        if (i > m_n - 1) i = m_n - 1;
        return i;
    }

    // Shell sort: no <algorithm>, no allocation, no recursion, and ~n log^2 n on 256 ints. An
    // insertion sort would also do, but it is O(n^2) exactly when the window is full of a sorted
    // run, which is the ordinary case on a steady link.
    static void sort_asc(int *a, int n) {
        for (int gap = n / 2; gap > 0; gap /= 2)
            for (int i = gap; i < n; ++i) {
                int t = a[i], j = i;
                while (j >= gap && a[j - gap] > t) {
                    a[j] = a[j - gap];
                    j -= gap;
                }
                a[j] = t;
            }
    }

    int m_v[LATE_WINDOW];
    int m_n;
    int m_next;
};

// ---- everything one peer is measured by ------------------------------------------------------------
struct PeerStats {
    RttEstimator rtt;
    PingTracker  ping;
    LossWindow   loss;

    void reset() {
        rtt.reset();
        ping.reset();
        loss.reset();
    }
};

// ---- the adaptive-lookahead decision ---------------------------------------------------------------
//
// The shape is the one mp:P1 measured and the perf page records -- GROW FAST, SHRINK SLOWLY, with
// a hysteresis band between the two thresholds. What mp:T3 changes is the ERROR TERM, from the
// aggregate starved-TIME fraction over the window (a local, peer-blind proxy: "was the sim blocked")
// to the binding peer's arrival-lateness tail ("how close to its deadline is the peer actually
// cutting it"). The proxy could only ever say that the horizon ran out; the tail says by how much,
// against which peer, and therefore by how much the lookahead has to move.
//
// THE THRESHOLDS ARE RELATIVE TO THE SIM SUB-STEP, for the same reason the floor is (the 2026-07-25
// frozen-client trap, net_lockstep.cpp): "half a sub-step of margin" is a physical statement about
// whether one hiccup wedges the peer, and "10 ms of margin" is a statement about one particular
// sim_step_ms that stopped being true when the shipping value moved from 10 to 20.
constexpr double AD_LATE_GROW_MULT    = 0.5;  // tail95 margin under this many sub-steps -> grow
constexpr double AD_LATE_SHRINK_MULT  = 1.5;  // ...over this many -> may shrink. Between: hold.
constexpr double AD_LATE_GROW_STEP    = 1.25; // the FLOOR on a growth step (the old AD_GROW)
constexpr double AD_LATE_GROW_MAX     = 2.00; // ...and its ceiling: one window cannot more than double
constexpr double AD_LATE_SHRINK       = 0.96; // the FLOOR on a shrink step
constexpr int    AD_LATE_SHRINK_AFTER = 3;    // ...only after this many consecutive clean windows
constexpr int    AD_LATE_MIN_SAMPLES  = 8;    // a tail off fewer samples than this is not a tail

// ---- mp:T3c: giving latency back at the speed it was taken ---------------------------------------
//
// T3 shipped the shrink half as a flat 4% per window, and a clean-LAN run then measured what that
// costs: the host walked 100 -> 60 ms in 29.6 s and the client never arrived at all inside 89 s.
// Two separate causes, and each needs its own answer.
//
// (1) THE STEP WAS FLAT WHILE THE ERROR WAS NOT. Growth has been proportional since mp:P1 fix (d)
// -- "add exactly the margin that is missing" -- and the shrink half kept the flat step it was
// written with. A peer sitting on 100 ms of lookahead while the measurement says it has 105 ms of
// margin is not 4% too high, it is nearly twice too high, and thirteen windows of 4% is the
// arithmetic saying so slowly. So a shrink now ALSO gives back a fraction of the measured surplus,
// and the two forms compose without a threshold between them: the step actually taken is whichever
// of the two is LARGER, which makes the proportional form self-gating -- it only overtakes the 4%
// step once the surplus is around twice the shrink threshold, and below that the old behaviour is
// bit-for-bit what it was.
//
// WHY HALF THE SURPLUS AND NOT ALL OF IT: it is the largest fraction that provably cannot overshoot.
// Take the worst case, where the margin tracks the lookahead one-for-one; then after giving back
// half of (tail95 - shrink_at) the new margin is (tail95 + shrink_at)/2, which is still ABOVE
// shrink_at for any tail95 above it. The controller therefore approaches the hysteresis band from
// above and can never cross it in a single step, which is the property that stops a shrink from
// manufacturing the starvation that the next window answers with a grow.
//
// (2) THE ERROR TERM CANNOT SEE A LOOKAHEAD THAT IS NOT BINDING. Arrival lateness is measured off
// PEER_HORIZON -- it says how close the PEER is cutting it. A peer whose own advertised horizon is
// larger than the one the match actually runs on is bound by the OTHER side, and its lateness tail
// is then a property of the other side's lookahead, not of its own: measured on the rig, a client
// sitting at 120 ms read tail95 = 20..40 ms all run long while the host walked 100 -> 60, because
// what it was measuring was the host's horizon; its own 60 ms of waste was invisible to it.
//
// The missing signal is available locally and needs no wire: UNUSED HORIZON, local_h - COMMITTED.
// When that is positive our own advertisement is not what the match is running on, so the part of it
// above COMMITTED is doing nothing for anybody, and giving it back cannot make us stall -- COMMITTED
// is min(local, peers) and we are not the min. It enters the decision twice: as a second surplus the
// proportional step may be computed from, and as an alternative way for a window to count as
// comfortable, so a tail quantised onto the band edge no longer erases the shrink credit of a peer
// that is demonstrably holding slack. It is self-terminating: the slack falls to zero exactly when
// our horizon becomes the binding one, and the lateness tail governs alone from there.
//
// (3) AND ONE THIN WINDOW ERASED EVERY COMFORTABLE ONE BEFORE IT. The shrink credit was zeroed by a
// reading inside the hysteresis band, which is not a failure -- it is the state the controller is
// AIMING for. The error term is quantised onto the sim sub-step grid (both the horizon and the sim
// clock move in sub-steps), and the band is one sub-step wide, so on a healthy link the 5th
// percentile lands inside it about half the time purely from where the grid falls. Three CONSECUTIVE
// comfortable windows then arrive about once in eight, which is the 12-to-17-second gaps the clean
// LAN measured between one client shrink and the next. The credit now DECAYS by one instead, so the
// question it answers becomes "were these windows MOSTLY comfortable" rather than "were the last
// three flawless" -- and mp:P1's scar is untouched, because the event that scarred it (a starved
// window, i.e. a GROW) still resets the credit to zero outright.
constexpr double AD_LATE_SHRINK_FRAC = 0.5;  // give back half the measured surplus...
constexpr double AD_LATE_SHRINK_MAX  = 0.60; // ...but never more than 40% of the value in one window
constexpr double AD_SLACK_MULT       = 0.5;  // unused horizon over this many sub-steps -> not binding

// ---- mp:P10: the unused-horizon signal has to be measured against the LINK, not against zero ----
//
// T3c's (2) above is right that "the error term cannot see a lookahead that is not binding", and it
// acted on that in the shrink half only. The grow half kept growing on a low lateness tail whatever
// the slack said -- and the 2026-09-20 field corpus is what that costs: in all six 2-peer sessions
// (c86c5b49, eb1c9f9d, bd8bffcf, 31f319ae, 4665a570, bf340f97) the pair walked to (host = 400 =
// lockstep_max_ms, joiner = 60 = the effective floor) inside ~25 s, from an identical 100 on both
// sides, every time. The mechanism is a two-peer ratchet: the host reads the joiner's horizon
// arriving late, grows ITS OWN lookahead -- which cannot raise its own COMMITTED, because COMMITTED
// is a min and the host is not the min -- and thereby hands the joiner margin, so the joiner reads a
// comfortable tail and gives its own lookahead back, so its horizon reaches the host later still.
// The host ends up paying ~420 ms of command latency (the net indicator's CMD is the LOCAL lookahead
// plus one sub-step) for horizon that nobody could use.
//
// THE OBVIOUS ONE-LINE FIX -- `&& !slack` on the grow arm, with `slack` as T3c defined it -- IS
// WRONG, and the field logs say so before any rig does. BOTH peers read `slack 100 ms` in their
// FIRST window, while both were genuinely starved (`tail95 -31`) at 100 ms of lookahead on a 205 ms
// link. That is not a measurement error: COMMITTED is computed against the peer's LAST ARRIVED
// advertisement, which is one one-way delay old, so on any link with real latency BOTH sides see
// their own horizon above their own COMMITTED at the same instant. There is no consistent global
// min in a system with delay. Refusing to grow there deadlocks the pair at its start value: measured
// in the offline two-peer arm (udpstatstest (l)), 0.452x realtime against 0.995x (the numbers the
// arm prints for itself), because the pair's throughput needs the SUM of the two lookaheads to
// cover the round trip and neither side will raise its half.
//
// SO THE SIGNAL IS CORRECTED RATHER THAN THE ARM. An advertisement in flight is not idle horizon --
// it is news that has not landed yet. Subtract the link's own one-way delay and what is left is the
// part of our horizon that is genuinely unused:
//
//     idle = slack - owd  ~=  (our lookahead) - (the peer's lookahead)
//
// (the algebra: with clocks c_us/c_peer and steps S_us/S_peer, slack = (c_us - c_peer) + S_us -
// S_peer + owd, so subtracting owd leaves the step difference plus the clock offset). Read that way
// the rule states itself: DO NOT GROW PAST YOUR PEER. If we are already advertising more lookahead
// than our peer is, the pair's shortfall is on the peer's side, and more of our own horizon cannot
// move COMMITTED on either side of the link. Equal peers on a slow link have idle ~= 0 and grow
// together, which is the case the naive refusal broke.
//
// `link_owd_ms` IS DELIBERATELY A CONSERVATIVE (HIGH) ESTIMATE, because the two errors are not
// symmetric and the offline arm measures the asymmetry: fed HALF the true one-way delay the pair
// runs at 0.76x, fed a quarter of it 0.45x; fed 1.5x it runs at 0.997x and fed 3x at 0.997x -- an
// over-estimate only gives back some of the latency win and degrades smoothly toward the old
// behaviour, an under-estimate costs throughput. The caller therefore passes RFC 6298's own upper
// bound halved, (SRTT + 4*RTTVAR)/2, which on the field's link (SRTT 205-230 ms, RTTVAR 1-4 ms) is
// ~108 ms against a true one-way of ~104. A caller that cannot measure the link passes a NEGATIVE
// value and the gate stands down entirely -- every decision is then bit-for-bit what it was before
// this note, which is what the TCP module (lat_supported = 0) and every pre-P10 selftest arm get.
//
// AND THE PROBE THAT IS NOT THERE. The obvious belt-and-braces -- let the gate suppress at most N
// starved windows, then grow anyway -- was built and measured and is NOT in the code: at N = 1 or 2
// it restores the full (400, 60) ratchet even when the estimate is exact, and at N = 3 it is
// non-monotonic in the estimate error (0.62x at f = 0, 0.92x at f = 0.5, 0.99x at f = 0.75). A gate
// that periodically forgets its own reason is a slower ratchet, not a safer one; the protection is
// the conservative estimate above, which cannot be wrong in the dangerous direction.

// ---- mp:P12: the FIRST decision after the warm-up may take the whole proportional step --------------
//
// AD_LATE_GROW_MAX (one doubling per window) is right in general: it is what stops a noisy tail from
// launching the lookahead. But it is a cap on noise, and the first decision after T3c's warm-up is
// the one window whose measurement is not noisy. Measured on the rig at --shim-delay 200 (RTT ~400,
// 2026-09-22): the first decision read tail95 -297 (host) / -359 (client), so `want` was 407 / 469 --
// within a few ms of what the link turned out to need -- and the cap cut it to 200. The second window
// is NOT capped (200 -> 273 is the proportional step landing under 2x), so exactly one clamped decision
// cost the whole visible stall: the first window 73-74% starved, ~2 s of the game stalling three frames
// in four, and essentially all of the run's 4.7% / 6.1% starvation.
//
// So the first decided window after the warm-up drops the cap when its tail has the weight the
// controller already demands before it will act EARLY: AD_FIRST_GROW_MIN_SAMPLES, which is
// net_lockstep.cpp's AD_FAST_MIN_SAMPLES (32, static_assert'd equal there). The rig's first windows
// carried 47+; a thinner tail keeps the doubling cap. The step is then `cur + (grow_at - tail95)` with
// only the ceiling above it. Every later window keeps the cap, including a later FIRST grow: the claim
// is about the post-warm-up measurement, not about growing for the first time.
//
// WHY THERE IS NO SECOND, "TAIL DEFICIT OVER ONE SUB-STEP" GUARD, although it was the obvious other
// candidate (a tail within one sub-step of zero can be where the grid fell -- the T3c (3) note). It is
// implied, so it would be a guard that no input can trip: the cap only BINDS when the proportional step
// exceeds a doubling, i.e. when grow_at - tail95 > cur_ms, and cur_ms >= floor_ms >= AD_SIM_FLOOR_MULT
// (3) sub-steps. So every window this changes already has a deficit of more than three sub-steps and a
// tail below -2.5 of them; the grid argument is settled by the floor before this code runs.
// THE SAME ARITHMETIC IS WHY A LAN IS INERT: a first decision whose deficit is under cur_ms -- a LAN's
// first tail sits at or near zero margin -- is not capped in the first place, so it is bit-for-bit the
// pre-P12 decision (udpstatstest (m) asserts that equality; the rig's LAN arm measures that the first
// decision there does not grow at all).
constexpr int AD_FIRST_GROW_MIN_SAMPLES = 32;

enum LookaheadVerdict {
    LA_NO_SAMPLES = 0, // nothing measured this window -- HOLD, and say so
    LA_HOLD       = 1, // inside the hysteresis band, or still accruing shrink credit
    LA_GROW       = 2,
    LA_SHRINK     = 3,
    LA_WARMUP     = 4 // mp:T3c -- still inside the join transient; decide nothing yet
};

struct LookaheadIn {
    double cur_ms;    // the lookahead in force
    double sim_ms;    // sim_step_ms -- the unit the thresholds are expressed in
    double floor_ms;  // max(configured lockstep_min_ms, AD_SIM_FLOOR_MULT * sim_ms)
    double ceil_ms;   // configured lockstep_max_ms
    bool   have;      // a binding peer produced at least AD_LATE_MIN_SAMPLES samples
    double tail95_ms; // its 95th-percentile-worst arrival lateness (positive = margin)
    int    clean_in;  // consecutive clean windows so far
    // mp:T3c. `warm` is false for the first moments after the peer set goes live, while the join
    // transient is still in the samples; the caller keeps the clock, this file only refuses to
    // decide. `slack_ms` is the window's median unused horizon (local_h - COMMITTED, floored at 0),
    // which is zero for the peer whose own horizon is binding.
    bool   warm;
    double slack_ms;
    // mp:P10. The link's own one-way delay, in the same milliseconds as `slack_ms`, as a
    // CONSERVATIVE (high) estimate -- the caller passes (SRTT + 4*RTTVAR)/2, RFC 6298's RTO shape
    // halved. NEGATIVE means "this transport cannot measure its link": the grow gate then stands
    // down and `slack_ms` is used raw, so the whole decision is bit-for-bit pre-P10. See the
    // AD_SLACK_MULT note above for why the estimate is biased high rather than centred.
    double link_owd_ms;
    // mp:P12. `samples` is the binding peer's sample count behind `tail95_ms`; `first_warm` is true
    // for the first window the caller judges after the warm-up (it stays true across LA_NO_SAMPLES
    // windows -- nothing was decided on those). Both default to "off", so a caller that does not set
    // them gets the pre-P12 decision bit for bit.
    int  samples    = 0;
    bool first_warm = false;
};

struct LookaheadOut {
    double want_ms;
    int    clean_out;
    int    verdict;            // LookaheadVerdict
    bool   first_full = false; // mp:P12 -- this GROW took the uncapped first-decision step
};

inline LookaheadOut lookahead_decide(const LookaheadIn &in) {
    LookaheadOut out;
    out.want_ms   = in.cur_ms;
    out.clean_out = in.clean_in;
    out.verdict   = LA_NO_SAMPLES;
    if (!in.warm) {
        // mp:T3c. The join transient is REAL starvation -- the peer genuinely has not advertised yet
        // -- so there is nothing here to filter out by percentile or sample count; the samples are
        // honest and the regime they describe is over before the controller could act on it. The
        // only correct response to a measurement of a regime that no longer exists is to decline to
        // decide, and (at the call site) to throw the samples away. Banking no shrink credit either
        // is deliberate: `clean_in` counts evidence of surplus, and a window nobody judged is not
        // evidence of anything.
        out.clean_out = 0;
        out.verdict   = LA_WARMUP;
    } else if (in.have && in.sim_ms > 0.0 && in.cur_ms > 0.0) {
        const double grow_at   = AD_LATE_GROW_MULT * in.sim_ms;
        const double shrink_at = AD_LATE_SHRINK_MULT * in.sim_ms;
        // mp:T3c -- "our own horizon is not the one the match is running on". See the AD_SLACK_MULT
        // note above: while this holds, the lateness tail is a measurement of the OTHER side.
        // mp:P10 -- and the measurement of it is corrected for the link, because an advertisement
        // in flight is not idle horizon. With no link measurement (`link_owd_ms` negative) this is
        // the raw T3c slack and the grow gate below is off, i.e. exactly the pre-P10 decision.
        const bool have_owd = (in.link_owd_ms >= 0.0);
        double     idle_ms  = in.slack_ms;
        if (have_owd) {
            idle_ms = in.slack_ms - in.link_owd_ms;
            if (idle_ms < 0.0) idle_ms = 0.0;
        }
        const bool slack = (idle_ms > AD_SLACK_MULT * in.sim_ms);
        // mp:P10 -- THE GROW GATE, and `have_owd` is half of it: WITHOUT a link measurement
        // `slack` is the raw T3c quantity, which is positive on both peers of any slow link, so
        // gating on it would deadlock the pair -- the gate is only ever armed by a measured link.
        // With one, `slack` means "we are already advertising more lookahead than our peer is", so
        // the pair's shortfall is on the peer's side and nothing we add to our own horizon can move
        // COMMITTED on either side. Falling THROUGH to the shrink arm rather
        // than holding is load-bearing: the arm below qualifies on `slack` alone, so a lookahead
        // carried in from a previous match (the field's 400) is given back instead of being
        // stranded by its own gate -- measured, a `hold` here leaves 400/60 exactly where it was.
        if (in.tail95_ms < grow_at && !(slack && have_owd)) {
            // Add exactly the margin that is missing, then clamp the STEP (not the target) so the
            // response is proportional to the error -- mp:P1 fix (d): a flat +25% took four windows
            // to climb from 100 ms to the 200 ms a 200 ms link needs, and the peer was starved for
            // all eight of those seconds.
            double want = in.cur_ms + (grow_at - in.tail95_ms);
            if (want < in.cur_ms * AD_LATE_GROW_STEP) want = in.cur_ms * AD_LATE_GROW_STEP;
            // mp:P12 -- the first post-warm-up decision, on a tail with AD_FIRST_GROW_MIN_SAMPLES behind
            // it, keeps the whole proportional step (the ceiling below still bounds it). See the note.
            const bool full = in.first_warm && in.samples >= AD_FIRST_GROW_MIN_SAMPLES;
            out.first_full  = full && want > in.cur_ms * AD_LATE_GROW_MAX;
            if (!full && want > in.cur_ms * AD_LATE_GROW_MAX) want = in.cur_ms * AD_LATE_GROW_MAX;
            out.want_ms   = want;
            out.clean_out = 0; // any starvation resets the patience counter
            out.verdict   = LA_GROW;
        } else if (in.tail95_ms > shrink_at || slack) {
            out.clean_out = in.clean_in + 1;
            if (out.clean_out >= AD_LATE_SHRINK_AFTER) {
                // The flat step is the FLOOR on a shrink, not the shrink: whichever of the three
                // candidates takes the most off wins, and the two proportional ones only overtake
                // 4% once there is a real surplus behind them (see the T3c note above).
                double want = in.cur_ms * AD_LATE_SHRINK;
                if (in.tail95_ms > shrink_at) {
                    const double give = AD_LATE_SHRINK_FRAC * (in.tail95_ms - shrink_at);
                    if (in.cur_ms - give < want) want = in.cur_ms - give;
                }
                if (slack) {
                    // mp:P10 -- half the IDLE horizon, not half the raw slack. Giving back the
                    // flight-time part makes us the binding peer and starves us next window: with
                    // the correction on the grow arm only, the offline two-peer arm measured the
                    // joiner cycling 70 <-> 100 ms on a six-window period. One signal, corrected
                    // once, read by both arms.
                    const double give = AD_LATE_SHRINK_FRAC * idle_ms;
                    if (in.cur_ms - give < want) want = in.cur_ms - give;
                }
                // ...and the bound that makes "cannot oscillate" a property of the code rather than
                // of the two proofs above: whatever the measurement says, one window never gives
                // back more than 40% of the value in force.
                if (want < in.cur_ms * AD_LATE_SHRINK_MAX) want = in.cur_ms * AD_LATE_SHRINK_MAX;
                out.want_ms = want;
                out.verdict = LA_SHRINK;
            } else {
                out.verdict = LA_HOLD;
            }
        } else {
            // In the band: hold, accrue no credit toward shrinking -- and, since mp:T3c, DECAY the
            // credit rather than zeroing it (see (3) above). Only a grow wipes it.
            out.clean_out = (in.clean_in > 0) ? in.clean_in - 1 : 0;
            out.verdict   = LA_HOLD;
        }
    }
    if (out.want_ms < in.floor_ms) out.want_ms = in.floor_ms;
    if (out.want_ms > in.ceil_ms) out.want_ms = in.ceil_ms;
    return out;
}

// ---- mp:T4: the per-frame horizon advert's dedup --------------------------------------------------
// The present hook's eager advert (net_lockstep.cpp, [net] eager_advertise, a SHIP default) runs once
// per PRESENT. It writes HORIZON = GAME_CLOCK + lookahead and, before T4, also sent that value as a
// type-2 EXTEND on every frame. Each send takes its own segment in the UDP transport's 1024-slot send
// window, and a frame rate with no cap (1100-3500 fps on a headless rig lane) filled the window at a
// 360 ms round trip. The link then dropped with `the outbound stream ran a full window ahead of the
// peer's acknowledgements` (dead-ends G294).
//
// THE RULE: send when this frame's write CHANGES the global HORIZON. Skip when HORIZON already held
// exactly that value before the write.
//
// WHY NOT "SKIP WHEN IT EQUALS THE LAST VALUE THIS PATH SENT". That was the first version, and it
// can skip a send the protocol needs. The eager write can LOWER the horizon. The order scheduler
// (libmh orders/order_queue.cpp `schedule`) PULLS HORIZON forward to an order's exec_time, and the
// order carries it to the peer. The next eager write sets HORIZON back to clock + lookahead. Before
// T4 that lowered value went straight onto the wire. Keyed on "last value this path sent", it matched
// the eager path's own previous send and was skipped. The peer would keep the higher value while our
// next order was stamped with the lower one. (A determinism run of the first version did desync, but
// runs with eager_advertise=0 desync in the same shape too -- tracked separately under mp:T4 -- so
// that run does not prove this mechanism. The argument here is from the code.)
//
// What the rule keeps is the pre-T4 invariant: after the eager block, the last horizon this peer put
// on the wire is the value in HORIZON. Every other writer of HORIZON puts what it writes on the wire:
// the pump, the heartbeat, the keepalive/emergency bumps, resync and the U19b repair send an EXTEND.
// The order scheduler's pull-forward goes out inside the order itself, because the receiver takes
// an order's exec_time as the sender's horizon (rx_dispatch.cpp MSG_ORDER). So when this frame's
// write leaves HORIZON unchanged, the value on the wire is already that value, and the skipped send
// carried no information. The 50 ms heartbeat still sends for liveness.
inline bool advert_should_send(double horizon_before, double horizon_now) {
    return !(horizon_before == horizon_now);
}

// ---- mp:D30: the advertised horizon never goes DOWN -----------------------------------------------
// THE LOCKSTEP GUARANTEE: an order's exec_time must be >= every horizon this peer has already
// advertised. A peer that holds our horizon H as its committed limit may simulate every step up to
// H. So an order we stamp below H can arrive after that peer has stepped past its exec_time, and the
// peer then releases it a step late (release_due's `!(exec > now)`) -- a desync.
//
// Orders are stamped from the horizon: order_dispatch uses max(GAME_CLOCK + STEP_SIZE, HORIZON) and
// schedule only raises that to HORIZON. Every horizon writer computes GAME_CLOCK + STEP_SIZE: retail
// time_tick's advertise_horizon (libmh timekeeper.cpp), the pump keepalive, the eager present hook and
// the heartbeat. So when the adaptive controller cuts the lookahead in one jump (315 -> 253 ms
// measured), every writer LOWERS the horizon by ~62 ms, and the next orders go out below a value the
// peer already holds. That is D30 (dead-ends G297, runs tmp/o5_rig/D30/).
//
// THE RULE: the lookahead actually pinned into STEP_SIZE is
//     max(controller target, max_horizon_sent - GAME_CLOCK)
// so a shrink takes effect as the clock catches up to the horizon already sent, instead of by
// lowering it. The delay is bounded by the size of the cut (at most AD ceiling - floor of game time)
// and costs nothing else: the controller still owns the target, and a grow is immediate.
//
// `clock + step` is nudged up if rounding would leave it one ulp under `max_sent`: a horizon that
// reads back a hair below the value on the wire is exactly the bug, only smaller.
constexpr double HZ_RESTART_S = 2.0; // a sent max more than this ahead of the clock = a restarted clock

inline bool horizon_max_is_stale(double clock_s, double max_sent_s) {
    return clock_s + HZ_RESTART_S < max_sent_s; // the same rule on_time_tick's g_hz_max_seen uses
}

inline double monotone_step(double target_s, double clock_s, double max_sent_s) {
    if (!(max_sent_s > 0.0) || horizon_max_is_stale(clock_s, max_sent_s)) return target_s;
    const double need = max_sent_s - clock_s;
    if (!(need > target_s)) return target_s;
    double step = need;
    for (int i = 0; i < 3 && clock_s + step < max_sent_s; ++i) step += 1e-9;
    return step;
}

// The horizon a writer that does its OWN arithmetic (the eager hook, the heartbeat) may put on the
// wire: clock + step, never below the maximum already sent (or held -- `floor_s` is the HORIZON
// global's current value where the caller can read it safely).
inline double monotone_horizon(double clock_s, double step_s, double floor_s) {
    const double h = clock_s + step_s;
    if (floor_s > 0.0 && !horizon_max_is_stale(clock_s, floor_s) && h < floor_s) return floor_s;
    return h;
}

// mp:D30 / D31 -- an order is LATE at the receiver when its sim has already run the step that should
// have released it: release_due releases at the first step whose clock satisfies !(exec > clock), so
// a receiver whose GAME_CLOCK already satisfies it will release the order one step (or more) later
// than the sender did. Same comparison shape as release_due, so the NaN case agrees with it.
inline bool order_is_late(double exec_s, double receiver_clock_s) { return !(exec_s > receiver_clock_s); }

} // namespace netstats
} // namespace mh

#endif // MH_NET_UDP_STATS_H
