//
// udp_transport.cpp -- mh_net_udp.dll's EXPORTED SURFACE (tracker mp:T1).
//
// The 27 rows of MH_NET_MODULE_SYMBOLS (mh_common/include/mh_net_module.h) over ONE Endpoint
// (udp_endpoint.cpp). Everything interesting is in the endpoint; this file exists to answer the
// contract and to own the three things the endpoint deliberately does not know about:
//
//   1. THE SIX TYPED CONTROL HANDLERS. The exported surface has six setters with six signatures
//      (MH_Net_SetJoinHandler and friends); the endpoint has ONE callback carrying the WireHdr flag.
//      Fanning out here keeps the endpoint ignorant of a surface it is not the only user of -- the
//      selftest drives the same endpoint with no handlers at all.
//   2. THE LOG PATH. Ruling Q1: run_context.cpp stays mh.dll-side, so a satellite is GIVEN its run
//      directory (MH_NetModule_Init) rather than composing one. Same rule, same fallback to the exe
//      directory, and the same file name -- mh_net.log -- as mh_net.dll, because which transport a
//      run used is a fact INSIDE the log, not a second log to go looking for.
//   3. THE ONE KNOB THAT IS THIS MODULE'S ALONE, `[net] udp_redundancy`. It is not in MH_NetConfig
//      and will not be: that struct is the mh.dll <-> module ABI, the TCP module must stay
//      byte-for-byte the build it was, and an ABI field meaningful to one transport would have to be
//      understood by both. So it is read here, from mh_net.ini beside the EXE -- the same place and
//      by the same composition mh_net/net_transport.cpp already uses to find mh_key.txt. That is not
//      a second reader of one config block (the F2 defect): NOTHING else in the process reads this
//      key, and this module reads no key anything else reads.
//
// WHY THIS TU IS NOT IN net_selftest.exe. It defines all 26 MH_Net_* symbols, and net_selftest.exe
// already compiles mh_net/net_transport.cpp, which defines the same 26. The suite tests the
// ENDPOINT, so it compiles udp_endpoint.cpp and nothing here -- which is the same two-TU discipline
// check_module_bind.py --net-surface enforces for module_bind.cpp.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "udp_endpoint.h"
#include "udp_snapshot.h" // mp:X1b -- the chunked snapshot pipeline these three rows drive
#include "udp_relay.h"    // mp:R1 -- the loopback tunnel that puts the Rust relay in the path
#include "mh_net_export.h"
#include "mh_net_key.h"
#include "mh_net_module.h"
#include "mh_run_context.h"            // mh_log_stamp ONLY -- the stamp format shared with the seams
#include "mh_net_proto/net_wire.h"     // the control-frame flags the six handlers fan out on
#include "mh_net_proto/session_info.h" // the match_id the relay's log is keyed by (SES0 / mp:R1)

#pragma comment(lib, "user32.lib") // wsprintfA

namespace {

mh::netudp::Endpoint g_ep;
bool                 g_started = false;

MH_SessionInfoCb g_si_cb       = nullptr;
MH_JoinCb        g_join_cb     = nullptr;
MH_StartCb       g_start_cb    = nullptr;
MH_LeaveCb       g_leave_cb    = nullptr;
MH_AnnounceCb    g_announce_cb = nullptr;
MH_HashCb        g_hash_cb     = nullptr;

bool g_log_on = false;
char g_log_path[MAX_PATH];
char g_run_dir[MAX_PATH] = {0};

// The run directory, handed in rather than asked for (fork F4B ruling Q1). The fallback is the EXE
// directory and it is not a nicety: net_selftest.exe links no mh.dll and has nothing to hand this
// module, so an un-set run dir must still produce a usable log beside the test binary.
const char *module_run_dir(void) {
    if (g_run_dir[0] != '\0') return g_run_dir;
    GetModuleFileNameA(nullptr, g_run_dir, MAX_PATH);
    char *slash = g_run_dir;
    for (char *p = g_run_dir; *p != '\0'; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    slash[1] = '\0';
    return g_run_dir;
}

void exe_dir(char *out) {
    GetModuleFileNameA(nullptr, out, MAX_PATH);
    char *slash = out;
    for (char *p = out; *p != '\0'; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    slash[1] = '\0';
}

// One line into mh_net.log, with the same local wall-clock stamp every other writer to that file
// uses (mh_run_context.h's mh_log_stamp). Absolute rather than relative because the questions this
// log answers are cross-process and cross-machine.
void log_line(void * /*ctx*/, const char *s) {
    if (!g_log_on) return;
    char      line[600];
    const int pre = mh_log_stamp(line);
    lstrcpynA(line + pre, s, (int)sizeof(line) - pre - 2);
    int n = lstrlenA(line);
    if (n < (int)sizeof(line) - 2) {
        line[n++] = '\n';
        line[n]   = '\0';
    }
    const HANDLE h = CreateFileA(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD wrote = 0;
    WriteFile(h, line, lstrlenA(line), &wrote, nullptr);
    CloseHandle(h);
}

// mp:R1. The relay's structured log is keyed by match_id (plan D8), and the transport ABI has no
// field for one -- MH_NetConfig is the mh.dll <-> module contract and this item does not change it.
// But the id is already crossing this file: every SESSION_INFO the host sends and every one a
// client receives carries it (SES0, session_info.h v3). So it is read from there, at the one point
// both roles pass through. A decode failure or a nil id is simply "not yet", never an error: an
// advert from before the lobby had an id is a normal state, and set_match_id() ignores both.
void sniff_match_id(const unsigned char *buf, int len) {
    if (!mh::udprelay::active() || buf == nullptr || len <= 0) return;
    mh_net_proto::SessionInfo si;
    if (!mh_net_proto::session_info_decode(buf, (size_t)len, si)) return;
    mh::udprelay::set_match_id(si.match_id);
}

// mp:R2 -- ONE ROW OF THE RELAY'S SESSION DIRECTORY, arriving on the tunnel thread.
//
// It is delivered through the SAME handler a SESSION_INFO from a connected peer uses, because it
// IS a SESSION_INFO -- the host registered the bytes verbatim and the relay stored them without
// looking. What differs is that there is no peer to name as the sender, and what the browser needs
// in that field instead is the relay ROOM, so the record can be dialled. session_info.h's
// `session_sender_for_relay_room` is that encoding, and the reason it is an encoding rather than a
// 24th module export is written out there: MH_NET_MODULE_SYMBOLS is a contract the TCP module must
// also answer, and a directory is not a thing the TCP module has.
void relay_directory_row(void * /*ctx*/, uint32_t room, const uint8_t *si, int len) {
    if (!g_si_cb) return;
    if (si == nullptr || len <= 0) {
        // The relay answered with an EMPTY directory. Room 0 is the directory room and is never a
        // lobby, so `relay sender, zero bytes` is unambiguous: "I am relayed, and there is nothing
        // to list". mh.dll drops its relay rows on it, which is how a departed host's row goes
        // away at once instead of on a timer.
        g_si_cb(mh_net_proto::SESSION_SENDER_RELAY_BASE, nullptr, 0);
        return;
    }
    const int sender = mh_net_proto::session_sender_for_relay_room(room);
    if (sender == mh_net_proto::SESSION_SENDER_NONE) return; // unroutable room; never mis-routed
    g_si_cb(sender, si, len);
}

// mp:R3e -- the peer-ownership predicate the relay tunnel frees its per-peer slots by
// (udp_relay.h, "who says a peer is gone"). Asked on the tunnel thread; the endpoint answers from
// its own peer table under its own lock, and learns nothing about relays by being asked.
bool relay_peer_known(void * /*ctx*/, const sockaddr_in &a) { return g_ep.knows_addr(a); }

// mp:L1f -- the SAME edge asked the other way (udp_endpoint.h path_class_fn, udp_relay.h
// path_class). This file is the one that knows both layers exist, so the crossing lives here and
// neither side gains a header of the other's: the endpoint hands over an address it already holds
// and gets back an opaque 1/0/-1 it copies into MH_NetPeerLatency.relayed; the relay answers from
// its own published table. Asked on the GAME thread (get_stats), answered lock-free.
int relay_path_class(void * /*ctx*/, const sockaddr_in &a) { return mh::udprelay::path_class(a); }

// The endpoint's single control edge, fanned back out into the six the surface declares.
void ctrl_dispatch(void * /*ctx*/, uint16_t flags, int sender, const unsigned char *buf, int len) {
    using namespace mh_net_proto;
    switch (flags) {
        case FLAG_SESSION_INFO:
            sniff_match_id(buf, len);
            if (g_si_cb) g_si_cb(sender, buf, len);
            break;
        case FLAG_JOIN:
            if (g_join_cb) g_join_cb(sender, buf, len);
            break;
        case FLAG_START:
            if (g_start_cb) g_start_cb(sender, buf, len);
            break;
        case FLAG_LEAVE:
            if (g_leave_cb) g_leave_cb(sender);
            break;
        case FLAG_ANNOUNCE:
            if (g_announce_cb) g_announce_cb(sender, buf, len);
            break;
        case FLAG_HASH:
            if (g_hash_cb) g_hash_cb(sender, buf, len);
            break;
        default: break;
    }
}

} // namespace

// =================================================================================================
// mp:T1b -- RE-INITIALISING AN ALREADY-STARTED TRANSPORT RESTARTS IT, the same rule mh_net.dll's
// MH_Net_InitEx acquired at U40 and for the same measured reason: without it `g_started` latches
// true for the life of the process, so a client whose link died at match teardown can never dial the
// re-created lobby (MH_Net_IsStarted() gates the discovery poll's connect kick, and InitEx itself
// early-returned). U40 fixed that for transport=tcp only; a UDP lane brought the dead end straight
// back.
//
// NOT A 24TH EXPORT. The module's contract is the 23 rows of MH_NET_MODULE_SYMBOLS and this file
// answers exactly those; a "stop" symbol would give every caller two ways to say one thing, and the
// thing they mean is "dial". So the rule is carried by InitEx, and the two modules now carry the
// SAME rule rather than one of them carrying an extra entry point.
//
// THE RESTART ITSELF IS THE ENDPOINT'S (udp_endpoint.cpp's start/stop pair), not this file's, which
// is what lets `net_selftest.exe udprelinktest` test it: the selftest links the endpoint core and
// never this TU (it would be 23 duplicate symbols against mh_net/net_transport.cpp). A restart
// implemented here would be a restart no oracle could reach.
//
// A REFUSED RESET LEAVES THE OLD TRANSPORT UP. Endpoint::stop() refuses when a thread will not stop
// inside its budget; start() then returns false with `started()` still true, and the two together
// are the signal this function turns into "0, and g_started untouched" -- exactly the state the
// caller was in before it asked, which is the pre-T1b behaviour rather than a half-torn-down link.
// The same is true of the key-file refusal below: it returns before the endpoint is touched at all.
extern "C" int MH_Net_InitEx(const MH_NetConfig *cfg) {
    if (!cfg) return g_started ? 1 : 0;

    g_log_on = (cfg->log != 0);
    if (g_log_on) wsprintfA(g_log_path, "%smh_net.log", module_run_dir());

    char dir[MAX_PATH];
    exe_dir(dir);

    // Link security: mh_key.txt decides it, once, for the whole session -- and FAILING CLOSED on a
    // broken key file is deliberate. The alternative (fall back to open) would silently take a host
    // that believes it is protected and publish it.
    unsigned char psk[MH_KEY_LEN];
    char          key_hex[MH_KEY_HEX_LEN + 1] = {0};
    int           generated                   = 0;
    const int     st                          = MH_Key_Load(dir, psk, key_hex, &generated);
    if (st == MH_KEY_INVALID) {
        log_line(nullptr, "net: mh_key.txt is unreadable/corrupt -- REFUSING to start the udp "
                          "transport. Fix or delete it (delete = a fresh key is generated; the "
                          "single word 'open' = no protection).");
        return 0;
    }
    const bool secure = (st == MH_KEY_SECURE);
    if (secure) {
        char b[220];
        wsprintfA(b, "net: udp link security ON (key %.8s..., mh_key.txt). Everyone must use the "
                     "SAME key.",
                  key_hex);
        log_line(nullptr, b);
        if (generated) {
            char g[160];
            wsprintfA(g, "net: generated a new key -- send mh_key.txt to the players joining you: %s",
                      key_hex);
            log_line(nullptr, g);
        }
    } else {
        // "open" here still runs the handshake -- UDP has no connection, so the host needs something
        // to bind a source address to a peer slot -- but under an all-zero key, so it authenticates
        // nobody. That is the same MEANING "open" has on TCP, reached by a different mechanism.
        memset(psk, 0, sizeof(psk));
        log_line(nullptr,
                 "net: *** udp link security OFF (mh_key.txt says 'open') *** -- no authentication, "
                 "no encryption. Anyone who can reach this port can join or feed data to the game. "
                 "LAN/testing only.");
    }

    mh::netudp::Config c;
    memset(&c, 0, sizeof(c));
    c.net       = *cfg;
    c.bind_port = 0;
    // `[net] udp_redundancy` -- K, the number of recent segments every datagram repeats. Default 3
    // (T0's INPUT_K_DEFAULT); the endpoint clamps it into [1, 4], the ceiling being the MTU rather
    // than T0's K of 8 (four 255-byte entries plus the header and tag already fill a 1200-byte
    // datagram).
    char ini[MAX_PATH];
    wsprintfA(ini, "%smh_net.ini", dir);
    c.redundancy = GetPrivateProfileIntA("net", "udp_redundancy", mh::netudp::K_DEFAULT, ini);
    // mp:T2 -- the channel-C MEASUREMENT knob, off by default and off in every shipped run. Non-zero
    // makes the HOST push that many mebibytes of a synthetic blob over channel C once a peer's
    // desync sample reports the configured sim step, so that "a bulk transfer in progress does not
    // delay lockstep traffic on channel A" is measurable on the rig against a run without one. Read
    // here for the same reason `udp_redundancy` is: MH_NetConfig is the mh.dll <-> module ABI, and
    // the TCP module must stay byte-for-byte the build it was.
    c.bulk_selftest_mb   = GetPrivateProfileIntA("net", "bulk_selftest_mb", 0, ini);
    c.bulk_selftest_step = GetPrivateProfileIntA("net", "bulk_selftest_step", 100, ini);

    // ---- mp:R1/R7a, the RELAY -------------------------------------------------------------------
    // `[net] relay=<host:port>` puts the Rust relay in the path: the tunnel is a separate object --
    // udp_relay.cpp -- and the endpoint does not know it exists: a CLIENT is simply pointed at a
    // loopback port, and a HOST binds its usual port and is dialled on loopback. Promotion to direct
    // is mp:R3.
    //
    // mp:R7a -- WHO DECIDES A DIAL IS RELAYED IS NOW mh.dll, NOT THIS MODULE. Before R7a this module
    // read `[net] relay` itself and tunnelled EVERY connection, so a peer with a relay configured had
    // no direct mode at all -- an *Internet server* + a typed address was dialled through the relay
    // anyway. Now mh.dll reads the ini once and decides, per dial, whether THIS connection is relayed
    // (first-browser refresh/join and a relay-directory row -> yes; *Internet server* + typed IP ->
    // no), and hands the answer in `cfg->relay_addr`/`cfg->relay_room`. So the tunnel comes up iff
    // `cfg->relay_addr` is non-empty; an empty one is a DIRECT dial to `cfg->host:cfg->port`, even
    // when the ini still carries `relay=`. The trailing-`;`-comment trim that used to live here moved
    // to mh.dll (net_seams.cpp lazy_start) with the ini read; a relayed `cfg->relay_addr` arrives
    // already clean. What STAYS a module ini read is `force_relay` (a relayed-path knob, below) and
    // the `relay_room` retirement notice (mp:R1e) -- neither is the dial read R7a removed.
    const char *const relay_addr = cfg->relay_addr;
    // mp:R1d -- the endpoint's datagram ceiling drops by what the leg wraps around it. Set BEFORE
    // g_ep.start below reads the config, and left at 0 for a direct link, which keeps that wire
    // byte for byte what it was.
    c.leg_overhead = (relay_addr[0] != '\0') ? mh::udprelay::LEG_OVERHEAD : 0;
    if (relay_addr[0] != '\0') {
        mh::udprelay::Config rc;
        memset(&rc, 0, sizeof(rc));
        const char *colon = nullptr;
        for (const char *p = relay_addr; *p != '\0'; ++p)
            if (*p == ':') colon = p;
        int hlen = colon ? (int)(colon - relay_addr) : lstrlenA(relay_addr);
        if (hlen > (int)sizeof(rc.host) - 1) hlen = (int)sizeof(rc.host) - 1;
        memcpy(rc.host, relay_addr, (size_t)hlen);
        rc.host[hlen] = '\0';
        int rport     = 0;
        for (const char *p = colon ? colon + 1 : ""; *p >= '0' && *p <= '9'; ++p)
            rport = rport * 10 + (*p - '0');
        if (rport <= 0 || rport > 65535) rport = 7100; // mh_relay's own default port
        rc.port = (unsigned short)rport;
        // mp:R6 -- A HOST'S ROOM IS MINTED, PER LOBBY, and never `[net] port`. The port was R1's
        // room for a good reason then (two peers who agree on a port agree on a room) and a bad
        // one on a shared relay now: every player ships with 6501, so the second host at any
        // moment was refused `room_busy` until the first lobby ended, and a host that crashed out
        // of a lobby and relaunched inside the relay's idle window was refused by its OWN stale
        // slot (measured 2026-09-19 on the VPS: 30 refusals over 16 s). A random 30-bit code needs
        // no agreement -- the client learns it from the directory (mp:R2) -- and a collision is
        // met by a re-mint inside the tunnel (udp_relay.cpp's room_busy branch). Minted here, once
        // per transport start, which is also every re-host and every U40/R7 relink: those restart
        // the tunnel below, and a restarted tunnel is a fresh registration under a fresh code.
        // A CLIENT's room is `cfg->relay_room` (mp:R7a; was `cfg->port`): mh.dll writes the
        // directory's pick there, or the ini port as the pre-directory guess that the tunnel's
        // `no_host` fallback turns into a directory browse.
        //
        // A zero mint is the CSPRNG failing, and the answer is the endpoint's own for the same
        // event: refuse to host. A host on a guessable or fixed fallback code would be exactly
        // the collision this item removes, wearing a log line that says the relay started.
        if (cfg->role == 0) {
            rc.room = mh::udprelay::mint_host_room();
            if (rc.room == 0) {
                log_line(nullptr, "net: udp relay -- no secure randomness to mint a host room; "
                                  "REFUSING to start the transport (mp:R6)");
                return 0;
            }
        } else {
            rc.room = cfg->relay_room;
            // mp:R2c -- mh.dll now hands DIRECTORY_ROOM (0) itself when it has no typed address and
            // no directory pick yet (net_discovery.cpp relay_default_room()), rather than the ini
            // port `[net] port` a HELLO would need to be REFUSED (`no_host`) before falling back to
            // this same room. Recognising that here is what lets start() skip arming the endpoint's
            // peer handshake below: a room this peer never asked to JOIN should never cost a
            // HS_BUDGET_MS "handshake FAILED" wait. A stale `relay_room=` override or a genuine
            // guess that turns out unhosted still falls back to DIRECTORY_ROOM reactively inside the
            // tunnel (the OP_ERROR handling below) -- browse_only only short-circuits the COMMON
            // case where mh.dll already knows there is nothing to dial.
            c.browse_only = (rc.room == mh::udprelay::DIRECTORY_ROOM);
        }
        // mp:R1e CLAUSE 2 -- `relay_room` IS READ AND IGNORED, and the notice is the point.
        // R1 gave the knob a default of `[net] port` because two peers who agree on a port agree
        // on a room; R2 made the LOBBY mint the room (a client re-dials the room the directory
        // named) and R6 finished the job for the host, which retired the reason the knob existed.
        // What is left is a footgun with the shape a silent override always has: a stale
        // `relay_room=` in somebody's ini out-votes the lobby the player just clicked, and the
        // symptom is `no_host` in a file nobody re-reads. So the value is read ONLY to say it is
        // being ignored -- deleting the read would leave the operator with an ini key that does
        // nothing and no line anywhere admitting it. The room the notice names is the one this
        // peer really uses: a host's minted code, a client's directory pick or pre-directory guess.
        const int pinned_room = GetPrivateProfileIntA("net", "relay_room", -1, ini);
        if (pinned_room >= 0 && (uint32_t)pinned_room != rc.room) {
            char b[240];
            wsprintfA(b,
                      "net: udp relay -- [net] relay_room=%d is IGNORED (mp:R1e); the room comes "
                      "from the lobby now (a host's is minted, mp:R6), and this peer is using "
                      "room %u. Remove the key.",
                      pinned_room, (unsigned)rc.room);
            log_line(nullptr, b);
        }
        rc.role      = cfg->role;
        rc.game_port = (unsigned short)cfg->port;
        // mp:R3 -- `[net] force_relay=1` pins this peer to the relayed path: no candidates are
        // published, no probes are sent, and an inbound probe is answered (so the far end is not
        // left diagnosing a network that drops probes) but never promotes. Read here for the same
        // reason `udp_redundancy` and `relay` are: MH_NetConfig is the mh.dll <-> module ABI and
        // the TCP module, which has no relay and no punch, must stay the build it was.
        rc.force_relay = GetPrivateProfileIntA("net", "force_relay", 0, ini);

        // A tunnel already up belongs to the PREVIOUS lobby (a peer that re-hosts, or the U40
        // restart path). Restart it rather than leaving a leg registered as the thing this peer no
        // longer is: the relay would keep routing to a host that has become a client.
        if (mh::udprelay::active()) mh::udprelay::stop();
        // mp:R2. The directory sink is registered for a CLIENT only, and that is what decides
        // whether this peer asks the relay for the session list at all: a host is the thing being
        // listed, and a run with no browser on it (the determinism gate's force-entry path never
        // reaches this branch as a browsing client) puts no LIST on the wire.
        mh::udprelay::set_directory_sink(cfg->role == 1 ? relay_directory_row : nullptr, nullptr);
        // mp:R3e. Both roles, so a host's per-client slots are released when the endpoint drops
        // the client (LEAVE, link timeout, a handshake that never finished) rather than held until
        // the lobby ends -- the ninth distinct joiner of a lobby used to get no socket and no punch.
        mh::udprelay::set_peer_known(relay_peer_known, nullptr);
        if (!mh::udprelay::start(rc, psk, log_line, nullptr)) {
            log_line(nullptr, "net: udp relay could not be started -- REFUSING to start the "
                              "transport. A peer that believes it is relayed and is not would sit "
                              "in a lobby nobody can reach; fix `[net] relay` or remove it.");
            return 0;
        }
        if (cfg->role == 1) {
            // THE ONE LINE THAT PUTS THE RELAY IN THE PATH. The endpoint dials the tunnel's
            // loopback port instead of the host's address; everything after this is unchanged.
            lstrcpynA(c.net.host, "127.0.0.1", (int)sizeof(c.net.host));
            c.net.port = (int)mh::udprelay::client_dial_port();
        }
    }

    g_ep.set_log(log_line, nullptr);
    g_ep.set_ctrl(ctrl_dispatch, nullptr);
    // mp:L1f -- UNCONDITIONALLY, outside the `relay configured` branch above and for BOTH roles.
    // The answer for a build with no relay at all is not "unknown", it is DIRECT (path_class returns
    // 0 when the tunnel is not running), and that is exactly the answer an ordinary LAN lobby's
    // ping cell should carry. Installing it only when a relay is configured would make the plain
    // case the letterless one -- the opposite of the truth.
    g_ep.set_path_class(relay_path_class, nullptr);
    const bool ok = g_ep.start(c, psk, secure); // restarts a started endpoint; it logs the relink
    // The refusal case, and the ONLY reason this is not a bare assignment: the endpoint could not
    // stop, so the OLD link is still up and still ours. Leaving g_started true keeps the module's
    // view and the endpoint's view of that one fact identical -- the alternative (g_started = false
    // over a running endpoint) would advertise a stopped transport that still has threads, peers and
    // a bound port.
    if (!ok && g_ep.started()) return 0;
    g_started = ok;
    return ok ? 1 : 0;
}

extern "C" int MH_Net_Send(int dst_player, const void *buf, int len) {
    return g_started ? g_ep.send(dst_player, buf, len) : 0;
}
extern "C" int MH_Net_Recv(int *out_sender, void *buf, int *inout_len) {
    return g_started ? g_ep.recv(out_sender, buf, inout_len) : 0;
}
extern "C" int MH_Net_PeerCount(void) { return g_started ? g_ep.peer_count() : 0; }
extern "C" int MH_Net_LocalPlayerId(void) { return g_started ? g_ep.local_player_id() : -1; }
extern "C" int MH_Net_IdAssigned(void) { return g_started ? g_ep.id_assigned() : 1; }
extern "C" int MH_Net_ActivePeerIds(int *out, int cap) {
    return g_started ? g_ep.active_peer_ids(out, cap) : 0;
}
extern "C" int  MH_Net_TakeDeadPeer(void) { return g_started ? g_ep.take_dead_peer() : -1; }
extern "C" int  MH_Net_IsStarted(void) { return g_started ? 1 : 0; }
extern "C" void MH_Net_GetStats(MH_NetStats *out) {
    if (!out) return;
    if (!g_started) {
        memset(out, 0, sizeof(*out));
        return;
    }
    g_ep.get_stats(out);
}
extern "C" void MH_Net_SetPeerHorizon(int player_id, int horizon_ms) {
    if (g_started) g_ep.set_peer_horizon(player_id, horizon_ms);
}
// mp:U41e (was mp:U41b's no-op -- see mp:U41c/G305): this module's inbound ring is now the SAME
// sequence-merged lane pair mh_net.dll's TCP transport uses (udp_endpoint.h/.cpp), so the match
// boundary restarts the SAME per-match counters through the SAME entry point.
extern "C" void MH_Net_QueueMatchBoundary(void) {
    if (g_started) g_ep.queue_match_boundary();
}

extern "C" void MH_Net_SetSessionInfoHandler(MH_SessionInfoCb cb) { g_si_cb = cb; }
extern "C" void MH_Net_SendSessionInfo(const unsigned char *buf, int len) {
    sniff_match_id(buf, len); // mp:R1 -- the HOST's half of the same read ctrl_dispatch does
    // mp:R2 -- and the same sniff point publishes the lobby to the relay's directory. A host
    // advertises at ~1 Hz for as long as it sits in its lobby, so this doubles as the liveness
    // signal: when it stops, the tunnel withdraws the row.
    mh::udprelay::set_session_info(buf, len);
    g_ep.send_ctrl(mh_net_proto::FLAG_SESSION_INFO, buf, len);
}
extern "C" void MH_Net_SetJoinHandler(MH_JoinCb cb) { g_join_cb = cb; }
extern "C" void MH_Net_SendJoin(const unsigned char *buf, int len) {
    g_ep.send_ctrl(mh_net_proto::FLAG_JOIN, buf, len);
}
extern "C" void MH_Net_SetStartHandler(MH_StartCb cb) { g_start_cb = cb; }
extern "C" void MH_Net_SendStart(const unsigned char *buf, int len) {
    g_ep.send_ctrl(mh_net_proto::FLAG_START, buf, len);
}
extern "C" void MH_Net_SetLeaveHandler(MH_LeaveCb cb) { g_leave_cb = cb; }
extern "C" void MH_Net_SendLeave(void) { g_ep.send_ctrl(mh_net_proto::FLAG_LEAVE, nullptr, 0); }
extern "C" void MH_Net_SetAnnounceHandler(MH_AnnounceCb cb) { g_announce_cb = cb; }
extern "C" void MH_Net_SendAnnounce(const unsigned char *buf, int len) {
    g_ep.send_ctrl(mh_net_proto::FLAG_ANNOUNCE, buf, len);
}
extern "C" void MH_Net_SetHashHandler(MH_HashCb cb) { g_hash_cb = cb; }
extern "C" void MH_Net_SendHash(const unsigned char *buf, int len) {
    if (len <= 0) return;
    g_ep.send_ctrl(mh_net_proto::FLAG_HASH, buf, len);
}

// ---- mp:X1b -- THE SNAPSHOT ROWS, IN ONE BLOCK ---------------------------------------------------
//
// Three contract rows over mh::netudp::snapshot (udp_snapshot.h) and the endpoint's channel-C edge.
// They are KEPT TOGETHER, at the end of the file, deliberately: this TU is edited by more than one
// thread of work at a time, and a feature whose state, helpers and three entry points are one
// contiguous block is one merge rather than five.
//
// WHAT THIS BLOCK ADDS TO udp_snapshot.h, WHICH IS ALL OF IT. That header owns the format, the
// hashes and the refusals, and it owns them for one process at a time -- `net_selftest udpsnaptest`
// drives a Sender and a Receiver by hand across two endpoints it created itself. What was missing
// was an entry point mh.dll could REACH, and that is what these three are. Nothing about the
// pipeline is re-decided here.
//
// ---- THE TWO ALLOCATIONS, AND WHY A MODULE THAT "ALLOCATES NOTHING" MAKES THEM --------------------
//
// udp_snapshot.h says, correctly, that the pipeline allocates nothing: the Sender borrows the
// caller's blob, the Receiver writes into a buffer the caller owns. That rule is about the PIPELINE.
// This block is the APPLICATION on this side of the module boundary -- it IS the caller the header
// is talking about -- so the two buffers are its job:
//
//   g_snap_tx_copy   OUR COPY of the blob handed to Send. Sender::begin() hashes now and reads the
//                    bytes later -- for tens of seconds at channel C's rate limit -- so the blob must
//                    not move. It belongs to mh.dll's harness, off the process heap, and asking
//                    another module to keep 8 MB alive and UNCHANGED across an unknown number of
//                    frames is a contract nobody can keep. Its breach, moreover, appears at the far
//                    end as ERR_CHUNK_HASH, which names the symptom and hides the cause. Copying is
//                    8 MB and one memcpy, once per transfer, against a whole class of bug.
//
//   g_snap_rx_arena  the Receiver's destination. It is sized MAX_BODY_BYTES (32 MiB) because the
//                    receiver must be pointed at its buffer BEFORE the manifest arrives, and the
//                    manifest is the thing that says how big the body is -- a smaller arena would be
//                    refused with ERR_CAPACITY at the manifest, which is the right refusal and the
//                    wrong outcome. VirtualAlloc rather than a static array: 32 MiB of .bss would be
//                    paid by every run of a shipping game, while committed pages are only BACKED
//                    when touched, so a real transfer costs body_len and an idle process costs
//                    address space.
//
// AND THE ARENA IS OPENED ON THE FIRST CHUNK, NOT ON THE FIRST POLL. Both peers of a snapshot run arm
// the receiver (the harness knob is symmetric; only one of them is sent to), so a poll that allocated
// eagerly would reserve 32 MiB on the SENDER too, for a transfer it will never receive. The lane is
// drained first and the arena opened only if something was actually in it.
namespace {

namespace snap = mh::netudp::snapshot;

snap::Sender   g_snap_tx;
snap::Receiver g_snap_rx;

uint8_t *g_snap_tx_copy = nullptr; // our copy of the caller's blob; owned until the next Send
uint32_t g_snap_tx_len  = 0;
bool     g_snap_tx_on   = false;

uint8_t *g_snap_rx_arena = nullptr;
bool     g_snap_rx_open  = false;

int g_snap_err        = snap::OK;
int g_snap_rerequests = 0;

// ---- mp:X1c -- THE LAST DELIVERED ROOT, KEPT --------------------------------------------------
//
// THE BUG THIS EXISTS FOR. MH_Net_SnapshotPoll re-arms the Receiver before it returns, because a
// second snapshot in one session must not pay a 32 MiB reservation twice. The re-arm clears the
// receiver's manifest, so `MH_Net_SnapshotStatus` -- whose `root_hex` is populated only
// `if (s.manifest_ready)` -- answers "" to the very next caller. That caller is the one with a
// reason to ask: the harness reads the status IMMEDIATELY AFTER a successful Poll, to stamp the
// import line with the root the blob arrived under, and it printed `root=-` on every run.
//
// SO THE DELIVERY IS REMEMBERED HERE, in the application layer, rather than by making the receiver
// hold state it is finished with. The value is the root of the blob most recently HANDED OUT, and
// it stands until the next transfer opens a receiver -- which is the honest window, because during
// that next transfer the answer to "what did you last deliver" is genuinely the older one until the
// new one completes. `rx_len` is kept beside it for the same reason and by the same rule, so a
// caller can say how big the thing it just imported was without having held on to the Poll's
// out-parameter.
char     g_snap_last_root[65] = {0}; // 64 lowercase hex + NUL; empty = nothing delivered yet
uint32_t g_snap_last_len      = 0;

// The chunk staging buffer. FILE SCOPE, not a local: CHUNK_BYTES is 16 KiB and this is reached from
// the game's frame thread, whose stack is not ours to spend.
uint8_t g_snap_chunk[mh::netudp::bulk::CHUNK_BYTES];

void snap_free_tx(void) {
    if (g_snap_tx_copy != nullptr) VirtualFree(g_snap_tx_copy, 0, MEM_RELEASE);
    g_snap_tx_copy = nullptr;
    g_snap_tx_len  = 0;
    g_snap_tx_on   = false;
    g_snap_tx.reset();
}

// Drain every completed chunk the lane holds into the receiver. THE REFUSAL EDGE IS THE RE-REQUEST
// PROTOCOL -- there is no NAK frame: a chunk whose bytes disagree with the manifest rewinds the
// receiver's frontier to that index, and mp:T2's backward tx_seek re-sends it. Returns the number of
// chunks taken out of the lane.
int snap_drain(void) {
    int got = 0;
    for (;;) {
        uint32_t id  = 0;
        int      len = (int)mh::netudp::bulk::CHUNK_BYTES;
        if (!g_ep.bulk_recv(&id, g_snap_chunk, &len)) return got;
        ++got;
        if (!g_snap_rx_open) {
            // The first chunk of a transfer nobody asked for is still a transfer: open the arena now.
            if (g_snap_rx_arena == nullptr) {
                g_snap_rx_arena = (uint8_t *)VirtualAlloc(nullptr, snap::MAX_BODY_BYTES,
                                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                if (g_snap_rx_arena == nullptr) {
                    g_snap_err = snap::ERR_CAPACITY;
                    return got; // the chunk is dropped un-acked; the sender repeats it
                }
            }
            g_snap_rx.reset(g_snap_rx_arena, snap::MAX_BODY_BYTES);
            g_snap_rx_open = true;
            // mp:X1c: A NEW TRANSFER HAS BEGUN, so the remembered delivery is no longer "the last
            // thing this peer received" in any useful sense -- it is the previous one, and a status
            // that kept answering with it while a different blob was arriving would be a worse lie
            // than the empty string this item replaced. Cleared at the FIRST CHUNK, which is the
            // earliest moment a transfer is observable at all.
            g_snap_last_root[0] = '\0';
            g_snap_last_len     = 0;
        }
        const int rc = g_snap_rx.on_chunk(id, g_snap_chunk, (uint32_t)len);
        if (rc == snap::ERR_CHUNK_HASH) {
            ++g_snap_rerequests;
            g_snap_err = rc;
            g_ep.bulk_resume_at(id);
        } else if (rc != snap::OK) {
            g_snap_err = rc;
        }
    }
}

} // namespace

// Arm a transfer of `blob` to `dst_player`. 1 = the transfer is running; 0 = it is not, and
// MH_Net_SnapshotStatus says why. The caller may free `blob` the instant this returns.
extern "C" int MH_Net_SnapshotSend(int dst_player, const void *blob, int len) {
    if (!g_started || blob == nullptr || len <= 0) {
        g_snap_err = snap::ERR_ARG;
        return 0;
    }
    if ((uint32_t)len > snap::MAX_BODY_BYTES) {
        g_snap_err = snap::ERR_ARG;
        log_line(nullptr, "net: snapshot send REFUSED -- the blob is past the pipeline's cap");
        return 0;
    }
    snap_free_tx(); // a previous transfer's copy; see snap_free_tx and the ownership note above
    g_snap_tx_copy =
        (uint8_t *)VirtualAlloc(nullptr, (SIZE_T)len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (g_snap_tx_copy == nullptr) {
        g_snap_err = snap::ERR_CAPACITY;
        log_line(nullptr, "net: snapshot send REFUSED -- no room for the module's own copy");
        return 0;
    }
    memcpy(g_snap_tx_copy, blob, (size_t)len);
    g_snap_tx_len = (uint32_t)len;

    const int rc = g_snap_tx.begin(g_snap_tx_copy, g_snap_tx_len);
    if (rc != snap::OK) {
        g_snap_err = rc;
        snap_free_tx();
        return 0;
    }
    if (!g_ep.bulk_send_src(dst_player, &snap::Sender::source, &g_snap_tx, g_snap_tx.image_len())) {
        // Not an error and not a state: the peer is not admitted yet, or a transfer is already
        // running. The caller retries. The sender is torn down so a later call re-hashes rather than
        // half-arming over a manifest nothing is pushing.
        snap_free_tx();
        return 0;
    }
    g_snap_tx_on = true;
    char root[65];
    snap::hex32(g_snap_tx.root(), root);
    char b[260];
    wsprintfA(b,
              "net: snapshot SEND armed -- %lu B in %lu body + %lu manifest chunk(s) to player %d, "
              "root %.16s",
              (unsigned long)g_snap_tx_len, (unsigned long)g_snap_tx.body_chunks(),
              (unsigned long)g_snap_tx.manifest_chunks(), dst_player, root);
    log_line(nullptr, b);
    return 1;
}

// Drive the receiver and, once the blob is whole and verified, deliver it. 1 = *buf now holds a
// complete snapshot of *inout_len bytes; 0 = nothing yet, and *out_state says what "nothing" means.
//
// THE DELIVERY IS A COPY OUT, not a pointer into the arena, and that is the all-or-nothing rule
// reaching the caller: `Receiver::body()` is null until every chunk is held AND the assembled bytes
// hash to the manifest's body_sha, so there is no window in which a caller could be handed a
// half-filled buffer to look at.
extern "C" int MH_Net_SnapshotPoll(void *buf, int *inout_len, int *out_state) {
    const int cap = (inout_len != nullptr) ? *inout_len : 0;
    if (inout_len) *inout_len = 0;
    if (out_state) *out_state = MH_SNAP_IDLE;
    if (!g_started) return 0;

    snap_drain();
    if (!g_snap_rx_open) {
        if (out_state) *out_state = g_snap_tx_on ? MH_SNAP_SENDING : MH_SNAP_IDLE;
        return 0;
    }
    if (!g_snap_rx.complete()) {
        if (out_state) *out_state = MH_SNAP_RECEIVING;
        return 0;
    }

    const uint32_t n = g_snap_rx.body_len();
    if (buf == nullptr || cap < (int)n) {
        // The blob is whole and the caller cannot hold it. REFUSED rather than a truncated copy, and
        // the receiver is left armed so a caller that comes back with a bigger buffer gets it without
        // a second transfer.
        g_snap_err = snap::ERR_CAPACITY;
        if (out_state) *out_state = MH_SNAP_REFUSED;
        return 0;
    }
    memcpy(buf, g_snap_rx.body(), n);
    *inout_len = (int)n;
    if (out_state) *out_state = MH_SNAP_READY;

    char root[65];
    snap::hex32(g_snap_rx.root(), root);
    // mp:X1c: REMEMBERED BEFORE THE RE-ARM BELOW, which is the whole of the fix -- the reset three
    // lines down is what used to make the status answer "" to the caller reading it one line after
    // this one. The log line and the status now carry the same string by construction.
    lstrcpynA(g_snap_last_root, root, sizeof(g_snap_last_root));
    g_snap_last_len = n;
    char b[260];
    wsprintfA(b, "net: snapshot READY -- %lu B in %lu chunk(s), root %.16s, %d re-request(s)",
              (unsigned long)n, (unsigned long)g_snap_rx.body_chunks(), root, g_snap_rerequests);
    log_line(nullptr, b);
    // Re-arm for the next transfer. The arena is KEPT: a second snapshot in one session (mp:X3's
    // resync is exactly that) should not pay a 32 MiB reservation twice.
    g_snap_rx.reset(g_snap_rx_arena, snap::MAX_BODY_BYTES);
    g_snap_rx_open    = false;
    g_snap_rerequests = 0;
    return 1;
}

extern "C" void MH_Net_SnapshotStatus(MH_NetSnapshotStatus *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->size = (unsigned)sizeof(MH_NetSnapshotStatus);
    // SUPPORTED IS 1 EVEN WHEN THE TRANSPORT IS DOWN, and that is the distinction the field exists
    // to draw: it is a property of the BUILD (this module has channel C), not of the link. A caller
    // asking "can I do this right now" reads `state`; one asking "is this transport capable at all"
    // reads this, and conflating them is what MH_NetStats.lat_supported had to unpick.
    out->supported = 1;
    out->last_err  = g_snap_err;
    out->tx_len    = g_snap_tx_on ? g_snap_tx_len : 0u;
    out->tx_chunks = g_snap_tx_on ? g_snap_tx.body_chunks() : 0u;

    snap::Receiver::Stats s;
    g_snap_rx.stats(s);
    out->rx_len      = s.manifest_ready ? g_snap_rx.body_len() : 0u;
    out->rx_chunks   = s.body_chunks;
    out->rx_verified = s.verified_prefix;
    out->rx_refused  = (int)s.chunks_hash_refused;
    if (s.manifest_ready) {
        snap::hex32(g_snap_rx.root(), out->root_hex);
    } else if (g_snap_last_root[0] != '\0') {
        // mp:X1c: no manifest in flight, but this peer HAS delivered one. Answering with the last
        // delivered root is what lets a caller stamp the thing it just imported; it is replaced the
        // moment a new transfer's first chunk lands (see snap_drain).
        lstrcpynA(out->root_hex, g_snap_last_root, (int)sizeof(out->root_hex));
        if (out->rx_len == 0u) out->rx_len = g_snap_last_len;
    }

    if (!g_started) out->state = MH_SNAP_IDLE;
    else if (s.complete) out->state = MH_SNAP_READY;
    else if (g_snap_rx_open) out->state = MH_SNAP_RECEIVING;
    else if (g_snap_tx_on) out->state = MH_SNAP_SENDING;
    else out->state = MH_SNAP_IDLE;
}

// MH_Key_Load is the module's 23rd contract row and its body is mh_net/net_key.cpp, compiled into
// this DLL too. Both transports mint and read the SAME mh_key.txt, which is the point: swapping
// `[net] transport` must not ask the players to exchange a second secret.

// ---- the module's one INTERNAL entry (the mirror of mh_net's) ------------------------------------
// NOT exported: mh.dll never calls it. mh_net_udp_dllmain.cpp owns the attach record and reaches the
// log path through here, for the same two-TU reason the TCP module splits them.
extern "C" void MH_NetInternal_SetRunDir(const char *dir) {
    if (dir == nullptr || dir[0] == '\0') return;
    lstrcpynA(g_run_dir, dir, MAX_PATH);
}
