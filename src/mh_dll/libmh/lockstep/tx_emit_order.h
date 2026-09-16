#pragma once

#include <cstdint>

#include "addr/mh_structs.gen.h"

namespace mh::lockstep {

using game_order_tx = mh::game::mh_llm_strat_order; // 0x44, passed BY VALUE on the wire

// The tag byte that precedes an order record in the outbound batch. `MOV byte ptr [EDI+0x5d55cc],1`.
inline constexpr uint8_t ORDER_RECORD_TAG = 0x01;

// sizeof(order) + the tag. llm_strat_order_schedule's own overflow test is written as `cursor + 0x45
// >= 0x3f8`, i.e. it knows this constant too -- if one ever changes, both must.
inline constexpr int32_t ORDER_RECORD_BYTES = 0x45;

struct order_tx_state {
    uint8_t *send_buf; // _G_LLM_NET_SEND_BUF        (0x005d55cc), 1016 bytes
    int32_t *cursor;   // _G_LLM_NET_SEND_BUF_CURSOR (0x005d59c4)
};

struct order_tx_calls {
    // The queue consistency check. Its second argument is a caller tag string; the original passes
    // the .rdata literal "NetgameSendOrderAdd" at 0x005017e0, reproduced verbatim so a log line or
    // an assert raised inside the callee still names the same site it always did.
    void (*integrity_check)(const game_order_tx *order, const char *tag);
};

order_tx_state        live_order_tx_state();
const order_tx_calls &live_order_tx_calls();
const order_tx_calls &inert_order_tx_calls();

namespace detail {

// Returns 0 always -- see the header note. The signature keeps the int so it can be bound to the
// generated export thunk without a cast that would hide a real mismatch.
int32_t send_order(const order_tx_state &st, const order_tx_calls &calls, const game_order_tx *order);

} // namespace detail

// Production entry point: detail::send_order over the live buffer and the real integrity check.
// `const` because the ORIGINAL is read-only through this record, evidenced rather than assumed
// (LIB-CONSTSIG, 2026-09-04): the 68-byte order is passed BY VALUE and the pointer exists only
// at the marshalling boundary, where the thunk uses it solely as the REP MOVSD SOURCE. It is
// what lets the rebind bind this row -- a calls-struct member's type is the CALL direction, so
// a non-const wrapper cannot be its target and the row was parked keep_original.
//
// SPELLED WITH THE COMMITTED TYPE NAME, not the `game_order_tx` alias, and that is load-bearing
// rather than cosmetic (LIB-REF, 2026-09-11). gen_libmh_rebind's R1 walk compares the public
// sibling's parameter types to the committed callee shape TEXTUALLY (norm_type strips `::` and
// nothing else), so `const game_order_tx *` did not match `const mh::game::mh_llm_strat_order *`
// even though they are the same type. The walk therefore took its documented "no match -> route
// through the arm, which is safe" fallback -- and that claim is REFUTED here: the arm it chose,
// `promoted_order_tx::send_order`, is DEFINED non-const (it has to be: MH_EXPORT_REPLACE pins it to
// mh_export.gen.h's `sig_llm_net_send_order`, which is one of the four rows that generated header
// spells non-const while mh_calls.gen.h spells it const). rebind_targets.gen.h then DECLARED a
// const overload that nothing defines. Hosted never noticed -- MH_PROMOTED is `mh::call::<fn>`
// there -- and the standalone build takes its address, so libmh.lib held a dangling symbol that
// only a LINK could see. Keep this spelling; the alias stays for every other use in this unit.
int32_t send_order(const mh::game::mh_llm_strat_order *order);

// The seam. Joins `[promote] lockstep` -- NOT `[promote] wire`, even though this is an emitter,
// because it is part of C8's residue closure and is the one thing llm_strat_order_schedule still
// calls into the original for. turn_engine.cpp's single table drives this wrapper; the macro that
// defines the underlying installer is `static` in tx_emit_order.cpp and cannot move.
bool install_seam_send_order();


long order_tx_promotion_calls();

} // namespace mh::lockstep
