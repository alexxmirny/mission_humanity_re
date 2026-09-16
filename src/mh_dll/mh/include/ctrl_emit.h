#pragma once
// ctrl_emit.h -- the ONE emitter template the nineteen control-message builders are copies of
// (RI-WIRE W3). See the wire-emitter inventory for the inventory this generalises.
//
// WHAT THE ORIGINAL DOES, in the order it does it (llm_net_send_lockstep_extend @0x0049d33b and
// llm_net_send_lockstep_keepalive @0x0049d84c are the two worked examples; the other seventeen differ only
// in tag, payload and the guard):
//
//     assert_stack_capacity(N);                 // Watcom stack probe -- see the note below
//     if (cursor != 0) cursor = 0;              // SOME emitters only -- a parameter here, not a default
//     buf[cursor] = OUTER_TAG;  cursor += 1;
//     buf[cursor] = INNER_TAG;  cursor += 1;    // only when OUTER_TAG == MSG_CONTROL
//     memcpy(buf + cursor, payload, len);       // inlined REP MOVSD + a REP MOVSB remainder
//     cursor += len;
//     transport_send(buf, cursor);
//     cursor = 0;
//
// THE STACK PROBE IS DROPPED AS A STATED DECISION, not an omission. `assert_stack_capacity`
// (0x004cf46f) is Watcom's stack-touch helper: it walks guard pages so a large frame faults
// predictably. It has no semantic effect and nothing observable depends on it.
//
// THE ZERO-CURSOR GUARD IS A PARAMETER AND MUST STAY ONE. W1 established that it is present on
// exactly three emitters (extend + both chat senders) and that the split is "can this fire while an
// order batch is pending?", not message class. It is NOT a tidy-up candidate: with a non-empty
// cursor the guarded form DISCARDS the pending order batch, while the unguarded form appends after
// it and sends batch+record concatenated -- which the receiver parses correctly, because its
// dispatch loop consumes records until the length runs out. So the guarded form is the LOSSY one.
// Normalising either way is a silent change to what goes on the wire. Each caller passes what its
// own original does; there is deliberately no default.

#include <cstdint>
#include <cstring>

#include "include/packet_buffer.h"

namespace mh::net {

// What the transport is handed. A pointer rather than a direct call so the round-trip test can
// capture the bytes; production binds the generated thunk for llm_net_transport_send.
using transport_send_fn = void (*)(uint8_t *buf, int32_t len);

struct ctrl_record {
    uint8_t     outer       = 0;       // MSG_* -- always written
    bool        has_inner   = false;   // true exactly when outer == MSG_CONTROL
    uint8_t     inner       = 0;       // CTL_*
    const void *payload     = nullptr; // may be null for the header-only messages
    int32_t     payload_len = 0;       // 0 for CTL_PLAYER_LEFT, CTL_SESSION_ENDED, CTL_RESYNC_END
    bool        reset_first = false;   // the per-emitter guard above. No default that means "usual".
};

// Build one record into the shared buffer and hand it to the transport. Returns the byte count the
// transport was given -- which is NOT always the record's own size: an unguarded emitter firing on a
// dirty buffer legitimately sends the pending batch as well.
inline int32_t emit_ctrl(packet_buffer &pb, const ctrl_record &rec, transport_send_fn send) {
    if (rec.reset_first && pb.dirty()) pb.reset();

    pb.bytes[*pb.cursor] = rec.outer;
    *pb.cursor += 1;
    if (rec.has_inner) {
        pb.bytes[*pb.cursor] = rec.inner;
        *pb.cursor += 1;
    }
    if (rec.payload_len > 0) {
        std::memcpy(pb.bytes + *pb.cursor, rec.payload, static_cast<size_t>(rec.payload_len));
        *pb.cursor += rec.payload_len;
    }

    const int32_t len = *pb.cursor;
    if (send) send(pb.bytes, len);
    pb.reset();
    return len;
}

} // namespace mh::net
