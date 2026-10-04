//
// udp_endpoint.cpp -- the UDP transport core (tracker mp:T1). Read udp_endpoint.h first: it carries
// the design, the reason this is an object rather than a file of globals, and the one place where
// this deviates from the item's scope sketch (the timeout retransmit, and the arithmetic for it).
//
// THE WIRE, TOP TO BOTTOM, so the layering is legible before the code:
//
//   mh.exe's own lobby/lockstep bytes
//     -> a 12-byte WireHdr (mh_net_proto/net_wire.h) -- IDENTICAL to what mh_net.dll puts on TCP,
//        which is what makes "transport=udp is the same game" a claim about one variable
//     -> a reliable ordered BYTE STREAM of this endpoint's making
//     -> SEGMENTS of <= 254 bytes, numbered, carried K-redundantly in T0's channel A
//     -> T0 channel frames (CH_INPUT / CH_STATE / CH_BULK / CH_HS)
//     -> a T0 sealed datagram: 18-byte authenticated header + ChaCha20 body + 16-byte tag
//     -> one UDP socket
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define _WINSOCK_DEPRECATED_NO_WARNINGS // inet_addr: the direct-connect-by-IP path, as in mh_net
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <stdarg.h>
#include <string.h>

#include "udp_endpoint.h"
#include "mh_net_key.h"            // MH_Key_Random -- the CSPRNG behind every nonce here
#include "mh_net_queue_policy.h"   // D24: WHICH inbound frame a full queue may destroy
#include "mh_net_watchdog.h"       // D16: the watchdog's timing decisions, reused untouched
#include "mh_net_proto/net_wire.h" // the 12-byte routing header, shared with the TCP module

// mp:T3 -- the RTT stopwatch. GetTickCount (which everything else in this file times with, because
// everything else in this file is a multi-second timeout) has a ~15.6 ms quantum, and the latency
// done_when's budget is 8 ms. This is the only clock in the endpoint that needs to be better than a
// system tick, so it is the only place QPC is read.
static int64_t qpc_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (int64_t)t.QuadPart;
}

// SIO_UDP_CONNRESET is a Windows-only vendor ioctl and which SDK header carries it has moved
// around (mswsock.h / mstcpip.h, and neither reliably under WIN32_LEAN_AND_MEAN). Its value is
// fixed and public, so the fallback is a definition rather than a header hunt -- and it is
// guarded, so a future SDK that does declare it wins.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib") // wsprintfA

namespace mh {
namespace netudp {

using namespace mh_net_proto;
namespace U = mh_net_proto::udp;

namespace {

// mp:U41e -- the periodic inbound-queue rollup cadence, IDENTICAL to net_transport.cpp's
// QUEUE_ROLLUP_MS: often enough that a long match carries the approach to the cap, rare enough that
// it does not bury anything, and matched to `[desync] STATUS`'s own ~50 s cadence.
constexpr DWORD QUEUE_ROLLUP_MS = 50000;

// ---- the handshake channel ----------------------------------------------------------------------
//
// A channel id T0 does not define, which is deliberate and is the forward-compatibility rule T0
// states for its own mux: "the receiver dispatches by id and IGNORES an id it does not know". A
// relay (mp:R1) demuxes on the plaintext conn_id and never opens a body, and mp:T2 owns CH_BULK's
// piece machinery, so nothing else has to learn this id. Chosen well clear of 1..3 so a channel
// added later cannot collide with it by counting upward.
constexpr uint8_t CH_HS = 0x40;

// CH_HS payload = kind(1) | data. The three kinds are the PSK handshake's own three messages plus
// the host's token grant; their lengths are net_crypto.h's, so a wrong-length message is refused
// before anything looks at its contents.
constexpr uint8_t HSK_HELLO     = 1; // client -> host, HS_HELLO_LEN
constexpr uint8_t HSK_CHALLENGE = 2; // host -> client, HS_CHALLENGE_LEN
constexpr uint8_t HSK_GRANT     = 3; // host -> client, TOKEN_WIRE (the minted connect token)

// The three labels the bootstrap secrets are derived from. Distinct labels under one HMAC so a
// deployment configures ONE secret (mh_key.txt) and the three uses can never be the same bytes.
const char *const LBL_CONN = "mh-udp-boot-conn";
const char *const LBL_ENC  = "mh-udp-boot-enc";
const char *const LBL_MAC  = "mh-udp-boot-mac";

// Unix milliseconds, for the connect token's expiry. Written out rather than taken from
// mh_common/include/mh_session_id.h because that header also carries a BCryptGenRandom helper, and
// a satellite loaded from inside another module's DllMain must not acquire an import mh.dll does
// not already have (the subset rule, docs/dll-split.md).
uint64_t unix_ms() {
    FILETIME       ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // 100-ns ticks since 1601-01-01 -> ms since 1970-01-01.
    return (u.QuadPart - 116444736000000000ULL) / 10000ULL;
}

bool same_addr(const sockaddr_in &a, const sockaddr_in &b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

void addr_str(const sockaddr_in &a, char *out, size_t cap) {
    char ip[64];
    if (!inet_ntop(AF_INET, (void *)&a.sin_addr, ip, sizeof(ip))) lstrcpyA(ip, "?");
    wsprintfA(out, "%s:%d", ip, (int)ntohs(a.sin_port));
    (void)cap;
}

bool ensure_wsa() {
    static bool done = false;
    if (done) return true;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    done = true;
    return true;
}

// mp:U59 -- the one frame class the retention ring coalesces: the bare lockstep horizon advert,
// latest-wins by construction (the inbound queue's lane H evicts superseded ones by the same predicate).
inline bool is_bare_horizon(const uint8_t *d, int n) {
    return mh::net::queue_policy::lane_of_game_frame(d, n) == mh::net::queue_policy::lane::supersedable;
}
static_assert(osq::PAYLOAD_MAX == MH_NET_MAX_PAYLOAD, "origin_seq.h's payload ceiling is the module's");
static_assert(osq::ORIGINS == MH_NET_MAX_PEERS, "one retention ring per player slot");

} // namespace

// =================================================================================================
Endpoint::Endpoint() {
    memset(this, 0, sizeof(*this));
    m_sock      = INVALID_SOCKET;
    m_dead_peer = -1;
    m_my_id     = -1;
    m_fo_test_first = -1; // mp:U63
    m_spectators    = 0;
}

void Endpoint::logf(const char *fmt, ...) {
    if (!m_log) return;
    // mp:SES6 bumped this from 512 to 1024 -- the counters line below now appends one segment per
    // active peer and can outgrow 512 with 3+ peers. 1024 is not an arbitrary round number: it is
    // wsprintfA's OWN documented output ceiling (it silently truncates past it regardless of buffer
    // size), so this is exactly as large as the buffer can ever usefully be.
    char    line[1024];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(line, fmt, ap);
    va_end(ap);
    m_log(m_log_ctx, line);
}

// ---- the bootstrap secrets ----------------------------------------------------------------------
// Derived from the PSK alone, so both ends have them before any exchange: a client must be able to
// address a host it has never spoken to, and a host must be able to open that first datagram with no
// per-peer state at all. They are used ONLY for the three PSK handshake messages, whose contents are
// nonces and HMAC proofs -- the same values the TCP module sends in the clear.
//
// EVERY BOOTSTRAP DATAGRAM CARRIES A RANDOM 64-BIT SEQUENCE, and that is load-bearing rather than
// tidy. The sequence IS the ChaCha20 nonce (T0), so a fixed key plus a counter starting at zero
// would repeat a keystream across two clients' handshakes. A CSPRNG draw makes a collision a 2^-64
// event instead of a certainty. The bootstrap exchange therefore runs with NO replay window (there
// is no per-peer state yet to hold one); freshness comes from the challenge-response itself, which
// is exactly how the TCP handshake gets it.
void Endpoint::boot_keys() {
    uint8_t full[SHA256_LEN];
    hmac_sha256(m_psk, KEY_LEN, (const uint8_t *)LBL_CONN, lstrlenA(LBL_CONN), full);
    memcpy(m_boot_conn, full, CONN_ID_BYTES);
    hmac_sha256(m_psk, KEY_LEN, (const uint8_t *)LBL_ENC, lstrlenA(LBL_ENC), m_boot_enc);
    hmac_sha256(m_psk, KEY_LEN, (const uint8_t *)LBL_MAC, lstrlenA(LBL_MAC), m_boot_mac);
}

// ---- raw send -----------------------------------------------------------------------------------
bool Endpoint::send_dgram(const sockaddr_in &to, const uint8_t *pkt, size_t len) {
    const int n = sendto(m_sock, (const char *)pkt, (int)len, 0, (const sockaddr *)&to, sizeof(to));
    if (n == SOCKET_ERROR) return false;
    ++m_c.dgram_tx;
    return true;
}

bool Endpoint::send_sealed(const sockaddr_in &to, uint8_t type, const uint8_t conn_id[8],
                           uint64_t seq, const uint8_t enc[32], const uint8_t mac[32],
                           const uint8_t *body, size_t blen) {
    uint8_t      pkt[U::MAX_DATAGRAM];
    U::Verdict   why = U::Verdict::Ok;
    const size_t n   = U::packet_encode(type, conn_id, seq, enc, mac, body, blen, pkt, why);
    if (n == 0) {
        logf("net: udp encode refused (%s, body %d) -- this is a bug in the sender, not the link",
             U::verdict_name(why), (int)blen);
        return false;
    }
    // mp:R1d -- THE BACKSTOP, and it REFUSES rather than shrugging. Every producer above is sized
    // so this cannot fire; if one ever is not, emitting the datagram would put a 1201+ byte packet
    // on a relayed path, which is precisely the failure the 1200 figure exists to avoid and which
    // shows up as an unexplained blackhole on exactly the networks least able to diagnose it. A
    // refusal with a named size is a bug report; the datagram is not.
    if (m_leg_overhead > 0 && (int)n + m_leg_overhead > (int)U::MAX_DATAGRAM) {
        if (!m_over_cap_said) {
            m_over_cap_said = true;
            logf("net: udp REFUSED a %d B datagram -- with the relay leg's %d B it would be %d, "
                 "over the 1200 B ceiling (mp:R1d). This is a sender bug; the transfer will stall "
                 "rather than send something the path may drop.",
                 (int)n, m_leg_overhead, (int)n + m_leg_overhead);
        }
        return false;
    }
    return send_dgram(to, pkt, n);
}

// ---- synthetic loss (the acceptance test's hook) -------------------------------------------------
void Endpoint::set_rx_loss(unsigned per_mille, uint32_t seed) {
    m_loss_pm    = per_mille;
    m_loss_state = seed ? seed : 0x9e3779b9u;
}

void Endpoint::set_rx_block_port(unsigned short port, bool on) {
    if (port == 0) return;
    for (int i = 0; i < 8; ++i)
        if (m_block_port[i] == (LONG)port) {
            if (!on) InterlockedExchange(&m_block_port[i], 0);
            return;
        }
    if (on)
        for (int i = 0; i < 8; ++i)
            if (InterlockedCompareExchange(&m_block_port[i], (LONG)port, 0) == 0) return;
}

bool Endpoint::lose_it() {
    if (m_loss_pm == 0) return false;
    // xorshift32: deterministic, so a failing loss pattern is replayable from its seed.
    uint32_t x = m_loss_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m_loss_state = x;
    return (x % 1000u) < m_loss_pm;
}

// ---- start / stop -------------------------------------------------------------------------------
bool Endpoint::start(const Config &cfg, const uint8_t psk[KEY_LEN], bool secure) {
    // mp:T1b -- RE-STARTING A STARTED ENDPOINT RESTARTS IT, the UDP mirror of U40's rule for
    // mh_net.dll. It used to `return true` here, which is what made the TCP defect reachable through
    // this module too: a client whose link died at match teardown kept `m_started` (and the module's
    // `g_started` above it) latched true for the life of the process, so the discovery poll's connect
    // kick -- gated on `!MH_Net_IsStarted()` -- could never fire and MH_Net_InitEx early-returned.
    // The rule is expressed as a RESTART rather than as a 24th export for the reason net_transport.cpp
    // sets out at length: "stop" is not a symbol a caller can get wrong, it is what re-dialling means.
    // A refused stop leaves the old endpoint running and reports false; the caller is then exactly
    // where the pre-T1b build left it.
    if (m_started) {
        // The same line mh_net.dll writes when its net_reset() begins, verbatim: mh_net.log is ONE
        // file both transports append to, and a reader grepping it for a relink should not have to
        // know which module wrote the run. It is logged HERE and not in stop() because stop() is
        // also the ordinary teardown, and only this call site is a relink.
        logf("net: returning the transport to the pre-init state (relink)");
        if (!stop()) return false;
    }
    if (!ensure_wsa()) return false;

    m_cfg         = cfg;
    m_role        = cfg.net.role;
    m_my_id       = cfg.net.player_id;
    m_host_assign = (cfg.net.host_assign != 0);
    m_secure      = secure;
    {
        // mp:U59 -- this life's incarnation: a per-start() tag (never an ordering) that lets every peer
        // tell "the same origin, restarted" from "the same origin, still going". Mixed from four
        // sources that differ between two endpoints created in one process in one tick (the selftests).
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        uint32_t x = (uint32_t)q.QuadPart ^ (GetTickCount() * 2654435761u) ^
                     (GetCurrentProcessId() * 40503u) ^ (uint32_t)(uintptr_t)this;
        m_osq.reset((uint16_t)(x ^ (x >> 16)));
        m_osq.set_local(m_my_id);
        m_osq_legacy_said = false;
        // mp:U60 -- a fresh life has no rehome history. (The WELCOME counters are lifetime and stay.)
        m_rehomed      = false;
        m_hs_budget_ms = 0;
        memset(m_roster, 0, sizeof(m_roster));
        memset(&m_rc, 0, sizeof(m_rc));
        // mp:U61 -- a fresh life has no mesh. (The hooks and the test knobs are set once and stay.)
        memset(&m_mesh, 0, sizeof(m_mesh));
        m_mesh.M.clear();
        m_mesh.on = true;
        // mp:U62 -- ...and no handover history. (The mute test knob is set once and stays.)
        memset(&m_hl, 0, sizeof(m_hl));
        m_hl.hub_id  = -1;
        m_hl.old_hub = m_hl.new_hub = -1;
        // mp:U63 -- ...and no failover history. (The fo test knobs are set once and stay.)
        memset(&m_fo, 0, sizeof(m_fo));
        m_fo.hub_id = m_fo.cand = m_fo.forced = -1;
        m_dead_extra = 0;
        m_spectators = 0;
    }
    m_K           = cfg.redundancy;
    if (m_K < K_MIN) m_K = K_DEFAULT;
    if (m_K > K_MAX) m_K = K_MAX;
    // R-live, and the SAME three-state convention mh_net.dll's InitEx uses, because net_seams.cpp's
    // lazy_start feeds both from one ini: 0 in the config means UNSET -> the transport's own
    // defaults, and a NEGATIVE value is the explicit "off" (the documented `rx_timeout_ms=-1` for a
    // peer parked under a debugger). Reading the raw field instead is not a smaller bug than it
    // looks: `[net]` ships neither key, so every real run arrived here with 0/0 and the log said
    // `ping every 0 ms, drop after 0 ms of silence` -- no keepalives generated and no dead link ever
    // detected, which is precisely the R-live defect this module inherited the watchdog to fix.
    // Caught on the first rig boot, by that log line.
    m_ping_ms       = (cfg.net.ping_ms == 0) ? PING_MS_DEFAULT : (cfg.net.ping_ms > 0 ? cfg.net.ping_ms : 0);
    m_rx_timeout_ms = (cfg.net.rx_timeout_ms == 0)
                          ? RX_TIMEOUT_MS_DEFAULT
                          : (cfg.net.rx_timeout_ms > 0 ? cfg.net.rx_timeout_ms : 0);
    memcpy(m_psk, psk, KEY_LEN);
    boot_keys();
    {
        LARGE_INTEGER f;
        m_qpc_freq = QueryPerformanceFrequency(&f) ? (int64_t)f.QuadPart : 0;
    }

    // mp:T2. The channel is bound, never re-created: its chunk frontier is what a restart must not
    // forget (udp_endpoint.h's m_bulk note), so a relink re-points the emit edge and leaves the
    // transfer state exactly where the last acknowledgement put it.
    m_bulk.bind(&Endpoint::bulk_emit_thunk, this);
    m_bulk.configure(cfg.bulk_selftest_mb,
                     cfg.bulk_selftest_step > 0 ? (uint32_t)cfg.bulk_selftest_step : 100u);

    // mp:R1d -- THE RELAYED CEILING. Everything this endpoint emits is at most MAX_DATAGRAM minus
    // whatever wraps it downstream, and the ONE path that can reach that ceiling is channel C: a
    // full piece is 18 + 3 + 51 + 1100 + 16 = 1188 bytes, so 1222 on a relay leg. Channel A's
    // K-redundant input frame caps at four 256-byte entries (1063) and channel B's records are
    // tens of bytes, so lowering the piece stride is the whole of the fix -- which is why the
    // stride is what moves and not a body cap plumbed through every producer. `send_sealed` holds
    // the backstop that turns "we believe nothing else gets near it" into an assertion.
    m_leg_overhead = cfg.leg_overhead;
    if (m_leg_overhead < 0) m_leg_overhead = 0;
    if (m_leg_overhead > 64) m_leg_overhead = 64; // a wrapper bigger than this is not our leg
    m_over_cap_said = false;
    m_bulk.set_piece_max(m_leg_overhead > 0 ? U::PIECE_MAX_RELAYED : U::PIECE_MAX);
    if (m_leg_overhead > 0)
        logf("net: udp datagram ceiling %d B (1200 minus the %d B a relay leg adds); channel C "
             "pieces are %d B (mp:R1d)",
             (int)U::MAX_DATAGRAM - m_leg_overhead, m_leg_overhead, (int)U::PIECE_MAX_RELAYED);

    // Initialised ONCE per object and never deleted (stop() explains why). A restart re-uses them.
    if (!m_cs_ready) {
        InitializeCriticalSection(&m_conn_cs);
        InitializeCriticalSection(&m_q_cs);
        m_cs_ready = true;
    }
    memset(m_conns, 0, sizeof(m_conns));
    memset(m_pend, 0, sizeof(m_pend));
    m_lanes.reset(); // mp:U41e -- the TRANSPORT boundary; see m_lanes' note in udp_endpoint.h
    // N1: a client in host_assign mode must WAIT for its id; everyone else's is settled at start.
    m_id_assigned = (m_role == 1 && m_host_assign) ? 0 : 1;

    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == INVALID_SOCKET) {
        logf("net: udp socket() failed %d", WSAGetLastError());
        return false;
    }
    // SO_EXCLUSIVEADDRUSE for the same reason the TCP host sets it (net_transport.cpp's long note):
    // a second instance silently taking the port answers joiners with ITS OWN mh_key.txt, and the
    // joiner then reports a key mismatch that is nothing of the sort. Failing the bind says so.
    BOOL excl = TRUE;
    setsockopt(m_sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof(excl));
    // A UDP socket that has ICMP port-unreachable reported back to it fails its NEXT recvfrom with
    // WSAECONNRESET, which on a star whose clients come and go would kill the host's recv loop. This
    // ioctl is the documented way to stop that; without it a single departed peer ends the transport.
    {
        DWORD off = 0, got = 0;
        WSAIoctl(m_sock, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &got, nullptr, nullptr);
    }

    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family        = AF_INET;
    a.sin_addr.s_addr   = htonl(INADDR_ANY);
    unsigned short want = cfg.bind_port;
    if (want == 0 && m_role == 0) want = (unsigned short)cfg.net.port;
    a.sin_port = htons(want);
    if (bind(m_sock, (sockaddr *)&a, sizeof(a)) == SOCKET_ERROR) {
        const int e = WSAGetLastError();
        if (e == WSAEADDRINUSE || e == WSAEACCES)
            logf("net: udp bind(:%d) REFUSED -- something else on THIS machine already holds that "
                 "port. Almost always another mh.focus.exe (a rig lane, a determinism run, or an "
                 "older host you did not close). Close it and host again -- do NOT assume it is a "
                 "key problem: a second instance answers joiners with its own mh_key.txt.",
                 (int)want);
        else
            logf("net: udp bind(:%d) failed %d", (int)want, e);
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
        return false;
    }

    InterlockedExchange(&m_running, 1);
    m_started = true;

    if (m_role == 0) {
        // The host is passive: it has nothing to say until a client presents a HELLO.
        if (!MH_Key_Random(m_transport_id, (unsigned)UUID7_BYTES)) {
            logf("net: no secure randomness available -- refusing to host");
            stop();
            return false;
        }
        logf("net: udp HOST listening on :%d as player %d (K=%d) [key %s]", cfg.net.port, m_my_id,
             m_K, m_secure ? "set" : "open");
    } else {
        // The client's single conn slot is 0 and its handshake starts on the timer thread's first
        // pass, so start() never blocks the lobby thread -- which is the S8 property the TCP client
        // had to acquire the hard way (a blocking connect froze the whole lobby for ~20 s).
        Pending &p = m_pend[0];
        memset(&p, 0, sizeof(p));
        // mp:R2c -- BROWSE ONLY: mh.dll set cfg.browse_only because this dial has no target room at
        // all (no typed address, no directory pick -- see udp_transport.cpp). Leaving `p.used`
        // false means client_handshake_tick (below) never arms, so there is no peer to retry
        // against and no HS_BUDGET_MS "handshake FAILED" line -- the relay leg still comes up
        // (started below, both branches) and still delivers the directory LIST, which is all a
        // browse-only dial is for.
        if (!cfg.browse_only) {
            p.used = true;
            memset(&p.addr, 0, sizeof(p.addr));
            p.addr.sin_family      = AF_INET;
            p.addr.sin_port        = htons((u_short)cfg.net.port);
            p.addr.sin_addr.s_addr = inet_addr(cfg.net.host[0] ? cfg.net.host : "127.0.0.1");
            p.first_ms             = GetTickCount();
            if (!MH_Key_Random(p.cn, (unsigned)NONCE_LEN) ||
                !MH_Key_Random((uint8_t *)&p.seq, (unsigned)sizeof(p.seq))) {
                logf("net: no secure randomness available -- refusing to connect");
                stop();
                return false;
            }
            logf("net: udp CLIENT -> %s:%d as player %d (K=%d) [key %s]",
                 cfg.net.host[0] ? cfg.net.host : "127.0.0.1", cfg.net.port, m_my_id, m_K,
                 m_secure ? "set" : "open");
        } else {
            logf("net: udp CLIENT (browse only -- no typed address and no directory pick yet, "
                 "player %d) -- no handshake attempted; browsing the relay directory (mp:R2c)",
                 m_my_id);
        }
    }

    m_recv_thread  = CreateThread(nullptr, 0, recv_thunk, this, 0, nullptr);
    m_timer_thread = CreateThread(nullptr, 0, timer_thunk, this, 0, nullptr);
    logf("net: udp link watchdog armed (ping every %d ms, drop after %d ms of silence)", m_ping_ms,
         m_rx_timeout_ms);
    return true;
}

// One budgeted join. Returning false (rather than closing the handle anyway, which is what this used
// to do) is the whole difference between a refusal and a corruption: a CloseHandle on a thread that
// is still inside recv_loop leaves that thread running over tables the next start() is about to
// memset, and the crash lands somewhere else entirely.
bool Endpoint::join_thread(HANDLE &h, const char *what) {
    if (!h) return true;
    if (WaitForSingleObject(h, RESET_JOIN_MS) != WAIT_OBJECT_0) {
        logf("net: reset REFUSED -- the %s thread did not stop within %lu ms; keeping the old "
             "transport (no relink this time)",
             what, (unsigned long)RESET_JOIN_MS);
        return false;
    }
    CloseHandle(h);
    h = nullptr;
    return true;
}

// mp:T1b -- the UDP mirror of mh_net.dll's net_reset(). Same order and the same three properties:
//
//   CLOSE THE SOCKET FIRST, THEN JOIN. Both threads test `m_running`, but the recv thread spends its
//   life blocked in recvfrom(); closing the handle under it is what returns that call. The timer
//   thread needs no help -- it sleeps TICK_MS at a time.
//
//   NOTHING IS ZEROED WHILE A THREAD THAT COULD READ IT IS ALIVE. Every memset below is after both
//   joins succeeded, and a join that fails aborts the whole reset with the endpoint left started.
//
//   THE CRITICAL SECTIONS SURVIVE. They are initialised once per object (m_cs_ready) and never
//   deleted, because udp_transport.cpp's MH_Net_Send/Recv test the module's `g_started` without
//   holding anything: a CS deleted under a concurrent caller is a crash, where a stale-but-valid one
//   is an empty queue. The cost is two un-deleted CRITICAL_SECTIONs per Endpoint object at process
//   exit, which is what mh_net.dll already pays for the same reason.
//
// THE KEYS GO WITH THE CONNECTIONS, which the TCP module gets for free (its per-conn state IS the
// socket) and this one has to do by hand: the PSK, the three bootstrap secrets derived from it, the
// host's transport id and every peer's SessionKeys live in this object, and a relink is a new link
// -- a new scope for conn_ids, new session keys, and no reason to keep the old ones in memory.
bool Endpoint::stop() {
    if (!m_started && m_sock == INVALID_SOCKET) return true;
    InterlockedExchange(&m_running, 0);
    if (m_sock != INVALID_SOCKET) {
        closesocket(m_sock); // unblocks recvfrom
        m_sock = INVALID_SOCKET;
    }
    // Both joins are ATTEMPTED even if the first refuses, so a handle is never left un-waited.
    bool ok = join_thread(m_recv_thread, "udp recv");
    ok      = join_thread(m_timer_thread, "udp timer") && ok;
    if (!ok) return false; // m_started stays true -- the pre-T1b behaviour, loudly

    // NOTE FOR ANYONE TIDYING UP: m_bulk is deliberately NOT cleared here. A transfer's chunk
    // frontier is exactly what must survive a restart for mp:T2's resume to mean anything, and
    // clearing it is a mutation `udpbulktest`'s resume arm catches by name.
    memset(m_conns, 0, sizeof(m_conns)); // the connection table AND its per-peer session keys
    memset(m_pend, 0, sizeof(m_pend));   // ...and the handshakes that never finished
    memset(m_psk, 0, sizeof(m_psk));
    memset(m_boot_conn, 0, sizeof(m_boot_conn));
    memset(m_boot_enc, 0, sizeof(m_boot_enc));
    memset(m_boot_mac, 0, sizeof(m_boot_mac));
    memset(m_transport_id, 0, sizeof(m_transport_id));
    if (m_cs_ready) {
        // mp:U41e -- THE LAST rollup of the match goes out before the lanes are cleared, so a match
        // that never reached the periodic cadence still leaves one line saying how deep its queue
        // got (mirrors net_transport.cpp's net_reset()). Read under the lock, logged after it.
        int  q_depth, q_dh, q_dm, q_high, q_hh, q_hm;
        long q_ev, q_ref;
        EnterCriticalSection(&m_q_cs);
        q_depth = m_lanes.depth();
        q_dh    = m_lanes.depth_h();
        q_dm    = m_lanes.depth_m();
        q_high  = m_lanes.high_water();
        q_hh    = m_lanes.high_water_h();
        q_hm    = m_lanes.high_water_m();
        q_ev    = m_lanes.evicted();
        q_ref   = m_lanes.refused();
        m_lanes.reset(); // the TRANSPORT boundary: empties the lanes (see m_lanes' note in the header)
        LeaveCriticalSection(&m_q_cs);
        log_queue_rollup(q_depth, q_dh, q_dm, q_high, q_hh, q_hm, q_ev, q_ref);
    }
    m_qhigh_band  = 0;
    m_q_rollup_at = 0;
    // U17: a dead-peer latch minted by the OLD link is not the new one's news, and `id_assigned` is
    // not gated on m_started -- left set, a relinking host-assign client would report "my id is
    // settled" before the new host had said anything, and the WELCOME check above it would be
    // vacuous.
    InterlockedExchange(&m_dead_peer, -1);
    InterlockedExchange(&m_id_assigned, 0);
    m_my_id   = -1;
    m_started = false;
    logf("net: transport stopped -- ready to dial again");
    return true;
}

// ---- conn slots ---------------------------------------------------------------------------------
// A free peer slot, EXCLUDING THE ONES A HANDSHAKE HAS ALREADY CLAIMED. The second half is not
// tidiness: a conn slot only becomes `active` when its client presents its token, so two clients
// that send HELLO within the same round trip would both be handed slot 0 -- and since the T0 conn_id
// is derived from the slot, both would be minted the SAME connection id. The host then demuxes two
// peers onto one conn and the second join silently never completes. Measured on the first run of
// `udploopbacktest`, where the two clients start milliseconds apart: host saw 1 peer, forever.
int Endpoint::alloc_conn_slot() {
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i) {
        if (m_conns[i].active) continue;
        bool claimed = false;
        for (int k = 0; k < MH_NET_MAX_PEERS; ++k)
            if (m_pend[k].used && m_pend[k].slot == (uint8_t)i) claimed = true;
        if (!claimed) return i;
    }
    return -1;
}

void Endpoint::drop_conn(int idx, const char *why) {
    if (InterlockedExchange(&m_conns[idx].tx_dead, 1) != 0) return;
    m_conns[idx].bound = 0; // no more datagrams for this conn_id are ours to open
    InterlockedExchange(&m_conns[idx].active, 0);
    // U17: latch the dropped peer's id for the main thread's in-order removal. The host learns real
    // client ids (>= 1); a client's only conn carries -1, so a host death is not fast-dropped here,
    // which is the same boundary the TCP module draws.
    // mp:U62 -- a hub that is HANDING OVER retires its conns on purpose: those are not peers dying, and
    // the game's fast-drop (U17) would otherwise remove every survivor the hub itself just let go.
    // mp:U64 (HM-M6): the latch is ONE slot, and a hub that loses several clients in the same watchdog pass (its own
    // partition from all of them, found by the 4-peer arm) overwrote the earlier ids, so only the last was ever removed
    // from the roster and the sim stalled on a human that nobody ever dropped. The displaced id waits in m_dead_extra,
    // which take_dead_peer pops once the slot is empty (every caller holds m_conn_cs).
    if (!m_hl.leaving) {
        const LONG prev = InterlockedExchange(&m_dead_peer, m_conns[idx].player_id);
        if (prev >= 0 && prev < 8 && prev != m_conns[idx].player_id) m_dead_extra |= 1u << prev;
    }
    logf("net: udp conn %d dropped -- %s (keepalives: sent %ld, received %ld)", idx, why,
         (long)m_conns[idx].ping_tx, (long)m_conns[idx].ping_rx);
    // mp:X2f -- every caller holds m_conn_cs (the watchdog pass, the recv path, send/send_ctrl), so
    // the channel is touched under the same lock as every other channel-C call. The rc4 host crash
    // was a snapshot transfer outliving its destination here: the index was re-issued to a rejoiner
    // 88 s later, which inherited the stale transfer and drove it into a composer the application
    // was tearing down.
    if (m_bulk.on_conn_dropped(idx))
        logf("net: udp conn %d's channel-C transfer cancelled with it -- its destination is gone, "
             "and the next peer on this index must not inherit it (mp:X2f)",
             idx);
}

// =================================================================================================
// THE HANDSHAKE
//
// Four datagrams, two round trips, and it is the TCP module's mutual PSK proof with T0's connect
// token folded into it. The token is what mp:R1 will keep: a relay mints one instead of the host,
// and only step 2's author changes.
//
//   1. C -> H   PKT_DATA, conn=BOOT, seq=random
//               [CH_HS kind=HELLO : HS_HELLO_LEN bytes = magic | version | client_nonce(16)]
//
//      The host parses it, draws a server nonce, derives the SessionKeys, allocates the peer slot
//      the joiner will occupy, and MINTS a connect token carrying (transport id, conn_id, slot,
//      expiry, those session keys), sealed under the PSK. Minting at HELLO -- before the client has
//      proved anything -- is safe because the token is useless without the keys it is sealed with,
//      and it is what lets the grant ride the challenge instead of costing a third round trip.
//
//   2. H -> C   PKT_DATA, conn=BOOT, seq=random
//               [CH_HS kind=CHALLENGE : HS_CHALLENGE_LEN] [CH_HS kind=GRANT : TOKEN_WIRE]
//
//      The client checks the challenge (the HOST has now proved it holds the PSK), derives the same
//      SessionKeys, and opens the token (which proves the token is this host's and yields conn_id
//      and slot).
//
//   3. C -> H   PKT_TOKEN, conn=<token conn_id>, seq=0.., sealed with the SESSION keys
//               body = token(TOKEN_WIRE) | response(HS_RESPONSE_LEN)
//
//      The host opens the echoed token, requires it to be the one it minted for this peer, and
//      checks the response (the CLIENT has now proved it holds the PSK -- the proof is mutual, as
//      on TCP). It admits the peer: the slot goes live and the stream starts at segment 0.
//
//   4. H -> C   PKT_TOKEN_ACK, conn=<conn_id>, seq=0.., empty body, sealed with the session keys
//
//      Admitted. The client then sends FLAG_HELLO as ordinary stream traffic, exactly as on TCP.
//
// RETRANSMIT AND TIMEOUT. Only the CLIENT retransmits, every HS_RETRY_MS, up to HS_BUDGET_MS in
// total: datagram 1 until a grant arrives, then datagram 3 until the ack does. The host is
// idempotent rather than retransmitting -- a repeated HELLO from a known address re-sends datagram 2
// with the SAME server nonce and the SAME token (so a lost grant is repaired without invalidating
// the client's derivation), and a repeated datagram 3 on an admitted conn re-sends the ack. Every
// client retransmit uses a FRESH sequence, because a repeat of one already accepted is exactly what
// the replay window is there to refuse.
// =================================================================================================

void Endpoint::client_handshake_tick(DWORD now) {
    Pending &p = m_pend[0];
    if (!p.used) return;
    const DWORD budget = m_hs_budget_ms > 0 ? (DWORD)m_hs_budget_ms : HS_BUDGET_MS; // mp:U60: a re-dial may be given longer
    if (now - p.first_ms > budget) {
        p.used = false;
        logf("net: udp handshake FAILED -- no answer from the host within %u ms. Either nothing is "
             "listening behind that address (the host must have a game open), the host is running "
             "transport=tcp while this peer is on udp, or the two of you hold different mh_key.txt.",
             (unsigned)budget);
        return;
    }
    if (p.tries > 0 && now - p.last_tx_ms < HS_RETRY_MS) return;
    if (p.tries > 0) ++m_c.hs_retries;
    p.last_tx_ms = now;
    ++p.tries;

    if (!p.have_token) { // datagram 1
        uint8_t hello[HS_HELLO_LEN];
        hs_build_hello(p.cn, hello);
        uint8_t body[U::MAX_BODY];
        size_t  used = 0;
        uint8_t fr[1 + HS_HELLO_LEN];
        fr[0] = HSK_HELLO;
        memcpy(fr + 1, hello, HS_HELLO_LEN);
        U::frame_append(body, sizeof(body), &used, CH_HS, fr, sizeof(fr));
        send_sealed(p.addr, U::PKT_DATA, m_boot_conn, p.seq++, m_boot_enc, m_boot_mac, body, used);
        if (p.tries == 1) ++m_c.hs_started;
        return;
    }
    // datagram 3, on the conn slot the token named
    EnterCriticalSection(&m_conn_cs);
    Conn   &c = m_conns[0];
    uint8_t body[U::TOKEN_WIRE + HS_RESPONSE_LEN];
    memcpy(body, p.token, U::TOKEN_WIRE);
    hs_build_response(m_psk, p.cn, p.sn, body + U::TOKEN_WIRE);
    send_sealed(p.addr, U::PKT_TOKEN, c.conn_id, c.tx_seq++, c.keys.enc_c2s, c.keys.mac_c2s, body,
                sizeof(body));
    LeaveCriticalSection(&m_conn_cs);
}

void Endpoint::host_on_boot(const sockaddr_in &from, const uint8_t *body, size_t len, DWORD now) {
    // Walk the CH_HS frames; the host only ever expects a HELLO here.
    size_t         off = 0;
    U::Frame       f;
    bool           ok    = true;
    const uint8_t *hello = nullptr;
    while (U::frame_next(body, len, &off, f, ok)) {
        if (f.id != CH_HS || f.len != 1 + HS_HELLO_LEN || f.data[0] != HSK_HELLO) continue;
        hello = f.data + 1;
    }
    if (!ok || hello == nullptr) {
        ++m_c.malformed;
        return;
    }

    uint8_t  cn[NONCE_LEN];
    uint16_t ver = 0;
    if (!hs_parse_hello(hello, cn, ver)) {
        char a[80];
        addr_str(from, a, sizeof(a));
        logf("net: udp handshake from %s -- bad HELLO (magic/version %u, expected %u)", a, ver,
             HS_VERSION);
        return;
    }

    EnterCriticalSection(&m_conn_cs);
    // An address we are already talking to: re-answer with the SAME nonce and token (idempotent).
    Pending *p = nullptr;
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
        if (m_pend[i].used && same_addr(m_pend[i].addr, from)) p = &m_pend[i];
    if (p == nullptr) {
        // One pass: retire the handshakes that timed out, and take the first free slot. Doing both
        // here is what keeps a scanner from holding the table -- an abandoned entry is reclaimed by
        // the next real HELLO rather than by a sweeper nobody runs.
        for (int i = 0; i < MH_NET_MAX_PEERS; ++i) {
            if (m_pend[i].used && now - m_pend[i].first_ms > HS_PEND_MS) m_pend[i].used = false;
            if (!m_pend[i].used && p == nullptr) p = &m_pend[i];
        }
        if (p == nullptr) {
            LeaveCriticalSection(&m_conn_cs);
            logf("net: udp too many handshakes in flight -- dropping a new one");
            return;
        }
        const int slot = alloc_conn_slot();
        if (slot < 0) {
            LeaveCriticalSection(&m_conn_cs);
            logf("net: udp connection refused (full)");
            return;
        }
        memset(p, 0, sizeof(*p));
        p->used     = true;
        p->addr     = from;
        p->first_ms = now;
        p->slot     = (uint8_t)slot;
        memcpy(p->cn, cn, NONCE_LEN);
        if (!MH_Key_Random(p->sn, (unsigned)NONCE_LEN) ||
            !MH_Key_Random((uint8_t *)&p->seq, (unsigned)sizeof(p->seq))) {
            p->used = false;
            LeaveCriticalSection(&m_conn_cs);
            logf("net: udp no secure randomness available, refusing the handshake");
            return;
        }
        hs_derive_keys(m_psk, p->cn, p->sn, p->keys);
        // conn_id: the T0 derivation, scoped by this transport's own 16-byte id. mp:SES0's match_id
        // is the scope mp:R1 will use; it does not exist at transport-start time, so a per-process
        // random id stands in -- the PROPERTY that matters here is that a conn_id is unique per peer
        // per host process, which either scope gives.
        conn_id_from_match(m_transport_id, p->slot, p->conn_id);

        U::ConnectToken tok;
        memset(&tok, 0, sizeof(tok));
        memcpy(tok.match_id, m_transport_id, UUID7_BYTES);
        memcpy(tok.conn_id, p->conn_id, CONN_ID_BYTES);
        tok.slot           = p->slot;
        tok.expire_unix_ms = unix_ms() + TOKEN_TTL_MS;
        tok.keys           = p->keys;
        uint64_t tnonce    = 0;
        MH_Key_Random((uint8_t *)&tnonce, (unsigned)sizeof(tnonce));
        if (U::token_seal(m_psk, tnonce, tok, p->token) == 0) {
            p->used = false;
            LeaveCriticalSection(&m_conn_cs);
            logf("net: udp token mint refused for slot %d -- a bug, not a link fault", (int)p->slot);
            return;
        }
        ++m_c.hs_started;
    }

    // datagram 2: challenge + grant, one datagram, two frames.
    uint8_t chal[HS_CHALLENGE_LEN];
    hs_build_challenge(m_psk, p->cn, p->sn, chal);
    uint8_t out[U::MAX_BODY];
    size_t  used = 0;
    uint8_t f1[1 + HS_CHALLENGE_LEN];
    f1[0] = HSK_CHALLENGE;
    memcpy(f1 + 1, chal, HS_CHALLENGE_LEN);
    uint8_t f2[1 + U::TOKEN_WIRE];
    f2[0] = HSK_GRANT;
    memcpy(f2 + 1, p->token, U::TOKEN_WIRE);
    U::frame_append(out, sizeof(out), &used, CH_HS, f1, sizeof(f1));
    U::frame_append(out, sizeof(out), &used, CH_HS, f2, sizeof(f2));
    const sockaddr_in to  = p->addr;
    const uint64_t    seq = p->seq++;
    LeaveCriticalSection(&m_conn_cs);
    send_sealed(to, U::PKT_DATA, m_boot_conn, seq, m_boot_enc, m_boot_mac, out, used);
}

void Endpoint::client_on_boot(const sockaddr_in &from, const uint8_t *body, size_t len, DWORD now) {
    (void)now;
    Pending &p = m_pend[0];
    if (!p.used || p.have_token) return;

    size_t         off = 0;
    U::Frame       f;
    bool           ok    = true;
    const uint8_t *chal  = nullptr;
    const uint8_t *grant = nullptr;
    while (U::frame_next(body, len, &off, f, ok)) {
        if (f.id != CH_HS || f.len < 1) continue;
        if (f.data[0] == HSK_CHALLENGE && f.len == 1 + HS_CHALLENGE_LEN) chal = f.data + 1;
        if (f.data[0] == HSK_GRANT && f.len == 1 + U::TOKEN_WIRE) grant = f.data + 1;
    }
    if (!ok || chal == nullptr || grant == nullptr) {
        ++m_c.malformed;
        return;
    }

    uint16_t ver = 0;
    if (!hs_check_challenge(m_psk, p.cn, chal, p.sn, ver)) {
        logf("net: udp handshake REJECTED -- the far end answered with a DIFFERENT key (version %u "
             "vs %u). Either it is not the host you meant -- another game instance on that machine "
             "or behind that tunnel answers on the same port with its own mh_key.txt -- or the two "
             "of you really do hold different keys. Compare the host's listen banner with this one "
             "BEFORE copying key files around.",
             ver, HS_VERSION);
        p.used = false;
        return;
    }
    hs_derive_keys(m_psk, p.cn, p.sn, p.keys);

    U::ConnectToken  tok;
    bool             expired = false;
    const U::Verdict v       = U::token_open(m_psk, grant, U::TOKEN_WIRE, unix_ms(), tok, expired);
    if (v != U::Verdict::Ok) {
        logf("net: udp connect token refused (%s%s) -- the host's grant did not verify",
             U::verdict_name(v), expired ? ", expired" : "");
        p.used = false;
        return;
    }

    EnterCriticalSection(&m_conn_cs);
    Conn &c = m_conns[0];
    memset(&c, 0, sizeof(c));
    c.addr      = from;
    c.player_id = -1; // the host's id is unknown and irrelevant to a client (as on TCP)
    memcpy(c.conn_id, tok.conn_id, CONN_ID_BYTES);
    c.keys            = tok.keys;
    c.bound           = 1; // openable now; ADMITTED only when the token ack lands
    c.last_rx         = GetTickCount();
    c.peer_horizon_ms = -1; // mp:SES6 -- nothing pushed yet; see the Conn field's own comment
    c.rx_win.reset();
    memcpy(p.token, grant, U::TOKEN_WIRE);
    memcpy(p.conn_id, tok.conn_id, CONN_ID_BYTES);
    p.slot       = tok.slot;
    p.have_token = true;
    p.tries      = 0;
    p.last_tx_ms = 0;
    LeaveCriticalSection(&m_conn_cs);
    logf("net: udp handshake OK (authenticated + encrypted), slot %d", (int)tok.slot);
}

void Endpoint::host_on_token(const sockaddr_in &from, const U::Header &h, const uint8_t *body,
                             size_t len, DWORD now) {
    if (len != U::TOKEN_WIRE + HS_RESPONSE_LEN) {
        ++m_c.malformed;
        return;
    }
    EnterCriticalSection(&m_conn_cs);
    Pending *p = nullptr;
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
        if (m_pend[i].used && memcmp(m_pend[i].conn_id, h.conn_id, CONN_ID_BYTES) == 0) p = &m_pend[i];
    if (p == nullptr) {
        LeaveCriticalSection(&m_conn_cs);
        ++m_c.wrong_conn;
        return;
    }

    U::ConnectToken tok;
    bool            expired = false;
    if (U::token_open(m_psk, body, U::TOKEN_WIRE, unix_ms(), tok, expired) != U::Verdict::Ok ||
        memcmp(tok.conn_id, p->conn_id, CONN_ID_BYTES) != 0 || tok.slot != p->slot) {
        LeaveCriticalSection(&m_conn_cs);
        logf("net: udp token presented is not the one this host minted%s -- refusing",
             expired ? " (expired)" : "");
        return;
    }
    if (!hs_check_response(m_psk, p->cn, p->sn, body + U::TOKEN_WIRE)) {
        LeaveCriticalSection(&m_conn_cs);
        char a[80];
        addr_str(from, a, sizeof(a));
        logf("net: udp handshake from %s REJECTED -- wrong key (they need this host's mh_key.txt)", a);
        return;
    }

    const int idx = (int)p->slot;
    Conn     &c   = m_conns[idx];
    if (!c.active) {
        memset(&c, 0, sizeof(c));
        c.addr = from;
        memcpy(c.conn_id, p->conn_id, CONN_ID_BYTES);
        c.keys            = p->keys;
        c.bound           = 1;
        c.last_rx         = now;
        c.admitted_ms     = now; // mp:P15 -- the warm-up ping window starts here, once
        c.peer_horizon_ms = -1;  // mp:SES6 -- nothing pushed yet; see the Conn field's own comment
        c.rx_win.reset();
        // N1: the host-assigned id is the first free 1..MAX, independent of what the client claims
        // in its later FLAG_HELLO -- hand-clicked clients all default to 1, and distinct slots are
        // what makes a >2-player lobby seat them apart.
        if (m_host_assign) {
            bool taken[MH_NET_MAX_PEERS + 1] = {false};
            taken[0]                         = true;
            for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
                if (m_conns[i].active && i != idx && m_conns[i].player_id >= 0 &&
                    m_conns[i].player_id <= MH_NET_MAX_PEERS)
                    taken[m_conns[i].player_id] = true;
            c.player_id = -1;
            for (int k = 1; k <= MH_NET_MAX_PEERS; ++k)
                if (!taken[k]) {
                    c.player_id = k;
                    break;
                }
            InterlockedCompareExchange(&m_dead_peer, -1, c.player_id);
            if (c.player_id >= 0 && c.player_id < 8) m_dead_extra &= ~(1u << c.player_id); // mp:U64: a rejoiner is not dead
        } else {
            c.player_id = -1; // learned from the client's FLAG_HELLO
        }
        InterlockedExchange(&c.active, 1);
        ++m_c.hs_done;
        // The handshake is over: release the pending entry so the table does not fill with completed
        // joins. It is freed only AFTER `active` is set, so alloc_conn_slot() above never sees a slot
        // that is neither claimed nor live.
        p->used = false;
        char a[80];
        addr_str(from, a, sizeof(a));
        logf("net: udp accepted %s -> conn %d (player %d)", a, idx, c.player_id);
    } else if (!same_addr(c.addr, from)) {
        // Same conn_id, new address: a NAT rebinding, which is the whole reason T0 puts a connection
        // id in the header. Adopt it -- the token proved who this is, and the address did not.
        char a[80], b[80];
        addr_str(c.addr, a, sizeof(a));
        addr_str(from, b, sizeof(b));
        logf("net: udp conn %d rebound %s -> %s (same connection id)", idx, a, b);
        c.addr = from;
    }
    const sockaddr_in to  = c.addr;
    const uint64_t    seq = c.tx_seq++;
    const int         pid = c.player_id;
    uint8_t           cid[CONN_ID_BYTES];
    memcpy(cid, c.conn_id, CONN_ID_BYTES);
    SessionKeys keys = c.keys;
    // A client in host-assign mode learns its id from the FLAG_WELCOME the TCP module sends, and the
    // same frame is sent here -- over the stream, so it is ordered against everything after it.
    if (m_host_assign && pid >= 0) {
        const uint8_t cap = cap_byte();
        InterlockedIncrement(&m_welcome_tx); // mp:U60: a rehome must never move this
        send_frame(idx, FLAG_WELCOME, (int16_t)m_my_id, (int16_t)pid, m_test_legacy ? nullptr : &cap,
                   m_test_legacy ? 0 : 1);
    }
    LeaveCriticalSection(&m_conn_cs);
    send_sealed(to, U::PKT_TOKEN_ACK, cid, seq, keys.enc_s2c, keys.mac_s2c, nullptr, 0);
}

void Endpoint::client_on_token_ack(const U::Header &h, DWORD now) {
    (void)h;
    Pending &p = m_pend[0];
    if (!p.used) return;
    EnterCriticalSection(&m_conn_cs);
    Conn &c = m_conns[0];
    if (!c.active) {
        InterlockedExchange(&c.active, 1);
        c.last_rx     = now;
        c.admitted_ms = now; // mp:P15 -- the warm-up ping window starts here, once
        ++m_c.hs_done;
        // Announce our player id so the host can route to us before any game data flows. First bytes
        // of the stream, so it cannot arrive after them.
        const uint8_t cap = cap_byte(); // mp:U59/U61: the capability version (Q5: refuse older)
        send_frame(0, FLAG_HELLO, (int16_t)m_my_id, MH_NET_BROADCAST, m_test_legacy ? nullptr : &cap,
                   m_test_legacy ? 0 : 1);
        LeaveCriticalSection(&m_conn_cs);
        p.used = false;
        logf("net: udp CLIENT admitted as player %d", m_my_id);
        return;
    }
    LeaveCriticalSection(&m_conn_cs);
}

// =================================================================================================
// THE RELIABLE ORDERED SEGMENT STREAM
//
// Channel A's payload is T0's `count | newest_step | count x { len | bytes }` with IMPLIED, newest-
// first numbering -- exactly the shape a K-redundant window wants, so "step" here is this stream's
// segment sequence. Each entry is `kind(1) | up to 254 stream bytes`; the kind byte says whether the
// copy was emitted normally or by the retransmit timer, which is the only way the RECEIVER can
// attribute a repair, and attribution is the whole content of the acceptance clause ("redundancy K
// covers it").
// =================================================================================================

// Emit one datagram whose newest entry is `seq` and which also carries the K-1 segments below it.
// Caller holds m_conn_cs.
// `retx` marks the DATAGRAM, not the entry, and that distinction is the whole attribution. Every
// redundant copy is by definition a repeat of something already sent, so deriving the mark from "has
// this segment been sent before" marks all K-1 of them -- and the receiver then credits the
// retransmit timer for every loss the redundancy window actually covered. Measured on the first
// green run: repaired_by_k 0, repaired_by_rto 137, on a link where the K window had done the work.
void Endpoint::emit_segment(int idx, uint32_t seq, bool retx) {
    Conn &c = m_conns[idx];

    // How many of the K we actually have: at the start of a stream there is nothing below segment 0,
    // and nothing below the peer's acknowledged frontier is still worth carrying.
    int k = m_K;
    if ((uint32_t)k > seq + 1) k = (int)(seq + 1);
    if ((int32_t)(seq - c.tx_acked) >= 0 && (uint32_t)k > seq - c.tx_acked + 1)
        k = (int)(seq - c.tx_acked + 1);
    if (k < 1) k = 1;
    if (k > K_MAX) k = K_MAX;

    uint8_t       ebuf[K_MAX][1 + SEG_PAYLOAD];
    U::InputEntry ents[K_MAX];
    for (int i = 0; i < k; ++i) {
        const uint32_t s = seq - (uint32_t)i;
        const Seg     &g = c.tx_ring[s & SEG_MASK];
        ebuf[i][0]       = (uint8_t)(SEG_KIND_STREAM | (retx ? 0x80 : 0x00));
        memcpy(ebuf[i] + 1, g.data, g.len);
        ents[i].step  = s;
        ents[i].bytes = ebuf[i];
        ents[i].len   = (size_t)g.len + 1;
    }

    uint8_t      pay[U::MAX_BODY];
    const size_t plen = U::input_encode(ents, (uint8_t)k, pay, sizeof(pay));
    if (plen == 0) return; // refused by the encoder = a bug here, and silence beats garbage
    uint8_t body[U::MAX_BODY];
    size_t  used = 0;
    if (!U::frame_append(body, sizeof(body), &used, U::CH_INPUT, pay, plen)) return;

    send_sealed(c.addr, U::PKT_DATA, c.conn_id, c.tx_seq++,
                (m_role == 0) ? c.keys.enc_s2c : c.keys.enc_c2s,
                (m_role == 0) ? c.keys.mac_s2c : c.keys.mac_c2s, body, used);
    ++m_c.seg_tx;
    DWORD now = GetTickCount();
    if (now == 0) now = 1; // 0 is this ring's "never sent" sentinel
    c.tx_sent_ms[seq & SEG_MASK] = now;
}

// One new segment of `len` (<= SEG_PAYLOAD) bytes, sent now. Caller holds m_conn_cs and has checked
// the window has room.
void Endpoint::stream_new_segment(int idx, const uint8_t *bytes, size_t len, DWORD now) {
    Conn          &c   = m_conns[idx];
    const uint32_t seq = c.tx_next;
    // The kept drop's clock starts when something becomes outstanding; while the frontier keeps
    // moving, on_bulk_frame restarts it. See udp_endpoint.h's T4b note.
    if (c.tx_next == c.tx_acked) c.tx_stall_since = now ? now : 1u;
    Seg &g = c.tx_ring[seq & SEG_MASK];
    g.len  = (uint16_t)len;
    memcpy(g.data, bytes, len);
    c.tx_sent_ms[seq & SEG_MASK] = 0; // the RTO scan's "never sent" sentinel
    c.tx_next                    = seq + 1;
    emit_segment(idx, seq, false);
}

// Append `len` bytes to the peer's stream. Caller holds m_conn_cs.
//
// mp:T4b -- two paths, and the first is the pre-T4b code exactly: while nothing is queued and the
// window has room, each write goes out in this call, one datagram per new segment, so an open window
// adds no latency at all. What does not fit waits in the backlog (bundled when it is sent, by
// stream_pump) instead of dropping the link -- see udp_endpoint.h for the two runs that showed a full
// window is a peer that is behind, not a dead one. Once anything is queued, every later byte queues
// BEHIND it: a write that overtook the backlog would reorder the stream.
void Endpoint::stream_write(int idx, const uint8_t *bytes, size_t len) {
    Conn       &c   = m_conns[idx];
    const DWORD now = GetTickCount();
    while (len > 0 && c.tx_bl_len == 0 && c.tx_next - c.tx_acked < (uint32_t)SEG_WINDOW) {
        const size_t n = len < (size_t)SEG_PAYLOAD ? len : (size_t)SEG_PAYLOAD;
        stream_new_segment(idx, bytes, n, now);
        if (c.tx_dead) return;
        bytes += n;
        len -= n;
    }
    if (len == 0) return;

    // BACK-PRESSURE. Bounded, so a path that cannot carry the send rate is a named drop rather than
    // a queue that grows until the process runs out of memory.
    if (len > (size_t)(TX_BACKLOG_BYTES - c.tx_bl_len)) {
        drop_conn(idx, "the outbound backlog overflowed -- a full window of unacknowledged segments "
                       "AND 256 KiB queued behind it: the path cannot carry this send rate");
        return;
    }
    if (c.tx_bp_since == 0) {
        c.tx_bp_since   = now ? now : 1u;
        c.tx_bp_peak    = 0;
        c.tx_bp_bundled = 0;
        ++m_c.bp_episodes;
    }
    uint32_t tail = (c.tx_bl_head + c.tx_bl_len) % TX_BACKLOG_BYTES;
    size_t   left = len;
    while (left > 0) {
        const uint32_t room = TX_BACKLOG_BYTES - tail; // contiguous bytes before the ring wraps
        const uint32_t n    = left < (size_t)room ? (uint32_t)left : room;
        memcpy(c.tx_bl + tail, bytes, n);
        bytes += n;
        left -= n;
        tail = (tail + n) % TX_BACKLOG_BYTES;
    }
    c.tx_bl_len += (uint32_t)len;
    if (c.tx_bl_len > c.tx_bp_peak) c.tx_bp_peak = c.tx_bl_len;
    if ((long)c.tx_bl_len > m_c.bp_peak_bytes) m_c.bp_peak_bytes = (long)c.tx_bl_len;
}

// Move queued bytes into the window as it opens, cut into FULL segments -- the bundling. Called
// wherever the window can have opened: the acknowledgement that advanced the frontier, and every
// timer tick as a backstop. Caller holds m_conn_cs.
void Endpoint::stream_pump(int idx, DWORD now) {
    Conn &c = m_conns[idx];
    // PACED, like rto_pass: an acknowledgement that frees 600 slots would otherwise put 600 bundled
    // datagrams on the wire in one go, and the far end's 256-slot inbound ring (MP D24) receives
    // them faster than a lobby drains it. PUMP_BURST per call, and the ack path plus the 20 ms tick
    // both call it, so a backlog still moves at >= 3200 segments/s (~800 KB/s).
    int budget = PUMP_BURST;
    while (c.tx_bl_len > 0 && c.tx_next - c.tx_acked < (uint32_t)SEG_WINDOW && !c.tx_dead &&
           budget-- > 0) {
        uint8_t        seg[SEG_PAYLOAD];
        const uint32_t n = c.tx_bl_len < (uint32_t)SEG_PAYLOAD ? c.tx_bl_len : (uint32_t)SEG_PAYLOAD;
        for (uint32_t i = 0; i < n; ++i) seg[i] = c.tx_bl[(c.tx_bl_head + i) % TX_BACKLOG_BYTES];
        c.tx_bl_head = (c.tx_bl_head + n) % TX_BACKLOG_BYTES;
        c.tx_bl_len -= n;
        ++c.tx_bp_bundled;
        ++m_c.bp_bundled_segs;
        stream_new_segment(idx, seg, n, now);
    }
    if (c.tx_bl_len != 0 || c.tx_bp_since == 0 || c.tx_dead) return;
    // The episode is over. One line per episode would be one line per round trip under a sustained
    // flood, so the line is limited to one a second and says how many episodes it stands for.
    ++m_bp_unlogged;
    if (m_bp_last_log == 0 || now - m_bp_last_log >= 1000u) {
        logf("net: udp conn %d back-pressure released after %u ms -- peak %u B queued behind a full "
             "%d-segment window, sent as %ld bundled segment(s) (%ld episode(s) since the last line)",
             idx, (unsigned)(now - c.tx_bp_since), (unsigned)c.tx_bp_peak, SEG_WINDOW,
             c.tx_bp_bundled, m_bp_unlogged);
        m_bp_last_log = now ? now : 1u;
        m_bp_unlogged = 0;
    }
    c.tx_bp_since = 0;
}

DWORD Endpoint::conn_rto_ms(const Conn &c) const {
    return rto_for(c.stats.rtt.srtt_ms, c.stats.rtt.rttvar_ms, c.stats.rtt.samples);
}

// Frame + queue one record, in the SAME 12-byte WireHdr the TCP module uses. Caller holds m_conn_cs.
void Endpoint::send_frame(int idx, uint16_t flags, int16_t src, int16_t dst, const void *payload,
                          int len) {
    Conn &c = m_conns[idx];
    if (c.tx_dead) return;
    if (len < 0) len = 0;
    if (len > MH_NET_MAX_PAYLOAD) return;
    WireHdr h;
    h.magic = WIRE_MAGIC;
    h.flags = flags;
    h.src   = src;
    h.dst   = dst;
    h.len   = (uint32_t)len;
    uint8_t buf[WIRE_HDR_SIZE + MH_NET_MAX_PAYLOAD];
    wire_hdr_encode(h, buf);
    if (len > 0) memcpy(buf + WIRE_HDR_SIZE, payload, (size_t)len);
    stream_write(idx, buf, WIRE_HDR_SIZE + (size_t)len);
}

// ---- receive side -------------------------------------------------------------------------------
void Endpoint::on_input_frame(int idx, const uint8_t *payload, size_t len) {
    Conn         &c = m_conns[idx];
    U::InputEntry ents[U::INPUT_K_MAX];
    U::Verdict    why   = U::Verdict::Ok;
    const uint8_t count = U::input_decode(payload, len, ents, why);
    if (count == 0) {
        ++m_c.malformed;
        return;
    }
    for (int i = 0; i < (int)count; ++i) {
        const uint32_t s = ents[i].step;
        if (ents[i].len < 1 || ents[i].len > 1 + (size_t)SEG_PAYLOAD) {
            ++m_c.malformed;
            continue;
        }
        const uint8_t kind = ents[i].bytes[0];
        if ((kind & 0x7f) != SEG_KIND_STREAM) continue; // a segment kind from a newer build: skip
        // Below the delivery frontier = a copy of something already consumed. A full window above it
        // = a stream we have fallen too far behind to reassemble, which is a dead link, not reorder.
        if ((int32_t)(s - c.rx_next) < 0) {
            ++m_c.seg_rx_dup;
            continue;
        }
        if (s - c.rx_next >= (uint32_t)SEG_WINDOW) {
            drop_conn(idx, "the inbound stream ran a full window ahead of what we could reassemble");
            return;
        }
        if (c.rx_have[s & SEG_MASK]) {
            ++m_c.seg_rx_dup;
            continue;
        }
        Seg &g = c.rx_ring[s & SEG_MASK];
        g.len  = (uint16_t)(ents[i].len - 1);
        memcpy(g.data, ents[i].bytes + 1, g.len);
        c.rx_have[s & SEG_MASK] = 1;
        ++m_c.seg_rx_new;
        // ATTRIBUTION. A first sighting in a retransmit copy means the K-window did NOT cover the
        // loss; a first sighting at i > 0 means it did (this segment's own datagram never arrived);
        // i == 0 with no retransmit mark is the ordinary case and is not a repair at all. These two
        // counters are what the acceptance clause "redundancy K covers it" is read off.
        if (kind & 0x80) ++m_c.repaired_by_rto;
        else if (i > 0) ++m_c.repaired_by_k;
        if (!c.rx_seen_any || (int32_t)(s - c.rx_top) > 0) {
            c.rx_top      = s;
            c.rx_seen_any = 1;
        }
    }
    stream_drain(idx);
}

// mp:T4b -- the RECEIVER's half of back-pressure. The reorder window holds up to 1024 segments and
// the application's inbound ring 256 must-keep slots, so the repair of one gap can release far more
// frames in one stream_drain than the ring can hold.
//
// mp:U41e CHANGED WHAT "ROOM" MEANS, without changing the guarantee. Before this item the ring was a
// single FIFO and a burst that outran it could force D24's scan to evict a REAL, non-supersedable
// frame when nothing evictable was left (the `ev_unsafe` case) -- which is exactly what this guard
// existed to prevent (measured in the mp:T5 stall arm: a 3.4 s freeze, then a gap repaired with ~700
// segments queued behind it). Now the ring is the SAME sequence-merged lane pair mh_net.dll's TCP
// transport uses (mh_net_queue_policy.h): lane H (bare horizon adverts) NEVER blocks -- a full lane H
// evicts its own head, by design, which is exactly what the lane exists for -- and lane M (everything
// else) is the ONLY lane that can lose a frame, by REFUSING an arrival rather than destroying one
// already queued. So the only capacity this guard needs to protect is lane M's: free slots plus the
// horizons lane H's own auto-eviction is worth is `QUEUE_CAP_M - m_lanes.depth_m()` -- there is no
// second term for evictable frames "in the way" any more, because a full lane H never blocks anything
// downstream of it. Otherwise the stream PAUSES: the segments stay in the reorder window, the
// published frontier stops, and the sender's window fills -- which is back-pressure all the way to
// the writer, and which it now survives. The timer resumes a paused stream every tick. Caller holds
// m_conn_cs (lock order conn -> q, as in enqueue).
static const int INBOUND_HEADROOM = 32; // > the 22 frames one 254-byte segment can complete
bool             Endpoint::inbound_has_room() {
    EnterCriticalSection(&m_q_cs);
    const int room = QUEUE_CAP_M - m_lanes.depth_m();
    LeaveCriticalSection(&m_q_cs);
    return room >= INBOUND_HEADROOM;
}

// Deliver every contiguous segment into the WireHdr reassembler. Caller holds m_conn_cs.
void Endpoint::stream_drain(int idx) {
    Conn         &c          = m_conns[idx];
    const DWORD   now        = GetTickCount();
    const uint8_t was_paused = c.rx_paused;
    c.rx_paused              = 0;
    while (c.rx_have[c.rx_next & SEG_MASK]) {
        if (!inbound_has_room()) {
            // Not a gap: the next segment is HERE, the application is behind. The ledger below is
            // for loss and must not read this as one.
            if (!was_paused) ++m_c.rx_pauses;
            c.rx_paused = 1;
            return;
        }
        Seg &g                          = c.rx_ring[c.rx_next & SEG_MASK];
        c.rx_have[c.rx_next & SEG_MASK] = 0;
        c.rx_next += 1;

        const uint8_t *p    = g.data;
        uint32_t       left = g.len;
        while (left > 0) {
            if (c.asm_need == 0) { // collecting a header
                const uint32_t want = (uint32_t)WIRE_HDR_SIZE - c.asm_used;
                const uint32_t take = left < want ? left : want;
                memcpy(c.asm_buf + c.asm_used, p, take);
                c.asm_used += take;
                p += take;
                left -= take;
                if (c.asm_used < (uint32_t)WIRE_HDR_SIZE) break;
                WireHdr h;
                if (!wire_hdr_decode(c.asm_buf, h) || h.len > (uint32_t)MH_NET_MAX_PAYLOAD) {
                    drop_conn(idx, "bad frame header on the reassembled stream (magic or length)");
                    return;
                }
                if (h.len == 0) { // a header-only frame: dispatch and start the next one
                    c.asm_used = 0;
                    deliver_frame(idx, h, nullptr, 0);
                    if (c.tx_dead) return;
                    continue;
                }
                c.asm_need = h.len;
                continue;
            }
            const uint32_t want = c.asm_need - (c.asm_used - (uint32_t)WIRE_HDR_SIZE);
            const uint32_t take = left < want ? left : want;
            memcpy(c.asm_buf + c.asm_used, p, take);
            c.asm_used += take;
            p += take;
            left -= take;
            if (c.asm_used - (uint32_t)WIRE_HDR_SIZE < c.asm_need) break;
            WireHdr h;
            wire_hdr_decode(c.asm_buf, h);
            const uint32_t plen = c.asm_need;
            c.asm_used          = 0;
            c.asm_need          = 0;
            deliver_frame(idx, h, c.asm_buf + WIRE_HDR_SIZE, plen);
            if (c.tx_dead) return;
        }
    }
    // THE GAP LEDGER. A block that outlives RTO_MS is a stall the K-window did not cover, and it is
    // counted whether or not the retransmit eventually repairs it -- "it recovered" is not the same
    // statement as "loss cost the run nothing", and the acceptance clause asks for the second.
    const bool blocked = c.rx_seen_any && (int32_t)(c.rx_top - c.rx_next) >= 0;
    if (blocked) {
        if (c.gap_since == 0) c.gap_since = now ? now : 1;
    } else if (c.gap_since != 0) {
        const DWORD held = now - c.gap_since;
        if (held >= RTO_MS) {
            ++m_c.gap_stalls;
            if ((long)held > m_c.gap_ms_worst) m_c.gap_ms_worst = (long)held;
            logf("net: udp conn %d stream gap before segment %u held %u ms -- the K=%d redundancy "
                 "window did not cover it, the retransmit did",
                 idx, c.rx_next, held, m_K);
        }
        c.gap_since = 0;
    }
}

// ---- channel B: ping / pong (liveness now; mp:T3 is what CONSUMES the timestamps) ---------------
void Endpoint::on_state_frame(int idx, const uint8_t *payload, size_t len, DWORD now) {
    Conn  &c   = m_conns[idx];
    size_t off = 0;
    while (off + 2 <= len) {
        const uint8_t id   = payload[off];
        const uint8_t rlen = payload[off + 1];
        if (off + 2 + rlen > len) {
            ++m_c.malformed;
            return;
        }
        const uint8_t *r = payload + off + 2;
        if (id == U::REC_PING && rlen == U::REC_PING_LEN) {
            InterlockedIncrement(&c.ping_rx);
            U::PingRecord pong;
            pong.t_origin_ms = (uint32_t)r[0] | ((uint32_t)r[1] << 8) | ((uint32_t)r[2] << 16) |
                               ((uint32_t)r[3] << 24);
            pong.t_echo_ms = (uint32_t)now;
            uint8_t pay[U::REC_LEN_MAX * 2];
            size_t  used = 0;
            U::record_append_ping(pay, sizeof(pay), &used, U::REC_PONG, pong);
            uint8_t body[U::MAX_BODY];
            size_t  bused = 0;
            U::frame_append(body, sizeof(body), &bused, U::CH_STATE, pay, used);
            send_sealed(c.addr, U::PKT_DATA, c.conn_id, c.tx_seq++,
                        (m_role == 0) ? c.keys.enc_s2c : c.keys.enc_c2s,
                        (m_role == 0) ? c.keys.mac_s2c : c.keys.mac_c2s, body, bused);
        } else if (id == U::REC_PONG && rlen == U::REC_PONG_LEN) {
            InterlockedIncrement(&c.ping_rx);
            // mp:T3 -- the round trip. The echoed t_origin is only a MATCH KEY (it is a uint32 of
            // the sender's GetTickCount, so subtracting it would quantise the answer to a system
            // tick); the elapsed time comes from the QPC stamp this endpoint kept when it sent that
            // probe. An echo we have no record of sending returns -1 and is DISCARDED rather than
            // folded in as a zero -- an unmatched pong is a peer replaying, or a probe that aged out
            // of the table, and neither is a measurement.
            const uint32_t origin = (uint32_t)r[0] | ((uint32_t)r[1] << 8) |
                                    ((uint32_t)r[2] << 16) | ((uint32_t)r[3] << 24);
            mh::netstats::EchoMatch m;
            const double            rtt_ms = c.stats.ping.on_echo(origin, qpc_now(), m_qpc_freq, &m);
            if (rtt_ms >= 0.0) c.stats.rtt.sample(rtt_ms);
            // mp:T3b -- THE SECOND CLOCK, and it is here because the first one was doubted for a
            // whole session. The QPC stopwatch is started before the sendto and stopped here, so it
            // can only ever over-report; a rig run nonetheless read ~19 ms BELOW what an independent
            // prober measured on the same VM path. Exactly one of the two had to be wrong, and no
            // amount of reading the code could say which. `t_origin_ms` is OUR OWN GetTickCount at
            // send time, so `GetTickCount() - origin` is a complete second measurement of the same
            // round trip on a different clock -- coarse (a ~15.6 ms quantum, which is why it is not
            // what the estimator eats) but INDEPENDENT of the QPC table this line also prints. The
            // two agreeing is what retired the stamping hypothesis; the two disagreeing would have
            // named the table. Registered as net.udp_rtt_sample.
            const long tick_rtt = (long)(uint32_t)(GetTickCount() - origin);
            logf("net: udp conn %d rtt %ld us (tick clock %ld ms) srtt %ld us rttvar %ld us "
                 "slot %d outstanding %d sample %ld",
                 idx, (long)(rtt_ms * 1000.0), tick_rtt, (long)(c.stats.rtt.srtt_ms * 1000.0),
                 (long)(c.stats.rtt.rttvar_ms * 1000.0), m.slot, m.outstanding,
                 (long)c.stats.rtt.samples);
        }
        off += 2u + rlen;
    }
}

// ---- channel C: three frame kinds, told apart by their first byte --------------------------------
// kind 0 PIECE      mp:T2's bulk chunk fragment
// kind 1 ACK        mp:T1's channel-A SEGMENT STREAM frontier -- the reuse T1 made of a spec'd frame
// kind 2 BULK_ACK   mp:T2's piece acknowledgement (module-local; udp_channel_c.h says why it is a
//                   new kind rather than a reinterpretation of kind 1)
// Anything else is refused as malformed rather than half-handled.
void Endpoint::on_bulk_frame(int idx, const uint8_t *payload, size_t len) {
    if (len >= 1 && payload[0] == U::BULK_PIECE) {
        if (!m_bulk.on_piece(idx, payload, len, GetTickCount())) ++m_c.malformed;
        return;
    }
    if (len >= 1 && payload[0] == bulk::KIND_BULK_ACK) {
        if (!m_bulk.on_ack(idx, payload, len, GetTickCount())) ++m_c.malformed;
        return;
    }
    U::Ack a;
    if (!U::ack_decode(payload, len, a)) {
        ++m_c.malformed;
        return;
    }
    Conn &c = m_conns[idx];
    // mp:T4b -- a frontier past anything we SENT is not an acknowledgement (and would let the window
    // arithmetic below run backwards); it is ignored, as a malformed ack always effectively was.
    if ((int32_t)(a.ack_seq - c.tx_acked) > 0 && a.ack_seq - c.tx_acked <= c.tx_next - c.tx_acked) {
        const DWORD now = GetTickCount();
        c.tx_acked      = a.ack_seq;
        c.rto_backoff   = 0; // mp:T5 -- the path is delivering again
        // PROGRESS restarts the kept drop's clock; nothing outstanding stops it.
        c.tx_stall_since = (c.tx_acked == c.tx_next) ? 0u : (now ? now : 1u);
        stream_pump(idx, now); // the window just opened: send what was waiting for it
    }
}

// mp:T2's one outbound edge. Caller holds m_conn_cs (every caller is on a path that already does).
bool Endpoint::send_bulk_payload(int idx, const uint8_t *payload, size_t len) {
    if (idx < 0 || idx >= MH_NET_MAX_PEERS) return false;
    Conn &c = m_conns[idx];
    if (!c.bound || c.tx_dead) return false;
    uint8_t body[U::MAX_BODY];
    size_t  used = 0;
    if (!U::frame_append(body, sizeof(body), &used, U::CH_BULK, payload, len)) return false;
    return send_sealed(c.addr, U::PKT_DATA, c.conn_id, c.tx_seq++,
                       (m_role == 0) ? c.keys.enc_s2c : c.keys.enc_c2s,
                       (m_role == 0) ? c.keys.mac_s2c : c.keys.mac_c2s, body, used);
}

bool Endpoint::bulk_emit_thunk(void *ctx, int idx, const uint8_t *payload, size_t len) {
    return ((Endpoint *)ctx)->send_bulk_payload(idx, payload, len);
}

void Endpoint::send_ack(int idx, DWORD now) {
    Conn &c = m_conns[idx];
    if (m_ack_mute) return; // mp:T4b's test hook: a peer that talks but never acknowledges
    U::Ack a;
    a.ack_seq = c.rx_next;
    // Faithful to T0's semantics: bit i acknowledges (ack_seq - 1 - i). Everything below the
    // delivery frontier is by definition held, so the bitmap is all ones as far as the stream goes.
    // The SENDER here ignores it -- the frontier alone drives the retransmit -- but it is filled
    // correctly because mp:T2's chunk receiver will read exactly these bits.
    const uint32_t below = c.rx_next < 32u ? c.rx_next : 32u;
    a.ack_bits           = (below >= 32u) ? 0xffffffffu : ((1u << below) - 1u);
    uint8_t pay[U::ACK_LEN];
    if (U::ack_encode(a, pay, sizeof(pay)) == 0) return;
    uint8_t body[U::MAX_BODY];
    size_t  used = 0;
    if (!U::frame_append(body, sizeof(body), &used, U::CH_BULK, pay, U::ACK_LEN)) return;
    send_sealed(c.addr, U::PKT_DATA, c.conn_id, c.tx_seq++,
                (m_role == 0) ? c.keys.enc_s2c : c.keys.enc_c2s,
                (m_role == 0) ? c.keys.mac_s2c : c.keys.mac_c2s, body, used);
    c.last_ack_ms = now;
}

// The backstop the arithmetic in udp_endpoint.h demands. Caller holds m_conn_cs.
void Endpoint::rto_pass(int idx, DWORD now) {
    Conn &c   = m_conns[idx];
    DWORD rto = conn_rto_ms(c) << c.rto_backoff; // mp:T5 -- was the constant RTO_MS; udp_endpoint.h
    if (rto > RTO_MAX_MS) rto = RTO_MAX_MS;
    int n = 0;
    for (uint32_t s = c.tx_acked; s != c.tx_next && n < RTO_BURST; ++s) {
        const DWORD sent = c.tx_sent_ms[s & SEG_MASK];
        if (sent == 0) continue;
        if (now - sent < rto) continue;
        if (s == c.tx_acked && c.rto_backoff < RTO_BACKOFF_MAX) ++c.rto_backoff; // the head timed out
        emit_segment(idx, s, true);
        ++m_c.rto_sent;
        ++n;
        if (c.tx_dead) return;
    }
}

// =================================================================================================
// DISPATCH -- byte-for-byte the TCP module's flag switch, because it is the same WireHdr
// =================================================================================================
void Endpoint::deliver_frame(int idx, const WireHdr &h, const uint8_t *payload, uint32_t len) {
    Conn &c = m_conns[idx];

    if (h.flags == FLAG_PING) return; // the arrival was the point; last_rx is already stamped

    // mp:U60 -- on a rehomed hub a conn that has not yet been seated by an accepted HELLO may say nothing
    // else (its first frame IS the HELLO; anything before it is a peer that skipped the roster check).
    if (m_role == 0 && m_rehomed && c.player_id < 0 && h.flags != FLAG_HELLO) return;
    if (h.flags == FLAG_RECONCILE) { // mp:U60 -- consumed here, never handed to the game or the ctrl callback
        rc_on_frame(idx, h, payload, len);
        return;
    }
    if (h.flags == FLAG_MESH) { // mp:U61 -- the same class: consumed here, never the game queue
        mesh_on_frame(idx, h, payload, len);
        return;
    }

    if (h.flags == FLAG_HELLO) {
        // mp:U59 (user decision Q5): a client without the exactly-once layer is REFUSED at join. Its
        // HELLO carries no capability byte (or an older one); admitting it would put frames on the
        // wire that cannot be sequenced, deduped or reconciled.
        if (m_role == 0 && (len < 1 || payload[0] != osq::CAP_VERSION)) {
            ++m_osq.c.legacy_refused;
            drop_conn(idx, "the peer's HELLO lacks the exactly-once frame layer capability -- an older "
                           "build (mp:U59, refused at join)");
            return;
        }
        // mp:U60 -- a REHOMED hub seats only the survivors it was told about, under the ids they already
        // hold: a HELLO naming anyone else (an id outside the roster, this hub's own id, a negative or
        // out-of-range one) is refused and the conn dropped. A second seat for an id already held is the
        // same survivor re-dialling (its first conn is stale): the old conn goes, the new one is seated.
        if (m_role == 0 && m_rehomed) {
            const int id = h.src;
            if (id < 0 || id >= osq::ORIGINS || id == m_my_id || !m_roster[id]) {
                ++m_rc.foreign_refused;
                char why[160];
                wsprintfA(why, "a HELLO naming player %d, which is not in this hub's roster -- a foreign id "
                               "(mp:U60, refused)",
                          id);
                refuse_conn(idx, why);
                return;
            }
            for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
                if (i != idx && m_conns[i].active && m_conns[i].player_id == id)
                    drop_conn(i, "the same survivor re-dialled; this conn is stale (mp:U60)");
        }
        if (m_host_assign) {
            logf("net: udp conn %d HELLO claims player %d (ignored; assigned %d)", idx, (int)h.src,
                 c.player_id);
        } else {
            c.player_id = h.src;
            InterlockedCompareExchange(&m_dead_peer, -1, h.src);
            if (h.src >= 0 && h.src < 8) m_dead_extra &= ~(1u << h.src); // mp:U64: a rejoiner is not dead
            logf("net: udp conn %d is player %d", idx, (int)h.src);
        }
        return;
    }
    if (h.flags == FLAG_WELCOME) { // N1: the host assigned US an id (client side)
        if (len < 1 || payload[0] != osq::CAP_VERSION) { // mp:U59: an older host (Q5: refuse)
            ++m_osq.c.legacy_refused;
            drop_conn(idx, "the host's WELCOME lacks the exactly-once frame layer capability -- an older "
                           "build (mp:U59, refused at join)");
            return;
        }
        InterlockedIncrement(&m_welcome_rx);
        m_my_id = h.dst;
        m_osq.set_local(m_my_id);
        InterlockedExchange(&m_id_assigned, 1);
        logf("net: udp WELCOME -- host assigned us player %d", m_my_id);
        return;
    }
    if (h.flags == FLAG_SESSION_INFO || h.flags == FLAG_JOIN || h.flags == FLAG_START ||
        h.flags == FLAG_LEAVE || h.flags == FLAG_ANNOUNCE || h.flags == FLAG_HASH) {
        // D21: the HASH sample is RELAYED like a DATA broadcast -- without it a 3-peer run only ever
        // compares each client against the host, and two clients could diverge from each other
        // unseen. The other five are point-to-point control frames, exactly as on TCP.
        if (h.flags == FLAG_HASH && m_role == 0)
            for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
                if (m_conns[i].active && i != idx)
                    send_frame(i, FLAG_HASH, h.src, BROADCAST, payload, (int)len);
        // mp:T2's rig knob: the sample carries the sender's sim step, which is the only step number
        // crossing this transport. `[net] bulk_selftest_mb` off (the ship default) makes this a
        // magic-number check and a return.
        if (h.flags == FLAG_HASH && m_role == 0)
            m_bulk.note_hash_frame(idx, c.player_id, payload, (size_t)len);
        if (m_ctrl) m_ctrl(m_ctrl_ctx, h.flags, h.src, payload, (int)len);
        return;
    }
    // Anything else is a control frame from a NEWER build: ignore it. Treating an unknown flag as
    // FLAG_DATA would push it into the game's lockstep queue as garbage, which is how a
    // forward-compatible protocol turns into a desync (R-live).
    if (h.flags != FLAG_DATA) return;

    // mp:U59 -- the exactly-once prefix. A game frame without it comes from a pre-layer build: refuse
    // the peer (Q5) rather than deliver bytes whose first 7 would be read as game data.
    uint16_t osq_inc = 0;
    uint32_t osq_seq = 0;
    if (!osq::prefix_decode(payload, (int)len, osq_inc, osq_seq)) {
        ++m_osq.c.legacy_refused;
        if (!m_osq_legacy_said) {
            m_osq_legacy_said = true;
            logf("net: udp conn %d sent a game frame without the exactly-once prefix (%u bytes) -- a "
                 "pre-mp:U59 build; refused",
                 idx, len);
        }
        drop_conn(idx, "game frame without the exactly-once prefix -- an older build (mp:U59, refused)");
        return;
    }
    const uint8_t *app     = payload + osq::PREFIX_BYTES;
    const int      app_len = (int)len - osq::PREFIX_BYTES;

    ++m_rx_pkts;
    m_rx_bytes += (long)(WIRE_HDR_SIZE + len);
    m_last_rx_tick = GetTickCount();
    // mp:SES6 -- the per-peer twin of the aggregate stamp just above, narrower than `last_rx` (which
    // any accepted packet moves, keepalives included): this is FLAG_DATA only, so it is what "did the
    // peer keep SENDING game data" actually reads on.
    c.last_data_rx = m_last_rx_tick;
    c.data_rx_bytes += (long)(WIRE_HDR_SIZE + len);
    // UNSEQUENCED: a unicast (seq 0 -- the census found none in a match, only lobby traffic) or a frame
    // whose origin is not a player slot. Delivered as before, with no dedupe and no retention.
    if (osq_seq == 0 || h.dst != BROADCAST || h.src < 0 || h.src >= osq::ORIGINS) {
        if (m_role == 0) host_dispatch(idx, h, payload, (int)len, app, app_len);
        else enqueue(h.src, app, app_len);
        return;
    }
    // SEQUENCED broadcast: dedupe by (origin, seq), deliver per origin in order. A hub forwards every
    // frame that is NEW to it, as received (prefix and all), the moment it arrives -- the receivers
    // run the same layer, so a frame that overtakes a hole waits there for its predecessor.
    const osq::Layer::Rx rx = m_osq.rx(
        h.src, osq_inc, osq_seq, osq_seq, app, app_len, is_bare_horizon(app, app_len), m_last_rx_tick,
        [this](int origin, const uint8_t *d, int n) { enqueue(origin, d, n); });
    if (m_role == 0 && (rx == osq::Layer::Rx::Delivered || rx == osq::Layer::Rx::Ahead))
        for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
            if (m_conns[i].active && i != idx) send_frame(i, FLAG_DATA, h.src, h.dst, payload, (int)len);
}

void Endpoint::host_dispatch(int from_idx, const WireHdr &h, const uint8_t *raw, int raw_len,
                             const uint8_t *app, int app_len) {
    if (h.dst == m_my_id || h.dst == BROADCAST) enqueue(h.src, app, app_len);
    if (h.dst == BROADCAST) {
        for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
            if (m_conns[i].active && i != from_idx)
                send_frame(i, FLAG_DATA, h.src, h.dst, raw, raw_len);
    } else if (h.dst != m_my_id) {
        for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
            if (m_conns[i].active && m_conns[i].player_id == h.dst) {
                send_frame(i, FLAG_DATA, h.src, h.dst, raw, raw_len);
                break;
            }
    }
}

// mp:U41e -- per-match rollup, over THIS endpoint's logf (see the declaration's note in the header).
void Endpoint::log_queue_rollup(int depth, int depth_h, int depth_m, int high, int high_h, int high_m,
                                long evicted, long refused) {
    logf("net: inbound queue rollup (this match): depth %d (H %d / M %d), high-water %d / %d "
         "(H %d / %d, M %d / %d), evicted %ld superseded horizon(s), REFUSED %ld real input(s)",
         depth, depth_h, depth_m, high, QUEUE_CAP, high_h, QUEUE_CAP_H, high_m, QUEUE_CAP_M, evicted,
         refused);
}

// ---- the inbound ring, mp:U41e -- the SAME sequence-merged lane pair as mh_net.dll's, not D24's
// single-FIFO victim scan any more (see udp_endpoint.h's note on m_lanes and inbound_has_room above) --
void Endpoint::enqueue(int src, const void *data, int len) {
    if (len < 0) len = 0;
    if (len > MH_NET_MAX_PAYLOAD) len = MH_NET_MAX_PAYLOAD;
    namespace qp = mh::net::queue_policy;

    bool evicted = false, refused = false;
    int  ev_src   = 0;
    long ev_total = 0, ref_total = 0;
    int  new_high = 0;
    bool rollup   = false;
    int  r_depth = 0, r_dh = 0, r_dm = 0, r_high = 0, r_hh = 0, r_hm = 0;
    long r_ev = 0, r_ref = 0;

    // CLASSIFY ONCE, HERE -- the only call to the router predicate on the inbound path.
    const qp::lane l = qp::lane_of_game_frame(static_cast<const uint8_t *>(data), len);

    EnterCriticalSection(&m_q_cs);
    const qp::push_result r = m_lanes.push(l);
    if (r.accepted) {
        if (r.evicted) {
            evicted  = true;
            ev_src   = m_qh[r.evicted_pos].src; // lane H only -- lane M never evicts
            ev_total = ++m_qdropped;
        }
        if (r.which == qp::lane::supersedable) {
            HMsg &m = m_qh[r.pos];
            m.src   = src;
            if (len) memcpy(m.data, data, (size_t)len); // len == BARE_HORIZON_LEN by construction
        } else {
            Msg &m = m_qm[r.pos];
            m.src  = src;
            m.len  = len;
            if (len) memcpy(m.data, data, (size_t)len);
        }
    } else {
        refused   = true;
        ref_total = m_lanes.refused();
    }
    // Depth high-water, reported in 32-slot steps -- the half of the instrument that can REFUTE.
    {
        const int band = (m_lanes.high_water() / 32) * 32;
        if (band > m_qhigh_band) {
            m_qhigh_band = band;
            new_high     = m_lanes.high_water();
        }
    }
    {
        const DWORD now = GetTickCount();
        if (m_q_rollup_at == 0) m_q_rollup_at = now;
        if (now - m_q_rollup_at >= QUEUE_ROLLUP_MS) {
            m_q_rollup_at = now;
            rollup        = true;
            r_depth       = m_lanes.depth();
            r_dh          = m_lanes.depth_h();
            r_dm          = m_lanes.depth_m();
            r_high        = m_lanes.high_water();
            r_hh          = m_lanes.high_water_h();
            r_hm          = m_lanes.high_water_m();
            r_ev          = m_lanes.evicted();
            r_ref         = m_lanes.refused();
        }
    }
    LeaveCriticalSection(&m_q_cs);

    if (new_high) logf("net: udp inbound queue depth high-water %d / %d", new_high, QUEUE_CAP);
    // A REFUSAL is a correctness event and is logged every time. An EVICTION is bookkeeping: lane H
    // only ever holds frames the next one supersedes, so a badly-behind peer sheds thousands and a
    // line each would bury the one that matters. First occurrence in full, then a rollup.
    if (refused)
        logf("net: *** INBOUND QUEUE: lane M FULL (cap %d) -- REFUSED a real lockstep input from "
             "peer=%d (type=0x%02x len=%d, %ld refused this run). type 0x01 is an ORDER: this peer "
             "will never execute it and WILL desync (MP D24/U41).",
             QUEUE_CAP_M, src, len > 0 ? ((const unsigned char *)data)[0] : 0, len, ref_total);
    else if (evicted && (ev_total == 1 || (ev_total % 256) == 0))
        logf("net: inbound queue: lane H full (cap %d) -- evicted a superseded horizon advertisement "
             "from peer=%d (%ld evicted this run). Orders and control frames are in the other lane "
             "and cannot be reached from here (MP D24/U41).",
             QUEUE_CAP_H, ev_src, ev_total);
    if (rollup) log_queue_rollup(r_depth, r_dh, r_dm, r_high, r_hh, r_hm, r_ev, r_ref);
}

// =================================================================================================
// THE TWO THREADS
// =================================================================================================
DWORD WINAPI Endpoint::recv_thunk(LPVOID p) {
    ((Endpoint *)p)->recv_loop();
    return 0;
}
DWORD WINAPI Endpoint::timer_thunk(LPVOID p) {
    ((Endpoint *)p)->timer_loop();
    return 0;
}

void Endpoint::recv_loop() {
    uint8_t pkt[U::MAX_DATAGRAM + 64];
    while (InterlockedCompareExchange(&m_running, 1, 1)) {
        sockaddr_in from;
        int         flen = sizeof(from);
        const int   n    = recvfrom(m_sock, (char *)pkt, (int)sizeof(pkt), 0, (sockaddr *)&from, &flen);
        if (n == SOCKET_ERROR) {
            const int e = WSAGetLastError();
            // WSAECONNRESET on a UDP socket is an ICMP port-unreachable for a datagram we SENT, not
            // a fault on this receive. SIO_UDP_CONNRESET at start() suppresses it; this is the belt
            // to that braces, because one departed peer ending the host's recv loop would be a
            // transport that dies when anybody quits.
            if (e == WSAEINTR || e == WSAECONNRESET || e == WSAEMSGSIZE) continue;
            break;
        }
        ++m_c.dgram_rx;
        if (lose_it()) {
            ++m_c.dgram_dropped_sim;
            continue;
        }
        {
            bool blocked = false;
            const unsigned short sp = ntohs(from.sin_port);
            for (int i = 0; i < 8; ++i)
                if (m_block_port[i] == (LONG)sp) blocked = true;
            if (blocked) {
                ++m_c.dgram_dropped_sim;
                continue;
            }
        }
        on_datagram(pkt, n, from, GetTickCount());
    }
}

void Endpoint::on_datagram(uint8_t *pkt, int len, const sockaddr_in &from, DWORD now) {
    U::Header peek;
    if (!U::hdr_peek(pkt, (size_t)len, peek)) {
        ++m_c.malformed;
        return;
    }

    // 1. the bootstrap conn: the PSK handshake, opened with the key both ends already had.
    if (memcmp(peek.conn_id, m_boot_conn, CONN_ID_BYTES) == 0 && peek.type == U::PKT_DATA) {
        U::Header        h;
        size_t           blen = 0;
        const U::Verdict v    = U::packet_decode(pkt, (size_t)len, m_boot_conn, m_boot_enc,
                                                 m_boot_mac, nullptr, h, &blen);
        if (v != U::Verdict::Ok) {
            if (v == U::Verdict::BadMac) ++m_c.mac_fail;
            else ++m_c.malformed;
            return;
        }
        if (m_role == 0) host_on_boot(from, pkt + U::HDR_SIZE, blen, now);
        else client_on_boot(from, pkt + U::HDR_SIZE, blen, now);
        return;
    }

    // 2. an established conn, found by CONNECTION ID rather than by address -- which is what lets a
    //    NAT rebinding survive, and is the property T0 put the id in the header for.
    EnterCriticalSection(&m_conn_cs);
    int idx = -1;
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
        if (m_conns[i].bound && memcmp(m_conns[i].conn_id, peek.conn_id, CONN_ID_BYTES) == 0) idx = i;
    if (idx >= 0) {
        Conn            &c = m_conns[idx];
        U::Header        h;
        size_t           blen = 0;
        const U::Verdict v    = U::packet_decode(pkt, (size_t)len, c.conn_id,
                                              (m_role == 0) ? c.keys.enc_c2s : c.keys.enc_s2c,
                                              (m_role == 0) ? c.keys.mac_c2s : c.keys.mac_s2c,
                                                 &c.rx_win, h, &blen);
        if (v != U::Verdict::Ok) {
            LeaveCriticalSection(&m_conn_cs);
            if (v == U::Verdict::BadMac) ++m_c.mac_fail;
            else if (v == U::Verdict::Replay) ++m_c.replay_drop;
            else ++m_c.malformed;
            return;
        }
        InterlockedExchange((volatile LONG *)&c.last_rx, (LONG)now);
        // mp:T3 -- RFC 7680 loss, off the T0 header sequence. Counted HERE, after the MAC and the
        // replay window have both passed, so the population is "datagrams this peer really sent and
        // we really accepted": a hole in it is a datagram the path dropped. Counting before the MAC
        // would fold forgeries into the loss ratio, and counting per channel FRAME would count one
        // datagram once per channel it happened to carry.
        c.stats.loss.on_rx(h.seq);
        if (!same_addr(c.addr, from)) c.addr = from; // rebinding, already authenticated by the MAC
        if (h.type == U::PKT_KEEPALIVE) {
            LeaveCriticalSection(&m_conn_cs);
            return; // the arrival is the whole content
        }
        if (h.type == U::PKT_DISCONNECT) {
            drop_conn(idx, "the peer said it was leaving");
            LeaveCriticalSection(&m_conn_cs);
            return;
        }
        if (h.type == U::PKT_TOKEN) { // a client retransmit of handshake datagram 3: re-ack it
            const sockaddr_in to  = c.addr;
            const uint64_t    seq = c.tx_seq++;
            uint8_t           cid[CONN_ID_BYTES];
            memcpy(cid, c.conn_id, CONN_ID_BYTES);
            const SessionKeys keys = c.keys;
            LeaveCriticalSection(&m_conn_cs);
            send_sealed(to, U::PKT_TOKEN_ACK, cid, seq, keys.enc_s2c, keys.mac_s2c, nullptr, 0);
            return;
        }
        if (h.type == U::PKT_TOKEN_ACK) {
            // NOT NECESSARILY "already admitted". Since the conn becomes OPENABLE with its keys and
            // ADMITTED only here, the ack that admits it arrives on a conn this lookup already
            // matches -- so swallowing it as a duplicate would leave a client that can read the host
            // and can never send to it. (Measured: host saw both peers, both clients read every host
            // broadcast, and the host received nothing from either.)
            const bool admit = (m_role == 1 && !c.active);
            LeaveCriticalSection(&m_conn_cs);
            if (admit) client_on_token_ack(h, now);
            return;
        }
        // PKT_DATA: walk the channel mux.
        size_t   off = 0;
        U::Frame f;
        bool     ok = true;
        while (U::frame_next(pkt + U::HDR_SIZE, blen, &off, f, ok)) {
            if (f.id == U::CH_INPUT) on_input_frame(idx, f.data, f.len);
            else if (f.id == U::CH_STATE) on_state_frame(idx, f.data, f.len, now);
            else if (f.id == U::CH_BULK) on_bulk_frame(idx, f.data, f.len);
            // any other id: a channel this build does not speak. Skipped, per T0's mux rule.
            if (m_conns[idx].tx_dead) break;
        }
        if (!ok) ++m_c.malformed;
        LeaveCriticalSection(&m_conn_cs);
        return;
    }
    LeaveCriticalSection(&m_conn_cs);

    // 2b. mp:U61 -- a MESH PROBE or ECHO from another client: not a conn of ours, but sealed under a
    //     per-sender key derived from the per-match mesh key (mesh.h). Only a client with a key looks.
    // mp:U63 -- a candidate that already BECAME the hub still hears FO_STATE from survivors that have not dialled yet.
    if ((m_role == 1 || m_rehomed) && m_mesh.key_valid && mesh_on_datagram(pkt, len, from, now)) return;

    // 3. handshake datagram 3 (host side) or 4 (client side), on a conn that is not live yet.
    if (m_role == 0 && peek.type == U::PKT_TOKEN) {
        EnterCriticalSection(&m_conn_cs);
        Pending *p = nullptr;
        for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
            if (m_pend[i].used && memcmp(m_pend[i].conn_id, peek.conn_id, CONN_ID_BYTES) == 0)
                p = &m_pend[i];
        SessionKeys keys;
        memset(&keys, 0, sizeof(keys));
        const bool have = (p != nullptr);
        if (have) keys = p->keys;
        LeaveCriticalSection(&m_conn_cs);
        if (!have) {
            ++m_c.wrong_conn;
            return;
        }
        U::Header h;
        size_t    blen = 0;
        // NO REPLAY WINDOW HERE: the conn it would belong to does not exist until this datagram is
        // believed. Freshness comes from the handshake's own challenge-response, and a duplicate is
        // idempotent (it re-sends the ack) rather than admitting anybody twice.
        const U::Verdict v = U::packet_decode(pkt, (size_t)len, peek.conn_id, keys.enc_c2s,
                                              keys.mac_c2s, nullptr, h, &blen);
        if (v != U::Verdict::Ok) {
            if (v == U::Verdict::BadMac) ++m_c.mac_fail;
            else ++m_c.malformed;
            return;
        }
        host_on_token(from, h, pkt + U::HDR_SIZE, blen, now);
        return;
    }
    if (m_role == 1 && peek.type == U::PKT_TOKEN_ACK) {
        Pending &p = m_pend[0];
        if (!p.used || !p.have_token) return;
        if (memcmp(peek.conn_id, p.conn_id, CONN_ID_BYTES) != 0) {
            ++m_c.wrong_conn;
            return;
        }
        U::Header        h;
        size_t           blen = 0;
        const U::Verdict v    = U::packet_decode(pkt, (size_t)len, p.conn_id, p.keys.enc_s2c,
                                                 p.keys.mac_s2c, nullptr, h, &blen);
        if (v != U::Verdict::Ok) {
            if (v == U::Verdict::BadMac) ++m_c.mac_fail;
            else ++m_c.malformed;
            return;
        }
        client_on_token_ack(h, now);
        return;
    }
    ++m_c.wrong_conn;
}

// ---- the timer: handshake retries, pings, acks, retransmits, and the R-live watchdog -------------
//
// The blind-time credit is mh_net_watchdog.h's, unchanged and for the same reason: a watchdog may
// only charge a peer for time it was actually WATCHING. It matters less here than on TCP (a UDP
// sendto does not block for seconds under the conn lock, which is the starvation D16 measured) but a
// suspended VM and a debugger-held process produce exactly the same shape, and the correction is
// free.
void Endpoint::timer_loop() {
    const DWORD LATE_MS   = (DWORD)(m_ping_ms > 0 ? m_ping_ms : 1000);
    DWORD       last_pass = GetTickCount();
    // The repair ledger, into mh_net.log. It exists because the acceptance clause for mp:T1 is a
    // claim about ATTRIBUTION -- "zero stalls attributed to loss, redundancy K covers it" -- and
    // nothing else in a rig run can settle it: mp_pacing_report reads the SIM's own stall flag,
    // which cannot tell a step waiting on a lost datagram from a step waiting on a slow peer. Only
    // the receiver knows which mechanism repaired what, so it is the receiver that has to say so.
    // Printed every CTR_MS and only when a counter actually moved, so an idle link stays silent and
    // the log carries one line per thing that happened rather than a heartbeat nobody reads.
    const DWORD CTR_MS   = 10000;
    DWORD       last_ctr = GetTickCount();
    Counters    prev;
    memset(&prev, 0, sizeof(prev));
    while (InterlockedCompareExchange(&m_running, 1, 1)) {
        Sleep(TICK_MS);
        DWORD       now     = GetTickCount();
        const DWORD elapsed = now - last_pass;
        const DWORD stalled = mh_watchdog_blind_ms(elapsed, TICK_MS, LATE_MS);
        last_pass           = now;

        if (m_role == 1) client_handshake_tick(now);
        if (m_hl.todo_valid && m_hl.todo_go) hl_service(); // mp:U62 -- the survivor's half of a handover, with NO lock held
        if (m_fo.act != FA_NONE) fo_service();             // mp:U63 -- the crash failover's action, with NO lock held

        EnterCriticalSection(&m_conn_cs);
        now = GetTickCount(); // re-sample INSIDE the lock (the 2026-08-30 false-drop race)
        if (stalled) {
            for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
                if (m_conns[i].active && !m_conns[i].tx_dead) {
                    const DWORD credited = m_conns[i].last_rx + stalled;
                    const bool  future   = mh_watchdog_silence_ms(now, credited) == 0u;
                    InterlockedExchange((volatile LONG *)&m_conns[i].last_rx,
                                        (LONG)(future ? now : credited));
                    // mp:T4b -- the kept drop's clock gets the same credit, for the same reason: a
                    // frontier cannot move while THIS process was not running to hear it move.
                    if (m_conns[i].tx_stall_since != 0) {
                        const DWORD cs = m_conns[i].tx_stall_since + stalled;
                        m_conns[i].tx_stall_since =
                            (mh_watchdog_silence_ms(now, cs) == 0u) ? (now ? now : 1u) : cs;
                    }
                }
            logf("net: udp link watchdog was blocked %u ms (a suspend, or a debugger) -- crediting "
                 "that to every peer rather than reading it as silence",
                 stalled);
        }
        for (int i = 0; i < MH_NET_MAX_PEERS; ++i) {
            Conn &c = m_conns[i];
            if (!c.active || c.tx_dead) continue;
            const unsigned silent = mh_watchdog_silence_ms(now, c.last_rx);
            if (mh_watchdog_should_drop(silent, (unsigned)m_rx_timeout_ms)) {
                logf("net: udp conn %d silent %u ms", i, silent);
                drop_conn(i, "no data from peer within the link timeout");
                continue;
            }
            // mp:T4b -- THE KEPT DROP. A full window no longer drops the link (stream_write queues
            // instead); a frontier that has not moved for the link timeout with data outstanding
            // still does, and that covers the peer the silence watchdog above cannot see: one whose
            // pings arrive and whose acknowledgements do not. Off with the watchdog (`rx_timeout_ms`
            // < 0, the debugger setting), where only the backlog's size bound remains -- which is
            // what the pre-T4b full-window rule was, too: a size, not a time.
            if (m_rx_timeout_ms > 0 && c.tx_stall_since != 0 &&
                now - c.tx_stall_since >= (DWORD)m_rx_timeout_ms) {
                char why[200];
                wsprintfA(why,
                          "the peer acknowledged nothing for %u ms with %u segment(s) and %u B "
                          "outstanding (the link timeout)",
                          (unsigned)(now - c.tx_stall_since), (unsigned)(c.tx_next - c.tx_acked),
                          (unsigned)c.tx_bl_len);
                ++m_c.ack_stall_drops;
                drop_conn(i, why);
                continue;
            }
            rto_pass(i, now);
            if (c.tx_dead) continue;
            stream_pump(i, now); // mp:T4b backstop: the ack path pumps too, this catches the rest
            if (c.tx_dead) continue;
            if (c.rx_paused) stream_drain(i); // mp:T4b -- the application may have made room
            if (c.tx_dead) continue;
            // mp:T5 -- channel C's timers follow the same measured round trip as channel A's.
            {
                const DWORD rto  = conn_rto_ms(c);
                const DWORD fast = c.stats.rtt.samples > 0
                                       ? (DWORD)c.stats.rtt.srtt_ms + bulk::FAST_RETX_MS
                                       : 0u;
                m_bulk.set_link_timing(c.stats.rtt.samples > 0 ? rto : 0u, fast);
            }
            m_bulk.tick(i, c.player_id, now); // mp:T2, rate-limited to BULK_BURST pieces per tick
            // An acknowledgement only when there is something to acknowledge or a gap is open: an
            // idle link then costs nothing, and a stalled one tells the sender where to resume.
            if ((c.rx_next != 0 || c.gap_since != 0) && now - c.last_ack_ms >= ACK_MS)
                send_ack(i, now);
            // mp:P15 -- PER-CONN cadence, via the SAME pure function udpstatstest drives offline
            // (udp_ping_cadence.h): FAST_PING_MS for FAST_PING_WINDOW_MS after THIS peer's own
            // admission, the steady m_ping_ms after. `last_ping_ms` is per-conn -- see the field's own
            // note in udp_endpoint.h.
            const bool do_ping = ping_due(m_ping_ms, now - c.admitted_ms, now - c.last_ping_ms,
                                          FAST_PING_MS, FAST_PING_WINDOW_MS);
            if (do_ping) c.last_ping_ms = now;
            if (do_ping) {
                U::PingRecord p;
                p.t_origin_ms = (uint32_t)now;
                // mp:T3 -- STAMP BEFORE THE SEND. The first build stamped after send_sealed, on the
                // argument that our own ChaCha20/Poly1305 work should not be billed to the peer.
                // Measured on the rig (2026-09-18, net_shim at a verified 83 ms round trip): SRTT
                // read 65-68 ms, and at a verified 203 ms it read 185 -- a CONSTANT ~15 ms short,
                // which is one Windows scheduler quantum. The gap is not the crypto, it is that this
                // thread can lose the CPU between the sendto and the stamp, and every millisecond it
                // loses there is subtracted from every RTT the probe goes on to measure. A stamp
                // taken before can only ever over-report by the send's own cost (microseconds);
                // taken after it under-reports by a whole quantum whenever the scheduler feels like
                // it, and an under-reported RTT is the dangerous direction for a latency instrument.
                const int64_t ping_stamp = qpc_now();
                p.t_echo_ms              = 0;
                uint8_t pay[U::REC_LEN_MAX * 2];
                size_t  used = 0;
                U::record_append_ping(pay, sizeof(pay), &used, U::REC_PING, p);
                uint8_t body[U::MAX_BODY];
                size_t  bused = 0;
                U::frame_append(body, sizeof(body), &bused, U::CH_STATE, pay, used);
                send_sealed(c.addr, U::PKT_DATA, c.conn_id, c.tx_seq++,
                            (m_role == 0) ? c.keys.enc_s2c : c.keys.enc_c2s,
                            (m_role == 0) ? c.keys.mac_s2c : c.keys.mac_c2s, body, bused);
                InterlockedIncrement(&c.ping_tx);
                c.stats.ping.on_sent(p.t_origin_ms, ping_stamp);
            } else if (m_ping_ms <= 0 && now - c.last_keep_ms >= NAT_KEEP_MS) {
                // Pings off: still hold the NAT binding open (plan D4 puts the floor at ~2 min).
                send_sealed(c.addr, U::PKT_KEEPALIVE, c.conn_id, c.tx_seq++,
                            (m_role == 0) ? c.keys.enc_s2c : c.keys.enc_c2s,
                            (m_role == 0) ? c.keys.mac_s2c : c.keys.mac_c2s, nullptr, 0);
                c.last_keep_ms = now;
            }
        }
        {
            // mp:U59 -- a hole above the exactly-once frontier that nothing filled within
            // gap_force_ms is skipped and SAID. Normal play never gets here (each hop is a reliable
            // ordered stream); the line exists so a run that did is not silent about it.
            const long skipped = m_osq.c.gap_skipped;
            m_osq.tick(now, [this](int o, const uint8_t *d, int n) { enqueue(o, d, n); });
            if (m_osq.c.gap_skipped != skipped)
                logf("net: udp exactly-once layer SKIPPED %ld hole(s) above an origin's frontier "
                     "(%ld total) -- nothing filled them within %u ms (last: origin %ld seq %ld..%ld)",
                     m_osq.c.gap_skipped - skipped, m_osq.c.gap_skipped, (unsigned)m_osq.gap_force_ms,
                     m_osq.c.skip_origin, m_osq.c.skip_from, m_osq.c.skip_to - 1);
            rc_tick(now); // mp:U60 -- the hub-change reconcile (a no-op until rehome() ran)
            mesh_tick(now); // mp:U61 -- probes, brokering, the matrix, succession epochs
            fo_tick(now);   // mp:U63 -- hub-loss detection, corroboration, the failover's choice
        }
        LeaveCriticalSection(&m_conn_cs);

        if (GetTickCount() - last_ctr >= CTR_MS) {
            last_ctr = GetTickCount();
            if (memcmp(&prev, &m_c, sizeof(prev)) != 0) {
                prev = m_c;
                // mp:SES6 -- ONE PEER SEGMENT PER ACTIVE CONN, appended to the SAME line rather than
                // a row each: report §8/§9.3's cross-matching need was answerable from one peer's log
                // alone once it carries per-direction DATA age (unlike `last_rx`/the aggregate
                // counters above, both moved by keepalives too -- see the Conn field comment), byte
                // counts each way, and the peer's own advertised horizon (mh.dll's push via
                // set_peer_horizon; -1 = nothing pushed, e.g. no lockstep seam bound). -1 also stands
                // for "no DATA yet" on the age columns, the same sentinel style MH_NetPeerLatency's
                // loss_pm already uses. Read outside the lock, same as m_c above: racy, fine for a
                // timing trace (mh_net_export.h's own words for this file's counters).
                char peers[600];
                peers[0]         = '\0';
                const DWORD pnow = GetTickCount();
                for (int i = 0; i < MH_NET_MAX_PEERS; ++i) {
                    const Conn &c = m_conns[i];
                    if (!c.active) continue;
                    const long rx_age = c.last_data_rx ? (long)(pnow - c.last_data_rx) : -1;
                    const long tx_age = c.last_data_tx ? (long)(pnow - c.last_data_tx) : -1;
                    // 200 comfortably covers the worst case: 6 fields, each up to 11 chars for a
                    // signed 32-bit decimal (-2147483648), plus the fixed text around them (~35
                    // chars) -- ~100 max in practice, doubled for headroom since these are COUNTERS
                    // that grow over a match's lifetime, not bounded inputs.
                    char seg[200];
                    wsprintfA(seg,
                              " | peer%d data_rx_age %ld data_tx_age %ld data_rx_bytes %ld "
                              "data_tx_bytes %ld horizon_ms %ld",
                              c.player_id, rx_age, tx_age, c.data_rx_bytes, c.data_tx_bytes,
                              c.peer_horizon_ms);
                    if (lstrlenA(peers) + lstrlenA(seg) < (int)sizeof(peers)) lstrcatA(peers, seg);
                }
                logf("net: udp counters dgram tx %ld rx %ld | seg tx %ld new %ld dup %ld | repaired "
                     "K %ld rto %ld (rto sent %ld) | gap stalls %ld (worst %ld ms) | mac-fail %ld "
                     "replay %ld malformed %ld wrong-conn %ld%s",
                     m_c.dgram_tx, m_c.dgram_rx, m_c.seg_tx, m_c.seg_rx_new, m_c.seg_rx_dup,
                     m_c.repaired_by_k, m_c.repaired_by_rto, m_c.rto_sent, m_c.gap_stalls,
                     m_c.gap_ms_worst, m_c.mac_fail, m_c.replay_drop, m_c.malformed, m_c.wrong_conn,
                     peers);
            }
            // mp:T2's rollup, on the same cadence and the same rule: printed only when something
            // moved. `evicted` is in it because "no chunk was evicted" has to be a number a rig log
            // can be grepped for, not a property asserted in a comment.
            char bl[420];
            if (m_bulk.rollup_line(bl, sizeof(bl))) logf("%s", bl);
        }
    }
}

// =================================================================================================
// THE TRANSPORT SURFACE
// =================================================================================================
int Endpoint::send(int dst_player, const void *buf, int len) {
    // mp:U59 -- the exactly-once prefix costs osq::PREFIX_BYTES of the datagram budget, so the game
    // payload ceiling is that much below MH_NET_MAX_PAYLOAD (mh.exe's packet buffer is 0x3f8).
    if (!m_started || len < 0 || len > MH_NET_MAX_PAYLOAD - osq::PREFIX_BYTES) return 0;
    const bool legacy = m_test_legacy != 0; // test-only: behave as a pre-U59 build (see osq_test_legacy)
    const int  pfx    = legacy ? 0 : osq::PREFIX_BYTES;
    uint8_t    wire[osq::PREFIX_BYTES + MH_NET_MAX_PAYLOAD];
    if (len > 0) memcpy(wire + pfx, buf, (size_t)len);
    const int wlen = len + pfx;
    EnterCriticalSection(&m_conn_cs);
    // mp:SES6 -- per-conn twins of the aggregate tx stamp below, one GetTickCount() for every conn a
    // broadcast fans out to (a peer's OWN "did I send" answer, not a per-datagram timestamp).
    const DWORD now = GetTickCount();
    // mp:U59 -- number a broadcast ONLY if it will actually leave: a client with no live hub conn drops
    // the frame (as it always did), and a consumed seq would be a permanent hole at every receiver.
    uint32_t seq = 0;
    // mp:U60 -- ...and a REHOMING endpoint numbers + retains it even with no live hub conn: the frame is
    // not sent now, but the reconcile serves it from the ring once the new hub is up, so an order issued
    // in the outage is neither lost nor a hole.
    if (!legacy && dst_player == MH_NET_BROADCAST &&
        (m_role == 0 || m_conns[0].active || (m_rehomed && m_rc.active)))
        seq = m_osq.tx(m_my_id, (const uint8_t *)buf, len, is_bare_horizon((const uint8_t *)buf, len), now);
    if (!legacy) osq::prefix_encode(wire, m_osq.incarnation(), seq);
    if (m_role == 0) {
        if (dst_player == MH_NET_BROADCAST) {
            for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
                if (m_conns[i].active) {
                    send_frame(i, FLAG_DATA, (int16_t)m_my_id, BROADCAST, wire, wlen);
                    m_conns[i].last_data_tx = now;
                    m_conns[i].data_tx_bytes += (long)(WIRE_HDR_SIZE + wlen);
                }
        } else {
            for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
                if (m_conns[i].active && m_conns[i].player_id == dst_player) {
                    send_frame(i, FLAG_DATA, (int16_t)m_my_id, (int16_t)dst_player, wire, wlen);
                    m_conns[i].last_data_tx = now;
                    m_conns[i].data_tx_bytes += (long)(WIRE_HDR_SIZE + wlen);
                    break;
                }
        }
    } else if (m_conns[0].active) {
        send_frame(0, FLAG_DATA, (int16_t)m_my_id, (int16_t)dst_player, wire, wlen);
        m_conns[0].last_data_tx = now;
        m_conns[0].data_tx_bytes += (long)(WIRE_HDR_SIZE + wlen);
    }
    LeaveCriticalSection(&m_conn_cs);
    ++m_tx_pkts;
    m_tx_bytes += (long)(WIRE_HDR_SIZE + wlen);
    return 1;
}

// ---- mp:U59: the exactly-once layer's reconcile surface (see the declaration's note) ---------------
void Endpoint::osq_report(osq::Report &out) {
    EnterCriticalSection(&m_conn_cs);
    m_osq.report(out);
    LeaveCriticalSection(&m_conn_cs);
}
int Endpoint::osq_serve(int origin, uint32_t from, uint32_t to, osq_serve_fn fn, void *ctx) {
    EnterCriticalSection(&m_conn_cs);
    const int n = m_osq.serve(origin, from, to,
                              [&](uint16_t inc, uint32_t lo, uint32_t hi, const uint8_t *d, int len, bool sup) {
                                  fn(ctx, inc, lo, hi, d, len, sup);
                              });
    LeaveCriticalSection(&m_conn_cs);
    return n;
}
int Endpoint::osq_ingest(int origin, uint16_t inc, uint32_t lo, uint32_t hi, const uint8_t *data, int len) {
    EnterCriticalSection(&m_conn_cs);
    // The same backpressure the live stream obeys (stream_drain): with the game queue near full, touch
    // NOTHING -- a frame the layer counted as delivered but the queue refused would be lost for good.
    if (!inbound_has_room()) {
        LeaveCriticalSection(&m_conn_cs);
        return -1;
    }
    const osq::Layer::Rx r = m_osq.rx(origin, inc, lo, hi, data, len, is_bare_horizon(data, len), GetTickCount(),
                                      [this](int o, const uint8_t *d, int n) { enqueue(o, d, n); });
    LeaveCriticalSection(&m_conn_cs);
    return (int)r;
}
osq::Counters Endpoint::osq_counters() {
    EnterCriticalSection(&m_conn_cs);
    const osq::Counters c = m_osq.counters();
    LeaveCriticalSection(&m_conn_cs);
    return c;
}
void Endpoint::osq_test_legacy(bool on) { InterlockedExchange(&m_test_legacy, on ? 1 : 0); }
void Endpoint::osq_gap_force(uint32_t ms) {
    EnterCriticalSection(&m_conn_cs);
    m_osq.gap_force_ms = ms;
    LeaveCriticalSection(&m_conn_cs);
}

void Endpoint::send_ctrl(uint16_t flags, const unsigned char *buf, int len) {
    if (!m_started) return;
    if (len < 0 || len > MH_NET_MAX_PAYLOAD) return;
    if (len > 0 && buf == nullptr) len = 0;
    EnterCriticalSection(&m_conn_cs);
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
        if (m_conns[i].active) send_frame(i, flags, (int16_t)m_my_id, BROADCAST, buf, len);
    LeaveCriticalSection(&m_conn_cs);
}

int Endpoint::recv(int *out_sender, void *buf, int *inout_len) {
    if (!m_started || !buf || !inout_len) return 0;
    const int cap = *inout_len;
    int       got = 0;
    EnterCriticalSection(&m_q_cs);
    // mp:U41e -- THE MERGE. `pop()` returns whichever lane's head arrived first, so what comes out of
    // here is the arrival order the single ring used to deliver, minus only what lane H evicted.
    const mh::net::queue_policy::pop_result r = m_lanes.pop();
    if (r.ok) {
        const uint8_t *src_bytes;
        int            src_len, src_from;
        if (r.which == mh::net::queue_policy::lane::supersedable) {
            src_bytes = m_qh[r.pos].data;
            src_len   = mh::net::queue_policy::BARE_HORIZON_LEN;
            src_from  = m_qh[r.pos].src;
        } else {
            src_bytes = m_qm[r.pos].data;
            src_len   = m_qm[r.pos].len;
            src_from  = m_qm[r.pos].src;
        }
        const int n = src_len < cap ? src_len : cap;
        if (n > 0) memcpy(buf, src_bytes, n);
        if (out_sender) *out_sender = src_from;
        *inout_len = n;
        got        = 1;
    }
    LeaveCriticalSection(&m_q_cs);
    return got;
}

// mp:U41e -- the udp twin of net_transport.cpp's MH_Net_QueueMatchBoundary. Called from
// udp_transport.cpp's export, which used to be a bare no-op (mp:U41c/G305: this module carried no
// per-match rollup at all). Same shape as stop()'s half: read under the lock, log after it -- but
// reset_counters(), not reset(): frames already queued are the next match's inputs, and a match
// ending is no reason to destroy them.
void Endpoint::queue_match_boundary() {
    if (!m_cs_ready) return; // never started: no lanes, no counters, nothing to roll up
    int      q_depth, q_dh, q_dm, q_high, q_hh, q_hm;
    long     q_ev, q_ref;
    unsigned epoch;
    long     post_ev, post_ref;
    EnterCriticalSection(&m_q_cs);
    q_depth = m_lanes.depth();
    q_dh    = m_lanes.depth_h();
    q_dm    = m_lanes.depth_m();
    q_high  = m_lanes.high_water();
    q_hh    = m_lanes.high_water_h();
    q_hm    = m_lanes.high_water_m();
    q_ev    = m_lanes.evicted();
    q_ref   = m_lanes.refused();
    m_lanes.reset_counters();
    // mp:U41d/U41e -- read BACK OUT, under the same lock, what reset_counters() just did. `epoch` is
    // a marker only that call can move; post_ev/post_ref are evicted/refused read AFTER the reset,
    // which reset_counters() zeroes -- so they read 0 here iff the call above actually ran.
    epoch         = m_lanes.epoch();
    post_ev       = m_lanes.evicted();
    post_ref      = m_lanes.refused();
    m_qhigh_band  = 0;
    m_q_rollup_at = 0;
    LeaveCriticalSection(&m_q_cs);
    log_queue_rollup(q_depth, q_dh, q_dm, q_high, q_hh, q_hm, q_ev, q_ref);
    logf("net: match boundary -- inbound queue counters restarted (link kept; %d frame(s) still "
         "queued carry over; epoch %u, post-reset evicted %ld / refused %ld)",
         q_depth, epoch, post_ev, post_ref);
}

// mp:U41e -- qmatchtest's read of the lane counters (net_selftest.exe compiles udp_endpoint.cpp
// directly). Under the same lock the writers take. `epoch_out` is the reset marker; pass nullptr
// from a call site that does not need it.
void Endpoint::queue_counters_for_test(int *depth, int *high, long *evicted, long *refused,
                                       unsigned *epoch_out) {
    if (!m_cs_ready) {
        *depth = *high = 0;
        *evicted = *refused = 0;
        if (epoch_out) *epoch_out = 0;
        return;
    }
    EnterCriticalSection(&m_q_cs);
    *depth   = m_lanes.depth();
    *high    = m_lanes.high_water();
    *evicted = m_lanes.evicted();
    *refused = m_lanes.refused();
    if (epoch_out) *epoch_out = m_lanes.epoch();
    LeaveCriticalSection(&m_q_cs);
}

// mp:X2h -- a bare read of lane M's CURRENT depth, no rollup and no log line (see udp_endpoint.h's
// note on the row). Under the same lock the writers take.
int Endpoint::queue_depth_m() {
    if (!m_cs_ready) return 0;
    EnterCriticalSection(&m_q_cs);
    int dm = m_lanes.depth_m();
    LeaveCriticalSection(&m_q_cs);
    return dm;
}

int Endpoint::peer_count() {
    if (!m_started) return 0;
    int n = 0;
    EnterCriticalSection(&m_conn_cs);
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
        if (m_conns[i].active) ++n;
    LeaveCriticalSection(&m_conn_cs);
    return n;
}

int Endpoint::id_assigned() const {
    return InterlockedCompareExchange((volatile LONG *)&m_id_assigned, 0, 0) != 0;
}

int Endpoint::active_peer_ids(int *out, int cap) {
    if (!m_started || !out || cap <= 0) return 0;
    int n = 0;
    EnterCriticalSection(&m_conn_cs);
    for (int i = 0; i < MH_NET_MAX_PEERS && n < cap; ++i)
        if (m_conns[i].active && m_conns[i].player_id >= 1 && m_conns[i].player_id <= 7)
            out[n++] = m_conns[i].player_id;
    LeaveCriticalSection(&m_conn_cs);
    return n;
}

// U17's latch is ONE slot. mp:U63 queues more than one dead peer at a time (the old hub AND a survivor that never
// re-dialled), so the extras wait in a mask and are popped lowest id first once the slot is empty.
int Endpoint::take_dead_peer() {
    const int v = (int)InterlockedExchange(&m_dead_peer, -1);
    if (v >= 0 || !m_cs_ready) return v;
    EnterCriticalSection(&m_conn_cs);
    int r = -1;
    for (int i = 0; i < 8; ++i)
        if (m_dead_extra >> i & 1u) {
            m_dead_extra &= ~(1u << i);
            r = i;
            break;
        }
    LeaveCriticalSection(&m_conn_cs);
    return r;
}

// mp:R3e. `tx_dead` rather than `active` for the conn half: drop_conn sets both, but a conn is
// admitted (`active`) only once its token has been presented, and a peer between the challenge and
// the token is a `m_pend` row -- which the second loop covers, with the same HS_PEND_MS rule
// host_on_hello applies when it reclaims one. Before start() there is no table and nothing is known.
bool Endpoint::knows_addr(const sockaddr_in &a) {
    if (!m_cs_ready) return false;
    const DWORD now   = GetTickCount();
    bool        known = false;
    EnterCriticalSection(&m_conn_cs);
    for (int i = 0; i < MH_NET_MAX_PEERS && !known; ++i)
        if (m_conns[i].active && !m_conns[i].tx_dead && same_addr(m_conns[i].addr, a)) known = true;
    for (int i = 0; i < MH_NET_MAX_PEERS && !known; ++i)
        if (m_pend[i].used && now - m_pend[i].first_ms <= HS_PEND_MS && same_addr(m_pend[i].addr, a))
            known = true;
    LeaveCriticalSection(&m_conn_cs);
    return known;
}

// ---- mp:T2, channel C ---------------------------------------------------------------------------
// The surface is HERE and not in mh_net_udp.def: the module answers a 23-export contract shared with
// mh_net.dll (check_module_bind.py --net-surface), and a bulk row would be a 24th that the TCP
// module cannot answer. So channel C is reachable through this internal header only -- which is the
// same place `udploopbacktest` and `udpbulktest` reach the endpoint from, and exactly what mp:X1 and
// mp:X2 will link against.
bool Endpoint::bulk_send(int dst_player, const void *blob, uint32_t len) {
    if (!m_started) return false;
    bool ok = false;
    EnterCriticalSection(&m_conn_cs);
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i) {
        if (!m_conns[i].active || m_conns[i].player_id != dst_player) continue;
        ok = m_bulk.start_send(i, dst_player, (const uint8_t *)blob, len);
        break;
    }
    LeaveCriticalSection(&m_conn_cs);
    return ok;
}

bool Endpoint::bulk_send_src(int dst_player, bulk::source_fn src, void *ctx, uint32_t len,
                             bool drop_cancels) {
    if (!m_started) return false;
    bool ok = false;
    EnterCriticalSection(&m_conn_cs);
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i) {
        if (!m_conns[i].active || m_conns[i].player_id != dst_player) continue;
        ok = m_bulk.start_send_src(i, dst_player, src, ctx, len, drop_cancels);
        break;
    }
    LeaveCriticalSection(&m_conn_cs);
    return ok;
}

// mp:X2f. THE LOCK IS THE WHOLE POINT. Every read of a composer happens inside a channel call made
// under m_conn_cs (on_ack -> tx_seek -> tx_load on the recv thread; tick on the timer thread;
// start_send_src here), so taking the same lock to clear the pointer is what makes "after this
// returns, nobody is inside the composer and nobody will enter it again" true. Not gated on
// m_started: a stopped endpoint keeps its channel (T1b), and an owner tearing down after a stop must
// still be able to detach. m_cs_ready is the only precondition, exactly as for bulk_recv.
//
// DEADLOCK REVIEW, since this adds a lock acquisition on the main thread: it takes ONLY m_conn_cs,
// holds it across two field writes and no callback, and its caller (MH_Net_SnapshotSend) holds no
// endpoint lock and is documented as reached with map_transfer's leaf lock already released. The
// recv thread re-entering through a handler is a recursive acquire of a CRITICAL_SECTION it owns.
bool Endpoint::bulk_cancel_src(void *ctx) {
    if (!m_cs_ready) return m_bulk.abort_src(ctx);
    EnterCriticalSection(&m_conn_cs);
    const bool was = m_bulk.abort_src(ctx);
    LeaveCriticalSection(&m_conn_cs);
    return was;
}

void Endpoint::bulk_resume_at(uint32_t chunk_index) {
    if (!m_cs_ready) return;
    EnterCriticalSection(&m_conn_cs);
    m_bulk.rx_resume_at(chunk_index);
    LeaveCriticalSection(&m_conn_cs);
}

int Endpoint::bulk_recv(uint32_t *out_chunk_id, void *buf, int *inout_len) {
    if (!m_cs_ready) return 0;
    EnterCriticalSection(&m_conn_cs);
    const int n = m_bulk.recv_chunk(out_chunk_id, buf, inout_len);
    LeaveCriticalSection(&m_conn_cs);
    return n;
}

void Endpoint::bulk_stats(bulk::Stats &out) {
    if (!m_cs_ready) {
        m_bulk.stats(out);
        return;
    }
    EnterCriticalSection(&m_conn_cs);
    m_bulk.stats(out);
    LeaveCriticalSection(&m_conn_cs);
}

void Endpoint::get_stats(MH_NetStats *out) {
    if (!out) return;
    out->tx_pkts      = m_tx_pkts;
    out->tx_bytes     = m_tx_bytes;
    out->rx_pkts      = m_rx_pkts;
    out->rx_bytes     = m_rx_bytes;
    out->last_rx_tick = m_last_rx_tick;
    out->dropped      = m_qdropped;
    out->peers        = peer_count();
    // mp:T3 -- the per-peer latency block. This transport DOES measure its link, so lat_supported is
    // 1 even before any probe has completed: `samples == 0` is how a reader tells "measured nothing
    // yet" from "cannot measure", which is the TCP module's lat_supported = 0.
    out->lat_supported = 1;
    out->lat_count     = 0;
    EnterCriticalSection(&m_conn_cs);
    for (int i = 0; i < MH_NET_MAX_PEERS && out->lat_count < MH_NET_MAX_PEERS; ++i) {
        const Conn &c = m_conns[i];
        if (!c.active || c.tx_dead) continue;
        MH_NetPeerLatency &L = out->lat[out->lat_count++];
        L.player_id          = c.player_id;
        L.samples            = (int)c.stats.rtt.samples;
        L.srtt_us            = (int)(c.stats.rtt.srtt_ms * 1000.0);
        L.rttvar_us          = (int)(c.stats.rtt.rttvar_ms * 1000.0);
        L.ipdv_us            = (int)(c.stats.rtt.ipdv_ms * 1000.0);
        L.loss_pm            = c.stats.loss.loss_pm();
        // mp:L1f -- THE BRIDGE mp:L1e left open, and it needed no handle<->player_id table after
        // all. L1e looked for the crossing in the wrong currency: the relay layer keys everything by
        // its relay HANDLE and this file keys everything by `player_id`, and no table maps one to
        // the other. But the two layers DO already share a key, and have since mp:R3 -- the
        // per-remote LOOPBACK ADDRESS. udp_relay.cpp gives every remote peer its own loopback socket
        // precisely so this Endpoint can tell two clients apart (udp_relay.h reason 2), so
        // `c.addr` IS the relay's `Remote::self` for a tunnelled host, and the client's one conn
        // address IS the tunnel's own dial port. That address is already the currency the reverse
        // question travels in (`knows_addr`, mp:R3e's ownership predicate); this is the same edge
        // asked the other way, and the relay answers from a latch it keeps on the LAST ACCEPTED DATA
        // frame -- last, not first, because mp:R3's rendezvous can promote a pair from relayed to
        // direct mid-session and the lobby must show that when it happens.
        //
        // Still -1 whenever nobody can honestly answer: no classifier wired (a selftest, an older
        // transport), an address the tunnel does not recognise, or a remote that has not delivered
        // a DATA frame yet. -1 renders as the bare number, never a guessed letter (mp:L1e's rule).
        L.relayed = m_path_class ? m_path_class(m_path_class_ctx, c.addr) : -1;
    }
    LeaveCriticalSection(&m_conn_cs);
}

// mp:SES6 -- see the header's comment on this method and mh_net_export.h's note on
// MH_Net_SetPeerHorizon for what `player_id` means and why. A CLIENT ALWAYS APPLIES IT TO m_conns[0]:
// its one conn to the host carries player_id -1 (the declared-id convention host_on_token/
// client_on_boot both set), so matching by player_id would silently drop every push a client ever
// receives -- exactly the client-sees-an-empty-set trap net_lockstep.cpp already documents for
// MH_Net_ActivePeerIds, one level up.
void Endpoint::set_peer_horizon(int player_id, int horizon_ms) {
    EnterCriticalSection(&m_conn_cs);
    if (m_role == 1) {
        if (m_conns[0].bound) m_conns[0].peer_horizon_ms = horizon_ms;
    } else {
        for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
            if (m_conns[i].active && m_conns[i].player_id == player_id) {
                m_conns[i].peer_horizon_ms = horizon_ms;
                break;
            }
    }
    LeaveCriticalSection(&m_conn_cs);
}

// =================================================================================================
// mp:U60 (HM-M2) -- REHOME
//
// The mechanism the host-migration plan (mp:U57, design section 6.1) names and M3..M5
// will drive. Three things happen, in this order, all under m_conn_cs so neither thread ever sees a
// half-switched endpoint:
//
//   1. THE OLD HUB CONNECTION IS RETIRED IN PLACE. drop_conn() on slot 0 -- the one conn a client
//      holds. Nothing else is touched: not the socket (its NAT mapping and port are what the plan's
//      pinhole argument rests on), not the inbound lanes (a frame already queued for the game stays
//      queued -- stop() would have emptied them), not m_my_id (the id is KEPT, never reassigned: the
//      re-binding is the HELLO that carries it), not the exactly-once layer (incarnation, per-origin
//      sequence and retention rings are what the reconcile serves from) and not channel C.
//   2. THE ROLE SWITCHES. A hub: m_role 1 -> 0, a fresh transport id (the scope its conn ids derive
//      from), host_assign OFF -- so host_on_token sends no WELCOME -- and a roster of the ids that may
//      re-dial. A client: a new Pending handshake toward the new hub address; the old id rides in the
//      FLAG_HELLO client_on_token_ack already sends.
//   3. THE RECONCILE RUNS over the new topology (FLAG_RECONCILE, below). osq_gap_force(0) holds for
//      its duration (M1's note): a hole above a frontier must be filled by the reconcile, never
//      skipped by the timeout.
//
// WHY A FRAME TYPE AND NOT A CALLBACK. The reconcile is the endpoint's own business: it needs the
// retention rings, the conn table and the game-queue headroom, all of which live here. A new control
// flag (10) is consumed in deliver_frame and never reaches the game queue or the ctrl callback, so it
// cannot touch determinism, and an endpoint that does not know it ignores it by the receiver rule
// (net_wire.h) -- which is why no capability byte moved.
//
//   RC_REPORT  survivor -> hub    [1][report: version | 8 x (front u32, oldest u32, inc u16)]  82 B
//   RC_FETCH   hub -> holder      [2][origin][from u32][to u32][requester id]                    11 B
//   RC_RANGE   holder -> requester (dst = requester; the hub forwards)  [3][range record]
//   RC_TARGET  hub -> survivor    [4][flags: bit0 = unrecoverable][8 x target u32]               34 B
//
// A survivor REPORTs (again every RC_REPORT_MS while it has not reached its TARGET -- a report is a
// snapshot, and the live stream keeps moving); the hub plans with osq::plan over the reports it holds
// plus its own, FETCHes from each max-holder (serving locally when it is the holder), and sends every
// reporter its TARGET: the per-origin frontier it is done at. A survivor is DONE when every origin's
// frontier is at or past the target and nothing waits above a hole. RANGEs arrive on the ordinary
// conn stream, so stream_drain's headroom rule (INBOUND_HEADROOM free slots before it delivers a
// segment) IS the "osq_ingest returned -1: drain and retry" of M1 -- a range is only ever handed to
// the layer with room for the 17 frames it can release; `ranges_noroom` counts the day that is wrong.
// =================================================================================================
namespace {
constexpr uint8_t RC_REPORT = 1, RC_FETCH = 2, RC_RANGE = 3, RC_TARGET = 4;
constexpr DWORD   RC_REPORT_MS       = 500;   // a survivor re-reports at this cadence until done
constexpr DWORD   RC_ABORT_MS        = 30000; // a reconcile that has not finished by now is given up on
constexpr int     RC_WAIT_MS_DEFAULT = 5000;  // a hub waits this long for the roster's reports
constexpr int     RC_FETCH_CAP       = 64;
inline uint32_t   rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
} // namespace

int Endpoint::conn_of_player(int id) {
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
        if (m_conns[i].active && !m_conns[i].tx_dead && m_conns[i].player_id == id) return i;
    return -1;
}

// Tell a peer it is refused (so its side drops promptly instead of waiting out the link timeout), then
// drop it. Caller holds m_conn_cs.
void Endpoint::refuse_conn(int idx, const char *why) {
    Conn &c = m_conns[idx];
    if (c.bound && !c.tx_dead)
        send_sealed(c.addr, U::PKT_DISCONNECT, c.conn_id, c.tx_seq++,
                    (m_role == 0) ? c.keys.enc_s2c : c.keys.enc_c2s,
                    (m_role == 0) ? c.keys.mac_s2c : c.keys.mac_c2s, nullptr, 0);
    drop_conn(idx, why);
}

unsigned short Endpoint::bound_port() {
    if (!m_started || m_sock == INVALID_SOCKET) return 0;
    sockaddr_in a;
    int         alen = (int)sizeof(a);
    memset(&a, 0, sizeof(a));
    if (getsockname(m_sock, (sockaddr *)&a, &alen) != 0) return 0;
    return ntohs(a.sin_port);
}

bool Endpoint::rehome(const RehomeSpec &sp) {
    if (!m_started || !m_cs_ready) return false;
    sockaddr_in to;
    memset(&to, 0, sizeof(to));
    uint8_t tid[UUID7_BYTES];
    if (sp.as_hub) {
        if (!MH_Key_Random(tid, (unsigned)UUID7_BYTES)) {
            logf("net: udp rehome REFUSED -- no secure randomness available for the hub's transport id");
            return false;
        }
    } else {
        const unsigned long v = sp.host[0] ? inet_addr(sp.host) : INADDR_NONE;
        if (v == INADDR_NONE || sp.port <= 0 || sp.port > 65535) {
            logf("net: udp rehome REFUSED -- the new hub address '%s':%d does not parse", sp.host, sp.port);
            return false;
        }
        to.sin_family      = AF_INET;
        to.sin_port        = htons((u_short)sp.port);
        to.sin_addr.s_addr = v;
    }

    EnterCriticalSection(&m_conn_cs);
    if (m_role != 1 || InterlockedCompareExchange(&m_id_assigned, 0, 0) == 0 || m_my_id < 0 ||
        m_my_id >= osq::ORIGINS) {
        LeaveCriticalSection(&m_conn_cs);
        logf("net: udp rehome REFUSED -- only a started CLIENT that holds a player id can re-home "
             "(role %d, id %d)",
             (int)m_role, m_my_id);
        return false;
    }
    const DWORD now = GetTickCount();

    // 1. retire the old hub connection; keep everything else.
    if (m_conns[0].bound || m_conns[0].active)
        drop_conn(0, "re-homing: the old hub connection is retired in place (mp:U60)");
    memset(m_pend, 0, sizeof(m_pend)); // any handshake still in flight was toward the OLD hub
    InterlockedExchange(&m_dead_peer, -1);
    m_host_assign = false; // ids are KEPT: no WELCOME is ever sent or expected from here on
    m_rehomed     = true;

    // 3 (state). The reconcile: M1's note -- never skip a hole while migrating.
    const uint32_t saved = m_rc.active ? m_rc.saved_gap_ms : m_osq.gap_force_ms;
    memset(&m_rc, 0, sizeof(m_rc));
    m_rc.active        = true;
    m_rc.started_ms    = now;
    m_rc.saved_gap_ms  = saved;
    m_rc.skip          = sp.test_skip_reconcile;
    m_osq.gap_force_ms = 0;

    // 2. the role.
    if (sp.as_hub) {
        memset(m_roster, 0, sizeof(m_roster));
        int n = 0;
        for (int i = 0; i < sp.roster_n && i < MH_NET_MAX_PEERS; ++i) {
            const int id = sp.roster[i];
            if (id >= 0 && id < osq::ORIGINS && id != m_my_id && !m_roster[id]) {
                m_roster[id] = 1;
                ++n;
            }
        }
        memcpy(m_transport_id, tid, UUID7_BYTES);
        m_rc.deadline_ms = now + (DWORD)(sp.reconcile_wait_ms > 0 ? sp.reconcile_wait_ms : RC_WAIT_MS_DEFAULT);
        m_role           = 0;
        logf("net: udp REHOME -- player %d is now the HUB on :%u, socket/lanes/ids/exactly-once state kept; "
             "%d survivor(s) expected, no WELCOME will be sent (mp:U60)",
             m_my_id, (unsigned)bound_port(), n);
    } else {
        m_hs_budget_ms = sp.dial_budget_ms > 0 ? sp.dial_budget_ms : 0;
        Pending &p     = m_pend[0];
        memset(&p, 0, sizeof(p));
        p.used     = true;
        p.addr     = to;
        p.first_ms = now;
        if (!MH_Key_Random(p.cn, (unsigned)NONCE_LEN) ||
            !MH_Key_Random((uint8_t *)&p.seq, (unsigned)sizeof(p.seq))) {
            p.used = false;
            LeaveCriticalSection(&m_conn_cs);
            logf("net: udp rehome REFUSED -- no secure randomness available for the handshake");
            return false;
        }
        logf("net: udp REHOME -- player %d re-dials the new hub %s:%d keeping its id, socket and lanes "
             "(mp:U60)",
             m_my_id, sp.host, sp.port);
    }
    LeaveCriticalSection(&m_conn_cs);
    return true;
}

void Endpoint::rehome_status(RehomeStatus &o) {
    memset(&o, 0, sizeof(o));
    if (!m_cs_ready) return;
    EnterCriticalSection(&m_conn_cs);
    o.rehomed         = m_rehomed;
    o.role            = (int)m_role;
    o.reconciling     = m_rc.active;
    o.done            = m_rc.done;
    o.unrecoverable   = m_rc.unrecoverable;
    o.aborted         = m_rc.aborted;
    o.reports_in      = m_rc.reports_in;
    o.plans           = m_rc.plans;
    o.fetches_issued  = m_rc.fetches_issued;
    o.ranges_served   = m_rc.ranges_served;
    o.ranges_ingested = m_rc.ranges_ingested;
    o.ranges_noroom   = m_rc.ranges_noroom;
    o.foreign_refused = m_rc.foreign_refused;
    o.welcome_tx      = (long)m_welcome_tx;
    o.welcome_rx      = (long)m_welcome_rx;
    for (int i = 0; i < 8; ++i) o.target[i] = m_rc.target[i];
    o.target_valid = m_rc.target_valid;
    LeaveCriticalSection(&m_conn_cs);
}

// True when this endpoint's exactly-once layer is at or past the hub's TARGET on every origin, with no
// frame waiting above a hole. Caller holds m_conn_cs.
bool Endpoint::rc_reached() {
    if (!m_rc.target_valid) return false;
    osq::Report r;
    m_osq.report(r);
    for (int o = 0; o < osq::ORIGINS; ++o) {
        if (r.front[o] < m_rc.target[o]) return false;
        if (m_osq.o[o].ahead_n != 0) return false;
    }
    return true;
}

void Endpoint::rc_finish(const char *why) {
    if (!m_rc.active) return;
    // mp:U63 -- an incomplete reconcile says WHICH origin is short (a crash can leave a hole nobody holds)
    if (!m_rc.done)
        for (int o = 0; o < osq::ORIGINS; ++o) {
            osq::Report r;
            m_osq.report(r);
            if (m_rc.target_valid && (r.front[o] < m_rc.target[o] || m_osq.o[o].ahead_n != 0))
                logf("net: udp reconcile origin %d: front %u target %u ahead %d next %u oldest-retained %u (mp:U63)", o,
                     (unsigned)r.front[o], (unsigned)m_rc.target[o], m_osq.o[o].ahead_n, (unsigned)m_osq.o[o].next,
                     (unsigned)r.oldest[o]);
        }
    m_osq.gap_force_ms = m_rc.saved_gap_ms; // normal play skips a stuck hole again
    m_rc.active        = false;
    logf("net: udp reconcile %s (player %d, role %d: reports %d, plans %d, fetches %d, ranges served %ld "
         "ingested %ld) (mp:U60)",
         why, m_my_id, (int)m_role, m_rc.reports_in, m_rc.plans, m_rc.fetches_issued, m_rc.ranges_served,
         m_rc.ranges_ingested);
}

// Put every retained entry of `origin` in [from, to] on the wire as RC_RANGE frames addressed to
// `to_id` (a hub sends on that survivor's conn; a client sends on its one conn and the hub forwards).
void Endpoint::rc_serve_to(int to_id, int origin, uint32_t from, uint32_t to) {
    const int ci = (m_role == 0) ? conn_of_player(to_id) : (m_conns[0].active ? 0 : -1);
    if (ci < 0) return;
    m_osq.serve(origin, from, to,
                [&](uint16_t inc, uint32_t lo, uint32_t hi, const uint8_t *d, int len, bool) {
                    uint8_t buf[1 + osq::RANGE_HDR_BYTES + osq::PAYLOAD_MAX];
                    if (1 + osq::RANGE_HDR_BYTES + len > MH_NET_MAX_PAYLOAD) return; // cannot be framed
                    buf[0]      = RC_RANGE;
                    const int k = osq::range_encode(buf + 1, (int)sizeof(buf) - 1, origin, inc, lo, hi, d, len);
                    if (k == 0) return;
                    send_frame(ci, FLAG_RECONCILE, (int16_t)m_my_id, (int16_t)to_id, buf, 1 + k);
                    ++m_rc.ranges_served;
                });
}

void Endpoint::rc_ingest_range(const uint8_t *rec, int len) {
    int            origin = 0, dlen = 0;
    uint16_t       inc = 0;
    uint32_t       lo = 0, hi = 0;
    const uint8_t *data = nullptr;
    if (osq::range_decode(rec, len, origin, inc, lo, hi, data, dlen) == 0) {
        ++m_c.malformed;
        return;
    }
    if (!inbound_has_room()) ++m_rc.ranges_noroom; // the stream's headroom rule should make this impossible
    m_osq.rx(origin, inc, lo, hi, data, dlen, is_bare_horizon(data, dlen), GetTickCount(),
             [this](int o, const uint8_t *d, int n) { enqueue(o, d, n); });
    ++m_rc.ranges_ingested;
}

void Endpoint::rc_on_frame(int idx, const WireHdr &h, const uint8_t *payload, uint32_t len) {
    if (!m_rehomed || len < 1) return; // a reconcile frame outside a rehome is nobody's business
    Conn &c = m_conns[idx];
    switch (payload[0]) {
        case RC_REPORT: {
            if (m_role != 0 || c.player_id < 0 || c.player_id >= osq::ORIGINS) return;
            osq::Report r;
            if (!osq::report_decode(payload + 1, (int)len - 1, r)) {
                ++m_c.malformed;
                return;
            }
            m_rc.rep[c.player_id]  = r;
            m_rc.have[c.player_id] = 1;
            m_rc.dirty             = 1;
            ++m_rc.reports_in;
            return;
        }
        case RC_FETCH: {
            if (len != 11) {
                ++m_c.malformed;
                return;
            }
            rc_serve_to((int)payload[10], (int)payload[1], rd32(payload + 2), rd32(payload + 6));
            return;
        }
        case RC_RANGE: {
            if (m_role == 0 && h.dst != m_my_id) { // the hub forwards a range to its requester
                const int ci = conn_of_player(h.dst);
                if (ci >= 0 && ci != idx) send_frame(ci, FLAG_RECONCILE, h.src, h.dst, payload, (int)len);
                return;
            }
            rc_ingest_range(payload + 1, (int)len - 1);
            return;
        }
        case RC_TARGET: {
            if (m_role != 1 || len != 34 || m_rc.done) return;
            for (int o = 0; o < osq::ORIGINS; ++o) m_rc.target[o] = rd32(payload + 2 + 4 * o);
            m_rc.target_valid = true;
            if (payload[1] & 1) {
                m_rc.unrecoverable = true;
                rc_finish("found a needed frame below every holder's retention -- UNRECOVERABLE, the match must end");
            }
            return;
        }
        default: return; // a kind from a newer build
    }
}

// The hub's plan: its own report plus every survivor's latest, through osq::plan. Fetch from each
// max-holder, tell every reporter its TARGET. Idempotent -- a repeat fetches ranges a requester may
// already hold, and the layer dedupes them. Caller holds m_conn_cs.
void Endpoint::rc_plan(DWORD now) {
    (void)now;
    osq::Report rep[osq::ORIGINS + 1];
    int         ids[osq::ORIGINS + 1];
    int         n = 0;
    m_osq.report(rep[n]);
    ids[n++] = m_my_id;
    for (int id = 0; id < osq::ORIGINS; ++id)
        if (m_rc.have[id] && id != m_my_id) {
            rep[n]   = m_rc.rep[id];
            ids[n++] = id;
        }
    osq::Fetch f[RC_FETCH_CAP];
    uint32_t   target[osq::ORIGINS];
    bool       unrec = false;
    const int  nf    = osq::plan(rep, n, f, RC_FETCH_CAP, &unrec, target);
    m_rc.dirty       = 0;
    m_rc.planned     = true;
    ++m_rc.plans;
    m_rc.fetches_issued += nf;
    for (int k = 0; k < nf && k < 8; ++k)
        logf("net: udp reconcile plan: origin %d seq %u..%u from player %d to player %d (target %u) (mp:U63)", (int)f[k].origin,
             (unsigned)f[k].from, (unsigned)f[k].to, ids[f[k].holder], ids[f[k].to_peer], (unsigned)target[f[k].origin]);
    for (int k = 0; k < nf; ++k) {
        const int holder = ids[f[k].holder], req = ids[f[k].to_peer];
        if (holder == m_my_id) {
            rc_serve_to(req, f[k].origin, f[k].from, f[k].to);
        } else {
            const int ci = conn_of_player(holder);
            if (ci < 0) continue;
            uint8_t b[11];
            b[0] = RC_FETCH;
            b[1] = f[k].origin;
            wr32(b + 2, f[k].from);
            wr32(b + 6, f[k].to);
            b[10] = (uint8_t)req;
            send_frame(ci, FLAG_RECONCILE, (int16_t)m_my_id, (int16_t)holder, b, (int)sizeof(b));
        }
    }
    uint8_t t[34];
    t[0] = RC_TARGET;
    t[1] = unrec ? 1 : 0;
    for (int o = 0; o < osq::ORIGINS; ++o) wr32(t + 2 + 4 * o, target[o]);
    for (int i = 1; i < n; ++i) {
        const int ci = conn_of_player(ids[i]);
        if (ci >= 0) send_frame(ci, FLAG_RECONCILE, (int16_t)m_my_id, (int16_t)ids[i], t, (int)sizeof(t));
    }
    for (int o = 0; o < osq::ORIGINS; ++o) m_rc.target[o] = target[o];
    m_rc.target_valid = true;
    if (unrec) {
        m_rc.unrecoverable = true;
        rc_finish("found a needed frame below every holder's retention -- UNRECOVERABLE, the match must end");
    }
}

// mp:U63 -- HOLE REPAIR, for as long as this endpoint has been rehomed (not only while the reconcile is in flight).
// While the reconcile ranges are still arriving, the live stream already delivers the survivor's NEW frames: those
// land above the hole the reconcile is about to fill, the 16-slot ahead window holds only the first of them and the
// rest are dropped for good (the stream delivered them once). A survivor also stops reporting once its own reconcile
// is done. So any origin with frames waiting above a hole for >= 300 ms is asked for [next, next+1023] again: the hub
// asks every connected survivor, a client asks its hub; the serving side sends what its ring retains and the layer
// dedupes the rest. Cheap when nothing waits. Caller holds m_conn_cs.
void Endpoint::rc_repair_holes(DWORD now) {
    if ((DWORD)(now - m_rc.last_repair_ms) < 300) return;
    bool any = false;
    for (int o = 0; o < osq::ORIGINS; ++o) {
        const osq::Layer::Origin &g = m_osq.o[o];
        if (g.ahead_n == 0 || (DWORD)(now - g.ahead_t) < 300) continue;
        any = true;
        uint32_t lowest = 0xffffffffu;
        for (int s = 0; s < osq::AHEAD_SLOTS; ++s)
            if (g.ahead[s].used && g.ahead[s].lo < lowest) lowest = g.ahead[s].lo;
        if (lowest == 0xffffffffu || lowest <= g.next) continue;
        uint8_t b[11];
        b[0] = RC_FETCH;
        b[1] = (uint8_t)o;
        wr32(b + 2, g.next);
        wr32(b + 6, g.next + 1023);
        b[10] = (uint8_t)m_my_id;
        if (m_role == 0) {
            for (int id = 0; id < osq::ORIGINS; ++id) {
                const int ci = (id == m_my_id) ? -1 : conn_of_player(id);
                if (ci < 0) continue;
                send_frame(ci, FLAG_RECONCILE, (int16_t)m_my_id, (int16_t)id, b, (int)sizeof(b));
                ++m_rc.fetches_issued;
            }
        } else if (m_conns[0].active) {
            send_frame(0, FLAG_RECONCILE, (int16_t)m_my_id, BROADCAST, b, (int)sizeof(b));
            ++m_rc.fetches_issued;
        }
    }
    if (any) m_rc.last_repair_ms = now ? now : 1u;
}

void Endpoint::rc_tick(DWORD now) {
    if (!m_rehomed || m_rc.skip) return;
    rc_repair_holes(now);
    if (m_role == 0) {
        int expected = 0, have = 0;
        const bool waited = (int32_t)(now - m_rc.deadline_ms) >= 0;
        for (int id = 0; id < osq::ORIGINS; ++id)
            if (m_roster[id]) {
                // mp:U63 -- a roster id that has neither reported nor connected once the wait is over is a survivor that
                // died with the hub (a crash failover seats ids that may be gone): it is not waited for, or the reconcile
                // could never finish. (A planned handover's roster is all alive: unchanged.)
                if (m_rc.have[id] || !waited || conn_of_player(id) >= 0) ++expected;
                if (m_rc.have[id]) ++have;
            }
        if (m_rc.active && !m_rc.planned && (have == expected || (int32_t)(now - m_rc.deadline_ms) >= 0))
            rc_plan(now);
        else if (m_rc.planned && m_rc.dirty)
            rc_plan(now);
        if (m_rc.active && m_rc.planned && !m_rc.unrecoverable && have == expected && rc_reached()) {
            m_rc.done = true;
            rc_finish("complete");
        }
    } else if (m_rc.active) {
        if (m_conns[0].active && !m_rc.done && (DWORD)(now - m_rc.last_report_ms) >= RC_REPORT_MS) {
            uint8_t p[1 + osq::REPORT_WIRE_BYTES];
            p[0] = RC_REPORT;
            osq::Report r;
            m_osq.report(r);
            osq::report_encode(r, p + 1);
            send_frame(0, FLAG_RECONCILE, (int16_t)m_my_id, BROADCAST, p, (int)sizeof(p));
            m_rc.last_report_ms = now ? now : 1u;
            ++m_rc.reports_in; // on a survivor: reports SENT
        }
        if (m_rc.target_valid && rc_reached()) {
            m_rc.done = true;
            rc_finish("complete");
        }
    }
    if (m_rc.active && (DWORD)(now - m_rc.started_ms) >= RC_ABORT_MS) {
        m_rc.aborted = true;
        rc_finish("ABORTED -- not complete within the abort budget");
    }
}

} // namespace netudp
} // namespace mh
