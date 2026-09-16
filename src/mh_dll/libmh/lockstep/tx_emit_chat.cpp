//
// lockstep/tx_emit_chat.cpp -- the three functions declared in tx_emit_chat.h (RI-WIRE / W6-A).
//
// Translated from tmp/decomp_wire/llm_net_chat_send_team_0049d450.asm,
// tmp/decomp_wire/llm_net_chat_send_all_0049d50b.asm and
// tmp/decomp_wire/llm_net_send_buf_flush_0049d7ff.asm -- NOT from the decompile beside each; see
// tx_emit_chat.h's header comment for the specific spots where the decompile is misleading.
//
#include "lockstep/tx_emit_chat.h"

#include "addr/mh_addrs.gen.h"
#include "addr/mh_calls.gen.h"
#include "state/host_api.h"
#include "lockstep/tx_emit.h" // C8-d: promoted_wire::live -- the seam liveness counter

#include <cstring>
#include <vector>

namespace mh::lockstep {

const emit_chat_calls &live_chat_calls() {
    static const emit_chat_calls ec = {
        // llm_net_transport_send takes `void *`; emit_ctrl's transport_send_fn is spelled over
        // `uint8_t *` (see ctrl_emit.h) -- a one-line adapter, not a behaviour change. Same lambda
        // shape as tx_emit.cpp's live_emit_calls().
        [](uint8_t *buf, int32_t len) { mh::host().transport_send(buf, len); },
    };
    return ec;
}

namespace detail {

// ---- shared helper: rows 3 & 4 differ by exactly one parameter -------------------------------------
void chat_send(const emit_chat_state &es, const emit_chat_calls &calls, const void *text, uint32_t len,
               uint8_t recipient) {
    // Runtime-sized blob: `len` byte + recipient byte + `len` bytes of text. MSG_CHAT is the ONLY
    // variable-length record among all nineteen emitters (the wire-emitter inventory) -- every sibling
    // emitter in tx_emit.cpp has a compile-time payload size and uses a fixed `uint8_t payload[N]`.
    // A fixed cap here would silently truncate chat text where the original has NO length check at
    // all (a large enough `len` overruns `_G_LLM_NET_SEND_BUF` in the original, and that overrun is
    // reproduced faithfully by feeding the SAME uncapped `len` into emit_ctrl's own uncapped memcpy
    // below -- capping locally would instead silently swallow the overflow, which is a real, different
    // observable difference for a caller that (mis)uses a length near or past the buffer's 0x3f8-byte
    // capacity). A raw stack VLA/alloca would avoid the cap but risk smashing THIS function's own
    // stack for a large `len`, a failure mode the original never has (its overrun lands in the SHARED
    // game buffer's neighbouring globals, not on its own stack frame). std::vector is the one option
    // that introduces neither a new cap nor a new stack-overflow mode -- it is the first STL container
    // anywhere in mh_dll/mh's game-logic modules (checked 2026-07-29: no `std::vector`/heap `new` in
    // any other `.cpp` under `src/mh_dll/mh/`), a deliberate, scoped exception for the one
    // variable-length record in this whole effort, not a precedent for the rest of the module.
    std::vector<uint8_t> payload(static_cast<size_t>(len) + 2);

    // 0x0049d498/0x0049d553 (team) and the byte-identical site in _all: `MOV AL, byte ptr [len]` reads
    // only the LOW BYTE of the 32-bit `len` argument -- a truncating cast, NOT a saturating clamp. For
    // len > 255 this byte UNDERSTATES the text that follows; that is the original's own behaviour and
    // is reproduced verbatim (see tx_emit_chat.h "THE LENGTH BYTE'S DOUBLE DUTY").
    payload[0] = static_cast<uint8_t>(len);
    payload[1] = recipient;

    // 0x0049d4be..d4dc (team) / 0x0049d574..d592 (all): REP MOVSD over `len >> 2` dwords then REP
    // MOVSB over `len & 3` bytes -- together exactly `len` contiguous bytes, i.e. one
    // `memcpy(dst, src, len)`. The `if (len)` guard is defensive against a null `text` with len == 0
    // (the original's REP *S with ECX == 0 is an unconditional no-op either way, so this changes
    // nothing observable for any input the original could receive).
    if (len) std::memcpy(payload.data() + 2, text, len);

    mh::net::packet_buffer pb = es.packet; // aliases the SAME memory as es.packet -- packet_buffer
                                           // holds only the two pointers, not the bytes; see
                                           // tx_emit.cpp's identical note above send_lockstep_extend.
    mh::net::ctrl_record rec{};
    rec.outer     = static_cast<uint8_t>(MSG_CHAT); // 5 -- 0x0049d485 / 0x0049d540
    rec.has_inner = false;                          // the length byte is PAYLOAD, not an inner (MSG_CONTROL) tag -- see
                                                    // tx_emit_chat.h's "MSG_CHAT'S SHAPE" note.
    rec.payload     = payload.data();
    rec.payload_len = static_cast<int32_t>(payload.size()); // 2 + len, i.e. the FULL len, matching the
                                                            // original's `cursor += len` at full width
                                                            // (0x0049d4df.. / 0x0049d595..) -- NOT the
                                                            // truncated length-prefix byte.
    rec.reset_first = true;                                 // GUARDED -- the wire-emitter inventory rows 3 & 4 (0x0049d46d..d480 / the
                                                            // byte-identical guard in _all at 0x0049d528..d53b).
    mh::net::emit_ctrl(pb, rec, calls.transport_send);
}

// ---- 1. llm_net_chat_send_team @0x0049d450 (row 3) -------------------------------------------------
void chat_send_team(const emit_chat_state &es, const emit_chat_calls &calls, const void *text,
                    uint32_t len) {
    chat_send(es, calls, text, len, *es.chat_target_mask); // 0x0049d4ac: MOV DL, byte ptr [0x00e5898b]
}

// ---- 2. llm_net_chat_send_all @0x0049d50b (row 4) --------------------------------------------------
void chat_send_all(const emit_chat_state &es, const emit_chat_calls &calls, const void *text,
                   uint32_t len) {
    chat_send(es, calls, text, len, CHAT_RECIPIENT_ALL); // 0x0049d567: MOV byte ptr [...],0xff
}

// ---- 3. llm_net_send_buf_flush @0x0049d7ff (row 19) ------------------------------------------------
//
// NOT an emitter -- no tag, no payload, no emit_ctrl call. The original (0x0049d817..0x0049d83f):
// read cursor into EDX, read the buffer base into EAX, CALL transport_send(EAX=buf, EDX=len); then
// re-read cursor into a local, zero the real cursor, and return the local. transport_send does not
// write the cursor (it is a stub in retail, and our DLL's replacement is a socket call, not a buffer
// operation), so the two reads of `queued()` here are the same value -- the second read is kept
// anyway to mirror the original's own instruction order exactly rather than reusing the first result.
int32_t send_buf_flush(const emit_chat_state &es, const emit_chat_calls &calls) {
    mh::net::packet_buffer pb = es.packet;

    const int32_t len = pb.queued();                               // 0x0049d817: MOV EDX, [cursor] -- read BEFORE the call
    if (calls.transport_send) calls.transport_send(pb.bytes, len); // 0x0049d81d/0x0049d822

    const int32_t old = pb.queued(); // 0x0049d827: re-read [cursor] -- unconditionally, even if the
                                     // buffer was empty (queued() == 0); there is no dirty-check guard
                                     // here the way the chat pair's `reset_first` has one.
    pb.reset();                      // 0x0049d82f: MOV [cursor], 0
    return old;                      // 0x0049d83f/0x0049d842 (the value threads through two more local
                                     // stack slots in the original before EAX is loaded for RET -- an
                                     // artifact of Watcom's epilogue shuffling, not a second value).
}

} // namespace detail

// ---- production entry points (bound to the live game state) ----------------------------------------
void chat_send_team(const void *text, uint32_t len) {
    // C8-d: the liveness counter is at the PRODUCTION ENTRY, not the entry thunk -- see the long
    // note at turn_engine.cpp's production entry points. A seam reached only from inside the
    // closure never touches its entry once the internal edges are direct.
    promoted_wire::live(5, "wire/chat_send_team");
    detail::chat_send_team(chat_estate(), live_chat_calls(), text, len);
}

void chat_send_all(const void *text, uint32_t len) {
    promoted_wire::live(6, "wire/chat_send_all");
    detail::chat_send_all(chat_estate(), live_chat_calls(), text, len);
}

int32_t send_buf_flush() {
    promoted_wire::live(7, "wire/send_buf_flush");
    return detail::send_buf_flush(chat_estate(), live_chat_calls());
}

} // namespace mh::lockstep
