//
// udp_channel_c.cpp -- channel C's state machine (tracker mp:T2). The WHY is in udp_channel_c.h.
//
// Three objects, and the whole item is the contract between them:
//   Tx   a window of WINDOW_CHUNKS chunks, each with a per-piece acknowledgement mask and a per-piece
//        send stamp. It puts at most BULK_BURST pieces on the wire per tick, which is the clause
//        about not delaying channel A.
//   Rx   the same window on the other side, assembling pieces into chunks and refusing any chunk
//        whose bytes do not match the SHA-256 the pieces themselves carried.
//   Lane the completed-chunk queue whose overflow policy is REFUSE. Nothing here can evict.
//
// THE SENDER FOLLOWS THE RECEIVER'S BASE, it does not track its own. `on_ack` derives the receiver's
// lowest incomplete chunk from the frontier and shifts the send window to it. That is one line of
// arithmetic and it is what makes the restart clause fall out for free: a receiver that comes back
// after a stop() still holds its base, its first acknowledgement carries it, and the sender resumes
// there instead of at chunk zero. There is no resume message, no side channel, and nothing to
// persist -- which is exactly the property plan D7 asked the per-piece SHA to buy.
//
#include "udp_channel_c.h"

#include <stdio.h>
#include <string.h>

namespace mh {
namespace netudp {
namespace bulk {

namespace U  = mh_net_proto::udp;
namespace QP = mh::net::queue_policy;

// The lane rule, checked by the compiler rather than asserted in prose: a completed chunk is in a
// class the eviction path cannot reach. If someone ever widens `lane_evictable`, this file stops
// building -- which is the only way a "never" survives a year of edits.
static_assert(!QP::lane_evictable(QP::lane_of_bulk_chunk()),
              "a completed bulk chunk must never be in an evictable lane (mp:T2 / MP D24)");

namespace {

// Explicit little-endian byte work, as everything in mh_net_proto is: a packed struct would be a
// different format under a different compiler.
inline void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}
inline uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// The desync sample's header (mh/desync/desync_watch.h sample_wire): magic(4) | version(2) |
// region_count(2) | manifest_fp(8) | step(4). Read here rather than included, because this module
// must not depend on mh.dll's headers -- and the only field it wants is the step.
constexpr uint32_t DSNC_MAGIC     = 0x434e5344u; // 'DSNC', little-endian
constexpr size_t   DSNC_STEP_OFF  = 16;
constexpr size_t   DSNC_MIN_BYTES = DSNC_STEP_OFF + 4;

inline uint16_t full_mask(uint16_t total) {
    return (uint16_t)((1u << total) - 1u);
}

} // namespace

Channel::Channel() {
    memset(&m_tx, 0, sizeof(m_tx));
    memset(&m_rx, 0, sizeof(m_rx));
    memset(&m_lane, 0, sizeof(m_lane));
    memset(&m_s, 0, sizeof(m_s));
    memset(&m_prev_logged, 0, sizeof(m_prev_logged));
    m_emit           = nullptr;
    m_emit_ctx       = nullptr;
    m_piece_max      = U::PIECE_MAX; // mp:R1d -- the endpoint lowers it on a relayed link
    m_link_rto_ms    = 0;            // mp:T5 -- the constants until the endpoint measures a link
    m_link_fast_ms   = 0;
    m_selftest_mb    = 0;
    m_selftest_step  = 0;
    m_selftest_armed = false;
    m_rx.conn        = -1;
    m_tx.conn        = -1;
    m_tx.player      = -1;
    m_s.lane_cap     = LANE_SLOTS;
}

void Channel::configure(int selftest_mb, uint32_t selftest_step) {
    m_selftest_mb   = selftest_mb < 0 ? 0 : selftest_mb;
    m_selftest_step = selftest_step;
}

// =================================================================================================
// THE SENDER
// =================================================================================================
uint32_t Channel::chunk_len_of(uint32_t index) const {
    if (index >= m_tx.chunks) return 0;
    const uint32_t start = index * CHUNK_BYTES;
    const uint32_t left  = m_tx.blob_len - start;
    return left > CHUNK_BYTES ? CHUNK_BYTES : left;
}

void Channel::fill_chunk(uint32_t index, uint8_t *out, uint32_t len) const {
    const uint32_t start = index * CHUNK_BYTES;
    if (m_tx.src != nullptr) {
        // mp:X1's composed image. Pulled once per chunk LOAD, not per piece -- so a window that is
        // retransmitted ten times still asks the composer once, and the composer may be as
        // expensive as it likes.
        m_tx.src(m_tx.src_ctx, start, out, len);
        return;
    }
    if (m_tx.blob != nullptr) {
        memcpy(out, m_tx.blob + start, len);
        return;
    }
    // The synthetic blob. Generated rather than allocated: the rig arm moves a megabyte and this
    // module allocates nothing, so the knob costs one 16 KiB window slot it already owns.
    for (uint32_t i = 0; i < len; ++i) out[i] = synth_byte(start + i);
}

void Channel::tx_load(int w, uint32_t chunk_index) {
    TxChunk &c = m_tx.win[w];
    memset(c.sent, 0, sizeof(c.sent));
    c.acked  = 0;
    c.ack_ms = 0;
    c.index  = chunk_index;
    if (chunk_index >= m_tx.chunks) {
        c.valid = false;
        c.len   = 0;
        c.total = 0;
        return;
    }
    c.len = chunk_len_of(chunk_index);
    fill_chunk(chunk_index, c.data, c.len);
    c.total = U::piece_count(c.len, m_piece_max);
    mh_net_proto::sha256(c.data, c.len, c.sha);
    c.valid = (c.total != 0);
}

// Move the send window to the receiver's frontier -- in EITHER direction.
//
// Forward is the ordinary case and shifts, keeping the chunk that is already loaded. BACKWARD is not
// a hypothetical: a receiver that lost its state entirely (a restarted PROCESS, not the endpoint
// restart mp:T1b covers) comes back with a frontier of zero while this sender is still at chunk N,
// and a sender that could only advance would sit there re-sending a window the peer will never
// accept -- a deadlock, measured exactly that way while mutation-testing the resume arm. Following
// the frontier down costs a repeat of what was already delivered and cannot get stuck. A stale
// acknowledgement reordered behind a newer one causes one needless rewind, which the next
// acknowledgement corrects; the packet layer's replay window means it cannot arrive twice.
void Channel::tx_seek(uint32_t new_base) {
    if (new_base > m_tx.chunks) new_base = m_tx.chunks;
    if (new_base < m_tx.base) {
        ++m_s.tx_resumes;
        m_tx.base = new_base;
        for (int w = 0; w < WINDOW_CHUNKS; ++w) tx_load(w, m_tx.base + (uint32_t)w);
    }
    while (m_tx.base < new_base) {
        for (int w = 0; w + 1 < WINDOW_CHUNKS; ++w) m_tx.win[w] = m_tx.win[w + 1];
        ++m_tx.base;
        tx_load(WINDOW_CHUNKS - 1, m_tx.base + (uint32_t)(WINDOW_CHUNKS - 1));
    }
    m_s.tx_chunks     = (long)m_tx.base;
    m_s.tx_base_chunk = m_tx.base;
}

bool Channel::start_send_src(int conn_idx, int player_id, source_fn src, void *ctx, uint32_t len) {
    if (src == nullptr) return false;
    if (!start_send(conn_idx, player_id, nullptr, len)) return false;
    m_tx.src     = src;
    m_tx.src_ctx = ctx;
    // The window was loaded from the SYNTHETIC generator a statement ago -- start_send does not know
    // a composer is coming. Re-load it now rather than reordering start_send, so the two entries
    // keep one arming path and a future edit to it cannot apply to only one of them.
    for (int w = 0; w < WINDOW_CHUNKS; ++w) tx_load(w, m_tx.base + (uint32_t)w);
    return true;
}

bool Channel::start_send(int conn_idx, int player_id, const uint8_t *blob, uint32_t len) {
    if (m_tx.active) return false; // one transfer at a time -- see the header
    if (len == 0) return false;
    const uint64_t chunks = ((uint64_t)len + CHUNK_BYTES - 1u) / CHUNK_BYTES;
    // The address space is piece_seq = chunk * 16, so the chunk index has 28 usable bits. At 16 KiB
    // a chunk that is 4 TiB; the check exists so the refusal is here rather than in an overflow.
    if (chunks == 0 || chunks > 0x0ffffffful) return false;
    memset(&m_tx, 0, sizeof(m_tx));
    m_tx.active           = true;
    m_tx.conn             = conn_idx;
    m_tx.player           = player_id;
    m_tx.blob             = blob;
    m_tx.blob_len         = len;
    m_tx.chunks           = (uint32_t)chunks;
    m_tx.base             = 0;
    m_tx.last_progress_ms = GetTickCount();
    for (int w = 0; w < WINDOW_CHUNKS; ++w) tx_load(w, (uint32_t)w);
    m_s.tx_active       = true;
    m_s.tx_done         = false;
    m_s.tx_chunks_total = m_tx.chunks;
    m_s.tx_base_chunk   = 0;
    return true;
}

void Channel::abort_send() {
    m_tx.active         = false;
    m_s.tx_active       = false;
    m_s.tx_chunks_total = 0;
}

void Channel::send_piece(int w, uint16_t piece, DWORD now, bool retx) {
    TxChunk &c = m_tx.win[w];
    if (!c.valid || piece >= c.total) return;
    U::Piece p;
    if (!U::piece_for(c.data, c.len, c.index, piece, c.index * PIECES_PER_CHUNK + (uint32_t)piece, p,
                      c.sha, m_piece_max))
        return;
    uint8_t      buf[U::PIECE_HDR + U::PIECE_MAX];
    const size_t n = U::piece_encode(p, buf, sizeof(buf));
    if (n == 0) return; // refused by the encoder = a bug here, and silence beats garbage
    if (m_emit == nullptr || !m_emit(m_emit_ctx, m_tx.conn, buf, n)) return;
    c.sent[piece] = (now == 0) ? 1u : now; // 0 is this ring's "never sent" sentinel
    ++m_s.tx_pieces;
    if (retx) ++m_s.tx_retx;
}

// =================================================================================================
// THE RECEIVER
// =================================================================================================
void Channel::rx_shift(uint32_t new_base) {
    while (m_rx.base < new_base) {
        for (int w = 0; w + 1 < WINDOW_CHUNKS; ++w) m_rx.win[w] = m_rx.win[w + 1];
        RxChunk &tail = m_rx.win[WINDOW_CHUNKS - 1];
        tail.valid    = false;
        tail.complete = false;
        tail.have     = 0;
        ++m_rx.base;
    }
    m_s.rx_base_chunk = m_rx.base;
}

// THE APPLICATION'S FRONTIER CONTROL (mp:X1) -- see the header for the two clauses it serves.
//
// It moves the base and DROPS the window, because every reason to call it says the window's contents
// are not wanted: a re-request means the chunk that is there is the wrong one, and a resume means
// this receiver has never seen the chunks the window happens to hold. The lane is untouched --
// rewinding the frontier is not a way to un-deliver a chunk the application already took.
//
// The acknowledgement is sent immediately rather than left to the next tick, because the sender may
// be parked: it has no unacknowledged piece to time out on, so nothing but an ack can move it.
void Channel::rx_resume_at(uint32_t chunk_index) {
    m_rx.base = chunk_index;
    for (int w = 0; w < WINDOW_CHUNKS; ++w) {
        m_rx.win[w].valid    = false;
        m_rx.win[w].complete = false;
        m_rx.win[w].have     = 0;
    }
    m_s.rx_base_chunk = m_rx.base;
    if (m_rx.conn >= 0) send_ack(GetTickCount());
}

void Channel::rx_try_admit() {
    for (;;) {
        RxChunk &c = m_rx.win[0];
        if (!c.valid || !c.complete) return;
        if (m_lane.count >= LANE_SLOTS) {
            // BACKPRESSURE, NOT LOSS. The chunk stays assembled in the window and the base does not
            // advance, so the acknowledgement keeps naming this chunk and the sender keeps its
            // window here. Nothing is destroyed; `lane_evicted` stays zero because no code can
            // choose a lane::bulk frame as a victim.
            ++m_lane.refused;
            m_s.lane_refused = m_lane.refused;
            return;
        }
        LaneSlot &s = m_lane.slot[(m_lane.head + m_lane.count) % LANE_SLOTS];
        s.chunk_id  = c.index;
        s.len       = c.len;
        memcpy(s.data, c.data, c.len);
        ++m_lane.count;
        if (m_lane.count > m_lane.high_water) m_lane.high_water = m_lane.count;
        m_s.lane_depth      = m_lane.count;
        m_s.lane_high_water = m_lane.high_water;
        ++m_s.rx_chunks;
        rx_shift(m_rx.base + 1);
        if (m_selftest_mb > 0) verify_drain();
    }
}

bool Channel::on_piece(int conn_idx, const uint8_t *payload, size_t len, DWORD now) {
    U::Piece   p;
    U::Verdict why = U::Verdict::Ok;
    if (!U::piece_decode(payload, len, p, why, m_piece_max)) return false;

    m_rx.conn          = conn_idx;
    m_rx.last_piece_ms = (now == 0) ? 1u : now;
    ++m_s.rx_pieces;

    if (p.chunk_id < m_rx.base) {
        // Already delivered. The sender has not seen our frontier yet (a lost ack, or a link that
        // came back) -- so answer with one rather than staying silent, which is what turns a lost
        // acknowledgement into a stalled transfer.
        ++m_s.rx_dup;
        send_ack(now);
        return true;
    }
    if (p.chunk_id >= m_rx.base + (uint32_t)WINDOW_CHUNKS) {
        ++m_s.rx_out_of_window; // the sender is ahead of our window; its RTO will repeat this
        return true;
    }
    const int w = (int)(p.chunk_id - m_rx.base);
    RxChunk  &c = m_rx.win[w];
    if (!c.valid) {
        c.valid    = true;
        c.complete = false;
        c.index    = p.chunk_id;
        c.len      = p.chunk_len;
        c.total    = p.total;
        c.have     = 0;
        memcpy(c.sha, p.chunk_sha, mh_net_proto::SHA256_LEN);
    } else if (c.index != p.chunk_id || c.len != p.chunk_len || c.total != p.total ||
               memcmp(c.sha, p.chunk_sha, mh_net_proto::SHA256_LEN) != 0) {
        // A piece describing a DIFFERENT chunk at the same index -- a straggler from a transfer that
        // was aborted and restarted. Refused rather than merged: merging two chunks' bytes is how a
        // reliable channel delivers something neither end ever sent.
        ++m_s.rx_mismatch;
        return true;
    }
    if (p.index >= c.total || p.index >= PIECES_PER_CHUNK) return true;
    if (c.have & (uint16_t)(1u << p.index)) {
        ++m_s.rx_dup;
        return true;
    }
    // THE BOUND, STATED HERE AND NOT ASSUMED FROM THE CODEC. `piece_decode` checks that `total` is
    // the piece count `chunk_len` implies and that `piece_len` matches the frame -- but nothing
    // there ties `piece_len` to the INDEX, so a peer could declare the last piece of a 16384-byte
    // chunk (offset 15400, 984 bytes left) as a full 1100 and walk 116 bytes off the end of this
    // buffer. Authenticated is not the same as correct: the peer holding the session key is the
    // peer whose build might be wrong. Every piece but the last is exactly the STRIDE; the last is
    // the remainder; anything else is a chunk this receiver did not agree to assemble. mp:R1d made
    // the stride a link property (m_piece_max) rather than the constant it used to be -- and the
    // bound is asserted against OUR stride deliberately: a peer sending at a stride we did not
    // agree to is refused as a mismatch, not reassembled at its offsets.
    const uint32_t pm   = (uint32_t)m_piece_max;
    const uint32_t off  = (uint32_t)p.index * pm;
    const uint32_t want = (c.len - off > pm) ? pm : c.len - off;
    if (c.len > CHUNK_BYTES || off >= c.len || (uint32_t)p.len != want) {
        ++m_s.rx_mismatch;
        return true;
    }
    memcpy(c.data + off, p.bytes, p.len);
    c.have = (uint16_t)(c.have | (uint16_t)(1u << p.index));
    if (c.have == full_mask(c.total) && !c.complete) {
        uint8_t h[mh_net_proto::SHA256_LEN];
        mh_net_proto::sha256(c.data, c.len, h);
        if (memcmp(h, c.sha, mh_net_proto::SHA256_LEN) != 0) {
            // The pieces arrived and the chunk is not what the sender hashed. Drop the assembly and
            // acknowledge nothing: the sender's RTO re-sends, which is the only repair that can
            // work, and the counter says it happened rather than a corrupt chunk being delivered.
            ++m_s.rx_sha_fail;
            c.have = 0;
            return true;
        }
        c.complete = true;
        rx_try_admit();
        send_ack(now);
    }
    return true;
}

void Channel::send_ack(DWORD now) {
    if (m_emit == nullptr || m_rx.conn < 0) return;
    // EXCLUSIVE frontier: one past the top piece_seq the bitmap describes. See the header.
    const uint32_t ack_seq = (m_rx.base + 1u) * PIECES_PER_CHUNK;
    uint32_t       bits    = 0;
    const RxChunk &c       = m_rx.win[0];
    // A chunk that is COMPLETE but not yet admitted (the lane was full) reports its pieces as
    // received, which is the truth -- they are. What it does NOT do is advance `base`, and that is
    // the backpressure: the sender stays on this chunk until the application drains.
    const uint16_t have = (c.valid && c.index == m_rx.base) ? c.have : (uint16_t)0;
    for (int i = 0; i < 16; ++i) {
        const int piece = 15 - i; // bit i acknowledges ack_seq - 1 - i
        if (have & (uint16_t)(1u << piece)) bits |= (1u << i);
    }
    // Bits 16..31 reach chunk `base - 1`, every piece of which is delivered by definition -- `base`
    // is the LOWEST incomplete chunk. Saying so is what repairs a lost acknowledgement without a
    // round trip: the sender learns the frontier from the bitmap it already reads.
    if (m_rx.base > 0) bits |= 0xffff0000u;

    uint8_t pay[BULK_ACK_LEN];
    pay[0] = KIND_BULK_ACK;
    put_u32(pay + 1, ack_seq);
    put_u32(pay + 5, bits);
    m_emit(m_emit_ctx, m_rx.conn, pay, BULK_ACK_LEN);
    m_rx.last_ack_ms = (now == 0) ? 1u : now;
    m_rx.ack_due     = false;
}

bool Channel::on_ack(int conn_idx, const uint8_t *payload, size_t len, DWORD now) {
    if (payload == nullptr || len != BULK_ACK_LEN || payload[0] != KIND_BULK_ACK) return false;
    const uint32_t ack_seq = get_u32(payload + 1);
    const uint32_t bits    = get_u32(payload + 5);
    if (!m_tx.active) return true;
    // mp:T2a. THE MISSING CHECK a two-joiners-in-a-row scenario finds and a two-endpoint one never
    // can: this Channel is ONE PER ENDPOINT, so `m_tx` addresses whichever peer is CURRENTLY being
    // sent to, but an ack frame arrives on a SPECIFIC connection (`conn_idx`, from `on_bulk_frame`)
    // that may not be it. A peer whose OWN transfer just finished keeps re-announcing its full
    // frontier for up to ~2s after its last piece (the `tick()` heartbeat, `m_rx.last_piece_ms`
    // window below) -- so its FINAL ack can still be in flight, or repeated, after the host has
    // already re-armed a DIFFERENT transfer to a DIFFERENT peer. Applying it unfiltered clamps
    // `rx_base` to `m_tx.chunks` (the clamp two lines down) and completes the NEW transfer on the
    // strength of the OLD peer's ack -- silently, with `mismatch`/`sha_fail` still zero, because
    // nothing about the bytes was wrong; the receiver just never got them. Measured 2026-09-23
    // building mp:T2a's own selftest arm (three endpoints, one host, two joiners in a row): the
    // host's tx reported the second transfer complete in one tick while the second joiner's rx sat
    // at a handful of pieces. A two-endpoint arm cannot produce this ack at all, which is why nothing
    // upstream of T2a's own multi-joiner arm ever exercised it.
    if (m_tx.conn != conn_idx) return true;
    // The frontier is a chunk boundary by construction; anything else is not this module's ack.
    if (ack_seq < PIECES_PER_CHUNK || (ack_seq % PIECES_PER_CHUNK) != 0) return true;

    uint32_t rx_base = ack_seq / PIECES_PER_CHUNK - 1u;
    if (rx_base > m_tx.chunks) rx_base = m_tx.chunks;
    // THE RECEIVER'S FRONTIER IS THE ONLY THING THAT ADVANCES THE SENDER. A tempting second rule --
    // "all of my base chunk's pieces are acknowledged, so move on" -- is wrong, and wrong in a way
    // that corrupts: a receiver whose LANE IS FULL reports its chunk fully received and does NOT
    // advance (that is the backpressure), so the sender would run one chunk ahead and then apply the
    // next acknowledgement's per-piece bits to the wrong chunk.
    if (rx_base != m_tx.base) tx_seek(rx_base);

    TxChunk &c = m_tx.win[0];
    if (c.valid && c.index == rx_base && rx_base == m_tx.base) {
        for (int i = 0; i < 16; ++i) {
            if ((bits & (1u << i)) == 0) continue;
            const int piece = 15 - i;
            if (piece < (int)c.total) c.acked = (uint16_t)(c.acked | (uint16_t)(1u << piece));
        }
        c.ack_ms = (now == 0) ? 1u : now; // the fast-retransmit clock, see tick()
    }
    if (m_tx.base >= m_tx.chunks) {
        m_tx.active   = false;
        m_tx.done     = true;
        m_s.tx_active = false;
        m_s.tx_done   = true;
    }
    return true;
}

// =================================================================================================
// THE TICK
// =================================================================================================
void Channel::tick(int conn_idx, int player_id, DWORD now) {
    if (m_tx.active && m_tx.player >= 0 && m_tx.player == player_id && m_tx.conn != conn_idx) {
        // mp:T1b -- the peer came back on a different conn slot. The transfer is addressed to a
        // PLAYER, so re-bind and let the first acknowledgement say where to resume.
        m_tx.conn = conn_idx;
        ++m_s.tx_resumes;
    }
    if (m_tx.active && m_tx.conn == conn_idx) {
        // mp:T5 -- the larger of the LAN constant and what the link measured (set_link_timing).
        const DWORD rto  = m_link_rto_ms > BULK_RTO_MS ? m_link_rto_ms : BULK_RTO_MS;
        const DWORD fast = m_link_fast_ms > FAST_RETX_MS ? m_link_fast_ms : FAST_RETX_MS;
        int         budget = BULK_BURST;
        for (int w = 0; w < WINDOW_CHUNKS && budget > 0; ++w) {
            TxChunk &c = m_tx.win[w];
            if (!c.valid) continue;
            for (uint16_t i = 0; i < c.total && budget > 0; ++i) {
                if (c.acked & (uint16_t)(1u << i)) continue;
                const DWORD sent = c.sent[i];
                if (sent != 0) {
                    // Gone, on the evidence of an acknowledgement that had time to mention it and
                    // did not -- or, failing that, simply overdue.
                    const bool refuted = c.ack_ms != 0 && (long)(c.ack_ms - sent) >= (long)fast;
                    if (!refuted && (now - sent) < rto) continue;
                }
                send_piece(w, i, now, sent != 0);
                --budget;
            }
        }
    }
    // The receiver publishes its frontier while a transfer is live, and goes quiet two seconds after
    // the last piece -- so an idle link costs nothing and a stalled one keeps telling the sender
    // where to resume.
    if (m_rx.conn == conn_idx && m_rx.last_piece_ms != 0 && (now - m_rx.last_piece_ms) < 2000u &&
        (now - m_rx.last_ack_ms) >= BULK_ACK_MS)
        send_ack(now);
}

void Channel::note_hash_frame(int conn_idx, int player_id, const uint8_t *payload, size_t len) {
    if (m_selftest_mb <= 0 || m_selftest_armed || m_tx.active) return;
    if (payload == nullptr || len < DSNC_MIN_BYTES) return;
    if (get_u32(payload) != DSNC_MAGIC) return; // not a desync sample: ignore, never guess
    const uint32_t step = get_u32(payload + DSNC_STEP_OFF);
    if (step < m_selftest_step) return;
    m_selftest_armed = true;
    start_send(conn_idx, player_id, nullptr, (uint32_t)m_selftest_mb * 1024u * 1024u);
}

// =================================================================================================
// THE LANE
// =================================================================================================
int Channel::recv_chunk(uint32_t *out_chunk_id, void *buf, int *inout_len) {
    if (buf == nullptr || inout_len == nullptr) return 0;
    if (m_lane.count <= 0) return 0;
    LaneSlot &s = m_lane.slot[m_lane.head];
    if ((int)s.len > *inout_len) return 0; // the caller's buffer is too small: NOT a drop
    memcpy(buf, s.data, s.len);
    *inout_len = (int)s.len;
    if (out_chunk_id != nullptr) *out_chunk_id = s.chunk_id;
    m_lane.head = (m_lane.head + 1) % LANE_SLOTS;
    --m_lane.count;
    ++m_s.lane_drained;
    m_s.lane_depth = m_lane.count;
    // Draining may have made room for a chunk the lane refused a moment ago -- and if it did, the
    // SENDER has to be told. It is parked on a chunk whose pieces are all acknowledged, so it is
    // sending nothing and the periodic acknowledgement has gone quiet with it; without this the
    // backpressure would never lift. (Measured as a deadlock the first time the lane arm ran.)
    const uint32_t was = m_rx.base;
    rx_try_admit();
    if (m_rx.base != was) send_ack(GetTickCount());
    return 1;
}

void Channel::verify_drain() {
    while (m_lane.count > 0) {
        LaneSlot      &s   = m_lane.slot[m_lane.head];
        const uint32_t off = s.chunk_id * CHUNK_BYTES;
        bool           ok  = true;
        for (uint32_t i = 0; i < s.len; ++i)
            if (s.data[i] != synth_byte(off + i)) {
                ok = false;
                break;
            }
        if (ok) ++m_s.verify_ok;
        else ++m_s.verify_fail;
        m_lane.head = (m_lane.head + 1) % LANE_SLOTS;
        --m_lane.count;
        ++m_s.lane_drained;
        m_s.lane_depth = m_lane.count;
    }
}

void Channel::stats(Stats &out) const {
    out                 = m_s;
    out.lane_depth      = m_lane.count;
    out.lane_high_water = m_lane.high_water;
    out.lane_cap        = LANE_SLOTS;
    out.lane_refused    = m_lane.refused;
    out.lane_evicted    = m_lane.evicted;
    out.tx_base_chunk   = m_tx.base;
    out.rx_base_chunk   = m_rx.base;
}

bool Channel::rollup_line(char *out, size_t cap) {
    Stats s;
    stats(s);
    const bool moved = s.tx_pieces != m_prev_logged.tx_pieces ||
                       s.tx_chunks != m_prev_logged.tx_chunks ||
                       s.rx_pieces != m_prev_logged.rx_pieces ||
                       s.rx_chunks != m_prev_logged.rx_chunks ||
                       s.lane_refused != m_prev_logged.lane_refused ||
                       s.lane_evicted != m_prev_logged.lane_evicted ||
                       s.lane_high_water != m_prev_logged.lane_high_water ||
                       s.rx_sha_fail != m_prev_logged.rx_sha_fail ||
                       s.verify_ok != m_prev_logged.verify_ok ||
                       s.verify_fail != m_prev_logged.verify_fail;
    if (!moved) return false;
    m_prev_logged = s;
    _snprintf(out, cap,
              "net: udp bulk chunk lane depth %d / %d high-water %d evicted %ld refused %ld | tx "
              "chunk %u/%u pieces %ld (retx %ld) | rx chunks %ld pieces %ld dup %ld out-of-window "
              "%ld sha-fail %ld | verify ok %ld fail %ld",
              s.lane_depth, s.lane_cap, s.lane_high_water, s.lane_evicted, s.lane_refused,
              s.tx_base_chunk, s.tx_chunks_total, s.tx_pieces, s.tx_retx, s.rx_chunks, s.rx_pieces,
              s.rx_dup, s.rx_out_of_window, s.rx_sha_fail, s.verify_ok, s.verify_fail);
    out[cap - 1] = '\0';
    return true;
}

} // namespace bulk
} // namespace netudp
} // namespace mh
