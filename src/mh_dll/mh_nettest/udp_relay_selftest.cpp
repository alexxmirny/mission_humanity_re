//
// udp_relay_selftest.cpp -- `net_selftest.exe udprelaytest` (tracker mp:R3e; arm D mp:R4a).
//
// THE HOST TUNNEL'S PER-PEER SLOTS, RELEASED. A host's relay tunnel (udp_relay.cpp) holds three
// tables keyed by the relay handle -- one loopback socket per remote peer, one punch, one pair-key
// set -- and MAX_REMOTE (8) of each. Until R3e a slot, once used, was held until the tunnel stopped:
// a lobby that eight distinct peers had joined and left over its life refused the ninth a socket
// and a punch, and the ninth join silently never completed. The rig cannot stage this cheaply (nine
// sequential joins through the harness is nine lobby walks, and the lane pool has no headroom --
// TL-LANEPOOL), so it is staged here over the REAL tunnel with a STAND-IN relay and a stand-in
// endpoint, both a loopback socket the suite owns:
//
//   * the stand-in RELAY answers the tunnel's HELLO with a WELCOME and then plays every fake
//     client: a DATA from handle k is the announcement a host learns a client from, a CAND from k
//     is what starts the punch toward it. It never forwards anything anywhere.
//   * the stand-in ENDPOINT is the loopback port the tunnel dials as `game_port`. A datagram arriving
//     there FROM a fresh 127.0.0.1 port is the proof handle k was given a loopback socket of its own.
//   * the PROBE TARGET is the candidate the relay offers for every fake client. A PROBE arriving
//     there naming dst=k is the proof handle k was given a punch.
//   * the OWNERSHIP PREDICATE (udp_relay.h set_peer_known) is the suite's own table: a peer is
//     "joined" when its loopback port is put in and "gone" when it is taken out, which is exactly
//     what Endpoint::knows_addr says for a peer that was admitted and then dropped.
//
// The arms, in the order they run (one tunnel, one clock, ~13 s of wall time for the two sweeps):
//
//   A  eight joins: eight distinct loopback sockets, eight punches. A NINTH while all eight are
//      still held gets neither -- the cap is real, and this is the shape the ninth join had for
//      every lobby before R3e. (Reverting the fix turns arm B into this arm.)
//   B  the eight leave (the endpoint no longer holds them); after the sweep the ninth and tenth
//      join and get a socket and a punch each, and the tunnel logged exactly eight releases.
//   C  a punch with no peer behind it -- a CAND from a handle that never sends DATA, which is what
//      a PSK holder probing under made-up handles looks like -- is released by the orphan rule,
//      and the slot it held is a slot a real peer then gets: with 9 and 10 still live, six more
//      join to a full table of eight, and the next one is refused as in A.
//
// What the suite does NOT claim: nothing here promotes (the probe target never answers), and no
// pair key is ever minted (the stand-in endpoint never replies, so no grant crosses) -- both are
// udppunchtest's and the rig's. This is the ownership rule and the tables, nothing more.
//
//   D  (mp:R4a) THE PROTOCOL LEVEL. The tunnel is restarted against the same stand-in, which now
//      answers the HELLO in each of the shapes a relay can have: no level (a 4-byte WELCOME, the
//      pre-R4a wire shape), a lower level, the same level, a higher one, a leg version this build
//      does not speak, and an op above its table. The HELLO itself is read for the level it
//      carries (18 bytes still, bit 0 still set -- what makes it additive against an old relay).
//      What is asserted is the tunnel's REPORT: the one named `relay protocol <theirs> < <ours>`
//      line + the notice for the two "behind" shapes, silence for the matching pair, an info line
//      and no notice for the newer relay. The notice's mh.dll half has no mh.dll here, so what is
//      read is the tunnel's own "not deliverable in this build" line for the same text -- the rig
//      scenario relay_stale_notice reads the pixels.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../mh_net_udp/udp_relay.h"
#include "mh_net_proto/net_crypto.h"

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

// ---- the leg codec, the stand-in's half ------------------------------------------------------------
// MIRRORED from udp_relay.cpp / leg.rs, as udp_relay.cpp mirrors the endpoint's constants: the
// header layout, the ops this suite speaks and the truncated-HMAC tag. Getting one wrong here is not
// a wrong answer but no answer -- the tunnel drops what it cannot verify -- so a red run of this
// suite after a leg change is "the mirror drifted" before it is "the tunnel broke".
constexpr uint8_t LEG_MAGIC_VER = 0x51;
constexpr int     LEG_HDR       = 18;
constexpr int     LEG_TAG       = 16;
constexpr uint8_t OP_HELLO      = 1;
constexpr uint8_t OP_WELCOME    = 2;
constexpr uint8_t OP_DATA       = 3;
constexpr uint8_t OP_CAND       = 12;
constexpr uint8_t OP_PROBE      = 13;
constexpr uint8_t OP_LAST       = 14;
// mp:R4a -- mirrored from leg.rs / udp_relay.cpp: the level this build was made against, and where
// it rides in HELLO's flags byte. Bumped in lockstep with those two; a drift here reads as arm D
// failing its "the HELLO carries the level" check before anything else.
constexpr uint8_t  PROTOCOL_LEVEL      = 1;
constexpr int      HELLO_LEVEL_SHIFT   = 1;
constexpr uint8_t  HELLO_FLAG_PEER_KEY = 0x01;
constexpr uint16_t HOST_HANDLE         = 100;
constexpr uint32_t ROOM                = 0x1234;

uint8_t g_psk[mh_net_proto::KEY_LEN];
uint8_t g_leg_key[mh_net_proto::KEY_LEN];

void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}
uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

// mp:R4a -- arm D's knob: the first header byte the stand-in stamps (0x52 = a leg version this
// build does not speak).
uint8_t g_magic_ver = LEG_MAGIC_VER;

int leg_encode(uint8_t op, uint16_t src, uint16_t dst, uint64_t seq, const uint8_t *payload,
               int plen, uint8_t *out) {
    out[0] = g_magic_ver;
    out[1] = op;
    put16(out + 2, src);
    put16(out + 4, dst);
    for (int i = 0; i < 4; ++i) out[6 + i] = (uint8_t)((ROOM >> (8 * i)) & 0xff);
    for (int i = 0; i < 8; ++i) out[10 + i] = (uint8_t)((seq >> (8 * i)) & 0xff);
    if (plen > 0) memcpy(out + LEG_HDR, payload, (size_t)plen);
    uint8_t full[mh_net_proto::SHA256_LEN];
    mh_net_proto::hmac_sha256(g_leg_key, mh_net_proto::KEY_LEN, out, (size_t)(LEG_HDR + plen), full);
    memcpy(out + LEG_HDR + plen, full, LEG_TAG);
    return LEG_HDR + plen + LEG_TAG;
}

// Verified under the deployment key -- the only key a pair that never exchanged a grant has.
bool leg_open(const uint8_t *pkt, int n, uint8_t *op, uint16_t *src, uint16_t *dst) {
    if (n < LEG_HDR + LEG_TAG || pkt[0] != LEG_MAGIC_VER) return false;
    uint8_t full[mh_net_proto::SHA256_LEN];
    mh_net_proto::hmac_sha256(g_leg_key, mh_net_proto::KEY_LEN, pkt, (size_t)(n - LEG_TAG), full);
    if (memcmp(full, pkt + n - LEG_TAG, LEG_TAG) != 0) return false;
    *op  = pkt[1];
    *src = get16(pkt + 2);
    *dst = get16(pkt + 4);
    return true;
}

// ---- the three stand-in sockets --------------------------------------------------------------------

SOCKET loopback_socket(unsigned short *port) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (sockaddr *)&a, sizeof(a)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    int alen = sizeof(a);
    getsockname(s, (sockaddr *)&a, &alen);
    *port     = ntohs(a.sin_port);
    DWORD off = 0, got = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof(off), nullptr, 0,
             &got, nullptr, nullptr);
    u_long nb = 1; // non-blocking, so a ready socket can be drained to empty
    ioctlsocket(s, FIONBIO, &nb);
    return s;
}

struct Rig {
    SOCKET         relay, ep, probe; // the stand-in relay, the stand-in endpoint, the probe target
    unsigned short relay_port, ep_port, probe_port;
    sockaddr_in    tunnel; // where the tunnel's leg socket is, learned from its HELLO
    bool           have_tunnel;
    uint64_t       seq;

    // The ownership table: the loopback ports the "endpoint" currently holds a peer for.
    CRITICAL_SECTION cs;
    unsigned short   known[64];
    int              known_n;

    // What the tunnel logged, for the release lines.
    int released;
    int refused; // "no loopback socket left"

    // mp:R4a, arm D. How the stand-in answers a HELLO (`welcome_len` 4 = no level, 5 = with
    // `welcome_level`), what the last HELLO's flags byte and length were, and what the tunnel
    // logged about the level.
    int     welcome_len;
    uint8_t welcome_level;
    bool    send_unknown_op; // after the WELCOME, one authenticated op above the table
    int     hello_len;
    uint8_t hello_flags;
    int     line_older;      // `relay protocol <t> < <o>`
    int     line_newer;      // `relay protocol <t> > <o>`
    int     line_legver;     // `relay protocol leg v<x> != v<y>`
    int     line_unknown_op; // `above this build's table`
    int     notices;         // the notice was attempted (here: `not deliverable in this build`)
    char    notice_text[64];
};

Rig g_rig;

bool rig_known(void *ctx, const sockaddr_in &a) {
    Rig *r = (Rig *)ctx;
    if (a.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) return false;
    const unsigned short port = ntohs(a.sin_port);
    bool                 hit  = false;
    EnterCriticalSection(&r->cs);
    for (int i = 0; i < r->known_n && !hit; ++i)
        if (r->known[i] == port) hit = true;
    LeaveCriticalSection(&r->cs);
    return hit;
}

void rig_log(void * /*ctx*/, const char *line) {
    printf("  | %s\n", line);
    EnterCriticalSection(&g_rig.cs);
    if (strstr(line, "released") != nullptr && strstr(line, "mp:R3e") != nullptr) ++g_rig.released;
    if (strstr(line, "no loopback socket left") != nullptr) ++g_rig.refused;
    // mp:R4a -- the needles are the registered ones (tools/data/log_formats.json, net.relay_protocol_*).
    if (strstr(line, "relay protocol leg v") != nullptr) ++g_rig.line_legver;
    else if (strstr(line, "relay protocol ") != nullptr && strstr(line, " < ") != nullptr) ++g_rig.line_older;
    else if (strstr(line, "relay protocol ") != nullptr && strstr(line, " > ") != nullptr) ++g_rig.line_newer;
    if (strstr(line, "above this build's table") != nullptr) ++g_rig.line_unknown_op;
    const char *nt = strstr(line, "browser notice not deliverable in this build: ");
    if (nt != nullptr) {
        ++g_rig.notices;
        lstrcpynA(g_rig.notice_text, nt + lstrlenA("browser notice not deliverable in this build: "),
                  sizeof(g_rig.notice_text));
        char *tail = strstr(g_rig.notice_text, " (mp:R4a)");
        if (tail) *tail = 0;
    }
    LeaveCriticalSection(&g_rig.cs);
}

void rig_join_known(unsigned short port) {
    EnterCriticalSection(&g_rig.cs);
    if (g_rig.known_n < 64) g_rig.known[g_rig.known_n++] = port;
    LeaveCriticalSection(&g_rig.cs);
}

void rig_forget_all() {
    EnterCriticalSection(&g_rig.cs);
    g_rig.known_n = 0;
    LeaveCriticalSection(&g_rig.cs);
}

int rig_released() {
    EnterCriticalSection(&g_rig.cs);
    const int n = g_rig.released;
    LeaveCriticalSection(&g_rig.cs);
    return n;
}

void relay_send(uint8_t op, uint16_t src, uint16_t dst, const uint8_t *payload, int plen) {
    if (!g_rig.have_tunnel) return;
    uint8_t   pkt[1300];
    const int n = leg_encode(op, src, dst, g_rig.seq++, payload, plen, pkt);
    sendto(g_rig.relay, (const char *)pkt, n, 0, (const sockaddr *)&g_rig.tunnel,
           sizeof(g_rig.tunnel));
}

// One pass over the three sockets, `wait_ms` at most, DRAINING each one that is ready: the tunnel
// probes every held peer four times a second, so a reader that took one datagram per pass would
// fall behind the probe target inside arm A. Answers the HELLO; records what the endpoint and the
// probe target received. `ep_from` gets the source port of the last datagram at the endpoint (0 if
// none); `saw_probe` is set if a PROBE naming dst=`want` reached the probe target.
void rig_pump(DWORD wait_ms, unsigned short *ep_from, uint16_t want, bool *saw_probe) {
    if (ep_from) *ep_from = 0;
    if (saw_probe) *saw_probe = false;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(g_rig.relay, &rd);
    FD_SET(g_rig.ep, &rd);
    FD_SET(g_rig.probe, &rd);
    timeval tv;
    tv.tv_sec  = (long)(wait_ms / 1000);
    tv.tv_usec = (long)((wait_ms % 1000) * 1000);
    if (select(0, &rd, nullptr, nullptr, &tv) <= 0) return;
    uint8_t     buf[1400];
    sockaddr_in from;
    int         flen;
    if (FD_ISSET(g_rig.relay, &rd)) {
        for (;;) {
            flen        = sizeof(from);
            const int n = recvfrom(g_rig.relay, (char *)buf, (int)sizeof(buf), 0, (sockaddr *)&from,
                                   &flen);
            if (n <= 0) break;
            uint8_t  op;
            uint16_t src, dst;
            if (!leg_open(buf, n, &op, &src, &dst)) continue;
            if (op == OP_HELLO) {
                g_rig.tunnel      = from;
                g_rig.have_tunnel = true;
                // mp:R4a -- what the HELLO said: its length (18, whatever the level) and flags.
                g_rig.hello_len   = n - LEG_HDR - LEG_TAG;
                g_rig.hello_flags = g_rig.hello_len >= 2 ? buf[LEG_HDR + 1] : 0;
                uint8_t w[5];
                put16(w, HOST_HANDLE);
                put16(w + 2, 0);
                w[4] = g_rig.welcome_level;
                relay_send(OP_WELCOME, 0, HOST_HANDLE, w, g_rig.welcome_len == 5 ? 5 : 4);
                if (g_rig.send_unknown_op) relay_send(OP_LAST + 1, 0, HOST_HANDLE, nullptr, 0);
            }
            // PING, CAND (the tunnel publishing its own candidates), everything else: drained.
        }
    }
    if (FD_ISSET(g_rig.ep, &rd)) {
        for (;;) {
            flen        = sizeof(from);
            const int n = recvfrom(g_rig.ep, (char *)buf, (int)sizeof(buf), 0, (sockaddr *)&from, &flen);
            if (n <= 0) break;
            if (ep_from) *ep_from = ntohs(from.sin_port);
        }
    }
    if (FD_ISSET(g_rig.probe, &rd)) {
        for (;;) {
            flen        = sizeof(from);
            const int n = recvfrom(g_rig.probe, (char *)buf, (int)sizeof(buf), 0, (sockaddr *)&from,
                                   &flen);
            if (n <= 0) break;
            uint8_t  op;
            uint16_t src, dst;
            if (leg_open(buf, n, &op, &src, &dst) && op == OP_PROBE && src == HOST_HANDLE &&
                dst == want && saw_probe)
                *saw_probe = true;
        }
    }
}

// Wait up to `budget_ms` for a specific thing, pumping meanwhile.
bool wait_ep(DWORD budget_ms, unsigned short *port) {
    const DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < budget_ms) {
        unsigned short p = 0;
        rig_pump(50, &p, 0, nullptr);
        if (p != 0) {
            *port = p;
            return true;
        }
    }
    return false;
}

bool wait_probe(DWORD budget_ms, uint16_t handle) {
    const DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < budget_ms) {
        bool saw = false;
        rig_pump(50, nullptr, handle, &saw);
        if (saw) return true;
    }
    return false;
}

void pump_for(DWORD ms) {
    const DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < ms) rig_pump(50, nullptr, 0, nullptr);
}

// The candidate list a fake client offers: one v4 candidate, the probe target. The wire form is
// udp_punch.cpp's cands_encode: count u8, then per candidate fam u8 | flags u8 | port u16 LE |
// addr (4 bytes for v4), with trailing bytes REFUSED -- so the length is exact.
int cand_payload(uint8_t *out) {
    out[0] = 1;
    out[1] = 4; // FAM_V4
    out[2] = 0;
    put16(out + 3, g_rig.probe_port);
    const uint32_t lb = htonl(INADDR_LOOPBACK);
    memcpy(out + 5, &lb, 4);
    return 1 + 1 + 1 + 2 + 4;
}

// "Handle k joins": the announcement DATA, then the CAND. Returns the loopback port the tunnel
// presented k to the endpoint from (0 = no socket), and whether a probe for k reached the target.
struct Join {
    unsigned short port;
    bool           punched;
};

Join join(uint16_t k, bool expect) {
    Join j;
    j.port    = 0;
    j.punched = false;
    uint8_t t0[40];
    memset(t0, 0x5a, sizeof(t0));
    t0[0] = (uint8_t)k;
    relay_send(OP_DATA, k, HOST_HANDLE, t0, (int)sizeof(t0));
    (void)wait_ep(expect ? 1500 : 400, &j.port);
    uint8_t   c[32];
    const int clen = cand_payload(c);
    relay_send(OP_CAND, k, HOST_HANDLE, c, clen);
    j.punched = wait_probe(expect ? 1500 : 400, k);
    return j;
}

bool distinct(const unsigned short *p, int n) {
    for (int i = 0; i < n; ++i)
        for (int k = i + 1; k < n; ++k)
            if (p[i] == p[k] || p[i] == 0) return false;
    return n > 0 && p[n - 1] != 0;
}

} // namespace

int run_udprelaytest() {
    printf("=== udprelaytest (mp:R3e: a host tunnel's per-peer slots are released when the endpoint "
           "drops the peer; mp:R4a: the relay protocol level; stand-in relay, ~21 s) ===\n");
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    memset(&g_rig, 0, sizeof(g_rig));
    InitializeCriticalSection(&g_rig.cs);
    for (int i = 0; i < (int)mh_net_proto::KEY_LEN; ++i) g_psk[i] = (uint8_t)(0xa0 + i);
    mh::udprelay::deployment_leg_key(g_psk, g_leg_key);

    g_rig.relay = loopback_socket(&g_rig.relay_port);
    g_rig.ep    = loopback_socket(&g_rig.ep_port);
    g_rig.probe = loopback_socket(&g_rig.probe_port);
    check("three stand-in sockets bound", g_rig.relay != INVALID_SOCKET &&
                                              g_rig.ep != INVALID_SOCKET &&
                                              g_rig.probe != INVALID_SOCKET);

    mh::udprelay::Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    lstrcpyA(cfg.host, "127.0.0.1");
    cfg.port      = g_rig.relay_port;
    cfg.room      = ROOM;
    cfg.role      = 0; // host
    cfg.game_port = g_rig.ep_port;
    mh::udprelay::set_peer_known(rig_known, &g_rig);
    g_rig.welcome_len   = 5; // mp:R4a -- a current relay: arms A-C run against a matching level
    g_rig.welcome_level = PROTOCOL_LEVEL;
    check("tunnel started as host", mh::udprelay::start(cfg, g_psk, rig_log, nullptr));

    // The WELCOME lands on the first pump; give it a moment.
    pump_for(400);
    check("the tunnel HELLOed and was welcomed", g_rig.have_tunnel);

    // ---- A: eight in, the ninth refused while all eight are held ------------------------------
    printf("-- A: eight joins fill the tables; a ninth LIVE peer is refused (the cap)\n");
    unsigned short ports[8];
    bool           punched = true;
    for (uint16_t k = 1; k <= 8; ++k) {
        const Join j = join(k, true);
        ports[k - 1] = j.port;
        punched      = punched && j.punched;
        if (j.port != 0) rig_join_known(j.port);
    }
    check("A: eight distinct loopback sockets, one per handle", distinct(ports, 8));
    check("A: eight punches, one per handle", punched);

    // ---- mp:L1f: THE PATH CLASSIFICATION, asked the way the ENDPOINT asks it ------------------
    // udp_endpoint.cpp's get_stats() answers MH_NetPeerLatency.relayed from
    // mh::udprelay::path_class(conn.addr) -- the reverse of the set_peer_known edge this suite
    // already drives. THE RIG IS THE ONLY PLACE the `relayed = 1` answer can be produced on
    // demand: every DATA above arrived on the relay leg by construction, so each of those eight
    // loopback ports must classify as relayed, and a port the tunnel never minted must classify
    // as UNKNOWN rather than as a default. The DIRECT answer (0) is proven two ways -- the
    // no-tunnel arm at the end of this suite, and a live 3-peer lobby on the rig (tracker mp:L1f)
    // -- because a pair PROMOTED to direct under a running tunnel needs a real punch to exist.
    {
        sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bool all_relayed  = true;
        for (int i = 0; i < 8; ++i) {
            a.sin_port = htons(ports[i]);
            if (mh::udprelay::path_class(a) != 1) all_relayed = false;
        }
        check("A/L1f: every joined peer classifies as RELAYED by its loopback address", all_relayed);
        // A port nothing minted. ports[] are ephemeral, so pick one provably outside the set.
        unsigned short spare = 1;
        for (int i = 0; i < 8; ++i)
            if (ports[i] >= spare) spare = (unsigned short)(ports[i] + 1);
        a.sin_port = htons(spare);
        check("A/L1f: an address the tunnel never minted is UNKNOWN, not a default",
              mh::udprelay::path_class(a) == -1);
        // ...and a non-loopback address is not ours to classify at all.
        a.sin_addr.s_addr = htonl(0x08080808u);
        a.sin_port        = htons(ports[0]);
        check("A/L1f: a non-loopback address is UNKNOWN whatever its port",
              mh::udprelay::path_class(a) == -1);
    }
    {
        const Join j = join(9, false);
        check("A: a ninth while all eight are held gets no loopback socket", j.port == 0);
        check("A: ...and no punch", !j.punched);
        check("A: the refusal was logged", g_rig.refused >= 1);
    }

    // ---- B: the eight leave; the ninth and tenth then get everything ----------------------------
    printf("-- B: the eight leave (the endpoint drops them); after the sweep, 9 and 10 join\n");
    rig_forget_all();
    {
        // GRACE_MS (5 s) from the slots' birth plus one SWEEP_MS (1 s) of timer slack.
        const DWORD t0 = GetTickCount();
        while (GetTickCount() - t0 < 7000 && rig_released() < 8) pump_for(100);
        check("B: exactly eight release lines (one per departed handle)", rig_released() == 8);
        // Not before the grace: a release inside the first second would be a slot freed on a
        // guess, before the endpoint could have registered its handshake.
        check("B: ...and none of them inside the first second", GetTickCount() - t0 >= 1000);
    }
    unsigned short late[2];
    {
        const Join j9 = join(9, true);
        late[0]       = j9.port;
        check("B: the ninth join gets a loopback socket", j9.port != 0);
        check("B: the ninth join gets a punch", j9.punched);
        if (j9.port) rig_join_known(j9.port);
        const Join j10 = join(10, true);
        late[1]        = j10.port;
        check("B: the tenth join gets a loopback socket, distinct from the ninth's",
              j10.port != 0 && j10.port != j9.port);
        check("B: the tenth join gets a punch", j10.punched);
        if (j10.port) rig_join_known(j10.port);
    }

    // ---- C: a punch with nobody behind it is an orphan, released by the same sweep --------------
    printf("-- C: a CAND from a handle that never sends DATA holds a punch slot only until the "
           "orphan rule\n");
    {
        uint8_t   c[32];
        const int clen = cand_payload(c);
        relay_send(OP_CAND, 50, HOST_HANDLE, c, clen);
        check("C: the orphan was probed (it did hold a punch slot)", wait_probe(1500, 50));
        const int   before = rig_released();
        const DWORD t0     = GetTickCount();
        while (GetTickCount() - t0 < 7000 && rig_released() < before + 1) pump_for(100);
        check("C: the orphan's punch was released", rig_released() == before + 1);
        check("C: ...and 9 and 10, still held by the endpoint, were not",
              rig_released() == before + 1 && late[0] != 0 && late[1] != 0);
        // Six more fill the table beside 9 and 10 -- so every one of the eight slots the first
        // eight held, and the one the orphan held, is a slot a real peer got back.
        unsigned short more[6];
        bool           all = true;
        for (uint16_t k = 11; k <= 16; ++k) {
            const Join j = join(k, true);
            more[k - 11] = j.port;
            all          = all && j.punched;
            if (j.port) rig_join_known(j.port);
        }
        check("C: six more joins each get a loopback socket", distinct(more, 6));
        check("C: ...and a punch", all);
        const Join j = join(17, false);
        check("C: the table is full again at eight LIVE peers: the next is refused", j.port == 0);
    }

    // ---- D (mp:R4a): the protocol level, every shape a relay's answer can take ---------------------
    printf("-- D: the protocol level -- no level / lower / equal / newer / a foreign leg version / an "
           "op above the table (mp:R4a)\n");
    mh::udprelay::stop();
    struct Shape {
        const char *name;
        int         welcome_len;
        uint8_t     welcome_level;
        uint8_t     magic_ver;
        bool        unknown_op;
        int         want_older, want_newer, want_legver, want_unknown, want_notice;
        const char *want_text; // the notice text, or null for "no notice"
    };
    // "lower" is staged as an explicit 0 in the 5-byte form: with PROTOCOL_LEVEL at 1 there is no
    // lower non-zero number, and 0 in the fifth byte is the same decision as no fifth byte, which
    // is worth pinning on its own (a relay that grew the byte but claims nothing).
    const Shape shapes[] = {
        {"no level (4-byte WELCOME, the pre-R4a shape)", 4, 0, LEG_MAGIC_VER, false, 1, 0, 0, 0, 1,
         "Relay outdated (protocol 0 < 1)"},
        {"lower level (5-byte WELCOME saying 0)", 5, 0, LEG_MAGIC_VER, false, 1, 0, 0, 0, 1,
         "Relay outdated (protocol 0 < 1)"},
        {"the same level", 5, PROTOCOL_LEVEL, LEG_MAGIC_VER, false, 0, 0, 0, 0, 0, nullptr},
        {"a newer relay", 5, (uint8_t)(PROTOCOL_LEVEL + 1), LEG_MAGIC_VER, false, 0, 1, 0, 0, 0,
         nullptr},
        {"a leg version this build does not speak", 4, 0, 0x52, false, 0, 0, 1, 0, 1,
         "Incompatible relay (leg v2/v1)"},
        {"an op above this build's table (newer relay)", 5, PROTOCOL_LEVEL, LEG_MAGIC_VER, true,
         0, 0, 0, 1, 0, nullptr},
    };
    for (int i = 0; i < (int)(sizeof(shapes) / sizeof(shapes[0])); ++i) {
        const Shape &sh = shapes[i];
        printf("   D.%d %s\n", i + 1, sh.name);
        EnterCriticalSection(&g_rig.cs);
        g_rig.have_tunnel     = false;
        g_rig.welcome_len     = sh.welcome_len;
        g_rig.welcome_level   = sh.welcome_level;
        g_rig.send_unknown_op = sh.unknown_op;
        g_rig.hello_len = g_rig.hello_flags = 0;
        g_rig.line_older = g_rig.line_newer = g_rig.line_legver = g_rig.line_unknown_op = 0;
        g_rig.notices                                                                   = 0;
        g_rig.notice_text[0]                                                            = 0;
        g_magic_ver                                                                     = sh.magic_ver;
        LeaveCriticalSection(&g_rig.cs);
        char what[160];
        wsprintfA(what, "D.%d: tunnel restarted (%s)", i + 1, sh.name);
        check(what, mh::udprelay::start(cfg, g_psk, rig_log, nullptr));
        // Long enough for the HELLO (sent on the first pump), the WELCOME and two HELLO retries
        // (500 ms) -- the foreign-version shape never gets a WELCOME the tunnel can read, so its
        // HELLO retries, and a retry must not earn a second line.
        pump_for(1300);
        EnterCriticalSection(&g_rig.cs);
        const Rig r = g_rig;
        LeaveCriticalSection(&g_rig.cs);
        wsprintfA(what, "D.%d: the HELLO is still 18 bytes (additive: an old relay's exact-length "
                        "parse accepts it)",
                  i + 1);
        check(what, r.hello_len == 18);
        wsprintfA(what, "D.%d: the HELLO carries level %u above the re-key bit", i + 1,
                  (unsigned)PROTOCOL_LEVEL);
        check(what, (r.hello_flags >> HELLO_LEVEL_SHIFT) == PROTOCOL_LEVEL &&
                        (r.hello_flags & HELLO_FLAG_PEER_KEY) != 0);
        wsprintfA(what, "D.%d: `relay protocol <t> < <o>` lines: %d (want %d)", i + 1, r.line_older,
                  sh.want_older);
        check(what, r.line_older == sh.want_older);
        wsprintfA(what, "D.%d: `relay protocol <t> > <o>` lines: %d (want %d)", i + 1, r.line_newer,
                  sh.want_newer);
        check(what, r.line_newer == sh.want_newer);
        wsprintfA(what, "D.%d: `relay protocol leg v` lines: %d (want %d)", i + 1, r.line_legver,
                  sh.want_legver);
        check(what, r.line_legver == sh.want_legver);
        wsprintfA(what, "D.%d: unknown-op lines: %d (want %d)", i + 1, r.line_unknown_op,
                  sh.want_unknown);
        check(what, r.line_unknown_op == sh.want_unknown);
        wsprintfA(what, "D.%d: notices attempted: %d (want %d)", i + 1, r.notices, sh.want_notice);
        check(what, r.notices == sh.want_notice);
        if (sh.want_text) {
            wsprintfA(what, "D.%d: notice text is `%s` (got `%s`)", i + 1, sh.want_text, r.notice_text);
            check(what, strcmp(r.notice_text, sh.want_text) == 0);
        }
        mh::udprelay::stop();
    }
    g_magic_ver = LEG_MAGIC_VER;

    mh::udprelay::set_peer_known(nullptr, nullptr);
    closesocket(g_rig.relay);
    closesocket(g_rig.ep);
    // mp:L1f: WITH NO TUNNEL RUNNING every address is DIRECT, and that is a fact about the build's
    // configuration rather than a default -- with no relay leg there is no path a datagram could
    // have been relayed over. It is what makes an ordinary LAN lobby's ping cell read "D <n>"
    // instead of a letterless number, so it is asserted rather than assumed. (`stop()` already ran
    // in arm D; this is the state every non-relay build is in for the whole of its life.)
    {
        sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port        = htons(ports[0]);
        check("L1f: with no tunnel running, every address classifies as DIRECT",
              mh::udprelay::path_class(a) == 0);
    }
    closesocket(g_rig.probe);
    DeleteCriticalSection(&g_rig.cs);
    printf("=== udprelaytest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
