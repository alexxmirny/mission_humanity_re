#pragma once
//
// udp_endpoint.h -- the UDP transport CORE (tracker mp:T1, plan D3), as an instantiable object.
//
// WHAT THIS IS. One process's whole side of the UDP star: the socket, the connect handshake, the
// per-peer reliable ordered segment stream, the inbound ring, and the two background threads. It
// answers the same questions mh_net.dll's TCP transport answers -- send a buffer to a player id,
// dequeue buffers tagged with their sender, six out-of-band control channels -- over T0's packet
// format (mh_net_proto/net_udp.h, docs/mp-wire-udp.md) instead of over TCP.
//
// WHY A CLASS AND NOT A FILE OF GLOBALS, WHICH IS WHAT net_transport.cpp IS. Because the acceptance
// test needs THREE peers in ONE process (a host and two clients on 127.0.0.1, with a synthetic loss
// rate the network cannot be asked for), and a module of globals can only ever be one peer. The TCP
// module's own loopback suites spawn child PROCESSES for exactly this reason, and that is why they
// cannot inject loss: there is nowhere to put the hook. Here the endpoint is an object, the module
// owns exactly one of them (udp_transport.cpp), and `net_selftest.exe udploopbacktest` owns three.
//
//   THE SPLIT IS ALSO WHAT KEEPS THE SELFTEST LINKABLE. net_selftest.exe already compiles
//   mh_net/net_transport.cpp, which DEFINES every MH_Net_* symbol. A second TU defining them would
//   be 23 duplicate symbols. So this file defines none of them: udp_transport.cpp is the only place
//   the exports exist, it is in mh_net_udp.vcxproj alone, and the selftest compiles this core.
//
// ---- THE TRANSPORT CONTRACT, AND THE ONE PLACE UDP MAKES IT HARDER ------------------------------
//
// mh.exe's lobby + lockstep engine carries its own framing, CRC and dispatch, so a transport here is
// a DUMB DATAGRAM PIPE. Over TCP that is free: the stream is reliable and ordered, and the 12-byte
// WireHdr framing rides it. Over UDP neither property exists, and the game needs BOTH -- a lockstep
// order stream with a gap in it is a peer that never executes an order, which is a desync (the same
// defect MP D24 measured from the other end, an inbound queue evicting an order).
//
// So this endpoint puts the SAME 12-byte WireHdr framing (mh_net_proto/net_wire.h -- byte for byte
// what the TCP module sends) on top of a reliable ordered BYTE STREAM of its own, built from T0's
// channel A:
//
//   * the outbound byte stream is chopped into SEGMENTS of at most SEG_PAYLOAD bytes, each with a
//     32-bit sequence number;
//   * every datagram carries the newest segment PLUS the previous K-1 (K_DEFAULT, was `[net] udp_redundancy`,
//     default 3) using T0's channel-A payload, whose step numbering is implied and newest-first.
//     One lost datagram is repaired by the next with no retransmit and no added round trip -- the
//     trade plan D2 chose, and the reason lockstep inputs are the right traffic for it;
//   * the receiver reorders into a window and delivers in order into a WireHdr reassembler;
//   * a segment still missing after the K-window is repaired by a TIMEOUT RETRANSMIT off the peer's
//     channel-C acknowledgement frontier.
//
// THAT LAST BULLET IS A DELIBERATE DEPARTURE FROM THE ITEM'S SCOPE SKETCH ("no retransmit on channel
// A"), and it is arithmetic rather than taste. A pure K-window loses a segment permanently only when
// K CONSECUTIVE datagrams are lost, which at the 5% loss the acceptance test injects is 0.05^3 =
// 1.25e-4 per segment. A determinism run exchanges thousands of segments, so the expected number of
// permanent gaps per run is around one -- and a byte stream cannot resynchronise after a gap, so
// each one is a dead link. "Zero stalls attributed to loss" is not reachable that way. The timeout
// path costs one round trip in the 1-in-8000 case and nothing at all otherwise; the K-window is
// still what repairs essentially every loss, which is what the counters below report.
//
// ---- WHAT THIS FILE DOES NOT DO -----------------------------------------------------------------
//
// Channel C's PIECE half -- the chunked bulk transfer, the resume-by-index state machine and the
// never-evictable chunk lane -- is mp:T2 and lives in udp_channel_c.{h,cpp}, driven from here by
// four additive hooks (route a CH_BULK frame by its kind, tick the transfer, emit a frame, roll the
// counters up into the log). THIS file still owns the channel-C ACK of kind 1, which is the
// channel-A segment stream's frontier and NOT a bulk acknowledgement; T2's is kind 2, and the
// reasoning for that split is in udp_channel_c.h.
//
// Channel B (CH_STATE) carries ping/pong so that silence is unambiguous (the R-live reason), and
// since mp:T3 the round trip it describes is CONSUMED: each conn owns a `mh::netstats::PeerStats`
// (udp_stats.h) fed from three places in the .cpp -- the ping send (remember the high-resolution
// stamp), the pong arrival (fold the round trip into SRTT/RTTVAR/IPDV) and every accepted datagram
// (the T0 header sequence, for the 256-packet loss window). get_stats() publishes the result through
// MH_NetStats::lat[]. Nothing on the wire changed for it: T0's PingRecord is used as-is and its
// fixtures are untouched. The ARRIVAL-LATENESS half of mp:T3 is not here -- it is a property of the
// lockstep horizon, not of the transport, and lives in mh/seams/net_lockstep.cpp.
//
#ifndef MH_NET_UDP_ENDPOINT_H
#define MH_NET_UDP_ENDPOINT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>

#include "mh_net_export.h" // MH_NetConfig / MH_NetStats / MH_NET_MAX_PEERS / MH_NET_MAX_PAYLOAD
#include "mh_net_proto/net_crypto.h"
#include "mh_net_proto/net_udp.h"
#include "mh_net_proto/net_wire.h" // the 12-byte framing the stream carries -- see the note above
#include "mh_net_queue_policy.h"   // mp:U41e -- the SAME lane pair mh_net.dll's TCP transport uses
#include "mesh.h"                 // mp:U61 -- elect(), the RTT matrix, the FLAG_MESH codecs (socket-free)
#include "origin_seq.h"           // mp:U59 -- the end-to-end exactly-once layer for broadcast game frames
#include "udp_channel_c.h"         // mp:T2 -- channel C's bulk transfer and its chunk lane
#include "udp_ping_cadence.h"      // mp:P15 -- the warm-up ping cadence's decision function
#include "udp_stats.h"             // mp:T3 -- the RFC 6298 / 3393 / 7680 arithmetic, I/O-free

namespace mh {
namespace netudp {

// ---- sizes --------------------------------------------------------------------------------------
//
// SEG_PAYLOAD is 254 and not 255 because a channel-A entry's length is ONE BYTE (T0
// INPUT_ENTRY_MAX), and one byte of every entry is spent on this endpoint's own segment header
// (see seg_hdr below). 254 * K + 5 + the 18-byte packet header + the 16-byte tag stays inside T0's
// 1200-byte ceiling for every K up to 4; K beyond that is refused at config time rather than
// producing a datagram encode() would reject at run time.
constexpr int SEG_PAYLOAD = 254;
// The reorder / retransmit windows. Both are powers of two so the modulo is a mask, and both are
// far larger than any burst the game produces: a lockstep frame is a segment or two, so 1024 is
// about sixteen seconds of traffic at frame rate. On RECEIVE a sequence outside the window is not a
// reorder, it is a broken stream, and it drops the connection rather than corrupting it. On SEND a
// full window is back-pressure since mp:T4b (see TX_BACKLOG_BYTES below), not a drop.
constexpr int SEG_WINDOW = 1024;
constexpr int SEG_MASK   = SEG_WINDOW - 1;

// mp:U41e -- THE SAME SEQUENCE-MERGED LANE PAIR mh_net.dll's TCP transport uses (mp:U41,
// mh_net_queue_policy.h), sized identically for the identical reason: lane H (bare horizon
// adverts, evictable by construction) never blocks and covers a multi-second flood in a few tens
// of KB; lane M (orders, control frames, anything else) is the ONLY lane that may REFUSE an
// arrival, and it keeps the ring's original 256-slot capacity so every existing capacity-sensitive
// arm (udploopbacktest's burst/stall/ring-full arms, all of which send non-horizon-tagged frames)
// is unaffected byte for byte. Before this item the ring was a single 256-slot FIFO with D24's
// victim-scan eviction (still compiled below as `choose_victim`, used nowhere on this path any
// more) -- which, unlike the TCP module since mp:U41, could force-evict a REAL, non-supersedable
// frame when nothing evictable was left (the `ev_unsafe` case udploopbacktest's `g_evict_unsafe`
// witness existed to catch). The lane pair removes that case by construction: a full lane M
// REFUSES the new arrival instead, exactly as mh_net.dll's has since U41.
constexpr int QUEUE_CAP_H = 4096;                      // bare horizons -- the lane that floods
constexpr int QUEUE_CAP_M = 256;                       // everything else -- the lane that must not lose
constexpr int QUEUE_CAP   = QUEUE_CAP_H + QUEUE_CAP_M; // reported depth denominator

constexpr int K_MIN     = (int)mh_net_proto::udp::INPUT_K_MIN;
constexpr int K_MAX     = 4; // see SEG_PAYLOAD above -- the MTU, not T0's INPUT_K_MAX of 8
constexpr int K_DEFAULT = (int)mh_net_proto::udp::INPUT_K_DEFAULT;

// ---- timing -------------------------------------------------------------------------------------
constexpr DWORD HS_RETRY_MS  = 300;   // handshake datagram retransmit interval
constexpr DWORD HS_BUDGET_MS = 4000;  // total handshake budget, matching the TCP connect budget
constexpr DWORD HS_PEND_MS   = 8000;  // how long a host keeps an unfinished handshake
constexpr DWORD ACK_MS       = 40;    // how often a receiver publishes its stream frontier
constexpr DWORD RTO_MS       = 200;   // the retransmit timeout's FLOOR (and its value before any RTT)
constexpr int   RTO_BURST    = 16;    // ... at most this many per pass, so a stall cannot flood
constexpr DWORD NAT_KEEP_MS  = 20000; // PKT_KEEPALIVE when pings are off (plan D4's NAT floor)
constexpr DWORD TOKEN_TTL_MS = 60000; // a minted connect token is presentable for this long
constexpr DWORD TICK_MS      = 20;    // the timer thread's period
// mp:T1b -- how long stop() waits for each of the two threads. The same budget mh_net.dll's
// net_reset() uses, and generous by two orders of magnitude: both loops test `m_running` at most
// one TICK_MS apart and the recv loop is woken by the socket closing under it. A thread still
// running after this is not slow, it is stuck, and the reset refuses rather than freeing memory it
// may still be reading.
constexpr DWORD RESET_JOIN_MS = 3000;
// The R-live liveness defaults, deliberately lopsided and deliberately IDENTICAL to mh_net.dll's:
// ping often enough that silence is unambiguous, and give up only after many missed pings, because
// a false drop is far worse than a slow one. `[net] ping_ms` / `rx_timeout_ms` of 0 mean "unset ->
// these"; a negative value is the explicit off.
constexpr int PING_MS_DEFAULT       = 1000;
constexpr int RX_TIMEOUT_MS_DEFAULT = 10000;

// ---- mp:P15 -- a WARM-UP ping burst for the first seconds after a peer is admitted ----------------
//
// P14 seeds the adaptive lookahead's start value from the SRTT already measured in the lobby, but the
// seed needs AD_START_MIN_RTT_SAMPLES (3, lookahead_start.h) pings, and at the shipped 1 Hz cadence
// that is a 3 s FLOOR on top of however long the lobby actually took: the o4 rig lanes (a hand-clicked
// lobby, match starting ~1.4 s after connect) only ever accrued 2 samples, so every run fell back to
// the 100 ms guess P14 exists to replace. A real lobby usually sits open far longer than 3 s, but a
// quick rematch or an auto-start flow does not, and there is no reason a seed that is cheap (three
// ~200-byte round trips) should be gated on how long a human happens to leave a menu open.
//
// So each peer is pinged at FAST_PING_MS for FAST_PING_WINDOW_MS after admission -- 3 samples inside
// 750 ms even at a LAN's ~1 ms RTT, and inside the window at any RTT this transport's own ceiling
// (RTO_MAX_MS, 2 s) allows -- then drops back to the steady m_ping_ms (1000 ms) cadence for the rest
// of the match. TRAFFIC COST: at most 12 extra ~40-byte sealed pings per peer per side (4 Hz - 1 Hz
// for 3 s = 9 extra ticks, rounded up for the boundary tick) -- ~480 B one-shot, once per connect, on
// a link whose steady state already spends far more than that on lockstep orders. Never faster than
// the CONFIGURED cadence: a `[net] ping_ms` set below FAST_PING_MS is left alone (see timer_loop's
// `warm_ms` clamp), so this can only ADD samples early, never replace a deliberately fast steady rate.
constexpr DWORD FAST_PING_MS        = 250;  // 4 Hz
constexpr DWORD FAST_PING_WINDOW_MS = 3000; // ...for this long after admission

// ---- mp:T4b / mp:T5 -- the send window is BACK-PRESSURE, not a tripwire ---------------------------
//
// Until T4b a sender that ran SEG_WINDOW segments past the peer's acknowledgement frontier DROPPED
// THE LINK. That rule assumed a full window meant "the peer stopped answering", and two rig runs
// showed it does not: mp:T4 (a per-frame advert at thousands of fps filling 1024 segments inside a
// 360 ms round trip) and mp:T5 (a lobby at a 500 ms round trip, after a 3.4 s machine-wide freeze,
// dropped while the peer was STILL acknowledging -- 451 segments delivered, 12 keepalives answered).
// A full window is a peer that is BEHIND, and a peer that is behind is not a dead one.
//
// So bytes that do not fit the window wait in a per-peer BACKLOG and go out as the frontier moves.
// They go out BUNDLED: the backlog is a byte stream like the one it feeds, so it is cut into full
// SEG_PAYLOAD segments however many frames that spans -- a burst of 21-byte adverts that took one
// segment each while the window was open takes 1/12 of a slot each once it is not. The OPEN-window
// path is untouched: a frame written while the window has room leaves in that same call, in its own
// segment, exactly as before, so back-pressure costs nothing until the window is actually full.
//
// WIRE: unchanged. A segment carrying the tail of one frame and the head of the next is something
// the receiver's reassembler has always accepted (stream_drain walks a segment frame by frame and
// carries a partial header across a boundary); v0.2.0-rc2's on_input_frame/stream_drain are
// byte-identical to this build's, so an rc2 peer reads a bundled segment exactly as this one does.
// Nothing is negotiated because nothing new is on the wire.
//
// THE DROP IS KEPT for what it was always meant to catch, stated as TIME rather than as a byte
// count: a peer whose acknowledgement frontier has not moved for the link timeout (rx_timeout_ms,
// 10 s by default -- the bound the silence watchdog has always promised for a dead peer) while this
// side has data outstanding is dropped, even if its pings still arrive. The backlog is also bounded:
// TX_BACKLOG_BYTES waiting behind a closed window is a sustained send rate the path cannot carry,
// and that drops the link by name rather than growing without limit.
constexpr uint32_t TX_BACKLOG_BYTES = 256u * 1024u;
constexpr int      PUMP_BURST       = 64; // backlog segments sent per pump call (see stream_pump)

// ---- mp:T5 -- the retransmit timeout follows the measured round trip -----------------------------
//
// RTO_MS was a CONSTANT 200 ms, which is below every round trip the field and the rig run at (field
// SRTT 205-230 ms; rig shims 360 and 500 ms). Every segment was therefore re-sent before its
// acknowledgement could possibly have arrived: on every clean 250 ms-one-way rig run the host logged
// `rto sent` at ~2x its new segments (e.g. 1862 re-sends for 896 new ones in 10 s), each re-send a
// K-redundant datagram. That is a retransmit STORM on a loss-free path, and after T5's 3.4 s freeze
// it was what turned a backlog into ~1000 datagrams/s through the shim.
//
// RFC 6298's shape, with the acknowledgement cadence as the variance floor: an ack is published at
// most every ACK_MS by the peer's 20 ms timer, so a segment's ack can lag its round trip by
// ACK_MS + 2 * TICK_MS on a path with no variance at all. RTO_MS stays the floor, so a LAN or any
// path under ~120 ms round trip retransmits exactly as before; RTO_MAX_MS caps a wild estimate.
constexpr DWORD RTO_MAX_MS       = 2000;
constexpr DWORD RTO_ACK_SLACK_MS = ACK_MS + 2 * TICK_MS;
// ...and it BACKS OFF (RFC 6298 5.5): each time the OLDEST outstanding segment has to be re-sent the
// timeout doubles, up to 2^RTO_BACKOFF_MAX, and the next frontier advance resets it. A round-trip
// estimate taken from pings on an idle path is short of the queueing a burst adds, and without the
// backoff every re-send joins that queue and lengthens it -- the congestion-collapse shape (seen
// offline when a starved relay fell behind: 7616 re-sends for 1150 new segments).
constexpr int RTO_BACKOFF_MAX = 3;

// `samples == 0` (no pong yet) keeps the old constant: there is nothing to follow.
inline DWORD rto_for(double srtt_ms, double rttvar_ms, long samples) {
    if (samples <= 0) return RTO_MS;
    double var = 4.0 * rttvar_ms;
    if (var < (double)RTO_ACK_SLACK_MS) var = (double)RTO_ACK_SLACK_MS;
    double r = srtt_ms + var;
    if (r < (double)RTO_MS) r = (double)RTO_MS;
    if (r > (double)RTO_MAX_MS) r = (double)RTO_MAX_MS;
    return (DWORD)r;
}

// ---- the channel-A segment header ---------------------------------------------------------------
// One byte in front of every segment's bytes. It exists for exactly one reason: a byte stream that
// is only ever APPENDED to has no way to say "this segment is padding", and the K-window needs to
// re-send segments that may be shorter than SEG_PAYLOAD. The length is carried by channel A itself
// (the entry's one-byte len), so all this byte carries is a version/typing nibble -- which makes a
// stream from a future build identifiable rather than silently mis-parsed.
constexpr uint8_t SEG_KIND_STREAM = 0x01;

// ---- counters -----------------------------------------------------------------------------------
// The half of the instrument that can REFUTE. `repaired_by_k` vs `repaired_by_rto` is the whole
// claim of the acceptance test: if the first is large and the second is zero, redundancy covered the
// injected loss; if `gap_stalls` is non-zero, it did not, and no run-level green may say otherwise.
struct Counters {
    long dgram_tx, dgram_rx;
    long dgram_dropped_sim; // the selftest's synthetic loss, so a run can prove loss really happened
    long seg_tx, seg_rx_new, seg_rx_dup;
    long repaired_by_k;   // a segment first seen in a REDUNDANT copy (its own datagram was lost)
    long repaired_by_rto; // a segment that needed the timeout retransmit
    long rto_sent;
    long gap_stalls;   // times the reassembler was blocked on a missing segment past RTO
    long gap_ms_worst; // the longest such block
    long mac_fail, replay_drop, malformed, wrong_conn;
    long hs_started, hs_done, hs_retries;
    // mp:T4b -- the back-pressure half. `bp_episodes` counts times a write found the window full and
    // had to queue; `bp_bundled_segs` the segments cut from the backlog (each may carry many frames);
    // `bp_peak_bytes` the most ever queued on one peer. A burst arm that never touched the backlog
    // proved nothing about it, so the suite asserts these are non-zero before believing a survival.
    long bp_episodes, bp_bundled_segs, bp_peak_bytes;
    long ack_stall_drops; // links dropped because the peer's frontier stopped moving (the kept drop)
    long rx_pauses;       // times delivery paused on a full inbound ring instead of destroying a frame
};

// ---- the control-frame callback -----------------------------------------------------------------
// ONE callback for all six of the module's out-of-band channels, dispatched by the WireHdr flag.
// The module's exported surface has six differently-typed setters (MH_Net_SetJoinHandler and
// friends); collapsing them here keeps the endpoint ignorant of that surface, and udp_transport.cpp
// -- which is the only file that knows the surface exists -- fans this one edge back out into six.
typedef void (*ctrl_fn)(void *ctx, uint16_t flags, int sender, const unsigned char *buf, int len);
typedef void (*log_fn)(void *ctx, const char *line);

// mp:L1f -- "which PATH did this peer's traffic last arrive on?", asked by ADDRESS. 1 = the relay
// leg, 0 = direct, -1 = nobody can say.
//
// THE ENDPOINT DOES NOT LEARN ABOUT RELAYS BY HAVING THIS (udp_relay.h reason 1 stands). It is the
// exact mirror of `knows_addr`, which the relay already asks the endpoint: there, the endpoint
// answers a question about its OWN peer table and learns nothing about tunnels; here it asks a
// question about an address it already holds and learns nothing about legs, handles or rooms. The
// answer is an opaque 1/0/-1 it only ever copies into MH_NetPeerLatency.relayed -- it never routes,
// paces, retransmits or drops on it, and nothing in the endpoint reads the field back. Wired by
// udp_transport.cpp (the one file that knows both layers exist), exactly as relay_peer_known is.
typedef int (*path_class_fn)(void *ctx, const sockaddr_in &a);

// ---- configuration ------------------------------------------------------------------------------
// MH_NetConfig plus the two things it cannot carry. MH_NetConfig is the mh.dll <-> module ABI
// (mh_net_export.h) and this item does not change it: the TCP module must stay byte-for-byte the
// build it was, and an ABI field added for one transport would have to be understood by both.
struct Config {
    MH_NetConfig   net;        // role / host / port / player_id / log / host_assign / ping / timeout
    int            redundancy; // K -- K_DEFAULT, clamped into [K_MIN, K_MAX]
    unsigned short bind_port;  // 0 = ephemeral. The host binds net.port; a client takes whatever the
                               // OS gives it, except in the selftest, which pins all three.
    // mp:T2 -- the channel-C measurement arm (selftest-set only; the `[net] bulk_selftest_mb` / `bulk_selftest_step` knobs are retired). 0 MB is off. Non-zero arms a
    // SYNTHETIC channel-C transfer of that many mebibytes, started once a peer's desync sample
    // reports a sim step at or past the second value, which is how the rig arm measures "a bulk
    // transfer does not delay channel A" without a game that knows how to ask for one.
    int bulk_selftest_mb;
    int bulk_selftest_step;
    // mp:R1d -- HOW MANY BYTES SOMETHING WRAPS AROUND EVERY DATAGRAM THIS ENDPOINT SENDS. 0 for a
    // direct link; 34 (the relay leg's header + tag) when `[net] relay` is set, which udp_transport
    // fills in from mh::udprelay::LEG_OVERHEAD.
    //
    // WHY THE ENDPOINT IS TOLD AT ALL, when mp:R1's whole design is that it does not know the relay
    // exists. Because the 1200-byte figure is not the endpoint's preference, it is a property of
    // the PATH: 1200 is the size RFC 9000 picked as crossing the internet without PMTU discovery,
    // and a relayed datagram of 1200 is 1234 on the wire -- outside the guarantee the number is
    // for. The endpoint is the only thing that can honour it, because it is the only thing that
    // decides how much it puts in a datagram. So it is told the OVERHEAD, not the relay: "something
    // adds 34 bytes downstream of you" is a fact about the link, and the endpoint still has no idea
    // what that something is. A promoted (mp:R3) pair keeps the envelope precisely so this number
    // never changes mid-session.
    int leg_overhead;
    // mp:R2c -- CLIENT ONLY: this dial has no target yet (the relay leg is registering under
    // DIRECTORY_ROOM, not a real room -- udp_transport.cpp sets this from `rc.room ==
    // mh::udprelay::DIRECTORY_ROOM`). A client that dials with no typed address and no directory
    // pick used to arm the ordinary peer handshake anyway, against a "room" that was never more
    // than `[net] port` -- a number nothing hosts, so client_handshake_tick's HS_BUDGET_MS wait
    // was a guaranteed, silent 4-second "handshake FAILED" on every such dial (mp:R2c). With this
    // set, start() does not arm the client's Pending handshake at all: the leg still comes up (so
    // the relay's directory LIST still arrives), but nothing tries to reach a peer that was never
    // named. The moment a real room IS known (a typed IP, or a picked directory row), the caller
    // does a full relink -- a fresh start() with this false -- so the handshake still runs, just
    // not against a guess. False for a direct (non-relayed) dial and for the host role, where it
    // is never read.
    bool browse_only;
    // mp:U62 (HM-M4) -- `[net] hub_migration=0` (read by udp_transport.cpp; the shipped default is ON, so a
    // zero-initialised Config is ON): the NEGATIVE arm. This endpoint neither hands the hub over when its
    // player leaves (hub_leave() answers HL_NOTHING) nor acts on a HUB_LEAVING it receives, which reproduces
    // U55's measured 58 s outcome-8 timeline.
    bool no_hub_migration;
    // mp:U63 (HM-M5) -- the failover budget (0 = mesh::FO_BUDGET_MS; selftest-set only, the `[net] failover_budget_ms` knob is retired): the bound on a whole crash failover. A
    // zero-initialised Config gets the default. `[net] hub_migration=0` (no_hub_migration) switches the failover off
    // with the handover: the negative arm is U55's measured 58 s outcome-8 timeline.
    int failover_budget_ms;
    // mp:U64 (HM-M6) -- `[net] mesh_test_delay_ms` (0 = off, TEST ONLY, like bulk_selftest_mb): this peer holds every
    // mesh PROBE and ECHO datagram it sends for this many ms, so a client<->client path reads slow exactly as a
    // slow link would. net_shim sits only on the client<->HUB link, and the hub is the peer that dies in a
    // failover test, so this is the only way a rig run can make one survivor the cheapest hub. The measured
    // round trip of a pair i<->j is then base + D_i + D_j.
    int mesh_test_delay_ms;
    // TEST ONLY (never read from the ini; 0/1 = shipped timing): divides the crash-failover clocks (mesh::FO_*_MS, the T_suspect
    // floor, the reconcile wait) so the loopback selftests do not wait out real-time budgets. The DLL never sets it.
    int mesh_time_div;
};

// mp:U69 -- the gate on `[net] mesh_test_delay_ms`. The key skews this peer's mesh RTT probes and so the host election
// (elect()), which makes it a way for a player to rig who becomes the hub. It is honoured ONLY when the test harness is
// armed (`[harness] enable=1` in the same mh_net.ini -- the rig always runs with it); otherwise it reads 0. Negative
// values are never honoured. `*ignored` is set when a non-zero request was dropped, so the caller logs it once.
inline int mesh_test_delay_gate(int requested, bool harness_armed, bool *ignored) {
    if (ignored) *ignored = (requested != 0 && !harness_armed);
    return (harness_armed && requested > 0) ? requested : 0;
}

// =================================================================================================
class Endpoint {
public:
    Endpoint();

    // Bring the endpoint up. `psk` is the 32-byte pre-shared key from mh_key.txt; `secure` false is
    // the file's literal "open" word, which in THIS transport still runs the handshake (UDP has no
    // connection, so the host needs something to bind a source address to a peer slot) but does it
    // under a published all-zero key -- so "open" means unauthenticated, exactly as it does for TCP,
    // rather than meaning unframed. Returns true on success.
    //
    // mp:T1b -- START ON AN ALREADY-STARTED ENDPOINT RESTARTS IT. This is the UDP half of U40's
    // rule, and it lives HERE rather than in udp_transport.cpp so that the module's exported
    // MH_Net_InitEx and `net_selftest.exe udprelinktest` drive the SAME code: the selftest links
    // this core and not the exported surface (see the header note above), so a restart implemented
    // one layer up would be a restart no oracle can reach. A restart is stop() followed by an
    // ordinary start; if the stop REFUSES (a thread that will not come back inside its budget,
    // below) this returns false with the OLD endpoint left running and `started()` still true --
    // the caller's cue that nothing was torn down and it may try again later.
    bool start(const Config &cfg, const uint8_t psk[mh_net_proto::KEY_LEN], bool secure);

    // Return the endpoint to the pre-start state: stop the threads, close the socket, and drop the
    // connection table, the pending handshakes, the session keys and the PSK. Returns FALSE if a
    // thread did not stop inside RESET_JOIN_MS, in which case NOTHING is zeroed and `started()`
    // stays true -- a refusal is the pre-T1b behaviour, loudly, not a transport whose tables are
    // cleared under a thread still reading them. The two critical sections are NOT deleted: they
    // are per-object process-lifetime (see the m_cs_ready note in the .cpp), because the exported
    // MH_Net_Send/Recv test the module's `g_started` without a lock, so a CS deleted under a
    // concurrent caller is a crash where a stale-but-valid one is an empty queue.
    bool stop();
    bool started() const { return m_started; }

    // Diagnostics sinks, set before start().
    void set_log(log_fn fn, void *ctx) {
        m_log     = fn;
        m_log_ctx = ctx;
    }
    void set_ctrl(ctrl_fn fn, void *ctx) {
        m_ctrl     = fn;
        m_ctrl_ctx = ctx;
    }
    // mp:L1f -- the path classifier (see path_class_fn above). Set once by udp_transport.cpp,
    // before start(), for BOTH roles and whether or not a relay is configured: a build with no
    // classifier installed answers -1 for every peer, which is what every pre-L1f reader saw.
    void set_path_class(path_class_fn fn, void *ctx) {
        m_path_class     = fn;
        m_path_class_ctx = ctx;
    }

    // ---- the transport surface (one method per MH_Net_* row that has a body) ---------------------
    int  send(int dst_player, const void *buf, int len);
    int  recv(int *out_sender, void *buf, int *inout_len);
    void send_ctrl(uint16_t flags, const unsigned char *buf, int len);
    int  peer_count();
    int  local_player_id() const { return m_started ? m_my_id : -1; }
    int  id_assigned() const;
    int  active_peer_ids(int *out, int cap);
    int  take_dead_peer();
    void get_stats(MH_NetStats *out);
    // mp:SES6 -- mh.dll pushes the peer's ADVERTISED LOCKSTEP HORIZON here (a game-level quantity the
    // transport cannot measure itself; see mh_net_export.h's note on MH_Net_SetPeerHorizon), so the
    // "net: udp counters" line can print it beside the delivery counters. `player_id` is the caller's
    // strategic-player-slot space; see the .cpp for why a client applies it to its one conn regardless.
    void set_peer_horizon(int player_id, int horizon_ms);
    // mp:U41e -- THE MATCH BOUNDARY, mirroring mh_net.dll's MH_Net_QueueMatchBoundary exactly (see
    // net_transport.cpp's note on the row): rolls the finished match's inbound-queue rollup into the
    // log, then restarts ONLY the per-match counters (mh::net::queue_policy::lane_queue::
    // reset_counters(), which bumps `epoch_` and zeroes evicted/refused) -- NOT the lanes themselves,
    // which is `stop()`'s transport-boundary job. Called by udp_transport.cpp's
    // MH_Net_QueueMatchBoundary export, which used to be a no-op (this module carried no per-match
    // rollup at all -- mp:U41c/G305).
    void queue_match_boundary();
    // mp:U41e -- qmatchtest's read of the lane counters, the udp twin of net_transport.cpp's
    // free-function `mh_net_queue_counters_for_test`. Under the same lock the writers take.
    // `epoch_out` is the reset marker (mh_net_queue_policy.h); pass nullptr where it is not needed.
    void queue_counters_for_test(int *depth, int *high, long *evicted, long *refused,
                                 unsigned *epoch_out);
    // mp:X2h -- a bare, lock-protected read of lane M's CURRENT depth (no rollup, no log line, unlike
    // queue_match_boundary above). The udp twin of net_transport.cpp's MH_Net_QueueDepthM.
    int queue_depth_m();
    // ---- mp:U59 (HM-M1) -- the exactly-once layer's reconcile surface -----------------------------
    // Every broadcast game frame this endpoint sends carries (origin, per-origin seq) and every
    // receiver dedupes + delivers per origin in order (origin_seq.h). M1 ships the PRIMITIVE; nothing
    // in normal play calls these. The loopback arms drive them directly as the test-only "reconcile
    // among survivors" path; M2/M3 will route the same calls over a hub-change handshake.
    //   osq_report   -- this peer's per-origin frontier / retention (one Report per survivor feeds
    //                   osq::plan()).
    //   osq_serve    -- hand `fn(inc, lo, hi, data, len, sup)` every retained entry overlapping
    //                   [from, to] of `origin`; -1 if `from` is below the retention.
    //   osq_ingest   -- feed one served range back in. Deduped, ordered, and delivered to the game
    //                   queue exactly like a live frame. Returns osq::Layer::Rx as an int, or -1 (and
    //                   changes nothing) while the game queue has no room: drain it and call again.
    //   osq_counters / osq_gap_force -- instrumentation and the M2 switch (0 = never skip a hole).
    void           osq_report(osq::Report &out);
    typedef void (*osq_serve_fn)(void *ctx, uint16_t inc, uint32_t lo, uint32_t hi, const uint8_t *data,
                                 int len, bool sup);
    int            osq_serve(int origin, uint32_t from, uint32_t to, osq_serve_fn fn, void *ctx);
    int            osq_ingest(int origin, uint16_t inc, uint32_t lo, uint32_t hi, const uint8_t *data, int len);
    osq::Counters  osq_counters();
    void           osq_gap_force(uint32_t ms);
    // Test-only: make this endpoint behave as a PRE-U59 build (empty HELLO/WELCOME, unprefixed DATA) so
    // the version gate (user decision Q5) can be proved in both directions without an old binary.
    void           osq_test_legacy(bool on);

    // ---- mp:U60 (HM-M2) -- REHOME: a CLIENT endpoint changes hub, IN PLACE -------------------------
    // The mechanism a later step (M3 election, M4/M5 handover) drives; nothing in normal play calls it.
    // stop() + start() is NOT this: it wipes the connection table, the inbound lanes and the player id.
    // rehome() keeps the SOCKET (its NAT mapping and port), the inbound lanes (frames already queued for
    // the game stay queued), the local player id, the exactly-once layer (incarnation, per-origin
    // sequence, retention rings), channel C's frontier, the counters and the PSK -- and replaces only the
    // hub connection. The two directions:
    //   as_hub = true   this endpoint BECOMES the hub (m_role 1 -> 0). It accepts re-dials from the
    //                   listed `roster` ids and refuses any other id (a foreign HELLO is told it is
    //                   refused and dropped). It sends NO WELCOME: ids are kept, never assigned.
    //   as_hub = false  this endpoint re-dials `host:port` (the new hub) as a client and re-binds its
    //                   OLD player id through FLAG_HELLO. Lanes are not cleared, no WELCOME is expected.
    // Either way it then runs M1's reconcile over the new topology on FLAG_RECONCILE (see
    // udp_endpoint.cpp): each survivor REPORTs its per-origin frontier to the hub, the hub plans
    // (osq::plan), the max-holders SERVE ranges through the hub, the laggards INGEST them, and the hub
    // sends each survivor a TARGET frontier it is `done` at. osq_gap_force(0) is held for the duration.
    // Returns false (changing nothing) when the endpoint is not a started client with an assigned id.
    struct RehomeSpec {
        bool as_hub;
        int  roster[MH_NET_MAX_PEERS]; // as_hub: the survivor ids expected to re-dial (not this endpoint's own)
        int  roster_n;
        char host[64];            // !as_hub: the new hub's address ("127.0.0.1" through a relay tunnel)
        int  port;                // ...and its port (the tunnel's client_dial_port() through a relay)
        int  dial_budget_ms;      // !as_hub: how long the re-dial keeps trying (0 = HS_BUDGET_MS)
        int  reconcile_wait_ms;   // as_hub: wait this long for the roster's reports before planning (0 = 5000)
        bool test_skip_reconcile; // TEST ONLY: switch the role and re-dial but never reconcile (the mutation probe)
    };
    struct RehomeStatus {
        bool rehomed;          // rehome() has run since start()
        int  role;             // 0 = hub, 1 = client
        bool reconciling;      // a reconcile is still in flight here
        bool done;             // this endpoint reached the hub's TARGET frontier (no origin short, none waiting)
        bool unrecoverable;    // the plan found a needed seq below every holder's retention: end the match
        bool aborted;          // gave up after RC_ABORT_MS
        int  reports_in;       // hub: RC_REPORTs received
        int  plans;            // hub: plans issued
        int  fetches_issued;   // hub: fetches in the plans
        long ranges_served;    // ranges this endpoint put on the wire for someone else
        long ranges_ingested;  // ranges this endpoint fed to its own exactly-once layer
        long ranges_noroom;    // ranges that arrived with the game queue short of headroom (must stay 0)
        long foreign_refused;  // hub: HELLOs refused for an id outside the roster
        long welcome_tx;       // WELCOMEs this endpoint SENT, lifetime (rehome must not move it)
        long welcome_rx;       // WELCOMEs this endpoint RECEIVED, lifetime
        uint32_t target[8];    // the TARGET frontier it is converging on (0 = none yet)
        bool     target_valid;
    };
    bool           rehome(const RehomeSpec &sp);
    void           rehome_status(RehomeStatus &out);
    // The UDP port the socket is bound to (host byte order), 0 when not started. A relay tunnel that is
    // switching to the host shape needs it as its `game_port`.
    unsigned short bound_port();


    // ---- mp:U61 (HM-M3) -- MESH PROBES + ADDRESS BROKERING + RTT MATRIX + SUCCESSION EPOCHS ---------
    // While the host lives (the host-migration plan, mp:U57, sections 3-5): the host brokers every client the
    // OTHER clients' reachable candidates over its encrypted links, each client probes the others directly
    // on THIS endpoint's socket (the mapping the host observed), reports its row of the RTT matrix to the
    // host, and the host merges the rows and publishes SUCCESSION EPOCHS {epoch, matrix digest, ranked
    // successor list, dial info, a pre-minted relay room per candidate}; every client acks, and e+1 goes
    // out only after every ack of e. Nothing here touches the game queue: the frames are FLAG_MESH
    // control frames consumed inside the endpoint, the probes are separate datagrams. Hub-loss detection,
    // failover and handover are M4/M5; this item only measures, elects and agrees. See udp_mesh.cpp.
    //
    // The two things this endpoint cannot know come in as hooks (udp_transport.cpp wires the real ones;
    // the selftest wires fakes): this peer's own round trip to the relay, and a fresh relay room code.
    struct MeshHooks {
        int      (*leg_rtt_dms)(void *ctx) = nullptr; // own relay-leg round trip in 0.1 ms, or -1 (not relayed / not measured yet)
        uint32_t (*mint_room)(void *ctx)   = nullptr; // a fresh non-zero relay room code, 0 = the RNG failed
        void *ctx                          = nullptr;
        // mp:U62 -- the relay tunnel's role switch (udprelay::rehome), for a relayed match's handover. role 0 =
        // become `room`'s host (the tunnel then delivers to 127.0.0.1:game_port), role 1 = move to `room` as a
        // client; `*dial_port` receives the loopback port a client endpoint must re-dial. MUST be called
        // WITHOUT m_conn_cs held (the tunnel's pump thread asks the endpoint who it knows, under that lock).
        bool (*tunnel_rehome)(void *ctx, int role, uint32_t room, unsigned short game_port,
                              unsigned short *dial_port) = nullptr;
        // mp:U63 -- milliseconds since this peer last heard its relay (a leg round trip), or -1 when it never did /
        // is not relayed. A relayed survivor corroborates "the hub is lost" with its OWN link being alive: every
        // route runs through the relay, so a silent hub behind a live leg is the hub (plan section 2).
        int (*leg_age_ms)(void *ctx) = nullptr;
    };
    void set_mesh_hooks(const MeshHooks &h) { m_mesh_hooks = h; }
    // Test-only. -1 = the real capability byte. A pre-M3 build (U59/U60) sends CAP_VERSION 1.
    void mesh_test_cap(int cap) { InterlockedExchange(&m_test_cap, (LONG)(cap + 1)); }
    // Test-only: send no probe datagram, so a "this pair stays unmeasured" case is real.
    void mesh_test_no_probe(bool on) { InterlockedExchange(&m_test_noprobe, on ? 1 : 0); }
    // Test-only: adopt epochs but never ack them (a client whose acks are lost), so "e+1 only after every ack of e" is real.
    void mesh_test_no_ack(bool on) { InterlockedExchange(&m_test_noack, on ? 1 : 0); }
    struct MeshStatus {
        int      role;          // 0 hub, 1 client
        bool     key_valid;
        // the epoch this endpoint is working on: the host's published one, a client's held one
        uint32_t epoch;
        uint32_t digest;
        uint8_t  hub;
        uint8_t  S;             // survivor set (bit per player id)
        uint8_t  tier;
        int      rank_n;
        uint8_t  rank[8];
        mesh::Matrix   M;       // the matrix of that epoch (host before the first publish: the live merged one)
        mesh::CandList dial[8];
        uint32_t room[8];
        // host: who acked what
        uint32_t acked_mask;          // acks of the CURRENT epoch
        uint32_t last_acked_epoch[8]; // per peer, the latest epoch it acked (kept across roster changes)
        uint32_t last_acked_digest[8];
        int      cov_have, cov_total; // matrix pairs with an entry (direct or relay-estimated), over the roster
        // counters
        long     probe_tx, probe_rx, echo_tx, echo_rx;
        long     probe_bytes_tx, probe_bytes_rx; // UDP payload bytes of probe + echo datagrams
        long     frames_tx, frames_rx, frame_bytes_tx, frame_bytes_rx; // FLAG_MESH stream frames
        long     bad_probe, bad_frame, rank_mismatch, epochs_published, epochs_held, ack_rx, epoch_resends;
        DWORD    first_pair_ms[8]; // client: ms from the BROKER to the first echo from each peer, 0 = none yet
        DWORD    all_acked_ms;     // host: ms from the last publish to its last ack, 0 = outstanding
        DWORD    first_epoch_ms;   // host: ms from the first client CAND to the first publish, 0 = none
    };
    void mesh_status(MeshStatus &out);
    // This client's held epoch (the last one it acked) -- M5 reads it when the hub dies. False when none.
    bool mesh_held_epoch(mesh::Epoch &out);

    // ---- mp:U62 (HM-M4) -- THE PLANNED HANDOVER OF THE HUB (plan section 5.4) ----------------------------
    // A hub whose player LEAVES the match (quit from the ESC menu, leave from the defeat/stats screen, the
    // process exiting on purpose) hands the hub to the elected successor before going. Plan Q2: a defeated
    // hub that stays and watches keeps relaying; only a hub that LEAVES hands off.
    //
    //   hub_leave(timeout_ms)  THE LEAVING SIDE. Blocks the CALLER (a game thread; the endpoint's own threads
    //       keep forwarding while it waits). Sends MK_LEAVING{epoch, successor list + dial info} to every
    //       client, keeps forwarding until every client acknowledged it (or timeout_ms), then retires its
    //       connections quietly and returns. If the first successor never acknowledges within SUCC_ACK_MS the
    //       list is re-issued with it removed (generation + 1; "then rank 2").
    //   THE SURVIVING SIDE is automatic (udp_mesh.cpp, hl_on_leaving): every client acknowledges, the first
    //       listed id rehomes AS THE HUB, the others re-dial it keeping their ids (the M2 mechanism), the M1
    //       reconcile runs, and the new hub RESTARTS the mesh (same key, re-brokered; the next epoch is
    //       numbered above every epoch any survivor holds, with the departed hub out of the roster).
    //   The departure is NOT a roster change here: the leaving player is removed from the sim roster at a
    //       PINNED step by the game's own leave path (U19b's park step for a quit, the pinned elimination for
    //       a defeat) before this is called. This layer carries no roster state.
    enum LeaveState { HL_NONE = 0, HL_WAITING = 1, HL_HANDED = 2, HL_TIMEOUT = 3, HL_NOTHING = 4 };
    int hub_leave(int timeout_ms); // returns a LeaveState (HL_HANDED / HL_TIMEOUT / HL_NOTHING)
    struct HubStatus {
        bool     enabled;      // not `[net] hub_migration=0`
        int      role;         // 0 hub, 1 client
        int      hub_id;       // the hub's player id as this endpoint knows it, -1 unknown
        int      local_id;
        uint32_t epoch;        // the succession epoch held (client) / published (hub), 0 none
        int      rank_n;
        uint8_t  rank[8];
        uint32_t survivors;    // that epoch's S
        int      leave_state;  // a LeaveState
        bool     rehomed;
        bool     reconciling, reconcile_done, unrecoverable, aborted;
        long     changes;      // hub changes this endpoint has gone through as a SURVIVOR
        int      old_hub, new_hub;
        uint32_t change_epoch; // the epoch of the LEAVING that caused the last change
        int      clients;      // hub: active client conns
        long     leaving_rx, acks_tx, acks_rx, retargets;
        // mp:U63 -- the crash failover
        int      failover;        // a FailoverPhase
        bool     failover_active; // SUSPECT or ELECT: the game suspends its own silence timers
        int      failover_ms;     // ms since the hub went silent (0 when none)
        bool     change_crash;    // the last hub change was a crash failover (the notice says "lost", not "left")
        long     fo_redirects_rx, fo_redirects_tx;
        int      fo_group;        // survivors in the group this peer judged the quorum on (popcount)
    };
    void hub_status(HubStatus &out);
    // mp:U63 (HM-M5) -- CRASH FAILOVER (plan section 5.2 / 5.3 / 7.1). Automatic, no API: a client whose hub has been
    // silent for T_suspect, and whom another survivor (direct match: over the mesh; relayed match: its own live relay
    // leg) corroborates, walks the held epoch's ranking: the first live candidate rehomes as the hub, every other
    // survivor re-dials it keeping its id. `Endpoint::hub_status` carries the phase to the game, which suspends its own
    // silence timers while `failover_active`.
    enum FailoverPhase { FO_IDLE = 0, FO_SUSPECT = 1, FO_ELECT = 2, FO_DONE = 3, FO_FAILED = 4, FO_MINORITY = 5 };
    // Test-only: this endpoint's FIRST CHOICE is `id` whatever the ranking says (a survivor that dials a non-elected
    // candidate), so REDIRECT is real. -1 = off.
    void fo_test_force_first(int id) { InterlockedExchange(&m_fo_test_first, (LONG)id); }
    // Test-only: this endpoint sends no FO_STATE and answers none (a candidate that is up but unreachable).
    void fo_test_mute(bool on) { InterlockedExchange(&m_fo_test_mute, on ? 1 : 0); }
    // The game says whether a MATCH is running (MH_Net_SetInMatch). The failover is armed only inside one: a lobby whose
    // host closes, or a client that has returned to the menu, is not a hub crash. Leaving the match clears a terminal phase.
    void set_in_match(bool on);
    // mp:U63 -- a spectator (defeated, still watching) does not count toward the failover quorum (U54 drives this).
    void set_spectator(int id, bool on);
    // MANUAL DRIVER (the MH_Net_Rehome export): the same switch the handover performs, from outside.
    struct RehomeEx {
        bool     as_hub;
        uint32_t roster_mask;   // as_hub: bit per survivor id expected to re-dial
        char     host[64];      // !as_hub, direct: the new hub's address
        int      port;
        uint32_t relay_room;    // a relayed match: the room (0 = a direct match)
        int      dial_budget_ms;
    };
    bool rehome_ex(const RehomeEx &sp);
    // Test-only: this endpoint hears a HUB_LEAVING and does NOTHING (neither acknowledges nor rehomes): a
    // successor that is gone, so "the hub re-issues the list without it" is real.
    void hl_test_mute(bool on) { InterlockedExchange(&m_hl_test_mute, on ? 1 : 0); }

    // mp:R3e -- "is `a` a peer this endpoint still holds?" True for an admitted conn that has not
    // been dropped and for a handshake still inside HS_PEND_MS; false for everything else, which
    // includes a pending entry that timed out and simply has not been reclaimed yet (the table is
    // swept lazily, by the next HELLO). Answered from any thread. This is a QUERY about the peer
    // table, not relay knowledge: the caller that needs it is the relay tunnel, whose per-peer
    // loopback socket has no way of its own to learn that the peer behind it left -- and the
    // endpoint is the party that decides that (LEAVE, the link timeout, a stream fault), so its
    // table is the one source of truth rather than a second timer next to it.
    bool knows_addr(const sockaddr_in &a);

    // ---- the acceptance test's two hooks ---------------------------------------------------------
    // Synthetic INBOUND loss, in parts per thousand, with its own deterministic generator so a
    // failing run can be replayed. Applied before anything looks at the datagram, which is what
    // makes it a model of the network rather than of a decode failure.
    void set_rx_loss(unsigned per_mille, uint32_t seed);
    // mp:U64 -- test-only PARTITION hook: drop every datagram whose SOURCE port is `port` (host order), before anything looks
    // at it. Two endpoints that block each other's port are cut apart while both stay up; on loopback every endpoint has its own port.
    void set_rx_block_port(unsigned short port, bool on);
    void counters(Counters &out) const { out = m_c; }
    // mp:T4b -- a peer that keeps talking (pings, data) but stops ACKNOWLEDGING the stream. The one
    // shape only the kept drop can catch: the silence watchdog sees a live peer. Test-only.
    void set_ack_mute(bool on) { InterlockedExchange(&m_ack_mute, on ? 1 : 0); }

    // ---- mp:T2, channel C ------------------------------------------------------------------------
    // Push `len` bytes to `dst_player` as a sequence of SHA-verified chunks. `blob` must stay alive
    // for the transfer; null means "send `len` bytes of the deterministic synthetic pattern", which
    // is what the rig knob uses so the module allocates nothing. Refuses (false) when the endpoint
    // is down, the player is not an active peer, or a transfer is already running.
    bool bulk_send(int dst_player, const void *blob, uint32_t len);
    // The same transfer pulled from a composer rather than a buffer (mp:X1's `manifest || blob`
    // image, which is never materialised contiguously). `src`/`ctx` must outlive the transfer.
    // `drop_cancels` (mp:X2f): a destination the link drops takes the transfer with it -- see
    // bulk::Channel::start_send_src. The snapshot outbox sets it; T2's own callers keep the default.
    bool bulk_send_src(int dst_player, bulk::source_fn src, void *ctx, uint32_t len,
                       bool drop_cancels = false);
    // mp:X2f -- abort a transfer that pulls through `ctx`, UNDER m_conn_cs. When this returns, the
    // recv and timer threads can no longer reach `ctx`, so its owner may free or re-arm it. True if
    // a running transfer was stopped.
    bool bulk_cancel_src(void *ctx);
    // Pop one completed chunk from the never-evictable lane. 1 on success, 0 when empty.
    int bulk_recv(uint32_t *out_chunk_id, void *buf, int *inout_len);
    // Move the RECEIVER'S frontier (mp:X1). Two callers, one primitive: re-request a chunk the
    // application refused on its manifest hash, and resume a truncated transfer at the verified
    // prefix. See udp_channel_c.h's rx_resume_at.
    void bulk_resume_at(uint32_t chunk_index);
    void bulk_stats(bulk::Stats &out);

private:
    // ---- a peer -----------------------------------------------------------------------------------
    struct Seg {
        uint16_t len;
        uint8_t  data[SEG_PAYLOAD];
    };
    struct Conn {
        // TWO LIVENESS BITS, NOT ONE, and the difference is a real 200 ms delay.
        //   `bound`  the session keys are installed, so a datagram carrying this conn_id can be
        //            OPENED. Set the moment the keys exist; cleared only by drop_conn.
        //   `active` the peer is admitted and may be SENT to / counted as a peer.
        // The window between them is not hypothetical: the host sets `active` and then sends the
        // token ack AND a WELCOME, and until the ack lands the client's conn is not active yet. A
        // receive path gated on `active` DROPS that WELCOME, and the retransmit timer repairs it one
        // RTO later -- measured as two unexplained retransmits on a clean loopback run. Gating the
        // receive on `bound` instead closes the window: anything we hold keys for, we can read.
        uint8_t       bound;
        volatile LONG active;
        sockaddr_in   addr;
        uint8_t       conn_id[mh_net_proto::CONN_ID_BYTES];
        int           player_id;

        mh_net_proto::SessionKeys       keys;
        uint64_t                        tx_seq; // packet sequence = the ChaCha20 nonce, per direction
        mh_net_proto::udp::ReplayWindow rx_win;

        // the reliable ordered segment stream
        uint32_t tx_next;  // next segment sequence to allocate
        uint32_t tx_acked; // the peer's published frontier: everything below is safely delivered
        DWORD    tx_sent_ms[SEG_WINDOW];
        Seg      tx_ring[SEG_WINDOW];
        // mp:T4b -- bytes waiting for the window (a ring; see TX_BACKLOG_BYTES), the episode they
        // belong to, and the kept drop's clock.
        uint8_t  tx_bl[TX_BACKLOG_BYTES];
        uint32_t tx_bl_head, tx_bl_len;
        DWORD    tx_bp_since;    // when this back-pressure episode began, 0 = the window is open
        uint32_t tx_bp_peak;     // ...and its most bytes queued
        long     tx_bp_bundled;  // ...and the segments cut from its backlog
        DWORD    tx_stall_since; // since when the frontier has not moved with data outstanding (0 = none)
        uint8_t  rto_backoff;    // mp:T5 -- RFC 6298 5.5: doublings since the frontier last moved
        uint8_t  rx_paused;      // mp:T4b -- the inbound ring had no room; the timer resumes delivery
        uint32_t rx_next;        // next segment sequence to deliver
        uint32_t rx_top;         // highest sequence seen so far
        uint8_t  rx_seen_any;    // ...and whether ANY has been, since sequence 0 is a legal rx_top
                                 // and "nothing received" must not read as "segment 0 is missing"
        uint8_t rx_have[SEG_WINDOW];
        Seg     rx_ring[SEG_WINDOW];
        DWORD   gap_since; // when rx_next first blocked, 0 when not blocked

        // the WireHdr reassembler sitting on top of the byte stream
        uint8_t  asm_buf[mh_net_proto::WIRE_HDR_SIZE + MH_NET_MAX_PAYLOAD];
        uint32_t asm_used;
        uint32_t asm_need;

        // mp:T3. Zeroed with the rest of the Conn by the `memset(&c, 0, sizeof(c))` that admits it,
        // which is exactly PeerStats::reset() -- the type is plain data with no owned resource, so a
        // second explicit reset would be a second statement of the same fact.
        mh::netstats::PeerStats stats;

        volatile DWORD last_rx;
        volatile LONG  tx_dead;
        volatile LONG  ping_tx, ping_rx;
        DWORD          last_ack_ms;
        DWORD          last_keep_ms;
        // mp:P15 -- WHEN this conn was admitted (set once, at admission, unlike `last_rx` which every
        // inbound packet moves) and the timer's own per-peer ping clock. Together they decide the
        // warm-up cadence in timer_loop: FAST_PING_MS while `now - admitted_ms < FAST_PING_WINDOW_MS`,
        // the steady m_ping_ms after. `last_ping_ms` is PER-CONN (not one endpoint-wide clock) because
        // two peers can be admitted seconds apart and each needs its own warm-up window.
        DWORD admitted_ms;
        DWORD last_ping_ms;

        // mp:SES6 -- proving OUTBOUND DELIVERY needs a signal narrower than `last_rx` above: that one
        // is stamped by ANY accepted packet (keepalives included, deliver_frame's FLAG_PING early-out
        // says so), which is exactly why a data-silent-but-keepalive-alive peer reads as "alive" on it
        // (the same R-live shape mh_net_export.h documents for the TCP module's last_rx_tick). These
        // four are stamped ONLY on FLAG_DATA -- deliver_frame (rx) and send() (tx) -- so a peer whose
        // sim has stopped sending shows a climbing data_rx age while data_tx stays ~0, on one side's
        // log alone. 0 = never (not "at tick 0"; GetTickCount() can legally return 0, see emit_segment's
        // own sentinel note, so age computation treats 0 as "no data yet" the same way that site does.
        volatile DWORD last_data_rx;
        volatile DWORD last_data_tx;
        long           data_rx_bytes;
        long           data_tx_bytes;
        // The peer's own advertised lockstep horizon, in ms, pushed by mh.dll via set_peer_horizon;
        // -1 = nothing pushed yet (this conn predates the first push, or the module isn't bound to
        // libmh's lockstep seam at all -- a bare net_selftest.exe run, say).
        long peer_horizon_ms;
    };

    // ---- a handshake in flight --------------------------------------------------------------------
    struct Pending {
        bool                      used;
        sockaddr_in               addr;
        DWORD                     first_ms;
        DWORD                     last_tx_ms;
        int                       tries;
        uint8_t                   cn[mh_net_proto::NONCE_LEN];
        uint8_t                   sn[mh_net_proto::NONCE_LEN];
        mh_net_proto::SessionKeys keys;
        uint8_t                   token[mh_net_proto::udp::TOKEN_WIRE];
        uint8_t                   conn_id[mh_net_proto::CONN_ID_BYTES];
        uint8_t                   slot;
        bool                      have_token; // client: the host's grant arrived, present it
        uint64_t                  seq;        // the bootstrap direction's nonce counter (seeded random)
    };

    struct Msg {
        int     src;
        int     len;
        uint8_t data[MH_NET_MAX_PAYLOAD];
    };
    // mp:U41e -- lane H's slot, sized by the classifier's own guarantee (mh_net_queue_policy.h:
    // `is_evictable` admits nothing but a BARE_HORIZON_LEN frame into this lane), exactly as
    // net_transport.cpp's HMsg is.
    struct HMsg {
        int     src;
        uint8_t data[mh::net::queue_policy::BARE_HORIZON_LEN];
    };

    // ---- plumbing ---------------------------------------------------------------------------------
    void logf(const char *fmt, ...);
    void boot_keys();
    bool send_dgram(const sockaddr_in &to, const uint8_t *pkt, size_t len);
    bool send_sealed(const sockaddr_in &to, uint8_t type, const uint8_t conn_id[8], uint64_t seq,
                     const uint8_t enc[32], const uint8_t mac[32], const uint8_t *body, size_t blen);

    // handshake
    void client_handshake_tick(DWORD now);
    void host_on_boot(const sockaddr_in &from, const uint8_t *body, size_t len, DWORD now);
    void client_on_boot(const sockaddr_in &from, const uint8_t *body, size_t len, DWORD now);
    void host_on_token(const sockaddr_in &from, const mh_net_proto::udp::Header &h, const uint8_t *body,
                       size_t len, DWORD now);
    void client_on_token_ack(const mh_net_proto::udp::Header &h, DWORD now);
    int  alloc_conn_slot();

    // stream
    void  stream_write(int idx, const uint8_t *bytes, size_t len);
    void  stream_new_segment(int idx, const uint8_t *bytes, size_t len, DWORD now); // mp:T4b
    void  stream_pump(int idx, DWORD now);                                          // mp:T4b -- backlog -> window, bundled
    void  emit_segment(int idx, uint32_t seq, bool retx);
    DWORD conn_rto_ms(const Conn &c) const; // mp:T5 -- rto_for over the conn's measured RTT
    void  on_input_frame(int idx, const uint8_t *payload, size_t len);
    void  on_state_frame(int idx, const uint8_t *payload, size_t len, DWORD now);
    void  on_bulk_frame(int idx, const uint8_t *payload, size_t len);
    void  stream_drain(int idx);
    bool  inbound_has_room(); // mp:T4b -- the receiver's half of back-pressure (see stream_drain)
    void  deliver_frame(int idx, const mh_net_proto::WireHdr &h, const uint8_t *payload, uint32_t len);
    void  send_ack(int idx, DWORD now);
    void  rto_pass(int idx, DWORD now);
    // mp:T2's one outbound edge: a CH_BULK frame on conn `idx`. Caller holds m_conn_cs.
    bool        send_bulk_payload(int idx, const uint8_t *payload, size_t len);
    static bool bulk_emit_thunk(void *ctx, int idx, const uint8_t *payload, size_t len);

    void enqueue(int src, const void *data, int len);
    // mp:U61 -- the mesh (udp_mesh.cpp). All callers hold m_conn_cs unless noted.
    void    mesh_on_frame(int idx, const mh_net_proto::WireHdr &h, const uint8_t *payload, uint32_t len);
    void    mesh_tick(DWORD now);
    void    mesh_host_tick(DWORD now);
    void    mesh_client_tick(DWORD now);
    bool    mesh_on_datagram(uint8_t *pkt, int len, const sockaddr_in &from, DWORD now); // takes m_conn_cs itself
    void    mesh_send(int idx, const uint8_t *payload, int len, int dst);
    void    mesh_install_key(const uint8_t key[mh_net_proto::KEY_LEN]);
    void    mesh_send_probe(int peer, const sockaddr_in &to, uint8_t kind, uint32_t nonce);
    void    mesh_publish(DWORD now, uint32_t S, const mesh::Matrix &snap, const mesh::Election &el);
    // mp:U62 -- the handover. hl_on_leaving runs on the recv thread UNDER m_conn_cs (it only acknowledges and
    // queues); hl_service runs on the timer thread with NO lock held between passes, because the relay tunnel's
    // role switch must not be called under m_conn_cs (see MeshHooks::tunnel_rehome).
    void hl_on_leaving(int idx, const mesh::Leaving &L);
    void hl_on_ack(int idx, const uint8_t *payload, uint32_t len);
    void hl_service();
    void hl_become_hub_state(const mesh::Leaving &L, DWORD now);
    void hl_send_go();
    void hl_leave_tick(DWORD now);
    void hl_finish_leave(const char *why);
    void hl_build_leaving(uint32_t S, mesh::Leaving &L, DWORD now);
    void mesh_reset_hub_state(DWORD now);
    // mp:U63 -- the crash failover. fo_tick runs on the timer thread UNDER m_conn_cs (detection, corroboration, the
    // choice) and posts at most one ACTION; fo_service runs with NO lock held (the relay tunnel's role switch must not)
    // and performs it. fo_on_datagram is mesh_on_datagram's FO half (the caller holds the lock).
    void     fo_tick(DWORD now);
    void     fo_service();
    void     fo_on_datagram(int from_id, uint8_t kind, uint8_t flags, uint32_t epoch, uint8_t arg,
                            const sockaddr_in &from, DWORD now);
    void     fo_send(int peer, uint8_t kind, uint8_t flags, uint8_t arg, DWORD now);
    void     fo_broadcast_state(DWORD now);
    void     fo_enter(DWORD now, const char *why);
    void     fo_finish(int phase, const char *why);
    DWORD    fo_peer_age(int peer, DWORD now); // ms since the freshest sign of life from `peer`, 0xFFFFFFFF = never
    DWORD    mesh_t(DWORD ms) const { const int d = m_cfg.mesh_time_div; return d > 1 ? (ms / (DWORD)d ? ms / (DWORD)d : 1u) : ms; } // Config::mesh_time_div
    uint32_t fo_alive_mask(DWORD now);         // peers alive-known to this one (not this peer itself)
    int      fo_my_choice(DWORD now, bool *undecided);
    void     fo_client_done(int hub, DWORD now);
    uint8_t cap_byte() const;
    // mp:U60 -- the reconcile over a new hub (all callers hold m_conn_cs).
    void rc_on_frame(int idx, const mh_net_proto::WireHdr &h, const uint8_t *payload, uint32_t len);
    void rc_tick(DWORD now);
    void rc_repair_holes(DWORD now); // mp:U63
    void rc_plan(DWORD now);
    void rc_finish(const char *why);
    bool rc_reached();
    void rc_serve_to(int to_id, int origin, uint32_t from, uint32_t to);
    void rc_ingest_range(const uint8_t *rec, int len);
    int  conn_of_player(int id);
    void refuse_conn(int idx, const char *why);
    // mp:U41e -- the per-match rollup line, the same wording (and the same log_formats.json entry,
    // `net.queue_rollup`) as net_transport.cpp's free function `queue_rollup_line`, over THIS
    // endpoint's `logf` rather than a module-global one. Called with the lock already released (file
    // I/O under m_q_cs would put the recv thread's slowest operation inside the drain path).
    void log_queue_rollup(int depth, int depth_h, int depth_m, int high, int high_h, int high_m,
                          long evicted, long refused);
    // `raw` is the frame payload AS RECEIVED (its osq prefix included) so a hub can forward it
    // untouched; `app`/`app_len` is the same payload past the prefix, which is what the game sees.
    void host_dispatch(int from_idx, const mh_net_proto::WireHdr &h, const uint8_t *raw, int raw_len,
                       const uint8_t *app, int app_len);
    void send_frame(int idx, uint16_t flags, int16_t src, int16_t dst, const void *payload, int len);
    void drop_conn(int idx, const char *why);

    bool                join_thread(HANDLE &h, const char *what); // stop()'s budgeted join
    static DWORD WINAPI recv_thunk(LPVOID);
    static DWORD WINAPI timer_thunk(LPVOID);
    void                recv_loop();
    void                timer_loop();
    void                on_datagram(uint8_t *pkt, int len, const sockaddr_in &from, DWORD now);
    bool                lose_it();

    // ---- state ------------------------------------------------------------------------------------
    bool   m_started;
    bool   m_cs_ready; // the two critical sections are initialised once and never deleted (see stop)
    Config m_cfg;
    int    m_K;
    int    m_ping_ms; // the RESOLVED values, not the config's three-state raw ones
    int    m_rx_timeout_ms;
    volatile int m_role; // mp:U60: rehome(as_hub) flips 1 -> 0 under m_conn_cs; the two threads read it bare
    int    m_my_id;
    bool   m_host_assign;
    bool   m_secure;

    SOCKET        m_sock;
    HANDLE        m_recv_thread;
    HANDLE        m_timer_thread;
    volatile LONG m_running;

    uint8_t m_psk[mh_net_proto::KEY_LEN];
    uint8_t m_boot_conn[mh_net_proto::CONN_ID_BYTES];
    uint8_t m_boot_enc[mh_net_proto::KEY_LEN];
    uint8_t m_boot_mac[mh_net_proto::KEY_LEN];
    uint8_t m_transport_id[mh_net_proto::UUID7_BYTES]; // host: the scope conn_ids are derived from

    CRITICAL_SECTION m_conn_cs;
    Conn             m_conns[MH_NET_MAX_PEERS];
    Pending          m_pend[MH_NET_MAX_PEERS];
    volatile LONG    m_dead_peer;
    volatile LONG    m_id_assigned;

    // mp:U41e -- the SAME sequence-merged lane pair as mh_net.dll's `g_lanes`/`g_qh`/`g_qm`
    // (net_transport.cpp): `m_lanes` is pure index+sequence bookkeeping, `m_qm`/`m_qh` are the
    // frames, indexed by the position the lane pair hands back.
    CRITICAL_SECTION                                            m_q_cs;
    mh::net::queue_policy::lane_queue<QUEUE_CAP_H, QUEUE_CAP_M> m_lanes;
    Msg                                                         m_qm[QUEUE_CAP_M];
    HMsg                                                        m_qh[QUEUE_CAP_H];
    long                                                        m_qdropped; // LIFETIME lane-H evictions (MH_NetStats::dropped); the PER-MATCH counters live in m_lanes
    // D24 instrumentation, unchanged: the 32-slot band already reported, so the log carries the
    // APPROACH to the cap and not only the overflow.
    int m_qhigh_band;
    // mp:U41e -- the periodic rollup, matching net_transport.cpp's QUEUE_ROLLUP_MS cadence exactly.
    DWORD m_q_rollup_at;

    // mp:U59 -- guarded by m_conn_cs (every path that touches it already holds it). POD, valid
    // zero-filled; start() arms it. ~1.7 MB: eight origins x (128 KiB ring + 2048 entries + 16 waiting).
    osq::Layer m_osq;
    bool       m_osq_legacy_said;
    volatile LONG m_test_legacy; // osq_test_legacy; NOT reset by start()

    // mp:U60 -- rehome state. Guarded by m_conn_cs. Zeroed by start(); NOT by rehome() itself.
    struct Rc {
        bool     active;       // a reconcile is in flight
        bool     skip;         // test-only: rehome(test_skip_reconcile)
        bool     done, unrecoverable, aborted, planned, dirty;
        DWORD    started_ms, deadline_ms, last_report_ms, last_repair_ms;
        uint32_t saved_gap_ms; // osq gap_force_ms to restore when the reconcile ends
        uint8_t  have[osq::ORIGINS];   // hub: a report is held for this player id
        osq::Report rep[osq::ORIGINS]; // hub: the latest report per player id
        uint32_t target[osq::ORIGINS];
        bool     target_valid;
        int      reports_in, plans, fetches_issued;
        long     ranges_served, ranges_ingested, ranges_noroom, foreign_refused;
    };
    bool          m_rehomed;                   // rehome() ran since start()
    uint8_t       m_roster[osq::ORIGINS];      // hub: ids allowed to re-dial (1 = allowed)
    int           m_hs_budget_ms;              // client re-dial budget (0 = HS_BUDGET_MS)
    volatile LONG m_welcome_tx, m_welcome_rx;  // lifetime WELCOME counters (survive rehome by design)
    Rc            m_rc;


    // ---- mp:U61 -- mesh state. Guarded by m_conn_cs. start() re-initialises it; rehome() leaves it. ---
    struct MeshPeer {
        bool            known;     // brokered: probe it (when neither side is relayed)
        bool            relayed;
        mesh::CandList  cands;
        mesh::ProbeKeys keys;      // THIS peer's keys as a SENDER (derived from the mesh key)
        mh_net_proto::udp::ReplayWindow rx_win; // its probes/echoes to us
        bool            have_best;
        sockaddr_in     best;
        DWORD           best_ms;   // when `best` last answered
        DWORD           heard_ms;  // when we last received a PROBE from it (its own probing drives the steady state)
        uint32_t        nonce;
        uint32_t        ring_nonce[8];
        int64_t         ring_qpc[8];
        int             ring_at;
        double          srtt_dms;
        long            samples;
        DWORD           last_echo_ms;
        DWORD           first_probe_ms;
        DWORD           last_probe_ms;
    };
    struct Mesh {
        bool            on;
        bool            key_valid;
        uint8_t         key[mh_net_proto::KEY_LEN];
        mesh::ProbeKeys me;
        uint64_t        tx_seq;
        MeshPeer        peer[8];
        DWORD           broker_ms, first_cand_ms;
        DWORD           first_pair_ms[8];
        // client
        bool            cand_sent;
        DWORD           last_row_ms;
        mesh::Epoch     held;
        bool            held_valid;
        // host
        mesh::CandList  reported[8];
        bool            have_cand[8], reported_relayed[8], have_row[8];
        DWORD           row_ms[8];
        mesh::Matrix    M;             // raw merged rows
        uint32_t        S;             // current roster (bit per client id)
        DWORD           roster_ms;     // when S last changed
        bool            broker_dirty;
        DWORD           last_broker_ms;
        mesh::Epoch     cur;
        uint32_t        acked_mask;
        DWORD           pub_ms, last_send_ms, last_eval_ms, all_acked_ms, first_epoch_ms;
        uint32_t        last_acked_epoch[8], last_acked_digest[8];
        // counters
        long  probe_tx, probe_rx, echo_tx, echo_rx, probe_bytes_tx, probe_bytes_rx;
        long  frames_tx, frames_rx, frame_bytes_tx, frame_bytes_rx;
        long  bad_probe, bad_frame, rank_mismatch, epochs_published, epochs_held, ack_rx, epoch_resends;
        DWORD last_stat_log_ms;
        long  last_stat_moved;
        // mp:U64 -- Config::mesh_test_delay_ms: probe/echo datagrams held until `due`, flushed by mesh_tick.
        struct Deferred {
            DWORD       due;
            int         peer;
            sockaddr_in to;
            uint8_t     kind;
            uint32_t    nonce;
        };
        Deferred defer[64];
        int      defer_n;
        bool     flushing;
    };
    // ---- mp:U62 -- the handover state. Guarded by m_conn_cs. start() re-initialises it; rehome() leaves it. ---
    struct HubLeaveState {
        // the LEAVING hub
        bool          leaving;     // hub_leave() is in progress: conn drops are not "dead peers", the mesh is quiet
        int           state;       // a LeaveState
        uint32_t      want, ack;   // clients that must acknowledge / that did
        uint32_t      removed;     // successors given up on (re-issued without them)
        bool          succ_acked;  // the CURRENT first successor acknowledged
        bool          go_sent;     // ...and the list was repeated once as the "GO": the survivors may now dial it
        uint8_t       ackc[8];     // acks per client for the CURRENT generation (>= 2 = it saw the GO too)
        DWORD         started_ms, sent_ms;
        mesh::Leaving cur;         // the list as last sent
        // a SURVIVOR
        uint8_t       seen_gen;    // highest LEAVING generation acted on (0 = none)
        bool          todo_valid;  // a LEAVING is queued for hl_service
        bool          todo_go;     // ...and released: the successor acted at once, a survivor waits for the hub's GO
        mesh::Leaving todo;
        bool          mesh_resume; // the mesh runs on a rehomed endpoint (set by the handover only)
        bool          epoch_inherited; // a rehomed hub: m_mesh.cur is the held epoch renumbered, not one it published
        int           hub_id;      // the hub's id after a change, -1 = as the held epoch says
        long          changes;
        int           old_hub, new_hub;
        uint32_t      change_epoch;
        bool          change_crash; // mp:U63 -- the last change was a crash failover
        long          leaving_rx, acks_tx, acks_rx, retargets;
    };
    HubLeaveState m_hl;
    // ---- mp:U63 -- the crash failover. Guarded by m_conn_cs. start() re-initialises it; rehome() leaves it. ---
    enum FoAct { FA_NONE = 0, FA_BECOME_HUB = 1, FA_DIAL = 2 };
    struct FoPeer {
        DWORD    ms;    // when its last FO_STATE arrived (0 = never)
        uint8_t  flags;
        uint32_t epoch;
        uint8_t  arg;
        DWORD    redir_tx_ms;
        DWORD    reply_ms;
        sockaddr_in addr;     // where its last FO_STATE came from (a verified path to it)
        bool     have_addr;
    };
    struct Failover {
        int         phase;      // a FailoverPhase
        DWORD       start_ms;   // the hub went silent (the clock the budget runs from)
        DWORD       corrob_ms;  // the loss was corroborated (0 = not yet)
        DWORD       state_tx_ms;
        uint32_t    seq;
        mesh::Epoch ep;         // the epoch the whole failover works from (this peer's held one when it began)
        FoPeer      p[8];
        uint32_t    failed;     // candidates that did not answer this round
        int         forced;     // a candidate a REDIRECT (or a higher epoch's choice) sent this peer to, -1 none
        int         cand;       // the candidate being dialled, -1 none
        DWORD       cand_ms;    // when this candidate's dial began
        int         addr_i;     // which of its addresses is being tried
        int         addr_n;
        mesh::Addr4 addrs[8];
        DWORD       addr_ms;
        int         act;        // a FoAct, posted by fo_tick for fo_service
        int         act_cand;
        int         act_addr_i;
        bool        relayed;    // this peer is behind a relay tunnel: no peer-to-peer reachability, rooms instead of addresses
        bool        dialing;    // a re-dial toward `cand` is in flight (fo_service ran rehome())
        DWORD       cand_window;// how long `cand` is tried before the next one
        bool        hub_acted;  // this peer became the hub (FA_BECOME_HUB ran)
        bool        change_pending; // the hub change is counted (and shown) only once the quorum is judged
        bool        dialed_ok;  // a client: the re-dial succeeded; DONE waits for the quorum pre-check (direct match)
        int         hub_id;     // the elected hub (-1 none yet)
        DWORD       done_ms;
        bool        quorum_judged;   // the pre-check (direct) ran
        DWORD       quorum_deadline; // the new hub's wait for its roster
        uint32_t    group;           // the group the quorum was judged on
        long        redirects_rx, redirects_tx, states_tx, states_rx, rounds;
    };
    Failover      m_fo;
    volatile LONG m_spectators;    // bit per player id; set_spectator; reset by start()
    volatile LONG m_in_match;      // set_in_match; NOT reset by start() (the game owns it)
    uint32_t      m_dead_extra;    // peers latched dead beyond the single m_dead_peer slot (take_dead_peer pops them)
    volatile LONG m_fo_test_first; // fo_test_force_first; -1 = off (the ctor sets it); NOT reset by start()
    volatile LONG m_fo_test_mute;
    volatile LONG m_hl_test_mute; // hl_test_mute; NOT reset by start()
    Mesh          m_mesh;
    MeshHooks     m_mesh_hooks;   // set once by the transport; NOT reset by start()
    volatile LONG m_test_cap;     // mesh_test_cap(c) stores c+1; 0 = unset (the ctor zeroes it); NOT reset by start()
    volatile LONG m_test_noprobe;
    volatile LONG m_test_noack;
    volatile LONG m_block_port[8]; // mp:U64 set_rx_block_port: source ports (host order) dropped on receive, 0 = free slot

    ctrl_fn m_ctrl;
    void   *m_ctrl_ctx;
    log_fn  m_log;
    void   *m_log_ctx;
    // mp:L1f -- DISPLAY ONLY. Read in get_stats() and nowhere else; null until udp_transport.cpp
    // wires it, and null for every caller that links the endpoint without a transport (the
    // selftests), which is why every read goes through the `? :` default of -1.
    path_class_fn m_path_class;
    void         *m_path_class_ctx;

    // mp:T2. NOT cleared by stop(): a transfer's chunk frontier is exactly what must survive the
    // T1b restart, so that a receiver that came back resumes from its last acknowledged chunk
    // instead of from zero.
    bulk::Channel m_bulk;
    int           m_leg_overhead;  // mp:R1d -- Config::leg_overhead, clamped
    bool          m_over_cap_said; // ...and the once-per-endpoint log of a datagram that broke it

    Counters       m_c;
    int64_t        m_qpc_freq; // mp:T3 -- QueryPerformanceFrequency, 0 until start() reads it
    unsigned       m_loss_pm;
    uint32_t       m_loss_state;
    volatile LONG  m_ack_mute;    // mp:T4b test hook -- see set_ack_mute
    DWORD          m_bp_last_log; // mp:T4b -- the back-pressure line is rate-limited to one a second
    long           m_bp_unlogged; // ...and says how many episodes the limit folded into it
    volatile DWORD m_last_rx_tick;
    volatile long  m_tx_pkts, m_tx_bytes, m_rx_pkts, m_rx_bytes;
};

} // namespace netudp
} // namespace mh

#endif // MH_NET_UDP_ENDPOINT_H
