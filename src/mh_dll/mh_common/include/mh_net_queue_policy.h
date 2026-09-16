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

// A frame as the ring holds it.
struct frame_view {
    const uint8_t *data;
    int            len;
};

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

} // namespace mh::net::queue_policy
