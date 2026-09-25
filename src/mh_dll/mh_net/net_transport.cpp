//
// MP transport (Phase A0) -- TCP client-server star for the MH multiplayer restoration.
// See the lobby RE -- MP restoration Step 2 (vehicle A, TCP client-server).
//
// mh.exe already carries its own lobby+lockstep protocol (CRC, type-byte dispatch, slot lobby, map
// streaming) -- it is only hollow at the wire. So this module is a DUMB DATAGRAM PIPE: send a buffer
// to a player id (or broadcast), dequeue received buffers tagged with the sender. Topology is a
// client-server STAR: the host listens and RELAYS; clients connect to the host. Reliability +
// ordering are TCP's job (a lockstep order stream needs both; TCP deletes the ARQ that Extermination
// had to build over UDP -- see the Extermination cross-version reference).
//
// A0 scope: the standalone transport + a loopback self-test (net_selftest.cpp). The mh.exe seams
// (llm_net_transport_send/recv @0x0049b635/0x0049b65b, llm_net_send_packet/poll_recv) are wired in
// A1/A2.
//
// Style mirrors harness.cpp: raw Win32 (no STL / heavy CRT), fixed buffers, CRITICAL_SECTION locks
// -- predictable inside the injected DLL. Threads start from MH_Net_InitEx, NOT DllMain (loader
// lock): call it from a lobby-open hook.
//
// ---- WHERE THIS FILE LIVES NOW (fork F4B) -------------------------------------------------------
//
// It used to be mh_common/net_transport.cpp, compiled into mh.dll alongside bmp_io/bnk/lzw/misc --
// a static library with no boundary between its net half and its file-format half. It is now the
// body of mh_net.dll, loaded by mh.dll with LoadLibrary + GetProcAddress from DLL_PROCESS_ATTACH
// (the F4A mechanism, docs/dll-split.md), and the file can simply be absent: mh.dll's forwarding
// shims then answer what this code answers when the transport is not started (mh_net_module.h).
// net_selftest.exe still compiles this TU DIRECTLY -- the suites test the transport, not the
// loader -- so the DLL boundary costs the selftests nothing.
//
// TWO THINGS THIS FILE MAY NOT DO, both of them the satellite rule from docs/dll-split.md:
//   * its own DllMain (mh_net_dllmain.cpp) touches nothing outside this module, and
//   * its STATIC IMPORTS must stay a subset of mh.dll's (KERNEL32, USER32, WINMM, WS2_32). That is
//     what makes loading it from inside another module's DllMain safe, and it is gated:
//     tools/check_module_bind.py --subset reads both import tables with dumpbin.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define _WINSOCK_DEPRECATED_NO_WARNINGS // inet_addr: fine for the direct-connect-by-IP v1 path
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h> // SIO_KEEPALIVE_VALS (per-socket keepalive timing)
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

#include "include/mh_net_export.h"
#include "include/mh_net_key.h"          // mh_key.txt: the pre-shared session key
#include "include/mh_net_module.h"       // F4B: the module contract mh.dll binds this file through
#include "include/mh_net_queue_policy.h" // D24: WHICH inbound frame a full queue may destroy
#include "include/mh_net_watchdog.h"     // D16: the watchdog's timing decisions, as pure testable fns
#include "include/mh_run_context.h"      // mh_log_stamp ONLY -- see module_run_dir() below
#include "mh_net_proto/net_crypto.h"     // handshake + record encryption (portable, shared with the relay)
#include "mh_net_proto/net_wire.h"       // portable wire framing shared with the relay (S0)

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib") // wsprintfA

namespace {

// ---- wire format (shared, portable) -------------------------------------------------------------
// The 12-byte routing header + payload framing now lives in mh_net_proto (net_wire.h) so the headless-
// Linux relay shares it byte-for-byte (it's exactly what the relay reads to route, without parsing the
// mh.exe payload). Alias the names the rest of this file uses; the wire bytes are identical to the old
// x86 packed-struct memcpy, so existing peers interoperate unchanged.
using mh_net_proto::wire_hdr_decode;
using mh_net_proto::wire_hdr_encode;
using mh_net_proto::WireHdr;
constexpr uint16_t MAGIC             = mh_net_proto::WIRE_MAGIC;
constexpr uint16_t FLAG_DATA         = mh_net_proto::FLAG_DATA;
constexpr uint16_t FLAG_HELLO        = mh_net_proto::FLAG_HELLO;
constexpr uint16_t FLAG_SESSION_INFO = mh_net_proto::FLAG_SESSION_INFO;
constexpr uint16_t FLAG_JOIN         = mh_net_proto::FLAG_JOIN;
constexpr uint16_t FLAG_START        = mh_net_proto::FLAG_START;
constexpr uint16_t FLAG_LEAVE        = mh_net_proto::FLAG_LEAVE;
constexpr uint16_t FLAG_ANNOUNCE     = mh_net_proto::FLAG_ANNOUNCE;
constexpr uint16_t FLAG_WELCOME      = mh_net_proto::FLAG_WELCOME;
constexpr uint16_t FLAG_PING         = mh_net_proto::FLAG_PING;
constexpr uint16_t FLAG_HASH         = mh_net_proto::FLAG_HASH;

// ---- role ---------------------------------------------------------------------------------------
constexpr int ROLE_HOST   = 0;
constexpr int ROLE_CLIENT = 1;

// ---- connection table ---------------------------------------------------------------------------
struct Conn {
    SOCKET        sock;
    int           player_id; // host side: learned from HELLO (-1 until then). client: the host's id
    HANDLE        thread;
    volatile LONG active;
    // Secure-link state (empty in open mode). Sequence numbers are per DIRECTION and never sent --
    // both ends count their own, so a replayed, dropped or reordered record fails its MAC.
    bool                      secure;
    mh_net_proto::SessionKeys keys;
    uint64_t                  tx_seq;
    uint64_t                  rx_seq;
    // Liveness (R-live). last_rx is stamped by the recv thread on EVERY inbound frame (control frames
    // included -- a PING is exactly as good a proof of life as game data); tx_dead is the one-shot
    // latch that turns the first failed send into a single log line + one shutdown().
    volatile DWORD last_rx;
    volatile LONG  tx_dead;
    // D16 diagnostics: how many keepalives we PUSHED at this peer and how many arrived. When the
    // watchdog gives up it currently says "no data within the link timeout" without saying how much
    // proof-of-life it sent into that silence -- and "I pinged 10 times and heard nothing" is a
    // different fault from "I never got to ping at all", which is exactly the ambiguity that made
    // the 2026-08-02 relay drops unreadable.
    volatile LONG ping_tx;
    volatile LONG ping_rx;
};

// ---- inbound queue: the SEQUENCE-MERGED LANE PAIR (mp:U41; was one ring + a victim scan, D24) ----
//
// The policy header owns the decision (which lane, which position, what a full lane does); this file
// owns the STORAGE, and splitting it that way is what lets the two lanes have different slot sizes.
//
// THE SIZES ARE A CONSEQUENCE OF THE DESIGN, NOT A NUMBER PICKED UP FRONT -- which is what U41 asked
// for. A lane-H frame is exactly BARE_HORIZON_LEN (9) bytes BY CONSTRUCTION: `lane_of_game_frame`
// admits nothing else into that lane. So lane H does not need the 2048-byte datagram slot the old
// single ring handed every frame, and 4096 of its slots cost ~80 KB.
//
// WHAT THAT BUYS, against the measurement this whole thread is about (D24 run 18): the flood is
// ~1570 frames/s and ~99.5% bare horizons, and the freeze that lost 15 orders was 1844 ms, i.e.
// ~2900 frames. The old 256-slot single ring covered ~160 ms of it -- so it overflowed, and an order
// happening to sit in the evicted prefix is what made the shape fail about 1 run in 30. Lane H at
// 4096 slots covers ~2.6 s of that same flood in 80 KB. Lane M keeps the full-size slot and 256 of
// them (514 KB, exactly the old ring's footprint) but now holds ONLY the ~0.5% that is not a bare
// horizon: at the measured ~8 orders/s that is ~32 seconds of real input before it could refuse.
// Total 594 KB against the old 514 KB, and the coverage goes from 160 ms to seconds.
constexpr int QUEUE_CAP_H = 4096;                      // bare horizons -- the lane that floods
constexpr int QUEUE_CAP_M = 256;                       // everything else -- the lane that must not lose
constexpr int QUEUE_CAP   = QUEUE_CAP_H + QUEUE_CAP_M; // reported depth denominator
struct Msg {
    int     src;
    int     len;
    uint8_t data[MH_NET_MAX_PAYLOAD];
};
// Lane H's slot: the classifier guarantees the length, so the slot is sized by the guarantee.
struct HMsg {
    int     src;
    uint8_t data[mh::net::queue_policy::BARE_HORIZON_LEN];
};

// ---- module state -------------------------------------------------------------------------------
bool             g_started     = false;
int              g_role        = ROLE_HOST;
int              g_my_id       = 0;
bool             g_host_assign = false; // N1: host auto-assigns each joiner its id + WELCOMEs it
volatile LONG    g_id_assigned = 1;     // N1: 1 once THIS peer's id is settled (host: always; assign-client: after WELCOME)
volatile LONG    g_running     = 0;
MH_SessionInfoCb g_si_cb       = nullptr; // discovery: handler for received SESSION_INFO control frames (S2)
MH_JoinCb        g_join_cb     = nullptr; // discovery: handler for received JOIN control frames (S4, host side)
MH_StartCb       g_start_cb    = nullptr; // lobby: handler for received START control frames (U2, client side)
MH_LeaveCb       g_leave_cb    = nullptr; // lobby: handler for received LEAVE control frames (U12, host side)
MH_AnnounceCb    g_announce_cb = nullptr; // lobby: handler for received ANNOUNCE control frames (U16, client side)
MH_HashCb        g_hash_cb     = nullptr; // D21: handler for received desync-sample (HASH) control frames

SOCKET g_listen        = INVALID_SOCKET;
HANDLE g_accept_thread = nullptr;
HANDLE g_watch_thread  = nullptr; // R-live: ping + rx-timeout watchdog

// R-live liveness knobs (see MH_NetConfig). Defaults are deliberately lopsided: we ping often enough
// that silence is unambiguous, and only give up after MANY missed pings, because a false drop is far
// worse than a slow one. 10 s is still well inside the game's own ~6 s in-game presence timeout being
// meaningful, and infinitely better than the "never" it replaces.
int g_ping_ms       = 1000;
int g_rx_timeout_ms = 10000;

CRITICAL_SECTION g_conn_cs;
Conn             g_conns[MH_NET_MAX_PEERS];
volatile LONG    g_dead_peer = -1; // U17: last-dropped peer's player_id (== side_id); one-shot, drained by MH_Net_TakeDeadPeer

CRITICAL_SECTION g_q_cs;
// U41: two storages, one decision-maker. `g_lanes` is pure index+sequence bookkeeping; `g_qm` and
// `g_qh` are the frames, indexed by the position the lane pair hands back.
mh::net::queue_policy::lane_queue<QUEUE_CAP_H, QUEUE_CAP_M> g_lanes;
Msg                                                         g_qm[QUEUE_CAP_M];
HMsg                                                        g_qh[QUEUE_CAP_H];
long                                                        g_dropped = 0;
// D24 instrumentation: the 32-slot band already reported, so the log carries the APPROACH to the cap
// and not only the overflow. The high-water itself now lives in the lane pair (per lane and total).
int g_qhigh_band = 0;
// U41: the periodic rollup. `[desync] STATUS` comes out roughly every 50 s (desync_watch.cpp's
// SAMPLES_PER_STATUS_LINE at the shipped cadence), and this matches it -- often enough that a long
// match carries the approach to the cap, rare enough that it does not bury anything. The counters it
// reports are PER MATCH: net_reset() zeroes them, so a second match in the same process does not
// inherit the first one's high-water or eviction count.
constexpr DWORD QUEUE_ROLLUP_MS = 50000;
DWORD           g_q_rollup_at   = 0;

bool g_log_on = false;
char g_log_path[MAX_PATH];

// ---- THE RUN DIRECTORY, HANDED IN RATHER THAN ASKED FOR (fork F4B / ruling Q1) --------------------
//
// This file used to call MH_RunDir() (mh_common/run_context.cpp) for the one thing it needs a path
// for: where to append mh_net.log. Ruling Q1 keeps run_context.cpp on the MH.DLL side -- 11 of its
// 13 consumers are core or harness code, and config (1) has to keep every log even with no transport
// -- so the module cannot call it any more. There were exactly ONE such call and ZERO MH_ExeDir
// calls (re-measured at F4B's own HEAD; the inherited figure said two).
//
// It is GIVEN the directory by MH_NetModule_Init instead, which is the satellite rule F4A's spike
// established and docs/dll-split.md states: a satellite owns no path, no ini and no file handle of
// its own, because exactly one place in the process knows where this run's logs live. Importing
// MH_RunDir back out of mh.dll would have worked too (the reverse edge is proven) -- it is rejected
// because it puts an outbound dependency on the module for a string that is constant for the run,
// and mh_net.dll's outbound edge is otherwise EMPTY, which is what makes its import table trivially
// checkable against the subset rule.
//
// THE FALLBACK IS THE EXE DIRECTORY, and it is not a nicety: net_selftest.exe compiles this TU
// directly and has no mh.dll to hand it anything, so an un-set run dir must still produce a usable
// log next to the test binary. That is also the pre-2026-08 behaviour of this log, so the fallback
// is a return to a known-good shape rather than an invention.
char g_run_dir[MAX_PATH] = {0};

const char *module_run_dir(void) {
    if (g_run_dir[0] != '\0') return g_run_dir;
    GetModuleFileNameA(nullptr, g_run_dir, MAX_PATH);
    char *slash = g_run_dir;
    for (char *p = g_run_dir; *p != '\0'; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    slash[1] = '\0';
    return g_run_dir;
}

// ---- link security ------------------------------------------------------------------------------
// g_secure is decided once at init from mh_key.txt: with a key every connection must complete the
// PSK handshake before it gets a slot, and every record afterwards is encrypted+authenticated.
// Without one ("open") the wire is byte-identical to the pre-2026-07-25 format, which is both the
// LAN escape hatch and the only way to interoperate with an older DLL.
bool g_secure = false;
// First 8 hex chars of the PSK in use, for the log banners. Which KEY a listener serves with is the
// fact that identifies WHICH INSTALL is answering a joiner -- and on a box that runs both the rig
// (key 4d487465..., "MHtestke...") and an internet host, that is the difference between "your key is
// wrong" and "you are talking to the wrong process". Printed at listen/connect, not just at load.
char          g_key_hex8[9] = {0};
uint8_t       g_psk[mh_net_proto::KEY_LEN];
volatile LONG g_pending = 0; // connections mid-handshake (they do NOT hold a peer slot yet)
// A public port gets scanned. Two cheap caps keep a scanner from costing anything: a bound on
// concurrent handshakes, and a bound on accepts per second. Both are far above real play (a lobby
// fills once) and far below what a flood needs to matter.
constexpr LONG  MAX_PENDING  = 8;
constexpr DWORD HANDSHAKE_MS = 5000; // host side: runs on its own thread, so it can afford to wait
// Client side runs on the LOBBY thread, so its budget is a UI-freeze budget, not a network one. S8
// made connect() non-blocking for exactly this reason (a black-hole IP used to hang the lobby for
// the ~20 s OS timeout); a blocking handshake would have quietly reintroduced a chunk of that. A
// real host answers within one round-trip, so 2 s is generous even on a bad link.
constexpr DWORD HANDSHAKE_CLIENT_MS = 2000;
constexpr int   ACCEPTS_PER_SEC     = 20;
DWORD           g_accept_window     = 0;
int             g_accept_count      = 0;

// ---- diagnostics counters (racy-but-fine for a timing log) --------------------------------------
volatile long  g_tx_pkts = 0, g_rx_pkts = 0;
volatile long  g_tx_bytes = 0, g_rx_bytes = 0;
volatile DWORD g_last_rx_tick = 0; // GetTickCount() at the last inbound DATA frame

// ---- logging ------------------------------------------------------------------------------------
// Every mh_net.log line carries a LOCAL WALL-CLOCK stamp (added 2026-08-06). Absolute, not
// relative-to-first-line like mh_uidrive.log, because the questions this log has to answer are
// cross-process and cross-machine: which host instance was alive when a joiner connected, whether a
// drop lines up with an SSH channel opening, how long a link really survived. Correlating the
// 2026-08-02 internet session needed exactly that and none of the four logs had it, so the analysis
// had to fall back on file mtimes -- which only date the LAST write, and cost a wrong conclusion.
// The format matches mh_tunnel.bat's stamps and the run-directory names.
//
// The stamp goes BEFORE the existing "net: " / "; " prefixes. Substring matching is unaffected;
// the handful of parsers that anchor with startswith() strip it (tools/*.py `_nots`).
// mh_log_stamp() itself lives in include/mh_run_context.h -- shared with the seams' seam_log().
void logf(const char *fmt, ...) {
    if (!g_log_on) return;
    char    line[512];
    int     pre = mh_log_stamp(line);
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(line + pre, fmt, ap);
    va_end(ap);
    int n = lstrlenA(line);
    if (n < (int)sizeof(line) - 2) {
        line[n++] = '\n';
        line[n]   = '\0';
    }
    HANDLE h = CreateFileA(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD wrote = 0;
    WriteFile(h, line, lstrlenA(line), &wrote, nullptr);
    CloseHandle(h);
}

// ---- why a receive stopped ----------------------------------------------------------------------
// One "frame error" string used to cover an orderly FIN, a socket reset, an out-of-range length AND a
// MAC failure alike. That is what made the 2026-07-26 internet drop unattributable after the fact --
// the log said the connection died but not whether the wire, the relay, or the crypto had failed.
// Each cause now names itself, and socket failures carry their WinSock code.
enum RxFail {
    RXF_OK = 0,
    RXF_PEER_CLOSED, // orderly FIN -- the peer (or a relay in the path) closed the stream
    RXF_SOCKET,      // recv() error; `err` carries WSAGetLastError()
    RXF_REC_LEN,     // secure record length outside the legal range
    RXF_MAC,         // authentication failed: corrupt stream, wrong key, or sequence divergence
    RXF_HDR,         // routing header did not decode (bad magic)
    RXF_HDR_LEN,     // header length disagrees with the record it arrived in
    RXF_WATCHDOG,    // no inbound bytes for rx_timeout_ms -- a silently blackholed link
};
const char *rx_fail_str(RxFail f) {
    switch (f) {
        case RXF_PEER_CLOSED: return "peer closed the connection";
        case RXF_SOCKET: return "socket error";
        case RXF_REC_LEN: return "bad record length";
        case RXF_MAC: return "authentication failed (corrupt stream / key or sequence mismatch)";
        case RXF_HDR: return "bad frame header (magic)";
        case RXF_HDR_LEN: return "header length disagrees with the record";
        case RXF_WATCHDOG: return "no data from peer within the link timeout";
        default: return "ok";
    }
}

// ---- blocking full-buffer send/recv (TCP is a stream; frames may split) -------------------------
bool send_all(SOCKET s, const void *buf, int len) {
    const char *p    = static_cast<const char *>(buf);
    int         left = len;
    while (left > 0) {
        int n = send(s, p, left, 0);
        if (n == SOCKET_ERROR || n == 0) return false;
        p += n;
        left -= n;
    }
    return true;
}
// `why`/`err` are optional: the handshake paths log their own context and pass nullptr.
bool recv_all(SOCKET s, void *buf, int len, RxFail *why = nullptr, int *err = nullptr) {
    char *p    = static_cast<char *>(buf);
    int   left = len;
    while (left > 0) {
        int n = recv(s, p, left, 0);
        if (n == 0) { // orderly FIN
            if (why) *why = RXF_PEER_CLOSED;
            if (err) *err = 0;
            return false;
        }
        if (n == SOCKET_ERROR) {
            if (why) *why = RXF_SOCKET;
            if (err) *err = WSAGetLastError();
            return false;
        }
        p += n;
        left -= n;
    }
    return true;
}

// ---- socket options every peer link gets --------------------------------------------------------
// TCP_NODELAY: lockstep frames are small and latency-critical.
// SO_KEEPALIVE (+ SIO_KEEPALIVE_VALS): the OS-level backstop under our own ping -- it is what notices
//   a path that a NAT or an ssh tunnel dropped without ever sending a FIN.
// SO_SNDTIMEO: a send that blocks forever is worse than a failed one, because every send here runs
//   under g_conn_cs -- one wedged peer would stall the game thread's sends to everyone else. Bounded,
//   it becomes an ordinary TX failure, which now drops the connection.
constexpr DWORD SEND_TIMEOUT_MS   = 5000;  // >2 KB max record; generous even on a bad link
constexpr ULONG KEEPALIVE_IDLE_MS = 10000; // start probing after 10 s idle
constexpr ULONG KEEPALIVE_IVL_MS  = 2000;  // then every 2 s
void            link_socket_opts(SOCKET s) {
    BOOL one = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&one, sizeof(one));
    DWORD snd = SEND_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&snd, sizeof(snd));
    struct tcp_keepalive ka;
    ka.onoff             = 1;
    ka.keepalivetime     = KEEPALIVE_IDLE_MS;
    ka.keepaliveinterval = KEEPALIVE_IVL_MS;
    DWORD got            = 0;
    WSAIoctl(s, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &got, nullptr, nullptr);
}

// Retire a connection that we can no longer reach. Callers hold g_conn_cs.
//
// We deliberately do NOT tear it down here. conn_recv_thread owns the teardown (active=0, the U17
// dead-peer latch, closesocket) and must run it exactly once; shutdown() unblocks that thread
// immediately, so the single existing path does the work and there is no double-close race. The
// latch makes the log line appear once even if a dozen queued sends fail together.
void drop_conn(int idx, RxFail why, int err) {
    if (InterlockedExchange(&g_conns[idx].tx_dead, 1) != 0) return;
    if (err) logf("net: conn %d dropped -- %s (winsock %d)", idx, rx_fail_str(why), err);
    else logf("net: conn %d dropped -- %s", idx, rx_fail_str(why));
    shutdown(g_conns[idx].sock, SD_BOTH);
}

// Frame + transmit one record. Serialized per-connection by the caller (holds g_conn_cs) so two
// threads never interleave on the same socket -- which for a SECURE link is load-bearing beyond
// interleaving: tx_seq must advance in the same order the bytes hit the wire, or the peer's
// implicit sequence diverges and every later record fails its MAC.
//
// Takes the connection INDEX, not the Conn&, so that a failure can retire the right slot here rather
// than at a dozen call sites. Every caller used to discard the bool: the host in the 2026-07-26
// internet game pushed an advertisement into a dead socket every 50 ms for 90 s, learned nothing, and
// went on to start the match. A send that fails means the peer is gone -- treat it as such.
bool send_frame(int idx, uint16_t flags, int16_t src, int16_t dst, const void *payload, int len) {
    Conn   &c = g_conns[idx];
    WireHdr h;
    h.magic = MAGIC;
    h.flags = flags;
    h.src   = src;
    h.dst   = dst;
    h.len   = (uint32_t)len;
    uint8_t hdr[mh_net_proto::WIRE_HDR_SIZE];
    wire_hdr_encode(h, hdr);

    if (!c.secure) { // open mode: the historic two-part write, byte-identical to the old format
        if (!send_all(c.sock, hdr, sizeof(hdr)) || (len > 0 && !send_all(c.sock, payload, len))) {
            drop_conn(idx, RXF_SOCKET, WSAGetLastError());
            return false;
        }
        return true;
    }

    // Secure: one sealed record carrying header+payload, so an observer sees neither the routing
    // fields nor the game data -- only a length.
    uint8_t plain[mh_net_proto::WIRE_HDR_SIZE + MH_NET_MAX_PAYLOAD];
    memcpy(plain, hdr, sizeof(hdr));
    if (len > 0) memcpy(plain + sizeof(hdr), payload, len);
    uint32_t       plen = (uint32_t)(sizeof(hdr) + (len > 0 ? len : 0));
    uint8_t        rec[mh_net_proto::REC_LEN_SIZE + mh_net_proto::WIRE_HDR_SIZE + MH_NET_MAX_PAYLOAD +
                mh_net_proto::MAC_LEN];
    const uint8_t *ek = (g_role == ROLE_HOST) ? c.keys.enc_s2c : c.keys.enc_c2s;
    const uint8_t *mk = (g_role == ROLE_HOST) ? c.keys.mac_s2c : c.keys.mac_c2s;
    size_t         n  = mh_net_proto::rec_seal(ek, mk, c.tx_seq++, plain, plen, rec);
    if (!send_all(c.sock, rec, (int)n)) {
        drop_conn(idx, RXF_SOCKET, WSAGetLastError());
        return false;
    }
    return true;
}

// Receive one record into (h, payload). Mirrors send_frame; returns false on a dead socket, a
// malformed length, or -- on a secure link -- a MAC failure, which the caller treats as a dropped
// connection. There is deliberately no "retry": a record that does not authenticate means the
// stream is either corrupt or under attack, and neither is recoverable by continuing.
bool recv_frame(Conn &c, WireHdr &h, uint8_t *payload, uint32_t &out_len, RxFail &why, int &err) {
    why = RXF_OK;
    err = 0;
    if (!c.secure) {
        uint8_t hdr[mh_net_proto::WIRE_HDR_SIZE];
        if (!recv_all(c.sock, hdr, sizeof(hdr), &why, &err)) return false;
        if (!wire_hdr_decode(hdr, h)) return why = RXF_HDR, false;
        if (h.len > MH_NET_MAX_PAYLOAD) return why = RXF_HDR_LEN, false;
        if (h.len && !recv_all(c.sock, payload, (int)h.len, &why, &err)) return false;
        out_len = h.len;
        return true;
    }

    uint8_t lenbuf[mh_net_proto::REC_LEN_SIZE];
    if (!recv_all(c.sock, lenbuf, sizeof(lenbuf), &why, &err)) return false;
    uint32_t clen = (uint32_t)lenbuf[0] | ((uint32_t)lenbuf[1] << 8) | ((uint32_t)lenbuf[2] << 16) |
                    ((uint32_t)lenbuf[3] << 24);
    // Bound BEFORE allocating/reading: an attacker's first lever on a length-prefixed stream.
    if (clen < mh_net_proto::WIRE_HDR_SIZE || clen > mh_net_proto::WIRE_HDR_SIZE + MH_NET_MAX_PAYLOAD)
        return why = RXF_REC_LEN, false;
    uint8_t body[mh_net_proto::WIRE_HDR_SIZE + MH_NET_MAX_PAYLOAD];
    if (!recv_all(c.sock, body, (int)clen, &why, &err)) return false;
    uint8_t mac[mh_net_proto::MAC_LEN];
    if (!recv_all(c.sock, mac, sizeof(mac), &why, &err)) return false;
    const uint8_t *ek = (g_role == ROLE_HOST) ? c.keys.enc_c2s : c.keys.enc_s2c;
    const uint8_t *mk = (g_role == ROLE_HOST) ? c.keys.mac_c2s : c.keys.mac_s2c;
    if (!mh_net_proto::rec_open(ek, mk, c.rx_seq++, body, clen, mac)) return why = RXF_MAC, false;
    if (!wire_hdr_decode(body, h)) return why = RXF_HDR, false;
    uint32_t plen = clen - (uint32_t)mh_net_proto::WIRE_HDR_SIZE;
    if (h.len != plen || plen > MH_NET_MAX_PAYLOAD) return why = RXF_HDR_LEN, false; // header must agree
    if (plen) memcpy(payload, body + mh_net_proto::WIRE_HDR_SIZE, plen);
    out_len = plen;
    return true;
}

// ---- inbound queue ------------------------------------------------------------------------------
//
// EVICTION IS A LOST LOCKSTEP INPUT, AND UNTIL 2026-09-02 IT WAS BLIND AND SILENT (MP D24). The old
// policy was "full -> drop the OLDEST, favour fresh lockstep data". That is right for a horizon
// advertisement, which the next one supersedes, and catastrophic for an ORDER: the game's inbound
// stream is not a cache, and a peer that never sees an order never executes it. It was also
// invisible -- `g_dropped` existed all along but was reported only by the shutdown line, which a rig
// run (killed, never shut down) never reaches, so every artifact of every desynced run was silent
// about the one silent loss path in the design.
//
// D24 FIXED WHAT WAS DESTROYED; U41 FIXED THE SHAPE OF THE ANSWER. D24 kept the single ring and, on
// overflow, SEARCHED it head-first for a frame it could prove was nothing but a superseded horizon
// (`choose_victim`). Correct, and mutation-tested, but you should not have to look through a buffer
// for something you are allowed to throw away -- you should know by construction. So the
// classification now happens ONCE, at arrival, and the frame joins the lane it belongs to:
//
//   lane H  one bare horizon advertisement. FULL -> evict its own head, O(1), no scan.
//   lane M  everything else. FULL -> REFUSE the arrival and log the correctness event, which is the
//           same statement the old `-1` arm made.
//
// AND DELIVERY ORDER IS UNCHANGED, which is the whole reason this variant was picked over coalescing
// the horizon lane down to one slot per sender: every frame takes a monotonic arrival sequence
// number and the drain always takes the lower of the two lane heads, so the merge cannot reorder.
// (Why reordering would be dangerous at all -- `MSG_ORDER` carries a horizon in its `exec_time` and
// `LS_HORIZON_PENDING` is read at DISPATCH time -- is argued in mh_net_queue_policy.h.)
//
// The log calls are made AFTER the lock is released -- file I/O under the queue's critical section
// would put the recv thread's slowest operation inside the main thread's drain path.
void queue_rollup_line(int depth, int depth_h, int depth_m, int high, int high_h, int high_m,
                       long evicted, long refused) {
    logf("net: inbound queue rollup (this match): depth %d (H %d / M %d), high-water %d / %d "
         "(H %d / %d, M %d / %d), evicted %ld superseded horizon(s), REFUSED %ld real input(s)",
         depth, depth_h, depth_m, high, QUEUE_CAP, high_h, QUEUE_CAP_H, high_m, QUEUE_CAP_M, evicted,
         refused);
}

void enqueue(int src, const void *data, int len) {
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

    // CLASSIFY ONCE, HERE. This is the only call to the router predicate on the inbound path; nothing
    // downstream re-derives it, which is the difference between this and the scan it replaces.
    const qp::lane l = qp::lane_of_game_frame(static_cast<const uint8_t *>(data), len);

    EnterCriticalSection(&g_q_cs);
    const qp::push_result r = g_lanes.push(l);
    if (r.accepted) {
        if (r.evicted) {
            evicted  = true;
            ev_src   = g_qh[r.evicted_pos].src; // lane H only -- lane M never evicts
            ev_total = ++g_dropped;
        }
        if (r.which == qp::lane::supersedable) {
            HMsg &m = g_qh[r.pos];
            m.src   = src;
            if (len) memcpy(m.data, data, (size_t)len); // len == BARE_HORIZON_LEN by construction
        } else {
            Msg &m = g_qm[r.pos];
            m.src  = src;
            m.len  = len;
            if (len) memcpy(m.data, data, (size_t)len);
        }
    } else {
        refused   = true;
        ref_total = g_lanes.refused();
    }
    // Depth high-water, reported in 32-slot steps. This is the half of the instrument that can
    // REFUTE: if a clean run's backlog never leaves the low tens, an overflow cannot be the
    // explanation for anything, and no red run is needed to establish that.
    {
        const int band = (g_lanes.high_water() / 32) * 32;
        if (band > g_qhigh_band) {
            g_qhigh_band = band;
            new_high     = g_lanes.high_water();
        }
    }
    {
        const DWORD now = GetTickCount();
        if (g_q_rollup_at == 0) g_q_rollup_at = now;
        if (now - g_q_rollup_at >= QUEUE_ROLLUP_MS) {
            g_q_rollup_at = now;
            rollup        = true;
            r_depth       = g_lanes.depth();
            r_dh          = g_lanes.depth_h();
            r_dm          = g_lanes.depth_m();
            r_high        = g_lanes.high_water();
            r_hh          = g_lanes.high_water_h();
            r_hm          = g_lanes.high_water_m();
            r_ev          = g_lanes.evicted();
            r_ref         = g_lanes.refused();
        }
    }
    LeaveCriticalSection(&g_q_cs);

    if (new_high) logf("net: inbound queue depth high-water %d / %d", new_high, QUEUE_CAP);
    // A REFUSAL is a correctness event and is logged every time -- there is no rate at which losing a
    // game input is routine. An EVICTION is bookkeeping: lane H only ever holds frames the next one
    // supersedes, so a badly-behind peer sheds thousands and a line each would bury the one that
    // matters. First occurrence in full, then a rollup.
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
    if (rollup) queue_rollup_line(r_depth, r_dh, r_dm, r_high, r_hh, r_hm, r_ev, r_ref);
}

// ---- host relay: dispatch a DATA frame arriving from client `from_idx` --------------------------
void host_dispatch(int from_idx, const WireHdr *h, const void *payload, int len) {
    // Deliver locally if it targets the host or is a broadcast.
    if (h->dst == g_my_id || h->dst == MH_NET_BROADCAST)
        enqueue(h->src, payload, len);

    EnterCriticalSection(&g_conn_cs);
    if (h->dst == MH_NET_BROADCAST) {
        for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
            if (g_conns[i].active && i != from_idx) // every other client (not the origin)
                send_frame(i, FLAG_DATA, h->src, h->dst, payload, len);
    } else if (h->dst != g_my_id) { // unicast to one other player
        for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
            if (g_conns[i].active && g_conns[i].player_id == h->dst) {
                send_frame(i, FLAG_DATA, h->src, h->dst, payload, len);
                break;
            }
    }
    LeaveCriticalSection(&g_conn_cs);
}

// ---- per-connection receive loop ----------------------------------------------------------------
DWORD WINAPI conn_recv_thread(LPVOID param) {
    int     idx = (int)(intptr_t)param;
    SOCKET  s   = g_conns[idx].sock;
    uint8_t payload[MH_NET_MAX_PAYLOAD];
    for (;;) {
        WireHdr  h;
        uint32_t len = 0;
        RxFail   why = RXF_OK;
        int      err = 0;
        if (!recv_frame(g_conns[idx], h, payload, len, why, err)) {
            // Name the cause. A drop we retired ourselves (a failed send, or the watchdog) already
            // logged the real reason and only shows up here as the shutdown() it performed, so it
            // does not get a second, misleading line.
            if (InterlockedExchange(&g_conns[idx].tx_dead, 1) == 0) {
                if (err) logf("net: conn %d dropped -- %s (winsock %d)", idx, rx_fail_str(why), err);
                else logf("net: conn %d dropped -- %s", idx, rx_fail_str(why));
            }
            break;
        }
        // Proof of life: ANY frame, control ones included. This is what the watchdog reads.
        InterlockedExchange((volatile LONG *)&g_conns[idx].last_rx, (LONG)GetTickCount());

        if (h.flags == FLAG_PING) {                      // R-live keepalive: the arrival was the whole point
            InterlockedIncrement(&g_conns[idx].ping_rx); // D16: counted, so a drop can report it
            continue;
        }

        if (h.flags == FLAG_HELLO) {
            EnterCriticalSection(&g_conn_cs);
            if (g_host_assign) { // N1: the host already assigned this conn its id --
                logf("net: conn %d HELLO claims player %d (ignored; assigned %d)", idx, h.src, g_conns[idx].player_id);
            } else {
                g_conns[idx].player_id = h.src;                      // declared-id mode: the client's claim is authoritative
                InterlockedCompareExchange(&g_dead_peer, -1, h.src); // back: cancel a pending fast-drop
                logf("net: conn %d is player %d", idx, h.src);
            }
            LeaveCriticalSection(&g_conn_cs);
            continue;
        }
        if (h.flags == FLAG_WELCOME) { // N1: host assigned US an id (client side)
            g_my_id = h.dst;
            InterlockedExchange(&g_id_assigned, 1);
            logf("net: WELCOME -- host assigned us player %d", g_my_id);
            continue;
        }
        if (h.flags == FLAG_SESSION_INFO) { // discovery control frame -> handler, NOT the game queue
            MH_SessionInfoCb cb = g_si_cb;
            if (cb) cb(h.src, payload, (int)len);
            continue;
        }
        if (h.flags == FLAG_JOIN) { // client's join request -> handler, NOT the game queue (S4)
            MH_JoinCb cb = g_join_cb;
            if (cb) cb(h.src, payload, (int)len);
            continue;
        }
        if (h.flags == FLAG_START) { // host clicked Start -> handler, NOT the game queue (U2)
            // U28: the payload is the host's authoritative lobby slot array (may be empty on an older
            // peer -- the handler treats len==0 as the legacy bare signal and keeps its own slots).
            MH_StartCb cb = g_start_cb;
            if (cb) cb(h.src, payload, (int)len);
            continue;
        }
        if (h.flags == FLAG_LEAVE) { // client left the lobby -> handler, NOT the game queue (U12)
            MH_LeaveCb cb = g_leave_cb;
            if (cb) cb(h.src);
            continue;
        }
        if (h.flags == FLAG_ANNOUNCE) { // host's join/left announce -> handler, NOT the game queue (U16)
            MH_AnnounceCb cb = g_announce_cb;
            if (cb) cb(h.src, payload, (int)len);
            continue;
        }
        if (h.flags == FLAG_HASH) { // D21: a peer's desync sample -> handler, NOT the game queue
            // RELAYED like a DATA broadcast, and it has to be: control frames are point-to-point, so
            // without this a 3-peer run only ever compares each client against the host and two
            // clients could diverge from each other unseen. The relay is a straight forward -- the
            // host does not interpret the payload.
            if (g_role == ROLE_HOST) {
                EnterCriticalSection(&g_conn_cs);
                for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
                    if (g_conns[i].active && i != idx)
                        send_frame(i, FLAG_HASH, h.src, MH_NET_BROADCAST, payload, (int)len);
                LeaveCriticalSection(&g_conn_cs);
            }
            MH_HashCb cb = g_hash_cb;
            if (cb) cb(h.src, payload, (int)len);
            continue;
        }
        // Anything else that is not a game datagram is a control frame from a NEWER build. Ignore it:
        // treating an unknown flag as FLAG_DATA would push it into the game's lockstep queue as
        // garbage, which is how a forward-compatible protocol turns into a desync. (R-live.)
        if (h.flags != FLAG_DATA) continue;

        g_rx_pkts++;
        g_rx_bytes += (long)(mh_net_proto::WIRE_HDR_SIZE + len);
        g_last_rx_tick = GetTickCount();
        if (g_role == ROLE_HOST) host_dispatch(idx, &h, payload, len);
        else enqueue(h.src, payload, len); // client: host already routed it
    }
    InterlockedExchange(&g_conns[idx].active, 0);
    InterlockedExchange(&g_dead_peer, g_conns[idx].player_id); // U17: latch the dropped peer's id (>=1 on host; -1 for the client's host-conn) for the main-thread fast-drop
    closesocket(s);
    logf("net: conn %d closed", idx);
    return 0;
}

// U17: taken + cleared by the game's main thread (net_lockstep on_time_tick) to broadcast an immediate
// in-order removal of a dropped CLIENT (host-conn from a client latches -1 -> host death is not fast-dropped).
extern "C" int MH_Net_TakeDeadPeer(void) { return (int)InterlockedExchange(&g_dead_peer, -1); }

// ---- host: per-connection handshake ------------------------------------------------------------
// Runs on a short-lived thread, BEFORE the connection is given a peer slot. That ordering is the
// point: an unauthenticated connection must not be able to occupy one of the 8 slots (that alone
// was a trivial denial of service -- connect 8 times and stay silent), and must not be able to put
// a single byte into the game's parser. A blocking receive timeout bounds how long it can try.
bool server_handshake(SOCKET s, mh_net_proto::SessionKeys &keys, const char *ip) {
    using namespace mh_net_proto;
    DWORD tmo = HANDSHAKE_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));

    uint8_t  hello[HS_HELLO_LEN];
    uint8_t  cn[NONCE_LEN], sn[NONCE_LEN];
    uint16_t ver = 0;
    if (!recv_all(s, hello, sizeof(hello))) {
        logf("net: handshake from %s -- no HELLO within %u ms (scan/probe?)", ip, HANDSHAKE_MS);
        return false;
    }
    if (!hs_parse_hello(hello, cn, ver)) {
        // Distinguish the two: a version mismatch is a build-skew problem between friends, a magic
        // mismatch is anything else on the internet knocking on the port.
        logf("net: handshake from %s -- bad HELLO (magic/version %u, expected %u)", ip, ver, HS_VERSION);
        return false;
    }
    if (!MH_Key_Random(sn, NONCE_LEN)) {
        logf("net: handshake from %s -- no secure randomness available, refusing", ip);
        return false;
    }
    uint8_t chal[HS_CHALLENGE_LEN];
    hs_build_challenge(g_psk, cn, sn, chal);
    if (!send_all(s, chal, sizeof(chal))) return false;
    uint8_t resp[HS_RESPONSE_LEN];
    if (!recv_all(s, resp, sizeof(resp))) {
        logf("net: handshake from %s -- no RESPONSE", ip);
        return false;
    }
    if (!hs_check_response(g_psk, cn, sn, resp)) {
        logf("net: handshake from %s REJECTED -- wrong key (they need this host's mh_key.txt)", ip);
        return false;
    }
    hs_derive_keys(g_psk, cn, sn, keys);
    tmo = 0; // back to blocking: lockstep silence between frames is normal, not a failure
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
    logf("net: handshake from %s OK (authenticated + encrypted)", ip);
    return true;
}

struct PendingArg {
    SOCKET s;
    char   ip[64];
};

DWORD WINAPI pending_thread(LPVOID param) {
    PendingArg *pa = (PendingArg *)param;
    SOCKET      s  = pa->s;
    char        ip[64];
    lstrcpynA(ip, pa->ip, sizeof(ip));
    delete pa;

    mh_net_proto::SessionKeys keys;
    memset(&keys, 0, sizeof(keys));
    if (g_secure && !server_handshake(s, keys, ip)) {
        closesocket(s);
        InterlockedDecrement(&g_pending);
        return 0;
    }

    EnterCriticalSection(&g_conn_cs);
    int idx = -1;
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
        if (!g_conns[i].active) {
            idx = i;
            break;
        }
    int assigned = -1;
    if (idx >= 0) {
        Conn &c   = g_conns[idx];
        c.sock    = s;
        c.secure  = g_secure;
        c.keys    = keys;
        c.tx_seq  = 0;
        c.rx_seq  = 0;
        c.last_rx = GetTickCount(); // the handshake counts as life; start the watchdog clock here
        c.tx_dead = 0;
        if (g_host_assign) {
            // N1: assign the first-free player id in 1..MAX (host is 0), independent of what the client
            // claims in HELLO -- so hand-clicked clients (all defaulting to player_id=1) get distinct
            // slots. Scan the ids already taken by the OTHER active connections.
            bool taken[MH_NET_MAX_PEERS + 1] = {false};
            taken[0]                         = true; // 0 = the host
            for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
                if (g_conns[i].active && i != idx && g_conns[i].player_id >= 0 &&
                    g_conns[i].player_id <= MH_NET_MAX_PEERS)
                    taken[g_conns[i].player_id] = true;
            for (int k = 1; k <= MH_NET_MAX_PEERS; ++k)
                if (!taken[k]) {
                    assigned = k;
                    break;
                }
            c.player_id = assigned; // authoritative (HELLO won't override it below)
            // This id is BACK, so cancel any pending U17 fast-drop for it. The latch is only drained
            // in-game (SESSION_MODE 3), so one set during the lobby survives until the match starts --
            // and would then remove a player who had merely reconnected. Cheap now that R-live makes
            // lobby-phase transport drops something that actually happens.
            InterlockedCompareExchange(&g_dead_peer, -1, assigned);
        } else {
            c.player_id = -1; // learned from HELLO (declared-id mode)
        }
        InterlockedExchange(&c.active, 1);
        c.thread = CreateThread(nullptr, 0, conn_recv_thread, (LPVOID)(intptr_t)idx, 0, nullptr);
    }
    LeaveCriticalSection(&g_conn_cs);

    if (idx < 0) {
        closesocket(s);
        logf("net: connection refused (full)");
    } else {
        logf("net: accepted %s -> conn %d", ip, idx);
        if (assigned >= 0) { // N1: tell the client its assigned id (WELCOME)
            EnterCriticalSection(&g_conn_cs);
            send_frame(idx, FLAG_WELCOME, (int16_t)g_my_id, (int16_t)assigned, nullptr, 0);
            LeaveCriticalSection(&g_conn_cs);
            logf("net: assigned conn %d -> player %d (WELCOME sent)", idx, assigned);
        }
    }
    InterlockedDecrement(&g_pending);
    return 0;
}

// ---- host accept loop ---------------------------------------------------------------------------
// Accept is now only a dispatcher: it hands each socket to a pending thread and goes straight back
// to accepting, so one slow or hostile peer can never hold up the queue for a real one.
DWORD WINAPI accept_thread(LPVOID) {
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        sockaddr_in from;
        int         fromlen = sizeof(from);
        SOCKET      s       = accept(g_listen, (sockaddr *)&from, &fromlen);
        if (s == INVALID_SOCKET) break;
        link_socket_opts(s); // low-latency lockstep + keepalive + a bounded send

        char ip[64];
        if (!inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip))) lstrcpyA(ip, "?");

        DWORD now = GetTickCount();
        if (now - g_accept_window >= 1000) { // 1 s sliding window, reset on entry
            g_accept_window = now;
            g_accept_count  = 0;
        }
        if (++g_accept_count > ACCEPTS_PER_SEC) {
            closesocket(s);
            if (g_accept_count == ACCEPTS_PER_SEC + 1) // log the onset once, not every packet
                logf("net: accept rate limit hit (>%d/s) -- dropping connections from %s", ACCEPTS_PER_SEC, ip);
            continue;
        }
        if (InterlockedCompareExchange(&g_pending, 0, 0) >= MAX_PENDING) {
            closesocket(s);
            logf("net: too many handshakes in flight (%d) -- dropping %s", MAX_PENDING, ip);
            continue;
        }

        PendingArg *pa = new PendingArg;
        pa->s          = s;
        lstrcpynA(pa->ip, ip, sizeof(pa->ip));
        InterlockedIncrement(&g_pending);
        HANDLE th = CreateThread(nullptr, 0, pending_thread, pa, 0, nullptr);
        if (!th) {
            InterlockedDecrement(&g_pending);
            closesocket(s);
            delete pa;
        } else {
            CloseHandle(th);
        }
    }
    return 0;
}

// ---- link watchdog: generate liveness, then require it (R-live) ---------------------------------
// The failure this exists for: on 2026-07-26 a client's connection died mid-lobby and the HOST never
// found out. Its recv thread sat in a blocking recv with no timeout (deliberately -- lockstep silence
// between frames is normal), so a link that stops delivering without ever erroring is invisible. It
// advertised into the corpse for 90 s and started the match with a peer that was gone.
//
// TCP alone cannot distinguish "idle" from "gone", so we stop relying on the peer to talk: every
// connection gets a PING it must answer for, and inbound silence past the timeout is then a fact, not
// a guess. Because the traffic is ours, this works identically in the lobby, during a map load, and
// mid-lockstep -- no phase knowledge, and no dependence on the game thread, which may be busy.
// D16 (2026-08-06): the watchdog must not count time it was NOT RUNNING.
//
// It needs g_conn_cs both to ping and to judge silence -- and MH_Net_Send holds that same lock
// ACROSS a blocking send(), which is bounded only by SO_SNDTIMEO (5 s). So one stalled send starves
// this thread for up to 5 s: no pings go out, and the peer's watchdog counts that as OUR silence.
// Two in a row and the peer drops us at 10 s. Meanwhile, the moment we finally get the lock, every
// conn's `now - last_rx` has aged past the timeout too, so WE drop as well. Both ends, mutually,
// after a link that had been carrying data fine -- and only on a link slow enough to make a send
// block, which is why a LAN rig never sees it and a relayed internet game does. That is the exact
// shape of the 2026-08-02 relay drops.
//
// The fix is to make the deadline count OBSERVED time. If a pass starts late -- because we were
// starved, or the box slept, or a debugger held the process (the TTD caveat in the internet-play notes
// is the same bug wearing a different hat) -- credit that gap back to every connection instead of
// charging it to the peer. A watchdog that cannot see is not evidence of silence.
DWORD WINAPI watch_thread(LPVOID) {
    const DWORD TICK_MS = 250;
    // A pass is "late" once it slips a whole ping interval behind: below that, ordinary scheduling
    // jitter would keep re-arming the credit and the watchdog would never fire at all.
    const DWORD LATE_MS   = (DWORD)(g_ping_ms > 0 ? g_ping_ms : 1000);
    DWORD       last_ping = GetTickCount();
    DWORD       last_pass = GetTickCount();
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        Sleep(TICK_MS);
        DWORD now = GetTickCount();

        // How much longer than intended did it take to get back here? (pure, unit-tested)
        const DWORD elapsed = now - last_pass;
        const DWORD stalled = mh_watchdog_blind_ms(elapsed, TICK_MS, LATE_MS);
        last_pass           = now;

        bool do_ping = g_ping_ms > 0 && (now - last_ping) >= (DWORD)g_ping_ms;
        if (do_ping) last_ping = now;

        EnterCriticalSection(&g_conn_cs);
        // RE-SAMPLE `now` INSIDE THE LOCK. The value above is what the blind-time arithmetic needs
        // (it is measured against last_pass), but judging silence with it opens a window: waiting on
        // g_conn_cs takes real time, and the recv thread stamps last_rx with no lock at all, so a
        // frame arriving in that window leaves last_rx AHEAD of a `now` sampled before it. That is
        // half of the false drop measured 2026-08-30; mh_watchdog_silence_ms is the other half, and
        // both are kept because narrowing a race is not closing it.
        now = GetTickCount();
        if (stalled) {
            // Credit the blind interval to every live conn, and say so -- a silent correction here
            // would just move the mystery. Also re-arm the ping clock: pinging "late" is right,
            // pinging N times in a burst to catch up is not.
            //
            // CLAMPED AT `now`: the credit moves last_rx forward, and a conn heard from more
            // recently than the blind interval is long would be pushed into the FUTURE -- which the
            // silence subtraction then reads as ~4.29e9 ms and drops. Crediting can only ever mean
            // "treat this peer as heard from at most as recently as this instant".
            for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
                if (g_conns[i].active && !g_conns[i].tx_dead) {
                    const DWORD credited = g_conns[i].last_rx + stalled;
                    const bool  future   = mh_watchdog_silence_ms(now, credited) == 0u;
                    InterlockedExchange((volatile LONG *)&g_conns[i].last_rx,
                                        (LONG)(future ? now : credited));
                }
            logf("net: link watchdog was blocked %u ms (a send holding the conn lock, a suspend, or "
                 "a debugger) -- crediting that to every peer rather than reading it as silence",
                 stalled);
        }
        for (int i = 0; i < MH_NET_MAX_PEERS; ++i) {
            if (!g_conns[i].active || g_conns[i].tx_dead) continue;
            const unsigned silent = mh_watchdog_silence_ms(now, g_conns[i].last_rx);
            if (mh_watchdog_should_drop(silent, (unsigned)g_rx_timeout_ms)) {
                // Report what we PUT INTO the silence, not just that we heard nothing. `sent N,
                // got M` separates "the peer stopped answering" from "we never managed to ask".
                logf("net: conn %d silent %u ms (keepalives: sent %ld, received %ld)", i, silent,
                     (long)g_conns[i].ping_tx, (long)g_conns[i].ping_rx);
                drop_conn(i, RXF_WATCHDOG, 0);
                continue;
            }
            if (do_ping) {
                send_frame(i, FLAG_PING, (int16_t)g_my_id, MH_NET_BROADCAST, nullptr, 0);
                InterlockedIncrement(&g_conns[i].ping_tx);
            }
        }
        LeaveCriticalSection(&g_conn_cs);
    }
    return 0;
}

// ---- winsock lifetime ---------------------------------------------------------------------------
bool ensure_wsa() {
    static bool done = false;
    if (done) return true;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    done = true;
    return true;
}

bool start_host(const MH_NetConfig *cfg) {
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) {
        logf("net: socket() failed %d", WSAGetLastError());
        return false;
    }
    // SO_EXCLUSIVEADDRUSE, *not* SO_REUSEADDR (changed 2026-08-06 after the R2P internet session).
    //
    // Windows' SO_REUSEADDR is not Berkeley's: it lets a SECOND process bind an address:port that is
    // already being listened on, bind() SUCCEEDS, and new connections go to the last binder. So a
    // second game instance on this machine silently hijacks the port from the one the player is
    // looking at, and nothing anywhere says so.
    //
    // That is not hypothetical. On 2026-08-02 a remote joiner got
    //     net: handshake REJECTED -- host key mismatch
    // while both peers provably held the same mh_key.txt. The far end that answered was a DIFFERENT
    // install on the host machine (the rig lanes and /eng ship key 4d487465..., "MHtestke..."; the
    // internet rig's key is 5e69e5db...), reached because the SSH tunnel forwards to
    // 127.0.0.1:6501 -- exactly the port mp_run and the determinism rig use. The diagnostic then
    // blamed the player's key file, which is the one thing that was fine.
    //
    // SO_EXCLUSIVEADDRUSE makes the second bind FAIL instead, so the collision is reported at the
    // moment it happens, by the process that lost.
    //
    // MEASURED, not assumed (2026-08-06), because the obvious worry is that this breaks the rig's
    // constant host restarts:
    //   holder SO_REUSEADDR + second SO_REUSEADDR -> second BINDS (the bug: silent hijack)
    //   holder SO_REUSEADDR + second default      -> refused 10048
    //   holder default/exclusive + second SO_REUSEADDR -> refused 10013
    //   bind, accept a real connection, close all, rebind IMMEDIATELY -> OK for BOTH default and
    //     exclusive, so the feared TIME_WAIT penalty on restart does not materialise here.
    // So the hijack needs the HOLDER to have set SO_REUSEADDR; dropping it is what actually fixes
    // this, and SO_EXCLUSIVEADDRUSE additionally states the intent and protects against an older
    // build (which still sets SO_REUSEADDR) running beside a new one.
    BOOL excl = TRUE;
    setsockopt(g_listen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof(excl));

    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_port        = htons((u_short)cfg->port);
    a.sin_addr.s_addr = (cfg->host[0]) ? inet_addr(cfg->host) : htonl(INADDR_ANY);
    if (bind(g_listen, (sockaddr *)&a, sizeof(a)) == SOCKET_ERROR) {
        const int e = WSAGetLastError();
        // BOTH codes mean "someone else has this port": 10048 WSAEADDRINUSE when we lose to a plain
        // holder, 10013 WSAEACCES when we lose to one holding it exclusively -- which is the case an
        // OLDER build (still setting SO_REUSEADDR) produces, i.e. the likeliest one in practice.
        // Matching only WSAEADDRINUSE would print the generic line for exactly that case.
        if (e == WSAEADDRINUSE || e == WSAEACCES) {
            // Name the real cause and the real fix. This is the line that would have saved the
            // 2026-08-02 session.
            logf("net: bind(:%d) REFUSED -- something else on THIS machine is already listening on "
                 "that port. Almost always another mh.focus.exe (a rig lane, a determinism run, or "
                 "an older host you did not close). Close it and host again -- do NOT assume it is a "
                 "key problem: a second instance answers joiners with ITS OWN mh_key.txt, which "
                 "reports on the joiner as 'host key mismatch'.",
                 cfg->port);
        } else {
            logf("net: bind(:%d) failed %d", cfg->port, e);
        }
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        return false;
    }
    if (listen(g_listen, MH_NET_MAX_PEERS) == SOCKET_ERROR) {
        logf("net: listen() failed %d", WSAGetLastError());
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        return false;
    }
    InterlockedExchange(&g_running, 1);
    g_accept_thread = CreateThread(nullptr, 0, accept_thread, nullptr, 0, nullptr);
    logf("net: HOST listening on :%d as player %d (expect %d peers) [key %s]", cfg->port, g_my_id,
         cfg->peers, g_secure ? g_key_hex8 : "open");
    return true;
}

bool start_client(const MH_NetConfig *cfg) {
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_port        = htons((u_short)cfg->port);
    a.sin_addr.s_addr = inet_addr(cfg->host[0] ? cfg->host : "127.0.0.1");

    // NON-BLOCKING connect on a bounded total budget (S8). A dead/typo'd IP whose SYN is silently
    // dropped would otherwise stall the WHOLE lobby thread for the OS connect timeout (~20s) on a
    // single blocking connect() -- which is what made the S8 wrong-IP retry re-arm too slowly to be
    // usable. Instead we poll writability with a short per-attempt select() timeout, recreating the
    // socket each round, so a dead host fails within CONNECT_BUDGET_MS. The host still coming up
    // (same-box launch race -> fast RST/refused) still resolves via the same retry loop.
    const DWORD CONNECT_BUDGET_MS  = 4000;
    const long  ATTEMPT_TIMEOUT_US = 200000; // 200ms per non-blocking connect poll
    const DWORD deadline           = GetTickCount() + CONNECT_BUDGET_MS;

    SOCKET s         = INVALID_SOCKET;
    bool   connected = false;
    int    last_err  = 0;
    do {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) {
            last_err = WSAGetLastError();
            break;
        }
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);

        const DWORD attempt_start = GetTickCount();
        if (connect(s, (sockaddr *)&a, sizeof(a)) == 0) {
            connected = true; // immediate (loopback) -- rare but possible
            break;
        }
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            fd_set wf, ef;
            FD_ZERO(&wf);
            FD_SET(s, &wf);
            FD_ZERO(&ef);
            FD_SET(s, &ef);
            timeval tv;
            tv.tv_sec  = 0;
            tv.tv_usec = ATTEMPT_TIMEOUT_US;
            int sel    = select(0, nullptr, &wf, &ef, &tv);
            if (sel > 0) {
                int so_err = 0, len = sizeof(so_err);
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&so_err, &len);
                if (FD_ISSET(s, &wf) && so_err == 0) {
                    connected = true; // handshake completed
                    break;
                }
                last_err = so_err ? so_err : WSAECONNREFUSED; // refused/reset
            } else {
                last_err = WSAETIMEDOUT; // SYN dropped -- host absent/unreachable
            }
        } else {
            last_err = err;
        }

        closesocket(s);
        s = INVALID_SOCKET;
        // Pace fast-fail (RST/refused) retries; a timed-out attempt already spent ATTEMPT_TIMEOUT.
        if (GetTickCount() - attempt_start < 50)
            Sleep(100);
    } while ((long)(deadline - GetTickCount()) > 0);

    if (!connected) {
        logf("net: connect(%s:%d) failed %d", cfg->host, cfg->port, last_err);
        if (s != INVALID_SOCKET)
            closesocket(s);
        return false;
    }

    // The socket stays non-blocking through connect; restore blocking for the send/recv path below,
    // which uses blocking full-buffer send/recv semantics.
    u_long block = 0;
    ioctlsocket(s, FIONBIO, &block);

    link_socket_opts(s);

    // Authenticate the HOST before trusting it with anything (mutual: it proves the key first, we
    // prove it second). A wrong key here is the most common real-world failure -- someone joined
    // with a stale mh_key.txt -- so it gets an explicit, actionable log line rather than a timeout.
    mh_net_proto::SessionKeys keys;
    memset(&keys, 0, sizeof(keys));
    if (g_secure) {
        using namespace mh_net_proto;
        DWORD tmo = HANDSHAKE_CLIENT_MS;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
        uint8_t cn[NONCE_LEN], sn[NONCE_LEN];
        if (!MH_Key_Random(cn, NONCE_LEN)) {
            logf("net: no secure randomness available -- refusing to connect");
            closesocket(s);
            return false;
        }
        uint8_t hello[HS_HELLO_LEN];
        hs_build_hello(cn, hello);
        uint8_t  chal[HS_CHALLENGE_LEN];
        uint16_t ver = 0;
        if (!send_all(s, hello, sizeof(hello)) || !recv_all(s, chal, sizeof(chal))) {
            // We reached SOMETHING (the TCP connect succeeded) and it never answered the handshake.
            // Naming only the key here actively misleads: through a relay or SSH tunnel the far end
            // is a forwarder, which accepts the connection and only then discovers it has nowhere to
            // send it -- which looks exactly like this. That cost a real debugging session
            // (2026-07-26: a stale `ssh -R` still held the VPS port, so every join "failed the
            // handshake" while both keys were in fact identical). List the causes by likelihood.
            logf("net: handshake failed -- connected, but the peer never answered. Either nothing is "
                 "listening behind that address (a relay/tunnel whose far end is down -- the host must "
                 "have a game open), the host is an older build, or our mh_key.txt is wrong.");
            closesocket(s);
            return false;
        }
        if (!hs_check_challenge(g_psk, cn, chal, sn, ver)) {
            // Order the causes by what actually happens. On 2026-08-02 this line read "ask the host
            // for its mh_key.txt" while both peers held the same key -- the far end was a DIFFERENT
            // GAME INSTANCE on the host's machine (a rig lane / an older host), reached because a
            // tunnel forwards to 127.0.0.1:<port> and every install ships its own key. The joiner
            // cannot tell those apart from the wire, so the message must not pick one and assert it.
            logf("net: handshake REJECTED -- the far end answered with a DIFFERENT key (ours %s, "
                 "version %u vs %u). Either it is not the host you meant -- another game instance on "
                 "that machine or behind that tunnel answers on the same port with its own "
                 "mh_key.txt -- or the two of you really do hold different keys. Compare the host's "
                 "'HOST listening ... [key ...]' line with this one BEFORE copying key files around.",
                 g_secure ? g_key_hex8 : "open", ver, HS_VERSION);
            closesocket(s);
            return false;
        }
        uint8_t resp[HS_RESPONSE_LEN];
        hs_build_response(g_psk, cn, sn, resp);
        if (!send_all(s, resp, sizeof(resp))) {
            closesocket(s);
            return false;
        }
        hs_derive_keys(g_psk, cn, sn, keys);
        tmo = 0; // blocking again -- lockstep gaps are normal
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
        logf("net: handshake OK (authenticated + encrypted)");
    }

    EnterCriticalSection(&g_conn_cs);
    g_conns[0].sock      = s;
    g_conns[0].player_id = -1; // host id unknown/irrelevant to the client
    g_conns[0].secure    = g_secure;
    g_conns[0].keys      = keys;
    g_conns[0].tx_seq    = 0;
    g_conns[0].rx_seq    = 0;
    g_conns[0].last_rx   = GetTickCount();
    g_conns[0].tx_dead   = 0;
    InterlockedExchange(&g_conns[0].active, 1);
    // Announce our player id so the host can route unicasts/broadcasts to us before we send data.
    // Inside the lock: on a secure link this is record #0 and must not race another sender's seq.
    send_frame(0, FLAG_HELLO, (int16_t)g_my_id, MH_NET_BROADCAST, nullptr, 0);
    LeaveCriticalSection(&g_conn_cs);

    InterlockedExchange(&g_running, 1);
    g_conns[0].thread = CreateThread(nullptr, 0, conn_recv_thread, (LPVOID)(intptr_t)0, 0, nullptr);
    logf("net: CLIENT connected to %s:%d as player %d", a.sin_addr.s_addr ? cfg->host : "127.0.0.1",
         cfg->port, g_my_id);
    return true;
}

// ---- U40: returning the transport to the PRE-INIT state -----------------------------------------
//
// THIS IS THE ORDERLY STOP FORK F4B SAID WOULD ARRIVE WITH A CALLER AND A TEST, and it is the whole
// transport half of U40. Until it existed `g_started` latched true for the life of the process --
// MH_Net_Shutdown had been deleted for having no call sites -- so a CLIENT whose only connection was
// retired at match teardown could never dial again: `MH_Net_IsStarted()` stayed 1, the discovery
// poll's connect kick was gated behind `!MH_Net_IsStarted()`, and MH_Net_InitEx early-returned. The
// host meanwhile re-advertised its new lobby once a second into `peers=0`. Measured end to end in the
// 2026-09-01 internet session: `handshake OK` appears EXACTLY ONCE in a 16-minute client log.
//
// IT IS NOT EXPORTED, and that is deliberate rather than an omission: mh_net.dll's contract is the 23
// symbols in mh_net_module.h, and adding a 24th to say "stop" would leave every caller of InitEx with
// two ways to express one intent. The intent is instead carried by InitEx itself -- RE-INITIALISING
// AN ALREADY-STARTED TRANSPORT RESTARTS IT -- so a re-dial is one call, the same call a first dial is,
// and the UDP module mirrors a rule rather than an extra entry point.
//
// THE CALLER OWNS THE DECISION, not this function: there is no "is the link still in use?" guard here
// on purpose. net_seams' lazy_start is latched by `g_tried_init`, which only the U40 relink path
// clears, and that path is reachable only from the discovery browser on a manual client (see
// net_discovery.cpp's `g_net_relink`). A guard keyed on PeerCount would ALSO refuse the case U40 is
// about -- a match that ended with the socket still open -- so it would buy nothing and cost the fix.
//
// SHUT THE SOCKETS FIRST, THEN JOIN. Every thread below is blocked in a socket call (recv, accept, or
// a send under SO_SNDTIMEO); closing the handle is what returns them. The critical sections are NOT
// deleted -- they are initialised once per process (`g_cs_ready`) and outlive every reset -- because
// MH_Net_Recv/Send read `g_started` without the lock, so a CS deleted under a concurrent caller is a
// crash where a stale-but-valid one is an empty queue.
//
// A REFUSAL IS A RETURN TO TODAY'S BEHAVIOUR, not a corrupt transport: if a thread will not stop
// inside the budget we log it and leave `g_started` true, so InitEx returns 0, the seam logs a failed
// relink and the player is exactly where the pre-U40 build left them. Nothing is freed or zeroed
// while a thread that can touch it is still alive.
constexpr DWORD RESET_JOIN_MS = 3000;

bool join_thread(HANDLE &h, const char *what) {
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

bool net_reset() {
    logf("net: returning the transport to the pre-init state (relink)");
    InterlockedExchange(&g_running, 0); // accept_thread + watch_thread run conditions

    if (g_listen != INVALID_SOCKET) {
        closesocket(g_listen); // unblocks accept()
        g_listen = INVALID_SOCKET;
    }
    EnterCriticalSection(&g_conn_cs);
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
        if (g_conns[i].active) {
            InterlockedExchange(&g_conns[i].tx_dead, 1); // one-shot: no second "dropped" line
            shutdown(g_conns[i].sock, SD_BOTH);          // unblocks conn_recv_thread's recv()
        }
    LeaveCriticalSection(&g_conn_cs);

    bool ok = true;
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i) ok = join_thread(g_conns[i].thread, "connection") && ok;
    ok = join_thread(g_accept_thread, "accept") && ok;
    ok = join_thread(g_watch_thread, "link watchdog") && ok;
    if (!ok) return false; // g_started stays true -- pre-U40 behaviour, loudly

    memset(g_conns, 0, sizeof(g_conns));
    // U41: the LAST rollup of the match goes out before the counters are cleared, so a match that
    // never reached the periodic cadence still leaves one line saying how deep its queue got. Read
    // under the lock, logged after it.
    int  q_depth, q_dh, q_dm, q_high, q_hh, q_hm;
    long q_ev, q_ref;
    EnterCriticalSection(&g_q_cs);
    q_depth = g_lanes.depth();
    q_dh    = g_lanes.depth_h();
    q_dm    = g_lanes.depth_m();
    q_high  = g_lanes.high_water();
    q_hh    = g_lanes.high_water_h();
    q_hm    = g_lanes.high_water_m();
    q_ev    = g_lanes.evicted();
    q_ref   = g_lanes.refused();
    // THE PER-MATCH RESET. Both counters and both high-waters go with the lanes, so the next match in
    // this process reports its own numbers rather than inheriting these.
    g_lanes.reset();
    LeaveCriticalSection(&g_q_cs);
    queue_rollup_line(q_depth, q_dh, q_dm, q_high, q_hh, q_hm, q_ev, q_ref);
    g_qhigh_band  = 0;
    g_q_rollup_at = 0;
    InterlockedExchange(&g_dead_peer, -1); // U17: a latch from the old link is not the new one's news
    g_started = false;
    logf("net: transport stopped -- ready to dial again");
    return true;
}

bool g_cs_ready = false; // the two critical sections are process-lifetime (see net_reset)

} // namespace

// =================================================================================================
extern "C" int MH_Net_InitEx(const MH_NetConfig *cfg) {
    if (!cfg) return g_started ? 1 : 0;
    // U40: re-initialising an already-started transport RESTARTS it. See net_reset above for why the
    // decision belongs to the caller and why this is not a separate exported entry point.
    if (g_started && !net_reset()) return 0;
    if (!ensure_wsa()) return 0;

    if (!g_cs_ready) {
        InitializeCriticalSection(&g_conn_cs);
        InitializeCriticalSection(&g_q_cs);
        g_cs_ready = true;
    }
    memset(g_conns, 0, sizeof(g_conns));
    g_lanes.reset();
    g_qhigh_band  = 0;
    g_q_rollup_at = 0;
    g_role        = cfg->role;
    g_my_id       = cfg->player_id;
    g_host_assign = (cfg->host_assign != 0);
    // N1: a client in host_assign mode must WAIT for the host's WELCOME before trusting its id (its
    // configured player_id is just a placeholder). Everyone else's id is settled at start.
    g_id_assigned = (g_role == ROLE_CLIENT && g_host_assign) ? 0 : 1;
    g_log_on      = (cfg->log != 0);
    // R-live: 0 in the config means "unset" -> keep the built-in default; a NEGATIVE value is the
    // explicit "off" (the selftest's mute peer uses it to simulate a blackholed link).
    if (cfg->ping_ms) g_ping_ms = (cfg->ping_ms > 0) ? cfg->ping_ms : 0;
    if (cfg->rx_timeout_ms) g_rx_timeout_ms = (cfg->rx_timeout_ms > 0) ? cfg->rx_timeout_ms : 0;
    if (g_log_on)
        wsprintfA(g_log_path, "%smh_net.log", module_run_dir()); // per-run folder (shared with net_seams)

    // Link security: mh_key.txt decides it, once, for the whole session. Failing CLOSED on a
    // broken key file is deliberate -- the alternative (fall back to open) would silently take a
    // host that believes it is protected and publish it.
    {
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        char *slash = exe;
        for (char *p = exe; *p; ++p)
            if (*p == '\\' || *p == '/') slash = p;
        slash[1]                         = '\0';
        char key_hex[MH_KEY_HEX_LEN + 1] = {0};
        int  generated                   = 0;
        int  st                          = MH_Key_Load(exe, g_psk, key_hex, &generated);
        if (st == MH_KEY_INVALID) {
            logf("net: mh_key.txt is unreadable/corrupt -- REFUSING to start the transport. Fix or delete it "
                 "(delete = a fresh key is generated; the single word 'open' = no protection).");
            return 0;
        }
        g_secure = (st == MH_KEY_SECURE);
        lstrcpynA(g_key_hex8, key_hex, sizeof(g_key_hex8)); // for the listen/connect banners
        if (g_secure) {
            logf("net: link security ON (key %.8s..., mh_key.txt). Everyone must use the SAME key.", key_hex);
            if (generated)
                logf("net: generated a new key -- send mh_key.txt (or its first line) to the players joining you:\n"
                     "net:   %s",
                     key_hex);
        } else {
            logf("net: *** link security OFF (mh_key.txt says 'open') *** -- no authentication, no encryption. "
                 "Anyone who can reach this port can join or feed data to the game. LAN/testing only.");
        }
    }

    bool ok = (g_role == ROLE_HOST) ? start_host(cfg) : start_client(cfg);
    if (ok && (g_ping_ms > 0 || g_rx_timeout_ms > 0)) {
        // After start_*: both set g_running, which is this thread's run condition.
        g_watch_thread = CreateThread(nullptr, 0, watch_thread, nullptr, 0, nullptr);
        logf("net: link watchdog armed (ping every %d ms, drop after %d ms of silence)", g_ping_ms, g_rx_timeout_ms);
    }
    g_started = ok;
    return ok ? 1 : 0;
}

// MH_Net_Init AND MH_Net_Shutdown WERE HERE, AND FORK F4B DELETED THEM (both had ZERO call sites
// anywhere in src/ or tools/ -- re-measured at F4B's HEAD, which is why they are gone rather than
// exported into mh_net.dll unused):
//
//   MH_Net_Init  parsed [net] out of mh_net.ini and called MH_Net_InitEx from it. net_seams.cpp's
//                lazy_start has read the same keys itself since the ship build (it needs the lobby
//                role/ip, which the ini alone cannot give), so this was a second, drifting reader of
//                one config block -- exactly the shape fork F2 spent an umbrella removing.
//   MH_Net_Shutdown closed the sockets and joined the watchdog thread. Nothing ever called it: the
//                game exits the process, and a transport teardown that has never run once is not a
//                feature, it is untested code with a plausible name. If a future item wants an
//                orderly stop it gets one that something calls, and a test.
//
// THAT ITEM ARRIVED: mp:U40 (2026-09-17). The orderly stop is `net_reset()` above -- called by
// MH_Net_InitEx itself when the transport is already started, tested by `net_selftest.exe relinktest`
// and exercised on the rig by the `host_rematch` UI scenario. It is deliberately NOT a 24th export:
// the F4B reasoning holds, so "stop" is not a symbol a caller can get wrong, it is what re-dialling
// means.
//
// Keeping them would have cost the module two exports whose absent-value (mh_net_module.h) nobody
// could derive, because nobody could say what the caller that does not exist would expect.

extern "C" int MH_Net_Send(int dst_player, const void *buf, int len) {
    if (!g_started || len < 0 || len > MH_NET_MAX_PAYLOAD) return 0;
    EnterCriticalSection(&g_conn_cs);
    if (g_role == ROLE_HOST) {
        if (dst_player == MH_NET_BROADCAST) {
            for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
                if (g_conns[i].active)
                    send_frame(i, FLAG_DATA, (int16_t)g_my_id, MH_NET_BROADCAST, buf, len);
        } else {
            for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
                if (g_conns[i].active && g_conns[i].player_id == dst_player) {
                    send_frame(i, FLAG_DATA, (int16_t)g_my_id, (int16_t)dst_player, buf, len);
                    break;
                }
        }
    } else {
        if (g_conns[0].active) // client: send to host; it relays (even for a specific dst)
            send_frame(0, FLAG_DATA, (int16_t)g_my_id, (int16_t)dst_player, buf, len);
    }
    LeaveCriticalSection(&g_conn_cs);
    g_tx_pkts++;
    g_tx_bytes += (long)(sizeof(WireHdr) + len); // the local game's outbound rate
    return 1;
}

extern "C" void MH_Net_GetStats(MH_NetStats *out) {
    if (!out) return;
    out->tx_pkts      = g_tx_pkts;
    out->tx_bytes     = g_tx_bytes;
    out->rx_pkts      = g_rx_pkts;
    out->rx_bytes     = g_rx_bytes;
    out->last_rx_tick = g_last_rx_tick;
    out->dropped      = g_dropped;
    out->peers        = MH_Net_PeerCount();
    // mp:T3. THIS MODULE MEASURES NOTHING, and says so rather than answering zeros. FLAG_PING here
    // is an empty keepalive (send_frame(i, FLAG_PING, ..., nullptr, 0) below) -- no timestamp is
    // echoed, so no round trip is ever timed -- and RFC 7680 loss is not a meaningful quantity over
    // a stream that retransmits invisibly -- a drop arrives as added delay, never as a hole in a
    // sequence. A zero SRTT would render as a
    // perfect link in the lockstep log; `lat_supported = 0` renders as `n/a`, which is the truth.
    out->lat_supported = 0;
    out->lat_count     = 0;
}

// mp:SES6 -- no-op: this module has no per-peer counters line to plumb a pushed horizon into (see
// mh_net_export.h's note on the row).
extern "C" void MH_Net_SetPeerHorizon(int player_id, int horizon_ms) {
    (void)player_id;
    (void)horizon_ms;
}

// mp:U41b -- THE MATCH BOUNDARY, which is not the transport boundary. net_reset() rolls the queue up
// and clears it when the LINK goes; a host_rematch keeps the link and starts a second match on it, so
// until this entry existed the second match's rollup inherited the first's high-water and counts.
// Called by mh.dll's mp_session_close (SES1's match end). Same shape as net_reset's half: read under
// the lock, log after it -- but reset_counters(), not reset(): frames already queued are the next
// match's inputs (or the lobby's), and a match ending is no reason to destroy them.
extern "C" void MH_Net_QueueMatchBoundary(void) {
    if (!g_cs_ready) return; // never initialised: no lanes, no counters, nothing to roll up
    int  q_depth, q_dh, q_dm, q_high, q_hh, q_hm;
    long q_ev, q_ref;
    unsigned epoch;
    long     post_ev, post_ref;
    EnterCriticalSection(&g_q_cs);
    q_depth = g_lanes.depth();
    q_dh    = g_lanes.depth_h();
    q_dm    = g_lanes.depth_m();
    q_high  = g_lanes.high_water();
    q_hh    = g_lanes.high_water_h();
    q_hm    = g_lanes.high_water_m();
    q_ev    = g_lanes.evicted();
    q_ref   = g_lanes.refused();
    g_lanes.reset_counters();
    // mp:U41d -- read BACK OUT, under the same lock, what reset_counters() just did. `epoch` is a
    // marker only that call can move (see mh_net_queue_policy.h); post_ev/post_ref are evicted/
    // refused read AFTER the reset, which reset_counters() zeroes -- so they read 0 here iff the call
    // above actually ran. A build that skips the call would leave epoch at match 1's value and
    // post_ev/post_ref at whatever they had accumulated, not 0. Both are logged below rather than
    // trusted silently, so a build with the reset skipped fails check_queue_rollups.py loudly instead
    // of only failing the (luck-dependent) magnitude comparison.
    epoch    = g_lanes.epoch();
    post_ev  = g_lanes.evicted();
    post_ref = g_lanes.refused();
    g_qhigh_band  = 0;
    g_q_rollup_at = 0;
    LeaveCriticalSection(&g_q_cs);
    queue_rollup_line(q_depth, q_dh, q_dm, q_high, q_hh, q_hm, q_ev, q_ref);
    logf("net: match boundary -- inbound queue counters restarted (link kept; %d frame(s) still "
         "queued carry over; epoch %u, post-reset evicted %ld / refused %ld)",
         q_depth, epoch, post_ev, post_ref);
}

// qmatchtest's read of the lane counters (net_selftest.exe compiles this TU; nothing in a module
// calls it). Under the same lock the writers take. `epoch_out` is mp:U41d's reset marker -- pass
// nullptr from a call site that does not need it.
void mh_net_queue_counters_for_test(int *depth, int *high, long *evicted, long *refused,
                                     unsigned *epoch_out) {
    if (!g_cs_ready) {
        *depth = *high = 0;
        *evicted = *refused = 0;
        if (epoch_out) *epoch_out = 0;
        return;
    }
    EnterCriticalSection(&g_q_cs);
    *depth   = g_lanes.depth();
    *high    = g_lanes.high_water();
    *evicted = g_lanes.evicted();
    *refused = g_lanes.refused();
    if (epoch_out) *epoch_out = g_lanes.epoch();
    LeaveCriticalSection(&g_q_cs);
}

extern "C" int MH_Net_Recv(int *out_sender, void *buf, int *inout_len) {
    if (!g_started || !buf || !inout_len) return 0;
    int cap = *inout_len;
    int got = 0;
    EnterCriticalSection(&g_q_cs);
    // U41: the MERGE. `pop()` returns whichever lane's head arrived first, so what comes out of here
    // is the arrival order the single ring used to deliver, minus only what lane H explicitly evicted.
    const mh::net::queue_policy::pop_result r = g_lanes.pop();
    if (r.ok) {
        const uint8_t *src_bytes;
        int            src_len, src_from;
        if (r.which == mh::net::queue_policy::lane::supersedable) {
            src_bytes = g_qh[r.pos].data;
            src_len   = mh::net::queue_policy::BARE_HORIZON_LEN;
            src_from  = g_qh[r.pos].src;
        } else {
            src_bytes = g_qm[r.pos].data;
            src_len   = g_qm[r.pos].len;
            src_from  = g_qm[r.pos].src;
        }
        int n = src_len < cap ? src_len : cap;
        if (n > 0) memcpy(buf, src_bytes, n);
        if (out_sender) *out_sender = src_from;
        *inout_len = n;
        got        = 1;
    }
    LeaveCriticalSection(&g_q_cs);
    return got;
}

extern "C" int MH_Net_IsStarted(void) { return g_started ? 1 : 0; }

extern "C" void MH_Net_SetSessionInfoHandler(MH_SessionInfoCb cb) { g_si_cb = cb; }

// Broadcast a SESSION_INFO control frame to every connected peer (host advertising itself). Discovery
// control traffic; the receiver routes it to g_si_cb, never to the game queue -- so it cannot perturb
// the lockstep input stream (determinism-safe).
extern "C" void MH_Net_SendSessionInfo(const unsigned char *buf, int len) {
    if (!g_started || len < 0 || len > MH_NET_MAX_PAYLOAD) return;
    EnterCriticalSection(&g_conn_cs);
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
        if (g_conns[i].active)
            send_frame(i, FLAG_SESSION_INFO, (int16_t)g_my_id, MH_NET_BROADCAST, buf, len);
    LeaveCriticalSection(&g_conn_cs);
}

extern "C" void MH_Net_SetJoinHandler(MH_JoinCb cb) { g_join_cb = cb; }

// Send a JOIN control frame to every active connection (the client has one: the host). Routed to the
// host's g_join_cb, never the game queue -- determinism-safe, exactly like SESSION_INFO.
extern "C" void MH_Net_SendJoin(const unsigned char *buf, int len) {
    if (!g_started || len < 0 || len > MH_NET_MAX_PAYLOAD) return;
    EnterCriticalSection(&g_conn_cs);
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
        if (g_conns[i].active)
            send_frame(i, FLAG_JOIN, (int16_t)g_my_id, MH_NET_BROADCAST, buf, len);
    LeaveCriticalSection(&g_conn_cs);
}

extern "C" void MH_Net_SetStartHandler(MH_StartCb cb) { g_start_cb = cb; }

// Broadcast a FLAG_START control frame to every active connection (host -> clients). U28: the payload
// is the host's AUTHORITATIVE lobby slot array, so the client builds Players[] from the host's copy
// instead of its own (it used to be empty, which is what let an in-flight slot edit desync the two).
// Routed to the client's g_start_cb, never the game queue.
extern "C" void MH_Net_SendStart(const unsigned char *buf, int len) {
    if (!g_started) return;
    if (len < 0 || (len > 0 && buf == nullptr)) { // defensive: never send a length we cannot back
        buf = nullptr;
        len = 0;
    }
    EnterCriticalSection(&g_conn_cs);
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
        if (g_conns[i].active)
            send_frame(i, FLAG_START, (int16_t)g_my_id, MH_NET_BROADCAST, buf, len);
    LeaveCriticalSection(&g_conn_cs);
}

extern "C" void MH_Net_SetLeaveHandler(MH_LeaveCb cb) { g_leave_cb = cb; }

// Send a FLAG_LEAVE control frame to every active connection (client -> host). Empty payload -- the
// frame's arrival + sender player id IS the signal. Routed to the host's g_leave_cb, never the game queue.
extern "C" void MH_Net_SendLeave(void) {
    if (!g_started) return;
    EnterCriticalSection(&g_conn_cs);
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
        if (g_conns[i].active)
            send_frame(i, FLAG_LEAVE, (int16_t)g_my_id, MH_NET_BROADCAST, nullptr, 0);
    LeaveCriticalSection(&g_conn_cs);
}

extern "C" void MH_Net_SetAnnounceHandler(MH_AnnounceCb cb) { g_announce_cb = cb; }

// Broadcast a FLAG_ANNOUNCE control frame to every active connection (host -> all peers). Routed to the
// clients' g_announce_cb, never the game queue -- determinism-safe, exactly like SESSION_INFO. (U16)
extern "C" void MH_Net_SendAnnounce(const unsigned char *buf, int len) {
    if (!g_started || len < 0 || len > MH_NET_MAX_PAYLOAD) return;
    EnterCriticalSection(&g_conn_cs);
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
        if (g_conns[i].active)
            send_frame(i, FLAG_ANNOUNCE, (int16_t)g_my_id, MH_NET_BROADCAST, buf, len);
    LeaveCriticalSection(&g_conn_cs);
}

extern "C" void MH_Net_SetHashHandler(MH_HashCb cb) { g_hash_cb = cb; }

// Broadcast a FLAG_HASH desync sample to every active connection (D21). Routed to each peer's
// g_hash_cb, never the game queue -- determinism-safe like SESSION_INFO/ANNOUNCE, which is the whole
// reason the sample rides this channel and not the game's own lockstep wire: the retail dispatcher
// treats an outer tag it does not know as a GARBLED STREAM and ends the session (turn_engine.h
// LS_SESSION_ENDED), so a detector carried there would be a session-killer on any older peer.
extern "C" void MH_Net_SendHash(const unsigned char *buf, int len) {
    if (!g_started || len <= 0 || len > MH_NET_MAX_PAYLOAD) return;
    EnterCriticalSection(&g_conn_cs);
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
        if (g_conns[i].active)
            send_frame(i, FLAG_HASH, (int16_t)g_my_id, MH_NET_BROADCAST, buf, len);
    LeaveCriticalSection(&g_conn_cs);
}

extern "C" int MH_Net_PeerCount(void) {
    if (!g_started) return 0;
    int n = 0;
    EnterCriticalSection(&g_conn_cs);
    for (int i = 0; i < MH_NET_MAX_PEERS; ++i)
        if (g_conns[i].active) ++n;
    LeaveCriticalSection(&g_conn_cs);
    return n;
}

// N1: this peer's own player id (from [net] player_id, or the host-assigned id after a WELCOME). Each peer
// seats itself at its own slot from this, so a >2-player game puts client k at PlayerSide=k, not is_host?0:1.
extern "C" int MH_Net_LocalPlayerId(void) { return g_started ? g_my_id : -1; }

// N1: 1 once this peer's id is settled (host / declared-id client immediately; assign-mode client only after
// the host's WELCOME). The lobby glue gates entry on this so a client never seats itself at the default slot.
extern "C" int MH_Net_IdAssigned(void) { return InterlockedCompareExchange(&g_id_assigned, 0, 0) != 0; }

// N2: enumerate the ACTIVE connections' transport player ids (their g_conns[].player_id -- the host-assigned
// id in host_assign mode, or the HELLO-declared id otherwise). Skips a conn whose id isn't settled yet
// (player_id < 1). Replaces the peer-table mirror's positional pid=i+1, which mislabels every peer above a
// gap once the live id set isn't {1..N} (a middle peer left / a non-contiguous assigned id).
extern "C" int MH_Net_ActivePeerIds(int *out, int cap) {
    if (!g_started || !out || cap <= 0) return 0;
    int n = 0;
    EnterCriticalSection(&g_conn_cs);
    for (int i = 0; i < MH_NET_MAX_PEERS && n < cap; ++i)
        if (g_conns[i].active && g_conns[i].player_id >= 1 && g_conns[i].player_id <= 7)
            out[n++] = g_conns[i].player_id;
    LeaveCriticalSection(&g_conn_cs);
    return n;
}


// ---- mp:X1b -- THE SNAPSHOT ROWS, ANSWERED "UNSUPPORTED" -----------------------------------------
//
// THIS IS THE HALF OF mp:T2'S RULING THAT MAKES THE OTHER HALF LEGAL. T2 refused a 24th module row
// on the ground that mh_net.dll has no channel C and a contract half the implementations cannot
// answer is not a contract. X1b keeps the ground and removes the objection: the rows exist for both
// modules, and this one answers them with a NAMED status.
//
// UNSUPPORTED IS NOT A FAILURE CODE AND NOT A CRASH, and both halves of that matter:
//
//   NOT A CRASH, because these bodies are reachable. mh.dll's shims forward to whichever module is
//   bound, and `[net] transport` picks that at boot; a lane running the shipped default with a build
//   whose harness arms `snapshot_at` would land here. A stub that faulted would turn a configuration
//   mistake into a game crash, which is the one thing the whole absent-tolerant surface exists to
//   stop.
//
//   NOT A ZERO EITHER. Poll's *out_state is MH_SNAP_UNSUPPORTED rather than MH_SNAP_IDLE. IDLE says
//   "no transfer is running YET", which is an invitation to keep polling; UNSUPPORTED says "no
//   transfer can ever run on this link", which is terminal and is what the caller needs to log and
//   stop. This is MH_NetStats.lat_supported's rule (mp:T3) applied one surface over: "this transport
//   cannot do that" and "it did that and the answer was 0" are different claims and the surface says
//   which one it is making.
//
// THE PARAMETERS ARE READ AND DISCARDED EXPLICITLY. `(void)x` rather than an unnamed parameter,
// because an unnamed parameter is a decision the reader has to reconstruct from the signature and a
// cast is a statement that this body saw the argument and had nothing to do with it.
//
// WHY NOT FORWARD TCP BULK OVER THE RELIABLE STREAM. It would work -- the star topology already has
// an ordered byte stream per peer -- and it is still wrong here: the pipeline's manifest, chunk
// indices and resume-by-frontier are channel C's objects (mh_net_udp/udp_snapshot.h), and a second
// implementation over a second substrate would be a second thing to keep correct for a transport
// that is the DETERMINISM GATE'S REFERENCE and is deliberately left as the build it was. If TCP ever
// needs snapshots, the honest move is to lift the pipeline out of mh_net_udp, not to re-write it.
extern "C" int MH_Net_SnapshotSend(int dst_player, const void *blob, int len) {
    (void)dst_player;
    (void)blob;
    (void)len;
    return 0;
}

extern "C" int MH_Net_SnapshotPoll(void *buf, int *inout_len, int *out_state) {
    (void)buf;
    if (inout_len) *inout_len = 0;
    if (out_state) *out_state = MH_SNAP_UNSUPPORTED;
    return 0;
}

extern "C" void MH_Net_SnapshotStatus(MH_NetSnapshotStatus *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->size      = (unsigned)sizeof(MH_NetSnapshotStatus);
    out->supported = 0;
    out->state     = MH_SNAP_UNSUPPORTED;
    // root_hex is zeroed by the memset above, which IS the empty string -- stated rather than left
    // to the reader, because "" and "0000...0" are different answers and only one of them is true.
}

// ---- the module's one INTERNAL entry (fork F4B) --------------------------------------------------
//
// NOT exported (it is absent from mh_net.def): mh.dll never calls it. MH_NetModule_Init, in
// mh_net_dllmain.cpp, is the exported half; it owns the host-struct validation and the DllMain
// observations, and this is how it reaches the run directory this TU logs into. Two TUs because the
// attach counters belong to the DllMain that records them and the log path belongs to the code that
// writes the log; one accessor between them is cheaper than making either global public.
extern "C" void MH_NetInternal_SetRunDir(const char *dir) {
    if (dir == nullptr || dir[0] == '\0') return;
    lstrcpynA(g_run_dir, dir, MAX_PATH);
}
