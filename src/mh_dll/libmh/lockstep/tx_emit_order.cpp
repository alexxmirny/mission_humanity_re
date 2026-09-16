//
// lockstep/tx_emit_order.cpp -- llm_net_send_order (RI-CUTOVER / C8-c).
// Reviewed by the reimpl-verify adversarial pass (zero divergences); see the header.
//
// Translated from tmp/c8c/llm_net_send_order_0049d3c8.asm, NOT from the .c beside it: the decompile
// invents an `int` return the function does not have (see the header).
//
#include "lockstep/tx_emit_order.h"

#include <cstring>

#include "addr/mh_addrs.gen.h"
#include "addr/mh_calls.gen.h"
#include "addr/mh_rebind.gen.h"
#include "state/rebind_targets.gen.h"
#include "addr/mh_export.gen.h"   // MH_EXPORT_REPLACE -- the promotion seam (C8-c wiring, at the tail)
#include "lockstep/turn_engine.h" // say() -- one logger for the whole closure
#include "orders/order_codec.h"   // ST5: the record layout the SAVE path also uses

namespace mh::lockstep {

namespace {

// The .rdata caller tag the original passes, at 0x005017e0. Reproduced as a literal rather than
// read through a pointer because it is an argument, not state -- nothing writes it, and pointing at
// our own copy keeps the module free of a global it would otherwise need only for this.
const char ORDER_TAG[] = "NetgameSendOrderAdd";

void live_integrity_check(const game_order_tx *order, const char *tag) {
    // The generated wrapper takes `char *` (Ghidra typed the param non-const); the callee only reads
    // it. const_cast rather than widening the module's own signature, which should stay honest.
    MH_LIBMH_BIND(llm_strat_order_integrity_check)(order, const_cast<char *>(tag));
}

// INERT: force-returns to the main menu on an inconsistent queue. Writes no state and returns void,
// so nothing downstream of it is part of any verdict -- skipping it narrows nothing.
void inert_integrity_check(const game_order_tx *, const char *) {}

} // namespace

const order_tx_calls &live_order_tx_calls() {
    static const order_tx_calls cc = {live_integrity_check};
    return cc;
}

const order_tx_calls &inert_order_tx_calls() {
    static const order_tx_calls cc = {inert_integrity_check};
    return cc;
}

namespace detail {

// ---- llm_net_send_order @0x0049d3c8 --------------------------------------------------------------
//
// NO OVERFLOW CHECK, and that is faithful. The original appends unconditionally; the 1016-byte
// buffer is kept from overflowing by the CALLER (llm_strat_order_schedule flushes when
// `cursor + 0x45 >= 0x3f8`). Adding a bounds test here would be a behaviour change that could only
// show up as orders silently going missing, so it is not added -- but it IS worth knowing that this
// function will happily write past the buffer if ever called from somewhere that does not flush.
// There is exactly one caller today.
int32_t send_order(const order_tx_state &st, const order_tx_calls &calls, const game_order_tx *order) {
    st.send_buf[*st.cursor] = ORDER_RECORD_TAG; // 0x0049d3e6
    *st.cursor += 1;                            // 0x0049d3ed -- BEFORE the copy, see the header
    // ST5: the ONE codec, shared with the save path (orders/order_codec.h). The original's
    // instruction here is a `REP MOVSD` of the struct and the bytes are identical -- what changes is
    // that the layout is now written down in one place instead of implied by two memcpys.
    mh::orders::codec::encode(*order, st.send_buf + *st.cursor);         // 0x0049d3f3..0x0049d413
    *st.cursor += static_cast<int32_t>(mh::orders::codec::RECORD_BYTES); // 0x0049d416

    calls.integrity_check(order, ORDER_TAG); // 0x0049d41d..0x0049d43f
    return 0;                                // the original's EAX here is the callee's leftover
}

} // namespace detail
} // namespace mh::lockstep


namespace mh::lockstep {

// Spelled with the committed type name to match the declaration -- see tx_emit_order.h for why the
// alias may not be used here.
int32_t send_order(const mh::game::mh_llm_strat_order *order) {
    return detail::send_order(live_order_tx_state(), live_order_tx_calls(), order);
}

} // namespace mh::lockstep


// ---- promotion -----------------------------------------------------------------------------------
namespace mh::lockstep::promoted_order_tx {

long g_calls;

int32_t send_order(mh::lockstep::game_order_tx *order) {
    const long n = ++g_calls;
    if (n == 1 || n == 100 || n == 1000 || n == 10000 || n == 100000)
        mh::lockstep::say("; [promote] order_tx/send_order: call #%ld (OURS is live)\n", n);
    return mh::lockstep::send_order(order);
}

} // namespace mh::lockstep::promoted_order_tx

MH_EXPORT_REPLACE(llm_net_send_order, mh::lockstep::promoted_order_tx::send_order)

namespace mh::lockstep {

bool install_seam_send_order() { return mh_export_install_llm_net_send_order(); }


long order_tx_promotion_calls() { return promoted_order_tx::g_calls; }

} // namespace mh::lockstep
