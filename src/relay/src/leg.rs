//! The **relay leg**: the little authenticated envelope a peer and the relay exchange (mp:R1).
//!
//! # Why there is an envelope at all, when T0 already has a conn_id
//!
//! The relay's routing key is the 8-byte `conn_id` in T0's plaintext header, and for traffic that
//! has one that is exactly what this file carries forward: [`Leg::src`] / [`Leg::dst`] name the two
//! ends of the very connection that conn_id identifies, and the relay checks one against the other
//! before it forwards a byte (`relay.rs`). The envelope exists for the traffic that has **no**
//! conn_id yet, and for one property a bare forwarder cannot have:
//!
//!   * **The PSK handshake has no per-peer conn_id.** `udp_endpoint.cpp` opens every connection by
//!     sending its HELLO on `boot_conn = HMAC(psk, "mh-udp-boot-conn")` -- a value derived from the
//!     shared key alone, so it is *identical for every peer of every match*. A relay demuxing on
//!     that field alone could not tell two joining clients apart, and the host could not tell them
//!     apart either: `host_on_boot` matches a pending handshake by SOURCE ADDRESS, so two clients
//!     arriving through one relay address would be read as one client retransmitting.
//!   * **A source address is not an identity.** Demuxing on `(ip, port)` is what NAT rebinding
//!     breaks, which is the whole reason T0 put a connection id in the header. A peer handle in a
//!     header that is authenticated end-to-relay keeps that property for the pre-conn_id phase too:
//!     a peer whose address changes keeps its handle, and the relay adopts the new address *after*
//!     the tag verifies, never before.
//!
//! So: the envelope is the relay's own transport, the sealed T0 datagram is its payload, and the
//! relay reads the payload's conn_id to decide what it is allowed to do with it -- never to decide
//! where it goes. `docs/mp-relay.md` states that split as the settled design.
//!
//! # The bytes
//!
//! ```text
//!  off size field
//!   0   1   magic/version -- high nibble 0x5 (the relay-leg family), low nibble 1
//!   1   1   op
//!   2   2   src  LE u16 -- the SENDER's relay handle; 0 before one is assigned, and 0 = the relay
//!   4   2   dst  LE u16 -- the destination handle; 0 = "the relay itself", or for a client's DATA
//!                          "my room's host", which is the only counterpart a client has
//!   6   4   room LE u32 -- the rendezvous code (a host's is minted per lobby, mp:R6; a client's
//!                          is the one the directory named, mp:R2)
//!  10   8   seq  LE u64 -- per leg DIRECTION, strictly increasing; a 64-entry replay window
//!  18   n   payload     -- for DATA, the verbatim T0 datagram, byte for byte
//! 18+n 16   tag = HMAC-SHA256(leg_key, bytes[0, 18+n)) truncated to 128 bits
//! ```
//!
//! `leg_key = HMAC-SHA256(psk, "mh-udp-relay-leg")`, derived from the same `mh_key.txt` the game
//! peers already share, by a label of its own so it can never be the same bytes as the boot keys
//! `udp_endpoint.cpp` derives from that PSK.
//!
//! **THE LEG IS AUTHENTICATED, NOT ENCRYPTED, AND THAT IS THE POINT.** The payload is already
//! sealed end-to-end; encrypting it twice would buy nothing and would stop the relay reading the
//! conn_id it routes on. The tag is what stops an off-path forger injecting into a session, and
//! what makes a garbage datagram cost the relay one HMAC instead of a table entry.
//!
//! **WHAT THE SHARED LEG KEY DOES NOT DEFEND AGAINST**, stated because a reader will otherwise
//! assume it does: every peer of one deployment holds the same PSK, so any of them can forge
//! another's handle and steal its address binding. That is the same trust boundary the game's own
//! PSK draws (a peer who holds the key is already admitted to the match), and it is the reason
//! `docs/mp-relay.md` lists a per-peer leg key derived from the connect token as R2's work rather
//! than pretending this one is finished.

use crate::wire::{ct_eq, hmac16, KEY_LEN, TAG_SIZE};
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr};

/// High nibble 0x5 = the relay-leg family (T0's packets are 0x4), low nibble = version 1.
pub const LEG_MAGIC_NIBBLE: u8 = 0x5;
pub const LEG_VERSION: u8 = 1;
pub const LEG_MAGIC_VER: u8 = (LEG_MAGIC_NIBBLE << 4) | LEG_VERSION;

pub const LEG_HDR: usize = 18;
/// 18 + T0's 1200-byte ceiling + the 16-byte tag. See `docs/mp-relay.md` on the MTU cost.
pub const LEG_MAX: usize = LEG_HDR + crate::wire::MAX_DATAGRAM + TAG_SIZE;
pub const LEG_MIN: usize = LEG_HDR + TAG_SIZE;

pub const OP_HELLO: u8 = 1;
pub const OP_WELCOME: u8 = 2;
pub const OP_DATA: u8 = 3;
pub const OP_PING: u8 = 4;
pub const OP_PONG: u8 = 5;
pub const OP_BYE: u8 = 6;
pub const OP_ERROR: u8 = 7;
// ---- the SESSION DIRECTORY (mp:R2) --------------------------------------------------------------
// Four ops on top of R1's seven, and they are the whole of the directory: a host publishes one
// opaque descriptor for the room it holds, a browsing peer asks for the set, the relay answers with
// pages. ADDITIVE BY CONSTRUCTION -- an R1 peer never sends 8..11, and an R1 relay refuses them as
// `bad_op`, which is a counted refusal with a name rather than a mis-parse.
pub const OP_REGISTER: u8 = 8; // host -> relay: "this is my lobby"; payload = the descriptor
pub const OP_UNREGISTER: u8 = 9; // host -> relay: "my lobby is gone"; empty payload
pub const OP_LIST: u8 = 10; // any registered peer -> relay: "what is hosted here"; empty payload
pub const OP_SESSIONS: u8 = 11; // relay -> peer: one page of the directory
                                // ---- HOLE PUNCHING (mp:R3) ----------------------------------------------------------------------
                                // ONE relay op, because the relay's whole part in punching is one sentence: carry a peer's candidate
                                // list to its counterpart, and tell each of them the address it is SEEN from. The probes themselves
                                // never touch the relay -- they are the same leg envelope sent peer to peer.
pub const OP_CAND: u8 = 12; // peer -> relay -> counterpart: "here is where you might reach me"

// ---- the two PEER-TO-PEER ops -------------------------------------------------------------------
//
// THE RELAY NEVER FORWARDS THESE AND NEVER SENDS THEM. They are declared here, in the file that owns
// the op number space, for the reason a reserved range always is: an op number means one thing under
// this leg key or it means nothing. A probe WILL sometimes arrive at the relay -- a peer punches
// every candidate it is handed, and a peer behind the same NAT as the relay can be handed the
// relay's own address -- and that must be a counted no-op on a number nothing else may later reuse,
// rather than a hole in the space.
pub const OP_PROBE: u8 = 13; // peer -> peer, directly: "can you hear me on this path"
pub const OP_PROBE_ACK: u8 = 14; // peer -> peer, directly: the echo that validates it

pub const OP_MAX: u8 = 14;

/// The most candidates one [`OP_CAND`] payload may carry, in either direction.
///
/// Eight is a ceiling on WORK, not a guess at how many addresses a machine has: every candidate a
/// peer receives costs it a probe every retry interval, so the list is the lever a hostile or merely
/// multi-homed peer would use to make its counterpart send traffic somewhere. A machine with more
/// interfaces than this loses the ones its own gatherer ranked last, never the reflexive one -- the
/// relay appends that AFTER truncating (`relay.rs`).
pub const MAX_CANDS: usize = 8;

/// `flags` bit 0: the relay OBSERVED this address, rather than the peer having claimed it. A peer
/// may not set it -- the relay strips the bit from everything it is told and appends its own
/// observation, so "the relay saw you here" cannot be forged by the peer it describes.
pub const CAND_OBSERVED: u8 = 0x01;

pub const CAND_FAM_V4: u8 = 4;
pub const CAND_FAM_V6: u8 = 6;

/// HELLO payload: `role(1) | flags(1) | match_id(16)`.
pub const HELLO_LEN: usize = 18;

/// HELLO `flags` bit 0 (mp:R1c): **this peer can re-key its leg from the connect token.**
///
/// The byte was reserved and zero from mp:R1, and this is the use it was reserved for. It has to be
/// ADVERTISED rather than assumed, because the re-key is a ratchet the relay drives: the moment the
/// relay installs a peer key it stops accepting the deployment key from that handle, and doing that
/// to a peer that cannot follow would take the peer off the air. An older peer sends flags 0, keeps
/// the shared key, and keeps R1's trust boundary -- which is not a downgrade for anybody else,
/// because the ratchet is per handle.
pub const HELLO_FLAG_PEER_KEY: u8 = 0x01;

// ---- the PROTOCOL LEVEL (mp:R4a) ------------------------------------------------------------------
//
// The leg header's low nibble is the WIRE version and both ends refuse a datagram that does not
// carry theirs (`bad_version`). The OPS a relay speaks are a second axis the nibble does not
// describe: seven ops were added over three items without touching it, each one "additive by
// construction" because an older relay refuses an op it lacks as a counted `bad_op` -- counted AT
// THE RELAY, and answered with nothing. So a peer built after an op was added, talking to a relay
// built before it, sees a link that comes up, forwards the match, and silently never does the new
// thing (dead-ends G241: a pre-wave-9 relay refused the re-key op and a pair simply never promoted).
//
// The level is the number that makes that visible. A peer's HELLO carries the level it was built
// against; the relay's WELCOME carries its own; the peer compares and SAYS SO -- one named log line
// and a notice on the browser -- when the relay is behind. **Bump it when an op is added or its
// payload/semantics change**, on both sides in the same commit (`udp_relay.cpp` mirrors it), and
// list what the new level means beside the constant so the peer's line can name what will not work.
//
//   0  never advertised -- every build before mp:R4a (the peer treats it as "old")
//   1  ops 1..14 (R1 + R2 directory + R3 punch) + R1c re-key + R4b NOT_REGISTERED on a stale
//      handle + R6 room re-mint on room_busy + R3e (mp:R4a itself)
//
// WHERE IT RIDES, and why not a new field: an older relay's `hello_parse` refuses any HELLO that is
// not exactly HELLO_LEN bytes (`malformed`, unanswered), so a trailing byte would make a new peer
// unable to register with the deployed relay at all -- the opposite of additive. The reserved
// `flags` byte is the one place an old relay reads and carries unknown bits through untouched
// (bit 0 is the only bit it tests), so the level is `flags >> 1`: seven bits, 0..127. In the other
// direction an older PEER refuses a WELCOME that is not exactly 4 bytes, so the relay appends its
// level ONLY to a peer whose HELLO carried one; a pre-R4a peer keeps getting the 4-byte form.
pub const PROTOCOL_LEVEL: u8 = 1;
pub const HELLO_LEVEL_SHIFT: u32 = 1;
pub const PROTOCOL_LEVEL_MAX: u8 = 0x7f;
// 0 is "never advertised" and seven bits is the room the flags byte has, so a bump past either is
// a compile error here rather than a level that reads as none on the wire.
const _: () = assert!(PROTOCOL_LEVEL >= 1 && PROTOCOL_LEVEL <= PROTOCOL_LEVEL_MAX);

/// The level a HELLO's `flags` byte carries; 0 = the peer advertised none (a pre-R4a build).
pub fn hello_level(flags: u8) -> u8 {
    flags >> HELLO_LEVEL_SHIFT
}

/// `flags` bit 0 (the re-key capability) plus `level` in bits 1..7.
pub fn hello_flags_with_level(flags: u8, level: u8) -> u8 {
    (flags & HELLO_FLAG_PEER_KEY) | (level.min(PROTOCOL_LEVEL_MAX) << HELLO_LEVEL_SHIFT)
}

/// WELCOME payload: `self(2) | other(2)`, and since mp:R4a `| level(1)` to a peer that advertised
/// one -- WELCOME_LEN is the form a pre-R4a peer (which refuses any other length) still gets.
pub const WELCOME_LEN: usize = 4;
pub const WELCOME_LEN_LEVEL: usize = 5;

pub const ROLE_HOST: u8 = 0;
pub const ROLE_CLIENT: u8 = 1;

/// The relay's own handle, and the "not assigned yet" value a peer's first HELLO carries. One
/// value for both because they are the same statement: this datagram has no peer at that end.
pub const HANDLE_NONE: u16 = 0;

// ERROR codes. Small and closed: a peer shows the operator one of these, so a new one is a new
// diagnosis rather than a shade of "it did not work".
pub const ERR_NO_HOST: u8 = 1; // no host has claimed this room
pub const ERR_ROOM_BUSY: u8 = 2; // a different host already holds it
pub const ERR_NOT_REGISTERED: u8 = 3; // DATA/PING from a handle the relay does not know
pub const ERR_ROOM_FULL: u8 = 4; // the room is at its peer cap
pub const ERR_NO_PEER: u8 = 5; // the named destination handle is gone
pub const ERR_NOT_HOST: u8 = 6; // mp:R2 -- REGISTER/UNREGISTER from a peer that is not a room's host
pub const ERR_DESC_TOO_LONG: u8 = 7; // mp:R2 -- a REGISTER whose descriptor exceeds DESC_MAX

/// The largest session descriptor the relay will hold, in bytes.
///
/// **The relay never parses one.** What a descriptor MEANS is `SESSION_INFO`
/// (`src/mh_net_proto/include/mh_net_proto/session_info.h`, 473 bytes at its v4 worst case), and
/// the two game peers already share that codec. Teaching the relay to read it would put a third
/// implementation of a format into the one process in this system that is deliberately unable to
/// read the match it carries -- so this is a SIZE and nothing else: bytes in, the same bytes out,
/// with room for the descriptor to grow a field without a relay deploy.
pub const DESC_MAX: usize = 512;

/// A SESSIONS page: `total u16 | offset u16 | count u8`, then `count` entries of
/// `room u32 | len u16 | descriptor`.
pub const SESSIONS_HDR: usize = 5;
pub const SESSIONS_ENTRY_HDR: usize = 6;

/// The three label-derived secrets a relay and a peer share, all from the one PSK in `mh_key.txt`.
///
/// `boot_*` are **not this file's** -- they are `udp_endpoint.cpp`'s bootstrap secrets, mirrored
/// here because the relay has to open the host's token grant to learn a session's MAC keys. The
/// labels are a contract between the two files; a change on either side is a silent link failure,
/// which is why they are spelled out in `docs/mp-relay.md` rather than only living in two headers.
pub struct Secrets {
    pub leg: [u8; KEY_LEN],
    pub boot_conn: [u8; 8],
    pub boot_enc: [u8; KEY_LEN],
    pub boot_mac: [u8; KEY_LEN],
}

// ---- mp:R1c: THE PER-PEER LEG KEY ---------------------------------------------------------------
//
// R1's leg key is `HMAC(psk, "mh-udp-relay-leg")` -- one key for a whole deployment. Every peer of
// that deployment holds it, so any of them can forge another's handle: send `src = <their handle>`
// from your own address, the tag verifies, and the relay adopts your address as theirs (the
// rebinding path in `on_datagram`, which exists so a NAT can move a real peer). Their match is then
// yours to redirect. R3 widened it slightly -- a probe accepted on the strength of the shared key
// puts a forged DIRECT path in, with no relay left to fool.
//
// THE FIX IS TO KEY THE LEG ON SOMETHING ONLY THAT PEER'S SESSION HAS, and the connect token is
// exactly that: minted fresh by the host for one client, carried sealed inside a bootstrap datagram
// that only the two endpoints and the relay carrying it ever see. A PSK holder who is not on that
// path never sees it. So:
//
//     client's key = HMAC(token.mac_c2s, "mh-udp-relay-leg-c")
//     host's key   = HMAC(token.mac_s2c, "mh-udp-relay-leg-h")
//
// TWO KEYS, NOT ONE, FOR THE REASON T0 SPLITS ITS OWN MACS: with one shared pair key each end could
// replay the other's datagrams back at it. And the material is the token's MAC keys rather than its
// enc keys because the relay deliberately DISCARDS the enc keys (it must not be able to read the
// match) -- deriving from something it throws away would not work, and deriving from something it
// keeps costs the match nothing, since a leg key is not a session key.
//
// WHEN IT TAKES EFFECT is the whole subtlety, and it is a RATCHET rather than a flag day. The token
// does not exist until the T0 handshake runs, and the handshake rides the leg -- so the leg must
// start on the deployment key and change under itself. The datagram that CARRIES the grant is the
// last one on the old key, in both directions; everything after it is on the new one. `relay.rs`
// installs after forwarding for exactly that reason.

pub const LEG_LBL_CLIENT: &[u8] = b"mh-udp-relay-leg-c";
pub const LEG_LBL_HOST: &[u8] = b"mh-udp-relay-leg-h";

/// The leg key for one end of one connection, from that connection's token.
///
/// `client_side` picks which end: true takes `mac_c2s` under the client label, false `mac_s2c`
/// under the host label. Both ends and the relay call this with the same token and get the same two
/// keys, which is the whole of the agreement -- there is no negotiation and nothing on the wire.
pub fn peer_leg_key(mac_key: &[u8; KEY_LEN], client_side: bool) -> [u8; KEY_LEN] {
    crate::wire::hmac32(
        mac_key,
        if client_side {
            LEG_LBL_CLIENT
        } else {
            LEG_LBL_HOST
        },
    )
}

impl Secrets {
    pub fn derive(psk: &[u8; KEY_LEN]) -> Secrets {
        let conn = crate::wire::hmac32(psk, b"mh-udp-boot-conn");
        let mut boot_conn = [0u8; 8];
        boot_conn.copy_from_slice(&conn[..8]);
        Secrets {
            leg: crate::wire::hmac32(psk, b"mh-udp-relay-leg"),
            boot_conn,
            boot_enc: crate::wire::hmac32(psk, b"mh-udp-boot-enc"),
            boot_mac: crate::wire::hmac32(psk, b"mh-udp-boot-mac"),
        }
    }
}

/// Every way a leg datagram can be refused, one value each -- for the same reason T0's
/// [`crate::wire::Verdict`] has one: a refusal that arrives as a bare `false` makes "an old build"
/// and "someone is probing the port" the same line in the log.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LegError {
    TooShort,
    TooLong,
    BadMagic,
    BadVersion,
    BadOp,
    BadMac,
    Malformed,
}

impl LegError {
    pub fn name(self) -> &'static str {
        match self {
            LegError::TooShort => "too_short",
            LegError::TooLong => "too_long",
            LegError::BadMagic => "bad_magic",
            LegError::BadVersion => "bad_version",
            LegError::BadOp => "bad_op",
            LegError::BadMac => "bad_mac",
            LegError::Malformed => "malformed",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Leg {
    pub op: u8,
    pub src: u16,
    pub dst: u16,
    pub room: u32,
    pub seq: u64,
    pub payload: Vec<u8>,
}

fn u16le(b: &[u8]) -> u16 {
    u16::from_le_bytes([b[0], b[1]])
}
fn u32le(b: &[u8]) -> u32 {
    u32::from_le_bytes([b[0], b[1], b[2], b[3]])
}
fn u64le(b: &[u8]) -> u64 {
    let mut a = [0u8; 8];
    a.copy_from_slice(&b[..8]);
    u64::from_le_bytes(a)
}

/// Verify and split one leg datagram.
///
/// The refusal ORDER matters the same way T0's does: length first, so an oversize datagram is
/// refused for its size whatever it claims to be, and the MAC last, so a malformed header costs no
/// HMAC. Nothing here looks at the payload -- what is inside a DATA is the session layer's problem.
pub fn decode(pkt: &[u8], key: &[u8; KEY_LEN]) -> Result<Leg, LegError> {
    decode_any(pkt, std::slice::from_ref(&key)).map(|(l, _)| l)
}

/// The sender's handle, read out of the header WITHOUT verifying anything (mp:R1c).
///
/// The relay needs it before the MAC, because since R1c the key the MAC is checked under is a
/// property of the handle. That is not a weakening: `src` is an unauthenticated CLAIM at this point
/// and is treated as one -- it selects which key set to try, and a claim that names a handle whose
/// key the datagram was not tagged with fails, which is precisely the forgery being refused.
pub fn peek_src(pkt: &[u8]) -> Option<u16> {
    if pkt.len() < LEG_MIN {
        return None;
    }
    Some(u16le(&pkt[2..4]))
}

/// The room code, read out of the header WITHOUT verifying anything (mp:R4b) -- so a refusal of a
/// datagram that failed its MAC can still name the room in the ERROR it answers with. An
/// unauthenticated claim, used for nothing but that echo.
pub fn peek_room(pkt: &[u8]) -> Option<u32> {
    if pkt.len() < LEG_MIN {
        return None;
    }
    Some(u32le(&pkt[6..10]))
}

/// Verify and split one leg datagram against a SET of acceptable keys, returning which one matched.
///
/// The set is at most a peer's own per-connection keys (a host of N clients holds N, one per pair)
/// or the single deployment key, so it is bounded by the room's peer cap and each miss costs one
/// HMAC over a datagram the relay was going to have to hash anyway. The ORDER IS FIXED -- see the
/// note at the call site on why move-to-front is a bug here and not an optimisation.
pub fn decode_any(pkt: &[u8], keys: &[&[u8; KEY_LEN]]) -> Result<(Leg, usize), LegError> {
    if pkt.len() > LEG_MAX {
        return Err(LegError::TooLong);
    }
    if pkt.len() < LEG_MIN {
        return Err(LegError::TooShort);
    }
    if pkt[0] >> 4 != LEG_MAGIC_NIBBLE {
        return Err(LegError::BadMagic);
    }
    if pkt[0] & 0x0f != LEG_VERSION {
        return Err(LegError::BadVersion);
    }
    if pkt[1] == 0 || pkt[1] > OP_MAX {
        return Err(LegError::BadOp);
    }
    let end = pkt.len() - TAG_SIZE;
    let mut which = usize::MAX;
    for (i, k) in keys.iter().enumerate() {
        let tag = hmac16(k, &pkt[..end]);
        if ct_eq(&tag, &pkt[end..]) {
            which = i;
            break;
        }
    }
    if which == usize::MAX {
        return Err(LegError::BadMac);
    }
    Ok((
        Leg {
            op: pkt[1],
            src: u16le(&pkt[2..4]),
            dst: u16le(&pkt[4..6]),
            room: u32le(&pkt[6..10]),
            seq: u64le(&pkt[10..18]),
            payload: pkt[LEG_HDR..end].to_vec(),
        },
        which,
    ))
}

/// Build one leg datagram. `payload` longer than T0's datagram ceiling is a bug in the caller, not
/// a link fault, so it panics in debug and truncates nothing in release -- it returns `None`.
pub fn encode(
    op: u8,
    src: u16,
    dst: u16,
    room: u32,
    seq: u64,
    payload: &[u8],
    key: &[u8; KEY_LEN],
) -> Option<Vec<u8>> {
    if op == 0 || op > OP_MAX || payload.len() > crate::wire::MAX_DATAGRAM {
        return None;
    }
    let mut out = Vec::with_capacity(LEG_HDR + payload.len() + TAG_SIZE);
    out.push(LEG_MAGIC_VER);
    out.push(op);
    out.extend_from_slice(&src.to_le_bytes());
    out.extend_from_slice(&dst.to_le_bytes());
    out.extend_from_slice(&room.to_le_bytes());
    out.extend_from_slice(&seq.to_le_bytes());
    out.extend_from_slice(payload);
    let tag = hmac16(key, &out);
    out.extend_from_slice(&tag);
    Some(out)
}

/// `role(1) | flags(1) | match_id(16)`.
pub fn hello_payload(role: u8, match_id: &[u8; 16]) -> Vec<u8> {
    let mut v = Vec::with_capacity(HELLO_LEN);
    v.push(role);
    v.push(0);
    v.extend_from_slice(match_id);
    v
}

/// mp:R1c -- the same payload with the reserved `flags` byte actually set.
pub fn hello_payload_flags(role: u8, flags: u8, match_id: &[u8; 16]) -> Vec<u8> {
    let mut v = Vec::with_capacity(HELLO_LEN);
    v.push(role);
    v.push(flags);
    v.extend_from_slice(match_id);
    v
}

/// Returns `(role, flags, match_id)`. An UNKNOWN flag bit is carried through untouched rather than
/// refused: the byte is a capability advertisement, and refusing a HELLO for a bit from a newer
/// build would make every future capability a flag day.
pub fn hello_parse(p: &[u8]) -> Result<(u8, u8, [u8; 16]), LegError> {
    if p.len() != HELLO_LEN {
        return Err(LegError::Malformed);
    }
    if p[0] != ROLE_HOST && p[0] != ROLE_CLIENT {
        return Err(LegError::Malformed);
    }
    let mut mid = [0u8; 16];
    mid.copy_from_slice(&p[2..18]);
    Ok((p[0], p[1], mid))
}

pub fn welcome_payload(me: u16, other: u16) -> Vec<u8> {
    let mut v = Vec::with_capacity(WELCOME_LEN);
    v.extend_from_slice(&me.to_le_bytes());
    v.extend_from_slice(&other.to_le_bytes());
    v
}

/// mp:R4a -- the WELCOME a level-advertising peer gets: `self(2) | other(2) | level(1)`.
pub fn welcome_payload_level(me: u16, other: u16, level: u8) -> Vec<u8> {
    let mut v = welcome_payload(me, other);
    v.push(level);
    v
}

pub fn welcome_parse(p: &[u8]) -> Result<(u16, u16), LegError> {
    welcome_parse_level(p).map(|(me, other, _)| (me, other))
}

/// Returns `(self, other, level)`, level 0 when the WELCOME carried none (a pre-R4a relay). Bytes
/// past the level are carried through unread rather than refused, for `hello_parse`'s reason: a
/// field a newer relay appends must not make every future field a flag day. (The C++ peer reads it
/// the same way; a pre-R4a peer refuses anything but the 4-byte form, which is why the relay sends
/// the level only to a peer that asked.)
pub fn welcome_parse_level(p: &[u8]) -> Result<(u16, u16, u8), LegError> {
    if p.len() < WELCOME_LEN {
        return Err(LegError::Malformed);
    }
    let level = if p.len() >= WELCOME_LEN_LEVEL {
        p[4]
    } else {
        0
    };
    Ok((u16le(&p[0..2]), u16le(&p[2..4]), level))
}

/// Encode one PAGE of the session directory: the entries that fit, plus where the page sits in the
/// whole. Returns the payload and how many entries it consumed.
///
/// A page rather than a list because a descriptor is up to [`DESC_MAX`] bytes while a leg datagram
/// carries [`crate::wire::MAX_DATAGRAM`] -- two full-sized descriptors already fill one. `total`
/// and `offset` are what let the peer's log say "4 of 6" instead of quietly believing the
/// directory is smaller than it is, which is the failure a bare truncation would produce.
pub fn sessions_encode(entries: &[(u32, Vec<u8>)], total: u16, offset: u16) -> (Vec<u8>, usize) {
    let mut v = Vec::with_capacity(crate::wire::MAX_DATAGRAM);
    v.extend_from_slice(&total.to_le_bytes());
    v.extend_from_slice(&offset.to_le_bytes());
    v.push(0);
    let mut n = 0usize;
    for (room, desc) in entries {
        if desc.len() > DESC_MAX || n == 255 {
            continue;
        }
        if v.len() + SESSIONS_ENTRY_HDR + desc.len() > crate::wire::MAX_DATAGRAM {
            break;
        }
        v.extend_from_slice(&room.to_le_bytes());
        v.extend_from_slice(&(desc.len() as u16).to_le_bytes());
        v.extend_from_slice(desc);
        n += 1;
    }
    v[4] = n as u8;
    (v, n)
}

/// The peer's half of [`sessions_encode`]. Returns `(total, offset, entries)`.
#[allow(clippy::type_complexity)]
pub fn sessions_parse(p: &[u8]) -> Result<(u16, u16, Vec<(u32, Vec<u8>)>), LegError> {
    if p.len() < SESSIONS_HDR {
        return Err(LegError::Malformed);
    }
    let total = u16le(&p[0..2]);
    let offset = u16le(&p[2..4]);
    let count = p[4] as usize;
    let mut at = SESSIONS_HDR;
    let mut out = Vec::with_capacity(count);
    for _ in 0..count {
        if at + SESSIONS_ENTRY_HDR > p.len() {
            return Err(LegError::Malformed);
        }
        let room = u32le(&p[at..at + 4]);
        let len = u16le(&p[at + 4..at + 6]) as usize;
        at += SESSIONS_ENTRY_HDR;
        if len > DESC_MAX || at + len > p.len() {
            return Err(LegError::Malformed);
        }
        out.push((room, p[at..at + len].to_vec()));
        at += len;
    }
    Ok((total, offset, out))
}

// ---- the candidate list (mp:R3) -----------------------------------------------------------------
//
//   count u8
//   count x { fam u8 (4 or 6) | flags u8 | port u16 LE | addr (4 or 16 bytes, network order) }
//
// FAMILY-TAGGED RATHER THAN V6-ONLY-WITH-MAPPING, which is the encoding a reader expects to find
// here instead. An IPv4 address carried as `::ffff:a.b.c.d` is 12 bytes bigger and, worse, arrives
// at the far end as a v6 socket address that a v4-only sender cannot use without unwrapping it
// again -- so the mapping would have to be undone by exactly the code least able to test it. A
// one-byte family tag costs one byte and keeps the two families distinguishable end to end, which
// is what a peer needs: a v4 candidate is probed from its v4 socket and a v6 candidate from its v6
// socket, and those are different sockets with different fates.
pub fn cands_encode(list: &[(SocketAddr, u8)]) -> Vec<u8> {
    let n = list.len().min(MAX_CANDS);
    let mut v = Vec::with_capacity(1 + n * 22);
    v.push(n as u8);
    for (addr, flags) in list.iter().take(n) {
        match addr.ip() {
            IpAddr::V4(a) => {
                v.push(CAND_FAM_V4);
                v.push(*flags);
                v.extend_from_slice(&addr.port().to_le_bytes());
                v.extend_from_slice(&a.octets());
            }
            IpAddr::V6(a) => {
                v.push(CAND_FAM_V6);
                v.push(*flags);
                v.extend_from_slice(&addr.port().to_le_bytes());
                v.extend_from_slice(&a.octets());
            }
        }
    }
    v
}

/// The peer's half of [`cands_encode`]. A list longer than [`MAX_CANDS`], an unknown family, or a
/// payload that does not end exactly where the count says it does is [`LegError::Malformed`] --
/// never a shorter list, for [`sessions_parse`]'s reason: a truncation accepted as a short answer is
/// a peer quietly probing fewer paths than it was told about.
pub fn cands_parse(p: &[u8]) -> Result<Vec<(SocketAddr, u8)>, LegError> {
    if p.is_empty() {
        return Err(LegError::Malformed);
    }
    let n = p[0] as usize;
    if n > MAX_CANDS {
        return Err(LegError::Malformed);
    }
    let mut at = 1usize;
    let mut out = Vec::with_capacity(n);
    for _ in 0..n {
        if at + 4 > p.len() {
            return Err(LegError::Malformed);
        }
        let fam = p[at];
        let flags = p[at + 1];
        let port = u16le(&p[at + 2..at + 4]);
        at += 4;
        let ip = match fam {
            CAND_FAM_V4 => {
                if at + 4 > p.len() {
                    return Err(LegError::Malformed);
                }
                let mut o = [0u8; 4];
                o.copy_from_slice(&p[at..at + 4]);
                at += 4;
                IpAddr::V4(Ipv4Addr::from(o))
            }
            CAND_FAM_V6 => {
                if at + 16 > p.len() {
                    return Err(LegError::Malformed);
                }
                let mut o = [0u8; 16];
                o.copy_from_slice(&p[at..at + 16]);
                at += 16;
                IpAddr::V6(Ipv6Addr::from(o))
            }
            _ => return Err(LegError::Malformed),
        };
        out.push((SocketAddr::new(ip, port), flags));
    }
    if at != p.len() {
        return Err(LegError::Malformed); // trailing bytes: a shape we do not understand
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn key() -> [u8; KEY_LEN] {
        let mut k = [0u8; KEY_LEN];
        for (i, b) in k.iter_mut().enumerate() {
            *b = i as u8;
        }
        k
    }

    #[test]
    fn round_trips_and_refuses_a_flipped_bit() {
        let k = key();
        let pkt = encode(OP_DATA, 7, 9, 6501, 42, b"payload", &k).unwrap();
        let got = decode(&pkt, &k).unwrap();
        assert_eq!(
            (got.op, got.src, got.dst, got.room, got.seq),
            (OP_DATA, 7, 9, 6501, 42)
        );
        assert_eq!(got.payload, b"payload");

        for i in 0..pkt.len() {
            let mut bad = pkt.clone();
            bad[i] ^= 0x01;
            // A flip in byte 0 or 1 is caught before the MAC; everywhere else it is the MAC. Either
            // way the datagram is refused -- which is the claim, not which arm caught it.
            assert!(
                decode(&bad, &k).is_err(),
                "byte {i} flipped and still decoded"
            );
        }
    }

    #[test]
    fn a_wrong_key_is_bad_mac_and_not_something_vaguer() {
        let k = key();
        let mut other = key();
        other[0] ^= 0xff;
        let pkt = encode(OP_PING, 1, 0, 1, 1, &[], &k).unwrap();
        assert_eq!(decode(&pkt, &other), Err(LegError::BadMac));
    }

    #[test]
    fn the_size_boundary_is_pinned_from_both_sides() {
        let k = key();
        let ok = encode(
            OP_DATA,
            1,
            2,
            0,
            0,
            &vec![0u8; crate::wire::MAX_DATAGRAM],
            &k,
        )
        .unwrap();
        assert_eq!(ok.len(), LEG_MAX);
        assert!(decode(&ok, &k).is_ok());
        assert!(encode(
            OP_DATA,
            1,
            2,
            0,
            0,
            &vec![0u8; crate::wire::MAX_DATAGRAM + 1],
            &k
        )
        .is_none());
        let mut too_long = ok.clone();
        too_long.push(0);
        assert_eq!(decode(&too_long, &k), Err(LegError::TooLong));
        assert_eq!(decode(&ok[..LEG_MIN - 1], &k), Err(LegError::TooShort));
    }

    #[test]
    fn hello_and_welcome_round_trip() {
        let mid = [0xabu8; 16];
        let (role, flags, got) = hello_parse(&hello_payload(ROLE_CLIENT, &mid)).unwrap();
        assert_eq!(role, ROLE_CLIENT);
        assert_eq!(flags, 0, "the plain payload still advertises nothing");
        assert_eq!(got, mid);
        // mp:R1c: the reserved byte round-trips, including a bit this build does not know.
        let (_, f2, _) = hello_parse(&hello_payload_flags(
            ROLE_HOST,
            HELLO_FLAG_PEER_KEY | 0x80,
            &mid,
        ))
        .unwrap();
        assert_eq!(f2, HELLO_FLAG_PEER_KEY | 0x80);
        assert_eq!(welcome_parse(&welcome_payload(3, 1)).unwrap(), (3, 1));
        assert_eq!(hello_parse(&[0u8; 4]), Err(LegError::Malformed));
    }

    /// mp:R4a -- the level rides in HELLO's flags bits 1..7 and in a fifth WELCOME byte, and BOTH
    /// are optional in the direction that has to stay compatible: a HELLO without one (a pre-R4a
    /// peer) reads as level 0 and still carries its re-key bit; a WELCOME without one (a pre-R4a
    /// relay) reads as level 0 and still names both handles.
    #[test]
    fn the_protocol_level_is_additive_in_both_directions() {
        let mid = [0x5au8; 16];
        // A pre-R4a HELLO: flags = the re-key bit alone, level 0.
        let (_, f0, _) =
            hello_parse(&hello_payload_flags(ROLE_CLIENT, HELLO_FLAG_PEER_KEY, &mid)).unwrap();
        assert_eq!(hello_level(f0), 0);
        assert_ne!(f0 & HELLO_FLAG_PEER_KEY, 0);
        // An R4a HELLO: the same byte with the level above the re-key bit -- 18 bytes still, so a
        // pre-R4a relay's exact-length parse accepts it and reads only bit 0.
        let f = hello_flags_with_level(HELLO_FLAG_PEER_KEY, PROTOCOL_LEVEL);
        let p = hello_payload_flags(ROLE_HOST, f, &mid);
        assert_eq!(p.len(), HELLO_LEN);
        let (_, f1, _) = hello_parse(&p).unwrap();
        assert_eq!(hello_level(f1), PROTOCOL_LEVEL);
        assert_ne!(
            f1 & HELLO_FLAG_PEER_KEY,
            0,
            "the level does not displace the re-key bit"
        );
        assert_eq!(
            hello_level(hello_flags_with_level(0, 0xff)),
            PROTOCOL_LEVEL_MAX
        );
        assert_eq!(hello_flags_with_level(0xff, 0) & !HELLO_FLAG_PEER_KEY, 0);
        // WELCOME: the 4-byte form is level 0, the 5-byte form carries it, a longer one is read
        // for its first five bytes, a shorter one is malformed.
        assert_eq!(
            welcome_parse_level(&welcome_payload(3, 1)).unwrap(),
            (3, 1, 0)
        );
        let w = welcome_payload_level(3, 1, PROTOCOL_LEVEL);
        assert_eq!(w.len(), WELCOME_LEN_LEVEL);
        assert_eq!(welcome_parse_level(&w).unwrap(), (3, 1, PROTOCOL_LEVEL));
        assert_eq!(
            welcome_parse(&w).unwrap(),
            (3, 1),
            "the old reader still gets the handles"
        );
        let mut longer = w.clone();
        longer.push(0xee);
        assert_eq!(
            welcome_parse_level(&longer).unwrap(),
            (3, 1, PROTOCOL_LEVEL)
        );
        assert_eq!(welcome_parse_level(&w[..3]), Err(LegError::Malformed));
    }

    #[test]
    fn the_leg_key_is_not_any_of_the_boot_keys() {
        // One PSK, four uses, four labels: the property worth a test is that no two of them are
        // the same bytes, because a copy-paste in the label strings would not show up any other way.
        let s = Secrets::derive(&key());
        assert_ne!(s.leg, s.boot_enc);
        assert_ne!(s.leg, s.boot_mac);
        assert_ne!(s.boot_enc, s.boot_mac);
        assert_ne!(&s.boot_conn[..], &s.leg[..8]);
    }

    /// mp:R1c -- THE CROSS-LANGUAGE FIXTURE for the per-peer leg key.
    ///
    /// `udp_wire_selftest.cpp` pins the same two hex strings for the same two inputs. The relay and
    /// the peer derive this key independently and never exchange it, so a change in either label or
    /// either construction is a link that simply stops verifying -- with no message anywhere saying
    /// why. A round-trip test inside one implementation cannot see that; a pinned vector on both
    /// sides can.
    #[test]
    fn the_per_peer_leg_key_matches_the_cpp_side() {
        let mut mac_c2s = [0u8; KEY_LEN];
        let mut mac_s2c = [0u8; KEY_LEN];
        for i in 0..KEY_LEN {
            mac_c2s[i] = i as u8;
            mac_s2c[i] = (i as u8).wrapping_mul(7).wrapping_add(3);
        }
        let hex = |b: &[u8]| b.iter().map(|x| format!("{x:02x}")).collect::<String>();
        assert_eq!(
            hex(&peer_leg_key(&mac_c2s, true)),
            "ead16d7f41a7407ea694ca092468c24f4993189a7e621f598051442289ad6f34"
        );
        assert_eq!(
            hex(&peer_leg_key(&mac_s2c, false)),
            "d716d119872ec8f7e03a0462d06a2fc01d94a0718db3eb612566bd4431cbed85"
        );
        // The two labels are not interchangeable over one piece of material: if they were, the two
        // halves of a pair could replay each other's datagrams back.
        assert_ne!(peer_leg_key(&mac_c2s, true), peer_leg_key(&mac_c2s, false));
        // And neither is the deployment key, whatever PSK produced it.
        let s = Secrets::derive(&key());
        assert_ne!(peer_leg_key(&mac_c2s, true), s.leg);
        assert_ne!(peer_leg_key(&mac_s2c, false), s.leg);
    }

    #[test]
    fn a_directory_page_round_trips_and_says_how_much_it_left_out() {
        // Three descriptors at the DESC_MAX worst case: two fit one page and the third does not,
        // which is the whole reason the payload carries `total` and `offset` at all.
        let big = vec![0xcdu8; DESC_MAX];
        let entries: Vec<(u32, Vec<u8>)> = vec![
            (6501, big.clone()),
            (6502, big.clone()),
            (6503, big.clone()),
        ];
        let (page, used) = sessions_encode(&entries, 3, 0);
        assert_eq!(used, 2, "two DESC_MAX descriptors fill one page");
        assert!(page.len() <= crate::wire::MAX_DATAGRAM);
        let (total, offset, got) = sessions_parse(&page).unwrap();
        assert_eq!((total, offset, got.len()), (3, 0, 2));
        assert_eq!(got[0], (6501, big.clone()));
        assert_eq!(got[1], (6502, big));

        let (empty, used0) = sessions_encode(&[], 0, 0);
        assert_eq!(used0, 0);
        assert_eq!(sessions_parse(&empty).unwrap(), (0, 0, Vec::new()));
    }

    #[test]
    fn a_truncated_directory_page_is_malformed_and_not_a_short_list() {
        let (page, _) = sessions_encode(&[(7, b"hello".to_vec())], 1, 0);
        for cut in 0..page.len() {
            assert_eq!(
                sessions_parse(&page[..cut]),
                Err(LegError::Malformed),
                "a page cut at {cut} parsed as a shorter list"
            );
        }
        assert!(sessions_parse(&page).is_ok());
    }

    #[test]
    fn the_directory_ops_encode_and_the_one_above_them_does_not() {
        let k = key();
        for op in [OP_REGISTER, OP_UNREGISTER, OP_LIST, OP_SESSIONS] {
            let pkt = encode(op, 1, 0, 42, 7, b"x", &k).expect("a directory op encodes");
            assert_eq!(decode(&pkt, &k).unwrap().op, op);
        }
        // The ceiling is a real refusal on both sides, so an op added to one half of the contract
        // and not the other is a counted `bad_op` rather than a silent no-op.
        assert!(encode(OP_MAX + 1, 1, 0, 42, 7, b"x", &k).is_none());
        let mut bad = encode(OP_LIST, 1, 0, 42, 7, b"x", &k).unwrap();
        bad[1] = OP_MAX + 1;
        assert_eq!(decode(&bad, &k), Err(LegError::BadOp));
    }

    #[test]
    fn a_candidate_list_round_trips_in_both_families() {
        let list = vec![
            ("192.168.0.61:6501".parse::<SocketAddr>().unwrap(), 0u8),
            ("[fe80::1]:6501".parse::<SocketAddr>().unwrap(), 0u8),
            (
                "[2001:db8::dead:beef]:1".parse::<SocketAddr>().unwrap(),
                0u8,
            ),
            (
                "203.0.113.9:54321".parse::<SocketAddr>().unwrap(),
                CAND_OBSERVED,
            ),
        ];
        assert_eq!(cands_parse(&cands_encode(&list)).unwrap(), list);
        // The family is a real tag on the wire, not a guess from the entry's length.
        let v4 = cands_encode(&list[..1]);
        assert_eq!((v4[1], v4.len()), (CAND_FAM_V4, 1 + 4 + 4));
        let v6 = cands_encode(&list[1..2]);
        assert_eq!((v6[1], v6.len()), (CAND_FAM_V6, 1 + 4 + 16));
    }

    #[test]
    fn a_truncated_or_overlong_candidate_list_is_malformed_and_not_a_shorter_one() {
        let list: Vec<(SocketAddr, u8)> = (0..MAX_CANDS)
            .map(|i| {
                (
                    SocketAddr::new(Ipv4Addr::new(10, 0, 0, i as u8).into(), 1 + i as u16),
                    0u8,
                )
            })
            .collect();
        let pkt = cands_encode(&list);
        assert_eq!(cands_parse(&pkt).unwrap().len(), MAX_CANDS);
        for cut in 0..pkt.len() {
            assert_eq!(
                cands_parse(&pkt[..cut]),
                Err(LegError::Malformed),
                "a candidate list cut at {cut} parsed as a shorter list"
            );
        }
        let mut trailing = pkt.clone();
        trailing.push(0);
        assert_eq!(cands_parse(&trailing), Err(LegError::Malformed));
        // The count is capped on BOTH sides: encode drops the overflow, parse refuses the claim.
        let too_many: Vec<(SocketAddr, u8)> = (0..MAX_CANDS + 3)
            .map(|i| {
                (
                    SocketAddr::new(Ipv4Addr::new(10, 0, 0, i as u8).into(), 1),
                    0u8,
                )
            })
            .collect();
        assert_eq!(cands_encode(&too_many)[0] as usize, MAX_CANDS);
        let mut lying = pkt.clone();
        lying[0] = (MAX_CANDS + 1) as u8;
        assert_eq!(cands_parse(&lying), Err(LegError::Malformed));
        // An EMPTY payload is malformed; an empty LIST is a legal answer ("nothing to offer").
        assert_eq!(cands_parse(&[]), Err(LegError::Malformed));
        assert_eq!(cands_parse(&cands_encode(&[])).unwrap(), Vec::new());
    }

    #[test]
    fn an_unknown_address_family_is_refused_rather_than_skipped() {
        let mut pkt = cands_encode(&[("192.0.2.4:5".parse().unwrap(), 0u8)]);
        pkt[1] = 7; // neither 4 nor 6
        assert_eq!(cands_parse(&pkt), Err(LegError::Malformed));
    }

    #[test]
    fn the_punch_ops_are_inside_the_op_space_and_the_one_above_them_is_not() {
        let k = key();
        for op in [OP_CAND, OP_PROBE, OP_PROBE_ACK] {
            let pkt = encode(op, 1, 2, 42, 7, b"x", &k).expect("a punch op encodes");
            assert_eq!(decode(&pkt, &k).unwrap().op, op);
        }
        assert_eq!(OP_MAX, OP_PROBE_ACK);
        assert!(encode(OP_MAX + 1, 1, 0, 42, 7, b"x", &k).is_none());
    }
}
