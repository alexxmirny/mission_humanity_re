//
// udp_relay.cpp -- see udp_relay.h. The loopback tunnel that puts the Rust relay (src/relay) in
// the path of mh_net_udp.dll's datagrams (tracker mp:R1).
//
// ---- THE LEG PROTOCOL ---------------------------------------------------------------------------
//
// This file is the C++ half of `src/relay/src/leg.rs`, and the two are a CONTRACT: the same 18-byte
// header, the same ops, the same HMAC. docs/mp-relay.md is the normative description; a change on
// one side without the other shows up as the relay counting `leg_bad_magic` and the peer never
// getting a WELCOME, which is at least a loud failure rather than a quiet one.
//
//   0  1  magic/version  high nibble 0x5 (the relay-leg family; T0's packets are 0x4), low nibble 1
//   1  1  op             HELLO / WELCOME / DATA / PING / PONG / BYE / ERROR (+ R2's four, R3's three)
//   2  2  src  LE u16    the SENDER's relay handle; 0 before one is assigned
//   4  2  dst  LE u16    the destination handle; 0 = the relay itself (or, for a client's DATA,
//                        "my room's host", which is the only counterpart a client has)
//   6  4  room LE u32    the rendezvous code
//  10  8  seq  LE u64    per leg direction; the relay holds a 64-entry replay window
//  18  n  payload        for DATA: the T0 datagram, byte for byte
// 18+n 16 tag            HMAC-SHA256(leg_key, bytes[0, 18+n)) truncated to 128 bits
//
// The leg is AUTHENTICATED, NOT ENCRYPTED: the payload is already sealed end to end, and the relay
// has to read the T0 header's conn_id to decide what it may do with it.
//
// ---- THE ROOM, AND WHERE IT COMES FROM -----------------------------------------------------------
//
// A relay serves many matches on one socket, so a joining client has to say WHICH host it wants
// before either peer has a conn_id. That code is the room. R1 made it `[net] port` -- not because a
// port is a good lobby code, but because two peers that already agree on a port already agree on a
// room, so a relayed game needed no new UI and no new thing to type. mp:R2 taught the CLIENT to
// re-dial whatever room the relay's directory names, and mp:R6 made the HOST'S room a random
// 30-bit code MINTED at every tunnel start (udp_room.h) -- on a shared relay every player ships with
// `port=6501`, so the port-as-room made the second host at any one moment `room_busy`. A `room_busy`
// that still arrives (a 2^-30 collision, or a relay that answers it to everything) is met by a
// bounded re-mint below, and only then by R1's refusal line.
//
// ---- ANSWER THE WELCOME IMMEDIATELY --------------------------------------------------------------
//
// The relay holds an address it has not heard from SINCE IT REPLIED to 3x the bytes that address
// sent (plan D4's anti-amplification cap). A HELLO is 52 bytes, so the budget is ~156 and the
// WELCOME spends 38 of it -- which a single forwarded game datagram would then exceed. So the
// moment a WELCOME lands, this file PINGs. That one datagram is what validates the address and
// lifts the cap; without it the first packet of the match is the one the cap eats. Measured
// exactly that way in the relay's own loopback test before the ping was added.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define _WINSOCK_DEPRECATED_NO_WARNINGS // inet_addr, as in udp_endpoint.cpp's direct-connect path
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "udp_relay.h"
#include "udp_punch.h"            // mp:R3 -- the promotion state machine, which owns no socket
#include "udp_room.h"             // mp:R6 -- the host's minted room, and the re-mint on room_busy
#include "mh_net_key.h"           // MH_Key_Random -- the CSPRNG the room is drawn from
#include "mh_net_proto/net_udp.h" // mp:R1c -- the T0 codec, to read the grant we are carrying

// SIO_ADDRESS_LIST_QUERY is how a bound socket is asked for the local addresses of the machine it
// is bound on -- the LOCAL half of a candidate list. Same guarded fallback SIO_UDP_CONNRESET gets
// below, for the same reason: which SDK header carries it has moved, and the public value has not.
#ifndef SIO_ADDRESS_LIST_QUERY
#define SIO_ADDRESS_LIST_QUERY _WSAIOR(IOC_WS2, 22)
#endif

// SIO_UDP_CONNRESET is a Windows-only vendor ioctl and which SDK header carries it has moved
// around (mswsock.h / mstcpip.h, and neither reliably under WIN32_LEAN_AND_MEAN). Same guarded
// fallback udp_endpoint.cpp carries, for the same reason and with the same public value.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib") // wsprintfA

namespace mh {
namespace udprelay {

using namespace mh_net_proto;
namespace U = mh_net_proto::udp; // mp:R1c -- same alias udp_endpoint.cpp uses for the T0 codec

namespace {

constexpr uint8_t LEG_MAGIC_NIBBLE = 0x5;
constexpr uint8_t LEG_VERSION      = 1;
constexpr uint8_t LEG_MAGIC_VER    = (uint8_t)((LEG_MAGIC_NIBBLE << 4) | LEG_VERSION);

constexpr int LEG_HDR = 18;
constexpr int LEG_TAG = 16;
// T0's 1200-byte ceiling plus this header and tag. The receive buffer is larger so an oversize
// datagram is SEEN (and refused for its size) rather than silently truncated by recvfrom.
constexpr int T0_MAX  = 1200;
constexpr int LEG_MAX = LEG_HDR + T0_MAX + LEG_TAG;

constexpr uint8_t OP_HELLO   = 1;
constexpr uint8_t OP_WELCOME = 2;
constexpr uint8_t OP_DATA    = 3;
constexpr uint8_t OP_PING    = 4;
constexpr uint8_t OP_PONG    = 5;
constexpr uint8_t OP_BYE     = 6;
constexpr uint8_t OP_ERROR   = 7;
// mp:R2, the session directory. Four ops on top of R1's seven; the relay's half is leg.rs.
constexpr uint8_t OP_REGISTER   = 8;  // host -> relay: "this is my lobby"; payload = the descriptor
constexpr uint8_t OP_UNREGISTER = 9;  // host -> relay: "my lobby is gone"
constexpr uint8_t OP_LIST       = 10; // registered peer -> relay: "what is hosted here"
constexpr uint8_t OP_SESSIONS   = 11; // relay -> peer: one page of the directory
// mp:R3, hole punching. ONE relay op and TWO that never touch the relay: the candidate exchange
// goes through it (it is the only thing both peers can already reach, and it is the only party that
// knows where each of them is SEEN from), the probes go straight across.
constexpr uint8_t OP_CAND      = 12; // peer -> relay -> counterpart: "here is where to try me"
constexpr uint8_t OP_PROBE     = 13; // peer -> peer, directly
constexpr uint8_t OP_PROBE_ACK = 14; // peer -> peer, directly: the echo that validates a path
constexpr uint8_t OP_LAST      = OP_PROBE_ACK;

// ERROR codes, mirrored from leg.rs so a refusal can be acted on and not only printed.
constexpr uint8_t ERR_NO_HOST        = 1;
constexpr uint8_t ERR_ROOM_BUSY      = 2;
constexpr uint8_t ERR_NOT_REGISTERED = 3;

// SESSIONS page header: total u16 | offset u16 | count u8, then count x (room u32 | len u16 | bytes).
constexpr int SESSIONS_HDR       = 5;
constexpr int SESSIONS_ENTRY_HDR = 6;

constexpr uint8_t ROLE_HOST   = 0;
constexpr uint8_t ROLE_CLIENT = 1;

constexpr uint16_t HANDLE_NONE = 0;

// mp:R1c -- HELLO flags bit 0, mirrored from leg.rs: "I can re-key from the connect token".
constexpr uint8_t HELLO_FLAG_PEER_KEY = 0x01;

// ---- mp:R4a: THE PROTOCOL LEVEL ------------------------------------------------------------------
//
// The leg's low nibble is the WIRE version; the OPS a relay speaks are a second axis it does not
// describe. Seven ops were added over three items "additively": an older relay refuses one it lacks
// as a counted `bad_op` -- counted at the RELAY, answered with nothing. So this file, against a
// relay built before an op it sends, sees a leg that comes UP, a match that is carried, and the new
// thing silently never happening (dead-ends G241: a pre-wave-9 relay refused the re-key op and the
// pair never promoted; nothing in mh_net.log said why).
//
// The level is what makes that a visible failure. HELLO carries the level this build was made
// against; WELCOME carries the relay's; `on_relay_level` compares and, when the relay is behind,
// writes ONE named line and puts a notice on the browser (through mh.dll's F3c carrier, resolved by
// name like the browser predicate). MIRRORED FROM leg.rs, WHICH OWNS THE NUMBER AND ITS HISTORY:
// bump both in one commit when an op is added or changed. 0 = "never advertised" = every build
// before R4a, which the peer treats as old.
//
// WHERE IT RIDES: `flags >> 1`. A pre-R4a relay's hello_parse refuses any HELLO that is not exactly
// 18 bytes (malformed, unanswered) -- a trailing byte would make this build unable to register with
// the deployed relay at all. The flags byte is the one place an old relay reads and carries
// unknown bits through untouched (it tests bit 0 only), so the level sits above the re-key bit.
// The WELCOME grows a fifth byte ONLY when the HELLO carried a level: the pre-R4a reader below
// refused any other length, so a current relay keeps sending 4 bytes to an older peer.
constexpr uint8_t PROTOCOL_LEVEL    = 1;
constexpr int     HELLO_LEVEL_SHIFT = 1;

constexpr DWORD HELLO_RETRY_MS = 500;   // until the WELCOME lands
constexpr DWORD KEEP_MS        = 20000; // plan D4's keepalive floor; the relay pings at 25 s
constexpr DWORD SELECT_MS      = 100;   // the thread's timer granularity
// mp:R2. REGISTER_MS is a REFRESH rate against the relay's 20 s session TTL, so twenty may be lost
// before a live lobby falls out of the directory. SI_STALE_MS is the other direction: mh.dll
// re-advertises at ~1 Hz while it sits in its lobby, so five seconds of nothing means the lobby is
// gone and the row should be withdrawn rather than left to age out.
constexpr DWORD REGISTER_MS = 1000;
constexpr DWORD SI_STALE_MS = 3000;
constexpr DWORD LIST_MS     = 2000; // how often a browsing client re-asks for the directory
// mp:R3. How often this peer re-publishes its own candidate list to a counterpart that is still
// relayed. Slower than the PROBE cadence on purpose: the candidates change only when an interface
// or a NAT binding does, while the probes are what actually find the path. It stops entirely once a
// pair is direct, which is also the relay's only way to see that a promotion happened.
constexpr DWORD CAND_MS = 1000;

constexpr int MAX_REMOTE = 8; // MH_NET_MAX_PEERS -- one loopback socket per remote peer, host side
// mp:R3e. How often the pump asks the endpoint which of the host's per-peer slots it still holds,
// and how long a freshly made slot is exempt from the answer. The grace is NOT a liveness timer --
// it covers the one window in which "the endpoint does not know this address" is expected: between
// the loopback socket being made for a peer's first datagram and the endpoint registering the
// handshake that datagram carries. HS_BUDGET_MS (4 s) is how long a joiner keeps retrying that
// handshake, so a slot the endpoint has not learned in five seconds is a slot nobody is joining
// through. A peer that IS known stays for as long as the endpoint keeps it, however quiet.
constexpr DWORD SWEEP_MS = 1000;
constexpr DWORD GRACE_MS = 5000;

// mp:R1c -- MIRRORED FROM udp_endpoint.cpp, exactly as leg.rs mirrors them on the other side. The
// handshake channel id and the grant's frame kind are not part of T0 proper (T0's mux rule is that
// an unknown channel is skipped), so they live in the endpoint -- and this file has to recognise
// the one frame that carries a connect token. Getting them wrong is not a mis-parse: the sniff
// simply never fires and every leg quietly stays on the deployment key, which is why
// `peers_rekeyed` on the relay is the counter to read before believing R1c is live.
constexpr uint8_t CH_HS     = 0x40;
constexpr uint8_t HSK_GRANT = 3;

// ...and the boot-key labels, for the same reason and with the same consequence.
const char *const LBL_BOOT_CONN = "mh-udp-boot-conn";
const char *const LBL_BOOT_ENC  = "mh-udp-boot-enc";
const char *const LBL_BOOT_MAC  = "mh-udp-boot-mac";

void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}
void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
}
void put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
}
uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

// Constant-time-ish tag compare, the same fold net_udp.cpp uses: a byte-at-a-time == leaks the
// matching prefix through timing.
bool tag_eq(const uint8_t *a, const uint8_t *b, int n) {
    uint8_t acc = 0;
    for (int i = 0; i < n; ++i) acc |= (uint8_t)(a[i] ^ b[i]);
    return acc == 0;
}

struct Remote {
    bool        used;
    uint16_t    handle;
    SOCKET      s;       // a loopback socket of its own, so the endpoint sees distinct peers
    sockaddr_in from_ep; // the endpoint's address as seen on THIS socket
    bool        have_ep;
    sockaddr_in self; // mp:R3e -- what the ENDPOINT sees this peer as: 127.0.0.1:<s's port>
    DWORD       born; // mp:R3e -- when the slot was made (the GRACE_MS exemption counts from here)
};

// mp:R3 -- ONE PUNCH PER PEER PAIR, which is what makes this an array on both roles. A host holds
// one per client (they punch independently and one may go direct while another cannot); a client
// holds exactly one, for its host. Kept OUTSIDE `Remote` because a client has no `Remote` at all --
// its counterpart is the single `other_handle` -- and because a `Remote` owns a loopback socket,
// which a punch has no business being tied to the lifetime of.
struct PunchPeer {
    bool                used;
    uint16_t            handle;
    mh::udppunch::Punch pn;
    DWORD               last_cand; // when we last published OUR candidates to this counterpart
    DWORD               born;      // mp:R3e -- for the orphan rule (a punch with no peer behind it)
};

// mp:R1c -- ONE PEER PAIR'S LEG KEYS. `mine` tags what WE send on this pair's behalf; `theirs`
// verifies what that peer sends straight at us (a promoted probe or DATA). A client holds exactly
// one of these, for its host; a host holds one per client, because each client's token is its own.
//
// Why not fold it into PunchPeer, which is already keyed by counterpart handle: a punch is about a
// PATH and this is about a SESSION, and the two have different lifetimes -- a pair that demotes and
// re-punches keeps its keys, and a peer with no punch at all (force_relay) still has them.
struct KeyPeer {
    bool     used;
    uint16_t handle;
    uint8_t  mine[KEY_LEN];
    uint8_t  theirs[KEY_LEN];
};

struct State {
    volatile LONG running;
    Config        cfg;
    uint8_t       leg_key[KEY_LEN]; // the DEPLOYMENT key -- what every leg starts on
    // mp:R1c. The key this peer tags its RELAY-DIRECTED traffic with (PING, BYE, REGISTER, LIST,
    // and a still-relayed DATA), once any session has minted one. A host has several pair keys and
    // has to pick one for traffic that names no pair; the relay accepts any key that handle owns,
    // so "the first one installed" is a complete answer and needs no agreement.
    uint8_t my_key[KEY_LEN];
    bool    my_key_set;
    // ...and whether the RELAY has been seen using it. Until then a relay datagram may still arrive
    // under the deployment key (the relay installs when it forwards the grant, we install when we
    // read it, and the two are microseconds apart in an order that depends on the role). After it,
    // the deployment key is refused -- with the ERROR exception the decoder documents.
    bool    my_key_proved;
    KeyPeer kk[MAX_REMOTE];
    // mp:R1c -- the deployment PSK and the three boot secrets derived from it. Held so the tunnel
    // can OPEN the grant it is already carrying: the token is the per-peer secret, and the tunnel
    // is on the path of the one datagram that contains it. That is deliberately the whole of the
    // plumbing -- the alternative was a new call from udp_endpoint.cpp into this file, which would
    // put relay knowledge in the one file mp:R1 kept free of it.
    uint8_t     psk[KEY_LEN];
    uint8_t     boot_conn[8];
    uint8_t     boot_enc[KEY_LEN];
    uint8_t     boot_mac[KEY_LEN];
    sockaddr_in relay;
    SOCKET      leg; // the one socket the relay ever sees -- and mp:R3's v4 PUNCH socket
    // mp:R3. A SECOND socket, for IPv6 candidates only. It is not a tidiness split: the punch has
    // to leave from a socket whose source address is the candidate the peer was told to answer, and
    // a v4 socket has no v6 source address to offer. The leg itself stays v4 (the relay's own
    // address is `[net] relay`, which has always been a v4 address or a name resolved to one), so
    // this socket carries nothing but probes and, once a v6 path is promoted, that pair's DATA.
    // INVALID_SOCKET on a machine with no IPv6 -- a normal state, not a failure: the tunnel then
    // gathers and probes v4 only.
    SOCKET         leg6;
    unsigned short leg_port;  // the v4 punch source port -- what a local v4 candidate advertises
    unsigned short leg6_port; // ...and the v6 one
    SOCKET         local;     // client only: the port udp_endpoint.cpp dials
    unsigned short local_port;
    sockaddr_in    ep_addr; // client only: where the endpoint's datagrams came from
    bool           have_ep;
    Remote         rem[MAX_REMOTE]; // host only
    HANDLE         thread;

    volatile LONG self_handle;  // 0 until the WELCOME lands
    volatile LONG other_handle; // client: its host's handle
    // mp:R4b -- the relay has said NOT_REGISTERED for the handle we hold. The handle is KEPT (the
    // re-HELLO names it, and a restarted relay gives it back if it is free, so every table the
    // counterpart keys by it stays valid) and this flag is what says "re-HELLO, and put nothing
    // else on the relay leg until the WELCOME". Cleared by the WELCOME.
    volatile LONG leg_lost;
    DWORD         lost_at;
    // mp:R4a -- what the relay's WELCOME said its level was (0 = it said nothing: pre-R4a), and
    // whether that value has been reported. Logged on CHANGE, not once: a relay redeployed
    // mid-session (R4b) may come back at another level, and the re-HELLO's WELCOME says so.
    uint8_t  relay_level;
    bool     relay_level_known;
    bool     relay_leg_version_reported; // ...and the one line for a leg version we cannot read at all
    bool     relay_unknown_op_reported;  // ...and the one line for an op we do not have (a newer relay)
    uint64_t tx_seq;
    DWORD    last_hello;
    DWORD    last_keep;

    CRITICAL_SECTION cs; // guards match_id + match_dirty + the session descriptor (game thread)
    uint8_t          match_id[UUID7_BYTES];
    bool             match_dirty;

    // mp:R2, host side. Written by the game thread through set_session_info, read by the pump.
    uint8_t si[DESC_MAX];
    int     si_len;
    DWORD   si_at;         // when the game last handed one over (staleness, not registration)
    bool    si_registered; // we have told the relay about it and not yet withdrawn it
    DWORD   last_register;
    DWORD   last_list; // mp:R2, client side
    bool    listing;   // mp:R2a -- the browser was up on the previous pump (transition edge only)

    // mp:R3, hole punching. `pp` is read and written ONLY on the pump thread, like everything else
    // in this struct except the critical-section-guarded block above it.
    PunchPeer          pp[MAX_REMOTE];
    DWORD              last_sweep;                    // mp:R3e -- the per-peer teardown sweep's own timer
    mh::udppunch::Cand mine[mh::udppunch::MAX_CANDS]; // this peer's own local candidates
    int                mine_n;
    DWORD              mine_at; // when they were last gathered (interfaces come and go)
    bool               force_relay;

    // mp:R6, host only. The room this host names and how many `room_busy` answers it has already
    // moved away from (udp_room.h). `cfg.room` is what the leg header carries, so a re-mint
    // rewrites both; pump-thread only, like everything above the critical section.
    mh::udproom::Minter minter;

    log_fn log;
    void  *log_ctx;
};

State g;
bool  g_started = false;

// The directory sink lives OUTSIDE State because State is memset by start(): a sink registered
// before the tunnel comes up (or one that must survive the U40 stop/start relink) would otherwise
// be silently forgotten, and the symptom -- a browser that lists nothing -- looks exactly like a
// relay with no sessions on it.
dir_fn g_dir     = nullptr;
void  *g_dir_ctx = nullptr;
// mp:R3e -- the peer-ownership predicate, outside State for the same reason (udp_relay.h).
known_fn g_known     = nullptr;
void    *g_known_ctx = nullptr;

// ---- mp:L1f -- THE PUBLISHED PATH CLASSIFICATION (udp_relay.h, `path_class`) --------------------
//
// One row per remote, PARALLEL TO `g.rem[]` and indexed the same way on a host; row MAX_REMOTE is
// the client's single counterpart. Outside State for a second reason beyond g_dir's: State is
// pump-thread-only by contract ("`pp` is read and written ONLY on the pump thread, like everything
// else in this struct except the critical-section-guarded block above it"), and the READER here is
// the GAME thread inside Endpoint::get_stats. Rather than widen that contract -- or take g.cs on
// the datagram path, which is the one path in this file that must not grow a lock -- the pump
// PUBLISHES two LONGs and the reader reads two LONGs.
//
//   g_pc_port[i]  the loopback port the ENDPOINT sees this remote under (host: Remote::self's port;
//                 client: the tunnel's own dial port). 0 = the row is free.
//   g_pc_cls[i]   -1 unknown (no DATA delivered yet) / 0 direct / 1 relayed.
//
// Aligned 32-bit scalars, so neither can tear; the port is written LAST when a row is claimed and
// FIRST when it is released, so a reader that sees a port sees a class that was already written for
// it. The worst a race can do is hand back the previous tick's letter for one frame of a lobby,
// which is display-only by construction -- see the udp_relay.h banner.
volatile LONG g_pc_port[MAX_REMOTE + 1];
volatile LONG g_pc_cls[MAX_REMOTE + 1];
constexpr int PC_CLIENT = MAX_REMOTE; // the client's one row

void pc_clear_all() {
    for (int i = 0; i <= MAX_REMOTE; ++i) {
        InterlockedExchange(&g_pc_port[i], 0);
        InterlockedExchange(&g_pc_cls[i], -1);
    }
}

// Claim row `i` for a remote the endpoint will see on `port`. Class first, then the port.
void pc_open(int i, unsigned short port) {
    InterlockedExchange(&g_pc_cls[i], -1);
    InterlockedExchange(&g_pc_port[i], (LONG)port);
}

void pc_close(int i) {
    InterlockedExchange(&g_pc_port[i], 0);
    InterlockedExchange(&g_pc_cls[i], -1);
}

// The latch itself: the LAST ACCEPTED DATA frame's path. Called from the pump thread only, once per
// delivered OP_DATA, and deliberately not from anywhere else -- a PROBE or a WELCOME says nothing
// about where this peer's GAME traffic is flowing.
void pc_latch(int i, bool from_relay) { InterlockedExchange(&g_pc_cls[i], from_relay ? 1 : 0); }

void logf(const char *fmt, ...) {
    if (!g.log) return;
    char    line[400];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(line, fmt, ap);
    va_end(ap);
    g.log(g.log_ctx, line);
}

void hex32(const uint8_t *id, char *out) {
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < (int)UUID7_BYTES; ++i) {
        out[i * 2]     = H[id[i] >> 4];
        out[i * 2 + 1] = H[id[i] & 0x0f];
    }
    out[UUID7_BYTES * 2] = '\0';
}

bool nil_id(const uint8_t *id) {
    for (int i = 0; i < (int)UUID7_BYTES; ++i)
        if (id[i] != 0) return false;
    return true;
}

// ---- mp:R2a: "is the player looking at the session browser?" --------------------------------------
//
// An OPTIONAL call into mh.dll, resolved once by name. `MH_Seam_RelayBrowsing` is a
// __declspec(dllexport) on net_discovery.cpp's copy of the U40 browser predicate, and the whole
// contract is its absence: net_selftest.exe has no mh.dll, a standalone module has no mh.dll, and an
// mh.dll older than mp:R2a does not export it -- all three resolve to null and get R2's behaviour,
// an unconditional poll. THAT is why it is a GetProcAddress and not a module-table row: the table is
// a contract BOTH transports must answer, and the TCP module has no relay to poll with.
//
// Resolution is attempted exactly once (a failed GetProcAddress must not be retried at 10 Hz for the
// life of the process) and the result is cached in a plain static: this runs on one thread.
typedef int(__cdecl *browsing_fn)(void);

bool mh_browsing() {
    static browsing_fn fn    = nullptr;
    static bool        tried = false;
    if (!tried) {
        tried           = true;
        const HMODULE m = GetModuleHandleA("mh.dll");
        if (m != nullptr) fn = (browsing_fn)GetProcAddress(m, "MH_Seam_RelayBrowsing");
        if (fn == nullptr)
            logf("net: udp relay -- mh.dll exports no browser predicate; the session directory "
                 "will be polled unconditionally (mp:R2a is inactive in this build)");
    }
    return fn == nullptr || fn() != 0;
}

// ---- mp:R4a: the relay notice, into mh.dll by name --------------------------------------------------
//
// `MH_Seam_RelayNotice` is the same shape as the browser predicate above and for the same reason:
// mh.dll -> module is the table both transports answer, module -> mh.dll is optional by
// construction. It hands mh.dll one short ASCII line for the browser's status line (F3c's carrier,
// which the host's JOIN refusal already paints through); absent -- net_selftest.exe, a standalone
// module, an older mh.dll -- the log line below is the whole report, which is still one more line
// than G241 had. Resolved once; called from the pump thread, so the mh.dll side hands the text to
// its main thread rather than touching a widget.
typedef void(__cdecl *notice_fn)(const char *line);

void mh_notice(const char *line) {
    static notice_fn fn    = nullptr;
    static bool      tried = false;
    if (!tried) {
        tried           = true;
        const HMODULE m = GetModuleHandleA("mh.dll");
        if (m != nullptr) fn = (notice_fn)GetProcAddress(m, "MH_Seam_RelayNotice");
        if (fn == nullptr)
            logf("net: udp relay -- mh.dll exports no relay-notice carrier; a relay protocol "
                 "mismatch reaches this log only (mp:R4a)");
    }
    if (fn != nullptr) fn(line);
    else logf("net: udp relay -- browser notice not deliverable in this build: %s (mp:R4a)", line);
}

// The decision, on every WELCOME:
//
//   theirs == ours   nothing (the ordinary case; a matching pair logs no line)
//   theirs <  ours   ONE `relay protocol <theirs> < <ours>` line naming what will not work, and the
//                    browser notice. 0 is "the relay advertised none" (pre-R4a) and is this case.
//   theirs >  ours   ONE info line, no notice: everything this build sends is inside the relay's
//                    table, so nothing will be refused -- the relay is simply newer.
//
// "Will not work" is stated from the relay's side of the table: an op this build sends that the
// relay lacks is a silent `bad_op` there, so the line names the ops by item rather than guessing
// which one the gap is -- a level-0 relay is behind on every item since the one that introduced
// the level, and a relay at a lower non-zero level is behind on the items between the two numbers,
// which leg.rs's history beside PROTOCOL_LEVEL enumerates.
void on_relay_level(uint8_t theirs) {
    if (g.relay_level_known && theirs == g.relay_level) return;
    g.relay_level_known = true;
    g.relay_level       = theirs;
    if (theirs < PROTOCOL_LEVEL) {
        // Under logf's 400-byte line with a 63-byte host: keep this one short.
        logf("net: udp relay -- relay protocol %u < %u (the relay at %s:%u %s; an op it lacks is "
             "refused there as bad_op with no answer, so re-keying, punching, room re-mint or "
             "restart recovery may silently not happen -- update the relay to level %u) (mp:R4a)",
             (unsigned)theirs, (unsigned)PROTOCOL_LEVEL, g.cfg.host, (unsigned)g.cfg.port,
             theirs == 0 ? "advertised no level: a pre-R4a build" : "is a build behind this one",
             (unsigned)PROTOCOL_LEVEL);
        char n[64];
        wsprintfA(n, "Relay outdated (protocol %u < %u)", (unsigned)theirs, (unsigned)PROTOCOL_LEVEL);
        mh_notice(n);
    } else if (theirs > PROTOCOL_LEVEL) {
        logf("net: udp relay -- relay protocol %u > %u (the relay is newer than this build; every op "
             "this build sends is in its table, so nothing is refused) (mp:R4a)",
             (unsigned)theirs, (unsigned)PROTOCOL_LEVEL);
    }
}

// ---- the leg codec -------------------------------------------------------------------------------

int leg_encode(const uint8_t key[KEY_LEN], uint8_t op, uint16_t src, uint16_t dst, uint32_t room,
               uint64_t seq, const uint8_t *payload, int plen, uint8_t *out) {
    if (plen < 0 || plen > T0_MAX) return 0;
    out[0] = LEG_MAGIC_VER;
    out[1] = op;
    put16(out + 2, src);
    put16(out + 4, dst);
    put32(out + 6, room);
    put64(out + 10, seq);
    if (plen > 0) memcpy(out + LEG_HDR, payload, (size_t)plen);
    uint8_t full[SHA256_LEN];
    hmac_sha256(key, KEY_LEN, out, (size_t)(LEG_HDR + plen), full);
    memcpy(out + LEG_HDR + plen, full, LEG_TAG);
    return LEG_HDR + plen + LEG_TAG;
}

// mp:R1c -- the pair-key table. `create` is false everywhere but the one install site: a lookup
// must never mint a slot for a handle nothing has granted a token to.
KeyPeer *keys_for(uint16_t handle, bool create) {
    if (handle == HANDLE_NONE) return nullptr;
    for (int i = 0; i < MAX_REMOTE; ++i)
        if (g.kk[i].used && g.kk[i].handle == handle) return &g.kk[i];
    if (!create) return nullptr;
    for (int i = 0; i < MAX_REMOTE; ++i) {
        if (g.kk[i].used) continue;
        memset(&g.kk[i], 0, sizeof(g.kk[i]));
        g.kk[i].used   = true;
        g.kk[i].handle = handle;
        return &g.kk[i];
    }
    return nullptr;
}

// mp:R1c -- WHICH KEY WE TAG A DATAGRAM WITH. THE PATH DECIDES, NOT THE DESTINATION, and the
// difference is the whole of a bug this nearly shipped with.
//
//   * STRAIGHT AT A PEER (a probe, or a promoted pair's DATA) -> that pair's key. Both ends install
//     it from the same grant, whichever path the grant took, so a promoted pair is keyed per
//     session even when the relay never saw its handshake at all.
//   * AT THE RELAY -> our relay-directed key, and only once the relay has PROVED it holds it
//     (`my_key_proved`, set when a datagram from the relay verifies under one of ours). Until then,
//     the deployment key.
//
// WHY THE SECOND HALF IS A RATCHET AND NOT SIMPLY "use the new key once we have one". mp:R3
// promotes a pair in ~300 ms -- on the rig, BEFORE the T0 handshake completes -- and a grant that
// crossed directly is a grant the relay never saw and never learned a token from. Both peers would
// then be re-keyed and the relay would not be, so every keepalive we sent it would fail its MAC,
// it would evict us at the idle timer, and the relayed path we were keeping warm underneath would
// be gone by the time a demotion needed it. Keying the RELAY leg on what the relay has demonstrably
// got, and the PEER leg on what the pair has, makes the two independent -- which they are.
const uint8_t *key_for_path(uint16_t dst, bool to_relay) {
    if (!to_relay) {
        const KeyPeer *k = keys_for(dst, false);
        if (k != nullptr) return k->mine;
    }
    return g.my_key_proved ? g.my_key : g.leg_key;
}

// Returns the payload length, or -1 for a refusal. Order matches leg.rs: size, magic, version, op,
// then the MAC -- so a malformed header never costs an HMAC.
//
// mp:R1c -- WHICH KEY(S) ARE TRIED is decided by the path, which is the one thing known before the
// tag verifies:
//
//   * FROM THE RELAY -> our own relay-directed key. The deployment key is also tried until the
//     relay has been seen using the new one (the ratchet), and after that ONLY for an OP_ERROR --
//     because the one message a RESTARTED relay has to be able to send us is "I have never heard of
//     this handle", and it can only send that under the key it still has. Forging that error is a
//     nuisance a PSK holder could already cause by other means (it makes us re-HELLO); silently
//     black-holing every match across a relay restart would not be.
//   * FROM A PEER (a promoted pair's traffic, or a probe) -> that peer's key if we hold one, and
//     the deployment key ONLY if we do not. That second half is R3's hole closed: once a pair has a
//     session, a PSK holder can no longer aim our direct path anywhere.
int leg_decode(const uint8_t *pkt, int len, bool from_relay, uint8_t *op, uint16_t *src,
               uint16_t *dst, uint32_t *room) {
    if (len > LEG_MAX || len < LEG_HDR + LEG_TAG) return -1;
    if ((pkt[0] >> 4) != LEG_MAGIC_NIBBLE) return -1;
    if ((pkt[0] & 0x0f) != LEG_VERSION) {
        // mp:R4a -- a leg-family datagram FROM THE RELAY'S ADDRESS at a wire version this build
        // does not speak. Nothing it sends can be read, and nothing it reads of ours would verify,
        // so there is no WELCOME coming to carry a level: this line is the only signal there will
        // be. Unauthenticated by necessity (a tag under another version is not ours to check) and
        // therefore once per tunnel start, on the relay's address only -- a spoofer with the relay's
        // address can cost the player one wrong notice, which is the nuisance R4b already accepted
        // for a forged NOT_REGISTERED.
        if (from_relay && !g.relay_leg_version_reported) {
            g.relay_leg_version_reported = true;
            logf("net: udp relay -- relay protocol leg v%u != v%u (the relay at %s:%u speaks a leg "
                 "version this build does not; nothing it sends can be read -- update whichever "
                 "side is older) (mp:R4a)",
                 (unsigned)(pkt[0] & 0x0f), (unsigned)LEG_VERSION, g.cfg.host, (unsigned)g.cfg.port);
            char n[64];
            wsprintfA(n, "Incompatible relay (leg v%u/v%u)", (unsigned)(pkt[0] & 0x0f),
                      (unsigned)LEG_VERSION);
            mh_notice(n);
        }
        return -1;
    }
    // mp:R4a -- an op above this build's table is refused as before, but from the relay it is
    // refused AFTER the tag has verified, so the one line it earns is about an authentic datagram
    // from a relay that is newer than this build (which a level-aware relay never sends unasked)
    // and not about a port scan.
    const bool unknown_op = (pkt[1] == 0 || pkt[1] > OP_LAST);
    if (unknown_op && !from_relay) return -1;
    const uint8_t  the_op  = pkt[1];
    const uint16_t the_src = get16(pkt + 2);

    const uint8_t *cand[MAX_REMOTE + 2] = {nullptr};
    int            ncand                = 0;
    if (from_relay) {
        // OUR OWN KEYS, AND ALL OF THEM. `my_key` is the first pair key installed and is what we
        // TAG relay-directed traffic with; the relay tags what it sends us with `keys[0]` of the
        // set it holds for our handle, which is the first key IT installed. Those are the same key
        // on a two-peer match and need not be on a host with several clients (the relay learns one
        // token per client and nothing orders the two sides' "first"), so accepting any key this
        // peer owns is what makes a 3-peer relayed match work. It costs at most MAX_REMOTE HMACs
        // on a refusal, and one on the ordinary path, because `my_key` is tried first.
        if (g.my_key_set) cand[ncand++] = g.my_key;
        for (int i = 0; i < MAX_REMOTE && ncand < MAX_REMOTE + 1; ++i)
            if (g.kk[i].used && (!g.my_key_set || memcmp(g.kk[i].mine, g.my_key, KEY_LEN) != 0))
                cand[ncand++] = g.kk[i].mine;
        if (!g.my_key_proved || the_op == OP_ERROR || !g.my_key_set) cand[ncand++] = g.leg_key;
    } else {
        const KeyPeer *k = keys_for(the_src, false);
        if (k != nullptr) cand[ncand++] = k->theirs;
        else cand[ncand++] = g.leg_key;
    }

    const int end = len - LEG_TAG;
    uint8_t   full[SHA256_LEN];
    int       hit = -1;
    for (int i = 0; i < ncand && hit < 0; ++i) {
        hmac_sha256(cand[i], KEY_LEN, pkt, (size_t)end, full);
        if (tag_eq(full, pkt + end, LEG_TAG)) hit = i;
    }
    if (hit < 0) return -1;
    if (unknown_op) {
        if (!g.relay_unknown_op_reported) {
            g.relay_unknown_op_reported = true;
            logf("net: udp relay -- the relay sent op %u, above this build's table (%u); a newer "
                 "relay -- ignored (mp:R4a)",
                 (unsigned)the_op, (unsigned)OP_LAST);
        }
        return -1;
    }
    // The relay used one of our per-peer keys: the handover is complete in this direction and the
    // shared key is no longer good for anything but an ERROR.
    if (from_relay && g.my_key_set && cand[hit] != g.leg_key && !g.my_key_proved) {
        g.my_key_proved = true;
        logf("net: udp relay leg re-keyed -- this peer's leg is now authenticated with a key "
             "derived from its own connect token, not the deployment key (mp:R1c)");
    }
    *op   = the_op;
    *src  = the_src;
    *dst  = get16(pkt + 4);
    *room = (uint32_t)pkt[6] | ((uint32_t)pkt[7] << 8) | ((uint32_t)pkt[8] << 16) |
            ((uint32_t)pkt[9] << 24);
    return end - LEG_HDR;
}

// One leg datagram, at a named socket and a named destination. THE RELAY IS NOT SPECIAL HERE, and
// that is the whole of mp:R3's send side: a promoted pair's DATA is the same bytes, the same
// header, the same MAC and the same sequence space -- only the sockaddr differs. Two things follow
// that are worth stating because both were design choices and neither is obvious:
//
//   * THE DIRECT PATH STILL PAYS THE 34-BYTE LEG ENVELOPE, even though nothing in the middle reads
//     it any more. Stripping it would make the MTU of a promoted pair 34 bytes larger than the same
//     pair a second earlier, so a datagram that fitted before the promotion could stop fitting
//     after it -- a size-dependent failure that appears only on transitions. Keeping the envelope
//     keeps the path change invisible to every layer above.
//   * ONE SEQUENCE SPACE across both paths. `tx_seq` has exactly one writer (the pump thread) and
//     the relay holds a 64-entry replay window over what IT sees, which after a promotion is only
//     keepalives -- still strictly increasing, with gaps, which is what a sliding window is for.
void leg_send_at(SOCKET s, const sockaddr *to, int tolen, bool to_relay, uint8_t op, uint16_t dst,
                 const uint8_t *payload, int plen) {
    if (s == INVALID_SOCKET || to == nullptr) return;
    uint8_t   pkt[LEG_MAX];
    const int n = leg_encode(key_for_path(dst, to_relay), op,
                             (uint16_t)InterlockedCompareExchange(&g.self_handle, 0, 0), dst,
                             g.cfg.room, g.tx_seq++, payload, plen, pkt);
    if (n <= 0) return;
    sendto(s, (const char *)pkt, n, 0, to, tolen);
}

void leg_send(uint8_t op, uint16_t dst, const uint8_t *payload, int plen) {
    leg_send_at(g.leg, (const sockaddr *)&g.relay, sizeof(g.relay), true, op, dst, payload, plen);
}

// mp:R1c -- INSTALL ONE PAIR'S LEG KEYS from the token both ends of it hold.
//
// The direction split is the token's own: a CLIENT tags with the c2s-derived key and verifies the
// s2c-derived one, and a HOST does the reverse. Both ends run this code with the same token bytes,
// so the two tables are mirror images with no message between them.
void install_pair_keys(uint16_t peer_handle, const uint8_t mac_c2s[KEY_LEN],
                       const uint8_t mac_s2c[KEY_LEN]) {
    KeyPeer *k = keys_for(peer_handle, true);
    if (k == nullptr) return;
    const bool i_am_client = (g.cfg.role == 1);
    peer_leg_key(i_am_client ? mac_c2s : mac_s2c, i_am_client, k->mine);
    peer_leg_key(i_am_client ? mac_s2c : mac_c2s, !i_am_client, k->theirs);
    if (!g.my_key_set) {
        memcpy(g.my_key, k->mine, KEY_LEN);
        g.my_key_set = true;
        // NOT a switch of the relay-directed send key -- that waits for `my_key_proved`, i.e. for
        // the relay to show it learned the same token. See key_for_path.
        logf("net: udp relay leg -- peer %u's connect token keys this pair (mp:R1c); the relay leg "
             "follows once the relay answers under it",
             (unsigned)peer_handle);
    }
}

// mp:R1c -- LOOK FOR A CONNECT TOKEN IN A T0 DATAGRAM WE ARE CARRYING, and key this pair from it.
//
// `t0` is the sealed T0 datagram verbatim -- the thing this tunnel exists to move -- and the only
// one of them that matters here is the host's handshake datagram 2, whose CH_HS/GRANT frame holds
// the token. The relay reads the same frame out of the same datagram for the same reason
// (`relay.rs::learn_tokens`), so all three parties learn the pair key from one event with nothing
// added to any wire.
//
// CALLED AFTER THE DATAGRAM HAS BEEN SENT OR DELIVERED, and that ordering is the whole handover:
// the datagram carrying the grant is the last one either end can tag with the old key, because the
// far end cannot derive the new one until it has read this payload. Installing first would have us
// tag the grant with a key only the grant can produce -- a deadlock, not a race.
//
// Cheap in the steady state: a pair that already has keys returns on the first line, and a datagram
// whose conn_id is not the bootstrap one returns on the fourth, before any crypto.
void sniff_grant(const uint8_t *t0, int n, uint16_t peer_handle) {
    if (peer_handle == HANDLE_NONE || n < (int)U::HDR_SIZE || n > T0_MAX) return;
    if (keys_for(peer_handle, false) != nullptr) return;
    U::Header h;
    if (!U::hdr_peek(t0, (size_t)n, h)) return;
    if (memcmp(h.conn_id, g.boot_conn, sizeof(g.boot_conn)) != 0) return;

    // packet_decode decrypts IN PLACE, and the bytes we were handed are either already on the wire
    // or about to be -- so it works on a copy. No replay window: this is a passive read of a
    // datagram whose real receiver keeps the only window that means anything.
    uint8_t scratch[T0_MAX];
    memcpy(scratch, t0, (size_t)n);
    size_t    body_len = 0;
    U::Header hh;
    if (U::packet_decode(scratch, (size_t)n, g.boot_conn, g.boot_enc, g.boot_mac, nullptr, hh,
                         &body_len) != U::Verdict::Ok)
        return;

    const uint8_t *body = scratch + U::HDR_SIZE;
    size_t         off  = 0;
    U::Frame       f;
    bool           ok = true;
    while (U::frame_next(body, body_len, &off, f, ok)) {
        if (!ok) return;
        if (f.id != CH_HS || f.len != 1 + U::TOKEN_WIRE || f.data[0] != HSK_GRANT) continue;
        U::ConnectToken tok;
        bool            expired = false;
        // `now_unix_ms = 0` skips the expiry test, exactly as the relay's own token_open call does
        // and for the same reason: the host minted this token and the host checks it. A tunnel that
        // refused to re-key because of its own clock skew would be a fault diagnosable from nowhere.
        if (U::token_open(g.psk, f.data + 1, U::TOKEN_WIRE, 0, tok, expired) != U::Verdict::Ok)
            continue;
        install_pair_keys(peer_handle, tok.keys.mac_c2s, tok.keys.mac_s2c);
        return;
    }
}

// mp:R6 -- MH_Key_Random in udp_room.h's rand_fn shape. The room is drawn from the same CSPRNG the
// endpoint's handshake nonces come from, and a failure is a failure: the caller refuses to host,
// as the endpoint refuses to host without secure randomness.
int key_random(void * /*ctx*/, uint8_t *out, unsigned n) { return MH_Key_Random(out, n); }

void send_hello() {
    uint8_t p[2 + UUID7_BYTES];
    p[0] = (g.cfg.role == 0) ? ROLE_HOST : ROLE_CLIENT;
    // mp:R1c -- the reserved flags byte, finally used: "this peer can re-key its leg from the
    // connect token". The relay ratchets a handle off the deployment key only when it sees this,
    // so a peer that could not follow the ratchet is never subjected to it.
    // mp:R4a -- and, above that bit, the protocol level this build was made against. An older
    // relay reads bit 0 and carries the rest through untouched; a current one answers with its own
    // level in the WELCOME's fifth byte.
    p[1] = (uint8_t)(HELLO_FLAG_PEER_KEY | (PROTOCOL_LEVEL << HELLO_LEVEL_SHIFT));
    EnterCriticalSection(&g.cs);
    memcpy(p + 2, g.match_id, UUID7_BYTES);
    g.match_dirty = false;
    LeaveCriticalSection(&g.cs);
    leg_send(OP_HELLO, HANDLE_NONE, p, (int)sizeof(p));
}

const char *err_name(uint8_t code) {
    switch (code) {
        case 1: return "no host has claimed this room -- the host must be on the relay first";
        case 2: return "another host already holds this room code";
        case 3: return "this peer is not registered (the relay restarted?)";
        case 4: return "the room is full";
        case 5: return "the peer this datagram was for is gone";
        case 6: return "only a room's own host may publish or withdraw its lobby";
        case 7: return "the lobby descriptor is too long for the relay to hold";
        default: return "unknown";
    }
}

// mp:R2 -- read one SESSIONS page and hand every row up. The relay stores descriptors as opaque
// bytes and so does this: what a row MEANS is mh.dll's business (it is a SESSION_INFO), and the
// sink is the one place that knows it.
void on_sessions_page(const uint8_t *p, int len) {
    if (g_dir == nullptr || len < SESSIONS_HDR) return;
    const int total = (int)get16(p);
    const int count = p[4];
    // AN EMPTY DIRECTORY IS AN ANSWER, not an absence of one, and it is the answer the vanish case
    // is made of: the host left, the relay has nothing, and the browser should stop showing the
    // row NOW rather than waiting out a timer. Signalled to the sink as a row with no bytes.
    if (total == 0) {
        g_dir(g_dir_ctx, DIRECTORY_ROOM, nullptr, 0);
        return;
    }
    int at = SESSIONS_HDR;
    for (int i = 0; i < count; ++i) {
        if (at + SESSIONS_ENTRY_HDR > len) return; // truncated: stop, never guess
        const uint32_t room = (uint32_t)p[at] | ((uint32_t)p[at + 1] << 8) |
                              ((uint32_t)p[at + 2] << 16) | ((uint32_t)p[at + 3] << 24);
        const int dlen = (int)get16(p + at + 4);
        at += SESSIONS_ENTRY_HDR;
        if (dlen < 0 || dlen > DESC_MAX || at + dlen > len) return;
        g_dir(g_dir_ctx, room, p + at, dlen);
        at += dlen;
    }
}

// ---- mp:R3, HOLE PUNCHING: the socket half ---------------------------------------------------------
//
// udp_punch.cpp decides; this decides nothing. What lives here is the four things that need a
// socket: turning a sockaddr into a candidate and back, gathering this machine's own addresses,
// putting a probe on the wire, and logging the two transitions.
//
// THE PUNCH SOCKET IS THE LEG SOCKET, and that is the single most load-bearing choice in the item.
// A NAT's mapping is created by an OUTBOUND datagram and is keyed on the SOURCE port; the address
// the relay observes and reports as our reflexive candidate is the mapping of `g.leg`. Probing from
// any other socket would be probing a mapping the peer was never told about -- it would work on a
// full-cone NAT and nowhere else, which is the subset of networks that did not need punching. So
// probes, and a promoted pair's DATA, leave the same socket the relay traffic does, and the mapping
// the peer aims at is the one it was given.

using mh::udppunch::Cand;

bool cand_from_sockaddr(const sockaddr *sa, Cand &c) {
    memset(&c, 0, sizeof(c));
    if (sa == nullptr) return false;
    if (sa->sa_family == AF_INET) {
        const sockaddr_in *a = (const sockaddr_in *)sa;
        c.fam                = mh::udppunch::FAM_V4;
        c.port               = ntohs(a->sin_port);
        memcpy(c.addr, &a->sin_addr, 4);
        return true;
    }
    if (sa->sa_family == AF_INET6) {
        const sockaddr_in6 *a = (const sockaddr_in6 *)sa;
        c.fam                 = mh::udppunch::FAM_V6;
        c.port                = ntohs(a->sin6_port);
        memcpy(c.addr, &a->sin6_addr, 16);
        return true;
    }
    return false;
}

// Returns the sockaddr length, or 0 for a candidate we cannot dial.
int cand_to_sockaddr(const Cand &c, sockaddr_storage &ss) {
    memset(&ss, 0, sizeof(ss));
    if (c.fam == mh::udppunch::FAM_V4) {
        sockaddr_in *a = (sockaddr_in *)&ss;
        a->sin_family  = AF_INET;
        a->sin_port    = htons(c.port);
        memcpy(&a->sin_addr, c.addr, 4);
        return (int)sizeof(sockaddr_in);
    }
    if (c.fam == mh::udppunch::FAM_V6) {
        sockaddr_in6 *a = (sockaddr_in6 *)&ss;
        a->sin6_family  = AF_INET6;
        a->sin6_port    = htons(c.port);
        memcpy(&a->sin6_addr, c.addr, 16);
        return (int)sizeof(sockaddr_in6);
    }
    return 0;
}

SOCKET sock_for(const Cand &c) {
    return c.fam == mh::udppunch::FAM_V6 ? g.leg6 : g.leg;
}

// `a.b.c.d:p` or `[hhhh:...]:p`. Hand-rolled rather than WSAAddressToString because this runs on
// the pump thread for a LOG line: the last thing a path transition should be able to do is fail in
// an address formatter.
void cand_text(const Cand &c, char *out, int cap) {
    static const char *H = "0123456789abcdef";
    if (cap < 64) {
        if (cap > 0) out[0] = '\0';
        return;
    }
    if (c.fam == mh::udppunch::FAM_V4) {
        wsprintfA(out, "%u.%u.%u.%u:%u", (unsigned)c.addr[0], (unsigned)c.addr[1],
                  (unsigned)c.addr[2], (unsigned)c.addr[3], (unsigned)c.port);
        return;
    }
    int at    = 0;
    out[at++] = '[';
    for (int i = 0; i < 8; ++i) {
        if (i) out[at++] = ':';
        const unsigned v = ((unsigned)c.addr[i * 2] << 8) | c.addr[i * 2 + 1];
        out[at++]        = H[(v >> 12) & 0xf];
        out[at++]        = H[(v >> 8) & 0xf];
        out[at++]        = H[(v >> 4) & 0xf];
        out[at++]        = H[v & 0xf];
    }
    out[at++] = ']';
    out[at]   = '\0';
    wsprintfA(out + at, ":%u", (unsigned)c.port);
}

PunchPeer *punch_for(uint16_t handle, bool create) {
    if (handle == HANDLE_NONE) return nullptr;
    for (int i = 0; i < MAX_REMOTE; ++i)
        if (g.pp[i].used && g.pp[i].handle == handle) return &g.pp[i];
    if (!create) return nullptr;
    for (int i = 0; i < MAX_REMOTE; ++i) {
        if (g.pp[i].used) continue;
        g.pp[i].used   = true;
        g.pp[i].handle = handle;
        // The nonce seed. NOT a secret -- the datagram carrying it is already MAC'd under the leg
        // key -- but it must not repeat across peers of one process, or an echo meant for one pair
        // could credit another's path.
        mh::udppunch::reset(g.pp[i].pn, ((uint64_t)GetTickCount() << 16) ^ (uint64_t)handle ^
                                            ((uint64_t)(i + 1) << 48));
        mh::udppunch::set_forced(g.pp[i].pn, g.force_relay, GetTickCount());
        g.pp[i].last_cand = GetTickCount() - CAND_MS;
        g.pp[i].born      = GetTickCount();
        return &g.pp[i];
    }
    return nullptr;
}

// This machine's own addresses, as candidates. Re-gathered on a slow timer because interfaces come
// and go (a VPN dialled mid-match, Wi-Fi replacing Ethernet) and a candidate list is only useful
// while it is true.
void gather_local_cands(DWORD now) {
    if (g.mine_n > 0 && (DWORD)(now - g.mine_at) < 30000) return;
    g.mine_at = now;
    g.mine_n  = 0;

    uint8_t buf[4096];
    DWORD   got = 0;
    if (g.leg != INVALID_SOCKET && g.leg_port != 0 &&
        WSAIoctl(g.leg, SIO_ADDRESS_LIST_QUERY, nullptr, 0, buf, (DWORD)sizeof(buf), &got, nullptr,
                 nullptr) == 0) {
        const SOCKET_ADDRESS_LIST *l = (const SOCKET_ADDRESS_LIST *)buf;
        for (int i = 0; i < l->iAddressCount && g.mine_n < mh::udppunch::MAX_CANDS; ++i) {
            const sockaddr_in *a = (const sockaddr_in *)l->Address[i].lpSockaddr;
            if (a == nullptr || a->sin_family != AF_INET) continue;
            const unsigned long h = ntohl(a->sin_addr.s_addr);
            // LOOPBACK IS NEVER A CANDIDATE, and this is not pedantry: on the UI harness's own rig
            // both peers can be lanes on ONE box, where 127.0.0.1 would validate instantly and the
            // pair would "go direct" over a path that proves nothing about NAT traversal.
            if ((h >> 24) == 127 || h == 0) continue;
            Cand c;
            memset(&c, 0, sizeof(c));
            c.fam  = mh::udppunch::FAM_V4;
            c.port = g.leg_port;
            memcpy(c.addr, &a->sin_addr, 4);
            g.mine[g.mine_n++] = c;
        }
    }
    got = 0;
    if (g.leg6 != INVALID_SOCKET && g.leg6_port != 0 &&
        WSAIoctl(g.leg6, SIO_ADDRESS_LIST_QUERY, nullptr, 0, buf, (DWORD)sizeof(buf), &got, nullptr,
                 nullptr) == 0) {
        const SOCKET_ADDRESS_LIST *l = (const SOCKET_ADDRESS_LIST *)buf;
        for (int i = 0; i < l->iAddressCount && g.mine_n < mh::udppunch::MAX_CANDS; ++i) {
            const sockaddr_in6 *a = (const sockaddr_in6 *)l->Address[i].lpSockaddr;
            if (a == nullptr || a->sin6_family != AF_INET6) continue;
            const uint8_t *b = (const uint8_t *)&a->sin6_addr;
            // LINK-LOCAL IS NOT A CANDIDATE EITHER, for a sharper reason than loopback: fe80::/10
            // needs a scope id to be dialled at all, and the scope id is the SENDER's interface
            // index -- a number that means nothing on the machine we would be handing it to.
            if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) continue;
            bool unspec = true;
            for (int k = 0; k < 16; ++k)
                if (b[k] != 0) {
                    unspec = false;
                    break;
                }
            if (unspec) continue;
            bool loop = (b[15] == 1);
            for (int k = 0; k < 15 && loop; ++k)
                if (b[k] != 0) loop = false;
            if (loop) continue;
            Cand c;
            memset(&c, 0, sizeof(c));
            c.fam  = mh::udppunch::FAM_V6;
            c.port = g.leg6_port;
            memcpy(c.addr, b, 16);
            g.mine[g.mine_n++] = c;
        }
    }
}

void punch_send_probe(PunchPeer &pp, const Cand &c, uint8_t op, uint64_t token) {
    sockaddr_storage ss;
    const int        len = cand_to_sockaddr(c, ss);
    if (len == 0) return;
    const SOCKET s = sock_for(c);
    if (s == INVALID_SOCKET) return;
    uint8_t p[8];
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)((token >> (8 * i)) & 0xff);
    leg_send_at(s, (const sockaddr *)&ss, len, false, op, pp.handle, p, (int)sizeof(p));
}

// The two transition lines mp:R3's acceptance clause is read out of. Both name the PEER, the PATH
// and a DURATION, because the three questions an operator asks of a promotion are "which peer",
// "over what" and "how long did it take" -- and a log that answers only the first is a log that
// cannot tell a fast promotion from a lucky one.
void punch_log_transition(const PunchPeer &pp, int ev, DWORD now) {
    char at[80];
    if (ev == mh::udppunch::EV_PROMOTED) {
        const Cand *t = mh::udppunch::direct_target(pp.pn);
        if (t == nullptr) return;
        cand_text(*t, at, (int)sizeof(at));
        logf("net: udp path DIRECT -- peer %u via %s after %u ms of punching (the relay stays up "
             "underneath; promotion %d)",
             (unsigned)pp.handle, at, (unsigned)(pp.pn.promoted_ms - pp.pn.started_ms),
             pp.pn.promotions);
        return;
    }
    logf("net: udp path RELAY -- peer %u demoted after %u ms direct (no keepalive for %u ms); "
         "traffic is back on the relay and punching has restarted (demotion %d)",
         (unsigned)pp.handle, (unsigned)(now - pp.pn.promoted_ms),
         (unsigned)mh::udppunch::DEAD_MS, pp.pn.demotions);
}

// Publish OUR candidate list to one counterpart, through the relay. Stops the moment the pair is
// direct: a promoted pair needs no rendezvous, and the relay's `candidates` events going quiet is
// the only signal it can have that a promotion happened (nobody tells it).
void punch_publish(PunchPeer &pp, DWORD now) {
    if (g.force_relay || mh::udppunch::direct_target(pp.pn) != nullptr) return;
    if ((DWORD)(now - pp.last_cand) < CAND_MS) return;
    pp.last_cand = now;
    gather_local_cands(now);
    uint8_t   payload[1 + mh::udppunch::MAX_CANDS * 22];
    const int n = mh::udppunch::cands_encode(g.mine, g.mine_n, payload, (int)sizeof(payload));
    if (n <= 0) return;
    // dst = the counterpart. A client's is ignored by the relay (a client has exactly one and does
    // not get to choose); a host's is the client it means.
    leg_send(OP_CAND, g.cfg.role == 0 ? pp.handle : HANDLE_NONE, payload, n);
}

// ---- the host side's per-remote loopback sockets ---------------------------------------------------
//
// One socket per REMOTE PEER, not one shared socket: see udp_relay.h reason 2. The socket is bound
// to an ephemeral loopback port, and the endpoint answers to that port, which is how a datagram
// coming back out of the endpoint tells us which remote it is for.
Remote *remote_for(uint16_t handle, bool create) {
    for (int i = 0; i < MAX_REMOTE; ++i)
        if (g.rem[i].used && g.rem[i].handle == handle) return &g.rem[i];
    if (!create) return nullptr;
    for (int i = 0; i < MAX_REMOTE; ++i) {
        if (g.rem[i].used) continue;
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) return nullptr;
        sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port        = 0;
        if (bind(s, (sockaddr *)&a, sizeof(a)) == SOCKET_ERROR) {
            closesocket(s);
            return nullptr;
        }
        // Same ICMP suppression the endpoint sets: a port-unreachable for a datagram we sent must
        // not fail the NEXT recvfrom on this socket.
        DWORD off = 0, got = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &got, nullptr, nullptr);
        g.rem[i].used    = true;
        g.rem[i].handle  = handle;
        g.rem[i].s       = s;
        g.rem[i].have_ep = false;
        // mp:R3e -- the address the endpoint will see this peer under, which is the key the
        // ownership question is asked in. A bound loopback socket has a name; if it somehow does
        // not, the zeroed one matches nothing and the slot is treated as never-known.
        memset(&g.rem[i].self, 0, sizeof(g.rem[i].self));
        int slen = (int)sizeof(g.rem[i].self);
        getsockname(s, (sockaddr *)&g.rem[i].self, &slen);
        g.rem[i].born = GetTickCount();
        // mp:L1f -- publish the address the endpoint will key this remote by, class still UNKNOWN:
        // the slot exists because a datagram is about to be delivered, but the delivery (and so the
        // latch) happens at the bottom of on_leg_datagram, not here.
        pc_open(i, ntohs(g.rem[i].self.sin_port));
        return &g.rem[i];
    }
    logf("net: udp relay has no loopback socket left for peer %u -- dropping its traffic",
         (unsigned)handle);
    return nullptr;
}

// mp:R3e -- drop everything this peer holds about counterpart `handle`: (host side) its loopback
// socket, its punch state and its pair keys. ALL THREE IN ONE STEP, on the pump thread, which is
// the whole of the ordering rule: the tables are pump-thread-only, a datagram is handled to
// completion before the next is read, so nothing can observe a handle with a socket but no keys or
// keys but no punch. What a straggler for a freed handle then meets is decided by its path -- a
// DIRECT one (a promoted pair's DATA, a probe) is checked under the pair key we no longer hold,
// falls to the deployment key, fails the MAC and is dropped; a RELAYED one verifies (the relay's
// key) and simply makes a FRESH slot, zeroed, which the next sweep frees again if the endpoint
// refuses it. A new peer that lands on the same handle number starts from that same zero.
//
// `my_key` -- the relay-directed send key, a COPY of the first pair key installed -- is left alone
// when the pair that minted it goes: the relay keeps `keys[0]` for the life of our handle, and
// changing what we tag relay traffic with would need an agreement neither side has a message for.
void free_peer(uint16_t handle) {
    if (handle == HANDLE_NONE) return;
    for (int i = 0; i < MAX_REMOTE; ++i) {
        if (g.rem[i].used && g.rem[i].handle == handle) {
            closesocket(g.rem[i].s);
            memset(&g.rem[i], 0, sizeof(g.rem[i]));
            g.rem[i].s = INVALID_SOCKET;
            pc_close(i); // mp:L1f -- the published row goes with the socket that named it
        }
        if (g.pp[i].used && g.pp[i].handle == handle) memset(&g.pp[i], 0, sizeof(g.pp[i]));
        if (g.kk[i].used && g.kk[i].handle == handle) memset(&g.kk[i], 0, sizeof(g.kk[i]));
    }
}

// mp:R4b -- the re-home: the relay came back and the counterpart's handle is known to have changed,
// so a stale entry must not shadow the fresh one the next datagram creates. The same release as
// R3e's teardown; what differs is the reason, which the caller logs.
void forget_peer(uint16_t handle) { free_peer(handle); }

// mp:R3e -- THE TEARDOWN SWEEP (udp_relay.h, "who says a peer is gone"). Host side: a Remote the
// endpoint no longer holds, past its grace, is released with everything keyed by its handle. Then
// the orphans: a punch or a key set whose handle has no Remote behind it (a punch is created by a
// CAND or a PROBE as well as by DATA, so a peer that only ever probed -- or a PSK holder probing
// under handles it made up -- holds punch slots with no socket to anchor them). Client side the
// anchor is `other_handle`: the one counterpart a client has, with the same orphan rule for the
// rest. Nothing here is on the path of a datagram; a slot freed a second late costs nothing.
//
// The predicate is asked ONLY for a slot past its grace, so a tunnel with no predicate installed
// (net_selftest, a transport built without the wiring) never frees a Remote on a guess: it keeps
// R1's hold-until-stop, which leaks exactly as visibly as it always did.
void sweep_peers(DWORD now) {
    if ((DWORD)(now - g.last_sweep) < SWEEP_MS) return;
    g.last_sweep         = now;
    const uint16_t other = (uint16_t)InterlockedCompareExchange(&g.other_handle, 0, 0);
    if (g.cfg.role == 0) {
        if (g_known != nullptr) {
            for (int i = 0; i < MAX_REMOTE; ++i) {
                if (!g.rem[i].used || (DWORD)(now - g.rem[i].born) < GRACE_MS) continue;
                if (g_known(g_known_ctx, g.rem[i].self)) continue;
                const uint16_t h = g.rem[i].handle;
                free_peer(h);
                logf("net: udp relay -- peer %u released: the endpoint no longer holds it, so its "
                     "loopback socket, punch state and pair keys are freed (mp:R3e)",
                     (unsigned)h);
            }
        }
        for (int i = 0; i < MAX_REMOTE; ++i) {
            if (!g.pp[i].used || (DWORD)(now - g.pp[i].born) < GRACE_MS) continue;
            if (remote_for(g.pp[i].handle, false) != nullptr) continue;
            const uint16_t h = g.pp[i].handle;
            free_peer(h);
            logf("net: udp relay -- peer %u released: a punch with no peer behind it (mp:R3e)",
                 (unsigned)h);
        }
        for (int i = 0; i < MAX_REMOTE; ++i)
            if (g.kk[i].used && remote_for(g.kk[i].handle, false) == nullptr &&
                punch_for(g.kk[i].handle, false) == nullptr)
                memset(&g.kk[i], 0, sizeof(g.kk[i]));
        return;
    }
    // Client. `other` is 0 before the WELCOME and after a lost leg's re-HELLO; nothing is judged
    // against an anchor that is not there.
    if (other == HANDLE_NONE) return;
    for (int i = 0; i < MAX_REMOTE; ++i) {
        if (!g.pp[i].used || g.pp[i].handle == other || (DWORD)(now - g.pp[i].born) < GRACE_MS)
            continue;
        const uint16_t h = g.pp[i].handle;
        free_peer(h);
        logf("net: udp relay -- peer %u released: not this client's host (mp:R3e)", (unsigned)h);
    }
}

// ---- the two data directions -----------------------------------------------------------------------

// `from_relay` says which of the two paths this arrived on. It is NOT a security boundary -- the
// MAC is -- but it decides meaning: a PROBE is peer-to-peer by definition and a WELCOME is the
// relay's by definition, so an op arriving on the wrong path is a datagram we have no reading for
// and drop, rather than one we try to interpret in a context it was not sent in.
void on_leg_datagram(const uint8_t *pkt, int n, const sockaddr *from, bool from_relay) {
    uint8_t   op  = 0;
    uint16_t  src = 0, dst = 0;
    uint32_t  room = 0;
    const int plen = leg_decode(pkt, n, from_relay, &op, &src, &dst, &room);
    if (plen < 0) return; // not ours, or forged: the relay counts its side, we just ignore ours
    const uint8_t *payload = pkt + LEG_HDR;

    // ---- mp:R3: the three punch ops ------------------------------------------------------------
    if (op == OP_PROBE || op == OP_PROBE_ACK) {
        // A probe that came through the RELAY is a relay that forwarded something it should have
        // counted and dropped (its own `probes_misdirected`). Refusing it here as well is the half
        // of that rule this side owns: a probe carried by the relay would validate the relayed path
        // and "promote" a pair onto the route it was already using.
        if (from_relay || plen != 8) return;
        Cand c;
        if (!cand_from_sockaddr(from, c)) return;
        PunchPeer *pp = punch_for(src, true);
        if (pp == nullptr) return;
        uint64_t token = 0;
        for (int i = 7; i >= 0; --i) token = (token << 8) | payload[i];
        const DWORD now = GetTickCount();
        if (op == OP_PROBE) {
            mh::udppunch::on_peer_probe(pp->pn, c, now);
            // ECHO IT BACK UNCONDITIONALLY, including under force_relay and including to an address
            // we would never promote. Answering is what lets the OTHER end learn the path; refusing
            // to answer because we do not want the path ourselves would make one peer's `force_relay`
            // look to the other like a network that drops probes.
            punch_send_probe(*pp, c, OP_PROBE_ACK, token);
            return;
        }
        mh::udppunch::on_ack(pp->pn, token, now);
        return;
    }
    if (op == OP_CAND) {
        if (!from_relay) return; // the rendezvous is the relay's job and only the relay's
        Cand      list[mh::udppunch::MAX_CANDS];
        const int got = mh::udppunch::cands_decode(payload, plen, list, mh::udppunch::MAX_CANDS);
        if (got < 0) return;
        PunchPeer *pp = punch_for(src, true);
        if (pp == nullptr) return;
        const int added = mh::udppunch::add_cands(pp->pn, list, got, GetTickCount());
        if (added > 0)
            logf("net: udp punch -- peer %u offered %d candidate(s), %d new; probing them all "
                 "(the match keeps running relayed meanwhile)",
                 (unsigned)src, got, added);
        return;
    }

    // Everything below is either the relay speaking (WELCOME, PING, SESSIONS, ERROR) or DATA, which
    // is the one op that legitimately arrives on EITHER path -- that is what a promotion is.
    if (!from_relay && op != OP_DATA) return;

    switch (op) {
        case OP_WELCOME: {
            // mp:R4a -- 4 bytes from a pre-R4a relay (level 0), 5 from a current one; anything a
            // future relay appends past the level is carried through unread rather than refused,
            // so the next field is not a flag day. (A pre-R4a peer refused every length but 4,
            // which is why the relay sends 5 only to a peer whose HELLO carried a level.)
            if (plen < 4) return;
            const uint16_t me    = get16(payload);
            const uint16_t other = get16(payload + 2);
            const uint8_t  level = plen >= 5 ? payload[4] : 0;
            const LONG     was   = InterlockedExchange(&g.self_handle, (LONG)me);
            const LONG     was_o = InterlockedExchange(&g.other_handle, (LONG)other);
            if (was == 0) {
                logf("net: udp relay leg UP -- %s:%u room=%u, handle %u%s",
                     g.cfg.host, (unsigned)g.cfg.port, (unsigned)g.cfg.room, (unsigned)me,
                     other ? " (host handle known)" : "");
            } else if (InterlockedExchange(&g.leg_lost, 0) != 0) {
                // mp:R4b -- the re-HELLO after a NOT_REGISTERED landed. Same handle back: every
                // table the counterpart keys by it is still right, and the only thing that changed
                // is that the relay leg is on the deployment key again (a fresh relay learns a
                // token only from a handshake datagram, which a live match will not send again).
                // A different handle means something took ours in the gap; the counterpart will
                // key us afresh from our first datagram, and what WE hold about it stays valid
                // unless its own handle moved too (the client learns that from `other`).
                const DWORD gap = GetTickCount() - g.lost_at;
                if ((LONG)me == was) {
                    logf("net: udp relay leg RESTORED after %u ms -- handle %u kept, pair state "
                         "kept; the leg runs on the deployment key from here (mp:R4b)",
                         (unsigned)gap, (unsigned)me);
                } else {
                    logf("net: udp relay leg RESTORED after %u ms -- but handle %u is taken, we "
                         "are %u now; the counterpart re-learns us from our next datagram (mp:R4b)",
                         (unsigned)gap, (unsigned)was, (unsigned)me);
                }
                if (g.cfg.role == 1 && other != HANDLE_NONE && (LONG)other != was_o) {
                    logf("net: udp relay -- the host came back as handle %u (was %u); its pair "
                         "keys and punch state are rebuilt from scratch (mp:R4b)",
                         (unsigned)other, (unsigned)was_o);
                    forget_peer((uint16_t)was_o);
                }
            }
            // Answer at once: this is what validates our address and lifts the relay's 3x
            // anti-amplification cap. See the header note.
            leg_send(OP_PING, HANDLE_NONE, nullptr, 0);
            g.last_keep = GetTickCount();
            on_relay_level(level); // mp:R4a -- after the leg is up, so the line follows `leg UP`
            return;
        }
        case OP_PING: leg_send(OP_PONG, HANDLE_NONE, nullptr, 0); return;
        case OP_PONG: return;
        case OP_SESSIONS: on_sessions_page(payload, plen); return;
        case OP_ERROR: {
            if (plen < 1) return;
            const uint8_t  code = payload[0];
            static uint8_t last = 0xff;
            if (code != last) { // once per distinct code: a retry loop must not flood the log
                last = code;
                logf("net: udp relay refused us -- %s", err_name(code));
            }
            // WHICH REFUSALS MEAN "START OVER". Only NOT_REGISTERED does -- it is the relay saying
            // it has never heard of this handle (it restarted, or evicted us), and re-HELLOing is
            // the repair. Every other code is a refusal of one ACTION: R1 reset the handle on all
            // of them, which was harmless while the only refusals arrived at registration time and
            // is not any more -- a client parked in the directory room gets a `no_host` for every
            // game datagram the endpoint still emits, and resetting on those would churn a new
            // handle each second and never settle (mp:R2).
            if (code == ERR_NOT_REGISTERED) {
                // mp:R4b -- THE RELAY RESTARTED (or evicted us), and this is the one message it can
                // still send: under the deployment key, which is why the decoder kept that key as
                // a candidate for OP_ERROR. What R1 did here -- zero the handle and re-HELLO -- was
                // right at R1 and wrong since R1c, in two ways that each black-holed the match
                // (dead-ends G245): the re-HELLO went out tagged with the SESSION key the relay
                // had just failed to verify (`my_key_proved` was left standing), and a fresh handle
                // would have orphaned everything the counterpart keys by the old one. So: keep the
                // handle, drop the ratchet, and let the pump re-HELLO naming the handle we had.
                const LONG had = InterlockedCompareExchange(&g.self_handle, 0, 0);
                if (had != 0 && InterlockedCompareExchange(&g.leg_lost, 1, 0) == 0) {
                    g.my_key_proved = false;
                    g.lost_at       = GetTickCount();
                    g.last_hello    = g.lost_at - HELLO_RETRY_MS; // re-HELLO on the next pump
                    logf("net: udp relay leg LOST -- the relay does not know handle %u (it "
                         "restarted, or evicted us); re-HELLOing under the deployment key and "
                         "asking for the same handle (mp:R4b)",
                         (unsigned)had);
                }
                return;
            }
            // mp:R6. A HOST refused `room_busy` at REGISTRATION time drew a code another host on
            // this relay already holds -- a 2^-30 event per other host, or a stale slot of a
            // process that crashed and was relaunched inside the relay's idle window with, by
            // chance, the same draw. Either way the answer is a fresh room, not waiting for the
            // other slot to age out: re-mint, re-HELLO at once, and say which room this host is
            // now asking for. Bounded (udp_room.h BUSY_RETRIES); once spent, the R1 refusal above
            // stands and the HELLO keeps retrying the last room at HELLO_RETRY_MS, so a relay that
            // answers room_busy to everything produces four re-mint lines and then the refusal --
            // never a host that mints forever and never reports being refused. Gated on having no
            // handle for the same reason the client branch below is: a room_busy with a handle in
            // hand is a refusal of one re-home, not of this registration.
            if (g.cfg.role == 0 && code == ERR_ROOM_BUSY &&
                InterlockedCompareExchange(&g.self_handle, 0, 0) == 0) {
                const uint32_t busy_room = g.cfg.room;
                if (mh::udproom::minter_on_busy(g.minter)) {
                    g.cfg.room   = g.minter.room;
                    g.last_hello = GetTickCount() - HELLO_RETRY_MS; // re-HELLO on the next pump
                    logf("net: udp relay -- host room %u (re-minted: room_busy on %u, retry %d/%d; "
                         "mp:R6)",
                         (unsigned)g.cfg.room, (unsigned)busy_room, g.minter.busy,
                         mh::udproom::BUSY_RETRIES);
                } else {
                    logf("net: udp relay -- host room %u is busy and %d re-mints are spent; the "
                         "refusal stands (mp:R6 -- a relay that refuses every fresh code is not "
                         "a collision)",
                         (unsigned)busy_room, mh::udproom::BUSY_RETRIES);
                }
                return;
            }
            // mp:R2 / mp:R1e. A CLIENT refused its room at REGISTRATION time has been handed a
            // lobby code that is not hosted -- which is the ordinary state of a browser, because
            // its first dial can only guess a room (the pre-directory default) and since mp:R6 a
            // host's room is minted, so the guess never lands. Falling back to the directory room
            // keeps it registered, which is what buys it the LIST that tells it which codes DO
            // exist. A refusal with a handle already in hand is not this case and is left alone.
            if (g.cfg.role == 1 && g.cfg.room != DIRECTORY_ROOM &&
                (code == ERR_NO_HOST || code == ERR_ROOM_BUSY) &&
                InterlockedCompareExchange(&g.self_handle, 0, 0) == 0) {
                logf("net: udp relay room %u is not hosted -- browsing the relay's session "
                     "directory instead (join a listed game to dial its room)",
                     (unsigned)g.cfg.room);
                g.cfg.room   = DIRECTORY_ROOM;
                g.last_hello = GetTickCount() - HELLO_RETRY_MS;
            }
            return;
        }
        case OP_DATA: break;
        default: return;
    }

    // mp:R3. NOTHING IS DONE HERE WITH A DIRECT DATA, and the empty space is deliberate. Feeding it
    // to the punch state as a liveness signal is what this file did for one afternoon, and it is
    // wrong: game traffic arriving proves the peer can reach US, while the thing a direct path has
    // to keep proving is that we can reach IT. udp_punch.h's demotion note carries the measurement.

    if (g.cfg.role == 1) {
        if (!g.have_ep) return; // the endpoint has not spoken yet; nothing to answer
        // mp:L1f -- a CLIENT has exactly one counterpart, so one published row. Latched on the
        // delivery rather than on the decode, so what the cell reports is the path of a frame the
        // endpoint actually received.
        pc_latch(PC_CLIENT, from_relay);
        sendto(g.local, (const char *)payload, plen, 0, (const sockaddr *)&g.ep_addr,
               sizeof(g.ep_addr));
        // mp:R1c. AFTER the delivery: this is the datagram the client derives its key from, so it
        // had to arrive under the old one. `src` is the host's handle on a relayed datagram and the
        // peer's own on a promoted one -- either way it is the counterpart this token pairs us with.
        sniff_grant(payload, plen, src);
        return;
    }
    // Host: deliver from the loopback socket that belongs to this remote peer, so the endpoint
    // sees one distinct source address per client.
    //
    // mp:R3 -- and this is also where a host LEARNS a client exists at all. The relay's WELCOME
    // tells a client its host's handle; nothing tells a host its clients' handles, because the room
    // can gain one at any time. The first datagram carrying a handle is the announcement, and a
    // punch for that pair starts from it.
    punch_for(src, true);
    Remote *r = remote_for(src, true);
    if (!r) return;
    // mp:L1f -- this remote's row is `r`'s own index in g.rem[], which is also its index in the
    // published table (pc_open above claims them together). Pointer arithmetic rather than a second
    // search: remote_for just did the search, and a second one could disagree with the first.
    pc_latch((int)(r - g.rem), from_relay);
    sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family      = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port        = htons(g.cfg.game_port);
    sendto(r->s, (const char *)payload, plen, 0, (const sockaddr *)&to, sizeof(to));
}

// mp:R3 -- where a DATA for `handle` should go. Null while the pair is relayed, which is the state
// every pair starts in and the one it returns to whenever punching fails or a direct path dies.
const Cand *direct_for(uint16_t handle) {
    PunchPeer *pp = punch_for(handle, false);
    return pp ? mh::udppunch::direct_target(pp->pn) : nullptr;
}

// One T0 datagram out, by whichever path this pair is currently on. THE ONLY PLACE the choice is
// made, so "relayed" and "direct" cannot drift apart between the two directions of one pair.
void send_game_data(uint16_t handle, const uint8_t *buf, int n) {
    const Cand *t = direct_for(handle);
    if (t == nullptr) {
        // mp:R4b -- a relay that has just told us NOT_REGISTERED would answer every one of these
        // with another NOT_REGISTERED (30 a second, each a counted `unregistered` at its end);
        // the endpoint's own retransmit covers the sub-second gap until the WELCOME. A promoted
        // pair below is untouched: its path does not go through the relay at all.
        if (InterlockedCompareExchange(&g.leg_lost, 0, 0) != 0) return;
        leg_send(OP_DATA, handle, buf, n);
        return;
    }
    sockaddr_storage ss;
    const int        len = cand_to_sockaddr(*t, ss);
    if (len == 0) {
        leg_send(OP_DATA, handle, buf, n);
        return;
    }
    leg_send_at(sock_for(*t), (const sockaddr *)&ss, len, false, OP_DATA, handle, buf, n);
}

void pump() {
    uint8_t          buf[LEG_MAX + 64];
    sockaddr_storage from_ss;
    sockaddr_in      from;
    int              flen;

    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(g.leg, &rd);
    if (g.leg6 != INVALID_SOCKET) FD_SET(g.leg6, &rd);
    if (g.cfg.role == 1 && g.local != INVALID_SOCKET) FD_SET(g.local, &rd);
    for (int i = 0; i < MAX_REMOTE; ++i)
        if (g.rem[i].used) FD_SET(g.rem[i].s, &rd);

    timeval tv;
    tv.tv_sec       = 0;
    tv.tv_usec      = SELECT_MS * 1000;
    const int ready = select(0, &rd, nullptr, nullptr, &tv);
    if (ready == SOCKET_ERROR) return;

    if (FD_ISSET(g.leg, &rd)) {
        flen        = (int)sizeof(from_ss);
        const int n = recvfrom(g.leg, (char *)buf, (int)sizeof(buf), 0, (sockaddr *)&from_ss, &flen);
        if (n > 0) {
            // BEFORE mp:R3 this socket listened to the relay's address and nothing else. It cannot
            // any more -- a direct datagram arrives from the PEER -- so the address check becomes a
            // classification (`from_relay`) rather than a filter, and what refuses a stray datagram
            // is what always actually refused it: the MAC. The address test was never the boundary;
            // it was an optimisation that also happened to read like one.
            const sockaddr_in *v4       = (const sockaddr_in *)&from_ss;
            const bool         is_relay = from_ss.ss_family == AF_INET &&
                                  v4->sin_addr.s_addr == g.relay.sin_addr.s_addr &&
                                  v4->sin_port == g.relay.sin_port;
            on_leg_datagram(buf, n, (const sockaddr *)&from_ss, is_relay);
        }
    }
    // The v6 punch socket carries no relay traffic at all: `[net] relay` resolves to a v4 address,
    // so everything arriving here is peer-to-peer by construction.
    if (g.leg6 != INVALID_SOCKET && FD_ISSET(g.leg6, &rd)) {
        flen        = (int)sizeof(from_ss);
        const int n = recvfrom(g.leg6, (char *)buf, (int)sizeof(buf), 0, (sockaddr *)&from_ss, &flen);
        if (n > 0) on_leg_datagram(buf, n, (const sockaddr *)&from_ss, false);
    }

    const uint16_t self  = (uint16_t)InterlockedCompareExchange(&g.self_handle, 0, 0);
    const uint16_t other = (uint16_t)InterlockedCompareExchange(&g.other_handle, 0, 0);

    if (g.cfg.role == 1 && g.local != INVALID_SOCKET && FD_ISSET(g.local, &rd)) {
        flen        = sizeof(from);
        const int n = recvfrom(g.local, (char *)buf, (int)sizeof(buf), 0, (sockaddr *)&from, &flen);
        if (n > 0) {
            g.ep_addr = from;
            g.have_ep = true;
            // Before the WELCOME there is nowhere to send it. Dropping is right: the endpoint's own
            // handshake retransmits (HS_RETRY_MS), so the join costs one retry, not a failure.
            // The directory room has no host by construction (mp:R2), so a datagram sent from
            // there would buy one `no_host` per handshake retry and nothing else.
            if (self != HANDLE_NONE && g.cfg.room != DIRECTORY_ROOM)
                send_game_data(other, buf, n);
        }
    }
    for (int i = 0; i < MAX_REMOTE; ++i) {
        if (!g.rem[i].used || !FD_ISSET(g.rem[i].s, &rd)) continue;
        flen        = sizeof(from);
        const int n = recvfrom(g.rem[i].s, (char *)buf, (int)sizeof(buf), 0, (sockaddr *)&from, &flen);
        if (n <= 0) continue;
        g.rem[i].from_ep = from;
        g.rem[i].have_ep = true;
        if (self != HANDLE_NONE) send_game_data(g.rem[i].handle, buf, n);
        // mp:R1c. AFTER the send, and the host is the end that MINTS the token: this is handshake
        // datagram 2 on its way out, so the host keys the pair from a grant it wrote itself, the
        // relay from the copy it forwards, and the client from the copy it receives -- one event,
        // three parties, nothing added to the wire.
        if (self != HANDLE_NONE) sniff_grant(buf, n, g.rem[i].handle);
    }

    const DWORD now = GetTickCount();
    sweep_peers(now); // mp:R3e -- after the per-remote reads, before anything sends to a remote
    bool dirty;
    EnterCriticalSection(&g.cs);
    dirty = g.match_dirty;
    LeaveCriticalSection(&g.cs);
    if (self == HANDLE_NONE || InterlockedCompareExchange(&g.leg_lost, 0, 0) != 0) {
        // The second half is mp:R4b: the handle is kept and named in `src`, so a restarted relay
        // hands it back; the retry cadence is R1's.
        if (now - g.last_hello >= HELLO_RETRY_MS) {
            g.last_hello = now;
            send_hello();
        }
    } else if (dirty) {
        // A HELLO naming our existing handle is an UPDATE, not a second registration: it is how the
        // relay learns the match_id mh.dll minted after the leg was already up.
        g.last_hello = now;
        send_hello();
    } else if (now - g.last_keep >= KEEP_MS) {
        g.last_keep = now;
        leg_send(OP_PING, HANDLE_NONE, nullptr, 0);
    }

    if (self == HANDLE_NONE) return;

    // ---- mp:R3, hole punching ---------------------------------------------------------------------
    //
    // THE BACKGROUND JOB, and the word is exact: by the time this runs the pair is already playing
    // over the relay. Nothing here is on the path of starting a match, nothing here blocks, and if
    // every probe is lost forever the only consequence is that the game stays relayed.
    //
    // A CLIENT has exactly one counterpart and learns it from the WELCOME. A HOST learns each of its
    // clients from the first datagram carrying their handle, which is where punch_for() creates
    // them -- so a host with three clients runs three independent punches and may end up direct with
    // one and relayed with the other two, which is the correct outcome and not a partial failure.
    if (g.cfg.role == 1 && other != HANDLE_NONE && g.cfg.room != DIRECTORY_ROOM)
        punch_for(other, true);
    for (int i = 0; i < MAX_REMOTE; ++i) {
        if (!g.pp[i].used) continue;
        punch_publish(g.pp[i], now);
        int       idx[mh::udppunch::MAX_CANDS];
        uint64_t  tok[mh::udppunch::MAX_CANDS];
        const int due =
            mh::udppunch::due_probes(g.pp[i].pn, now, idx, tok, mh::udppunch::MAX_CANDS);
        for (int k = 0; k < due; ++k)
            punch_send_probe(g.pp[i], g.pp[i].pn.path[idx[k]].cand, OP_PROBE, tok[k]);
        const int ev = mh::udppunch::tick(g.pp[i].pn, now);
        if (ev != mh::udppunch::EV_NONE) punch_log_transition(g.pp[i], ev, now);
    }

    // ---- mp:R2, the session directory ------------------------------------------------------------
    if (g.cfg.role == 0) {
        // HOST: publish the lobby while mh.dll keeps advertising it, withdraw it when that stops.
        uint8_t desc[DESC_MAX];
        int     dlen = 0;
        bool    stale;
        EnterCriticalSection(&g.cs);
        dlen  = g.si_len;
        stale = (dlen <= 0) || (now - g.si_at >= SI_STALE_MS);
        if (dlen > 0) memcpy(desc, g.si, (size_t)dlen);
        LeaveCriticalSection(&g.cs);
        if (stale) {
            if (g.si_registered) {
                g.si_registered = false;
                leg_send(OP_UNREGISTER, HANDLE_NONE, nullptr, 0);
                logf("net: udp relay -- lobby withdrawn from the relay's directory (this host is "
                     "no longer advertising a game)");
            }
        } else if (now - g.last_register >= REGISTER_MS) {
            g.last_register = now;
            leg_send(OP_REGISTER, HANDLE_NONE, desc, dlen);
            if (!g.si_registered) {
                g.si_registered = true;
                logf("net: udp relay -- lobby published to the relay's directory, room=%u (%d B)",
                     (unsigned)g.cfg.room, dlen);
            }
        }
        return;
    }
    // CLIENT: ask for the directory while anything is listening for it. A host never asks (it is
    // the thing being listed), and a client whose mh.dll registered no sink asks for nothing --
    // which is what keeps a force-entry/determinism run's wire byte-for-byte what R1 measured.
    //
    // mp:R2a -- AND ONLY WHILE THE PLAYER IS LOOKING AT THE BROWSER. R2 shipped an unconditional
    // 2 s poll, and R2's own rig run measured what that costs: 321 LISTs over an 11-minute match,
    // every one of them answered, none of them read -- because mh.dll's directory half is already
    // gated on the browser being the active screen (the seam that stops a host's match-start
    // withdrawal from tearing the client out of its own game). The answers were being thrown away
    // at the top of the stack while the question kept being asked at the bottom, and the relay's
    // `list_requests` counter therefore described browsing activity that was not happening.
    // Asking the same predicate here is what makes the counter true again.
    const bool browsing = mh_browsing();
    if (browsing != g.listing) {
        g.listing = browsing;
        // Re-arm so the browser's FIRST frame gets an answer rather than waiting out LIST_MS: the
        // one moment a directory poll is worth anything is the moment the screen opens.
        if (browsing) g.last_list = now - LIST_MS;
        logf("net: udp relay directory polling %s (mp:R2a -- the session browser is %s)",
             browsing ? "RESUMED" : "PAUSED", browsing ? "up" : "not the active screen");
    }
    if (g_dir != nullptr && browsing && now - g.last_list >= LIST_MS) {
        g.last_list = now;
        leg_send(OP_LIST, HANDLE_NONE, nullptr, 0);
    }
}

DWORD WINAPI thread_main(LPVOID) {
    while (InterlockedCompareExchange(&g.running, 1, 1)) pump();
    return 0;
}

bool ensure_wsa() {
    static bool done = false;
    if (done) return true;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    done = true;
    return true;
}

// `host` is a dotted IPv4 address or a name. inet_addr first (the tree's own convention, and the
// only form `[net] host` has ever taken), getaddrinfo after -- a relay is far more likely than a
// LAN host to be reached by name.
bool resolve(const char *host, unsigned short port, sockaddr_in &out) {
    memset(&out, 0, sizeof(out));
    out.sin_family        = AF_INET;
    out.sin_port          = htons(port);
    const unsigned long v = inet_addr(host);
    if (v != INADDR_NONE) {
        out.sin_addr.s_addr = v;
        return true;
    }
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *res     = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || res == nullptr) return false;
    out.sin_addr = ((sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

} // namespace

// =================================================================================================

bool start(const Config &cfg, const uint8_t psk[KEY_LEN], log_fn log, void *log_ctx) {
    if (g_started) return true;
    if (!ensure_wsa()) return false;

    memset(&g, 0, sizeof(g));
    pc_clear_all(); // mp:L1f -- a relink must not inherit the previous tunnel's published rows
    g.cfg         = cfg;
    g.log         = log;
    g.log_ctx     = log_ctx;
    g.leg         = INVALID_SOCKET;
    g.leg6        = INVALID_SOCKET;
    g.local       = INVALID_SOCKET;
    g.force_relay = (cfg.force_relay != 0);
    // mp:R2a. Start in the POLLING state so the first transition a log can carry is the PAUSE: a
    // run that never opens the browser (force entry, the determinism gate) then says so once,
    // and a build whose mh.dll has no browser predicate at all says nothing, which is right --
    // it is not pausing anything.
    g.listing = true;
    InitializeCriticalSection(&g.cs);

    // The DEPLOYMENT leg key: one PSK, a label of its own, so it can never be the same bytes as the
    // boot keys udp_endpoint.cpp derives from that same PSK. leg.rs derives it identically. Since
    // mp:R1c this is the BOOTSTRAP key -- what a leg uses until a session mints a key of its own.
    deployment_leg_key(psk, g.leg_key);

    // mp:R1c -- and the three boot secrets, so the tunnel can open the grant it carries. Derived
    // here, from the same labels udp_endpoint.cpp uses, rather than plumbed across from the
    // endpoint: the endpoint does not know this file exists and mp:R1's whole shape depends on
    // that staying true.
    memcpy(g.psk, psk, KEY_LEN);
    {
        uint8_t full[SHA256_LEN];
        hmac_sha256(psk, KEY_LEN, (const uint8_t *)LBL_BOOT_CONN, (size_t)lstrlenA(LBL_BOOT_CONN),
                    full);
        memcpy(g.boot_conn, full, sizeof(g.boot_conn));
        hmac_sha256(psk, KEY_LEN, (const uint8_t *)LBL_BOOT_ENC, (size_t)lstrlenA(LBL_BOOT_ENC),
                    full);
        memcpy(g.boot_enc, full, KEY_LEN);
        hmac_sha256(psk, KEY_LEN, (const uint8_t *)LBL_BOOT_MAC, (size_t)lstrlenA(LBL_BOOT_MAC),
                    full);
        memcpy(g.boot_mac, full, KEY_LEN);
    }

    if (!resolve(cfg.host, cfg.port, g.relay)) {
        logf("net: udp relay=%s:%u does not resolve -- REFUSING to start (a peer that thinks it is "
             "relayed and is not would sit in a lobby nobody can join)",
             cfg.host, (unsigned)cfg.port);
        DeleteCriticalSection(&g.cs);
        return false;
    }

    g.leg = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g.leg == INVALID_SOCKET) {
        logf("net: udp relay socket() failed %d", WSAGetLastError());
        DeleteCriticalSection(&g.cs);
        return false;
    }
    {
        DWORD off = 0, got = 0;
        WSAIoctl(g.leg, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &got, nullptr, nullptr);
    }
    sockaddr_in any;
    memset(&any, 0, sizeof(any));
    any.sin_family      = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    any.sin_port        = 0;
    if (bind(g.leg, (sockaddr *)&any, sizeof(any)) == SOCKET_ERROR) {
        logf("net: udp relay leg bind failed %d", WSAGetLastError());
        closesocket(g.leg);
        g.leg = INVALID_SOCKET;
        DeleteCriticalSection(&g.cs);
        return false;
    }
    {
        // mp:R3. The leg socket's own port is what a LOCAL v4 candidate advertises, and it is the
        // port whose NAT mapping the relay will report back as the reflexive one -- the two halves
        // of the candidate list are the two views of this one socket.
        sockaddr_in me;
        int         mlen = sizeof(me);
        if (getsockname(g.leg, (sockaddr *)&me, &mlen) == 0) g.leg_port = ntohs(me.sin_port);
    }

    // mp:R3, the IPv6 punch socket. Best effort by design: a machine with no IPv6 stack, or one
    // where the bind is refused, simply has no v6 candidates to offer and probes none. That is a
    // narrower punch, not a broken tunnel, so it is logged at most once and never refuses start().
    if (!g.force_relay) {
        g.leg6 = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        if (g.leg6 != INVALID_SOCKET) {
            sockaddr_in6 a6;
            memset(&a6, 0, sizeof(a6));
            a6.sin6_family = AF_INET6;
            a6.sin6_port   = 0;
            // v6-ONLY. A dual-stack socket would receive v4 traffic as ::ffff: mapped addresses,
            // which would make one arriving datagram classifiable two ways and the punch table
            // able to hold the same path twice under two families.
            DWORD on = 1, ignored = 0;
            setsockopt(g.leg6, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&on, sizeof(on));
            if (bind(g.leg6, (sockaddr *)&a6, sizeof(a6)) == SOCKET_ERROR) {
                closesocket(g.leg6);
                g.leg6 = INVALID_SOCKET;
            } else {
                WSAIoctl(g.leg6, SIO_UDP_CONNRESET, &ignored, sizeof(ignored), nullptr, 0, &ignored,
                         nullptr, nullptr);
                int mlen = sizeof(a6);
                if (getsockname(g.leg6, (sockaddr *)&a6, &mlen) == 0)
                    g.leg6_port = ntohs(a6.sin6_port);
            }
        }
    }

    if (cfg.role == 1) {
        g.local = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port        = 0;
        if (g.local == INVALID_SOCKET || bind(g.local, (sockaddr *)&a, sizeof(a)) == SOCKET_ERROR) {
            logf("net: udp relay loopback bind failed %d", WSAGetLastError());
            if (g.local != INVALID_SOCKET) closesocket(g.local);
            closesocket(g.leg);
            g.leg = g.local = INVALID_SOCKET;
            DeleteCriticalSection(&g.cs);
            return false;
        }
        DWORD off = 0, got = 0;
        WSAIoctl(g.local, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &got, nullptr, nullptr);
        int nlen = sizeof(a);
        getsockname(g.local, (sockaddr *)&a, &nlen);
        g.local_port = ntohs(a.sin_port);
        // mp:L1f -- the client's one row: the endpoint dials THIS port (udp_transport.cpp rewrites
        // MH_NetConfig.host/port to 127.0.0.1:client_dial_port), so it is the address its single
        // conn carries and the key path_class is asked in.
        pc_open(PC_CLIENT, g.local_port);
    }

    // mp:R6 -- the host's minter starts AT the room the caller minted (mint_host_room, via
    // udp_transport.cpp), with its retry budget untouched; a re-mint on room_busy moves cfg.room
    // from here. A client has no minter to speak of: its room is the directory's to name.
    g.minter.rnd  = key_random;
    g.minter.ctx  = nullptr;
    g.minter.room = cfg.room;
    g.minter.busy = 0;

    InterlockedExchange(&g.running, 1);
    g_started    = true;
    g.last_hello = GetTickCount() - HELLO_RETRY_MS; // the first pump() sends HELLO at once
    g.last_keep  = GetTickCount();
    g.thread     = CreateThread(nullptr, 0, thread_main, nullptr, 0, nullptr);
    if (g.thread == nullptr) {
        stop();
        return false;
    }
    logf("net: udp RELAY mode -- %s:%u room=%u as %s%s", cfg.host, (unsigned)cfg.port,
         (unsigned)cfg.room, cfg.role == 0 ? "host" : "client",
         cfg.role == 1 ? " (the transport dials loopback; the relay carries it)" : "");
    // mp:R6 -- THE LINE A POST-CHECK READS: the room this host is registering under, minted for
    // this tunnel start and nothing a player configured. Written here, once per host start, so the
    // same needle also matches the re-mint line the OP_ERROR path writes.
    if (cfg.role == 0)
        logf("net: udp relay -- host room %u (minted for this lobby; mp:R6)", (unsigned)cfg.room);
    // mp:R3. Stated at start, once, for both settings -- a run whose log says nothing about
    // punching cannot be told apart from a build that has none.
    if (g.force_relay)
        logf("net: udp path RELAY (forced) -- [net] force_relay=1, so no candidates are published "
             "and no pair will be promoted to a direct path");
    else
        logf("net: udp punch ARMED -- v4 probe port %u, v6 probe port %u (0 = no IPv6 on this "
             "machine); the match runs relayed until a pair validates a direct path",
             (unsigned)g.leg_port, (unsigned)g.leg6_port);
    return true;
}

void stop() {
    if (!g_started && g.leg == INVALID_SOCKET) return;
    // THE THREAD GOES FIRST, AND THE BYE AFTER IT. `tx_seq` has exactly one writer -- the pump
    // thread -- and sending the goodbye from the game thread while that one is still running would
    // race it into a duplicate sequence, which the relay's replay window then drops. Joining first
    // costs one select() period and makes the last datagram of a session as ordinary as the rest.
    InterlockedExchange(&g.running, 0);
    if (g.thread) {
        WaitForSingleObject(g.thread, 2000);
        CloseHandle(g.thread);
        g.thread = nullptr;
    }
    if (InterlockedCompareExchange(&g.self_handle, 0, 0) != 0 && g.leg != INVALID_SOCKET)
        leg_send(OP_BYE, HANDLE_NONE, nullptr, 0);
    if (g.leg != INVALID_SOCKET) closesocket(g.leg);
    if (g.leg6 != INVALID_SOCKET) closesocket(g.leg6);
    if (g.local != INVALID_SOCKET) closesocket(g.local);
    g.leg = g.leg6 = g.local = INVALID_SOCKET;
    for (int i = 0; i < MAX_REMOTE; ++i)
        if (g.rem[i].used) {
            closesocket(g.rem[i].s);
            g.rem[i].used = false;
        }
    memset(g.pp, 0, sizeof(g.pp)); // mp:R3e -- the punch table goes with the sockets
    // mp:R1c -- drop the key material, the way udp_endpoint.cpp's stop() drops the PSK. A stopped
    // tunnel holds no secret anybody can reach, and a relink re-derives every one of these from
    // the PSK it is handed again.
    memset(g.psk, 0, sizeof(g.psk));
    memset(g.boot_enc, 0, sizeof(g.boot_enc));
    memset(g.boot_mac, 0, sizeof(g.boot_mac));
    memset(g.leg_key, 0, sizeof(g.leg_key));
    memset(g.my_key, 0, sizeof(g.my_key));
    memset(g.kk, 0, sizeof(g.kk));
    g.my_key_set = g.my_key_proved = false;
    // mp:L1f -- published rows go with the sockets that named them. Cleared BEFORE g_started drops,
    // so a reader that gets in between sees "tunnel up, nothing published" (-1) and never "tunnel
    // down, so everything is direct" for an address whose peer was in fact relayed a moment ago.
    pc_clear_all();
    if (g_started) DeleteCriticalSection(&g.cs);
    g_started = false;
}

bool active() { return g_started; }

// mp:L1f -- see the banner in udp_relay.h. Reads only the published side table, never State, so it
// is safe from the game thread while the pump runs.
int path_class(const sockaddr_in &a) {
    // NO TUNNEL, NO RELAY LEG, so no datagram this endpoint holds can have been relayed. A fact
    // about the build's own configuration, not an assumption about the network -- and it is what
    // makes an ordinary direct match render "D <n>" rather than a letterless number.
    if (!g_started) return 0;
    // Everything this file hands the endpoint is a LOOPBACK address it bound itself; an address
    // that is not one belongs to a conn that never came through the tunnel, and we have no reading
    // for it. (A host that relays one client and is dialled directly by another is exactly this
    // case -- and -1 is the honest answer there, not 0: "not through our tunnel" does not prove
    // "not through anything", and mp:R2b makes mixed hosts reachable.)
    if (a.sin_family != AF_INET || a.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) return -1;
    const LONG port = (LONG)ntohs(a.sin_port);
    if (port == 0) return -1;
    for (int i = 0; i <= MAX_REMOTE; ++i) {
        if (InterlockedCompareExchange(&g_pc_port[i], 0, 0) != port) continue;
        return (int)InterlockedCompareExchange(&g_pc_cls[i], 0, 0);
    }
    return -1;
}

unsigned short client_dial_port() { return g_started ? g.local_port : 0; }

void set_match_id(const uint8_t match_id[UUID7_BYTES]) {
    if (!g_started || match_id == nullptr || nil_id(match_id)) return;
    bool changed = false;
    EnterCriticalSection(&g.cs);
    if (memcmp(g.match_id, match_id, UUID7_BYTES) != 0) {
        memcpy(g.match_id, match_id, UUID7_BYTES);
        g.match_dirty = true;
        changed       = true;
    }
    LeaveCriticalSection(&g.cs);
    if (!changed) return;
    // The line that makes the acceptance clause checkable: the relay's structured log and BOTH
    // peers' mh_net.log now carry the same 32 hex digits for this match.
    char hex[UUID7_HEX_CAP];
    hex32(match_id, hex);
    logf("net: udp relay session match_id=%s", hex);
}

// ---- mp:R2, the session directory ------------------------------------------------------------

void set_session_info(const uint8_t *si, int len) {
    if (!g_started) return;
    if (len > DESC_MAX) {
        // Refused HERE rather than sent for the relay to refuse, so the log names the descriptor
        // that grew rather than a relay error code the player cannot act on.
        logf("net: udp relay -- lobby descriptor is %d B, over the %d B the relay holds; NOT "
             "published (the browser will not list this game over the relay)",
             len, DESC_MAX);
        return;
    }
    EnterCriticalSection(&g.cs);
    if (si != nullptr && len > 0) {
        memcpy(g.si, si, (size_t)len);
        g.si_len = len;
        g.si_at  = GetTickCount();
    } else {
        g.si_len = 0;
    }
    LeaveCriticalSection(&g.cs);
}

void set_directory_sink(dir_fn fn, void *ctx) {
    g_dir_ctx = ctx;
    g_dir     = fn;
}

void set_peer_known(known_fn fn, void *ctx) {
    g_known_ctx = ctx;
    g_known     = fn;
}

uint32_t room() { return g_started ? g.cfg.room : 0; }

uint32_t mint_host_room() { return mh::udproom::mint(key_random, nullptr); }

} // namespace udprelay
} // namespace mh
