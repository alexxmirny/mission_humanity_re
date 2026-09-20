#pragma once
//
// udp_punch.h -- UDP HOLE PUNCHING, the decision half (tracker mp:R3, plan decision D4).
//
// WHAT R3 IS. mp:R1 put the relay in the path and mp:R2 put a lobby directory on it. Both leave
// every datagram going the long way round: peer -> relay -> peer. R3 is the short way. Per PEER
// PAIR, the two ends exchange the addresses they might be reachable at (through the relay, which is
// the only thing they can both already talk to), probe them all at once, and -- if a probe and its
// echo both land -- start sending the SAME leg datagrams straight at each other instead. The relay
// leg stays registered and stays warm underneath, so the fall back to it is a change of destination
// and nothing else.
//
// THE PLAYER NEVER WAITS FOR ANY OF THIS. Plan D4 is RELAY-FIRST: the match is already running over
// the relay before the first probe is sent, so punching can take as long as it likes and can fail
// outright. That is the property to keep in mind while reading this file -- nothing here is on the
// path of starting a game, and nothing here may block one.
//
// ---- WHY THE DECISION IS A SEPARATE, SOCKET-FREE FILE --------------------------------------------
//
// Everything that decides WHETHER a pair is on a direct path lives here, in a struct and five
// functions that take the time as an argument and touch no socket, no clock and no global. The
// sending, the address conversion and the candidate gathering are udp_relay.cpp's.
//
// It is split that way because of what the alternative costs to TEST. The interesting states of a
// punch are: a probe that is never answered, an echo that arrives after the pair gave up, a peer
// that answers from an address nobody advertised, a path that works for ten seconds and then stops.
// Every one of those is a TIMING question, and a timing question asked of a file that reads
// GetTickCount() and writes to a socket can only be asked on a rig -- two machines, a firewall rule
// and several minutes per answer. Asked of this file it is a table of numbers in a selftest
// (`net_selftest.exe udppunchtest`), which is why the state machine is here and the sockets are not.
//
// ---- THE PROMOTION RULE, STATED ONCE --------------------------------------------------------------
//
// A path is promoted when BOTH of these are true of the same candidate address:
//
//   * ACKED  -- we sent a probe carrying a nonce and got that same nonce back. This proves the round
//               trip: our datagram reached them AND their answer reached us.
//   * HEARD  -- a probe of THEIR OWN arrived from that address. This proves they, independently,
//               believe the path is worth using, and it is what makes the promotion symmetric: both
//               ends run this same rule, so both switch, and neither is left sending into a path the
//               other has already written off.
//
// ACKED alone would be enough to know the path works. It is not enough to know the PEER knows, and a
// peer that has not promoted is a peer still sending through the relay -- which is harmless, but so
// is waiting for its probe, and requiring both means one rule describes both ends. It also makes the
// pairing backward-compatible for free: a peer from before R3 never sends a probe and never answers
// one, so neither flag ever sets and the pair simply stays relayed.
//
// ---- AND THE DEMOTION RULE, WHICH IS NARROWER THAN IT LOOKS ------------------------------------------
//
// While direct, the pair keeps probing the chosen path at [`KEEP_MS`]. **The echo of our own probe
// is the ONLY thing that keeps the path alive.** [`DEAD_MS`] without one demotes the pair back to
// the relay and re-opens the search. `DEAD_MS` is deliberately a small multiple of `KEEP_MS` and NOT
// a "loss tolerance": losing the direct path is not a fault, it is Tuesday (a NAT rebinding, a
// Wi-Fi roam, a sleeping laptop), and the cost of being wrong is one relayed second.
//
// THE OBVIOUS ALTERNATIVE IS WRONG AND WE SHIPPED IT FOR AN AFTERNOON. Counting *any* authenticated
// datagram from the peer -- its game traffic, or a probe of its own -- looks strictly better: more
// evidence, less keepalive traffic. It is not evidence of the same thing. An inbound datagram proves
// PEER -> US and says nothing whatever about US -> PEER, and a one-way path failure is a completely
// ordinary event (an asymmetric firewall rule, a NAT that expired one direction's mapping). Under
// that rule the peer whose OUTBOUND half died keeps being told it is alive by the inbound half, so
// it never demotes, and it sends the whole match into a hole while the other end -- which correctly
// sees nothing -- has already fallen back to the relay. Measured on the rig 2026-09-18: a mid-match
// cut demoted the client in 3 s and never demoted the host, and the match stalled at step 555. An
// echo of OUR OWN nonce is the only event that proves both halves, so it is the only one counted.
//
#ifndef MH_NET_UDP_PUNCH_H
#define MH_NET_UDP_PUNCH_H

#include <stdint.h>

namespace mh {
namespace udppunch {

// The relay's own ceiling (src/relay/src/leg.rs MAX_CANDS). Eight is a bound on WORK: every
// candidate costs a probe per interval, so the list is what a multi-homed -- or hostile -- peer
// would use to make its counterpart send traffic somewhere.
constexpr int MAX_CANDS = 8;

constexpr uint8_t FAM_V4 = 4;
constexpr uint8_t FAM_V6 = 6;

// `flags` bit 0, mirrored from leg.rs: the RELAY observed this address rather than the peer having
// claimed it. A peer may not set it -- the relay strips it off everything it is told.
constexpr uint8_t FLAG_OBSERVED = 0x01;

// One address a peer might be reachable at. Family-tagged rather than v6-with-a-mapping, for the
// reason leg.rs states: a v4 candidate is probed from the v4 socket and a v6 candidate from the v6
// socket, and those are different sockets with different fates, so the two must stay
// distinguishable end to end.
struct Cand {
    uint8_t  fam;      // FAM_V4 / FAM_V6; 0 in an unused slot
    uint8_t  flags;    // FLAG_OBSERVED
    uint16_t port;     // HOST order (the codec swaps; a sockaddr does not appear in this file)
    uint8_t  addr[16]; // network order; 4 significant bytes for v4, 16 for v6
};

// ---- the wire codec, shared with leg.rs's cands_encode/cands_parse --------------------------------
//
//   count u8 | count x { fam u8 | flags u8 | port u16 LE | addr (4 or 16) }
//
// Returns the bytes written, or 0 if `cap` is too small. `n` over MAX_CANDS is truncated, not an
// error -- the same rule the Rust encoder has, so the two halves cannot disagree about a list that
// is too long.
int cands_encode(const Cand *list, int n, uint8_t *out, int cap);

// Returns the number of candidates decoded, or -1 for a refusal. A list longer than MAX_CANDS, an
// unknown family, or a payload that does not end exactly where the count says it does is a refusal
// and NEVER a shorter list -- a truncation read as a short answer is a peer quietly probing fewer
// paths than it was told about.
int cands_decode(const uint8_t *p, int len, Cand *out, int cap);

bool cand_eq(const Cand &a, const Cand &b);

// ---- the state machine ----------------------------------------------------------------------------

enum State {
    ST_IDLE    = 0, // no candidates yet; the pair is relayed and not trying
    ST_PROBING = 1, // relayed, and probing every candidate it knows
    ST_DIRECT  = 2  // promoted; DATA goes straight to `chosen`
};

// What tick() observed. One value per TRANSITION, so a caller logs exactly the lines mp:R3's
// acceptance clause asks for and nothing in between.
enum Event {
    EV_NONE     = 0,
    EV_PROMOTED = 1,
    EV_DEMOTED  = 2
};

// How often each known candidate is probed while the pair is still relayed. Two rates, because the
// two jobs are different: the acceptance clause wants a pair to go direct inside ten seconds of a
// match starting, and after that the job is only to keep NOTICING that a path became possible (a
// firewall rule lifted, a peer that finished booting). FAST_MS x MAX_CANDS is the worst-case probe
// rate and is why MAX_CANDS is small.
constexpr uint32_t FAST_MS        = 250;
constexpr uint32_t FAST_WINDOW_MS = 10000;
constexpr uint32_t SLOW_MS        = 2000;

// While direct: the keepalive probe interval, and the silence that demotes. DEAD_MS is a small
// multiple of KEEP_MS on purpose -- see the header note.
constexpr uint32_t KEEP_MS = 1000;
constexpr uint32_t DEAD_MS = 3000;

struct Path {
    Cand     cand;
    uint64_t token;   // the nonce of the probe in flight; 0 = none outstanding
    uint32_t sent_ms; // when it went
    bool     acked;   // our nonce came back: the ROUND TRIP works
    bool     heard;   // a probe of their own arrived from here: they think so too
};

struct Punch {
    int      state;
    Path     path[MAX_CANDS];
    int      n;
    int      chosen;     // index into path[] while ST_DIRECT, else -1
    uint32_t started_ms; // when the first candidate arrived -- the clock the 10 s clause is on
    uint32_t last_probe_ms;
    // The last time OUR OWN probe was echoed on `chosen`. NOT "the last datagram from the peer" --
    // see the demotion note at the top of this file; that version of this field was the bug.
    uint32_t last_ack_ms;
    uint32_t promoted_ms; // when the current direct path was promoted (for the log line)
    int      promotions;  // counted, not just flagged: a pair that flaps is a different fault
    int      demotions;
    uint64_t next_token; // the nonce source; never 0 (0 means "no probe outstanding")
    bool     is_forced;  // `[net] force_relay=1`: pinned to the relay, never probes
};

// Zero it and seed the nonce. `seed` may be anything unpredictable-ish (a tick count mixed with a
// handle); the nonce is a REPLAY guard inside an already-MAC'd datagram, not a secret.
void reset(Punch &p, uint64_t seed);

// Merge a candidate list that arrived from the counterpart. Returns how many were NEW.
//
// A candidate already known keeps its probe state: re-learning an address must not un-ack a path
// that is working, which is what a plain overwrite would do every time the peer re-sent its list.
int add_cands(Punch &p, const Cand *list, int n, uint32_t now_ms);

// Which candidates are due a probe right now. Fills `out_idx` with indices and `out_token` with the
// nonce to put in each probe, and returns how many. Call it every pump; it is the ONLY thing that
// decides the probing cadence.
int due_probes(Punch &p, uint32_t now_ms, int *out_idx, uint64_t *out_token, int cap);

// A probe ECHO arrived, carrying `token`. Matched against the outstanding nonce of every path, so a
// stale echo from a path that has since been re-probed is ignored rather than crediting the wrong
// one. Returns true if it matched something.
bool on_ack(Punch &p, uint64_t token, uint32_t now_ms);

// A probe of the PEER'S OWN arrived from `from`. Returns the index of the path it belongs to,
// ADDING it if we have never heard of that address -- which is the case that makes punching work
// against a NAT whose mapping toward us is not the one the relay observed. An inbound probe is
// proof the address exists and can reach us, which is more than any advertised candidate is.
// Returns -1 only if the table is full. It does NOT refresh the liveness timer -- an inbound probe
// is evidence about one direction only; see the demotion note.
int on_peer_probe(Punch &p, const Cand &from, uint32_t now_ms);

// THERE IS NO `on_direct_rx`, and its absence is the design. Inbound game traffic from the chosen
// path was the obvious liveness signal and is the wrong one, for the reason spelled out at the top
// of this file: it proves peer -> us, and a path can fail in one direction. Only [`on_ack`] counts.

// Advance. Returns the transition, if any.
int tick(Punch &p, uint32_t now_ms);

// The address to send to, or null while the pair is relayed. THE ONE QUESTION the rest of the
// module asks this file.
const Cand *direct_target(const Punch &p);

// FORCE THE RELAY (`[net] force_relay=1`). Demotes if promoted and stops all probing -- the pair is
// pinned to the relay for the life of the tunnel. A knob rather than a build flag because the two
// things it is for are both live-system problems: proving a relay deployment carries a real match,
// and giving a player on a network where punching makes things WORSE a way to say so.
void set_forced(Punch &p, bool forced, uint32_t now_ms);
bool forced(const Punch &p);

} // namespace udppunch
} // namespace mh

#endif // MH_NET_UDP_PUNCH_H
