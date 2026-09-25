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

// The delay relay's sockets need the same ICMP-reset suppression the endpoint's do (udp_endpoint.cpp
// explains the value); a local name so this file does not depend on which SDK header carries it.
#define SIO_UDP_CONNRESET_T _WSAIOW(IOC_VENDOR, 12)
#include <mmsystem.h> // timeBeginPeriod
#pragma comment(lib, "winmm.lib")

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

// ---- drop / eviction witnesses, read off the endpoints' own log lines ----------------------------
// Every arm below asserts on WHY a link went, not just on whether: a survival arm that passed because
// the link dropped and re-formed, or a drop arm that passed on the wrong rule, would both be lies.
static volatile LONG g_drop_full    = 0; // the pre-T4b tripwire (must never fire again)
static volatile LONG g_drop_backlog = 0; // T4b's size bound
static volatile LONG g_drop_stall   = 0; // T4b's kept drop: the frontier stopped moving
static volatile LONG g_drop_silent  = 0; // the R-live silence watchdog
static volatile LONG g_drop_any     = 0;
static volatile LONG g_evict_unsafe = 0; // D24's ring destroyed a non-supersedable frame
static volatile LONG g_bp_lines     = 0; // `back-pressure released` lines
static void          wit_reset() {
    InterlockedExchange(&g_drop_full, 0);
    InterlockedExchange(&g_drop_backlog, 0);
    InterlockedExchange(&g_drop_stall, 0);
    InterlockedExchange(&g_drop_silent, 0);
    InterlockedExchange(&g_drop_any, 0);
    InterlockedExchange(&g_evict_unsafe, 0);
    InterlockedExchange(&g_bp_lines, 0);
}
static void wit_log(void * /*ctx*/, const char *line) {
    if (strstr(line, " dropped -- ")) InterlockedIncrement(&g_drop_any);
    if (strstr(line, "ran a full window ahead of the peer's acknowledgements"))
        InterlockedIncrement(&g_drop_full);
    if (strstr(line, "the outbound backlog overflowed")) InterlockedIncrement(&g_drop_backlog);
    if (strstr(line, "the peer acknowledged nothing for")) InterlockedIncrement(&g_drop_stall);
    if (strstr(line, "no data from peer within the link timeout")) InterlockedIncrement(&g_drop_silent);
    if (strstr(line, "NOTHING IN IT WAS SAFE TO DROP")) InterlockedIncrement(&g_evict_unsafe);
    if (strstr(line, "back-pressure released after")) InterlockedIncrement(&g_bp_lines);
    ep_log(nullptr, line);
}

// ---- the DELAY RELAY (mp:T4b / mp:T5) -------------------------------------------------------------
//
// The two arms below are claims about a LONG round trip, and 127.0.0.1 has none. This is the in-process
// twin of tools/net_shim.py --udp: one socket facing the client, one facing the host, every datagram
// held for `one_way_ms` in each direction. What net_shim cannot give an offline suite, this adds:
//
//   stall(ms, cap)  NOTHING moves in either direction for `ms` -- mp:T5's 3.4 s machine-wide freeze,
//                   seen from the path -- and while it lasts each direction keeps at most `cap` bytes
//                   (a socket receive buffer) and destroys the rest. When it ends, what was kept goes
//                   at once, which is the burst a frozen box releases.
//
// FIFO on one queue is exact: the delay is constant and a stall only postpones, so the head is always
// the next datagram due.
struct Held {
    int64_t       due_us;
    int           dir; // 0 = client -> host, 1 = host -> client
    int           len;
    unsigned char data[1536];
};
static const int QCAP = 8192;
static Held      g_q[QCAP];

static int64_t now_us() {
    static LARGE_INTEGER f = {};
    if (f.QuadPart == 0) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (int64_t)(t.QuadPart * 1000000 / f.QuadPart);
}

struct DelayRelay {
    SOCKET               down, up;
    sockaddr_in          client, target;
    volatile LONG        have_client;
    volatile LONG        running;
    HANDLE               th;
    int64_t              delay_us;
    CRITICAL_SECTION     cs;
    int                  qhead, qcount;
    volatile LONG64      stall_until_us;
    long                 stall_kept[2];
    long                 stall_cap;
    volatile LONG        dropped;
    volatile LONG        fwd;
    int64_t              next_free_us[2];
    static const int64_t BW_BYTES_PER_S = 1250000; // 10 Mbit/s each way -- a modest home uplink

    bool start(int listen_port, int target_port, DWORD one_way_ms) {
        memset(this, 0, sizeof(*this));
        InitializeCriticalSection(&cs);
        delay_us = (int64_t)one_way_ms * 1000;
        down     = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        up       = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        int big  = 8 * 1024 * 1024; // the relay itself must not be where loss happens
        setsockopt(down, SOL_SOCKET, SO_RCVBUF, (const char *)&big, sizeof(big));
        setsockopt(up, SOL_SOCKET, SO_RCVBUF, (const char *)&big, sizeof(big));
        DWORD off = 0, got = 0;
        WSAIoctl(down, SIO_UDP_CONNRESET_T, &off, sizeof(off), nullptr, 0, &got, nullptr, nullptr);
        WSAIoctl(up, SIO_UDP_CONNRESET_T, &off, sizeof(off), nullptr, 0, &got, nullptr, nullptr);
        sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port        = htons((u_short)listen_port);
        if (bind(down, (sockaddr *)&a, sizeof(a)) == SOCKET_ERROR) return false;
        a.sin_port = 0;
        if (bind(up, (sockaddr *)&a, sizeof(a)) == SOCKET_ERROR) return false;
        memset(&target, 0, sizeof(target));
        target.sin_family      = AF_INET;
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        target.sin_port        = htons((u_short)target_port);
        running                = 1;
        th                     = CreateThread(nullptr, 0, thunk, this, 0, nullptr);
        return th != nullptr;
    }
    void stop() {
        InterlockedExchange(&running, 0);
        if (th) {
            WaitForSingleObject(th, 5000);
            CloseHandle(th);
            th = nullptr;
        }
        closesocket(down);
        closesocket(up);
        DeleteCriticalSection(&cs);
    }
    void stall(DWORD ms, long cap_bytes) {
        EnterCriticalSection(&cs);
        stall_cap     = cap_bytes;
        stall_kept[0] = stall_kept[1] = 0;
        InterlockedExchange64(&stall_until_us, now_us() + (int64_t)ms * 1000);
        LeaveCriticalSection(&cs);
    }
    void push(int dir, const unsigned char *d, int n) {
        EnterCriticalSection(&cs);
        const bool stalled = now_us() < stall_until_us;
        if (stalled && stall_kept[dir] + n > stall_cap) {
            InterlockedIncrement(&dropped); // the frozen box's buffer is full: gone
        } else if (qcount >= QCAP) {
            InterlockedIncrement(&dropped);
        } else {
            if (stalled) stall_kept[dir] += n;
            Held &h  = g_q[(qhead + qcount) % QCAP];
            h.due_us = now_us() + delay_us;
            h.dir    = dir;
            h.len    = n;
            memcpy(h.data, d, (size_t)n);
            ++qcount;
        }
        LeaveCriticalSection(&cs);
    }
    // Send everything due. Returns microseconds until the next datagram is due (or a short poll).
    int64_t flush() {
        for (;;) {
            EnterCriticalSection(&cs);
            if (qcount == 0) {
                LeaveCriticalSection(&cs);
                return 2000;
            }
            Held         &h    = g_q[qhead];
            const int64_t t    = now_us();
            int64_t       gate = stall_until_us > h.due_us ? stall_until_us : h.due_us;
            if (next_free_us[h.dir] > gate) gate = next_free_us[h.dir];
            if (t < gate) {
                LeaveCriticalSection(&cs);
                return gate - t;
            }
            // A LINK RATE, which loopback does not have: each direction serialises at BW_BYTES_PER_S,
            // so a window released at once arrives over milliseconds rather than in one burst no
            // real path could deliver. (Without it the burst arm measured the RECEIVER's 256-slot ring
            // under CPU load, not the sender this suite is about.)
            next_free_us[h.dir] = (t > next_free_us[h.dir] ? t : next_free_us[h.dir]) +
                                  (int64_t)h.len * 1000000 / BW_BYTES_PER_S;
            if (h.dir == 0) sendto(up, (const char *)h.data, h.len, 0, (sockaddr *)&target, sizeof(target));
            else if (have_client)
                sendto(down, (const char *)h.data, h.len, 0, (sockaddr *)&client, sizeof(client));
            InterlockedIncrement(&fwd);
            qhead = (qhead + 1) % QCAP;
            --qcount;
            LeaveCriticalSection(&cs);
        }
    }
    static DWORD WINAPI thunk(LPVOID p) {
        ((DelayRelay *)p)->loop();
        return 0;
    }
    void loop() {
        unsigned char buf[2048];
        while (InterlockedCompareExchange(&running, 1, 1)) {
            int64_t wait = flush();
            if (wait > 1000) wait = 1000; // poll at least every millisecond
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(down, &rd);
            FD_SET(up, &rd);
            timeval tv;
            tv.tv_sec  = 0;
            tv.tv_usec = (long)wait;
            if (select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;
            if (FD_ISSET(down, &rd)) {
                sockaddr_in from;
                int         fl = sizeof(from);
                const int   n  = recvfrom(down, (char *)buf, sizeof(buf), 0, (sockaddr *)&from, &fl);
                if (n > 0) {
                    client = from;
                    InterlockedExchange(&have_client, 1);
                    push(0, buf, n);
                }
            }
            if (FD_ISSET(up, &rd)) {
                const int n = recvfrom(up, (char *)buf, sizeof(buf), 0, nullptr, nullptr);
                if (n > 0) push(1, buf, n);
            }
        }
    }
};
static DelayRelay g_relay;

// The client's application drain, on its own thread and SPINNING: a burst of bundled small frames
// arrives far faster than a Sleep(5) poll empties the 256-slot inbound ring (MP D24), and an arm about
// the SENDER must not fail on the receiver's ring. `wit_log` still reports an unsafe eviction if one
// happens anyway, so it cannot pass silently either.
struct Drainer {
    Endpoint           *ep;
    Ledger             *led;
    volatile LONG       running;
    HANDLE              th;
    static DWORD WINAPI thunk(LPVOID p) {
        Drainer *d = (Drainer *)p;
        while (InterlockedCompareExchange(&d->running, 1, 1)) {
            int           sender = -1;
            unsigned char buf[2048];
            int           len = (int)sizeof(buf);
            if (d->ep->recv(&sender, buf, &len)) d->led->absorb(sender, buf, len);
            else Sleep(1); // 1 ms under timeBeginPeriod(1), which the game process also runs
        }
        return 0;
    }
    void start(Endpoint &e, Ledger &l) {
        ep      = &e;
        led     = &l;
        running = 1;
        th      = CreateThread(nullptr, 0, thunk, this, 0, nullptr);
        // NOT a raised priority: a HIGHEST-priority spinner starved the relay thread (Sleep(0) yields
        // only to equal priority) and the arm measured a relay forwarding 170 datagrams/s -- a
        // congestion collapse of the test's own making. timeBeginPeriod(1) (run_udploopbacktest)
        // makes the Sleep(1) above a millisecond, which is what the game's hires_clock gives it too.
    }
    void stop() {
        InterlockedExchange(&running, 0);
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
    }
};

// A host and one client joined THROUGH the relay. The client dials the relay's port; the host binds
// its own. Returns false (and says why) if the handshake does not complete.
static bool relay_pair(const char *arm, int base, DWORD one_way_ms, int rx_timeout_ms) {
    Endpoint &host = g_ep[0], &c1 = g_ep[1];
    new (&host) Endpoint();
    new (&c1) Endpoint();
    wit_reset();
    Config ch, cc;
    fill_cfg(ch, 0, base, 0, (unsigned short)base);
    fill_cfg(cc, 1, base + 1, 1, (unsigned short)(base + 2)); // net.port = the relay
    ch.net.rx_timeout_ms = rx_timeout_ms;
    cc.net.rx_timeout_ms = rx_timeout_ms;
    host.set_log(wit_log, nullptr);
    c1.set_log(wit_log, nullptr);
    if (!g_relay.start(base + 1, base, one_way_ms)) {
        checkf(false, "%s: the delay relay could not bind :%d", arm, base + 1);
        return false;
    }
    checkf(host.start(ch, TEST_PSK, true), "%s: host started", arm);
    checkf(c1.start(cc, TEST_PSK, true), "%s: client started", arm);
    const bool joined = wait_for([&] { return host.peer_count() == 1 && c1.id_assigned(); }, 10000);
    checkf(joined, "%s: the client joined through the %u ms relay", arm, (unsigned)one_way_ms);
    if (!joined) {
        host.stop();
        c1.stop();
        g_relay.stop();
        return false;
    }
    // Let the RTT estimator see the path: pings every 200 ms (fill_cfg), and the RTO follows SRTT
    // only once there are samples. Six round trips is well past the first few.
    Sleep(6 * 2 * one_way_ms + 400);
    return true;
}

static void relay_close() {
    g_ep[0].stop();
    g_ep[1].stop();
    g_relay.stop();
}

// ---- ARM B: the burst (mp:T4b's first clause) ------------------------------------------------------
//
// > 2x the send window of small frames, written in one go, over a 360 ms round trip: the pre-T4b build
// dropped the link on the 1025th segment (the `full window` rule mp:T4 hit in the field shape). Now the
// link must SURVIVE and every frame must arrive, in order, exactly once. Non-vacuity: the backlog must
// actually have been used (a burst that fit the window proves nothing about back-pressure). And the
// mp:T5 half: on a CLEAN 360 ms path the retransmit timer must not fire for segments whose
// acknowledgements are merely in flight -- the constant 200 ms RTO re-sent essentially every one.
static int burst_arm(int base) {
    const int FRAMES = 2 * 1024 + 500; // > 2x SEG_WINDOW frames, one 21-byte frame per segment pre-T4b
    printf("  -- burst (mp:T4b: %d small frames at once over a 360 ms round trip)\n", FRAMES);
    if (!relay_pair("burst", base, 180, -1)) return 1;
    Endpoint &host = g_ep[0], &c1 = g_ep[1];
    l1.reset();
    Drainer dr;
    dr.start(c1, l1);

    // PHASE 1 -- STEADY (mp:T5's storm, in isolation). 200 frames at 100/s: nothing queues anywhere,
    // so every acknowledgement is one round trip plus the ack cadence away and a retransmit is
    // spurious by construction. The constant 200 ms RTO re-sent essentially every segment here
    // (every one is still unacknowledged 200 ms after it left); the measured RTO re-sends none.
    const int STEADY = 200;
    Counters  s0;
    host.counters(s0);
    const DWORD st0 = GetTickCount();
    for (int n = 0; n < STEADY;) {
        const int due = (int)((GetTickCount() - st0) / 10);
        while (n < STEADY && n <= due) {
            Rec r;
            rec_make(r, 0, n++, 9);
            r.b[0] = 0;
            host.send(MH_NET_BROADCAST, r.b, 9);
        }
        Sleep(2);
    }
    wait_for([&] { return l1.got_from[0] >= STEADY || g_drop_any != 0; }, 5000);
    Sleep(600); // the last acknowledgements land
    Counters s1;
    host.counters(s1);
    const long st_segs = s1.seg_tx - s0.seg_tx - (s1.rto_sent - s0.rto_sent);
    const long st_rto  = s1.rto_sent - s0.rto_sent;
    printf("     steady 100 frames/s: %ld new segments, %ld retransmits\n", st_segs, st_rto);
    checkf(st_rto * 20 <= st_segs, "burst/steady: a clean 360 ms path did not retransmit what was merely "
                                   "in flight (%ld re-sends for %ld segments; the constant 200 ms RTO re-sent "
                                   "~every one)",
           st_rto, st_segs);

    // PHASE 1b -- the same question of CHANNEL C (mp:T5: the rig host logged 681 pieces, 591 of them
    // repeats). 128 KiB of synthetic chunks on the clean 360 ms path; the pieces' fast-retransmit
    // guard was a 30 ms constant, so the first acknowledgement after any piece "refuted" it.
    {
        const uint32_t BULK = 128u * 1024u;
        check("burst/bulk: the transfer armed", host.bulk_send(c1.local_player_id(), nullptr, BULK));
        static unsigned char cb[mh::netudp::bulk::CHUNK_BYTES];
        uint32_t             got = 0, bad = 0;
        wait_for(
            [&] {
                for (;;) {
                    uint32_t id  = 0;
                    int      len = (int)sizeof(cb);
                    if (!c1.bulk_recv(&id, cb, &len)) break;
                    for (int i = 0; i < len; ++i)
                        if (cb[i] != mh::netudp::bulk::synth_byte(id * mh::netudp::bulk::CHUNK_BYTES + (uint32_t)i)) {
                            ++bad;
                            break;
                        }
                    ++got;
                }
                return got >= BULK / mh::netudp::bulk::CHUNK_BYTES;
            },
            15000);
        mh::netudp::bulk::Stats bs;
        host.bulk_stats(bs);
        printf("     bulk 128 KiB: %u chunks (bad %u), %ld pieces, %ld repeats\n", got, bad, bs.tx_pieces,
               bs.tx_retx);
        checkf(got == BULK / mh::netudp::bulk::CHUNK_BYTES && bad == 0, "burst/bulk: every chunk verified (%u)",
               got);
        checkf(bs.tx_retx * 20 <= bs.tx_pieces, "burst/bulk: a clean 360 ms path repeated (almost) no piece "
                                                "(%ld of %ld; the 30 ms/250 ms constants repeated most)",
               bs.tx_retx, bs.tx_pieces);
    }

    // PHASE 2 -- THE BURST.
    Counters k0;
    host.counters(k0);
    for (int n = STEADY; n < STEADY + FRAMES; ++n) {
        Rec r;
        rec_make(r, 0, n, 9); // a 9-byte payload: a horizon advert's size (21 B with the WireHdr)
        r.b[0] = 0;           // ...but not its TYPE, so D24's ring may never evict one
        host.send(MH_NET_BROADCAST, r.b, 9);
    }
    const bool all = wait_for([&] { return l1.got_from[0] >= STEADY + FRAMES || g_drop_any != 0; }, 20000);
    dr.stop();
    Counters k1;
    host.counters(k1);
    const long segs = k1.seg_tx - k0.seg_tx - (k1.rto_sent - k0.rto_sent);
    const long rto  = k1.rto_sent - k0.rto_sent;
    const long bund = k1.bp_bundled_segs - k0.bp_bundled_segs;
    const long epis = k1.bp_episodes - k0.bp_episodes;
    printf("     delivered %d/%d | new segments %ld (bundled from the backlog %ld, episodes %ld, peak "
           "%ld B) | retransmits %ld | relay forwarded %ld dropped %ld\n",
           l1.got_from[0] - STEADY, FRAMES, segs, bund, epis, k1.bp_peak_bytes, rto, (long)g_relay.fwd,
           (long)g_relay.dropped);
    checkf(g_drop_any == 0 && host.peer_count() == 1, "burst: the link SURVIVED (drops %ld, full-window %ld)",
           (long)g_drop_any, (long)g_drop_full);
    checkf(all && l1.got_from[0] == STEADY + FRAMES, "burst: every frame delivered (%d/%d)",
           l1.got_from[0] - STEADY, FRAMES);
    checkf(!l1.gap && !l1.disorder, "burst: in order, no gap (gap %d disorder %d)", (int)l1.gap,
           (int)l1.disorder);
    checkf(g_evict_unsafe == 0, "burst: the receiver's ring evicted nothing it should not have");
    checkf(epis > 0 && bund > 0, "burst: NON-VACUOUS -- the window filled and the backlog was used "
                                 "(episodes %ld, bundled segments %ld)",
           epis, bund);
    checkf(bund * 4 < FRAMES, "burst: the backlog BUNDLED (%ld segments carried the queued frames)", bund);
    // Not zero, and not the storm check (PHASE 1 is): the relay SERIALISES the window at 10 Mbit/s,
    // so the tail's acknowledgements come back later than the idle-path round trip the estimator saw
    // and its first timeout is spurious until the backoff catches it (measured 176-384 re-sends for
    // 1150). This bound only catches a COLLAPSE -- more re-sends than new segments.
    checkf(rto <= segs, "burst: no retransmit collapse (%ld re-sends for %ld new segments)", rto, segs);
    relay_close();
    return 0;
}

// ---- ARM C: stall-then-bulk at a 500 ms round trip (mp:T5 reproduced offline) ---------------------
//
// The rig run, rebuilt: a 462 065-byte channel-C transfer to the client, lobby-rate stream traffic,
// and then a 3.4 s freeze of the PATH (nothing moves; each direction keeps 64 KiB and loses the rest)
// while the host keeps writing -- more than a window's worth, which is what the pre-T4b build dropped
// on ("outbound stream ran a full window ahead"). The link must survive, every frame arrive in order,
// and the transfer complete with every chunk verified.
static int stall_arm(int base) {
    printf("  -- stall-then-bulk (mp:T5: 500 ms round trip, a 3.4 s freeze, a 462 KB transfer)\n");
    if (!relay_pair("stall", base, 250, -1)) return 1;
    Endpoint &host = g_ep[0], &c1 = g_ep[1];
    l1.reset();
    const uint32_t BULK = 462065;
    check("stall: the bulk transfer armed", host.bulk_send(c1.local_player_id(), nullptr, BULK));
    // The client's application: drain frames on one thread, chunks here.
    Drainer dr;
    dr.start(c1, l1);
    uint32_t             chunks_ok = 0, chunks_bad = 0;
    const uint32_t       chunks_want = (BULK + mh::netudp::bulk::CHUNK_BYTES - 1) / mh::netudp::bulk::CHUNK_BYTES;
    static unsigned char cbuf[mh::netudp::bulk::CHUNK_BYTES];
    auto                 drain_chunks = [&] {
        for (;;) {
            uint32_t id  = 0;
            int      len = (int)sizeof(cbuf);
            if (!c1.bulk_recv(&id, cbuf, &len)) return;
            bool ok = true;
            for (int i = 0; i < len; ++i)
                if (cbuf[i] != mh::netudp::bulk::synth_byte(id * mh::netudp::bulk::CHUNK_BYTES + (uint32_t)i)) {
                    ok = false;
                    break;
                }
            if (ok) ++chunks_ok;
            else ++chunks_bad;
        }
    };
    // Lobby-rate stream traffic: 400 150-byte frames a second (~400 segments/s, a little above the
    // ~245/s the rig host was writing when it dropped), for 0.5 s before the freeze, through the 3.4 s
    // freeze, and 1.5 s after it -- ~2150 segments, over a window's worth while nothing is acknowledged.
    int         sent  = 0;
    const DWORD t0    = GetTickCount();
    bool        froze = false;
    while (GetTickCount() - t0 < 5400) {
        if (!froze && GetTickCount() - t0 >= 500) {
            g_relay.stall(3400, 64 * 1024);
            froze = true;
        }
        // Paced by the clock, not by Sleep(5): without timeBeginPeriod a Sleep(5) is a ~15.6 ms tick,
        // and a count-per-iteration loop then writes a third of the intended traffic (measured: 682
        // frames, too few to fill the window during the freeze -- a vacuous arm).
        const int due = (int)((GetTickCount() - t0) * 2 / 5); // 400 frames/s
        while (sent < due) {
            Rec r;
            rec_make(r, 0, sent++, 150);
            r.b[0] = 0;
            host.send(MH_NET_BROADCAST, r.b, 150);
        }
        drain_chunks();
        Sleep(5);
    }
    const bool all = wait_for(
        [&] {
            drain_chunks();
            return (l1.got_from[0] >= sent && chunks_ok + chunks_bad >= chunks_want) || g_drop_any != 0;
        },
        30000);
    dr.stop();
    Counters kh, kc;
    host.counters(kh);
    c1.counters(kc);
    printf("     frames %d/%d | chunks verified %u/%u (bad %u) | host: bundled segments %ld, episodes "
           "%ld, peak %ld B, retransmits %ld | client: delivery paused %ld time(s) | relay dropped %ld\n",
           l1.got_from[0], sent, chunks_ok, chunks_want, chunks_bad, kh.bp_bundled_segs, kh.bp_episodes,
           kh.bp_peak_bytes, kh.rto_sent, kc.rx_pauses, (long)g_relay.dropped);
    checkf(g_relay.dropped > 0, "stall: NON-VACUOUS -- the freeze destroyed datagrams (%ld)", (long)g_relay.dropped);
    checkf(kh.bp_episodes > 0, "stall: NON-VACUOUS -- the window filled during the freeze");
    checkf(g_drop_any == 0 && host.peer_count() == 1,
           "stall: the link SURVIVED the freeze (drops %ld, full-window %ld)", (long)g_drop_any,
           (long)g_drop_full);
    checkf(all && l1.got_from[0] == sent, "stall: every frame delivered (%d/%d)", l1.got_from[0], sent);
    checkf(!l1.gap && !l1.disorder, "stall: in order, no gap");
    checkf(g_evict_unsafe == 0, "stall: the receiver's ring evicted nothing it should not have");
    checkf(chunks_ok == chunks_want && chunks_bad == 0, "stall: the transfer completed, every chunk verified "
                                                        "(%u/%u, bad %u)",
           chunks_ok, chunks_want, chunks_bad);
    relay_close();
    return 0;
}

// ---- ARM E: the receiver's ring is full of frames it may not destroy (mp:T4b, receiver half) -------
//
// The deterministic form of what the stall arm hit by timing: the application stops draining, the
// peer sends more non-evictable frames than the 256-slot ring holds. Pre-T4b the ring DESTROYED a real
// input (D24's loud line) and the stream went on; now delivery pauses, the frontier stops, the sender
// queues, and when the application drains again every frame arrives, in order.
static int ring_full_arm(int base) {
    const int FRAMES = 700;
    printf("  -- inbound ring full (mp:T4b: %d non-evictable frames at a peer that is not draining)\n", FRAMES);
    if (!relay_pair("ring", base, 1, -1)) return 1;
    Endpoint &host = g_ep[0], &c1 = g_ep[1];
    l1.reset();
    for (int n = 0; n < FRAMES; ++n) {
        Rec r;
        rec_make(r, 0, n, 40);
        r.b[0] = 0;
        host.send(MH_NET_BROADCAST, r.b, 40);
    }
    Sleep(1500); // everything the window allows has arrived and been refused or delivered
    Counters kc;
    c1.counters(kc);
    checkf(g_evict_unsafe == 0, "ring: NOTHING was destroyed while the application was not draining");
    checkf(kc.rx_pauses > 0, "ring: delivery PAUSED on the full ring (%ld)", kc.rx_pauses);
    const bool all = wait_for(
        [&] {
            drain(c1, l1);
            return l1.got_from[0] >= FRAMES || g_drop_any != 0;
        },
        10000);
    checkf(all && l1.got_from[0] == FRAMES && !l1.gap && !l1.disorder,
           "ring: once drained, every frame arrived in order (%d/%d, gap %d)", l1.got_from[0], FRAMES,
           (int)l1.gap);
    checkf(g_drop_any == 0, "ring: the link survived");
    relay_close();
    return 0;
}

// ---- ARM D: a peer that stops acknowledging is still dropped (mp:T4b's second clause) --------------
//
// TODAY'S BOUND is the link timeout, `rx_timeout_ms` (10 s by default): the time the silence watchdog
// has always given a dead peer. Two shapes, each with its own rule:
//   D1  the peer goes SILENT (nothing at all reaches the host) -- the watchdog drops it, as before.
//   D2  the peer keeps PINGING but never acknowledges -- the one shape the watchdog cannot see, and
//       the one the pre-T4b full-window rule was the only guard against. T4b's kept drop must fire
//       within the same bound, by name, and not early.
// Both at a 2 s timeout so the arm is quick; the bound is the configured value, whatever it is. And
// with the watchdog OFF (-1, the debugger setting) the size bound still holds: D3 fills window +
// backlog on a mute peer and expects the named overflow drop.
static int stops_acking_arm(int base) {
    const int TIMEOUT = 2000;
    printf("  -- stops acknowledging (mp:T4b: still dropped within the %d ms link timeout)\n", TIMEOUT);
    // D2 first: pings flow, acknowledgements do not.
    if (!relay_pair("mute", base, 20, TIMEOUT)) return 1;
    Endpoint &host = g_ep[0], &c1 = g_ep[1];
    Drainer   dr;
    l1.reset();
    dr.start(c1, l1);
    c1.set_ack_mute(true);
    Sleep(100); // acks already in flight land; from here the frontier cannot move
    const DWORD t0     = GetTickCount();
    int         n      = 0;
    DWORD       t_drop = 0;
    while (GetTickCount() - t0 < (DWORD)TIMEOUT + 1500) {
        Rec r;
        rec_make(r, 0, n++, 9);
        r.b[0] = 0;
        host.send(MH_NET_BROADCAST, r.b, 9);
        if (!t_drop && g_drop_any) t_drop = GetTickCount() - t0;
        Sleep(20);
    }
    dr.stop();
    printf("     mute peer: dropped after %u ms (stall rule %ld, silence rule %ld)\n", (unsigned)t_drop,
           (long)g_drop_stall, (long)g_drop_silent);
    checkf(g_drop_stall == 1 && g_drop_silent == 0,
           "mute: dropped by the KEPT rule (frontier stopped), not by the silence watchdog (stall %ld, "
           "silence %ld)",
           (long)g_drop_stall, (long)g_drop_silent);
    checkf(t_drop != 0 && t_drop <= (DWORD)TIMEOUT + 500, "mute: ...within the link timeout (%u ms, bound %d)",
           (unsigned)t_drop, TIMEOUT);
    checkf(t_drop >= (DWORD)TIMEOUT - 200, "mute: ...and not early (%u ms)", (unsigned)t_drop);
    relay_close();

    // D1: silent.
    if (!relay_pair("silent", base + 10, 20, TIMEOUT)) return 1;
    g_ep[0].set_rx_loss(1000, 0x7e4d00d5u);
    const DWORD s0     = GetTickCount();
    DWORD       s_drop = 0;
    while (GetTickCount() - s0 < (DWORD)TIMEOUT + 1500 && !s_drop) {
        unsigned char rec[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
        g_ep[0].send(MH_NET_BROADCAST, rec, 9);
        if (g_drop_any) s_drop = GetTickCount() - s0;
        Sleep(20);
    }
    checkf(s_drop != 0 && s_drop <= (DWORD)TIMEOUT + 500, "silent: dropped within the link timeout (%u ms)",
           (unsigned)s_drop);
    relay_close();

    // D3: watchdog off, peer mute, the writer keeps going -- the size bound.
    if (!relay_pair("overflow", base + 20, 20, -1)) return 1;
    g_ep[1].set_ack_mute(true);
    Sleep(100);
    int sent = 0;
    // A window of 21-byte frames (one per segment) plus 256 KiB of them queued: 1024 + 12483.
    for (; sent < 20000 && !g_drop_any; ++sent) {
        unsigned char rec[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
        g_ep[0].send(MH_NET_BROADCAST, rec, 9);
    }
    printf("     watchdog off, mute peer: dropped after %d frames (backlog rule %ld)\n", sent,
           (long)g_drop_backlog);
    checkf(g_drop_backlog == 1, "overflow: with the watchdog off the BACKLOG bound drops a mute peer");
    checkf(sent > 2 * 1024, "overflow: ...but only past the backlog, not at the old full window (%d frames)",
           sent);
    relay_close();
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
    // mp:T4b / mp:T5 -- 39650.. is clear of every other suite's range (udprelinktest takes the
    // default port + 100 = 39600..39603, udpbulktest +200). The game runs under timeBeginPeriod(1)
    // (mh.dll's hires_clock); these arms time a relay and a drainer, so they do too.
    timeBeginPeriod(1);
    burst_arm(39650);
    stall_arm(39660);
    stops_acking_arm(39670);
    ring_full_arm(39695); // 39695..39697: stops_acking takes 39670..39692, udpbulktest 39700..
    timeEndPeriod(1);

    printf("=== udploopbacktest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

// ---- uqmatchtest: TWO MATCHES OVER ONE LIVE UDP LINK (mp:U41e) -----------------------------------
//
// mh_net_udp.dll applies D24's queue policy (`is_evictable`, in inbound_has_room) but until this item
// its MH_Net_QueueMatchBoundary was a bare no-op and it kept no per-match refused/evicted/high-water
// counters at all (found by wave 5 lane G, dead-ends G305) -- so mp:U41c's "0 refusals" on the shipped
// udp transport was never actually MEASURED. This is the udp twin of net_selftest.exe's `qmatchtest`
// (net_selftest.cpp), proving the WIRING: that udp_transport.cpp's MH_Net_QueueMatchBoundary export,
// and the mh::netudp::Endpoint::queue_match_boundary() it now calls, really invoke
// mh::net::queue_policy::lane_queue::reset_counters() -- NOT the epoch mechanism itself, which
// `queuetest` (net_queue_selftest.cpp) already proves once, generically, for whichever transport
// reuses the shared template.
//
// Forces a REAL overflow first (host never drains, so lane H's 4096-slot cap is exceeded), which is
// what makes the rollup's `evicted` count non-vacuous, then asserts the epoch-based reset proof:
// MUTATION-CHECKED the same way qmatchtest is -- comment out
// Endpoint::queue_match_boundary()'s `m_lanes.reset_counters()` call (or the `++epoch_` inside
// lane_queue::reset_counters() itself) and the two epoch checks below red; nothing about the flood's
// timing or size can make them pass by accident.
int run_uqmatchtest() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    printf("=== uqmatchtest: two matches over one live udp link (mp:U41e) ===\n");

    Endpoint &host = g_ep[0], &c1 = g_ep[1];
    new (&host) Endpoint();
    new (&c1) Endpoint();
    Config ch, cc;
    fill_cfg(ch, 0, 39710, 0, (unsigned short)39710); // clear of every other suite's fixed range
    fill_cfg(cc, 1, 39710, 1, (unsigned short)39711);
    host.set_log(ep_log, nullptr);
    c1.set_log(ep_log, nullptr);
    checkf(host.start(ch, TEST_PSK, true), "uqmatch: host started");
    checkf(c1.start(cc, TEST_PSK, true), "uqmatch: client started");
    const bool joined = wait_for([&] { return host.peer_count() == 1 && c1.id_assigned(); }, 8000);
    checkf(joined, "uqmatch: the client joined the host");
    if (!joined) {
        host.stop();
        c1.stop();
        printf("=== uqmatchtest: %d checks, %d failures ===\n", g_checks, g_fails);
        return 1;
    }

    int      d = 0, h = 0;
    long     e = 0, r = 0;
    unsigned ep0 = 0, ep1 = 0, ep2 = 0;

    // The epoch BEFORE either boundary, so "did it move" has a real baseline.
    host.queue_counters_for_test(&d, &h, &e, &r, &ep0);

    // MATCH 1: the client floods the host with bare horizon adverts (byte[0] = MSG_HORIZON, the ONLY
    // frame shape lane H admits) that the host never drains. inbound_has_room() only guards lane M's
    // capacity (mp:U41e's note on it), so this traffic is never back-pressured -- exactly the point of
    // lane H -- and past QUEUE_CAP_H frames it genuinely overflows, evicting its own head.
    const int FLOOD = mh::netudp::QUEUE_CAP_H + 200;
    for (int n = 0; n < FLOOD; ++n) {
        unsigned char rec[mh::net::queue_policy::BARE_HORIZON_LEN];
        memset(rec, 0, sizeof(rec));
        rec[0] = mh::net::queue_policy::MSG_HORIZON;
        c1.send(MH_NET_BROADCAST, rec, (int)sizeof(rec));
    }
    const bool overflowed = wait_for(
        [&] {
            host.queue_counters_for_test(&d, &h, &e, &r, nullptr);
            return e > 0;
        },
        8000);
    checkf(overflowed, "uqmatch: match 1's flood actually overflowed lane H (evicted %ld) -- otherwise "
                       "the rollup's evicted count below would be untested",
           e);
    host.queue_counters_for_test(&d, &h, &e, &r, nullptr);
    printf("[uqmatch] match 1: depth %d high-water %d evicted %ld refused %ld\n", d, h, e, r);
    // high-water saturates AT the cap (eviction keeps depth <= cap by construction) -- `evicted > 0`
    // above is what proves the flood actually overran it, not the high-water reading alone.
    checkf(h == mh::netudp::QUEUE_CAP_H, "uqmatch: match 1's high-water %d did not reach lane H's cap %d",
           h, mh::netudp::QUEUE_CAP_H);

    // THE BOUNDARY.
    host.queue_match_boundary();
    host.queue_counters_for_test(&d, &h, &e, &r, &ep1);
    printf("[uqmatch] after boundary: depth %d high-water %d evicted %ld refused %ld epoch %u\n", d, h,
           e, r, ep1);
    checkf(e == 0 && r == 0, "uqmatch: evicted/refused not restarted by the boundary (%ld/%ld)", e, r);
    // THE MUTATION TARGET: if queue_match_boundary()'s reset_counters() call (or its ++epoch_) is
    // skipped, ep1 stays at ep0 and this reds -- unlike depth/high-water, nothing about the flood's
    // timing can make it pass by accident.
    checkf(ep1 == ep0 + 1, "uqmatch: epoch did not advance across the boundary (%u -> %u, want +1) -- "
                           "reset_counters() did not run",
           ep0, ep1);
    checkf(host.peer_count() == 1, "uqmatch: the link survived the match boundary");

    // MATCH 2 over the SAME link: one more frame, one more boundary, one more epoch step -- proving
    // this is a genuine per-call marker and not a one-shot latch.
    unsigned char one[mh::net::queue_policy::BARE_HORIZON_LEN];
    memset(one, 0, sizeof(one));
    one[0] = mh::net::queue_policy::MSG_HORIZON;
    c1.send(MH_NET_BROADCAST, one, (int)sizeof(one));
    Sleep(200);
    host.queue_match_boundary();
    host.queue_counters_for_test(&d, &h, &e, &r, &ep2);
    checkf(ep2 == ep1 + 1,
           "uqmatch: epoch did not advance across the second boundary (%u -> %u, want +1)", ep1, ep2);

    host.stop();
    c1.stop();
    printf("=== uqmatchtest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
