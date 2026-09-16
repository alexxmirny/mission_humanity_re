#pragma once
// packet_buffer.h -- the ONE owner of the shared outbound/inbound packet region (RI-WIRE W2).
//
// WHAT IT WRAPS
//   _G_LLM_NET_SEND_BUF        0x005d55cc, 0x3f8 bytes
//   _G_LLM_NET_SEND_BUF_CURSOR 0x005d59c4
//
// WHY AN OBJECT. Before W2 this one region was modelled three times in our own tree, each partially
// and independently: dispatch_state kept a bare `uint8_t *buf` plus its own RX_BUF_CAP, and
// container_state kept a bare `int32_t *send_cursor` plus its own SEND_BUF_HIGHWATER -- so the two
// halves of a single invariant lived in different structs, and the capacity was spelled 0x3f8 in
// two places that nothing forced to agree. Worse, `order_queue.cpp` READS the cursor to decide when
// to flush while the bytes are appended inside `gc.send_order()`, an ORIGINAL function we do not
// own. The invariant "cursor == bytes queued" is therefore maintained jointly by our code and the
// nineteen original emitters catalogued in the wire-emitter inventory, with no single owner. That is
// exactly the condition an owner type exists to fix.
//
// WHAT IT IS NOT. LAW 4 still binds: the layout is frozen while any original accessor remains, and
// after W2 most of the nineteen still do. This wraps the SAME memory with the SAME semantics -- two
// typed pointers and the named predicates over them. It is an owner, not a new representation.
// No struct of our own, no copy, no reordering, no extra state.
//
// THE THREE ROLES the region serves, none of which this type arbitrates between (see
// The wire-emitter inventory "The shared buffer and its three roles"):
//   (a) the order batch ACCUMULATES  -- append, then flush at the high-water mark
//   (b) a control message writes ONE record and resets the cursor to 0
//   (c) the receive path parses IN PLACE, with its own LOCAL read cursor
// Role (c)'s cursor is a parse position, deliberately NOT this object's `cursor` -- that one counts
// bytes queued for sending. Keeping them distinct is the point; they are never the same number.

#include <cstdint>

namespace mh::net {

struct packet_buffer {
    uint8_t *bytes;  // _G_LLM_NET_SEND_BUF        -- recv target, parse source AND send source
    int32_t *cursor; // _G_LLM_NET_SEND_BUF_CURSOR -- bytes queued in the outbound batch

    // The `MOV [EBP-0x38],0x3f8` the recv is capped with, and the same number the order batch's
    // high-water test compares against. One constant now, because it was always one buffer.
    static constexpr int32_t CAPACITY = 0x3f8;

    // 0x44 record + its one-byte tag. The batch flushes when one more would not fit:
    // `cursor + 0x45 >= 0x3f8` (llm_strat_order_schedule @0x004664ba).
    static constexpr int32_t ORDER_RECORD_BYTES = 0x45;

    int32_t queued() const { return *cursor; }
    bool    dirty() const { return *cursor != 0; }
    void    reset() { *cursor = 0; }

    // The original's comparison, kept in its original unsigned form rather than tidied: a negative
    // cursor would wrap to a huge value and force a flush, which is what the game does today.
    bool order_would_overflow() const {
        return static_cast<uint32_t>(*cursor + ORDER_RECORD_BYTES) >= static_cast<uint32_t>(CAPACITY);
    }

    // Read position helper for the parse path, which walks with its own local offset.
    const uint8_t *at(int32_t offset) const { return bytes + offset; }
    uint8_t       *at(int32_t offset) { return bytes + offset; }
};

} // namespace mh::net
