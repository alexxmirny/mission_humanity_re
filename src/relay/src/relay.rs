//! The relay itself: rooms, peers, sessions, and the decision of what may be forwarded (mp:R1).
//!
//! # A pure core, on purpose
//!
//! [`Relay::on_datagram`] takes bytes and a source address and returns the datagrams to send. It
//! owns no socket, no clock and no task. Everything interesting about a relay -- who is registered,
//! which connection a packet belongs to, whether an unvalidated address has already been sent three
//! times what it sent us -- is therefore testable without binding a port, and the tokio loop in
//! `main.rs` is the small part that cannot be. The alternative (state reachable only through a live
//! socket) is how a relay ends up with an anti-amplification cap nobody has ever seen trigger.
//!
//! # The three layers, and which one each rule belongs to
//!
//! 1. **The leg** (`leg.rs`) -- peer↔relay. Authenticated with the deployment's PSK. Gives every
//!    peer a stable handle, so routing survives a NAT rebinding, and gives the relay a cheap way to
//!    throw away a scanner's datagram.
//! 2. **The room** -- the rendezvous. One host claims a room code; clients joining that code are
//!    paired with it. R1 made the code `[net] port` (two peers that already agree on a port
//!    already agree on a room); R2 has a client re-dial the room the directory names, and R6 has
//!    the HOST mint a random 30-bit code per lobby, so two hosts with the same port never collide
//!    on a shared relay. Nothing here changed for either: a room is a `u32` this file never
//!    interprets.
//! 3. **The session** -- one T0 connection, keyed by the 8-byte `conn_id`. The relay learns a
//!    connection by opening the CONNECT TOKEN the host grants, exactly as `docs/mp-wire-udp.md`
//!    says a relay does. From the token it keeps the two MAC keys and **deliberately discards both
//!    encryption keys**: it can then prove every forwarded datagram is the registered peer's and
//!    still be unable to read one byte of the match.
//!
//! # Two rules that are weaker than they could be, stated rather than hidden
//!
//! * **An unknown `conn_id` between two registered peers is FORWARDED and counted**
//!   (`inner_unverified`), not dropped. The relay normally sees the whole handshake and learns
//!   every connection, so the counter should sit at a handful per session; a relay restarted
//!   mid-match, though, would otherwise black-hole a game it could simply carry. The endpoints'
//!   own crypto is the security boundary here -- the relay's check is defence in depth. Run with
//!   `--strict` to drop instead, which is what a public deployment should do once R2 gives the
//!   relay a registration it can require.
//! * **The leg key is per-DEPLOYMENT, not per-peer**, so any peer holding the PSK can forge
//!   another's handle. That is the same boundary the game's PSK already draws. A per-peer leg key
//!   derived from the connect token is R2's.

use std::collections::HashMap;
use std::net::SocketAddr;
use std::time::{Duration, Instant};

use crate::leg;
use crate::wire::{self, ReplayWindow, KEY_LEN};

/// The channel id `udp_endpoint.cpp` carries its PSK handshake on, and the frame kind that holds
/// the minted connect token. NOT part of T0 (which is why T0's own mux rule says an unknown id is
/// skipped) -- mirrored here because opening the grant is how the relay learns a session's keys.
/// A change on either side shows up as `tokens_learned` stuck at zero, so it is worth a look there
/// before anywhere else.
const CH_HS: u8 = 0x40;
const HSK_GRANT: u8 = 3;

/// Per-room peer cap: the host plus MH_NET_MAX_PEERS clients. A room that has filled is a refusal
/// with a code, never a silent drop.
pub const ROOM_PEER_CAP: usize = 9;

/// mp:R2 -- THE DIRECTORY ROOM. A peer registered here is browsing, not playing.
///
/// A client has to be REGISTERED before it may [`leg::OP_LIST`], and until R2 the only way to
/// register was to name a room that already had a host -- which is exactly the knowledge a browser
/// does not have yet. (It is also `mp:R1e`'s footgun wearing its other face: two peers who
/// disagree about `[net] port` disagree about the room, and the client was refused `no_host` with
/// no way to discover the right code.) So room 0 admits any client, holds no host, and forwards
/// nothing: it is a registration that buys LIST and PING and nothing else. Zero is free by
/// construction -- a host's room is minted non-zero (mp:R6; before that it was `[net] port`, which
/// is never 0 either), and a client's is a host's room the directory named.
pub const DIRECTORY_ROOM: u32 = 0;

/// Browsers are cheap (one registration, a LIST every couple of seconds) and many, so the
/// directory room's cap is its own rather than a game room's nine.
const DIRECTORY_PEER_CAP: usize = 64;

/// How many unvalidated addresses the amplification ledger remembers. Bounded because the ledger
/// is indexed by a value an attacker chooses: an unbounded map keyed on source address IS the
/// amplification attack, one level up.
const UNVALIDATED_CAP: usize = 4096;

/// mp:R2 -- how many SESSIONS pages one LIST may be answered with.
///
/// This is the relay's LARGEST amplification surface: a 34-byte LIST buys up to this many
/// ~1234-byte datagrams. Two things bound it. The leg tag must verify, so the asker holds the
/// deployment PSK -- the same boundary that admits a peer to a match at all; and a peer that has
/// not yet validated its address is still held to the 3x cap by [`Relay::may_send`], which is what
/// makes a spoofed source address buy nothing. Three pages carry at least six full-sized
/// descriptors, and a directory larger than that is served over successive LISTs via the per-peer
/// cursor rather than by raising this number.
const LIST_MAX_PAGES: usize = 3;

/// Anti-amplification: until an address has answered us once, we may send it at most this multiple
/// of what it has sent us. Three is the figure plan D4 names and the one RFC 9000 §8 picked for
/// QUIC, for the same reason -- it is enough for a handshake reply and far too little to be worth
/// spoofing a source address for.
pub const AMPLIFICATION_FACTOR: u64 = 3;

#[derive(Debug, Clone, Copy)]
pub struct Limits {
    /// No leg traffic from a peer for this long and it is evicted.
    pub idle: Duration,
    /// Silence for this long and the relay pings the peer itself. Plan D4's keepalive floor is
    /// 20-25 s; the peer pings at 20 and the relay at 25, so a NAT binding is refreshed by
    /// whichever end is still alive.
    pub ping_after: Duration,
    /// Drop a datagram whose inner `conn_id` the relay has not learned, instead of forwarding it.
    pub strict: bool,
    /// mp:R2 -- a registered session descriptor not refreshed within this long is forgotten.
    ///
    /// SHORTER THAN [`Limits::idle`] ON PURPOSE. A peer is evicted after a minute of total
    /// silence, but a LOBBY that has gone away has to stop being listed long before that: the
    /// acceptance clause is a browsing player watching a row disappear. The host refreshes at
    /// ~1 Hz while it sits in its lobby, so 20 s is twenty missed refreshes -- far outside
    /// ordinary loss and still a wait a person will sit through. The ORDINARY path is not this
    /// timer at all: a host leaving its lobby sends UNREGISTER and the row goes at once; the TTL
    /// is what covers a host that crashed or was firewalled off mid-lobby.
    pub session_ttl: Duration,
    /// mp:R4a -- the PROTOCOL LEVEL this relay claims in its WELCOME. Always
    /// [`leg::PROTOCOL_LEVEL`] in service; the only reason it is a knob (`--advertise-level`) is
    /// so a scenario can stage an OLDER relay -- one that answers a lower level, or with `0` the
    /// 4-byte WELCOME a pre-R4a build sends -- against a current peer and prove the peer says so.
    /// It changes what is CLAIMED, never what is spoken: the op table is the same whatever it says.
    pub level: u8,
}

impl Default for Limits {
    fn default() -> Self {
        Limits {
            idle: Duration::from_secs(60),
            ping_after: Duration::from_secs(25),
            strict: false,
            session_ttl: Duration::from_secs(20),
            level: leg::PROTOCOL_LEVEL,
        }
    }
}

#[derive(Debug, Default, Clone, Copy)]
pub struct Counters {
    pub leg_rx: u64,
    pub leg_tx: u64,
    pub bytes_rx: u64,
    pub bytes_tx: u64,
    /// mp:R1d -- THE LARGEST LEG DATAGRAM THIS RELAY HAS TOUCHED, in either direction.
    ///
    /// The high-water mark, not an average, because the claim it exists to check is a CEILING: a
    /// relayed datagram must stay inside the 1200 bytes RFC 9000 picked as the size that crosses
    /// the internet without PMTU discovery, and one datagram over it is the whole failure -- an
    /// occasional black hole on exactly the networks least able to diagnose one. A mean would hide
    /// it perfectly. It counts BOTH directions and every op, including the ones the relay generates
    /// itself (a three-page SESSIONS answer is the relay's own largest emission), so the number is
    /// "the biggest thing that was on this wire" and not "the biggest thing a peer sent".
    ///
    /// Note what it is NOT: evidence about a PROMOTED pair. A direct datagram never reaches the
    /// relay, so a run that promotes early leaves this describing the handshake. `relay_match` and
    /// `relay_browse` are pinned to `force_relay=1` for that reason, and the R1d measurement is
    /// taken on a relayed run for the same one.
    pub max_seen: u64,
    /// A leg datagram that did not decode: wrong magic, wrong version, an op we do not have, or a
    /// tag that did not verify. One counter per reason, because "refused" alone cannot tell a scan
    /// from a version skew.
    pub leg_too_short: u64,
    pub leg_too_long: u64,
    pub leg_bad_magic: u64,
    pub leg_bad_version: u64,
    pub leg_bad_op: u64,
    pub leg_bad_mac: u64,
    pub leg_malformed: u64,
    /// A well-formed leg datagram naming a handle the relay does not know. THE counter the R1
    /// acceptance clause names ("a packet from an unregistered address is dropped and counted").
    pub unregistered: u64,
    pub replay: u64,
    /// A send refused because the destination address has not yet proved it can receive.
    pub amplification_capped: u64,
    pub forwarded: u64,
    pub inner_malformed: u64,
    /// A datagram on a LEARNED connection whose T0 tag did not verify. Dropped, never forwarded.
    pub inner_bad_mac: u64,
    /// A datagram on a connection the relay has not learned. Forwarded unless `--strict`.
    pub inner_unverified: u64,
    /// The two ends named by the leg header are not the two ends of that connection.
    pub not_in_peer_set: u64,
    pub boot_refused: u64,
    pub tokens_learned: u64,
    pub peers_registered: u64,
    pub peers_evicted: u64,
    pub rooms_opened: u64,
    pub rooms_closed: u64,
    pub errors_sent: u64,
    // ---- mp:R2, the session directory ----------------------------------------------------------
    /// A descriptor accepted (a fresh one; a refresh of the same room is not counted again).
    pub sessions_registered: u64,
    /// A descriptor withdrawn by its host's UNREGISTER.
    pub sessions_unregistered: u64,
    /// A descriptor forgotten because it was not refreshed inside [`Limits::session_ttl`].
    pub sessions_expired: u64,
    /// A REGISTER refused: not this room's host, or a descriptor over [`leg::DESC_MAX`].
    pub register_refused: u64,
    pub list_requests: u64,
    /// Directory ENTRIES put on the wire (not pages) -- the number a peer's row count should match.
    pub list_entries_sent: u64,
    // ---- mp:R3, hole punching ------------------------------------------------------------------
    /// A candidate list carried to a counterpart. The number that says punching was ATTEMPTED at
    /// all -- a pair that never promotes with this at zero never exchanged candidates, which is a
    /// different fault from a pair that exchanged them and could not reach each other.
    pub cands_forwarded: u64,
    /// A candidate list dropped: malformed, or from a peer with no counterpart in its room yet
    /// (the ordinary race -- the host publishes before a client has HELLOed).
    pub cands_refused: u64,
    /// A peer-to-peer probe op that arrived AT THE RELAY. Never zero-by-design: a peer punches
    /// every candidate it is given, and one of them can be the relay's own address. Counted rather
    /// than forwarded, because a probe the relay carried would validate the relayed path and
    /// promote a pair onto a "direct" route that is the relay.
    pub probes_misdirected: u64,
    // ---- mp:R1c, the per-peer leg key ----------------------------------------------------------
    /// A handle that moved off the deployment key onto a key derived from its connect token. Two
    /// per learned session (the host end and the client end), so on a healthy two-peer match it
    /// reaches 2 and stops. ZERO ON A RUN THAT CLAIMS TO HAVE RE-KEYED IS THE RED: it means every
    /// leg is still on the shared key and `leg_bad_mac` below is refusing nothing it would not have
    /// refused at R1.
    pub peers_rekeyed: u64,
    // ---- mp:R4b, surviving a relay restart -----------------------------------------------------
    /// A datagram that FAILED THE MAC while naming a handle this relay does not hold, answered
    /// with `ERR_NOT_REGISTERED` under the deployment key (at most once per address per
    /// [`STALE_HANDLE_REPLY`]). This is what a re-keyed peer looks like to a relay that has just
    /// restarted: it tags under a session key the new process never learned, so without this reply
    /// nothing it sends can ever verify and nothing tells it to re-HELLO -- both peers then drop the
    /// link at the endpoint's 10 s silence timeout (dead-ends G245). Counted separately from
    /// `leg_bad_mac` because a forgery against a handle we DO hold is still refused silently.
    pub stale_handle_replies: u64,
    /// dist:RP5 -- liveness PINGs from `HANDLE_NONE` (the container healthcheck), answered with a
    /// PONG and nothing else. Steady growth is the healthcheck running; zero on a deployed relay
    /// means the compose healthcheck is not wired.
    pub health_pings: u64,
    /// A HELLO that named a handle we did not hold and got THAT handle back. A peer keeps its
    /// handle across our restart so its counterpart's tables (pair keys, punch state, the host's
    /// per-client loopback socket) stay valid; a collision falls through to a fresh allocation and
    /// the peer rebuilds. Two on a two-peer match that outlived one restart.
    pub handles_restored: u64,
    // ---- mp:R4a, the protocol level ------------------------------------------------------------
    /// A HELLO whose protocol level is not this relay's -- including a HELLO that carries NONE
    /// (level 0: a pre-R4a peer). Not a refusal: the peer is registered and served whatever it
    /// says, because the level describes what will be REFUSED LATER (an op one side lacks), not
    /// whether the link can come up. The number an operator reads to learn that the players and
    /// the relay are on different builds -- the peers say the same in their own logs (`relay
    /// protocol <theirs> < <ours>` in mh_net.log) when it is the relay that is behind.
    pub hello_level_mismatch: u64,
}

/// One datagram the caller must put on the wire.
#[derive(Debug, Clone)]
pub struct Out {
    pub to: SocketAddr,
    pub bytes: Vec<u8>,
}

/// Something worth a log line, produced by the core and rendered by `main.rs` so the core stays
/// free of a logging framework (and so a test can assert on events rather than on captured text).
#[derive(Debug, Clone)]
pub enum Event {
    PeerRegistered {
        handle: u16,
        room: u32,
        role: u8,
        addr: SocketAddr,
        match_id: String,
        /// mp:R4a -- the protocol level the peer's HELLO carried; 0 = none (a pre-R4a build).
        level: u8,
    },
    PeerRebound {
        handle: u16,
        room: u32,
        from: SocketAddr,
        to: SocketAddr,
    },
    /// A HELLO update named a room different from the peer's current one: it left the old room
    /// (whose count drops, and which closes if that empties it) and joined the new one.
    Rehomed {
        handle: u16,
        from_room: u32,
        to_room: u32,
    },
    MatchId {
        handle: u16,
        room: u32,
        match_id: String,
    },
    SessionLearned {
        room: u32,
        conn: String,
        slot: u8,
        host: u16,
        client: u16,
        scope: String,
    },
    PeerGone {
        handle: u16,
        room: u32,
        why: &'static str,
    },
    RoomClosed {
        room: u32,
    },
    Refused {
        addr: SocketAddr,
        why: &'static str,
    },
    /// mp:R2 -- a host published (or refreshed) the descriptor for the room it holds. `bytes` and
    /// not the descriptor's content, because the relay cannot read one and should not pretend to:
    /// what a room's lobby is CALLED belongs to the peers, and the operator-visible identity of a
    /// match here has always been `room` + `match_id`.
    SessionRegistered {
        room: u32,
        handle: u16,
        bytes: usize,
        fresh: bool,
    },
    SessionUnregistered {
        room: u32,
        why: &'static str,
    },
    /// mp:R3 -- a candidate list carried from one peer of a room to the other. `observed` is the
    /// address the sender was seen from, which is the field an operator reading a pair that never
    /// went direct actually wants: it says whether the two peers were told about each other at all.
    Candidates {
        from: u16,
        to: u16,
        room: u32,
        n: usize,
        observed: SocketAddr,
    },
    /// mp:R1c -- this handle's leg moved off the deployment key onto one derived from its own
    /// connect token. Worth a line rather than only a counter because it is the moment the trust
    /// boundary narrows for that peer, and an operator reading a relay that is refusing a peer
    /// wants to know whether the peer was re-keyed and when.
    LegRekeyed {
        handle: u16,
        room: u32,
    },
}

struct Peer {
    handle: u16,
    addr: SocketAddr,
    role: u8,
    room: u32,
    match_id: [u8; 16],
    win: ReplayWindow,
    tx_seq: u64,
    /// The address has sent us something AFTER we answered it, so it is not a spoofed source.
    validated: bool,
    last_rx: Instant,
    last_ping: Instant,
    /// mp:R2 -- where this peer's next LIST starts reading the directory. A directory bigger than
    /// [`LIST_MAX_PAGES`] can carry is served across successive LISTs instead of being silently
    /// truncated to the same first N rooms forever.
    list_cursor: usize,
    /// mp:R1c -- this peer advertised [`leg::HELLO_FLAG_PEER_KEY`] and can therefore be re-keyed.
    can_rekey: bool,
    /// mp:R1c -- THIS PEER'S OWN LEG KEYS, one per connection it is an end of. Empty means it is
    /// still on the deployment key, which is every peer until its first token is learned and every
    /// peer of an older build forever.
    ///
    /// A VEC RATHER THAN ONE KEY because a HOST is an end of one connection per client, and its
    /// relay-directed traffic (PING, REGISTER, LIST, BYE) names no pair at all -- it has to be
    /// taggable under something, and "any key this handle owns" is the rule that makes one sender
    /// and N pairs agree without a negotiation. It costs at most [`ROOM_PEER_CAP`] HMACs on a miss
    /// and one on a hit, since the winner is moved to the front.
    ///
    /// NON-EMPTY IS A RATCHET: the deployment key is no longer accepted from this handle. That is
    /// the whole security property -- a PSK holder can still forge a handle that has never had a
    /// session, and cannot forge one that has.
    keys: Vec<[u8; KEY_LEN]>,
    /// mp:R1c -- this peer has SENT something under one of `keys`, so it has demonstrably made the
    /// switch and the deployment key can stop being accepted from it.
    ///
    /// The gap between installing a key and this being true is real and short: the relay learns a
    /// token while FORWARDING the grant, and the client it is for cannot have the token until that
    /// forward lands. Refusing the deployment key in that window would cost a handshake
    /// retransmit per peer and put a `leg_bad_mac` on the counters of every clean run -- which is
    /// the wrong trade for a window an off-path forger cannot aim at anyway (it ends on the peer's
    /// next datagram). After it, the shared key is refused, which is the property the item is for.
    keys_proved: bool,
}

/// mp:R2 -- what a host published about the lobby it is holding, held verbatim.
struct Desc {
    bytes: Vec<u8>,
    /// The host handle that published it: a descriptor outlives neither its publisher nor the
    /// room, and a later peer handed the same room must not inherit the old lobby's row.
    host: u16,
    at: Instant,
}

#[derive(Clone)]
struct Conn {
    host: u16,
    client: u16,
    mac_c2s: [u8; KEY_LEN],
    mac_s2c: [u8; KEY_LEN],
}

#[derive(Default)]
struct Room {
    host: Option<u16>,
    clients: Vec<u16>,
    conns: HashMap<[u8; 8], Conn>,
    /// mp:R2 -- the lobby descriptor this room's host published, if it has published one.
    desc: Option<Desc>,
}

#[derive(Default, Clone, Copy)]
struct Budget {
    rx: u64,
    tx: u64,
    seen: u64, // a monotone stamp, so the cheapest eviction is "the least recently seen"
    /// mp:R4b -- when this address was last told `NOT_REGISTERED` for a stale-handle datagram.
    stale_reply_at: Option<Instant>,
}

/// mp:R4b -- how often ONE address is told "I do not know that handle" for datagrams that fail the
/// MAC under a handle we do not hold. A re-keyed peer sends at the lockstep rate (~30/s) until it
/// re-HELLOs, and it re-HELLOs on the first reply it reads, so one reply per half second per address
/// is plenty; the rest of its traffic is still a counted `leg_bad_mac`. The reply is ~35 bytes
/// against a >= 34-byte datagram and goes through the amplification ledger like everything else.
const STALE_HANDLE_REPLY: Duration = Duration::from_millis(500);

pub struct Relay {
    psk: [u8; KEY_LEN],
    secrets: leg::Secrets,
    limits: Limits,
    peers: HashMap<u16, Peer>,
    by_addr: HashMap<SocketAddr, u16>,
    rooms: HashMap<u32, Room>,
    unvalidated: HashMap<SocketAddr, Budget>,
    tick: u64,
    next_handle: u16,
    pub counters: Counters,
}

fn hex(b: &[u8]) -> String {
    let mut s = String::with_capacity(b.len() * 2);
    for x in b {
        s.push_str(&format!("{x:02x}"));
    }
    s
}

/// All-zero means "this peer has no match id yet" -- the same nil sentinel `uuid7_is_nil()` uses,
/// and the reason a peer re-sends HELLO once mh.dll has minted one.
fn is_nil(id: &[u8; 16]) -> bool {
    id.iter().all(|&b| b == 0)
}

impl Relay {
    pub fn new(psk: [u8; KEY_LEN], limits: Limits) -> Relay {
        Relay {
            secrets: leg::Secrets::derive(&psk),
            psk,
            limits,
            peers: HashMap::new(),
            by_addr: HashMap::new(),
            rooms: HashMap::new(),
            unvalidated: HashMap::new(),
            tick: 0,
            next_handle: 1,
            counters: Counters::default(),
        }
    }

    pub fn peer_count(&self) -> usize {
        self.peers.len()
    }
    /// mp:R4a -- the protocol level this relay claims in its WELCOME (`Limits::level`).
    pub fn level(&self) -> u8 {
        self.limits.level
    }
    pub fn room_count(&self) -> usize {
        self.rooms.len()
    }

    // ---- the amplification ledger ---------------------------------------------------------------

    fn note_rx(&mut self, addr: SocketAddr, n: usize) {
        self.tick += 1;
        if let Some(b) = self.unvalidated.get_mut(&addr) {
            b.rx += n as u64;
            b.seen = self.tick;
            return;
        }
        if self.unvalidated.len() >= UNVALIDATED_CAP {
            // Evict the least recently seen. An attacker can churn addresses to push a real peer's
            // budget out, and the cost of that is one extra round trip for that peer -- which is
            // strictly better than an unbounded map, and better than refusing new addresses.
            if let Some(&victim) = self
                .unvalidated
                .iter()
                .min_by_key(|(_, b)| b.seen)
                .map(|(a, _)| a)
            {
                self.unvalidated.remove(&victim);
            }
        }
        let t = self.tick;
        self.unvalidated.insert(
            addr,
            Budget {
                rx: n as u64,
                tx: 0,
                seen: t,
                stale_reply_at: None,
            },
        );
    }

    /// May we send `n` bytes to `addr` right now? Validated addresses are unconditional; everyone
    /// else is held to [`AMPLIFICATION_FACTOR`] times what they have sent us.
    fn may_send(&mut self, addr: SocketAddr, n: usize) -> bool {
        let validated = self
            .by_addr
            .get(&addr)
            .and_then(|h| self.peers.get(h))
            .map(|p| p.validated)
            .unwrap_or(false);
        if validated {
            return true;
        }
        let b = self.unvalidated.entry(addr).or_default();
        if b.tx + n as u64 > AMPLIFICATION_FACTOR * b.rx {
            return false;
        }
        b.tx += n as u64;
        true
    }

    /// mp:R4b -- may this address be told NOT_REGISTERED for a stale-handle datagram right now?
    /// Rides the unvalidated ledger, which `note_rx` has already given this address an entry in
    /// (an address we hold a validated peer for never reaches here: its handle is known).
    fn stale_reply_due(&mut self, addr: SocketAddr, now: Instant) -> bool {
        let b = self.unvalidated.entry(addr).or_default();
        if b.stale_reply_at
            .is_some_and(|t| now.duration_since(t) < STALE_HANDLE_REPLY)
        {
            return false;
        }
        b.stale_reply_at = Some(now);
        true
    }

    fn emit(&mut self, out: &mut Vec<Out>, to: SocketAddr, bytes: Vec<u8>) {
        if !self.may_send(to, bytes.len()) {
            self.counters.amplification_capped += 1;
            return;
        }
        self.counters.leg_tx += 1;
        self.counters.bytes_tx += bytes.len() as u64;
        if bytes.len() as u64 > self.counters.max_seen {
            self.counters.max_seen = bytes.len() as u64;
        }
        out.push(Out { to, bytes });
    }

    fn send_leg(
        &mut self,
        out: &mut Vec<Out>,
        to_handle: u16,
        op: u8,
        src: u16,
        payload: &[u8],
    ) -> bool {
        let Some(p) = self.peers.get_mut(&to_handle) else {
            return false;
        };
        let (addr, room, seq) = (p.addr, p.room, p.tx_seq);
        p.tx_seq += 1;
        // mp:R1c -- OUTBOUND IS TAGGED UNDER THE KEY OF THE PEER AT THE OTHER END OF THIS LEG, not
        // of the peer whose datagram we are carrying. A leg is a two-party link and its key belongs
        // to the party that is not the relay; that one rule covers everything the relay sends
        // (WELCOME, PING, SESSIONS, a forwarded DATA) without the receiver having to work out which
        // key a given op should be under.
        let key = p.keys.first().copied().unwrap_or(self.secrets.leg);
        let Some(bytes) = leg::encode(op, src, to_handle, room, seq, payload, &key) else {
            return false;
        };
        self.emit(out, addr, bytes);
        true
    }

    /// An error to an address we may not even know. Its own sequence space (0) because there is no
    /// peer to hold one, and it is subject to the amplification cap like everything else.
    fn send_error(&mut self, out: &mut Vec<Out>, to: SocketAddr, room: u32, code: u8) {
        // mp:R1c. Under the addressee's key when we know it -- but under the DEPLOYMENT key when we
        // do not, and that case is load-bearing rather than a gap: the commonest ERROR is
        // ERR_NOT_REGISTERED to a peer this relay has never heard of, which after a relay RESTART
        // is a peer that holds a key we no longer have. A restarted relay that could not say
        // "re-HELLO" would black-hole every match it used to carry. The peers' half of this is the
        // same exception, stated in `udp_relay.cpp`.
        let key = self
            .by_addr
            .get(&to)
            .and_then(|h| self.peers.get(h))
            .and_then(|p| p.keys.first().copied())
            .unwrap_or(self.secrets.leg);
        let Some(bytes) = leg::encode(
            leg::OP_ERROR,
            leg::HANDLE_NONE,
            leg::HANDLE_NONE,
            room,
            0,
            &[code],
            &key,
        ) else {
            return;
        };
        self.counters.errors_sent += 1;
        self.emit(out, to, bytes);
    }

    // ---- the entry point ------------------------------------------------------------------------

    pub fn on_datagram(
        &mut self,
        from: SocketAddr,
        pkt: &[u8],
        now: Instant,
        events: &mut Vec<Event>,
    ) -> Vec<Out> {
        let mut out = Vec::new();
        self.counters.leg_rx += 1;
        self.counters.bytes_rx += pkt.len() as u64;
        // mp:R1d. BEFORE the decode, so an oversize datagram is measured even when it is refused
        // for its size -- the measurement is of what the path carried, not of what we accepted.
        if pkt.len() as u64 > self.counters.max_seen {
            self.counters.max_seen = pkt.len() as u64;
        }
        self.note_rx(from, pkt.len());

        // mp:R1c -- WHICH KEY(S) THIS DATAGRAM MAY BE TAGGED UNDER, decided by the handle it claims
        // to be from. A handle that has been re-keyed accepts ONLY its own keys; every other case
        // (a HELLO with no handle yet, a handle we do not know, a peer that never re-keyed) is the
        // deployment key, exactly as at R1. `src` is unauthenticated here and the decode is what
        // authenticates it: naming a re-keyed handle you do not hold the key for fails the MAC,
        // which is the forgery this item closes.
        let claimed = leg::peek_src(pkt).unwrap_or(leg::HANDLE_NONE);
        let (peer_keys, proved): (Vec<[u8; KEY_LEN]>, bool) = self
            .peers
            .get(&claimed)
            .map(|p| (p.keys.clone(), p.keys_proved))
            .unwrap_or_default();
        let mut key_refs: Vec<&[u8; KEY_LEN]> = peer_keys.iter().collect();
        if peer_keys.is_empty() || !proved {
            key_refs.push(&self.secrets.leg);
        }
        // THE ORDER OF `keys` IS NOT AN OPTIMISATION SLOT. The obvious tweak here is move-to-front,
        // so a host with several pairs pays one HMAC in steady state instead of walking its set --
        // and it is WRONG, because `send_leg` tags what the relay sends to a handle with
        // `keys.first()`. Reordering on receipt would change which key the relay SENDS under,
        // while the peer (whose `my_key` is the first one IT installed) is still verifying against
        // a fixed one: a 2-peer match would never notice and a 3-peer one would lose every relay
        // PING, WELCOME and forwarded datagram to the host. `keys[0]` is therefore stable for the
        // life of the handle, and the walk is bounded by ROOM_PEER_CAP HMACs on a miss.
        let decoded = leg::decode_any(pkt, &key_refs);
        // A hit on anything but the trailing deployment key is the peer demonstrating it has made
        // the switch -- after which that trailing entry is gone and a forgery under the shared key
        // is a counted `leg_bad_mac`.
        if let Ok((_, which)) = decoded.as_ref() {
            if !peer_keys.is_empty() && *which < peer_keys.len() && !proved {
                if let Some(p) = self.peers.get_mut(&claimed) {
                    p.keys_proved = true;
                }
            }
        }
        let l = match decoded.map(|(l, _)| l) {
            Ok(l) => l,
            Err(e) => {
                match e {
                    leg::LegError::TooShort => self.counters.leg_too_short += 1,
                    leg::LegError::TooLong => self.counters.leg_too_long += 1,
                    leg::LegError::BadMagic => self.counters.leg_bad_magic += 1,
                    leg::LegError::BadVersion => self.counters.leg_bad_version += 1,
                    leg::LegError::BadOp => self.counters.leg_bad_op += 1,
                    leg::LegError::BadMac => self.counters.leg_bad_mac += 1,
                    leg::LegError::Malformed => self.counters.leg_malformed += 1,
                }
                // mp:R4b. A MAC failure under a handle WE DO NOT HOLD is, after a restart, every
                // datagram from every peer we used to carry: they tag under session keys this
                // process never learned. The one thing that makes such a peer re-HELLO is
                // ERR_NOT_REGISTERED, which the ordinary path below sends only for a datagram that
                // VERIFIED -- so a restarted relay used to be mute exactly when it had to speak
                // (dead-ends G245). Answer under the deployment key, rate-limited per address; a
                // failure under a handle we DO hold stays a silent refusal, because that one is a
                // forgery and a reply would be an oracle for it.
                if matches!(e, leg::LegError::BadMac)
                    && claimed != leg::HANDLE_NONE
                    && !self.peers.contains_key(&claimed)
                    && self.stale_reply_due(from, now)
                {
                    self.counters.stale_handle_replies += 1;
                    let room = leg::peek_room(pkt).unwrap_or(0);
                    self.send_error(&mut out, from, room, leg::ERR_NOT_REGISTERED);
                    events.push(Event::Refused {
                        addr: from,
                        why: "stale_handle",
                    });
                    return out;
                }
                events.push(Event::Refused {
                    addr: from,
                    why: e.name(),
                });
                return out;
            }
        };

        if l.op == leg::OP_HELLO {
            self.on_hello(&mut out, from, &l, now, events);
            return out;
        }
        // dist:RP5 -- the LIVENESS PROBE. A PING from no handle at all is the container's own
        // healthcheck (`mh_relay --health`), tagged under the deployment key like a HELLO would be:
        // it proves the socket is served and the key is the one the probe holds, registers nothing,
        // and is answered with a PONG of the same size (no amplification). Counted, never logged --
        // one every 30 s for the life of the container would bury every real event. A PING that
        // NAMES a handle is a peer's keepalive and goes through the handle path below, where an
        // unknown handle still earns ERR_NOT_REGISTERED (the re-HELLO trigger mp:R4b relies on).
        if l.op == leg::OP_PING && l.src == leg::HANDLE_NONE {
            self.counters.health_pings += 1;
            // Under the deployment key: the prober holds no other, and `l.seq` echoed back is
            // what lets it match the PONG to the PING it sent (sequence space 0, like an ERROR).
            if let Some(bytes) = leg::encode(
                leg::OP_PONG,
                leg::HANDLE_NONE,
                leg::HANDLE_NONE,
                l.room,
                l.seq,
                &[],
                &self.secrets.leg,
            ) {
                self.emit(&mut out, from, bytes);
            }
            return out;
        }

        // Everything else names a handle. An unknown one is the acceptance clause's drop.
        let Some(p) = self.peers.get_mut(&l.src) else {
            self.counters.unregistered += 1;
            events.push(Event::Refused {
                addr: from,
                why: "unregistered",
            });
            self.send_error(&mut out, from, l.room, leg::ERR_NOT_REGISTERED);
            return out;
        };
        if !p.win.check(l.seq) {
            self.counters.replay += 1;
            return out;
        }
        p.win.commit(l.seq);
        p.last_rx = now;
        // The second datagram from an address is what proves it can RECEIVE -- it answered the
        // WELCOME we sent. Only then does the amplification cap come off.
        p.validated = true;
        let (old_addr, room) = (p.addr, p.room);
        if old_addr != from {
            // A rebinding, adopted only now that the tag has verified. This is the property the
            // handle exists for: a peer that changed address is still the same peer.
            p.addr = from;
            self.by_addr.remove(&old_addr);
            self.by_addr.insert(from, l.src);
            events.push(Event::PeerRebound {
                handle: l.src,
                room,
                from: old_addr,
                to: from,
            });
        }

        match l.op {
            leg::OP_PING => {
                self.send_leg(&mut out, l.src, leg::OP_PONG, leg::HANDLE_NONE, &[]);
            }
            leg::OP_PONG => {}
            leg::OP_BYE => {
                self.drop_peer(l.src, "said goodbye", events);
            }
            leg::OP_DATA => self.on_data(&mut out, &l, events),
            leg::OP_REGISTER => self.on_register(&mut out, &l, from, now, events),
            leg::OP_UNREGISTER => self.on_unregister(&l, events),
            leg::OP_LIST => self.on_list(&mut out, l.src, events),
            leg::OP_CAND => self.on_cand(&mut out, &l, events),
            // mp:R3. A probe is a PEER-TO-PEER op and the relay is not a peer. Arriving here means
            // the sender punched a candidate that turned out to be the relay's own address, which
            // a peer behind the same NAT as the relay will do -- so it is counted and dropped, not
            // forwarded: forwarding it would let the relayed path answer a probe and a pair would
            // "promote to direct" onto the route it was already using.
            leg::OP_PROBE | leg::OP_PROBE_ACK => {
                self.counters.probes_misdirected += 1;
            }
            _ => {}
        }
        out
    }

    // ---- mp:R3: the candidate exchange ----------------------------------------------------------
    //
    // THE WHOLE OF THE RELAY'S PART IN HOLE PUNCHING, and it is deliberately this small. The relay
    // does not decide that a pair should punch, does not time the probes, does not learn whether
    // one succeeded, and never sees a probe. It does two things a peer cannot do for itself:
    //
    //   1. CARRY the list to the counterpart, using the same room/peer-set rule `on_data` uses --
    //      a client's list goes to its room's host and nobody else, a host names one of its own
    //      clients. That is "forward only within the registered peer set" applied to punching.
    //   2. SAY WHERE THE SENDER IS SEEN FROM. `from` is the address the sender's NAT actually
    //      rewrote it to, and it is the ONE candidate the sender cannot know. It is appended here,
    //      flagged [`leg::CAND_OBSERVED`], AFTER the peer's own claims are stripped of that flag --
    //      so a peer cannot dress a claim up as an observation.
    //
    // WHAT IT DOES NOT DO, stated because the obvious extension is wrong: it does not remember the
    // list. A stored candidate set would have to be invalidated on every rebinding, and the peer
    // re-sends its own list on a timer anyway (`udp_relay.cpp`) -- storing it would buy a cache
    // that can be stale in exactly the case that matters.
    fn on_cand(&mut self, out: &mut Vec<Out>, l: &leg::Leg, events: &mut Vec<Event>) {
        let Some(p) = self.peers.get(&l.src) else {
            return;
        };
        let (src, role, room_code, from_addr) = (p.handle, p.role, p.room, p.addr);

        let dest = if role == leg::ROLE_CLIENT {
            match self.rooms.get(&room_code).and_then(|r| r.host) {
                Some(h) => h,
                None => {
                    // No host in the room yet. SILENT: a browsing client parked in the directory
                    // room would otherwise buy one ERROR per retry, which is the log flood R2's
                    // `no_host` handling was already taught not to cause.
                    self.counters.cands_refused += 1;
                    return;
                }
            }
        } else if self
            .rooms
            .get(&room_code)
            .map(|r| r.clients.contains(&l.dst))
            .unwrap_or(false)
        {
            l.dst
        } else {
            self.counters.cands_refused += 1;
            return;
        };

        let mut cands = match leg::cands_parse(&l.payload) {
            Ok(c) => c,
            Err(_) => {
                self.counters.cands_refused += 1;
                events.push(Event::Refused {
                    addr: from_addr,
                    why: "cand_malformed",
                });
                return;
            }
        };
        for (_, flags) in cands.iter_mut() {
            *flags &= !leg::CAND_OBSERVED; // only the relay may say "I saw you here"
        }
        cands.truncate(leg::MAX_CANDS - 1); // the observation is never the entry that is dropped
        cands.push((from_addr, leg::CAND_OBSERVED));

        let payload = leg::cands_encode(&cands);
        if self.send_leg(out, dest, leg::OP_CAND, src, &payload) {
            self.counters.cands_forwarded += 1;
            events.push(Event::Candidates {
                from: src,
                to: dest,
                room: room_code,
                n: cands.len(),
                observed: from_addr,
            });
        }
    }

    // ---- mp:R2: the session directory -----------------------------------------------------------
    //
    // Three ops and one rule that decides all of them: A ROOM'S DESCRIPTOR BELONGS TO ITS HOST.
    // Only the peer the room's `host` slot names may publish or withdraw one, so a client cannot
    // advertise a lobby it does not run, and the descriptor dies with the host -- on BYE, on
    // eviction, on a re-home, and on the TTL. The relay never looks inside the bytes.

    fn on_register(
        &mut self,
        out: &mut Vec<Out>,
        l: &leg::Leg,
        from: SocketAddr,
        now: Instant,
        events: &mut Vec<Event>,
    ) {
        let Some(p) = self.peers.get(&l.src) else {
            return;
        };
        let (room_code, handle) = (p.room, p.handle);
        if l.payload.len() > leg::DESC_MAX {
            self.counters.register_refused += 1;
            events.push(Event::Refused {
                addr: from,
                why: "desc_too_long",
            });
            self.send_error(out, from, room_code, leg::ERR_DESC_TOO_LONG);
            return;
        }
        let is_host = self
            .rooms
            .get(&room_code)
            .and_then(|r| r.host)
            .map(|h| h == handle)
            .unwrap_or(false);
        if !is_host {
            self.counters.register_refused += 1;
            events.push(Event::Refused {
                addr: from,
                why: "not_host",
            });
            self.send_error(out, from, room_code, leg::ERR_NOT_HOST);
            return;
        }
        let bytes = l.payload.len();
        let room = self.rooms.get_mut(&room_code).expect("checked above");
        let fresh = room.desc.is_none();
        room.desc = Some(Desc {
            bytes: l.payload.clone(),
            host: handle,
            at: now,
        });
        if fresh {
            self.counters.sessions_registered += 1;
        }
        events.push(Event::SessionRegistered {
            room: room_code,
            handle,
            bytes,
            fresh,
        });
    }

    fn on_unregister(&mut self, l: &leg::Leg, events: &mut Vec<Event>) {
        let Some(p) = self.peers.get(&l.src) else {
            return;
        };
        let (room_code, handle) = (p.room, p.handle);
        let dropped = self
            .rooms
            .get_mut(&room_code)
            .map(|r| {
                let mine = r.desc.as_ref().map(|d| d.host == handle).unwrap_or(false);
                if mine {
                    r.desc = None;
                }
                mine
            })
            .unwrap_or(false);
        if dropped {
            self.counters.sessions_unregistered += 1;
            events.push(Event::SessionUnregistered {
                room: room_code,
                why: "withdrawn",
            });
        }
    }

    /// The rooms that currently have a live descriptor, in room order so successive LISTs walk one
    /// stable sequence (which is what makes the per-peer cursor mean anything).
    fn directory(&self) -> Vec<(u32, Vec<u8>)> {
        let mut v: Vec<(u32, Vec<u8>)> = self
            .rooms
            .iter()
            .filter_map(|(code, r)| r.desc.as_ref().map(|d| (*code, d.bytes.clone())))
            .collect();
        v.sort_by_key(|(code, _)| *code);
        v
    }

    fn on_list(&mut self, out: &mut Vec<Out>, handle: u16, events: &mut Vec<Event>) {
        let _ = events;
        self.counters.list_requests += 1;
        let dir = self.directory();
        let total = dir.len().min(u16::MAX as usize) as u16;
        let start = if dir.is_empty() {
            0
        } else {
            self.peers
                .get(&handle)
                .map(|p| p.list_cursor % dir.len())
                .unwrap_or(0)
        };
        // AN EMPTY DIRECTORY STILL GETS AN ANSWER, and that is the vanish clause: a peer that
        // learns "there are zero lobbies here" from a page is in a different state from one whose
        // LIST went unanswered. Its rows then age out on their own TTL either way, but the empty
        // page is what makes the log say so at the moment it happened.
        let mut at = start;
        let mut sent_entries = 0usize;
        for page in 0..LIST_MAX_PAGES {
            let rotated: Vec<(u32, Vec<u8>)> = if dir.is_empty() {
                Vec::new()
            } else {
                dir[at..].iter().chain(dir[..at].iter()).cloned().collect()
            };
            let (payload, used) = leg::sessions_encode(&rotated, total, sent_entries as u16);
            self.send_leg(out, handle, leg::OP_SESSIONS, leg::HANDLE_NONE, &payload);
            sent_entries += used;
            if used > 0 {
                at = (at + used) % dir.len();
            }
            if used == 0 || sent_entries >= dir.len() || page + 1 == LIST_MAX_PAGES {
                break;
            }
        }
        self.counters.list_entries_sent += sent_entries as u64;
        if let Some(p) = self.peers.get_mut(&handle) {
            p.list_cursor = at;
        }
    }

    fn on_hello(
        &mut self,
        out: &mut Vec<Out>,
        from: SocketAddr,
        l: &leg::Leg,
        now: Instant,
        events: &mut Vec<Event>,
    ) {
        let Ok((role, hflags, match_id)) = leg::hello_parse(&l.payload) else {
            self.counters.leg_malformed += 1;
            return;
        };
        // mp:R4a -- the level the peer was built against. Compared and counted, never refused:
        // a mismatch is information about what will be refused later, and the peer is the party
        // that shows it to a person. The WELCOME below answers with OUR level only to a peer that
        // advertised one -- a pre-R4a peer refuses any WELCOME that is not exactly 4 bytes.
        let peer_level = leg::hello_level(hflags);
        if peer_level != self.limits.level {
            self.counters.hello_level_mismatch += 1;
        }
        let welcome = |me: u16, other: u16, limits: &Limits| -> Vec<u8> {
            if peer_level != 0 && limits.level != 0 {
                leg::welcome_payload_level(me, other, limits.level)
            } else {
                leg::welcome_payload(me, other)
            }
        };

        // A HELLO naming an existing handle is an UPDATE, not a second registration: it is how a
        // peer tells the relay the match_id mh.dll minted after the link was already up, and how a
        // rebound peer re-announces itself. Registering again instead would leak a handle per lobby.
        if l.src != leg::HANDLE_NONE {
            if let Some(p) = self.peers.get(&l.src) {
                let (old_room, peer_role) = (p.room, p.role);
                // A HELLO naming a DIFFERENT room than the one this handle is currently in must
                // move it, not just refresh it -- otherwise the update branch silently strands the
                // peer in its old room forever (mp:R1b). Admission is the same check a fresh join
                // uses; a refusal here leaves the peer exactly where it was, address/match_id
                // refresh included below.
                if l.room != old_room {
                    if let Some((code, why)) = self.room_admission(l.room, peer_role) {
                        self.send_error(out, from, l.room, code);
                        events.push(Event::Refused { addr: from, why });
                        return;
                    }
                    self.rehome_peer(l.src, peer_role, old_room, l.room, events);
                }
            }
            if let Some(p) = self.peers.get_mut(&l.src) {
                p.last_rx = now;
                p.validated = true;
                if p.addr != from {
                    let old = p.addr;
                    p.addr = from;
                    self.by_addr.remove(&old);
                    self.by_addr.insert(from, l.src);
                    events.push(Event::PeerRebound {
                        handle: l.src,
                        room: p.room,
                        from: old,
                        to: from,
                    });
                }
                if !is_nil(&match_id) && p.match_id != match_id {
                    p.match_id = match_id;
                    let (h, room) = (p.handle, p.room);
                    events.push(Event::MatchId {
                        handle: h,
                        room,
                        match_id: hex(&match_id),
                    });
                }
                let other = self.counterpart(l.src);
                let pay = welcome(l.src, other, &self.limits);
                self.send_leg(out, l.src, leg::OP_WELCOME, leg::HANDLE_NONE, &pay);
                return;
            }
            // A handle we do not know: treat it as a fresh registration rather than an error, so a
            // peer that outlived a relay restart reconnects instead of hanging -- and (mp:R4b) give
            // it THE HANDLE IT NAMED, below, so that everything its counterpart keyed by that
            // number is still right afterwards.
        }

        let room_code = l.room;
        // THE ROOM IS LOOKED UP, NOT CREATED, UNTIL THE REGISTRATION IS ACCEPTED. An `entry()` here
        // would leave an empty room behind every refused client -- an unauthenticated peer could
        // then fill the room table by naming codes nobody hosts, which is the same unbounded-map
        // problem the amplification ledger is capped against.
        if let Some((code, why)) = self.room_admission(room_code, role) {
            self.send_error(out, from, room_code, code);
            events.push(Event::Refused { addr: from, why });
            return;
        }

        // mp:R4b -- A PEER THAT OUTLIVED OUR RESTART ASKS FOR ITS OLD HANDLE by leaving it in
        // `src`, and gets it if nothing else took it meanwhile. Handles are not secrets and carry
        // no authority of their own (a fresh registration holds no session key until a token is
        // learned), so granting a requested number costs nothing; what it BUYS is that the other
        // peer's pair keys, punch state and -- on a host -- the per-client loopback socket, all
        // keyed by this number, survive. A collision (a new peer got the number first) falls
        // through to a fresh allocation and the peer rebuilds those tables from the WELCOME.
        let handle = if l.src != leg::HANDLE_NONE && !self.peers.contains_key(&l.src) {
            self.counters.handles_restored += 1;
            l.src
        } else {
            self.alloc_handle()
        };
        // A stale entry for this address (a peer that restarted without a BYE) must go, or the
        // address map would point at a handle nothing can reach.
        if let Some(old) = self.by_addr.get(&from).copied() {
            self.drop_peer(
                old,
                "replaced by a new registration from the same address",
                events,
            );
        }
        self.peers.insert(
            handle,
            Peer {
                handle,
                addr: from,
                role,
                room: room_code,
                match_id,
                win: ReplayWindow::new(),
                tx_seq: 0,
                validated: false,
                last_rx: now,
                last_ping: now,
                list_cursor: 0,
                // mp:R1c. Recorded at registration and never revisited: a peer's build does not
                // change mid-session, and a handle whose capability could be edited later by a
                // subsequent HELLO would be a way to talk the relay OUT of a ratchet it had
                // already applied.
                can_rekey: (hflags & leg::HELLO_FLAG_PEER_KEY) != 0,
                keys: Vec::new(),
                keys_proved: false,
            },
        );
        self.by_addr.insert(from, handle);
        if let std::collections::hash_map::Entry::Vacant(e) = self.rooms.entry(room_code) {
            e.insert(Room::default());
            self.counters.rooms_opened += 1;
        }
        let room = self.rooms.get_mut(&room_code).expect("just inserted");
        if role == leg::ROLE_HOST {
            room.host = Some(handle);
        } else {
            room.clients.push(handle);
        }
        self.counters.peers_registered += 1;
        events.push(Event::PeerRegistered {
            handle,
            room: room_code,
            role,
            addr: from,
            match_id: hex(&match_id),
            level: peer_level,
        });

        let other = self.counterpart(handle);
        let pay = welcome(handle, other, &self.limits);
        self.send_leg(out, handle, leg::OP_WELCOME, leg::HANDLE_NONE, &pay);
    }

    /// Whether `role` may join `room` right now: `None` admits it, `Some((code, why))` refuses it
    /// with that leg ERROR code and the reason a [`Event::Refused`] should carry. Shared by a
    /// fresh registration and a HELLO update that re-homes an already-registered peer -- both are
    /// "does this room accept this role", the only difference is what happens after.
    fn room_admission(&self, room: u32, role: u8) -> Option<(u8, &'static str)> {
        let existing = self.rooms.get(&room);
        let live_host = existing
            .and_then(|r| r.host)
            .filter(|h| self.peers.contains_key(h));
        let occupancy = existing
            .map(|r| r.clients.len() + usize::from(r.host.is_some()))
            .unwrap_or(0);
        if room == DIRECTORY_ROOM {
            // A host in the directory room would be advertising a lobby nobody can be routed to,
            // so it is refused by name rather than admitted into a room that cannot carry a match.
            if role == leg::ROLE_HOST {
                return Some((leg::ERR_NOT_HOST, "host_in_directory_room"));
            }
            if occupancy >= DIRECTORY_PEER_CAP {
                return Some((leg::ERR_ROOM_FULL, "room_full"));
            }
            return None;
        }
        if role == leg::ROLE_HOST {
            if live_host.is_some() {
                // Someone already hosts this code. Refusing beats silently re-pointing the
                // clients: two hosts in one room is a lobby nobody can join reliably.
                return Some((leg::ERR_ROOM_BUSY, "room_busy"));
            }
        } else if live_host.is_none() {
            return Some((leg::ERR_NO_HOST, "no_host"));
        }
        if occupancy >= ROOM_PEER_CAP {
            return Some((leg::ERR_ROOM_FULL, "room_full"));
        }
        None
    }

    /// Move an already-registered peer from `old_room` to `new_room` (its handle is unchanged).
    /// Leaving uses the same bookkeeping [`Relay::drop_peer`] uses for a departing peer -- the old
    /// room's count drops, its stale conns for this handle go with it, and an emptied room is
    /// removed exactly as eviction removes one. Joining is the same bookkeeping a fresh
    /// registration uses. The caller has already checked [`Relay::room_admission`] for `new_room`.
    fn rehome_peer(
        &mut self,
        handle: u16,
        role: u8,
        old_room: u32,
        new_room: u32,
        events: &mut Vec<Event>,
    ) {
        let mut close_old = false;
        let mut desc_dropped = false;
        if let Some(room) = self.rooms.get_mut(&old_room) {
            if room.host == Some(handle) {
                room.host = None;
            }
            room.clients.retain(|&c| c != handle);
            room.conns
                .retain(|_, c| c.host != handle && c.client != handle);
            // mp:R2: the lobby a host advertised HERE is not the lobby it is holding THERE. Leaving
            // the descriptor behind would keep a browser dialling a room whose host has moved.
            if room
                .desc
                .as_ref()
                .map(|d| d.host == handle)
                .unwrap_or(false)
            {
                room.desc = None;
                desc_dropped = true;
            }
            close_old = room.host.is_none() && room.clients.is_empty();
        }
        if desc_dropped {
            self.counters.sessions_unregistered += 1;
            events.push(Event::SessionUnregistered {
                room: old_room,
                why: "host_rehomed",
            });
        }
        if close_old {
            self.rooms.remove(&old_room);
            self.counters.rooms_closed += 1;
            events.push(Event::RoomClosed { room: old_room });
        }

        if let std::collections::hash_map::Entry::Vacant(e) = self.rooms.entry(new_room) {
            e.insert(Room::default());
            self.counters.rooms_opened += 1;
        }
        let room = self.rooms.get_mut(&new_room).expect("just inserted");
        if role == leg::ROLE_HOST {
            room.host = Some(handle);
        } else {
            room.clients.push(handle);
        }
        if let Some(p) = self.peers.get_mut(&handle) {
            p.room = new_room;
        }
        events.push(Event::Rehomed {
            handle,
            from_room: old_room,
            to_room: new_room,
        });
    }

    /// The handle a peer should address by default: a client's host, or [`leg::HANDLE_NONE`] for a
    /// host (which has many counterparts and names each one explicitly).
    fn counterpart(&self, handle: u16) -> u16 {
        let Some(p) = self.peers.get(&handle) else {
            return leg::HANDLE_NONE;
        };
        if p.role != leg::ROLE_CLIENT {
            return leg::HANDLE_NONE;
        }
        self.rooms
            .get(&p.room)
            .and_then(|r| r.host)
            .unwrap_or(leg::HANDLE_NONE)
    }

    fn on_data(&mut self, out: &mut Vec<Out>, l: &leg::Leg, events: &mut Vec<Event>) {
        let Some(p) = self.peers.get(&l.src) else {
            return;
        };
        let (src, role, room_code, from_addr) = (p.handle, p.role, p.room, p.addr);

        // WHERE IT GOES. A client has exactly one counterpart and does not get to choose; a host
        // names the client. Either way the destination is a peer of THIS room and nothing else --
        // that is "forward only within the registered peer set" at the room level, and the conn
        // check below is the same rule at the session level.
        let dest = if role == leg::ROLE_CLIENT {
            match self.rooms.get(&room_code).and_then(|r| r.host) {
                Some(h) => h,
                None => {
                    self.send_error(out, from_addr, room_code, leg::ERR_NO_HOST);
                    return;
                }
            }
        } else {
            let ok = self
                .rooms
                .get(&room_code)
                .map(|r| r.clients.contains(&l.dst))
                .unwrap_or(false);
            if !ok {
                self.counters.not_in_peer_set += 1;
                self.send_error(out, from_addr, room_code, leg::ERR_NO_PEER);
                return;
            }
            l.dst
        };

        let inner = &l.payload;
        let Some(hdr) = wire::Header::peek(inner) else {
            self.counters.inner_malformed += 1;
            return;
        };

        // mp:R1c -- the keys a grant in THIS datagram mints, held until after it has been forwarded.
        let mut rekey: Option<(u16, [u8; KEY_LEN], u16, [u8; KEY_LEN])> = None;

        if hdr.conn_id == self.secrets.boot_conn {
            // The PSK bootstrap. The relay holds the boot keys (they are PSK-derived, like the
            // peers' own), so it verifies this datagram fully -- and, when the HOST is the sender,
            // reads the connect token out of the grant. That is the only way the relay ever learns
            // a session's MAC keys, and it is the moment `docs/mp-wire-udp.md` means by "it uses
            // token_open to decide who is registered".
            match wire::packet_decode(
                inner,
                Some(&self.secrets.boot_conn),
                &self.secrets.boot_enc,
                &self.secrets.boot_mac,
                None,
            ) {
                Err(_) => {
                    self.counters.boot_refused += 1;
                    return;
                }
                Ok((_, body)) => {
                    if role != leg::ROLE_CLIENT {
                        rekey = self.learn_tokens(&body, room_code, src, dest, events);
                    }
                }
            }
        } else {
            let conn = self
                .rooms
                .get(&room_code)
                .and_then(|r| r.conns.get(&hdr.conn_id))
                .cloned();
            match conn {
                Some(c) => {
                    let pair_ok =
                        (c.host == src && c.client == dest) || (c.client == src && c.host == dest);
                    if !pair_ok {
                        self.counters.not_in_peer_set += 1;
                        return;
                    }
                    // Direction decides the key: the client→host half and the host→client half of
                    // one connection are authenticated under different keys, which is also what
                    // stops a peer replaying the other side's traffic back at it.
                    let key = if role == leg::ROLE_CLIENT {
                        c.mac_c2s
                    } else {
                        c.mac_s2c
                    };
                    if !wire::mac_verify(inner, &key) {
                        self.counters.inner_bad_mac += 1;
                        events.push(Event::Refused {
                            addr: from_addr,
                            why: "inner_bad_mac",
                        });
                        return;
                    }
                }
                None => {
                    self.counters.inner_unverified += 1;
                    if self.limits.strict {
                        return;
                    }
                }
            }
        }

        let payload = inner.clone();
        if self.send_leg(out, dest, leg::OP_DATA, src, &payload) {
            self.counters.forwarded += 1;
        }

        // mp:R1c -- AND ONLY NOW. The datagram just forwarded is the one that CARRIES the grant, so
        // it is the last one either end can tag with the old key: the client does not hold the
        // token until it has read this very payload. Installing before the forward would have the
        // relay send the grant under a key its recipient can only derive FROM the grant, which is
        // a deadlock rather than a race -- it would never resolve on a retry. Ordering is the whole
        // of the handover; there is no negotiation anywhere.
        if let Some((h_host, k_host, h_client, k_client)) = rekey {
            self.install_leg_key(h_host, k_host, events);
            self.install_leg_key(h_client, k_client, events);
        }
    }

    /// mp:R1c -- move one handle off the deployment key, or add another pair's key to a handle
    /// already moved. Refused for a peer that did not advertise the capability, which is what keeps
    /// a pre-R1c peer working instead of being ratcheted off the air.
    fn install_leg_key(&mut self, handle: u16, key: [u8; KEY_LEN], events: &mut Vec<Event>) {
        let Some(p) = self.peers.get_mut(&handle) else {
            return;
        };
        if !p.can_rekey || p.keys.contains(&key) {
            return;
        }
        let first = p.keys.is_empty();
        // Bounded by what a peer can legitimately be an end of. A host of eight clients holds eight
        // beside `keys[0]`; nothing can push more in, because a key only ever arrives with a token
        // the relay opened, and `drop_peer` takes a departed client's key out again (mp:R3e).
        // Should the cap still be reached, the eviction NEVER takes `keys[0]`: that is the key the
        // host tags every relay-directed datagram with for the life of its handle (its `my_key`,
        // udp_relay.cpp), and evicting it silently failed every PING and relayed DATA from a host
        // whose TENTH client had just joined -- the relay-side half of the R3e leak.
        if p.keys.len() >= ROOM_PEER_CAP {
            p.keys.remove(1);
        }
        p.keys.push(key);
        if first {
            let room = p.room;
            self.counters.peers_rekeyed += 1;
            events.push(Event::LegRekeyed { handle, room });
        }
    }

    /// Walk a decrypted bootstrap body for the host's token grant and learn the session it mints.
    #[allow(clippy::type_complexity)]
    fn learn_tokens(
        &mut self,
        body: &[u8],
        room_code: u32,
        host: u16,
        client: u16,
        events: &mut Vec<Event>,
    ) -> Option<(u16, [u8; KEY_LEN], u16, [u8; KEY_LEN])> {
        let mut minted = None;
        let Ok(frames) = wire::frames(body) else {
            return None;
        };
        for f in frames {
            if f.id != CH_HS || f.data.len() != 1 + wire::TOKEN_WIRE || f.data[0] != HSK_GRANT {
                continue;
            }
            // `now_unix_ms = 0` skips the expiry test ON PURPOSE: the relay has no business
            // deciding a token is too old -- the host minted it and the host checks it, and a relay
            // with a skewed clock that refused a live match would be a fault nobody could diagnose
            // from either endpoint.
            let (res, _expired) = wire::token_open(&self.psk, &f.data[1..], 0);
            let Ok(tok) = res else { continue };
            let room = self.rooms.entry(room_code).or_default();
            let fresh = !room.conns.contains_key(&tok.conn_id);
            room.conns.insert(
                tok.conn_id,
                Conn {
                    host,
                    client,
                    mac_c2s: tok.mac_c2s,
                    mac_s2c: tok.mac_s2c,
                },
            );
            // mp:R1c -- THE TWO LEG KEYS THIS TOKEN MINTS, derived on every sighting rather than
            // only on a fresh one: a grant is retransmitted when its token-ack is lost, and the
            // retransmit is exactly the copy a peer that missed the first one is re-keying from.
            minted = Some((
                host,
                leg::peer_leg_key(&tok.mac_s2c, false),
                client,
                leg::peer_leg_key(&tok.mac_c2s, true),
            ));
            if fresh {
                self.counters.tokens_learned += 1;
                events.push(Event::SessionLearned {
                    room: room_code,
                    conn: hex(&tok.conn_id),
                    slot: tok.slot,
                    host,
                    client,
                    scope: hex(&tok.match_id),
                });
            }
        }
        minted
    }

    fn alloc_handle(&mut self) -> u16 {
        loop {
            let h = self.next_handle;
            self.next_handle = self.next_handle.wrapping_add(1);
            if self.next_handle == leg::HANDLE_NONE {
                self.next_handle = 1;
            }
            if h != leg::HANDLE_NONE && !self.peers.contains_key(&h) {
                return h;
            }
        }
    }

    fn drop_peer(&mut self, handle: u16, why: &'static str, events: &mut Vec<Event>) {
        let Some(p) = self.peers.remove(&handle) else {
            return;
        };
        self.by_addr.remove(&p.addr);
        self.counters.peers_evicted += 1;
        events.push(Event::PeerGone {
            handle,
            room: p.room,
            why,
        });
        let mut close = false;
        let mut desc_dropped = false;
        // mp:R3e -- the leg keys this peer's connections gave its COUNTERPARTS. A host's handle
        // holds one key per client token the relay opened for it, and a key that stayed after its
        // client left was the relay-side half of R3e's leak: at the tenth token the FIFO cap
        // evicted `keys[0]`, the host's own send key, and every relay-directed datagram from the
        // host failed the MAC from then on. Collected under the room borrow, removed after it.
        let mut stale: Vec<(u16, [u8; KEY_LEN])> = Vec::new();
        if let Some(room) = self.rooms.get_mut(&p.room) {
            if room.host == Some(handle) {
                room.host = None;
            }
            room.clients.retain(|&c| c != handle);
            for c in room.conns.values() {
                if c.client == handle {
                    stale.push((c.host, leg::peer_leg_key(&c.mac_s2c, false)));
                } else if c.host == handle {
                    stale.push((c.client, leg::peer_leg_key(&c.mac_c2s, true)));
                }
            }
            // A connection whose peer is gone is not routable any more, and keeping its keys would
            // let a later peer that happened to be handed the same handle inherit them.
            room.conns
                .retain(|_, c| c.host != handle && c.client != handle);
            // mp:R2, the same reasoning one level up: a lobby whose host is gone is not a lobby.
            // This is the path a host that crashed or was firewalled off takes; a host that LEAVES
            // its lobby normally sends UNREGISTER long before its peer is evicted.
            if room
                .desc
                .as_ref()
                .map(|d| d.host == handle)
                .unwrap_or(false)
            {
                room.desc = None;
                desc_dropped = true;
            }
            close = room.host.is_none() && room.clients.is_empty();
        }
        // `keys[0]` is never removed, whoever minted it: it is what the counterpart tags its
        // relay-directed traffic with (udp_relay.cpp keeps `my_key` as a COPY for exactly this
        // reason), and neither side has a message for agreeing on a replacement.
        for (other, key) in stale {
            if let Some(o) = self.peers.get_mut(&other) {
                if let Some(i) = o.keys.iter().position(|k| *k == key) {
                    if i > 0 {
                        o.keys.remove(i);
                    }
                }
            }
        }
        if desc_dropped {
            self.counters.sessions_unregistered += 1;
            events.push(Event::SessionUnregistered { room: p.room, why });
        }
        if close {
            self.rooms.remove(&p.room);
            self.counters.rooms_closed += 1;
            events.push(Event::RoomClosed { room: p.room });
        }
    }

    /// Housekeeping: evict the silent, ping the quiet. Called on a timer by `main.rs`.
    pub fn tick(&mut self, now: Instant, events: &mut Vec<Event>) -> Vec<Out> {
        let mut out = Vec::new();
        // mp:R2: forget a descriptor whose host stopped refreshing it. Done BEFORE the eviction
        // pass so a host that has gone quiet stops being LISTED (20 s) well before it stops being
        // a peer (60 s) -- which is the whole reason the two timers are different numbers.
        let stale: Vec<u32> = self
            .rooms
            .iter()
            .filter(|(_, r)| {
                r.desc
                    .as_ref()
                    .map(|d| now.duration_since(d.at) > self.limits.session_ttl)
                    .unwrap_or(false)
            })
            .map(|(code, _)| *code)
            .collect();
        for code in stale {
            if let Some(r) = self.rooms.get_mut(&code) {
                r.desc = None;
            }
            self.counters.sessions_expired += 1;
            events.push(Event::SessionUnregistered {
                room: code,
                why: "expired",
            });
        }
        let dead: Vec<u16> = self
            .peers
            .values()
            .filter(|p| now.duration_since(p.last_rx) > self.limits.idle)
            .map(|p| p.handle)
            .collect();
        for h in dead {
            self.drop_peer(h, "idle", events);
        }
        let quiet: Vec<u16> = self
            .peers
            .values()
            .filter(|p| {
                now.duration_since(p.last_rx) > self.limits.ping_after
                    && now.duration_since(p.last_ping) > self.limits.ping_after
            })
            .map(|p| p.handle)
            .collect();
        for h in quiet {
            if let Some(p) = self.peers.get_mut(&h) {
                p.last_ping = now;
            }
            self.send_leg(&mut out, h, leg::OP_PING, leg::HANDLE_NONE, &[]);
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::{IpAddr, Ipv4Addr};

    fn addr(n: u16) -> SocketAddr {
        SocketAddr::new(IpAddr::V4(Ipv4Addr::new(127, 0, 0, 1)), 40000 + n)
    }
    fn psk() -> [u8; KEY_LEN] {
        let mut k = [0u8; KEY_LEN];
        for (i, b) in k.iter_mut().enumerate() {
            *b = (i as u8).wrapping_mul(7).wrapping_add(3);
        }
        k
    }

    struct Rig {
        r: Relay,
        now: Instant,
        ev: Vec<Event>,
    }

    impl Rig {
        fn new() -> Rig {
            Rig {
                r: Relay::new(psk(), Limits::default()),
                now: Instant::now(),
                ev: Vec::new(),
            }
        }
        fn feed(&mut self, from: SocketAddr, pkt: &[u8]) -> Vec<Out> {
            self.r.on_datagram(from, pkt, self.now, &mut self.ev)
        }
        fn hello(&mut self, from: SocketAddr, role: u8, room: u32, mid: [u8; 16]) -> Vec<Out> {
            self.hello_flags(from, role, room, mid, 0)
        }
        fn hello_flags(
            &mut self,
            from: SocketAddr,
            role: u8,
            room: u32,
            mid: [u8; 16],
            flags: u8,
        ) -> Vec<Out> {
            let s = leg::Secrets::derive(&psk());
            let p = leg::hello_payload_flags(role, flags, &mid);
            let pkt = leg::encode(leg::OP_HELLO, 0, 0, room, 0, &p, &s.leg).unwrap();
            self.feed(from, &pkt)
        }
        /// Register and then validate the address, which is what a real peer's next datagram does.
        fn join(&mut self, from: SocketAddr, role: u8, room: u32) -> u16 {
            self.join_flags(from, role, room, 0)
        }
        /// mp:R1c -- the same, advertising that this peer can be re-keyed from its token.
        fn join_rekeying(&mut self, from: SocketAddr, role: u8, room: u32) -> u16 {
            self.join_flags(from, role, room, leg::HELLO_FLAG_PEER_KEY)
        }
        fn join_flags(&mut self, from: SocketAddr, role: u8, room: u32, flags: u8) -> u16 {
            let out = self.hello_flags(from, role, room, [0u8; 16], flags);
            let s = leg::Secrets::derive(&psk());
            let w = leg::decode(&out[0].bytes, &s.leg).unwrap();
            let (me, _) = leg::welcome_parse(&w.payload).unwrap();
            let ping = leg::encode(leg::OP_PING, me, 0, room, 1, &[], &s.leg).unwrap();
            self.feed(from, &ping);
            me
        }
        /// mp:R1c -- stage the host's GRANT for one connection, which is what re-keys both ends.
        /// Returns the token so the caller can derive the two leg keys the way both peers do.
        fn grant(&mut self, host: u16, client: u16, room: u32, seq: u64) -> wire::ConnectToken {
            let s = leg::Secrets::derive(&psk());
            let tok = wire::ConnectToken {
                match_id: [0xa5u8; 16],
                conn_id: [0x11u8, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x03],
                slot: 3,
                expire_unix_ms: 1_900_000_000_000,
                enc_c2s: [10u8; KEY_LEN],
                enc_s2c: [11u8; KEY_LEN],
                mac_c2s: [12u8; KEY_LEN],
                mac_s2c: [13u8; KEY_LEN],
            };
            let sealed = wire::token_seal(&psk(), 0x1234, &tok).unwrap();
            let mut frame = vec![HSK_GRANT];
            frame.extend_from_slice(&sealed);
            let mut body = Vec::new();
            assert!(wire::frame_append(&mut body, CH_HS, &frame));
            let boot = wire::packet_encode(
                wire::PKT_DATA,
                &s.boot_conn,
                7,
                &s.boot_enc,
                &s.boot_mac,
                &body,
            )
            .unwrap();
            let pkt = leg::encode(leg::OP_DATA, host, client, room, seq, &boot, &s.leg).unwrap();
            let host_addr = self.r.peers.get(&host).map(|p| p.addr).unwrap();
            self.feed(host_addr, &pkt);
            tok
        }
    }

    // ---- mp:R4b: surviving a relay restart -------------------------------------------------------

    #[test]
    fn a_restarted_relay_tells_a_re_keyed_peer_to_re_hello() {
        // THE G245 SHAPE. A fresh relay (no handles at all) receives what a re-keyed peer sends
        // after the restart: a DATA naming its old handle, tagged under a session key this process
        // never learned. Before R4b that was a silent `leg_bad_mac` forever; now it is answered
        // with NOT_REGISTERED under the deployment key -- the one key both ends still share --
        // once per STALE_HANDLE_REPLY per address, and the re-HELLO that follows is what restores
        // the match.
        let mut g = Rig::new();
        let s = leg::Secrets::derive(&psk());
        let session_key = [0x5au8; KEY_LEN];
        let stale = leg::encode(leg::OP_DATA, 7, 1, 6501, 900, &[1, 2, 3], &session_key).unwrap();

        let out = g.feed(addr(9), &stale);
        assert_eq!(
            g.r.counters.leg_bad_mac, 1,
            "still counted as the MAC failure it is"
        );
        assert_eq!(g.r.counters.stale_handle_replies, 1);
        assert_eq!(out.len(), 1, "one reply, to the sender");
        let e = leg::decode(&out[0].bytes, &s.leg).expect("under the DEPLOYMENT key");
        assert_eq!(e.op, leg::OP_ERROR);
        assert_eq!(e.payload, vec![leg::ERR_NOT_REGISTERED]);
        assert_eq!(
            e.room, 6501,
            "the room is echoed out of the unverified header"
        );
        assert!(g
            .ev
            .iter()
            .any(|e| matches!(e, Event::Refused { why, .. } if *why == "stale_handle")));

        // The same peer's next 29 datagrams in the same half second buy nothing more.
        for seq in 901..930 {
            let d = leg::encode(leg::OP_DATA, 7, 1, 6501, seq, &[], &session_key).unwrap();
            assert!(g.feed(addr(9), &d).is_empty(), "rate-limited per address");
        }
        assert_eq!(g.r.counters.stale_handle_replies, 1);
        assert_eq!(g.r.counters.leg_bad_mac, 30);

        // ...and after the window it is told again, because the first reply may have been lost.
        g.now += STALE_HANDLE_REPLY;
        let d = leg::encode(leg::OP_DATA, 7, 1, 6501, 931, &[], &session_key).unwrap();
        assert_eq!(g.feed(addr(9), &d).len(), 1);
        assert_eq!(g.r.counters.stale_handle_replies, 2);
    }

    /// dist:RP5 -- the container healthcheck: a PING from no handle, under the deployment key, is
    /// answered with a PONG to the sender, registers nothing, logs nothing, and is counted. Under
    /// the wrong key it is a plain bad_mac (the probe proves the key, not only the socket).
    #[test]
    fn a_keyed_ping_from_no_handle_is_a_health_pong() {
        let psk = [7u8; 32];
        let s = leg::Secrets::derive(&psk);
        let mut r = Relay::new(psk, Limits::default());
        let prober: SocketAddr = "127.0.0.1:40000".parse().unwrap();
        let mut events = Vec::new();
        let ping = leg::encode(leg::OP_PING, leg::HANDLE_NONE, 0, 0, 77, &[], &s.leg).unwrap();
        let out = r.on_datagram(prober, &ping, Instant::now(), &mut events);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].to, prober);
        let pong = leg::decode(&out[0].bytes, &s.leg).unwrap();
        assert_eq!(pong.op, leg::OP_PONG);
        assert_eq!(pong.seq, 77);
        assert_eq!(r.counters.health_pings, 1);
        assert_eq!(r.peer_count(), 0);
        assert!(
            events.is_empty(),
            "a health ping is not an event: {events:?}"
        );
        let wrong = [8u8; 32];
        let bad = leg::encode(leg::OP_PING, leg::HANDLE_NONE, 0, 0, 78, &[], &wrong).unwrap();
        let out = r.on_datagram(prober, &bad, Instant::now(), &mut events);
        assert!(out.is_empty());
        assert_eq!(r.counters.leg_bad_mac, 1);
        assert_eq!(r.counters.health_pings, 1);
    }

    #[test]
    fn a_mac_failure_under_a_handle_we_hold_is_still_silent() {
        // The other half of the rule, so R4b does not become an oracle for the forgery R1c closed:
        // a datagram naming a LIVE handle under the wrong key gets nothing back, ever.
        let mut g = Rig::new();
        let h = g.join_rekeying(addr(1), leg::ROLE_HOST, 6501);
        let wrong = [0x99u8; KEY_LEN];
        let forged = leg::encode(leg::OP_PING, h, 0, 6501, 50, &[], &wrong).unwrap();
        assert!(g.feed(addr(3), &forged).is_empty());
        assert_eq!(g.r.counters.leg_bad_mac, 1);
        assert_eq!(g.r.counters.stale_handle_replies, 0);
        // A HELLO is never a stale-handle case either: it names no handle to be stale about.
        let hello_p = leg::hello_payload_flags(leg::ROLE_CLIENT, 0, &[0u8; 16]);
        let bad_hello = leg::encode(leg::OP_HELLO, 0, 0, 6501, 0, &hello_p, &wrong).unwrap();
        assert!(g.feed(addr(4), &bad_hello).is_empty());
        assert_eq!(g.r.counters.stale_handle_replies, 0);
    }

    // ---- mp:R4a, the protocol level ------------------------------------------------------------

    fn welcome_of(out: &[Out]) -> Vec<u8> {
        let s = leg::Secrets::derive(&psk());
        let w = leg::decode(&out[0].bytes, &s.leg).unwrap();
        assert_eq!(w.op, leg::OP_WELCOME);
        w.payload
    }

    fn level_flags() -> u8 {
        leg::hello_flags_with_level(leg::HELLO_FLAG_PEER_KEY, leg::PROTOCOL_LEVEL)
    }

    #[test]
    fn a_hello_without_a_level_gets_the_four_byte_welcome_it_can_read_and_is_counted() {
        // A pre-R4a peer: the re-key bit alone. It refuses any WELCOME that is not exactly 4 bytes,
        // so the relay must not append its level to it -- and the mismatch is COUNTED, not refused:
        // the peer is registered, re-keyable, served.
        let mut g = Rig::new();
        let out = g.hello_flags(
            addr(1),
            leg::ROLE_HOST,
            6501,
            [0u8; 16],
            leg::HELLO_FLAG_PEER_KEY,
        );
        let w = welcome_of(&out);
        assert_eq!(
            w.len(),
            leg::WELCOME_LEN,
            "a pre-R4a peer gets the 4-byte form"
        );
        assert_eq!(leg::welcome_parse_level(&w).unwrap().2, 0);
        assert_eq!(g.r.counters.hello_level_mismatch, 1);
        assert_eq!(g.r.counters.peers_registered, 1);
        assert!(
            g.r.peers.values().next().unwrap().can_rekey,
            "the re-key bit still reads"
        );
        assert!(matches!(
            g.ev.iter()
                .find(|e| matches!(e, Event::PeerRegistered { .. })),
            Some(Event::PeerRegistered { level: 0, .. })
        ));
    }

    #[test]
    fn a_hello_with_the_level_gets_the_relays_level_back_in_the_welcome() {
        let mut g = Rig::new();
        let out = g.hello_flags(addr(1), leg::ROLE_HOST, 6501, [0u8; 16], level_flags());
        let w = welcome_of(&out);
        assert_eq!(w.len(), leg::WELCOME_LEN_LEVEL);
        let (me, _, theirs) = leg::welcome_parse_level(&w).unwrap();
        assert_eq!(theirs, leg::PROTOCOL_LEVEL);
        assert_eq!(
            g.r.counters.hello_level_mismatch, 0,
            "a matching pair counts nothing"
        );
        assert!(
            g.r.peers[&me].can_rekey,
            "the level does not displace the re-key bit"
        );
        assert!(matches!(
            g.ev.iter()
                .find(|e| matches!(e, Event::PeerRegistered { .. })),
            Some(Event::PeerRegistered {
                level: leg::PROTOCOL_LEVEL,
                ..
            })
        ));
        // A HELLO UPDATE (naming the handle, e.g. carrying the match_id in) is answered the same
        // way: every WELCOME this peer sees carries the level, not only the first.
        let s = leg::Secrets::derive(&psk());
        let p = leg::hello_payload_flags(leg::ROLE_HOST, level_flags(), &[0x77u8; 16]);
        let pkt = leg::encode(leg::OP_HELLO, me, 0, 6501, 5, &p, &s.leg).unwrap();
        let out2 = g.feed(addr(1), &pkt);
        assert_eq!(welcome_of(&out2).len(), leg::WELCOME_LEN_LEVEL);
        assert_eq!(g.r.counters.hello_level_mismatch, 0);
    }

    #[test]
    fn a_relay_staged_behind_answers_its_own_lower_level_or_no_level_at_all() {
        // The scenario knob: `--advertise-level N` makes this relay CLAIM level N. With N one
        // behind, an R4a peer reads `N < ours`; with N = 0 it gets the 4-byte WELCOME a pre-R4a
        // relay sends and reads "no level" -- both are the peer's notice case, staged without an
        // old binary. Either way the peer is served: the level is a claim, not a gate.
        // "One behind" is staged one ABOVE the real numbers (relay claims L+1, the peer says L+2)
        // so the arm stays meaningful while PROTOCOL_LEVEL is 1 -- the relay's answer is what it
        // claims, whatever the peer said, and that is the property under test.
        let older = Limits {
            level: leg::PROTOCOL_LEVEL + 1,
            ..Limits::default()
        };
        let mut g = Rig {
            r: Relay::new(psk(), older),
            now: Instant::now(),
            ev: Vec::new(),
        };
        let peer = leg::hello_flags_with_level(leg::HELLO_FLAG_PEER_KEY, leg::PROTOCOL_LEVEL + 2);
        let out = g.hello_flags(addr(1), leg::ROLE_HOST, 6501, [0u8; 16], peer);
        let w = welcome_of(&out);
        assert_eq!(w.len(), leg::WELCOME_LEN_LEVEL);
        assert_eq!(
            leg::welcome_parse_level(&w).unwrap().2,
            leg::PROTOCOL_LEVEL + 1
        );
        assert_eq!(g.r.counters.hello_level_mismatch, 1);
        assert_eq!(g.r.counters.peers_registered, 1);

        let none = Limits {
            level: 0,
            ..Limits::default()
        };
        let mut g0 = Rig {
            r: Relay::new(psk(), none),
            now: Instant::now(),
            ev: Vec::new(),
        };
        let out0 = g0.hello_flags(addr(1), leg::ROLE_HOST, 6501, [0u8; 16], level_flags());
        let w0 = welcome_of(&out0);
        assert_eq!(
            w0.len(),
            leg::WELCOME_LEN,
            "level 0 = the pre-R4a wire shape, byte for byte"
        );
        assert_eq!(g0.r.counters.hello_level_mismatch, 1);
        assert_eq!(g0.r.counters.peers_registered, 1);
    }

    #[test]
    fn a_peer_newer_than_the_relay_is_served_and_told_the_relays_level() {
        // The forward direction: a peer built against a level this relay does not have. It is
        // registered (the ops it sends that we lack will be `bad_op`, as before), it is counted,
        // and the WELCOME says OUR level so the peer can name the gap.
        let mut g = Rig::new();
        let newer = leg::hello_flags_with_level(leg::HELLO_FLAG_PEER_KEY, leg::PROTOCOL_LEVEL + 1);
        let out = g.hello_flags(addr(1), leg::ROLE_CLIENT, 0, [0u8; 16], newer);
        let w = welcome_of(&out);
        assert_eq!(leg::welcome_parse_level(&w).unwrap().2, leg::PROTOCOL_LEVEL);
        assert_eq!(g.r.counters.hello_level_mismatch, 1);
        assert_eq!(g.r.counters.peers_registered, 1);
    }

    #[test]
    fn a_hello_naming_a_free_handle_gets_that_handle_back() {
        // After a restart the host re-HELLOs with `src` = the handle it had. Granting it means the
        // client's `other_handle`, pair keys and punch state -- all keyed by that number -- are
        // still right, and the host's per-client loopback socket likewise once the client does the
        // same. A taken number falls through to a fresh allocation.
        let mut g = Rig::new();
        let s = leg::Secrets::derive(&psk());
        let p = leg::hello_payload_flags(leg::ROLE_HOST, leg::HELLO_FLAG_PEER_KEY, &[0u8; 16]);
        let pkt = leg::encode(leg::OP_HELLO, 17, 0, 6501, 0, &p, &s.leg).unwrap();
        let out = g.feed(addr(1), &pkt);
        let w = leg::decode(&out[0].bytes, &s.leg).unwrap();
        assert_eq!(w.op, leg::OP_WELCOME);
        let (me, _) = leg::welcome_parse(&w.payload).unwrap();
        assert_eq!(me, 17, "the requested handle, because it was free");
        assert_eq!(g.r.counters.handles_restored, 1);

        // The client asks for ITS old number and gets it, and is told the host's restored one.
        let p2 = leg::hello_payload_flags(leg::ROLE_CLIENT, leg::HELLO_FLAG_PEER_KEY, &[0u8; 16]);
        let pkt2 = leg::encode(leg::OP_HELLO, 23, 0, 6501, 0, &p2, &s.leg).unwrap();
        let out2 = g.feed(addr(2), &pkt2);
        let w2 = leg::decode(&out2[0].bytes, &s.leg).unwrap();
        let (me2, other2) = leg::welcome_parse(&w2.payload).unwrap();
        assert_eq!((me2, other2), (23, 17));

        assert_eq!(g.r.counters.handles_restored, 2);

        // A HELLO naming a TAKEN number is not a collision case at all: it is the pre-existing
        // "known handle re-announces itself" update (address rebinding, mp:R1b), which is why the
        // relay never has to choose between two claimants -- and why the ordinary allocator has
        // to skip numbers handed out this way. Fresh peers keep getting fresh numbers.
        let fresh = g.join(addr(3), leg::ROLE_CLIENT, 6501);
        assert!(fresh != 17 && fresh != 23);
        assert!(g.r.peers.contains_key(&17) && g.r.peers.contains_key(&23));
        assert_eq!(g.r.counters.handles_restored, 2);
    }

    // ---- mp:R1c: the per-peer leg key ----------------------------------------------------------

    #[test]
    fn a_forged_handle_under_a_different_token_is_refused_and_counted() {
        // THE ITEM'S ACCEPTANCE CLAUSE. Two clients of one host; the second forges the FIRST one's
        // handle. Before R1c that datagram verified -- both hold the deployment PSK -- and the
        // relay adopted the forger's address as the victim's, which redirects the victim's half of
        // the match. Now the tag is checked under the victim's token-derived key.
        let mut g = Rig::new();
        let h = g.join_rekeying(addr(1), leg::ROLE_HOST, 6501);
        let victim = g.join_rekeying(addr(2), leg::ROLE_CLIENT, 6501);
        let forger = g.join_rekeying(addr(3), leg::ROLE_CLIENT, 6501);
        let s = leg::Secrets::derive(&psk());

        let tok = g.grant(h, victim, 6501, 3);
        assert_eq!(
            g.r.counters.peers_rekeyed, 2,
            "one token re-keys both ends of it"
        );
        let victim_key = leg::peer_leg_key(&tok.mac_c2s, true);

        // THE VICTIM SPEAKS ONCE UNDER ITS NEW KEY, which is what closes the ratchet. Until a peer
        // has demonstrated the switch the relay still accepts the shared key from it -- the window
        // between "the relay learned the token while forwarding the grant" and "the client, which
        // learns it FROM that forward, has answered" -- and this is the datagram that ends it.
        let warm = leg::encode(leg::OP_PING, victim, 0, 6501, 8, &[], &victim_key).unwrap();
        assert_eq!(
            g.feed(addr(2), &warm).len(),
            1,
            "the victim is on its own key"
        );

        // The forger holds the deployment PSK -- everything R1 assumed an attacker might -- and
        // uses it to speak as the victim from its own address.
        let bad = leg::encode(leg::OP_PING, victim, 0, 6501, 9, &[], &s.leg).unwrap();
        let before_mac = g.r.counters.leg_bad_mac;
        assert!(g.feed(addr(9), &bad).is_empty(), "nothing is answered");
        assert_eq!(
            g.r.counters.leg_bad_mac,
            before_mac + 1,
            "refused, and counted as leg_bad_mac"
        );
        assert_eq!(
            g.r.peers.get(&victim).map(|p| p.addr),
            Some(addr(2)),
            "and the victim's address binding did not move"
        );

        // A DIFFERENT TOKEN IS NOT THE VICTIM'S KEY EITHER -- holding a session of your own buys
        // nothing, which is what makes this per-PEER rather than merely per-deployment.
        let other = leg::peer_leg_key(&[99u8; KEY_LEN], true);
        assert_ne!(other, victim_key);
        let bad2 = leg::encode(leg::OP_PING, victim, 0, 6501, 10, &[], &other).unwrap();
        assert!(g.feed(addr(9), &bad2).is_empty());
        assert_eq!(g.r.counters.leg_bad_mac, before_mac + 2);

        // And the victim itself, under the key its own token minted, is still served.
        let good = leg::encode(leg::OP_PING, victim, 0, 6501, 11, &[], &victim_key).unwrap();
        assert_eq!(g.feed(addr(2), &good).len(), 1, "a PONG");
        // The forger's own handle was never granted a token, so it still speaks the deployment
        // key: the ratchet is per handle, not per relay.
        let f = leg::encode(leg::OP_PING, forger, 0, 6501, 12, &[], &s.leg).unwrap();
        assert_eq!(g.feed(addr(3), &f).len(), 1);
    }

    #[test]
    fn the_deployment_key_stops_working_for_a_peer_that_has_been_re_keyed() {
        // The ratchet from the re-keyed peer's own side: a session does not ADD a key, it replaces
        // one. Otherwise R1c would buy nothing -- a forger would simply keep using the shared key.
        let mut g = Rig::new();
        let h = g.join_rekeying(addr(1), leg::ROLE_HOST, 6501);
        let c = g.join_rekeying(addr(2), leg::ROLE_CLIENT, 6501);
        let s = leg::Secrets::derive(&psk());
        let tok = g.grant(h, c, 6501, 3);
        let ck0 = leg::peer_leg_key(&tok.mac_c2s, true);

        // BEFORE the client has spoken under its new key the shared one is still taken -- that is
        // the deliberate window, and asserting it is what stops the leniency being removed by
        // accident later and costing a handshake retransmit on every clean run.
        let early = leg::encode(leg::OP_PING, c, 0, 6501, 4, &[], &s.leg).unwrap();
        assert_eq!(g.feed(addr(2), &early).len(), 1, "the window is real");
        // One datagram under the new key closes it.
        let warm = leg::encode(leg::OP_PING, c, 0, 6501, 5, &[], &ck0).unwrap();
        assert_eq!(g.feed(addr(2), &warm).len(), 1);

        let stale = leg::encode(leg::OP_PING, c, 0, 6501, 6, &[], &s.leg).unwrap();
        let before = g.r.counters.leg_bad_mac;
        assert!(g.feed(addr(2), &stale).is_empty());
        assert_eq!(g.r.counters.leg_bad_mac, before + 1);

        // AND WHAT THE RELAY SENDS IS UNDER THAT KEY TOO. A peer that could not read the answer
        // would be off the air, so this is the half that proves the handover is symmetric.
        let ck = ck0;
        let ping = leg::encode(leg::OP_PING, c, 0, 6501, 7, &[], &ck).unwrap();
        let out = g.feed(addr(2), &ping);
        assert_eq!(out.len(), 1);
        assert!(
            leg::decode(&out[0].bytes, &ck).is_ok(),
            "the PONG is tagged under the client's own key"
        );
        assert_eq!(
            leg::decode(&out[0].bytes, &s.leg),
            Err(leg::LegError::BadMac),
            "and not under the deployment key any more"
        );
    }

    #[test]
    fn a_peer_that_does_not_advertise_the_capability_is_never_re_keyed() {
        // BACKWARD COMPATIBILITY AS A TEST, not a hope: an older peer sends flags 0, and ratcheting
        // it would take it off the air with no way back.
        let mut g = Rig::new();
        let h = g.join(addr(1), leg::ROLE_HOST, 6501); // flags 0 -- a pre-R1c build
        let c = g.join(addr(2), leg::ROLE_CLIENT, 6501);
        let s = leg::Secrets::derive(&psk());
        g.grant(h, c, 6501, 3);
        assert_eq!(g.r.counters.peers_rekeyed, 0);
        let ping = leg::encode(leg::OP_PING, c, 0, 6501, 4, &[], &s.leg).unwrap();
        assert_eq!(
            g.feed(addr(2), &ping).len(),
            1,
            "still served on the shared key"
        );
    }

    #[test]
    fn a_departed_clients_key_leaves_the_host_and_the_tenth_token_never_evicts_the_first() {
        // mp:R3e, the relay-side half. A lobby that clients join and leave over its life hands the
        // host one leg key per token; before R3e the key stayed after the client left, and the
        // TENTH token's FIFO eviction took `keys[0]` -- the host's own send key -- so a host with
        // its tenth-ever joiner silently lost every relay-directed datagram. Ten sequential
        // clients here, each granted, each then gone: the host's key set never grows past two
        // (keys[0] plus the live client's), `keys[0]` is the first client's to the end, and a host
        // PING under it is still answered under it.
        let mut g = Rig::new();
        let h = g.join_rekeying(addr(1), leg::ROLE_HOST, 6501);
        let s = leg::Secrets::derive(&psk());
        let mut first: Option<[u8; KEY_LEN]> = None;
        for n in 0..10u8 {
            let caddr = addr(10 + n as u16);
            let c = g.join_rekeying(caddr, leg::ROLE_CLIENT, 6501);
            // A distinct token per client, as a real host mints: its own conn_id and MAC keys.
            let tok = wire::ConnectToken {
                match_id: [0xa5u8; 16],
                conn_id: [n, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x03],
                slot: 3,
                expire_unix_ms: 1_900_000_000_000,
                enc_c2s: [10u8; KEY_LEN],
                enc_s2c: [11u8; KEY_LEN],
                mac_c2s: [12u8.wrapping_add(n); KEY_LEN],
                mac_s2c: [13u8.wrapping_add(n); KEY_LEN],
            };
            let sealed = wire::token_seal(&psk(), 0x1234 + n as u64, &tok).unwrap();
            let mut frame = vec![HSK_GRANT];
            frame.extend_from_slice(&sealed);
            let mut body = Vec::new();
            assert!(wire::frame_append(&mut body, CH_HS, &frame));
            let boot = wire::packet_encode(
                wire::PKT_DATA,
                &s.boot_conn,
                7 + n as u64,
                &s.boot_enc,
                &s.boot_mac,
                &body,
            )
            .unwrap();
            // The host tags under the key it has: the deployment key for its first grant, `first`
            // after that (the ratchet, as the three-peer test above states).
            let tag = first.unwrap_or(s.leg);
            let pkt = leg::encode(leg::OP_DATA, h, c, 6501, 100 + n as u64, &boot, &tag).unwrap();
            g.feed(addr(1), &pkt);
            let k_host = leg::peer_leg_key(&tok.mac_s2c, false);
            if first.is_none() {
                first = Some(k_host);
            }
            let keys = &g.r.peers.get(&h).unwrap().keys;
            assert_eq!(
                keys.first(),
                first.as_ref(),
                "client {n}: keys[0] is the first key"
            );
            assert!(keys.contains(&k_host), "client {n}: its key is installed");
            assert!(
                keys.len() <= 2,
                "client {n}: never more than keys[0] + the live client"
            );

            // The client leaves. Its key leaves the host's set with it -- unless it IS keys[0].
            let bye = leg::encode(leg::OP_BYE, c, 0, 6501, 2, &[], &s.leg).unwrap();
            g.feed(caddr, &bye);
            let keys = &g.r.peers.get(&h).unwrap().keys;
            assert_eq!(
                keys.first(),
                first.as_ref(),
                "client {n} gone: keys[0] untouched"
            );
            if n > 0 {
                assert!(
                    !keys.contains(&k_host),
                    "client {n} gone: its key went with it"
                );
            }
        }
        // And the host is still on the air under the key it has used since its first client.
        let ping = leg::encode(leg::OP_PING, h, 0, 6501, 500, &[], &first.unwrap()).unwrap();
        let out = g.feed(addr(1), &ping);
        assert_eq!(
            out.len(),
            1,
            "the host's PING still verifies after ten tokens"
        );
        assert!(
            leg::decode(&out[0].bytes, &first.unwrap()).is_ok(),
            "and the PONG is tagged under keys[0], the key the host verifies with"
        );
    }

    #[test]
    fn a_hosts_first_key_stays_first_however_its_clients_talk() {
        // THE THREE-PEER TRAP, and it is a regression before it is a feature. A host is an end of
        // one connection per client, so it owns several leg keys -- and what the relay SENDS it is
        // tagged with `keys[0]`, while the host verifies against the first key IT installed. A
        // move-to-front on receipt (the obvious optimisation for the multi-key walk) silently
        // repoints the first of those and not the second, which a two-peer match cannot see and a
        // three-peer one fails on entirely: every relay PING, WELCOME and forwarded datagram to
        // the host stops verifying.
        let mut g = Rig::new();
        let h = g.join_rekeying(addr(1), leg::ROLE_HOST, 6501);
        let c1 = g.join_rekeying(addr(2), leg::ROLE_CLIENT, 6501);
        let c2 = g.join_rekeying(addr(3), leg::ROLE_CLIENT, 6501);

        let t1 = g.grant(h, c1, 6501, 3);
        let first = leg::peer_leg_key(&t1.mac_s2c, false);
        assert_eq!(g.r.peers.get(&h).unwrap().keys.first(), Some(&first));

        // A SECOND client's token gives the host a second key. Nothing about the first may move.
        let mut g2 = Rig::new();
        let _ = &mut g2; // (a second rig is not needed; kept explicit that the state below is g's)
        let t2 = {
            // A distinct token: another conn_id and another key pair, as a second client's is.
            let s = leg::Secrets::derive(&psk());
            let tok = wire::ConnectToken {
                match_id: [0xa5u8; 16],
                conn_id: [0x99u8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x04],
                slot: 4,
                expire_unix_ms: 1_900_000_000_000,
                enc_c2s: [20u8; KEY_LEN],
                enc_s2c: [21u8; KEY_LEN],
                mac_c2s: [22u8; KEY_LEN],
                mac_s2c: [23u8; KEY_LEN],
            };
            let sealed = wire::token_seal(&psk(), 0x4321, &tok).unwrap();
            let mut frame = vec![HSK_GRANT];
            frame.extend_from_slice(&sealed);
            let mut body = Vec::new();
            assert!(wire::frame_append(&mut body, CH_HS, &frame));
            let boot = wire::packet_encode(
                wire::PKT_DATA,
                &s.boot_conn,
                8,
                &s.boot_enc,
                &s.boot_mac,
                &body,
            )
            .unwrap();
            // UNDER `first`, NOT THE DEPLOYMENT KEY, and the distinction is the real system's:
            // the host is already re-keyed by the time it grants its SECOND client, so it tags
            // with the key it has. (An earlier draft of this test used the shared key here and the
            // grant was correctly refused -- which is the ratchet working, not a test fixture
            // detail.) The relay's forward of it still reaches c2 on the deployment key, because
            // c2 has no key until it reads this very payload.
            let pkt = leg::encode(leg::OP_DATA, h, c2, 6501, 4, &boot, &first).unwrap();
            let _ = &s;
            g.feed(addr(1), &pkt);
            tok
        };
        let second = leg::peer_leg_key(&t2.mac_s2c, false);
        assert_ne!(first, second);
        assert_eq!(g.r.peers.get(&h).unwrap().keys.len(), 2);

        // Now make the host speak under its SECOND key -- a datagram for client 2, which is the
        // ordinary case and the one that would trigger a move-to-front.
        let ping = leg::encode(leg::OP_PING, h, 0, 6501, 5, &[], &second).unwrap();
        let out = g.feed(addr(1), &ping);
        assert_eq!(out.len(), 1, "accepted under either of its keys");
        assert!(
            leg::decode(&out[0].bytes, &first).is_ok(),
            "and the ANSWER is still tagged under the host's first key"
        );
    }

    #[test]
    fn the_grant_itself_still_crosses_on_the_old_key() {
        // THE ORDERING, which is the one thing here that cannot be repaired by a retry: the client
        // derives its key FROM this datagram, so a relay that installed before forwarding would
        // send the grant under a key only the grant can produce. A deadlock, not a race.
        let mut g = Rig::new();
        let h = g.join_rekeying(addr(1), leg::ROLE_HOST, 6501);
        let c = g.join_rekeying(addr(2), leg::ROLE_CLIENT, 6501);
        let before = g.r.counters.forwarded;
        let tok = g.grant(h, c, 6501, 3);
        assert_eq!(g.r.counters.forwarded, before + 1, "the grant crossed");
        let hk = leg::peer_leg_key(&tok.mac_s2c, false);
        let next = leg::encode(leg::OP_PING, h, 0, 6501, 99, &[], &hk).unwrap();
        assert_eq!(
            g.feed(addr(1), &next).len(),
            1,
            "and the host's NEXT datagram is on the new key"
        );
    }

    #[test]
    fn a_host_and_a_client_pair_up_and_data_crosses() {
        let mut g = Rig::new();
        let h = g.join(addr(1), leg::ROLE_HOST, 6501);
        let c = g.join(addr(2), leg::ROLE_CLIENT, 6501);
        assert_ne!(h, c);

        // A T0 datagram the relay has no keys for: unverified, and forwarded by default.
        let s = leg::Secrets::derive(&psk());
        let inner = wire::packet_encode(
            wire::PKT_DATA,
            &[9u8; 8],
            1,
            &[1u8; KEY_LEN],
            &[2u8; KEY_LEN],
            b"hello",
        )
        .unwrap();
        let pkt = leg::encode(leg::OP_DATA, c, h, 6501, 2, &inner, &s.leg).unwrap();
        let out = g.feed(addr(2), &pkt);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].to, addr(1));
        let got = leg::decode(&out[0].bytes, &s.leg).unwrap();
        assert_eq!(got.op, leg::OP_DATA);
        assert_eq!(got.src, c);
        assert_eq!(got.payload, inner, "the T0 datagram crosses byte for byte");
        assert_eq!(g.r.counters.forwarded, 1);
        assert_eq!(g.r.counters.inner_unverified, 1);
    }

    #[test]
    fn an_unregistered_handle_is_dropped_and_counted() {
        let mut g = Rig::new();
        g.join(addr(1), leg::ROLE_HOST, 6501);
        let s = leg::Secrets::derive(&psk());
        // Well-formed, correctly tagged, and from an address that never registered.
        let pkt = leg::encode(leg::OP_DATA, 4242, 1, 6501, 1, &[0u8; 40], &s.leg).unwrap();
        let out = g.feed(addr(9), &pkt);
        assert_eq!(g.r.counters.unregistered, 1);
        assert_eq!(g.r.counters.forwarded, 0);
        // The refusal itself obeys the cap: 40+34 in, one small ERROR out.
        assert!(out.len() <= 1);
        assert!(out.iter().all(|o| o.to == addr(9)));
    }

    #[test]
    fn garbage_is_refused_by_reason_not_by_a_bare_drop() {
        let mut g = Rig::new();
        g.feed(
            addr(3),
            b"not ours at all, but long enough to reach the tag check.....",
        );
        assert_eq!(g.r.counters.leg_bad_magic, 1);
        g.feed(addr(3), &[0u8; 4]);
        assert_eq!(g.r.counters.leg_too_short, 1);
        let s = leg::Secrets::derive(&psk());
        let mut bad = leg::encode(leg::OP_PING, 0, 0, 1, 0, &[], &s.leg).unwrap();
        let n = bad.len();
        bad[n - 1] ^= 0xff;
        g.feed(addr(3), &bad);
        assert_eq!(g.r.counters.leg_bad_mac, 1);
    }

    #[test]
    fn an_unvalidated_address_never_gets_more_than_three_times_what_it_sent() {
        // One HELLO in, and then nothing else from that address ever. The relay must not be usable
        // as an amplifier, so we ask it for far more than 3x and count what it would send.
        let mut g = Rig::new();
        g.join(addr(1), leg::ROLE_HOST, 6501);
        let before = g.r.counters.bytes_tx;
        let s = leg::Secrets::derive(&psk());
        let mut sent_in = 0u64;
        // The client registers but never answers, so it stays unvalidated; repeated HELLOs are the
        // only inbound bytes it contributes.
        let p = leg::hello_payload(leg::ROLE_CLIENT, &[0u8; 16]);
        for i in 0..20u64 {
            let pkt = leg::encode(leg::OP_HELLO, 0, 0, 6501, i, &p, &s.leg).unwrap();
            sent_in += pkt.len() as u64;
            g.feed(addr(2), &pkt);
        }
        let to_client: u64 = g.r.counters.bytes_tx - before;
        assert!(
            to_client <= AMPLIFICATION_FACTOR * sent_in,
            "sent {to_client} for {sent_in} received"
        );
    }

    #[test]
    fn a_learned_conn_refuses_a_bad_mac_and_forwards_a_good_one() {
        let mut g = Rig::new();
        let h = g.join(addr(1), leg::ROLE_HOST, 6501);
        let c = g.join(addr(2), leg::ROLE_CLIENT, 6501);
        let s = leg::Secrets::derive(&psk());

        // The host grants a token, exactly as udp_endpoint.cpp's datagram 2 does: a CH_HS frame of
        // kind GRANT inside a body sealed under the boot keys.
        let conn_id = [0x11u8, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x03];
        let tok = wire::ConnectToken {
            match_id: [0xa5u8; 16],
            conn_id,
            slot: 3,
            expire_unix_ms: 1_900_000_000_000,
            enc_c2s: [10u8; KEY_LEN],
            enc_s2c: [11u8; KEY_LEN],
            mac_c2s: [12u8; KEY_LEN],
            mac_s2c: [13u8; KEY_LEN],
        };
        let sealed = wire::token_seal(&psk(), 0x1234, &tok).unwrap();
        let mut frame = vec![HSK_GRANT];
        frame.extend_from_slice(&sealed);
        let mut body = Vec::new();
        assert!(wire::frame_append(&mut body, CH_HS, &frame));
        let boot = wire::packet_encode(
            wire::PKT_DATA,
            &s.boot_conn,
            7,
            &s.boot_enc,
            &s.boot_mac,
            &body,
        )
        .unwrap();
        let pkt = leg::encode(leg::OP_DATA, h, c, 6501, 3, &boot, &s.leg).unwrap();
        let out = g.feed(addr(1), &pkt);
        assert_eq!(out.len(), 1, "the grant itself is still forwarded");
        assert_eq!(g.r.counters.tokens_learned, 1);

        // The client now speaks on that conn, correctly signed under mac_c2s: forwarded.
        let good = wire::packet_encode(
            wire::PKT_DATA,
            &conn_id,
            1,
            &tok.enc_c2s,
            &tok.mac_c2s,
            b"step inputs",
        )
        .unwrap();
        let pkt = leg::encode(leg::OP_DATA, c, h, 6501, 4, &good, &s.leg).unwrap();
        let before = g.r.counters.forwarded;
        assert_eq!(g.feed(addr(2), &pkt).len(), 1);
        assert_eq!(g.r.counters.forwarded, before + 1);
        assert_eq!(g.r.counters.inner_bad_mac, 0);

        // The same client, one bit flipped in the sealed body: the tag no longer verifies, and the
        // relay drops it rather than passing the forgery on.
        let mut forged = good.clone();
        forged[wire::HDR_SIZE] ^= 0x01;
        let pkt = leg::encode(leg::OP_DATA, c, h, 6501, 5, &forged, &s.leg).unwrap();
        assert!(g.feed(addr(2), &pkt).is_empty());
        assert_eq!(g.r.counters.inner_bad_mac, 1);
        assert_eq!(g.r.counters.forwarded, before + 1, "nothing else crossed");
    }

    #[test]
    fn a_rebinding_keeps_the_peer_and_the_replay_window_holds() {
        let mut g = Rig::new();
        g.join(addr(1), leg::ROLE_HOST, 6501);
        let c = g.join(addr(2), leg::ROLE_CLIENT, 6501);
        let s = leg::Secrets::derive(&psk());
        // Same handle, new address: adopted, and the peer count does not grow.
        let pkt = leg::encode(leg::OP_PING, c, 0, 6501, 9, &[], &s.leg).unwrap();
        g.feed(addr(22), &pkt);
        assert_eq!(g.r.peer_count(), 2);
        assert!(g
            .ev
            .iter()
            .any(|e| matches!(e, Event::PeerRebound { handle, .. } if *handle == c)));
        // A replay of that very datagram is refused by the window.
        let before = g.r.counters.replay;
        g.feed(addr(22), &pkt);
        assert_eq!(g.r.counters.replay, before + 1);
    }

    #[test]
    fn a_second_host_cannot_take_an_occupied_room() {
        let mut g = Rig::new();
        g.join(addr(1), leg::ROLE_HOST, 6501);
        g.hello(addr(5), leg::ROLE_HOST, 6501, [0u8; 16]);
        assert!(g
            .ev
            .iter()
            .any(|e| matches!(e, Event::Refused { why, .. } if *why == "room_busy")));
        assert_eq!(g.r.peer_count(), 1);
    }

    #[test]
    fn a_client_with_no_host_is_told_so_rather_than_parked() {
        let mut g = Rig::new();
        g.hello(addr(2), leg::ROLE_CLIENT, 6501, [0u8; 16]);
        assert!(g
            .ev
            .iter()
            .any(|e| matches!(e, Event::Refused { why, .. } if *why == "no_host")));
        assert_eq!(g.r.peer_count(), 0);
        assert_eq!(g.r.room_count(), 0, "an empty room does not linger");
    }

    #[test]
    fn a_hello_update_carries_the_match_id_in_after_the_link_is_up() {
        let mut g = Rig::new();
        let h = g.join(addr(1), leg::ROLE_HOST, 6501);
        let mid = [0x7au8; 16];
        let s = leg::Secrets::derive(&psk());
        let p = leg::hello_payload(leg::ROLE_HOST, &mid);
        let pkt = leg::encode(leg::OP_HELLO, h, 0, 6501, 5, &p, &s.leg).unwrap();
        g.feed(addr(1), &pkt);
        assert_eq!(
            g.r.peer_count(),
            1,
            "an update is not a second registration"
        );
        assert!(g
            .ev
            .iter()
            .any(|e| matches!(e, Event::MatchId { match_id, .. } if *match_id == hex(&mid))));
    }

    #[test]
    fn an_idle_peer_is_pinged_and_then_evicted() {
        let mut g = Rig::new();
        g.join(addr(1), leg::ROLE_HOST, 6501);
        g.now += Duration::from_secs(30);
        let out = g.r.tick(g.now, &mut g.ev);
        assert_eq!(out.len(), 1, "the quiet peer is pinged at 25 s");
        g.now += Duration::from_secs(40);
        g.r.tick(g.now, &mut g.ev);
        assert_eq!(g.r.peer_count(), 0);
        assert_eq!(g.r.room_count(), 0);
    }

    #[test]
    fn a_hello_naming_a_different_room_re_homes_the_peer() {
        // mp:R1b. A known handle's HELLO naming a room other than the one it is currently in must
        // move it there, not just refresh its address/match_id and leave it stranded in the old
        // room.
        let mut g = Rig::new();
        let h1 = g.join(addr(1), leg::ROLE_HOST, 6501);
        let c = g.join(addr(2), leg::ROLE_CLIENT, 6501);
        let h2 = g.join(addr(3), leg::ROLE_HOST, 6502);
        assert_eq!(g.r.room_count(), 2);

        let s = leg::Secrets::derive(&psk());
        let p = leg::hello_payload(leg::ROLE_CLIENT, &[0u8; 16]);
        let pkt = leg::encode(leg::OP_HELLO, c, 0, 6502, 9, &p, &s.leg).unwrap();
        g.feed(addr(2), &pkt);

        assert!(g.ev.iter().any(|e| matches!(
            e,
            Event::Rehomed { handle, from_room, to_room }
                if *handle == c && *from_room == 6501 && *to_room == 6502
        )));
        // Moved, not re-registered: the relay-wide peer count is unchanged, and 6501 survives
        // because its host is still there -- only the client's departure is what drops.
        assert_eq!(g.r.peer_count(), 3);
        assert_eq!(g.r.room_count(), 2);

        // The OLD room no longer forwards to it: the host of 6501 addressing the client's (still
        // valid) handle now gets "not in this room", not a forward.
        let before_fwd = g.r.counters.forwarded;
        let inner = wire::packet_encode(
            wire::PKT_DATA,
            &[1u8; 8],
            1,
            &[1u8; KEY_LEN],
            &[2u8; KEY_LEN],
            b"stale",
        )
        .unwrap();
        let stale = leg::encode(leg::OP_DATA, h1, c, 6501, 2, &inner, &s.leg).unwrap();
        let out = g.feed(addr(1), &stale);
        assert_eq!(g.r.counters.not_in_peer_set, 1);
        assert_eq!(
            g.r.counters.forwarded, before_fwd,
            "nothing crossed into the old room"
        );
        assert!(
            out.iter().all(|o| o.to == addr(1)),
            "only the ERROR reply, back to the sender"
        );

        // The NEW room does forward to it: the client's own DATA now reaches its new host.
        let inner2 = wire::packet_encode(
            wire::PKT_DATA,
            &[2u8; 8],
            1,
            &[1u8; KEY_LEN],
            &[2u8; KEY_LEN],
            b"fresh",
        )
        .unwrap();
        let fresh = leg::encode(leg::OP_DATA, c, h2, 6502, 3, &inner2, &s.leg).unwrap();
        let out2 = g.feed(addr(2), &fresh);
        assert_eq!(out2.len(), 1);
        assert_eq!(out2[0].to, addr(3));
        assert_eq!(g.r.counters.forwarded, before_fwd + 1);
    }

    #[test]
    fn a_rehome_that_empties_the_old_room_closes_it() {
        // The other half of mp:R1b's done_when: an emptied room is removed the same way eviction
        // removes one.
        let mut g = Rig::new();
        let h = g.join(addr(1), leg::ROLE_HOST, 6501);
        assert_eq!(g.r.room_count(), 1);

        let s = leg::Secrets::derive(&psk());
        let p = leg::hello_payload(leg::ROLE_HOST, &[0u8; 16]);
        let pkt = leg::encode(leg::OP_HELLO, h, 0, 7001, 9, &p, &s.leg).unwrap();
        g.feed(addr(1), &pkt);

        assert_eq!(g.r.peer_count(), 1, "moved, not duplicated");
        assert_eq!(g.r.room_count(), 1, "6501 closed, 7001 opened");
        assert_eq!(g.r.counters.rooms_closed, 1);
        assert!(g
            .ev
            .iter()
            .any(|e| matches!(e, Event::RoomClosed { room } if *room == 6501)));
        assert!(g.ev.iter().any(|e| matches!(
            e,
            Event::Rehomed { handle, from_room, to_room }
                if *handle == h && *from_room == 6501 && *to_room == 7001
        )));
    }

    // ---- mp:R2, the session directory -----------------------------------------------------------

    impl Rig {
        /// Any leg op from an already-registered peer. `seq` is the caller's because the replay
        /// window is per peer and a test that re-used one would be refused for the right reason
        /// at the wrong moment.
        fn op(
            &mut self,
            from: SocketAddr,
            h: u16,
            room: u32,
            seq: u64,
            op: u8,
            p: &[u8],
        ) -> Vec<Out> {
            let s = leg::Secrets::derive(&psk());
            let pkt = leg::encode(op, h, 0, room, seq, p, &s.leg).unwrap();
            self.feed(from, &pkt)
        }
        /// The directory as THIS peer would parse it out of the pages a LIST returns.
        fn list(&mut self, from: SocketAddr, h: u16, room: u32, seq: u64) -> Vec<(u32, Vec<u8>)> {
            let s = leg::Secrets::derive(&psk());
            let outs = self.op(from, h, room, seq, leg::OP_LIST, &[]);
            let mut all = Vec::new();
            for o in outs {
                let l = leg::decode(&o.bytes, &s.leg).unwrap();
                assert_eq!(l.op, leg::OP_SESSIONS);
                let (_total, _off, entries) = leg::sessions_parse(&l.payload).unwrap();
                all.extend(entries);
            }
            all
        }
    }

    #[test]
    fn a_host_publishes_a_lobby_and_a_browser_in_the_directory_room_lists_it() {
        let mut g = Rig::new();
        let h = g.join(addr(1), leg::ROLE_HOST, 6501);
        // THE BROWSER NAMES NO GAME ROOM. Before R2 this registration was refused `no_host`, which
        // is the state a client is in before it knows any lobby code at all.
        let b = g.join(addr(9), leg::ROLE_CLIENT, DIRECTORY_ROOM);
        assert_eq!(g.list(addr(9), b, DIRECTORY_ROOM, 2), Vec::new());

        let desc = b"SESSION_INFO bytes, opaque to the relay".to_vec();
        g.op(addr(1), h, 6501, 2, leg::OP_REGISTER, &desc);
        assert_eq!(g.r.counters.sessions_registered, 1);

        let got = g.list(addr(9), b, DIRECTORY_ROOM, 3);
        assert_eq!(
            got,
            vec![(6501u32, desc.clone())],
            "verbatim, and with its room"
        );

        // A REFRESH IS NOT A SECOND SESSION, and it does not duplicate the row.
        g.op(addr(1), h, 6501, 3, leg::OP_REGISTER, &desc);
        assert_eq!(g.r.counters.sessions_registered, 1);
        assert_eq!(g.list(addr(9), b, DIRECTORY_ROOM, 4).len(), 1);
    }

    #[test]
    fn the_row_vanishes_when_the_host_withdraws_it_and_when_it_goes_stale() {
        let mut g = Rig::new();
        let h = g.join(addr(1), leg::ROLE_HOST, 6501);
        let b = g.join(addr(9), leg::ROLE_CLIENT, DIRECTORY_ROOM);
        g.op(addr(1), h, 6501, 2, leg::OP_REGISTER, b"lobby");
        assert_eq!(g.list(addr(9), b, DIRECTORY_ROOM, 2).len(), 1);

        // (a) the ordinary path: the host leaves its lobby and says so.
        g.op(addr(1), h, 6501, 3, leg::OP_UNREGISTER, &[]);
        assert_eq!(g.r.counters.sessions_unregistered, 1);
        assert_eq!(g.list(addr(9), b, DIRECTORY_ROOM, 4), Vec::new());
        assert!(g
            .ev
            .iter()
            .any(|e| matches!(e, Event::SessionUnregistered { room, why } if *room == 6501 && *why == "withdrawn")));

        // (b) the host that vanished: no withdrawal, no refresh. The TTL is SHORTER than the peer
        // eviction idle, so the row goes while the host is still a registered peer -- which is the
        // property the two different numbers exist for.
        g.op(addr(1), h, 6501, 5, leg::OP_REGISTER, b"lobby again");
        assert_eq!(g.list(addr(9), b, DIRECTORY_ROOM, 6).len(), 1);
        g.now += Limits::default().session_ttl + Duration::from_secs(1);
        let mut ev = Vec::new();
        g.r.tick(g.now, &mut ev);
        assert_eq!(g.r.counters.sessions_expired, 1);
        assert_eq!(
            g.r.peer_count(),
            2,
            "the host is still a peer; only its row went"
        );
        assert!(ev
            .iter()
            .any(|e| matches!(e, Event::SessionUnregistered { room, why } if *room == 6501 && *why == "expired")));
    }

    #[test]
    fn only_a_rooms_own_host_may_publish_or_withdraw_its_descriptor() {
        let mut g = Rig::new();
        let h = g.join(addr(1), leg::ROLE_HOST, 6501);
        let c = g.join(addr(2), leg::ROLE_CLIENT, 6501);
        g.op(addr(1), h, 6501, 2, leg::OP_REGISTER, b"the host's lobby");

        // A CLIENT of the same room cannot advertise it, and cannot take the row down either.
        g.op(
            addr(2),
            c,
            6501,
            2,
            leg::OP_REGISTER,
            b"not mine to publish",
        );
        assert_eq!(g.r.counters.register_refused, 1);
        g.op(addr(2), c, 6501, 3, leg::OP_UNREGISTER, &[]);
        assert_eq!(g.r.counters.sessions_unregistered, 0);
        assert_eq!(
            g.list(addr(2), c, 6501, 4),
            vec![(6501u32, b"the host's lobby".to_vec())],
            "still the host's bytes"
        );

        // An over-long descriptor is refused for its size rather than truncated into a row a peer
        // would then fail to decode.
        g.op(
            addr(1),
            h,
            6501,
            3,
            leg::OP_REGISTER,
            &vec![0u8; leg::DESC_MAX + 1],
        );
        assert_eq!(g.r.counters.register_refused, 2);
        assert_eq!(g.list(addr(2), c, 6501, 5).len(), 1);
    }

    #[test]
    fn a_host_that_leaves_takes_its_row_with_it_by_every_route() {
        for (why, bye) in [("said goodbye", true), ("idle", false)] {
            let mut g = Rig::new();
            let h = g.join(addr(1), leg::ROLE_HOST, 6501);
            let b = g.join(addr(9), leg::ROLE_CLIENT, DIRECTORY_ROOM);
            g.op(addr(1), h, 6501, 2, leg::OP_REGISTER, b"lobby");
            assert_eq!(g.list(addr(9), b, DIRECTORY_ROOM, 2).len(), 1);
            let asker = if bye {
                g.op(addr(1), h, 6501, 3, leg::OP_BYE, &[]);
                b
            } else {
                // The silence that evicts the host evicts the browser too, so the reader here is a
                // browser that arrives AFTER the eviction -- which is the real shape anyway: a
                // player opening the browser once the host's machine has gone.
                g.now += Limits::default().idle + Duration::from_secs(1);
                let mut ev = Vec::new();
                g.r.tick(g.now, &mut ev);
                assert_eq!(g.r.peer_count(), 0, "both peers were silent");
                g.join(addr(9), leg::ROLE_CLIENT, DIRECTORY_ROOM)
            };
            assert_eq!(
                g.list(addr(9), asker, DIRECTORY_ROOM, 4),
                Vec::new(),
                "{why}"
            );
        }
    }

    #[test]
    fn a_directory_bigger_than_one_answer_is_served_across_successive_lists() {
        // Seven lobbies of 400 B each: more than LIST_MAX_PAGES can carry in one answer, so a
        // fixed truncation would show the same first rooms forever and the last ones never.
        let mut g = Rig::new();
        let desc = vec![0x5au8; 400];
        for i in 0..7u16 {
            let room = 6501 + u32::from(i);
            let h = g.join(addr(100 + i), leg::ROLE_HOST, room);
            g.op(addr(100 + i), h, room, 2, leg::OP_REGISTER, &desc);
        }
        assert_eq!(g.r.counters.sessions_registered, 7);
        let b = g.join(addr(9), leg::ROLE_CLIENT, DIRECTORY_ROOM);

        let mut seen: Vec<u32> = Vec::new();
        for seq in 2..8 {
            for (room, bytes) in g.list(addr(9), b, DIRECTORY_ROOM, seq) {
                assert_eq!(bytes, desc);
                if !seen.contains(&room) {
                    seen.push(room);
                }
            }
            if seen.len() == 7 {
                break;
            }
        }
        seen.sort_unstable();
        assert_eq!(
            seen,
            (6501..6508).collect::<Vec<u32>>(),
            "every lobby was reachable"
        );
    }

    #[test]
    fn nobody_hosts_the_directory_room() {
        let mut g = Rig::new();
        g.hello(addr(1), leg::ROLE_HOST, DIRECTORY_ROOM, [0u8; 16]);
        assert_eq!(g.r.peer_count(), 0);
        assert!(g
            .ev
            .iter()
            .any(|e| matches!(e, Event::Refused { why, .. } if *why == "host_in_directory_room")));
    }

    // ---- mp:R6, the host's room is minted, not its port -----------------------------------------
    //
    // Nothing in the relay changed for R6 -- a room is a u32 it never interprets -- and these are
    // the tests that say the relay's side of the contract already holds: two hosts under DISTINCT
    // codes coexist and are both listed, a host refused `room_busy` that re-HELLOs under a fresh
    // code is admitted with no residue of the refusal, and a host relaunched inside the idle window
    // after a crash (its stale slot still registered) is admitted under a fresh code where the
    // SAME code would have been refused. The C++ half -- that the codes ARE distinct, and that a
    // refused host re-mints -- is `net_selftest.exe udproomtest`.

    // Two 30-bit codes of the kind udp_room.h mints. Chosen to share nothing with 6501, the number
    // that used to be every host's room.
    const ROOM_A: u32 = 0x2A7F_1C03;
    const ROOM_B: u32 = 0x0193_E5D1;

    fn refused_room_busy(ev: &[Event]) -> usize {
        ev.iter()
            .filter(|e| matches!(e, Event::Refused { why, .. } if *why == "room_busy"))
            .count()
    }

    #[test]
    fn two_hosts_with_the_same_port_but_minted_rooms_both_register_and_both_list() {
        let mut g = Rig::new();
        // Both hosts bind `[net] port` 6501 on their own machines; neither names it as a room.
        let ha = g.join(addr(1), leg::ROLE_HOST, ROOM_A);
        let hb = g.join(addr(2), leg::ROLE_HOST, ROOM_B);
        assert_eq!(g.r.peer_count(), 2, "both hosts registered");
        assert_eq!(g.r.room_count(), 2, "in two rooms");
        assert_eq!(refused_room_busy(&g.ev), 0, "no room_busy for either");

        g.op(addr(1), ha, ROOM_A, 2, leg::OP_REGISTER, b"lobby A");
        g.op(addr(2), hb, ROOM_B, 2, leg::OP_REGISTER, b"lobby B");
        assert_eq!(g.r.counters.sessions_registered, 2);
        assert_eq!(g.r.counters.register_refused, 0);

        // A browser lists BOTH, each under its own room -- the code a join dials.
        let b = g.join(addr(9), leg::ROLE_CLIENT, DIRECTORY_ROOM);
        let mut got = g.list(addr(9), b, DIRECTORY_ROOM, 2);
        got.sort();
        let mut want = vec![(ROOM_A, b"lobby A".to_vec()), (ROOM_B, b"lobby B".to_vec())];
        want.sort();
        assert_eq!(got, want, "two rows, two distinct room codes");

        // ...and a client joins EACH by the room its row carries, landing with the right host.
        let ca = g.join(addr(11), leg::ROLE_CLIENT, ROOM_A);
        let cb = g.join(addr(12), leg::ROLE_CLIENT, ROOM_B);
        assert_eq!(g.r.counterpart(ca), ha);
        assert_eq!(g.r.counterpart(cb), hb);
        assert_eq!(g.r.counters.register_refused, 0);
        assert_eq!(refused_room_busy(&g.ev), 0);
    }

    #[test]
    fn a_host_refused_room_busy_re_hellos_under_a_fresh_room_and_is_admitted() {
        let mut g = Rig::new();
        let ha = g.join(addr(1), leg::ROLE_HOST, ROOM_A);
        // The 2^-30 collision (or a hostile relay's answer to everything): the second host's first
        // draw is the first host's room.
        g.hello(addr(2), leg::ROLE_HOST, ROOM_A, [0u8; 16]);
        assert_eq!(refused_room_busy(&g.ev), 1);
        assert_eq!(g.r.peer_count(), 1, "the refused host holds nothing");
        assert_eq!(g.r.room_count(), 1, "and left no room behind");

        // udp_relay.cpp answers that with a re-mint and a re-HELLO under the fresh code, from the
        // same address, still with no handle.
        let hb = g.join(addr(2), leg::ROLE_HOST, ROOM_B);
        assert_ne!(hb, ha);
        assert_eq!(g.r.peer_count(), 2);
        assert_eq!(g.r.room_count(), 2);
        assert_eq!(
            refused_room_busy(&g.ev),
            1,
            "the re-HELLO under the fresh code is not refused"
        );
        // The first host is undisturbed: still the host of ROOM_A, a client still lands with it.
        let ca = g.join(addr(11), leg::ROLE_CLIENT, ROOM_A);
        assert_eq!(g.r.counterpart(ca), ha);
        let cb = g.join(addr(12), leg::ROLE_CLIENT, ROOM_B);
        assert_eq!(g.r.counterpart(cb), hb);
    }

    #[test]
    fn a_host_relaunched_inside_the_idle_window_is_admitted_under_a_fresh_room() {
        let mut g = Rig::new();
        // The first run: a host registers, publishes, and then DIES -- no BYE, no withdrawal. Its
        // slot stays for `idle`, its row for `session_ttl`.
        let stale = g.join(addr(1), leg::ROLE_HOST, ROOM_A);
        g.op(
            addr(1),
            stale,
            ROOM_A,
            2,
            leg::OP_REGISTER,
            b"lobby, first run",
        );
        g.now += Duration::from_secs(5); // well inside idle (60 s) and session_ttl (20 s)
        let mut ev = Vec::new();
        g.r.tick(g.now, &mut ev);
        assert_eq!(g.r.peer_count(), 1, "the stale slot is still there");

        // The relaunch, from a fresh ephemeral port. WITH THE PORT AS THE ROOM this was the VPS
        // measurement: `room_busy` against its own stale slot until eviction. Stated here so the
        // next test's admission is read against it.
        g.hello(addr(2), leg::ROLE_HOST, ROOM_A, [0u8; 16]);
        assert_eq!(
            refused_room_busy(&g.ev),
            1,
            "the same code IS refused by the stale slot"
        );
        assert_eq!(g.r.peer_count(), 1);

        // With a MINTED room the relaunched process names a fresh code and is admitted at once --
        // the stale slot and its row age out beside it on their own timers.
        let fresh = g.join(addr(2), leg::ROLE_HOST, ROOM_B);
        assert_ne!(fresh, stale);
        assert_eq!(g.r.peer_count(), 2);
        assert_eq!(refused_room_busy(&g.ev), 1, "no further refusal");
        g.op(
            addr(2),
            fresh,
            ROOM_B,
            2,
            leg::OP_REGISTER,
            b"lobby, relaunched",
        );
        assert_eq!(g.r.counters.register_refused, 0);

        // A browser sees the live lobby now, beside the stale row until the TTL takes that one.
        let b = g.join(addr(9), leg::ROLE_CLIENT, DIRECTORY_ROOM);
        let got = g.list(addr(9), b, DIRECTORY_ROOM, 2);
        assert!(got.contains(&(ROOM_B, b"lobby, relaunched".to_vec())));
        // The crashed run's row is 5 s older than the relaunched one: step to where the first
        // is past the TTL and the second is not.
        g.now += Limits::default().session_ttl - Duration::from_secs(4);
        g.r.tick(g.now, &mut ev);
        assert_eq!(
            g.r.counters.sessions_expired, 1,
            "the crashed run's row expired"
        );
        let got = g.list(addr(9), b, DIRECTORY_ROOM, 3);
        assert_eq!(got, vec![(ROOM_B, b"lobby, relaunched".to_vec())]);
    }

    // ---- mp:R3, the candidate exchange ----------------------------------------------------------

    impl Rig {
        /// A leg op that NAMES a destination handle. `op` above hard-codes `dst = 0`, which is what
        /// a client sends; a host addressing one of several clients needs the other form.
        #[allow(clippy::too_many_arguments)]
        fn op_to(
            &mut self,
            from: SocketAddr,
            h: u16,
            dst: u16,
            room: u32,
            seq: u64,
            op: u8,
            p: &[u8],
        ) -> Vec<Out> {
            let s = leg::Secrets::derive(&psk());
            let pkt = leg::encode(op, h, dst, room, seq, p, &s.leg).unwrap();
            self.feed(from, &pkt)
        }
        /// The candidate list as the RECEIVING peer would parse it off the wire, plus who it is for.
        fn cands_out(&self, outs: &[Out]) -> Vec<(SocketAddr, Vec<(SocketAddr, u8)>)> {
            let s = leg::Secrets::derive(&psk());
            outs.iter()
                .map(|o| {
                    let l = leg::decode(&o.bytes, &s.leg).unwrap();
                    assert_eq!(l.op, leg::OP_CAND);
                    (o.to, leg::cands_parse(&l.payload).unwrap())
                })
                .collect()
        }
    }

    #[test]
    fn a_candidate_list_reaches_the_counterpart_with_the_observed_address_appended() {
        let mut g = Rig::new();
        let h = g.join(addr(1), leg::ROLE_HOST, 6501);
        let c = g.join(addr(2), leg::ROLE_CLIENT, 6501);

        // The CLIENT's local candidates. It does not name a destination -- a client has one.
        let local: Vec<(SocketAddr, u8)> = vec![
            ("192.168.0.38:6501".parse().unwrap(), 0),
            ("[fe80::5]:6501".parse().unwrap(), 0),
        ];
        let outs = g.op_to(
            addr(2),
            c,
            0,
            6501,
            2,
            leg::OP_CAND,
            &leg::cands_encode(&local),
        );
        let got = g.cands_out(&outs);
        assert_eq!(got.len(), 1, "one copy, to the room's host and nobody else");
        assert_eq!(got[0].0, addr(1));
        assert_eq!(
            got[0].1,
            vec![
                local[0],
                local[1],
                // THE ONE CANDIDATE THE SENDER COULD NOT KNOW, appended by the relay and flagged.
                (addr(2), leg::CAND_OBSERVED),
            ]
        );
        assert_eq!(g.r.counters.cands_forwarded, 1);
        assert!(g.ev.iter().any(|e| matches!(
            e,
            Event::Candidates { from, to, n, observed, .. }
                if *from == c && *to == h && *n == 3 && *observed == addr(2)
        )));

        // And the HOST's half, which must name the client it is answering.
        let outs = g.op_to(
            addr(1),
            h,
            c,
            6501,
            2,
            leg::OP_CAND,
            &leg::cands_encode(&[("10.0.0.1:7000".parse().unwrap(), 0u8)]),
        );
        let got = g.cands_out(&outs);
        assert_eq!(got[0].0, addr(2));
        assert_eq!(got[0].1.len(), 2);
        assert_eq!(got[0].1[1], (addr(1), leg::CAND_OBSERVED));
    }

    #[test]
    fn a_peer_cannot_claim_the_relay_observed_it_somewhere() {
        // The OBSERVED flag is the receiver's reason to trust one entry over the others, so a peer
        // that could set it could point its counterpart's "trusted" probe anywhere. It is stripped.
        let mut g = Rig::new();
        g.join(addr(1), leg::ROLE_HOST, 6501);
        let c = g.join(addr(2), leg::ROLE_CLIENT, 6501);
        let lie: Vec<(SocketAddr, u8)> =
            vec![("203.0.113.9:53".parse().unwrap(), leg::CAND_OBSERVED)];
        let outs = g.op_to(
            addr(2),
            c,
            0,
            6501,
            2,
            leg::OP_CAND,
            &leg::cands_encode(&lie),
        );
        let got = g.cands_out(&outs);
        assert_eq!(got[0].1[0], ("203.0.113.9:53".parse().unwrap(), 0));
        assert_eq!(got[0].1[1], (addr(2), leg::CAND_OBSERVED));
        assert_eq!(
            got[0]
                .1
                .iter()
                .filter(|(_, f)| *f & leg::CAND_OBSERVED != 0)
                .count(),
            1,
            "exactly one entry is the relay's own observation"
        );
    }

    #[test]
    fn a_full_candidate_list_loses_a_claim_and_never_the_observation() {
        let mut g = Rig::new();
        g.join(addr(1), leg::ROLE_HOST, 6501);
        let c = g.join(addr(2), leg::ROLE_CLIENT, 6501);
        let full: Vec<(SocketAddr, u8)> = (0..leg::MAX_CANDS)
            .map(|i| {
                (
                    SocketAddr::new(IpAddr::V4(Ipv4Addr::new(10, 0, 0, i as u8)), 1),
                    0u8,
                )
            })
            .collect();
        let outs = g.op_to(
            addr(2),
            c,
            0,
            6501,
            2,
            leg::OP_CAND,
            &leg::cands_encode(&full),
        );
        let got = g.cands_out(&outs);
        assert_eq!(got[0].1.len(), leg::MAX_CANDS);
        assert_eq!(*got[0].1.last().unwrap(), (addr(2), leg::CAND_OBSERVED));
        assert_eq!(got[0].1[0], full[0], "the ranked-first claim survives");
    }

    #[test]
    fn a_candidate_list_is_refused_across_a_room_and_when_it_is_malformed() {
        let mut g = Rig::new();
        g.join(addr(1), leg::ROLE_HOST, 6501);
        let c = g.join(addr(2), leg::ROLE_CLIENT, 6501);
        let other = g.join(addr(3), leg::ROLE_CLIENT, 6501);
        let good = leg::cands_encode(&[("192.0.2.4:5".parse().unwrap(), 0u8)]);

        // A CLIENT naming another client is still routed to the host -- a client does not choose.
        let outs = g.op_to(addr(2), c, other, 6501, 2, leg::OP_CAND, &good);
        assert_eq!(g.cands_out(&outs)[0].0, addr(1));

        // A host naming a peer that is not in its room: dropped, counted, and SILENT.
        let h2 = g.join(addr(4), leg::ROLE_HOST, 7001);
        let before = g.r.counters.cands_refused;
        let outs = g.op_to(addr(4), h2, c, 7001, 2, leg::OP_CAND, &good);
        assert!(outs.is_empty(), "no answer at all, not an ERROR");
        assert_eq!(g.r.counters.cands_refused, before + 1);

        // A malformed payload is refused BY NAME, so a version skew is not a silent no-op.
        let outs = g.op_to(addr(2), c, 0, 6501, 3, leg::OP_CAND, &[0xff]);
        assert!(outs.is_empty());
        assert_eq!(g.r.counters.cands_refused, before + 2);
        assert!(g
            .ev
            .iter()
            .any(|e| matches!(e, Event::Refused { why, .. } if *why == "cand_malformed")));
    }

    #[test]
    fn a_probe_that_reaches_the_relay_is_counted_and_never_forwarded() {
        // A peer punches every candidate it is handed; behind the same NAT as the relay, one of
        // them IS the relay. Forwarding it would validate the relayed path and "promote" a pair
        // onto the route it was already on, which is the one failure this op number must not have.
        let mut g = Rig::new();
        let h = g.join(addr(1), leg::ROLE_HOST, 6501);
        let c = g.join(addr(2), leg::ROLE_CLIENT, 6501);
        for (i, op) in [leg::OP_PROBE, leg::OP_PROBE_ACK].iter().enumerate() {
            let outs = g.op_to(addr(2), c, h, 6501, 2 + i as u64, *op, &[0u8; 8]);
            assert!(outs.is_empty(), "the relay answered a peer-to-peer probe");
        }
        assert_eq!(g.r.counters.probes_misdirected, 2);
        assert_eq!(g.r.counters.forwarded, 0);
    }
}
