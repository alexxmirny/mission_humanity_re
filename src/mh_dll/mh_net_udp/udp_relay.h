#pragma once
//
// udp_relay.h -- the CLIENT half of the UDP relay (tracker mp:R1, plan decision D4/D9).
//
// WHAT IT IS. A small loopback tunnel inside mh_net_udp.dll. With `[net] relay=<host:port>` set,
// both peers dial the relay instead of each other and the relay forwards between them, which is
// plan D4's RELAY-FIRST policy: a match starts relayed, and promotion to a direct path is mp:R3.
// The relay itself is `src/relay` (Rust, tokio); the settled design of both halves and the leg
// protocol they share is docs/mp-relay.md.
//
// WHY A TUNNEL AND NOT A CHANGE INSIDE THE ENDPOINT. Three reasons, in increasing order of how
// much they would have cost to learn later:
//
//   1. THE ENDPOINT DOES NOT HAVE TO KNOW. udp_endpoint.cpp is the T0 transport: a socket, a
//      handshake, a segment stream. "Where is the far end" is a question it already answers from
//      MH_NetConfig, and pointing it at 127.0.0.1 is a complete answer. Every line of relay
//      knowledge that would have gone into that file is a line the direct path would then have to
//      step around -- and R3's whole job is to go back to the direct path.
//   2. THE HOST CANNOT TELL TWO CLIENTS APART OTHERWISE. `host_on_boot` matches a handshake in
//      flight by SOURCE ADDRESS (udp_endpoint.cpp), because at that moment there is no conn_id
//      yet: the client's HELLO rides `boot_conn`, which is derived from the PSK and is therefore
//      the same eight bytes for every peer of every match. Two clients arriving through ONE relay
//      address would be read as one client retransmitting, and the second join would never
//      complete -- the identical defect `alloc_conn_slot`'s comment records from the first
//      three-peer loopback run. So the host side of this tunnel gives every remote peer its own
//      loopback socket, and the endpoint sees the distinct addresses it needs.
//   3. IT IS THE SAME SHAPE THE PROMOTION WANTS. Stopping the tunnel is exactly "go direct".
//
// THE COST, STATED: one extra loopback hop (microseconds) and 34 bytes of leg header per datagram,
// so a 1200-byte T0 datagram becomes 1234 on the wire. docs/mp-relay.md carries the MTU note.
//
#ifndef MH_NET_UDP_RELAY_H
#define MH_NET_UDP_RELAY_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>

#include "mh_net_proto/net_crypto.h" // KEY_LEN -- the leg key is derived from the same PSK
#include "mh_net_proto/uuid7.h"      // UUID7_BYTES -- the match_id the relay's logs are keyed by

namespace mh {
namespace udprelay {

typedef void (*log_fn)(void *ctx, const char *line);

// mp:R2 -- one row of the relay's SESSION DIRECTORY, handed up as it arrives (on the tunnel
// thread). `si`/`len` are the host's SESSION_INFO bytes VERBATIM -- neither this file nor the
// relay parses them; udp_transport.cpp passes them to mh.dll's ordinary session-info handler, and
// `room` is what a join has to dial.
//
// `len == 0` is the EMPTY DIRECTORY: the relay answered and has nothing registered. It is a row
// rather than a silence because a browser has to distinguish "no lobbies" (drop what you are
// showing, now) from "no answer" (keep showing it until it ages out).
typedef void (*dir_fn)(void *ctx, uint32_t room, const uint8_t *si, int len);

// The room a browsing client registers in when it has no lobby code yet (`relay.rs`'s
// DIRECTORY_ROOM). Zero is free by construction: a host's room is minted non-zero (udp_room.h,
// mp:R6) and a client's is a host's room the directory named.
constexpr uint32_t DIRECTORY_ROOM = 0;

// The relay's ceiling on one descriptor (leg.rs DESC_MAX). SESSION_INFO's v4 worst case is 473.
constexpr int DESC_MAX = 512;

// mp:R1d -- WHAT THE LEG COSTS A DATAGRAM: the 18-byte leg header plus the 16-byte tag. Declared
// here, in the header the transport already includes, because the number's one real consumer is
// somewhere else entirely: udp_endpoint has to lower its own 1200-byte ceiling by exactly this
// much on a relayed link, and a second copy of "34" living next to that subtraction is how the two
// halves drift when the envelope grows a field. The endpoint is handed the NUMBER (Config::
// leg_overhead) and never learns what adds it -- see the note there.
constexpr int LEG_OVERHEAD = 18 + 16;

// ---- mp:R1c: THE PER-PEER LEG KEY ----------------------------------------------------------------
//
// R1's leg key is HMAC(psk, "mh-udp-relay-leg") -- ONE key for a whole deployment, so any peer that
// holds the PSK can forge another peer's handle: put `src = <their handle>` in the header, send it
// from your own address, and the relay's rebinding path (which exists so a NAT can move a real peer)
// hands their traffic to you. R3 widened that slightly: a probe is accepted on the same key, so a
// forged DIRECT path needs no relay to be fooled at all.
//
// The fix is to key each leg on something only that peer's SESSION has, and the connect token is
// exactly that -- minted by the host for one client, sealed inside a bootstrap datagram that only
// the two endpoints and the relay carrying it ever see. A PSK holder off that path never has it.
//
//     a client's leg key = HMAC(token.mac_c2s, "mh-udp-relay-leg-c")
//     a host's leg key   = HMAC(token.mac_s2c, "mh-udp-relay-leg-h")     (one per client)
//
// Two keys, not one, for the reason T0 splits its own MACs: a single shared pair key would let each
// end replay the other's datagrams back at it. And the material is the token's MAC keys rather than
// its ENC keys because the relay deliberately throws the enc keys away -- it must not be able to
// read the match -- so it could not derive from them, while a leg key derived from a mac key gives
// it nothing it did not already hold.
//
// WHEN: the token does not exist until the T0 handshake has run, and the handshake rides the leg, so
// the leg starts on the deployment key and changes under itself. The datagram CARRYING the grant is
// the last one on the old key in both directions -- see the .cpp, where the install happens after
// the send, and `relay.rs`, where it happens after the forward.
//
// These two are here rather than in the .cpp so a selftest can assert them without linking a file
// that owns sockets and a thread.
constexpr char LEG_LBL_CLIENT[] = "mh-udp-relay-leg-c";
constexpr char LEG_LBL_HOST[]   = "mh-udp-relay-leg-h";

// `client_side` picks the end: true = the client's key from mac_c2s, false = the host's from
// mac_s2c. Both peers and the relay call this with the same token and get the same two keys; there
// is no negotiation and nothing new on the wire.
inline void peer_leg_key(const uint8_t mac_key[mh_net_proto::KEY_LEN], bool client_side,
                         uint8_t out[mh_net_proto::KEY_LEN]) {
    const char *lbl = client_side ? LEG_LBL_CLIENT : LEG_LBL_HOST;
    int         n   = 0;
    while (lbl[n] != '\0') ++n;
    uint8_t full[mh_net_proto::SHA256_LEN];
    mh_net_proto::hmac_sha256(mac_key, mh_net_proto::KEY_LEN, (const uint8_t *)lbl, (size_t)n, full);
    for (int i = 0; i < (int)mh_net_proto::KEY_LEN; ++i) out[i] = full[i];
}

// The DEPLOYMENT key, R1's -- still the bootstrap key every leg starts on and the only one a peer
// that never completes a handshake ever has.
constexpr char LEG_LBL_DEPLOYMENT[] = "mh-udp-relay-leg";

inline void deployment_leg_key(const uint8_t psk[mh_net_proto::KEY_LEN],
                               uint8_t       out[mh_net_proto::KEY_LEN]) {
    int n = 0;
    while (LEG_LBL_DEPLOYMENT[n] != '\0') ++n;
    uint8_t full[mh_net_proto::SHA256_LEN];
    mh_net_proto::hmac_sha256(psk, mh_net_proto::KEY_LEN, (const uint8_t *)LEG_LBL_DEPLOYMENT,
                              (size_t)n, full);
    for (int i = 0; i < (int)mh_net_proto::KEY_LEN; ++i) out[i] = full[i];
}

struct Config {
    char           host[64];  // the relay's address -- `[net] relay` up to the colon
    unsigned short port;      // ...and after it
    uint32_t       room;      // host: minted per tunnel start (mint_host_room, mp:R6); client: the
                              // directory's pick, or `[net] port` as the pre-directory guess
    int            role;      // 0 = host, 1 = client, as MH_NetConfig.role
    unsigned short game_port; // host only: the port udp_endpoint.cpp binds, dialled on loopback
    // mp:R3 -- `[net] force_relay=1`. Pins every pair to the relay: no candidates are published, no
    // probes are sent, and an inbound probe is answered but never promotes. Two uses, both of them
    // live-system rather than development: proving a relay deployment really carries a whole match
    // (a run that silently went direct would prove nothing about the relay), and giving a player on
    // a network where the direct path is WORSE than the relayed one -- it happens; a bad ISP path
    // between two peers can be worse than two good paths through a datacentre -- a way to say so.
    int force_relay;
};

// Bring the tunnel up. Returns false if it could not (no socket, a relay address that does not
// parse) -- and the CALLER MUST THEN REFUSE TO START, rather than silently falling back to a direct
// connection: a peer that thinks it is relayed and is not would sit in a lobby nobody can join,
// and the log would say the transport started.
bool start(const Config &cfg, const uint8_t psk[mh_net_proto::KEY_LEN], log_fn log, void *log_ctx);
void stop();
bool active();

// CLIENT only: the loopback port udp_endpoint.cpp must be pointed at instead of the real host. Zero
// before start() succeeds.
unsigned short client_dial_port();

// The match_id, once mh.dll has minted one (SES0). Sniffed from the SESSION_INFO both roles already
// carry through this module (udp_transport.cpp) -- the transport ABI has no field for it and this
// item does not add one. Sending it on tells the relay which match a leg belongs to, which is what
// makes the relay's structured log correlate with both peers' mh_net.log by the SAME id.
void set_match_id(const uint8_t match_id[mh_net_proto::UUID7_BYTES]);

// ---- mp:R2, the session directory ----------------------------------------------------------------
//
// HOST SIDE. The SESSION_INFO this peer is advertising, sniffed at the one point both roles
// already pass through (udp_transport.cpp's MH_Net_SendSessionInfo) for the same reason
// set_match_id is: the transport ABI has no field for it and R2 does not add one. While it keeps
// arriving the tunnel REGISTERs it with the relay at ~1 Hz, so the lobby appears in the relay's
// directory; when it STOPS arriving (the host left its lobby) the tunnel withdraws it, which is
// what makes the row vanish on a browsing client rather than waiting out the relay's TTL.
//
// `len` of 0 withdraws immediately. A descriptor over DESC_MAX is refused here rather than sent
// for the relay to refuse.
void set_session_info(const uint8_t *si, int len);

// CLIENT SIDE. Where the directory rows go. Set once, before or after start(); a null sink simply
// stops the tunnel asking for the list at all, which is what a host does.
void set_directory_sink(dir_fn fn, void *ctx);

// ---- mp:R3e, PEER TEARDOWN: who says a peer is gone ----------------------------------------------
//
// HOST SIDE. The tunnel holds three per-peer tables keyed by the relay handle -- a loopback socket
// (`Remote`), the punch state and the pair keys -- and until R3e nothing ever released a slot: the
// ninth distinct handle of a session got no socket and no punch. The tunnel itself has NO signal
// that a peer left. The relay says nothing to a host when one of its clients drops (a client's BYE
// is between the client and the relay), and the one relay refusal that means "that peer is gone"
// (`no_peer`) names no handle and is only provoked by traffic the endpoint has already stopped
// sending. What the tunnel does have is a party that already decides peer lifetime: the ENDPOINT,
// which drops a conn on the peer's LEAVE, on its link timeout and on a stream fault, and reclaims
// a handshake that never finished. So the rule is ownership, not a timer:
//
//     a Remote lives exactly as long as the endpoint holds a conn or a live handshake for the
//     loopback address that Remote presents to it.
//
// `set_peer_known` installs that predicate (udp_transport.cpp wires it to Endpoint::knows_addr,
// which is a query about the endpoint's own peer table and teaches it nothing about relays). The
// pump asks it once a second per Remote, and frees a Remote -- socket, punch, keys, all three in
// one step -- that the endpoint does not know and that is older than a short grace (the window
// between the socket being made for a peer's first datagram and the endpoint registering the
// handshake it carries). A peer that is merely QUIET is not freed: quiet peers are still in the
// endpoint's table until ITS timeout says otherwise, which is R-live's job and not this file's.
// With no predicate installed (net_selftest has no transport, and a tunnel started alone) the
// tunnel keeps R1's behaviour: a slot, once used, is held until stop() -- so a build where the
// wiring is missing leaks exactly as loudly as before rather than freeing on a guess.
//
// Survives start()'s memset of the tunnel state for the same reason the directory sink does: it is
// set once by the transport and must outlive every U40/R7 relink.
typedef bool (*known_fn)(void *ctx, const sockaddr_in &a);
void set_peer_known(known_fn fn, void *ctx);

// The room this tunnel is currently homed in. Not always `Config::room`: a client refused
// `no_host` for the room it was given falls back to DIRECTORY_ROOM so it can still LIST and learn
// the codes that DO exist (`mp:R1e`'s footgun -- two peers disagreeing about `[net] port`), and a
// host refused `room_busy` re-mints (mp:R6).
uint32_t room();

// mp:R6 -- a fresh HOST room: a random non-zero 30-bit code from the transport's CSPRNG
// (udp_room.h). 0 means the RNG failed, and the caller must then REFUSE TO HOST rather than fall
// back to a guessable code -- 0 is the directory room, which a host may not name anyway. Called by
// udp_transport.cpp before start() so the room is the same one the R1e notice names.
uint32_t mint_host_room();

} // namespace udprelay
} // namespace mh

#endif // MH_NET_UDP_RELAY_H
