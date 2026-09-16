//
// orders/order_codec.h -- ONE encoding of the order record (RI-STATE / ST5).
//
// THE PROBLEM. The 0x44-byte order record has two external encodings, and until this file both of
// them were the phrase "the raw bytes":
//
//   the WIRE  -- llm_net_send_order memcpy'd the struct into the packet buffer, and rx_dispatch
//                memcpy'd it back out.
//   the SAVE  -- mh::orders::emit_region handed the save driver `queue` as one 20400-byte block.
//
// That is not wrong today, because the in-memory representation IS the wire format. It becomes two
// places to update -- one of which will be forgotten -- the moment ST4 makes a representation change
// possible at all. And the forgetting is silent in the worst way: the save keeps round-tripping
// against itself while the wire disagrees with the peer, or vice versa, and each side's own test
// still passes.
//
// SO: ONE encode/decode pair, and both paths call it. A change to the layout below updates both or
// neither, which is the entire content of this item. The mutation test that proves it is in
// `orders_selftest.cpp` -- changing this file has to turn the WIRE round-trip and the SAVE
// round-trip red TOGETHER; either one staying green would mean it kept a private copy.
//
// EXPLICIT LITTLE-ENDIAN, FIELD BY FIELD, and not a struct memcpy -- even though on this target the
// two produce identical bytes and there is a static_assert below that says so. The point is that the
// wire format stops being "whatever the compiler laid out" and becomes a written-down thing that a
// representation change must consciously edit. A memcpy would leave the coupling exactly where it
// is: implicit, and satisfied by accident.
//
// THE BYTES ARE UNCHANGED, and that is checked two ways rather than asserted: `encode` must equal
// the struct's raw image for a randomized record (the strong form -- it holds for every value, not
// one), and against a recorded 68-byte golden captured before this change (the anchor -- it survives
// a future representation change, when the raw image stops being the reference).
//
#pragma once
#include <cstdint>
#include <cstring>

#include "addr/mh_structs.gen.h"

namespace mh::orders {

using order = mh::game::mh_llm_strat_order;

namespace codec {

// The record's size ON THE WIRE and IN THE FILE. Deliberately its own constant rather than
// sizeof(order): the day the in-memory representation changes, this number must NOT follow it.
inline constexpr uint32_t RECORD_BYTES = 68;

static_assert(sizeof(order) == RECORD_BYTES,
              "the in-memory order record no longer matches the wire/file record. That is allowed "
              "-- it is what ST4/ST5 exist to make possible -- but then encode/decode below must "
              "stop being a field-for-field mirror, and the raw-image test in orderstest must go.");

namespace detail {

inline void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}
inline void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}
inline uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

} // namespace detail

// ---- the layout ------------------------------------------------------------------------------
//
//   +0x00  double   exec_time        (IEEE-754 binary64, little-endian, BIT-COPIED)
//   +0x08  uint16   unit_index
//   +0x0a  uint16   owner_and_kind
//   +0x0c  int16    param0
//   +0x0e  uint16   order_code
//   +0x10  int32    args[13]
//   = 0x44
//
// exec_time is bit-copied rather than converted. It is a game-clock value that is COMPARED for
// equality across peers (llm_net_lockstep uses the sender's exec_time as its horizon, and
// x87_equal_or_unordered decides on the exact bits), so any transformation that is not the identity
// on NaN and on -0.0 would be a determinism bug that only shows up in a real match.
inline void encode(const order &o, uint8_t *out) {
    uint64_t bits;
    std::memcpy(&bits, &o.exec_time, sizeof(bits));
    detail::put32(out + 0, (uint32_t)(bits & 0xffffffffu));
    detail::put32(out + 4, (uint32_t)(bits >> 32));
    detail::put16(out + 8, o.unit_index);
    detail::put16(out + 10, o.owner_and_kind);
    detail::put16(out + 12, (uint16_t)o.param0);
    detail::put16(out + 14, o.order_code);
    for (int i = 0; i < 13; ++i) detail::put32(out + 16 + i * 4, (uint32_t)o.args[i]);
}

inline void decode(const uint8_t *in, order &o) {
    const uint64_t bits = (uint64_t)detail::get32(in + 0) | ((uint64_t)detail::get32(in + 4) << 32);
    std::memcpy(&o.exec_time, &bits, sizeof(bits));
    o.unit_index     = detail::get16(in + 8);
    o.owner_and_kind = detail::get16(in + 10);
    o.param0         = (int16_t)detail::get16(in + 12);
    o.order_code     = detail::get16(in + 14);
    for (int i = 0; i < 13; ++i) o.args[i] = (int32_t)detail::get32(in + 16 + i * 4);
}

} // namespace codec
} // namespace mh::orders
