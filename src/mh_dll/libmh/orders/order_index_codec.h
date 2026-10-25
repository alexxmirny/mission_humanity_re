//
// orders/order_index_codec.h -- the wire encoding of the replicated order record's unit_index (O1c).
//
// Vanilla stores every identity ushort of a SCHEDULED-lane record as its low byte DUPLICATED
// (0x012c -> 0x2c2c); the receiver masks `& 0xff` and llm_strat_order_integrity_check demands
// hi == lo. That caps the field at 255, which a 500-building roster outruns.
//
// The wide encoding widens ONLY unit_index, to 12 bits, and is a strict SUPERSET of vanilla:
//
//   encode(v):  lo = v & 0xff;  hi = lo ^ ((v >> 8) & 0x0f);   word = hi << 8 | lo
//   decode(w):  lo = w & 0xff;  v  = lo | (((w >> 8) ^ lo) & 0x0f) << 8           (0..4095)
//   check(w):   (((w >> 8) ^ w) & 0xf0) == 0                                      (field 0 only)
//
// For every v < 256 the word is exactly vanilla's duplicated byte, so STAGING, the wire, PENDING and
// every hash are byte-identical to retail whenever no index reaches 256. The other three fields keep
// the vanilla rule. The retail mh.dll carries the same rule as four in-place byte patches
// (seams/order_wide_index.cpp); keep the two in step.
//
#pragma once
#include <cstdint>

namespace mh::orders {

// Vanilla: the low byte written into both halves.
inline uint16_t dup_low_byte(uint16_t v) {
    const uint16_t b = static_cast<uint16_t>(v & 0xff);
    return static_cast<uint16_t>(b | (b << 8));
}

inline uint16_t encode_index(uint16_t v) {
    const uint16_t lo = static_cast<uint16_t>(v & 0xff);
    const uint16_t hi = static_cast<uint16_t>(lo ^ ((v >> 8) & 0x0f));
    return static_cast<uint16_t>((hi << 8) | lo);
}

inline uint16_t decode_index(uint16_t w) {
    const uint16_t lo = static_cast<uint16_t>(w & 0xff);
    return static_cast<uint16_t>(lo | ((((w >> 8) ^ lo) & 0x0f) << 8));
}

// Four of the eight check bits survive: the high nibble of hi must equal the high nibble of lo.
inline bool index_check_ok(uint16_t w) { return (((w >> 8) ^ w) & 0xf0) == 0; }

} // namespace mh::orders
