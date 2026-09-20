#pragma once
//
// udp_channel_c.h -- CHANNEL C, the bulk reliable chunk transfer (tracker mp:T2, plan D2/D7).
//
// WHAT THIS IS. udp_endpoint.cpp (mp:T1) built a reliable ordered BYTE STREAM on channel A for the
// game's lockstep traffic, and drove channel C for ACKNOWLEDGEMENTS ONLY -- there was no piece
// sender. This file is the missing half: a sequence of chunks crossing reliably, each verified by
// its own SHA-256, resumable by chunk index after a link restart, and rate-limited so that a
// transfer in progress cannot push a lockstep datagram out of its 20 ms tick.
//
// The FRAMING is not invented here. It is T0's, already specified in docs/mp-wire-udp.md and
// implemented in mh_net_proto/net_udp.{h,cpp}: PIECE carries `chunk_id | index | total | chunk_len |
// sha256(32) | piece_seq | bytes`, and the acknowledgement is Gaffer's `ack_seq + 32-bit bitfield`.
// This file owns the STATE MACHINE over that framing and nothing about the bytes.
//
// ---- THE ONE DEVIATION, AND WHY IT IS A NEW FRAME RATHER THAN A CHANGED ONE ---------------------
//
// mp:T1 had already taken channel C's ACK frame (`kind = 1`) for a different job: the channel-A
// segment stream's acknowledgement frontier, which is what drives its timeout retransmit. That is a
// module-local reuse of a spec'd frame, it is shipped, and its retransmit path is load-bearing --
// so T2 does not take it back and does not restructure it.
//
// Instead the piece acknowledgement is a THIRD kind on the same channel: `kind = 2`, same nine-byte
// shape (`kind | ack_seq(4 LE) | ack_bits(4 LE)`). A receiver that does not know kind 2 refuses the
// frame exactly as it refuses any other malformed channel-C payload, which is the safe direction:
// both ends of a match are the same module build (the transport is refused on a mismatch long
// before this), and the relay never opens a body at all. The alternative -- moving T1's stream ack
// to a new kind -- would have changed a byte that is already on the wire in a working, measured
// path, to buy nothing but tidiness.
//
// THE ACK'S SEMANTICS ARE THIS MODULE'S, and stated once so neither end has to guess: `ack_seq` is
// the EXCLUSIVE frontier -- one past the top piece_seq the bitmap describes -- and the bitmap alone
// carries acknowledgement (bit i acknowledges `ack_seq - 1 - i`, T0's rule). Nothing is claimed for
// `ack_seq` itself. That matters at the start of a transfer, where a receiver that has received
// NOTHING must still be able to send an ack; an inclusive frontier would have it claim a piece it
// does not hold.
//
// ---- PIECE_SEQ IS AN ADDRESS, NOT A COUNTER -----------------------------------------------------
//
// `piece_seq = chunk_index * PIECES_PER_CHUNK + piece_index`, with PIECES_PER_CHUNK fixed at 16 (a
// chunk is at most 15 pieces -- T0's PIECE_TOTAL_MAX -- so 16 leaves the arithmetic a shift and
// still numbers every piece uniquely). A monotonic counter would have been the obvious choice and is
// the wrong one HERE, because this item's acceptance clause is a RESTART: a counter is state that
// has to survive the restart and agree across it, and an address is derived from the chunk index
// both ends already have. It also makes the 32-bit bitfield mean something exact -- 32 bits is
// precisely two chunks -- which is where WINDOW_CHUNKS below comes from.
//
// ---- THE NEVER-EVICTABLE LANE -------------------------------------------------------------------
//
// MP D24 measured a full inbound ring destroying replicated ORDERS, and mh_net_queue_policy.h is the
// fix: a frame may be evicted only if it is provably one superseded horizon advertisement. T2 adds
// the OTHER end of that spectrum. A completed chunk is the opposite of supersedable -- it is the
// whole point of a reliable channel -- so chunks do not enter the game's inbound ring at all (a
// 16 KiB chunk does not fit a 2048-byte Msg slot, and a megabyte of bulk data flowing through the
// ring the lockstep orders share is exactly the pressure D24 died of). They land in a lane of their
// own, and that lane's overflow policy is REFUSE, never evict: a completed-but-unadmitted chunk is
// simply not acknowledged, so the sender holds its window and the transfer applies backpressure
// instead of losing a chunk. `lane_evicted` is reported so the claim "no chunk eviction" is a number
// in the log rather than an assertion in a comment; it is zero by construction and the suite watches
// it.
//
// `mh_net_queue_policy::lane` names the three classes and is where "MSG_ORDER is never a victim"
// and "a chunk is never a victim" are the same statement.
//
// ---- WHAT THIS FILE DOES NOT DO -----------------------------------------------------------------
//
// ONE transfer at a time, one sender and one receiver per endpoint. That is what mp:X1 (snapshot)
// and mp:X2 (map download) need -- a host pushing one artefact to one joiner -- and a second
// concurrent transfer would need a transfer id on the wire that T0's framing does not carry
// (`chunk_id` is the chunk's index, not a stream id). A second `start_send` while one is running is
// REFUSED and says so, rather than interleaving two chunk sequences into one address space.
//
// The BLOB's length and chunk count are the application's, not the wire's: channel C moves a
// SEQUENCE OF CHUNKS and each piece describes its own chunk. X1/X2 will carry the manifest in chunk
// zero. The selftest knows the size because it made the blob.
//
#ifndef MH_NET_UDP_CHANNEL_C_H
#define MH_NET_UDP_CHANNEL_C_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>

#include "mh_net_proto/net_crypto.h" // SHA256_LEN / sha256
#include "mh_net_proto/net_udp.h"    // the PIECE / ACK framing this file drives
#include "mh_net_queue_policy.h"     // D24's eviction policy; T2 adds the lane classes

namespace mh {
namespace netudp {
namespace bulk {

// ---- sizes and timing ---------------------------------------------------------------------------
// 16 rather than T0's PIECE_TOTAL_MAX of 15: see the piece_seq note above. The extra address per
// chunk is never sent.
constexpr uint32_t PIECES_PER_CHUNK = 16;
constexpr uint32_t CHUNK_BYTES      = mh_net_proto::udp::CHUNK_MAX; // 16 KiB (plan D7)
// Exactly the reach of one 32-bit ack bitfield. A wider window would be acknowledged only in part,
// which turns every loss in the un-covered half into an RTO instead of a bitmap repair.
constexpr int WINDOW_CHUNKS = 2;
// The completed-chunk lane. Four chunks (64 KiB) is two windows of slack: enough that an application
// draining once per frame never stalls the transfer, small enough that the REFUSE path is reachable
// in a test rather than theoretical.
constexpr int LANE_SLOTS = 4;

constexpr uint8_t KIND_BULK_ACK = 2; // the module-local third kind -- see the header note
constexpr size_t  BULK_ACK_LEN  = 9; // kind(1) | ack_seq(4 LE) | ack_bits(4 LE)

// The BACKSTOP, not the repair. A piece unacknowledged this long is re-sent blind -- which is what
// covers a link that went away entirely. Ordinary loss is repaired far faster than this, by the rule
// below, so the number can be generous without costing anything.
constexpr DWORD BULK_RTO_MS = 250;
constexpr DWORD BULK_ACK_MS = 20; // the receiver publishes its frontier at least this often
// FAST RETRANSMIT, and the whole reason the per-chunk MASK is on the wire rather than a bare
// frontier. An acknowledgement that arrived at least this long AFTER a piece was sent, and does not
// name that piece, is evidence the piece is gone -- so it is re-sent at once instead of waiting out
// BULK_RTO_MS. The guard is BULK_ACK_MS plus slack, because an ack generated in the same millisecond
// a piece left cannot be expected to mention it, and reading that as loss would make a CLEAN link
// retransmit -- which the suite's clean arm asserts it does not.
constexpr DWORD FAST_RETX_MS = 30;
// THE RATE LIMIT IS THE ACCEPTANCE CLAUSE, not a tuning knob: the transfer may put at most this many
// pieces on the wire per endpoint tick (TICK_MS, 20 ms), which is ~440 KB/s -- fast enough that a
// 16 KiB snapshot is one tick and a megabyte is a couple of seconds, and bounded enough that a
// lockstep datagram never queues behind a burst of bulk. A transfer that ignored this would delay
// channel A by exactly the depth of its burst.
constexpr int BULK_BURST = 8;

// ---- the instrument -----------------------------------------------------------------------------
// Split tx/rx because the two ends of one transfer are two different machines' logs.
struct Stats {
    // sender
    long     tx_pieces;       // pieces put on the wire
    long     tx_retx;         // ...of which were repeats (the RTO path)
    long     tx_chunks;       // chunks fully acknowledged
    uint32_t tx_base_chunk;   // the RESUME point: the lowest chunk not fully acknowledged
    uint32_t tx_chunks_total; // 0 when no transfer is running
    long     tx_resumes;      // times a peer's ack moved the base BACKWARD is impossible; this
                              // counts restarts-from-an-ack after a link came back
    // receiver
    long     rx_pieces;
    long     rx_dup;           // a piece for a chunk already delivered, or one already held
    long     rx_out_of_window; // a piece more than WINDOW_CHUNKS ahead -- the sender will repeat it
    long     rx_mismatch;      // a piece whose header disagrees with the chunk already in the slot
    long     rx_sha_fail;      // a chunk that assembled and did not match its own SHA-256
    long     rx_chunks;        // chunks verified and admitted to the lane
    uint32_t rx_base_chunk;    // the lowest chunk not yet complete+admitted
    // the lane
    int  lane_depth;
    int  lane_high_water;
    int  lane_cap;
    long lane_drained; // chunks taken OUT of the lane, by recv_chunk or the selftest verifier
    long lane_refused; // a verified chunk the lane had no room for -> NOT acked -> backpressure
    long lane_evicted; // MUST BE ZERO. A chunk is mh::net::queue_policy::lane::bulk, which is not
                       // lane::supersedable, so no code path can choose one as a victim.
    // the `[net] bulk_selftest_mb` arm
    long verify_ok;
    long verify_fail;
    bool tx_active;
    bool tx_done;
};

// How the channel puts a channel-C payload on the wire. The endpoint owns the socket, the session
// keys and the conn table; this file owns none of them and is handed one edge.
typedef bool (*emit_fn)(void *ctx, int conn_idx, const uint8_t *payload, size_t len);

// WHERE THE SENDER'S BYTES COME FROM (mp:X1). T2 had two sources -- a caller's buffer, or the
// synthetic generator -- and X1 needs a third: an image that is a MANIFEST FOLLOWED BY A BLOB, whose
// two halves live in two different allocations. Materialising `manifest || blob` contiguously would
// mean copying 8.19 MB of world snapshot to prepend 16 KB of hashes, in a module that allocates
// nothing. A pull edge costs one function pointer and lets the composer stay virtual.
//
// The contract is total: `src` must fill EVERY byte of [off, off+len) -- past the image's end is the
// composer's business (X1 zero-fills the manifest's last chunk), because the chunk the window loads
// is whatever `chunk_len_of` says and this file does not know what the image means.
typedef void (*source_fn)(void *ctx, uint32_t off, uint8_t *out, uint32_t len);

// =================================================================================================
class Channel {
public:
    Channel();

    void bind(emit_fn fn, void *ctx) {
        m_emit     = fn;
        m_emit_ctx = ctx;
    }

    // `[net] bulk_selftest_mb` (0 = off) and `[net] bulk_selftest_step`. When the megabyte count is
    // non-zero the channel arms a SYNTHETIC transfer of that many mebibytes, started by the host the
    // first time a peer's desync sample reports a sim step at or past `step`, and the receiving side
    // drains and VERIFIES its own lane instead of waiting for an application that -- on the rig --
    // does not exist: the module's 23 exports have no bulk row, by design (mp:T2 keeps the ABI
    // untouched), so nothing in mh.exe can call recv_chunk().
    void configure(int selftest_mb, uint32_t selftest_step);

    // mp:R1d -- THE PIECE STRIDE IS A PROPERTY OF THE LINK, and this is where the endpoint tells
    // the channel which link it is on. A RELAYED endpoint runs at PIECE_MAX_RELAYED so that the
    // relay leg's 34-byte envelope still leaves the datagram inside T0's 1200-byte ceiling; a
    // direct one runs at PIECE_MAX. Both ends of one link agree because both read `[net] relay`,
    // and a promoted (mp:R3) pair keeps the envelope precisely so the stride never has to move
    // mid-transfer. Set before a transfer starts; it is read by the sender's slicing AND by the
    // receiver's offset bound, which is why it is one setting and not two.
    void set_piece_max(size_t n) {
        m_piece_max = (n == 0 || n > mh_net_proto::udp::PIECE_MAX) ? mh_net_proto::udp::PIECE_MAX : n;
    }
    size_t piece_max() const { return m_piece_max; }

    // Begin a transfer to `conn_idx`. `blob` may be null, in which case `len` bytes of a
    // deterministic synthetic pattern are sent (see synth_byte) -- which is how the rig arm moves a
    // megabyte without the module allocating one. Refuses if a transfer is already running.
    bool start_send(int conn_idx, int player_id, const uint8_t *blob, uint32_t len);
    // The same transfer, pulled from a composer rather than a buffer (mp:X1). `src` is read under
    // the endpoint's m_conn_cs, once per chunk as the window loads it -- never per piece, so a
    // retransmit costs nothing extra.
    bool start_send_src(int conn_idx, int player_id, source_fn src, void *ctx, uint32_t len);
    void abort_send();
    bool sending() const { return m_tx.active; }

    // ---- the receiver's frontier, as an APPLICATION control (mp:X1) ------------------------------
    //
    // T2 made the frontier the one thing that steers the transfer, in both directions, and gave the
    // sender no rule of its own. That makes ONE primitive serve X1's two remaining clauses, which
    // would otherwise each want a wire message:
    //
    //   RE-REQUEST. A chunk that passed its own SHA-256 on the wire can still be the wrong chunk --
    //   the sender's build, not the link, is what the per-chunk hash cannot check. X1 compares every
    //   admitted chunk against the hash the MANIFEST declared for that index; a mismatch rewinds the
    //   frontier to it, and the sender's `tx_seek` backward path re-sends it. No NAK frame exists.
    //
    //   RESUME AFTER A TRUNCATION. An application that kept its verified prefix across a link
    //   restart restores the frontier to `manifest_chunks + verified`, and the first acknowledgement
    //   carries it -- so the sender skips what is already held instead of starting at zero.
    //
    // Rewinding does NOT touch the lane: chunks already admitted are the application's, and this is
    // not a way to un-deliver one.
    void     rx_resume_at(uint32_t chunk_index);
    uint32_t rx_base_chunk() const { return m_rx.base; }

    // ---- driven by the endpoint -----------------------------------------------------------------
    // All four run under the endpoint's m_conn_cs.
    // Both return false when the payload is not a well-formed frame of their kind, which is what
    // the endpoint counts as `malformed` -- this file keeps no opinion about the endpoint's counters.
    bool on_piece(int conn_idx, const uint8_t *payload, size_t len, DWORD now);
    bool on_ack(const uint8_t *payload, size_t len, DWORD now);
    // `player_id` is how a transfer survives the peer coming back on a different conn slot (mp:T1b):
    // the target is a PLAYER, and the conn index is re-bound here every tick.
    void tick(int conn_idx, int player_id, DWORD now);
    // The `[net] bulk_selftest_*` trigger. Fed the payload of a FLAG_HASH control frame, which
    // carries the sender's sim step; a payload that is not one is ignored, so this cannot misfire on
    // a future control frame.
    void note_hash_frame(int conn_idx, int player_id, const uint8_t *payload, size_t len);

    // ---- the never-evictable lane ----------------------------------------------------------------
    // Pop one completed, SHA-verified chunk. Returns 0 when the lane is empty, 1 on success with
    // `*inout_len` set to the chunk's length. The application's drain; on the rig the selftest arm
    // drains internally instead (see configure).
    int recv_chunk(uint32_t *out_chunk_id, void *buf, int *inout_len);

    void stats(Stats &out) const;
    // Render the lane/transfer rollup into `out` and return true when anything moved since the last
    // call -- so an idle link stays silent and the log carries one line per thing that happened.
    bool rollup_line(char *out, size_t cap);

private:
    struct TxChunk {
        uint32_t index;
        uint32_t len;
        uint16_t total;  // pieces
        uint16_t acked;  // bit i = piece i acknowledged
        DWORD    ack_ms; // when an acknowledgement last DESCRIBED this chunk (0 = never)
        bool     valid;  // the buffer below holds `index`
        DWORD    sent[PIECES_PER_CHUNK];
        uint8_t  sha[mh_net_proto::SHA256_LEN];
        uint8_t  data[CHUNK_BYTES];
    };
    struct RxChunk {
        uint32_t index;
        uint32_t len;
        uint16_t total;
        uint16_t have;
        bool     valid;
        bool     complete; // assembled and SHA-verified, waiting for the lane
        uint8_t  sha[mh_net_proto::SHA256_LEN];
        uint8_t  data[CHUNK_BYTES];
    };
    struct LaneSlot {
        uint32_t chunk_id;
        uint32_t len;
        uint8_t  data[CHUNK_BYTES];
    };

    struct Tx {
        bool           active;
        bool           done;
        int            conn;
        int            player;
        const uint8_t *blob; // null = synthetic, unless `src` is set
        source_fn      src;  // non-null = pull from the composer (mp:X1); wins over `blob`
        void          *src_ctx;
        uint32_t       blob_len;
        uint32_t       chunks;
        uint32_t       base;
        TxChunk        win[WINDOW_CHUNKS];
        DWORD          last_progress_ms;
    } m_tx;

    struct Rx {
        uint32_t base;
        RxChunk  win[WINDOW_CHUNKS];
        DWORD    last_ack_ms;
        DWORD    last_piece_ms; // acks stop when a transfer does -- an idle link stays silent
        bool     ack_due;
        int      conn; // where to send the acknowledgement
    } m_rx;

    struct Lane {
        LaneSlot slot[LANE_SLOTS];
        int      head, count, high_water;
        long     refused;
        long     evicted; // see Stats::lane_evicted
    } m_lane;

    // sender internals
    void     tx_load(int w, uint32_t chunk_index);
    void     tx_seek(uint32_t new_base); // follow the receiver's frontier, in either direction
    uint32_t chunk_len_of(uint32_t index) const;
    void     fill_chunk(uint32_t index, uint8_t *out, uint32_t len) const;
    void     send_piece(int w, uint16_t piece, DWORD now, bool retx);

    // receiver internals
    void rx_shift(uint32_t new_base);
    void rx_try_admit();
    void send_ack(DWORD now);
    void verify_drain();

    emit_fn m_emit;
    void   *m_emit_ctx;

    size_t   m_piece_max; // mp:R1d -- PIECE_MAX, or PIECE_MAX_RELAYED on a relayed link
    int      m_selftest_mb;
    uint32_t m_selftest_step;
    bool     m_selftest_armed; // a transfer was started by the step trigger; never twice

    Stats m_s;
    Stats m_prev_logged;
};

// The deterministic synthetic byte at absolute offset `off`. A cheap xorshift-flavoured mix rather
// than a counter, so a piece landing at the wrong offset changes the chunk's SHA instead of
// producing a plausible-looking run of bytes.
inline uint8_t synth_byte(uint32_t off) {
    uint32_t x = off * 2654435761u + 0x9e3779b9u;
    x ^= x >> 15;
    x *= 0x85ebca6bu;
    x ^= x >> 13;
    return (uint8_t)(x & 0xffu);
}

} // namespace bulk
} // namespace netudp
} // namespace mh

#endif // MH_NET_UDP_CHANNEL_C_H
