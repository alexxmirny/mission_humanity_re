//
// mh_common/include/mh_net_queue_policy.h -- WHICH inbound frame a full transport queue may destroy (MP D24).
//
// THE DEFECT THIS EXISTS TO REMOVE. The transport's inbound ring is fixed-size and, when full, used
// to evict the OLDEST frame -- "favour fresh lockstep data". That is right for a horizon
// advertisement, which the next one supersedes, and catastrophic for an ORDER, which is a game
// input that must be delivered exactly once. It was also silent.
//
// MEASURED, not supposed (D24, run 18 of the 2026-09-02 campaign): client1's main thread froze for
// 1844 ms; the other two peers reached the lockstep horizon, waited, and kept re-advertising at
// frame rate (client2 alone sent ~3100 frames in ~2 s); client1's recv thread enqueued all of it
// into 256 slots that nothing was draining. The eviction destroyed the head of that backlog --
// ~2700 frames, of which 15 carried orders (exec_time 14.57..14.60, from BOTH senders, none of
// client1's own, since local orders never enter this queue). Those 15 are exactly the orders
// client1's order_queue was missing for the 37 steps the run was red.
//
// THE POLICY. Evict only a frame we can PROVE carries nothing but a superseded horizon. Everything
// else -- orders, control transitions, keepalives, chat, and any datagram whose length says it
// holds more than one message -- is protected, and if the ring holds nothing droppable the caller
// must say so loudly rather than quietly picking something.
//
// WHY THE LENGTH TEST AND NOT THE TAG ALONE. The retail dispatcher walks a datagram with a cursor
// (`while (cursor < len)`, rx_dispatch.cpp), so one frame may carry several messages back to back.
// A frame whose first byte is MSG_HORIZON could still be followed by an order. Requiring the length
// to be EXACTLY one bare horizon means the proof does not depend on any assumption about how the
// sender packs its datagrams -- if a sender ever starts coalescing, these frames stop matching and
// become protected, which is the safe direction to be wrong in.
//
// AND IT IS NOT A PREDICATE THAT NEVER FIRES, which is the other way this could have gone wrong (a
// knob wired to nothing is its own trap). `send_lockstep_extend` is one of the three
// emitters that set `reset_first`, so it flushes any pending batch and writes its record into an
// EMPTY buffer: tag at cursor 0, eight payload bytes, len 9, every time (tx_emit.cpp
// send_lockstep_extend -> ctrl_emit.h emit_ctrl). It is also the emitter that floods -- a peer
// waiting at the lockstep horizon re-advertises at frame rate -- so the frames this predicate
// matches are exactly the ones filling the queue.
//
// A 9-byte frame beginning with MSG_HORIZON cannot be anything else: the horizon arm always consumes
// its 8 payload bytes, so there is no room for a second record inside the same length.
//
#pragma once
#include <cstdint>

namespace mh::net::queue_policy {

// The retail outer tags (turn_engine.h `outer_tag`). Only the two named here matter to this file.
inline constexpr uint8_t MSG_ORDER   = 1; // 0x44-byte order -- NEVER evictable
inline constexpr uint8_t MSG_HORIZON = 2; // bare 8-byte horizon advertisement

// tag byte + the 8-byte double the MSG_HORIZON arm copies out (rx_dispatch.cpp: cursor += 8).
inline constexpr int BARE_HORIZON_LEN = 1 + 8;

// Is this frame provably nothing but one superseded horizon advertisement?
inline bool is_evictable(const uint8_t *data, int len) {
    return data != nullptr && len == BARE_HORIZON_LEN && data[0] == MSG_HORIZON;
}

// ---- THE LANE CLASSES (mp:T2) --------------------------------------------------------------------
//
// `is_evictable` above answers one question about one frame. A LANE is the same answer decided ONCE,
// at enqueue, and carried -- which is what mp:U41 wants for the horizon flood (evict lane H's head,
// O(1), instead of walking the ring for a victim) and what mp:T2 needs for the opposite reason: a
// completed bulk chunk must be in a class no eviction path can reach.
//
// T2 adds the VOCABULARY and the bulk lane only. U41 still owns replacing the scan in the game
// queue's enqueue path with the sequence-merged lane pair; that item's premise -- delivery order
// proven byte-identical -- is a claim about the ORDER lanes, and nothing here changes it.
//
// Why `bulk` is a class and not simply "another must_keep": because the two differ in what OVERFLOW
// means. A full must_keep queue is a correctness EVENT (D24's loud refusal); a full bulk lane is
// ordinary BACKPRESSURE -- the chunk is not acknowledged, the sender holds its window, and nothing
// is lost. A single class would have to pick one of those behaviours for both.
enum class lane : uint8_t {
    bulk,         // a completed, verified bulk chunk (mp:T2). NEVER a victim; overflow = refuse.
    must_keep,    // orders, control transitions, anything not provably superseded. NEVER a victim.
    supersedable, // one bare horizon advertisement, which the next one replaces.
};

// The game queue's classification, decided at enqueue from the same predicate the scan used.
inline lane lane_of_game_frame(const uint8_t *data, int len) {
    return is_evictable(data, len) ? lane::supersedable : lane::must_keep;
}

// A bulk chunk's, which is not a function of its bytes: it is a chunk because of the channel it
// arrived on. Stated as a function anyway so the call site reads the same as the one above.
inline constexpr lane lane_of_bulk_chunk() {
    return lane::bulk;
}

// The whole eviction rule, in one place: exactly one lane may lose a frame. `MSG_ORDER` lands in
// must_keep and a chunk in bulk, so "orders are protected" and "chunks are never victims" are the
// same statement rather than two policies that could drift apart.
inline constexpr bool lane_evictable(lane l) {
    return l == lane::supersedable;
}

// A frame as the ring holds it.
struct frame_view {
    const uint8_t *data;
    int            len;
};

// ---- THE OLD SINGLE-FIFO VICTIM SCAN (mp:D24) ----------------------------------------------------
//
// NO LONGER ON THE ENQUEUE PATH (mp:U41, 2026-09-18). It is kept, deliberately, as the REFERENCE
// MODEL the lane pair below is proved against: `queuetest` runs a recorded arrival sequence through
// this one-FIFO-plus-scan model and through `lane_queue`, and asserts the drained sequences agree.
// A reference model that is only described in a comment is a model nobody can run, so this one stays
// compiled and stays tested -- which is also why its eight checks survive U41 intact.
//
// Pick the slot to destroy: the OLDEST evictable frame in the ring, or -1 when the ring holds none.
// `get(slot)` returns the frame in absolute ring slot `slot`; iteration runs head-first so "oldest"
// is the first match. Returning -1 is a real answer and the caller must handle it -- a queue with
// nothing safe to drop is a queue that must not silently drop anything.
template <class GetFrame>
inline int choose_victim(int head, int count, int cap, GetFrame get) {
    if (cap <= 0 || count <= 0) return -1;
    for (int i = 0; i < count; ++i) {
        const int        slot = (head + i) % cap;
        const frame_view f    = get(slot);
        if (is_evictable(f.data, f.len)) return slot;
    }
    return -1;
}

// ================================ THE SEQUENCE-MERGED LANE PAIR (mp:U41) ===========================
//
// WHAT WAS WRONG WITH THE SCAN, and it was never performance: `choose_victim` SEARCHES the buffer for
// something it is allowed to throw away. You should not have to look for that -- you should know it by
// construction. So the classification the scan re-derives per victim is made ONCE, at enqueue, and
// carried as the lane a frame joined (`lane_of_game_frame`, the same predicate).
//
// THE TWO LANES, and the whole of the policy:
//   H (supersedable) -- one bare horizon advertisement, nothing else. FULL -> evict its own head,
//                       O(1), no scan: the frame behind it supersedes it by definition.
//   M (must_keep)    -- orders, control transitions, keepalives, chat, and any datagram whose length
//                       says it carries more than one message. FULL -> REFUSE the arrival and say so
//                       loudly. This is the `-1` arm of the scan, unchanged in meaning.
//
// AND THE REASON THIS SHAPE WAS CHOSEN OVER COALESCING HORIZONS (which is strictly cheaper): DELIVERY
// ORDER MUST NOT CHANGE. `MSG_ORDER` carries the sender's horizon in its `exec_time`, and
// `LS_HORIZON_PENDING` is read at DISPATCH time, not at receive time -- so it is the receiver's flag
// WHEN IT DRAINS that decides whether a horizon lands in `peer_horizon[]` or `peer_horizon_pending[]`.
// Any scheme that changes WHEN a horizon is applied can apply a stale horizon after a newer one
// (moving a peer's barrier backward through `committed = min(peers)`) or land it in the wrong bucket
// during the resync handshake -- the exact ground mp D14, D17 and D24 were fought on. So every frame
// takes a monotonic ARRIVAL SEQUENCE number, and the drain always takes the lower of the two lane
// heads. The merge cannot reorder, so none of that can bite; the only frames missing from the
// delivered stream are the ones lane H explicitly evicted.
//
// WHY IT IS INDICES AND NOT FRAMES. This structure owns sequence numbers and ring arithmetic and
// nothing else: `push` returns the POSITION in a lane the caller should write its frame at, `pop`
// returns the position it should read. The caller keeps the storage -- which is what lets the two
// lanes have DIFFERENT slot sizes, and that is where the memory question answers itself rather than
// being answered by a constant. A lane-H frame is exactly BARE_HORIZON_LEN bytes BY CONSTRUCTION (the
// classifier admits nothing else), so lane H does not need the full-datagram slot the old single ring
// gave every frame. See net_transport.cpp for the sizes that fall out.

// What `push` decided. `accepted == false` is lane M refusing an arrival: a real input had nowhere to
// go, which is a correctness event, not bookkeeping.
struct push_result {
    bool accepted    = false;
    lane which       = lane::must_keep; // the lane joined (valid iff accepted)
    int  pos         = -1;              // position in THAT lane to store the frame at
    bool evicted     = false;           // lane H destroyed its head to make room
    int  evicted_pos = -1;              // which position that was (valid iff evicted)
};

// What `pop` decided: which lane's head is next in ARRIVAL order.
struct pop_result {
    bool ok    = false;
    lane which = lane::must_keep;
    int  pos   = -1;
};

// Arrival-order comparison done as a SIGNED difference rather than `a < b`, so a wrapped sequence
// counter still orders correctly. At the measured flood (~1570 frames/s) a uint32 wraps in about a
// month of continuous play, which no match reaches -- but a comparison that is only correct because
// of an assumption about session length is a comparison waiting to be wrong.
inline bool seq_before(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) < 0;
}

template <int CAP_H, int CAP_M>
class lane_queue {
public:
    static constexpr int cap_h = CAP_H;
    static constexpr int cap_m = CAP_M;

    void reset() {
        head_h_ = count_h_ = head_m_ = count_m_ = 0;
        next_seq_                               = 0;
        high_water_ = high_h_ = high_m_ = 0;
        evicted_ = refused_ = 0;
    }

    int  depth() const { return count_h_ + count_m_; }
    int  depth_h() const { return count_h_; }
    int  depth_m() const { return count_m_; }
    int  high_water() const { return high_water_; }
    int  high_water_h() const { return high_h_; }
    int  high_water_m() const { return high_m_; }
    long evicted() const { return evicted_; }
    long refused() const { return refused_; }

    push_result push(lane l) {
        push_result r;
        // `bulk` is T2's class and never reaches the game queue; if one ever did, treating it as
        // must_keep is the safe direction -- `lane_evictable` says only H may lose a frame.
        if (l == lane::supersedable) {
            if (count_h_ >= CAP_H) {
                r.evicted     = true;
                r.evicted_pos = head_h_;
                head_h_       = next_pos(head_h_, CAP_H);
                --count_h_;
                ++evicted_;
            }
            r.pos         = (head_h_ + count_h_) % CAP_H;
            seq_h_[r.pos] = next_seq_++;
            ++count_h_;
            r.which    = lane::supersedable;
            r.accepted = true;
        } else {
            if (count_m_ >= CAP_M) {
                ++refused_;
                return r; // accepted stays false -- the caller must report this, not swallow it
            }
            r.pos         = (head_m_ + count_m_) % CAP_M;
            seq_m_[r.pos] = next_seq_++;
            ++count_m_;
            r.which    = lane::must_keep;
            r.accepted = true;
        }
        note_depth();
        return r;
    }

    pop_result pop() {
        pop_result r;
        const bool have_h = count_h_ > 0, have_m = count_m_ > 0;
        if (!have_h && !have_m) return r;
        // THE MERGE. Lower arrival sequence first, always -- this single line is the whole
        // "delivery order is unchanged" claim.
        const bool take_h = have_h && (!have_m || seq_before(seq_h_[head_h_], seq_m_[head_m_]));
        if (take_h) {
            r.pos   = head_h_;
            head_h_ = next_pos(head_h_, CAP_H);
            --count_h_;
            r.which = lane::supersedable;
        } else {
            r.pos   = head_m_;
            head_m_ = next_pos(head_m_, CAP_M);
            --count_m_;
            r.which = lane::must_keep;
        }
        r.ok = true;
        return r;
    }

private:
    static int next_pos(int p, int cap) { return (p + 1) % cap; }
    void       note_depth() {
        const int d = depth();
        if (d > high_water_) high_water_ = d;
        if (count_h_ > high_h_) high_h_ = count_h_;
        if (count_m_ > high_m_) high_m_ = count_m_;
    }

    uint32_t seq_h_[CAP_H] = {};
    uint32_t seq_m_[CAP_M] = {};
    int      head_h_ = 0, count_h_ = 0;
    int      head_m_ = 0, count_m_ = 0;
    uint32_t next_seq_   = 0;
    int      high_water_ = 0, high_h_ = 0, high_m_ = 0;
    long     evicted_ = 0, refused_ = 0;
};

} // namespace mh::net::queue_policy
