// mh_net_proto -- GAMEPLAY SET: the host-dictated gameplay settings that ride the FLAG_START frame
// as a trailer after the 8x0x39 slot bytes (PT-SET1). Portable, no platform dependencies.
//
// Wire (all little-endian):  'G' 'P' fmt=1 n, then n x { u32 key_id, u8 type, value }
//   type 1 INT : i32 (4 bytes; bools ride here too)
//   type 2 F64 : the raw IEEE-754 bit pattern (8 bytes) -- no text round trip, -0.0 and NaN payloads survive
//   type 3 STR : RESERVED; decode rejects it
// n <= GAMEPLAY_MAX_ENTRIES. key_id is the registry hash of "section.key" (gameplay_key_id).
#pragma once
#include <cstddef>
#include <cstdint>

namespace mh_net_proto {

constexpr int          GAMEPLAY_MAX_ENTRIES = 64; // was 32 until 0.2.1-rc2: 34 synced rows silently lost the last two
constexpr std::uint8_t GAMEPLAY_FORMAT      = 1;
constexpr std::uint8_t GAMEPLAY_T_INT       = 1;
constexpr std::uint8_t GAMEPLAY_T_F64       = 2;
constexpr std::uint8_t GAMEPLAY_T_STR       = 3; // reserved: never encoded, rejected on decode
constexpr std::size_t  GAMEPLAY_HDR_BYTES   = 4;
constexpr std::size_t  GAMEPLAY_MAX_ENCODED = GAMEPLAY_HDR_BYTES + GAMEPLAY_MAX_ENTRIES * (4 + 1 + 8); // 836

struct GameplayEntry {
    std::uint32_t key_id = 0;
    std::uint8_t  type   = 0; // GAMEPLAY_T_INT / GAMEPLAY_T_F64
    std::int32_t  i      = 0; // valid for T_INT
    std::uint64_t f_bits = 0; // valid for T_F64: raw IEEE-754 bits
};

struct GameplaySet {
    int           n = 0;
    GameplayEntry e[GAMEPLAY_MAX_ENTRIES];
};

enum class GameplayErr : std::uint8_t {
    Ok = 0,
    Truncated,   // fewer bytes than the header / entries need
    BadMagic,    // not 'G' 'P'
    BadFormat,   // fmt != GAMEPLAY_FORMAT
    UnknownType, // type not INT/F64 (STR included)
    TooMany,     // n > GAMEPLAY_MAX_ENTRIES
};

const char *gameplay_err_text(GameplayErr e) noexcept; // never null

// Case-folded FNV-1a over "section.key" (a 0 hash maps to 1). Byte-identical to
// mh_ini_detail::hash_key in mh_common/include/mh_ini_gate.h -- pinned by a golden selftest.
std::uint32_t gameplay_key_id(const char *sec, const char *key) noexcept;

// Serialize into `out` (>= GAMEPLAY_MAX_ENCODED bytes). Returns bytes written, or 0 if `s` is
// invalid (n out of 0..32, or an entry type other than INT/F64).
std::size_t gameplay_set_encode(const GameplaySet &s, std::uint8_t *out) noexcept;

// Parse `len` bytes (trailing bytes after the last entry are ignored). `out` is cleared first and
// is only meaningful on Ok.
GameplayErr gameplay_set_decode(const std::uint8_t *in, std::size_t len, GameplaySet &out) noexcept;

// FNV-1a over the (key_id, type) pairs sorted by key_id: order independent, value independent --
// it names WHICH keys a peer syncs, so two builds with different key lists refuse each other.
// Never returns 0 (0 is reserved to mean "none"): a hash of 0 maps to 1.
std::uint32_t gameplay_set_digest(const GameplaySet &s) noexcept;

} // namespace mh_net_proto
