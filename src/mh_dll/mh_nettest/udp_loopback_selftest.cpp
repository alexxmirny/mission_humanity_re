//
// udp_loopback_selftest.cpp -- `net_selftest.exe udploopbacktest`: the UDP TRANSPORT (mp:T1).
//
// `udpwiretest` (mp:T0) proves the packet FORMAT against committed fixtures. This suite proves the
// thing built on top of it: a host and two clients that complete the connect handshake, exchange
// framed sender-tagged datagrams through the star, and deliver a lockstep stream IN ORDER WITH NO
// GAPS -- first on a clean link, then with 5% of inbound datagrams destroyed.
//
// ---- WHY IN-PROCESS, WHEN THE TCP SUITES SPAWN CHILDREN ------------------------------------------
//
// `selftest` / `selftest3` spawn copies of this executable because mh_net/net_transport.cpp is a
// file of globals: one process can only be one peer. That shape has a cost this item cannot pay --
// there is nowhere to inject loss. A child process talks to a real loopback socket, and 127.0.0.1
// does not drop packets; the acceptance clause "with 5% loss the run completes with zero stalls
// attributed to loss" would have no way to produce the 5%.
//
// So the UDP transport's core is an OBJECT (mh::netudp::Endpoint, udp_endpoint.cpp) and this suite
// owns three of them. Each gets its own socket on 127.0.0.1 and its own `set_rx_loss()` with its own
// seed, so the loss is deterministic, replayable, and applied where the network would apply it --
// before anything looks at the datagram.
//
//   THE LINK IS STILL REAL. These are three sockets exchanging real datagrams through the OS UDP
//   stack, not a mocked pipe: the handshake, the sealing, the sequence numbering and the reorder
//   window all run exactly as they do on the rig. What in-process buys is the loss dial and the
//   COUNTERS -- the rig can show that a run completed, and only this can show WHY it completed
//   (redundancy covered N losses, the retransmit covered M, the reassembler stalled K times).
//
// ---- THE FOUR ARMS -------------------------------------------------------------------------------
//
//   1. handshake     three peers, two joins. Both clients are admitted, the host sees two peers,
//                    each client learns a distinct host-assigned id.
//   2. ordered       the host broadcasts N numbered records and each client broadcasts its own; every
//                    peer receives every record exactly once, in the order it was sent, with no gap.
//                    Order is checked by CONTENT (a counter inside each record), not by arrival
//                    count -- a transport that delivered N records in the wrong order would pass a
//                    count test and desync a game.
//   3. lossy         the same, with 5% inbound loss on every peer. Same assertion, plus the one this
//                    item is really about: `gap_stalls == 0` and `repaired_by_k > 0` -- loss
//                    happened, redundancy absorbed it, and the reassembler never blocked past the
//                    retransmit timeout.
//   4. big           a record larger than one segment (the U28 Start payload is 456 bytes), so the
//                    fragmentation and reassembly path is exercised rather than assumed.
//
// A NOTE ON WHAT WOULD MAKE THIS VACUOUS: if the loss injection never fired, arm 3 would be arm 2
// wearing a label. So the arm asserts `dgram_dropped_sim > 0` before it believes anything else.
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

namespace {

using mh::netudp::Config;
using mh::netudp::Counters;
using mh::netudp::Endpoint;

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

void checkf(bool ok, const char *fmt, ...) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        char    b[400];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        printf("  FAIL: %s\n", b);
    }
}

// The test key. A fixed 32 bytes rather than mh_key.txt's: this suite must not depend on, or create,
// a key file next to whatever directory it happens to run in.
const unsigned char TEST_PSK[32] = {
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
    0x6c,
    0x6f,
    0x6f,
    0x70,
    0x62,
    0x61,
    0x63,
    0x6b,
    0x20,
    0x54,
    0x31,
    0x20,
    0x66,
    0x69,
    0x78,
    0x65,
    0x64,
    0x21,
};

// A record the game would send: 4 bytes of sender+counter plus filler, so ORDER is checkable from
// the contents. `who` is the sending player id, `n` the sender's own monotonic counter.
struct Rec {
    unsigned char b[512]; // room for a record LARGER than one 254-byte stream
                          // segment -- the U28 Start payload is 8 x 0x39 = 456 bytes, so
                          // fragmentation is a shipped path and not a hypothetical one
};
void rec_make(Rec &r, int who, int n, int fill_len) {
    memset(&r, 0, sizeof(r));
    r.b[0] = (unsigned char)who;
    r.b[1] = (unsigned char)(n & 0xff);
    r.b[2] = (unsigned char)((n >> 8) & 0xff);
    r.b[3] = (unsigned char)((n >> 16) & 0xff);
    for (int i = 4; i < fill_len; ++i) r.b[i] = (unsigned char)(0xa0 + ((n + i) & 0x0f));
}

void ep_log(void * /*ctx*/, const char *line) {
    if (getenv("MH_UDP_LOOPBACK_VERBOSE")) printf("    | %s\n", line);
}

void fill_cfg(Config &c, int role, int port, int player, unsigned short bind_port) {
    memset(&c, 0, sizeof(c));
    c.net.role = role;
    lstrcpynA(c.net.host, "127.0.0.1", sizeof(c.net.host));
    c.net.port        = port;
    c.net.player_id   = player;
    c.net.log         = 1;
    c.net.host_assign = 1; // the hand-clicked N-player shape, which is what the rig runs
    c.net.ping_ms     = 200;
    // -1 is the documented explicit "off" (0 means "use the default"). A suite that stops to
    // assert must not have a watchdog dropping its peers while it thinks.
    c.net.rx_timeout_ms = -1;
    c.redundancy        = 3;
    c.bind_port         = bind_port;
}

// Pump until `pred` holds or the budget expires. Returns true if it held. Sleeping rather than
// spinning because every endpoint's work happens on its own threads; this thread only waits.
template <class Pred>
bool wait_for(Pred pred, DWORD budget_ms) {
    const DWORD deadline = GetTickCount() + budget_ms;
    for (;;) {
        if (pred()) return true;
        if ((long)(deadline - GetTickCount()) <= 0) return pred();
        Sleep(5);
    }
}

// ---- the receiving ledger -------------------------------------------------------------------
// One per peer: what it got from each sender, in arrival order. The assertion is on the SEQUENCE,
// which is why the counter lives in the payload.
struct Ledger {
    int  next_from[8]; // the counter each sender's next record must carry
    int  got_from[8];
    bool gap;
    bool disorder;
    void reset() {
        memset(this, 0, sizeof(*this));
    }
    void absorb(int sender, const unsigned char *b, int len) {
        if (sender < 0 || sender > 7 || len < 4) {
            disorder = true;
            return;
        }
        const int n = (int)b[1] | ((int)b[2] << 8) | ((int)b[3] << 16);
        if ((int)b[0] != sender) disorder = true; // the transport tagged it with the wrong sender
        if (n != next_from[sender]) {
            // The only two ways this fires are a REORDER and a GAP, and a byte stream cannot produce
            // either without the reassembler being wrong -- which is the whole claim of the item.
            if (n > next_from[sender]) gap = true;
            else disorder = true;
            next_from[sender] = n; // resynchronise so one fault is not reported N times
        }
        next_from[sender] += 1;
        got_from[sender] += 1;
    }
};

// THE ENDPOINTS LIVE HERE, not on a stack and not one set per arm. Each carries ~4 MB of stream
// rings (8 peer slots x a 1024-segment reorder window x two directions), which is fine as BSS a page
// at a time and is not fine as a local. Three is the most any arm needs, and placement-new at the
// top of each arm is what makes reuse equivalent to a fresh construction.
Endpoint g_ep[3];
Ledger   lh, l1, l2;

void drain(Endpoint &ep, Ledger &led) {
    for (;;) {
        int           sender = -1;
        unsigned char buf[2048];
        int           len = (int)sizeof(buf);
        if (!ep.recv(&sender, buf, &len)) return;
        led.absorb(sender, buf, len);
    }
}

// ---- one scenario ------------------------------------------------------------------------------
// Three endpoints, `rounds` broadcasts each, optional per-peer inbound loss. Returns 0 on success.
int scenario(const char *name, int base_port, unsigned loss_pm, int rounds, int fill_len) {
    printf("  -- %s (loss %u/1000, %d rounds x 3 peers, %d-byte records)\n", name, loss_pm, rounds,
           fill_len);

    Endpoint &host = g_ep[0], &c1 = g_ep[1], &c2 = g_ep[2];
    new (&host) Endpoint(); // placement-new so each scenario starts from a zeroed endpoint
    new (&c1) Endpoint();
    new (&c2) Endpoint();
    lh.reset();
    l1.reset();
    l2.reset();

    Config ch, cc1, cc2;
    fill_cfg(ch, 0, base_port, 0, (unsigned short)base_port);
    fill_cfg(cc1, 1, base_port, 1, (unsigned short)(base_port + 1));
    fill_cfg(cc2, 1, base_port, 1, (unsigned short)(base_port + 2));
    host.set_log(ep_log, nullptr);
    c1.set_log(ep_log, nullptr);
    c2.set_log(ep_log, nullptr);
    // Distinct seeds: three peers dropping the SAME datagram indices would be a correlated loss
    // pattern no network produces, and would make the star's relay leg look healthier than it is.
    host.set_rx_loss(loss_pm, 0x1234abcdu);
    c1.set_rx_loss(loss_pm, 0x51ee7711u);
    c2.set_rx_loss(loss_pm, 0x0badc0deu);

    check("host started", host.start(ch, TEST_PSK, true));
    check("client 1 started", c1.start(cc1, TEST_PSK, true));
    check("client 2 started", c2.start(cc2, TEST_PSK, true));

    // ---- arm 1: the handshake ------------------------------------------------------------------
    const bool joined = wait_for([&] { return host.peer_count() == 2; }, 8000);
    checkf(joined, "%s: both clients completed the connect handshake (host sees %d peer(s))", name,
           host.peer_count());
    if (!joined) {
        host.stop();
        c1.stop();
        c2.stop();
        return 1;
    }
    const bool ids = wait_for([&] { return c1.id_assigned() && c2.id_assigned(); }, 4000);
    checkf(ids, "%s: both clients were assigned an id by the host", name);
    const int id1 = c1.local_player_id(), id2 = c2.local_player_id();
    checkf(id1 != id2 && id1 >= 1 && id2 >= 1, "%s: host-assigned ids are distinct and >= 1 (%d, %d)",
           name, id1, id2);

    // ---- arms 2-4: the ordered stream ----------------------------------------------------------
    // Each peer broadcasts its own numbered run. Drains happen between sends so the inbound rings
    // never approach their cap -- this suite is about ordering, not about D24's eviction policy,
    // which netqueuetest owns.
    for (int n = 0; n < rounds; ++n) {
        Rec r;
        rec_make(r, 0, n, fill_len);
        host.send(MH_NET_BROADCAST, r.b, fill_len);
        rec_make(r, id1, n, fill_len);
        c1.send(MH_NET_BROADCAST, r.b, fill_len);
        rec_make(r, id2, n, fill_len);
        c2.send(MH_NET_BROADCAST, r.b, fill_len);
        drain(host, lh);
        drain(c1, l1);
        drain(c2, l2);
        if ((n % 16) == 15) Sleep(1); // let the timer threads ack and retransmit
    }
    // Everything sent; wait for the tail. A lossy run needs the retransmit timer, which is why the
    // budget is seconds rather than milliseconds.
    const bool all = wait_for(
        [&] {
            drain(host, lh);
            drain(c1, l1);
            drain(c2, l2);
            return lh.got_from[id1] >= rounds && lh.got_from[id2] >= rounds &&
                   l1.got_from[0] >= rounds && l1.got_from[id2] >= rounds &&
                   l2.got_from[0] >= rounds && l2.got_from[id1] >= rounds;
        },
        15000);

    checkf(all, "%s: every peer received every record (host %d/%d, c1 %d/%d, c2 %d/%d)", name,
           lh.got_from[id1] + lh.got_from[id2], rounds * 2, l1.got_from[0] + l1.got_from[id2],
           rounds * 2, l2.got_from[0] + l2.got_from[id1], rounds * 2);
    checkf(!lh.gap && !l1.gap && !l2.gap, "%s: no GAP in any peer's stream", name);
    checkf(!lh.disorder && !l1.disorder && !l2.disorder, "%s: no REORDER in any peer's stream", name);

    Counters kh, k1, k2;
    host.counters(kh);
    c1.counters(k1);
    c2.counters(k2);
    const long stalls  = kh.gap_stalls + k1.gap_stalls + k2.gap_stalls;
    const long simlost = kh.dgram_dropped_sim + k1.dgram_dropped_sim + k2.dgram_dropped_sim;
    const long byk     = kh.repaired_by_k + k1.repaired_by_k + k2.repaired_by_k;
    const long byrto   = kh.repaired_by_rto + k1.repaired_by_rto + k2.repaired_by_rto;
    printf("     datagrams tx %ld rx %ld | synthetic loss %ld | repaired: K %ld, retransmit %ld | "
           "gap stalls %ld (worst %ld ms) | mac-fail %ld replay %ld malformed %ld\n",
           kh.dgram_tx + k1.dgram_tx + k2.dgram_tx, kh.dgram_rx + k1.dgram_rx + k2.dgram_rx, simlost,
           byk, byrto, stalls,
           (kh.gap_ms_worst > k1.gap_ms_worst ? kh.gap_ms_worst : k1.gap_ms_worst),
           kh.mac_fail + k1.mac_fail + k2.mac_fail, kh.replay_drop + k1.replay_drop + k2.replay_drop,
           kh.malformed + k1.malformed + k2.malformed);

    checkf(kh.mac_fail + k1.mac_fail + k2.mac_fail == 0, "%s: no authentication failures", name);
    checkf(kh.malformed + k1.malformed + k2.malformed == 0, "%s: no malformed frames", name);
    if (loss_pm > 0) {
        // NON-VACUITY FIRST. Without this the whole lossy arm could be a clean run mislabelled.
        checkf(simlost > 0, "%s: the loss injection actually fired (%ld datagrams destroyed)", name,
               simlost);
        checkf(byk > 0, "%s: the K-redundancy window repaired at least one loss (%ld)", name, byk);
        // THE ITEM'S CLAUSE, in the form this suite can state it: no reassembler block outlived the
        // retransmit timeout, so nothing the game would see as a stall is attributable to loss.
        checkf(stalls == 0, "%s: ZERO stalls attributed to loss -- redundancy K covered it (%ld "
                            "stalls, %ld repaired by retransmit)",
               name, stalls, byrto);
    } else {
        checkf(simlost == 0, "%s: the clean arm really was clean", name);
        checkf(byrto == 0, "%s: a clean link needed no retransmit", name);
        checkf(stalls == 0, "%s: a clean link never stalled the reassembler", name);
    }

    host.stop();
    c1.stop();
    c2.stop();
    return 0;
}

} // namespace

// The `refuses` arm: an endpoint whose PSK disagrees must never be admitted. It is here rather than
// in a wire test because the refusal is the HANDSHAKE's, not the packet format's -- the intruder's
// datagrams are well-formed, they simply do not authenticate, and the observable is that the host's
// peer count never moves.
static int wrong_key_arm(int base_port) {
    printf("  -- wrong key (the negative case: a well-formed peer with the wrong secret)\n");
    Endpoint &host = g_ep[0], &bad = g_ep[1];
    new (&host) Endpoint();
    new (&bad) Endpoint();
    Config ch, cb;
    fill_cfg(ch, 0, base_port, 0, (unsigned short)base_port);
    fill_cfg(cb, 1, base_port, 1, (unsigned short)(base_port + 1));
    host.set_log(ep_log, nullptr);
    bad.set_log(ep_log, nullptr);

    unsigned char other[32];
    memcpy(other, TEST_PSK, sizeof(other));
    other[0] ^= 0xff;

    check("host started (wrong-key arm)", host.start(ch, TEST_PSK, true));
    check("intruder started", bad.start(cb, other, true));
    // Give it longer than the handshake budget, so "not admitted" means refused rather than pending.
    Sleep(5000);
    check("an intruder holding a different key is NOT admitted", host.peer_count() == 0);
    check("...and the intruder does not believe it joined", bad.peer_count() == 0);
    host.stop();
    bad.stop();
    return 0;
}

int run_udploopbacktest(int argc, char **argv) {
    (void)argc;
    (void)argv;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    printf("=== udploopbacktest: host + 2 clients over 127.0.0.1 (mp:T1) ===\n");
    // Distinct port bases per scenario: a UDP socket lingers for no time at all, but a datagram in
    // flight from the previous scenario reaching the next one's host would be an invisible source of
    // "malformed" counts, and this suite asserts that count is zero.
    scenario("clean", 39560, 0, 400, 40);
    scenario("5% loss", 39570, 50, 400, 40);
    scenario("5% loss, FRAGMENTED records (456 B = 2 segments)", 39580, 50, 120, 456);
    wrong_key_arm(39590);

    printf("=== udploopbacktest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
