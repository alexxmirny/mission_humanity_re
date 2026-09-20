//
// udp_bulk_selftest.cpp -- `net_selftest.exe udpbulktest`: CHANNEL C (mp:T2).
//
// `udpwiretest` (mp:T0) proves the PIECE and ACK bytes against committed fixtures and `udploopbacktest`
// (mp:T1) proves the channel-A byte stream above them. This suite proves the third thing: a
// multi-megabyte artefact crossing channel C reliably, under loss, across a link that DIES in the
// middle -- and doing it without a completed chunk ever being thrown away.
//
// IN-PROCESS FOR udploopbacktest's REASON, which is worth repeating because it is the whole reason
// the endpoint is an object: a child process talks to a real 127.0.0.1 socket and 127.0.0.1 does not
// drop datagrams, so "1 MB crosses at 5% loss" would have no way to produce the 5%. Two endpoints in
// one process, each with its own socket and its own seeded `set_rx_loss`, gives a loss dial applied
// exactly where the network would apply it -- and the counters to say what repaired what.
//
// ---- THE NINE ARMS -------------------------------------------------------------------------------
//
// The first two need no sockets at all and run in milliseconds.
//
//   0. lane classes the vocabulary itself: an MSG_ORDER frame and a completed chunk are both in lanes
//                   no eviction path can reach, and a bare horizon is the one that is -- a vocabulary
//                   where nothing was evictable would pass every other check in the file.
//  0b. piece bound  a piece whose declared length disagrees with its INDEX is refused by the
//                   RECEIVER, because T0's codec has no rule tying the two and the arithmetic would
//                   otherwise run 116 bytes off the end of the assembly buffer. The well-formed run
//                   of the same chunk is accepted right after, so the check is a bound, not a wall.
//
//   1. clean        1 MiB, no loss. Every byte arrives, the receiving buffer's SHA-256 equals the
//                   sender's, and NOTHING was retransmitted -- a clean link that needed the repair
//                   path would mean the window arithmetic is wrong, not that the link was lucky.
//   2. lossy        1 MiB at 5% injected loss, BOTH directions (pieces and acknowledgements). Same
//                   hash assertion, plus the non-vacuity one first: the injection actually fired.
//                   This is mp:T2's headline acceptance clause.
//   3. resume       the receiver is KILLED mid-transfer (Endpoint::stop(), mp:T1b's rule) and dialled
//                   again. The claim is not merely that the transfer finishes -- it is that it
//                   RESUMES: the receiver's chunk frontier is the same number after the restart as
//                   before it, so the sender picks up at the last acknowledged chunk instead of at
//                   zero. MUTATION: clearing the bulk channel in Endpoint::stop() -- the obvious
//                   "tidy up on teardown" bug, since stop() memsets everything else -- turns this
//                   arm red and nothing else.
//   4. cold         the receiver comes back having forgotten EVERYTHING -- a restarted PROCESS, not
//                   arm 3's endpoint restart. Its frontier is back at zero while the sender is in
//                   the middle, so the sender must REWIND to it. Measured as a real deadlock while
//                   mutation-testing arm 3, which is why the sender follows the receiver's frontier
//                   in BOTH directions rather than only forward.
//   5. lane         the application STOPS draining. The lane fills, refuses, and the transfer
//                   applies backpressure; when draining resumes the transfer completes with the
//                   right hash. The assertion this item exists for is in the middle of that:
//                   `lane_refused > 0` AND `lane_evicted == 0`. A lane that evicted would also
//                   "recover", and would deliver a blob whose hash is wrong -- which is exactly the
//                   failure D24 measured on the other queue, one channel over.
//   6. synthetic    the `[net] bulk_selftest_mb` path, where the sender has no buffer at all and
//                   generates the blob from its offset. The bytes the receiver assembles must equal
//                   the same generator. Without this arm the rig knob would be untested code whose
//                   first run is on the rig.
//   7. pacing       channel-A DELIVERY LATENCY, idle versus with a mebibyte crossing underneath.
//                   The OFFLINE half of the item's rig clause, and a different claim with a
//                   different instrument rather than a substitute for it: the rig measures the
//                   game's frame pacing, this measures the transport's own latency for a channel-A
//                   record while channel C is saturated. Precise because both peers share one
//                   clock -- the sender stamps the record and the receiver subtracts. The budget is
//                   ONE endpoint tick, which is exactly what BULK_BURST spends.
//
// WHAT WOULD MAKE ARM 2 VACUOUS is the same trap udploopbacktest names: a loss dial that never fired
// makes the lossy arm the clean arm wearing a label. So it asserts `dgram_dropped_sim > 0` and
// `tx_retx > 0` before it believes the hash.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../mh_net_udp/udp_endpoint.h"
#include "../mh_net_udp/udp_relay.h" // mp:R1d -- LEG_OVERHEAD, the number the stride pays for

namespace {

using mh::netudp::Config;
using mh::netudp::Counters;
using mh::netudp::Endpoint;
using mh::netudp::bulk::CHUNK_BYTES;
using mh::netudp::bulk::LANE_SLOTS;
using mh::netudp::bulk::synth_byte;
using Bulk = mh::netudp::bulk::Stats;

int g_checks = 0, g_fails = 0;

void checkf(bool ok, const char *fmt, ...) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        char    b[500];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        printf("  FAIL: %s\n", b);
    }
}

// The suite's own key, for udploopbacktest's reason: a suite must not depend on, or create, an
// mh_key.txt beside whatever directory it happens to run in.
const unsigned char UB_PSK[32] = {
    0x4d,
    0x48,
    0x74,
    0x65,
    0x73,
    0x74,
    0x6b,
    0x65,
    0x79,
    0x20,
    0x75,
    0x64,
    0x70,
    0x20,
    0x62,
    0x75,
    0x6c,
    0x6b,
    0x20,
    0x54,
    0x32,
    0x20,
    0x66,
    0x69,
    0x78,
    0x65,
    0x64,
    0x21,
    0x21,
    0x21,
    0x21,
    0x21,
};

void ub_log(void * /*ctx*/, const char *line) {
    if (getenv("MH_UDP_BULK_VERBOSE")) printf("    | %s\n", line);
}

void ub_cfg(Config &c, int role, int port, unsigned short bind_port, int rx_timeout_ms) {
    memset(&c, 0, sizeof(c));
    c.net.role = role;
    lstrcpynA(c.net.host, "127.0.0.1", sizeof(c.net.host));
    c.net.port      = port;
    c.net.player_id = (role == 0) ? 0 : 1;
    c.net.log       = 1;
    // host_assign OFF, for udprelinktest's reason: a WELCOME rides the reliable stream, and a peer
    // that has received stream bytes acknowledges forever -- which would keep a killed receiver
    // looking alive across arm 3's outage.
    c.net.host_assign   = 0;
    c.net.ping_ms       = 200;
    c.net.rx_timeout_ms = rx_timeout_ms;
    c.redundancy        = 3;
    c.bind_port         = bind_port;
}

template <class Pred>
bool wait_for(Pred pred, DWORD budget_ms) {
    const DWORD deadline = GetTickCount() + budget_ms;
    for (;;) {
        if (pred()) return true;
        if ((long)(deadline - GetTickCount()) <= 0) return pred();
        Sleep(2);
    }
}

// ---- the blob and the sink -----------------------------------------------------------------------
// Statics rather than locals: a mebibyte each, and this file already owns two 4 MB endpoints.
constexpr uint32_t BLOB_MAX = 1024u * 1024u;
unsigned char      g_blob[BLOB_MAX];
unsigned char      g_sink[BLOB_MAX];
bool               g_sink_have[BLOB_MAX / CHUNK_BYTES + 2];

Endpoint g_host, g_cl;

void blob_fill(uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) g_blob[i] = synth_byte(i);
}

void sink_reset() {
    memset(g_sink, 0, sizeof(g_sink));
    memset(g_sink_have, 0, sizeof(g_sink_have));
}

// Drain the receiver's never-evictable lane into the sink. Returns how many chunks it took, so a
// caller can tell "nothing arrived" from "nothing was waiting".
int sink_drain(Endpoint &ep, uint32_t blob_len) {
    int got = 0;
    for (;;) {
        uint32_t      id  = 0;
        int           len = (int)CHUNK_BYTES;
        unsigned char tmp[CHUNK_BYTES];
        if (!ep.bulk_recv(&id, tmp, &len)) return got;
        const uint32_t off = id * CHUNK_BYTES;
        if (off < blob_len && (uint32_t)len <= blob_len - off) {
            memcpy(g_sink + off, tmp, (size_t)len);
            g_sink_have[id] = true;
        }
        ++got;
    }
}

uint32_t chunks_of(uint32_t len) {
    return (len + CHUNK_BYTES - 1u) / CHUNK_BYTES;
}

bool sink_complete(uint32_t len) {
    const uint32_t n = chunks_of(len);
    for (uint32_t i = 0; i < n; ++i)
        if (!g_sink_have[i]) return false;
    return true;
}

void print_bulk(const char *who, const Bulk &b) {
    printf("     %s: tx chunk %u/%u pieces %ld (retx %ld) | rx chunks %ld pieces %ld dup %ld "
           "out-of-window %ld sha-fail %ld mismatch %ld | lane depth %d high-water %d/%d refused "
           "%ld EVICTED %ld\n",
           who, b.tx_base_chunk, b.tx_chunks_total, b.tx_pieces, b.tx_retx, b.rx_chunks, b.rx_pieces,
           b.rx_dup, b.rx_out_of_window, b.rx_sha_fail, b.rx_mismatch, b.lane_depth,
           b.lane_high_water, b.lane_cap, b.lane_refused, b.lane_evicted);
}

// Bring a host and a client up and wait until the host can address the client by player id.
bool link_up(int base, int host_timeout_ms, unsigned loss_pm) {
    new (&g_host) Endpoint();
    new (&g_cl) Endpoint();
    g_host.set_log(ub_log, nullptr);
    g_cl.set_log(ub_log, nullptr);
    // Distinct seeds so the two ends do not destroy the SAME datagram indices -- a correlated loss
    // pattern no network produces, and one that would hide a repair path that only works one way.
    g_host.set_rx_loss(loss_pm, 0x2b17f00du);
    g_cl.set_rx_loss(loss_pm, 0x5eed1234u);
    Config ch, cc;
    ub_cfg(ch, 0, base, (unsigned short)base, host_timeout_ms);
    ub_cfg(cc, 1, base, (unsigned short)(base + 1), -1);
    if (!g_host.start(ch, UB_PSK, true)) return false;
    if (!g_cl.start(cc, UB_PSK, true)) return false;
    return wait_for([&] { return g_host.peer_count() == 1; }, 10000);
}

// ---- arms 1, 2 and 5 -----------------------------------------------------------------------------
int transfer_arm(const char *name, int base, uint32_t len, unsigned loss_pm, bool synthetic,
                 DWORD budget_ms) {
    printf("  -- %s (%u KiB, %u chunks, loss %u/1000%s)\n", name, len / 1024u, chunks_of(len),
           loss_pm, synthetic ? ", SYNTHETIC blob" : "");
    blob_fill(len);
    sink_reset();
    if (!link_up(base, -1, loss_pm)) {
        checkf(false, "%s: the link came up", name);
        g_host.stop();
        g_cl.stop();
        return 1;
    }
    // The host learns the client's player id from its FLAG_HELLO, which rides the reliable stream a
    // moment after admission -- so the send is retried rather than raced.
    const bool started = wait_for(
        [&] { return g_host.bulk_send(1, synthetic ? nullptr : g_blob, len); }, 5000);
    checkf(started, "%s: the transfer started", name);
    if (!started) {
        g_host.stop();
        g_cl.stop();
        return 1;
    }
    const bool done = wait_for(
        [&] {
            sink_drain(g_cl, len);
            return sink_complete(len);
        },
        budget_ms);
    sink_drain(g_cl, len);

    Bulk bh, bc;
    g_host.bulk_stats(bh);
    g_cl.bulk_stats(bc);
    Counters kh, kc;
    g_host.counters(kh);
    g_cl.counters(kc);
    print_bulk("host(tx)", bh);
    print_bulk("client(rx)", bc);
    printf("     link: synthetic loss %ld | mac-fail %ld malformed %ld\n",
           kh.dgram_dropped_sim + kc.dgram_dropped_sim, kh.mac_fail + kc.mac_fail,
           kh.malformed + kc.malformed);

    checkf(done, "%s: every chunk arrived (%ld of %u)", name, bc.rx_chunks, chunks_of(len));
    checkf(memcmp(g_sink, g_blob, len) == 0, "%s: the received blob is byte-identical to the sent one",
           name);
    checkf(bc.rx_sha_fail == 0, "%s: no chunk failed its own SHA-256 (%ld)", name, bc.rx_sha_fail);
    checkf(bc.lane_evicted == 0, "%s: NO CHUNK WAS EVICTED (%ld) -- chunks are a lane no eviction "
                                 "path can reach (mp:T2)",
           name, bc.lane_evicted);
    checkf(kh.malformed + kc.malformed == 0, "%s: no malformed frames (%ld)", name,
           kh.malformed + kc.malformed);
    if (loss_pm > 0) {
        const long lost = kh.dgram_dropped_sim + kc.dgram_dropped_sim;
        checkf(lost > 0, "%s: the loss injection actually fired (%ld datagrams destroyed)", name, lost);
        checkf(bh.tx_retx > 0, "%s: the retransmit path repaired the loss (%ld repeats)", name,
               bh.tx_retx);
    } else {
        checkf(bh.tx_retx == 0, "%s: a clean link needed NO retransmit (%ld) -- a clean run that used "
                                "the repair path means the window arithmetic is wrong",
               name, bh.tx_retx);
    }
    g_host.stop();
    g_cl.stop();
    return 0;
}

// ---- arm 3: the receiver dies mid-transfer --------------------------------------------------------
int resume_arm(int base) {
    const uint32_t len = 512u * 1024u;
    printf("  -- resume across a receiver restart (%u KiB, %u chunks; mp:T1b's stop/start)\n",
           len / 1024u, chunks_of(len));
    blob_fill(len);
    sink_reset();
    // A 1.5 s host link timeout, as udprelinktest uses: the host must RETIRE the corpse of the old
    // link before the same player dials again, or the transfer would be addressed to two conns.
    if (!link_up(base, 1500, 0)) {
        checkf(false, "resume: the link came up");
        g_host.stop();
        g_cl.stop();
        return 1;
    }
    const bool started = wait_for([&] { return g_host.bulk_send(1, g_blob, len); }, 5000);
    checkf(started, "resume: the transfer started");
    if (!started) {
        g_host.stop();
        g_cl.stop();
        return 1;
    }
    // Let a real prefix cross -- a "resume" from chunk zero is not a resume, so the arm needs the
    // frontier to be somewhere in the middle before it kills anything.
    const uint32_t want = chunks_of(len) / 3u;
    const bool     part = wait_for(
        [&] {
            sink_drain(g_cl, len);
            Bulk b;
            g_cl.bulk_stats(b);
            return b.rx_base_chunk >= want;
        },
        30000);
    Bulk before;
    g_cl.bulk_stats(before);
    checkf(part, "resume: a prefix crossed before the kill (frontier %u, wanted %u)",
           before.rx_base_chunk, want);

    // ---- the kill ---------------------------------------------------------------------------------
    checkf(g_cl.stop(), "resume: the receiver stopped (mp:T1b)");
    Bulk after_stop;
    g_cl.bulk_stats(after_stop);
    // THE LOAD-BEARING ASSERTION. stop() zeroes the conn table, the pending handshakes, the session
    // keys and the queue. If it zeroed the bulk channel too -- the obvious tidy-up -- the frontier
    // would be 0 here and the whole prefix would cross a second time.
    checkf(after_stop.rx_base_chunk == before.rx_base_chunk,
           "resume: the chunk frontier SURVIVED the stop (%u before, %u after)", before.rx_base_chunk,
           after_stop.rx_base_chunk);
    Sleep(2600); // > the host's 1.5 s link timeout: the corpse is retired

    Config cc;
    ub_cfg(cc, 1, base, (unsigned short)(base + 1), -1);
    checkf(g_cl.start(cc, UB_PSK, true), "resume: the receiver dialled again");
    checkf(wait_for([&] { return g_host.peer_count() == 1; }, 10000),
           "resume: the host admitted the second link (peers=%d)", g_host.peer_count());

    const bool done = wait_for(
        [&] {
            sink_drain(g_cl, len);
            return sink_complete(len);
        },
        60000);
    sink_drain(g_cl, len);

    Bulk bh, bc;
    g_host.bulk_stats(bh);
    g_cl.bulk_stats(bc);
    print_bulk("host(tx)", bh);
    print_bulk("client(rx)", bc);
    checkf(done, "resume: the transfer COMPLETED after the restart (%ld of %u chunks)", bc.rx_chunks,
           chunks_of(len));
    checkf(memcmp(g_sink, g_blob, len) == 0,
           "resume: the blob assembled across the restart is byte-identical");
    // A chunk is admitted to the lane exactly once -- the frontier only advances -- so this is the
    // count of DISTINCT chunks delivered. A transfer that restarted from zero would have re-delivered
    // the prefix and this would exceed the chunk count.
    checkf(bc.rx_chunks == (long)chunks_of(len),
           "resume: exactly %u chunks were delivered, none twice (%ld)", chunks_of(len), bc.rx_chunks);
    checkf(bc.lane_evicted == 0, "resume: no chunk was evicted (%ld)", bc.lane_evicted);
    g_host.stop();
    g_cl.stop();
    return 0;
}

// ---- arm 4: the receiver comes back having forgotten EVERYTHING -----------------------------------
// Arm 3's receiver keeps its channel across the restart, which is mp:T1b's rule. This is the other
// case, and it is the one a restarted PROCESS produces: the frontier comes back at zero while the
// sender is in the middle. A sender that could only advance would sit there re-sending a window the
// peer will never accept -- measured as a real deadlock while mutation-testing arm 3, which is why
// the sender FOLLOWS the receiver's frontier in both directions. Simulated by placement-new'ing the
// endpoint, which is total state loss with no test-only hook into the channel.
int cold_arm(int base) {
    const uint32_t len = 128u * 1024u;
    printf("  -- a receiver that forgot everything (%u KiB, %u chunks; the sender must REWIND)\n",
           len / 1024u, chunks_of(len));
    blob_fill(len);
    sink_reset();
    if (!link_up(base, 1500, 0)) {
        checkf(false, "cold: the link came up");
        g_host.stop();
        g_cl.stop();
        return 1;
    }
    const bool started = wait_for([&] { return g_host.bulk_send(1, g_blob, len); }, 5000);
    checkf(started, "cold: the transfer started");
    if (!started) {
        g_host.stop();
        g_cl.stop();
        return 1;
    }
    const uint32_t want = chunks_of(len) / 3u;
    wait_for(
        [&] {
            sink_drain(g_cl, len);
            Bulk b;
            g_cl.bulk_stats(b);
            return b.rx_base_chunk >= want;
        },
        30000);
    Bulk before_host;
    g_host.bulk_stats(before_host);
    checkf(before_host.tx_base_chunk >= want, "cold: the sender was mid-transfer (chunk %u of %u)",
           before_host.tx_base_chunk, chunks_of(len));

    g_cl.stop();
    new (&g_cl) Endpoint(); // total amnesia: a fresh process, not a relink
    g_cl.set_log(ub_log, nullptr);
    Config cc;
    ub_cfg(cc, 1, base, (unsigned short)(base + 1), -1);
    Sleep(2600); // the host retires the corpse first
    checkf(g_cl.start(cc, UB_PSK, true), "cold: the amnesiac receiver dialled again");
    sink_reset(); // it will re-deliver the prefix, which is the point

    const bool done = wait_for(
        [&] {
            sink_drain(g_cl, len);
            return sink_complete(len);
        },
        60000);
    sink_drain(g_cl, len);
    Bulk bh, bc;
    g_host.bulk_stats(bh);
    g_cl.bulk_stats(bc);
    print_bulk("host(tx)", bh);
    print_bulk("client(rx)", bc);
    checkf(done, "cold: the transfer completed from the start again (%ld of %u chunks)", bc.rx_chunks,
           chunks_of(len));
    checkf(memcmp(g_sink, g_blob, len) == 0, "cold: the re-sent blob is byte-identical");
    checkf(bh.tx_resumes > 0, "cold: the sender REWOUND to the peer's frontier (%ld rewinds) rather "
                              "than deadlocking above it",
           bh.tx_resumes);
    checkf(bc.lane_evicted == 0, "cold: no chunk was evicted (%ld)", bc.lane_evicted);
    g_host.stop();
    g_cl.stop();
    return 0;
}

// ---- arm 5: the lane fills and REFUSES ------------------------------------------------------------
int lane_arm(int base) {
    const uint32_t len = 256u * 1024u;
    printf("  -- lane backpressure (%u KiB, %u chunks; the application stops draining)\n",
           len / 1024u, chunks_of(len));
    blob_fill(len);
    sink_reset();
    if (!link_up(base, -1, 0)) {
        checkf(false, "lane: the link came up");
        g_host.stop();
        g_cl.stop();
        return 1;
    }
    const bool started = wait_for([&] { return g_host.bulk_send(1, g_blob, len); }, 5000);
    checkf(started, "lane: the transfer started");
    if (!started) {
        g_host.stop();
        g_cl.stop();
        return 1;
    }
    // NOBODY DRAINS. The lane takes LANE_SLOTS chunks and then refuses; the frontier stops advancing
    // and the sender holds its window. Long enough that a lane which EVICTED would have thrown
    // several chunks away by now.
    const bool filled = wait_for(
        [&] {
            Bulk b;
            g_cl.bulk_stats(b);
            return b.lane_refused > 0;
        },
        30000);
    Bulk stalled;
    g_cl.bulk_stats(stalled);
    print_bulk("client(rx, undrained)", stalled);
    checkf(filled, "lane: the lane filled and REFUSED (refused %ld)", stalled.lane_refused);
    checkf(stalled.lane_high_water == LANE_SLOTS, "lane: the high-water reached the cap (%d of %d)",
           stalled.lane_high_water, stalled.lane_cap);
    checkf(stalled.lane_evicted == 0,
           "lane: a FULL lane evicted NOTHING (%ld) -- refusal is the overflow policy, and a chunk "
           "is never a victim (mp:T2 / MP D24)",
           stalled.lane_evicted);
    checkf(stalled.rx_chunks == (long)LANE_SLOTS,
           "lane: exactly the cap was admitted before backpressure (%ld of %d)", stalled.rx_chunks,
           LANE_SLOTS);

    // Draining again must let the transfer finish -- backpressure, not a dead transfer.
    const bool done = wait_for(
        [&] {
            sink_drain(g_cl, len);
            return sink_complete(len);
        },
        60000);
    sink_drain(g_cl, len);
    Bulk bc;
    g_cl.bulk_stats(bc);
    print_bulk("client(rx, drained)", bc);
    checkf(done, "lane: draining released the backpressure and the transfer completed (%ld of %u)",
           bc.rx_chunks, chunks_of(len));
    checkf(memcmp(g_sink, g_blob, len) == 0,
           "lane: the blob delivered through a lane that refused is byte-identical");
    checkf(bc.lane_evicted == 0, "lane: still nothing evicted at the end (%ld)", bc.lane_evicted);
    g_host.stop();
    g_cl.stop();
    return 0;
}

// ---- arm 0: the lane VOCABULARY ------------------------------------------------------------------
// Three lines, no sockets, and they are the statement the item is named for. `MSG_ORDER is never a
// victim` and `a chunk is never a victim` are the same sentence in mh_net_queue_policy.h, and this
// is where that is checked rather than described. It runs first because every arm below depends on
// it being true.
void lane_class_arm() {
    namespace QP = mh::net::queue_policy;
    printf("  -- the lane classes (mh_net_queue_policy.h; no sockets)\n");
    const unsigned char order[0x44] = {QP::MSG_ORDER};
    const unsigned char horizon[9]  = {QP::MSG_HORIZON};
    const unsigned char hz_plus[10] = {QP::MSG_HORIZON};
    checkf(QP::lane_of_game_frame(order, (int)sizeof(order)) == QP::lane::must_keep,
           "an MSG_ORDER frame is lane::must_keep");
    checkf(!QP::lane_evictable(QP::lane_of_game_frame(order, (int)sizeof(order))),
           "...and must_keep is NOT evictable -- D24's whole fix, restated as a lane");
    checkf(QP::lane_of_game_frame(horizon, (int)sizeof(horizon)) == QP::lane::supersedable,
           "a bare 9-byte MSG_HORIZON is lane::supersedable");
    checkf(QP::lane_evictable(QP::lane_of_game_frame(horizon, (int)sizeof(horizon))),
           "...and supersedable is the ONE evictable lane -- a vocabulary where nothing is evictable "
           "would pass every other check here");
    checkf(QP::lane_of_game_frame(hz_plus, (int)sizeof(hz_plus)) == QP::lane::must_keep,
           "a horizon that is one byte too long is protected, not evictable (the length test)");
    checkf(!QP::lane_evictable(QP::lane_of_bulk_chunk()), "a completed bulk CHUNK is never a victim");
}

// ---- arm 0b: the piece BOUND, with no sockets ----------------------------------------------------
// T0's `piece_decode` checks that `total` is the piece count `chunk_len` implies and that `piece_len`
// matches the frame -- but nothing there ties the length to the INDEX. So a peer can declare the last
// piece of a 16384-byte chunk (offset 15400, 984 bytes remaining) as a full 1100 and walk 116 bytes
// off the end of the assembly buffer. Authenticated is not the same as correct: the peer holding the
// session key is the peer whose build might be wrong. This arm is the proof that the receiver, not the
// codec, holds that bound -- and that it is a bound rather than a wall, because the well-formed run of
// the same chunk is accepted immediately after.
static mh::netudp::bulk::Channel g_bare; // 128 KiB, same reason the endpoints are static
static unsigned char             g_chunk[mh_net_proto::udp::CHUNK_MAX];

void piece_bounds_arm() {
    namespace U = mh_net_proto::udp;
    printf("  -- the piece bound (no sockets: a length that disagrees with its index)\n");
    for (uint32_t i = 0; i < U::CHUNK_MAX; ++i) g_chunk[i] = synth_byte(i);
    unsigned char sha[mh_net_proto::SHA256_LEN];
    mh_net_proto::sha256(g_chunk, U::CHUNK_MAX, sha);
    const uint16_t total = U::piece_count(U::CHUNK_MAX);
    checkf(total == 15, "bounds: a full chunk is 15 pieces (%u)", total);

    U::Piece bad;
    memset(&bad, 0, sizeof(bad));
    bad.chunk_id  = 0;
    bad.index     = (uint16_t)(total - 1);
    bad.total     = total;
    bad.chunk_len = U::CHUNK_MAX;
    memcpy(bad.chunk_sha, sha, sizeof(sha));
    bad.piece_seq = bad.index;
    bad.bytes     = g_chunk;
    bad.len       = U::PIECE_MAX; // the lie: the last piece is 984 bytes
    unsigned char buf[U::PIECE_HDR + U::PIECE_MAX];
    const size_t  n = U::piece_encode(bad, buf, sizeof(buf));
    checkf(n > 0, "bounds: the ENCODER accepts it -- it has no index/length rule, which is the point");

    Bulk b0, b1;
    g_bare.stats(b0);
    g_bare.on_piece(0, buf, n, GetTickCount());
    g_bare.stats(b1);
    checkf(b1.rx_mismatch == b0.rx_mismatch + 1,
           "bounds: the RECEIVER refuses the overlong last piece (mismatch %ld -> %ld)", b0.rx_mismatch,
           b1.rx_mismatch);
    checkf(b1.rx_chunks == 0, "bounds: nothing was admitted (%ld)", b1.rx_chunks);

    // ...and the well-formed run of the SAME chunk still assembles, so the bound is a bound.
    int encoded = 0;
    for (uint16_t i = 0; i < total; ++i) {
        U::Piece p;
        if (!U::piece_for(g_chunk, U::CHUNK_MAX, 0, i, i, p, sha)) continue;
        const size_t m = U::piece_encode(p, buf, sizeof(buf));
        if (m == 0) continue;
        ++encoded;
        g_bare.on_piece(0, buf, m, GetTickCount());
    }
    checkf(encoded == total, "bounds: all %u well-formed pieces encoded (%d)", total, encoded);
    g_bare.stats(b1);
    checkf(b1.rx_chunks == 1, "bounds: the well-formed chunk WAS admitted (%ld) and SHA-verified",
           b1.rx_chunks);
    checkf(b1.rx_sha_fail == 0, "bounds: ...with no SHA failure (%ld)", b1.rx_sha_fail);
    uint32_t id  = 0xffffffffu;
    int      len = (int)CHUNK_BYTES;
    checkf(g_bare.recv_chunk(&id, g_chunk, &len) == 1 && id == 0 && len == (int)U::CHUNK_MAX,
           "bounds: and it came out of the lane whole (chunk %u, %d bytes)", id, len);
}

// ---- mp:R1d: THE RELAYED PIECE STRIDE ---------------------------------------------------------------
//
// A relay leg wraps 34 bytes around every datagram, so a full-size T0 datagram is 1234 on the wire --
// above the 1200 bytes RFC 9000 picked as the size that crosses the internet without PMTU discovery.
// Exactly ONE path in this transport can reach that size: a channel-C piece. Channel A's K-redundant
// input frame caps at four 256-byte entries and channel B's records are tens of bytes, so lowering
// the piece stride on a relayed link is the whole of the fix -- and the arithmetic that says so is
// what this arm pins, because it is the arithmetic that stops being true when any of those sizes
// moves.
//
// The transfer itself is asserted too, and not only the sizes: the stride is on BOTH sides of the
// wire (the sender slices by it, the receiver bounds its offsets by it), so an arm that checked the
// numbers without moving a chunk through them would miss the half that actually breaks.
static mh::netudp::bulk::Channel g_relayed;

void relayed_stride_arm() {
    namespace U = mh_net_proto::udp;
    printf("  -- mp:R1d: the relayed piece stride (a leg-wrapped datagram must stay under 1200)\n");

    // THE SIZE CLAIM, from the constants rather than from a measurement, so it fails at build time
    // of the test rather than on a rig.
    const int direct  = (int)(U::HDR_SIZE + U::FRAME_HDR + U::PIECE_HDR + U::PIECE_MAX + U::TAG_SIZE);
    const int relayed = (int)(U::HDR_SIZE + U::FRAME_HDR + U::PIECE_HDR + U::PIECE_MAX_RELAYED +
                              U::TAG_SIZE);
    checkf(direct + mh::udprelay::LEG_OVERHEAD > (int)U::MAX_DATAGRAM,
           "R1d: the DIRECT stride really does not fit a leg (%d + %d = %d > %d) -- if this ever "
           "goes green on its own, the fix below is dead code",
           direct, mh::udprelay::LEG_OVERHEAD, direct + mh::udprelay::LEG_OVERHEAD,
           (int)U::MAX_DATAGRAM);
    checkf(relayed + mh::udprelay::LEG_OVERHEAD <= (int)U::MAX_DATAGRAM,
           "R1d: the RELAYED stride fits, envelope and all (%d + %d = %d <= %d)", relayed,
           mh::udprelay::LEG_OVERHEAD, relayed + mh::udprelay::LEG_OVERHEAD, (int)U::MAX_DATAGRAM);

    const uint16_t total = U::piece_count(U::CHUNK_MAX, U::PIECE_MAX_RELAYED);
    checkf(total == 16, "R1d: a full chunk is %u pieces at the relayed stride", total);
    checkf(total <= U::PIECE_TOTAL_MAX, "R1d: ...which the codec's ceiling admits (%u <= %u)", total,
           (unsigned)U::PIECE_TOTAL_MAX);
    checkf(total <= (uint16_t)mh::netudp::bulk::PIECES_PER_CHUNK,
           "R1d: ...and the piece_seq address space holds it (%u <= %u)", total,
           (unsigned)mh::netudp::bulk::PIECES_PER_CHUNK);

    // THE TRANSFER. Same chunk, the relayed stride on both ends, straight into a bare receiver.
    g_relayed.set_piece_max(U::PIECE_MAX_RELAYED);
    checkf(g_relayed.piece_max() == U::PIECE_MAX_RELAYED, "R1d: the channel took the stride");
    for (uint32_t i = 0; i < U::CHUNK_MAX; ++i) g_chunk[i] = synth_byte(i);
    unsigned char sha[mh_net_proto::SHA256_LEN];
    mh_net_proto::sha256(g_chunk, U::CHUNK_MAX, sha);

    unsigned char buf[U::PIECE_HDR + U::PIECE_MAX];
    int           encoded = 0;
    size_t        biggest = 0;
    for (uint16_t i = 0; i < total; ++i) {
        U::Piece p;
        if (!U::piece_for(g_chunk, U::CHUNK_MAX, 0, i, i, p, sha, U::PIECE_MAX_RELAYED)) continue;
        const size_t m = U::piece_encode(p, buf, sizeof(buf));
        if (m == 0) continue;
        if (m > biggest) biggest = m;
        ++encoded;
        g_relayed.on_piece(0, buf, m, GetTickCount());
    }
    checkf(encoded == total, "R1d: all %u pieces encoded at the relayed stride (%d)", total, encoded);
    const int on_wire =
        (int)(U::HDR_SIZE + U::FRAME_HDR + biggest + U::TAG_SIZE) + mh::udprelay::LEG_OVERHEAD;
    checkf(on_wire <= (int)U::MAX_DATAGRAM,
           "R1d: the biggest piece frame actually produced is %d B on a relay leg (<= %d)", on_wire,
           (int)U::MAX_DATAGRAM);

    Bulk b;
    g_relayed.stats(b);
    checkf(b.rx_chunks == 1, "R1d: the chunk assembled at the relayed stride (%ld)", b.rx_chunks);
    checkf(b.rx_mismatch == 0, "R1d: ...with no offset mismatch (%ld)", b.rx_mismatch);
    checkf(b.rx_sha_fail == 0, "R1d: ...and no SHA failure (%ld)", b.rx_sha_fail);
    uint32_t id  = 0xffffffffu;
    int      len = (int)CHUNK_BYTES;
    checkf(g_relayed.recv_chunk(&id, g_chunk, &len) == 1 && id == 0 && len == (int)U::CHUNK_MAX,
           "R1d: and it came out whole (chunk %u, %d bytes)", id, len);
    bool same = true;
    for (uint32_t i = 0; i < U::CHUNK_MAX && same; ++i)
        if (g_chunk[i] != synth_byte(i)) same = false;
    checkf(same, "R1d: byte for byte what was sent -- the offsets were read at OUR stride");

    // AND THE NEGATIVE: a peer sending at the DIRECT stride into a relayed receiver is refused as a
    // mismatch, not reassembled at the wrong offsets. Both ends of one link read `[net] relay`, so
    // this cannot happen to a real pair -- which is exactly why it has to be asserted rather than
    // assumed.
    U::Piece p2;
    checkf(U::piece_for(g_chunk, U::CHUNK_MAX, 1, 1, 17, p2, sha), "R1d: a direct-stride piece");
    const size_t m2 = U::piece_encode(p2, buf, sizeof(buf));
    Bulk         before, after;
    g_relayed.stats(before);
    g_relayed.on_piece(0, buf, m2, GetTickCount());
    g_relayed.stats(after);
    checkf(after.rx_chunks == before.rx_chunks,
           "R1d: a foreign-stride piece admits nothing (%ld -> %ld)", before.rx_chunks,
           after.rx_chunks);
}

// ---- arm 7: does a transfer DELAY CHANNEL A? ------------------------------------------------------
//
// mp:T2's fourth acceptance clause is a rig measurement -- frame-gap p95 from two determinism
// campaigns, one with a transfer running and one without. This is the OFFLINE half of that question,
// and it is a different claim with a different instrument, not a substitute: the rig measures the
// GAME's frame pacing end to end, and this measures the TRANSPORT's own delivery latency for a
// channel-A record while channel C is saturated.
//
// It can be precise where the rig cannot, because both peers are in one process: the sender stamps
// GetTickCount() INTO the record and the receiver subtracts it from its own, so the number is a real
// latency off one clock rather than an inter-arrival gap off two. The drain loop adds a small
// constant to every sample, and it adds the SAME constant to both phases, which is all a comparison
// needs.
//
// The bound is BULK_BURST pieces per endpoint tick. That is what makes the answer structural rather
// than lucky: a transfer cannot put more than 8 datagrams on the wire between two ticks, so a
// channel-A record queued behind one waits at most that burst, not a whole chunk.
constexpr int LAT_ROUNDS = 400;
long          g_lat[2][LAT_ROUNDS];
int           g_lat_n[2];

int lat_cmp(const void *a, const void *b) {
    const long x = *(const long *)a, y = *(const long *)b;
    return (x > y) - (x < y);
}

// One phase: `rounds` channel-A records, one per drain cycle, optionally with a bulk transfer
// running underneath. Fills g_lat[slot].
void pacing_phase(int slot, int base, bool with_bulk, uint32_t blob_len) {
    g_lat_n[slot] = 0;
    blob_fill(blob_len);
    sink_reset();
    if (!link_up(base, -1, 0)) {
        checkf(false, "pacing: the link came up (%s)", with_bulk ? "with transfer" : "idle");
        return;
    }
    // Let the handshake and the first HELLO settle, so neither phase measures the join.
    wait_for([&] { return g_cl.peer_count() == 1; }, 5000);
    Sleep(300);

    bool started = !with_bulk;
    for (int n = 0; n < LAT_ROUNDS; ++n) {
        if (with_bulk && !started && n == 20) started = g_host.bulk_send(1, g_blob, blob_len);
        unsigned char rec[40];
        memset(rec, 0, sizeof(rec));
        const DWORD t0 = GetTickCount();
        rec[0]         = (unsigned char)(n & 0xff);
        rec[1]         = (unsigned char)((n >> 8) & 0xff);
        memcpy(rec + 4, &t0, sizeof(t0));
        g_host.send(MH_NET_BROADCAST, rec, (int)sizeof(rec));
        // Drain for ~10 ms: both the channel-A records (whose latency is the measurement) and the
        // chunk lane (so the transfer is not measuring backpressure instead of pacing).
        const DWORD until = GetTickCount() + 10;
        for (;;) {
            int           sender = -1;
            unsigned char buf[2048];
            int           len = (int)sizeof(buf);
            while (g_cl.recv(&sender, buf, &len)) {
                if (len >= 8 && g_lat_n[slot] < LAT_ROUNDS) {
                    DWORD sent = 0;
                    memcpy(&sent, buf + 4, sizeof(sent));
                    g_lat[slot][g_lat_n[slot]++] = (long)(GetTickCount() - sent);
                }
                len = (int)sizeof(buf);
            }
            if (with_bulk) sink_drain(g_cl, blob_len);
            if ((long)(until - GetTickCount()) <= 0) break;
            Sleep(1);
        }
    }
    if (with_bulk) {
        Bulk b;
        g_host.bulk_stats(b);
        checkf(started, "pacing: the concurrent transfer started");
        checkf(b.tx_pieces > 200,
               "pacing: the transfer was REALLY RUNNING during the measurement (%ld pieces) -- "
               "without this the arm is the idle phase wearing a label",
               b.tx_pieces);
    }
    g_host.stop();
    g_cl.stop();
    qsort(g_lat[slot], (size_t)g_lat_n[slot], sizeof(long), lat_cmp);
}

long lat_pct(int slot, int pct) {
    if (g_lat_n[slot] <= 0) return -1;
    int i = (g_lat_n[slot] * pct) / 100;
    if (i >= g_lat_n[slot]) i = g_lat_n[slot] - 1;
    return g_lat[slot][i];
}

void pacing_arm(int base) {
    printf("  -- channel-A delivery latency, idle vs a 1 MiB transfer underneath (%d records each)\n",
           LAT_ROUNDS);
    pacing_phase(0, base, false, BLOB_MAX);
    pacing_phase(1, base + 5, true, BLOB_MAX);
    const long p50a = lat_pct(0, 50), p95a = lat_pct(0, 95), maxa = lat_pct(0, 100);
    const long p50b = lat_pct(1, 50), p95b = lat_pct(1, 95), maxb = lat_pct(1, 100);
    printf("     idle:      n=%d  p50 %ld ms  p95 %ld ms  max %ld ms\n", g_lat_n[0], p50a, p95a, maxa);
    printf("     +transfer: n=%d  p50 %ld ms  p95 %ld ms  max %ld ms\n", g_lat_n[1], p50b, p95b, maxb);
    checkf(g_lat_n[0] >= LAT_ROUNDS / 2 && g_lat_n[1] >= LAT_ROUNDS / 2,
           "pacing: both phases delivered a sample set (%d, %d of %d)", g_lat_n[0], g_lat_n[1],
           LAT_ROUNDS);
    // ONE ENDPOINT TICK of slack, which is the whole budget the rate limit spends. A transfer that
    // ignored BULK_BURST would put a whole chunk -- 15 datagrams -- in front of a channel-A record
    // and this would be tens of milliseconds, not one tick.
    checkf(p95b <= p95a + (long)mh::netudp::TICK_MS,
           "pacing: a concurrent transfer costs channel-A p95 at most one tick (%ld -> %ld ms, "
           "budget %ld)",
           p95a, p95b, p95a + (long)mh::netudp::TICK_MS);
}

} // namespace

int run_udpbulktest(int port) {
    // A base of its own: udploopbacktest holds 39560..39591 and udprelinktest takes the argument plus
    // 100, so this suite takes it plus 200 and spaces its five arms ten apart.
    const int base = port + 200;
    printf("=== udpbulktest (mp:T2: channel C, bulk reliable chunks) on ports %d.. ===\n", base);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    lane_class_arm();
    piece_bounds_arm();
    relayed_stride_arm(); // mp:R1d
    transfer_arm("clean", base, BLOB_MAX, 0, false, 60000);
    transfer_arm("5% loss", base + 10, BLOB_MAX, 50, false, 120000);
    resume_arm(base + 20);
    cold_arm(base + 30);
    lane_arm(base + 40);
    transfer_arm("synthetic (the `[net] bulk_selftest_mb` path)", base + 50, 64u * 1024u, 0, true,
                 30000);
    pacing_arm(base + 60);

    printf("=== udpbulktest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
