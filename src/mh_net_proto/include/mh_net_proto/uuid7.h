// mh_net_proto -- UUIDv7 (RFC 9562 §5.7): the `match_id` that correlates one match's logs across
// machines. Portable and PURE: no clock, no RNG, no OS header. The caller supplies both inputs --
// a Unix millisecond timestamp and 10 random bytes -- so the same function serves the Windows DLL
// (GetSystemTimeAsFileTime + BCryptGenRandom, see mh_common/include/mh_session_id.h), the future
// Linux relay, and a selftest feeding fixed vectors. See the MP refinement plan, decision D8.
//
// WHY v7 AND NOT v4 OR THE EXISTING name#tag. The id has to be (a) unique across machines with no
// coordination, (b) k-sortable, so a directory listing of collected reports is in match order, and
// (c) self-dating, so a report whose logs were trimmed still says when the match was. v7 puts the
// 48-bit millisecond timestamp in the leading bytes and gets all three. The `name#tag` lobby-id
// stays exactly what it was: the HUMAN label of a lobby, 32 bits, chosen for readability.
//
// Layout (RFC 9562 §5.7), big-endian as UUIDs always are:
//   byte  0..5   unix_ts_ms, 48 bits
//   byte  6      0111vvvv   -- version 7 in the high nibble, 4 bits of rand_a
//   byte  7      rand_a low 8 bits
//   byte  8      10vvvvvv   -- variant 0b10 (RFC 4122/9562) in the top 2 bits, 6 bits of rand_b
//   byte  9..15  rand_b, 56 bits
// 74 random bits in total. The version/variant nibbles are NOT random: a reader can tell a real
// match_id from a zeroed buffer, which is what uuid7_is_nil() is for.
#pragma once
#include <cstdint>
#include <cstddef>

namespace mh_net_proto {

constexpr std::size_t UUID7_BYTES     = 16; // a match_id, on the wire and in memory
constexpr std::size_t UUID7_RAND_MAX  = 10; // random bytes uuid7_make() consumes
constexpr std::size_t UUID7_HEX_CAP   = 33; // 32 lowercase hex digits + NUL
constexpr std::size_t UUID7_TEXT_CAP  = 37; // 8-4-4-4-12 dashed form + NUL

// Build a UUIDv7 into `out` from `unix_ms` (milliseconds since 1970-01-01T00:00:00Z) and 10 random
// bytes. Only the low 48 bits of `unix_ms` are used (the field saturates in AD 10889). Total,
// deterministic and allocation-free: identical inputs give identical output, which is what makes
// this testable at all.
void uuid7_make(std::uint64_t unix_ms, const std::uint8_t rand10[UUID7_RAND_MAX], std::uint8_t out[UUID7_BYTES]) noexcept;

// The millisecond timestamp back out of a UUIDv7 (bytes 0..5). Meaningless for a nil/foreign id --
// check uuid7_version() first if the buffer's provenance is not known.
std::uint64_t uuid7_unix_ms(const std::uint8_t id[UUID7_BYTES]) noexcept;

// The version nibble (7 for one of ours) and the variant bits (2 == RFC 4122/9562).
std::uint8_t uuid7_version(const std::uint8_t id[UUID7_BYTES]) noexcept;
std::uint8_t uuid7_variant(const std::uint8_t id[UUID7_BYTES]) noexcept;

// True iff every byte is zero -- "no match_id" (a peer that has not created or joined a lobby, or
// an advert from a host too old to carry one). The sentinel is all-zero rather than a flag byte
// because a zeroed SessionInfo is already the "nothing yet" state everywhere else in this header's
// neighbour.
bool uuid7_is_nil(const std::uint8_t id[UUID7_BYTES]) noexcept;

// ---- text -------------------------------------------------------------------------------------
// 32 lowercase hex digits, NO dashes, NUL-terminated. THIS IS THE LOGGED FORM: mh_net.log carries
// `; [session] match_id=<32 hex>` and tools/mp_analyze.py parses exactly that. Needs cap >=
// UUID7_HEX_CAP; a short buffer writes nothing and returns `out` unchanged (never a partial id).
const char *uuid7_hex(const std::uint8_t id[UUID7_BYTES], char *out, std::size_t cap) noexcept;

// The canonical dashed 8-4-4-4-12 form, for prose and for anything that has to look like a UUID to
// an outside reader. Needs cap >= UUID7_TEXT_CAP. Not what the log line uses.
const char *uuid7_dashed(const std::uint8_t id[UUID7_BYTES], char *out, std::size_t cap) noexcept;

// Parse either text form (32 hex digits, or the dashed 36-char form) back into 16 bytes. Returns
// false and leaves `out` untouched on any non-hex character, a wrong length, or a misplaced dash.
bool uuid7_parse(const char *s, std::uint8_t out[UUID7_BYTES]) noexcept;

// ---- connection id ------------------------------------------------------------------------------
// The relay demux key (plan decision D4/D8), derived rather than separately minted so a packet's
// routing id and its match's logs cannot disagree: conn_id = the FIRST 8 BYTES of match_id, with
// the peer slot XORed into the LOW byte -- low meaning out[7], the least significant when the 8
// bytes are read big-endian, as the UUID's own byte order is. Slots are 0..7 (the lobby's physical
// slot array), so the XOR only ever touches the bottom 3 bits and the 8-byte prefix stays
// recognisably one match's.
//
// NOTHING USES THIS YET -- the UDP relay (tracker R1) is its first consumer. It lives here now
// because the derivation belongs beside the id it derives from, and because a second, later
// definition of "the connection id" is exactly how two encodings of one decision start to differ.
constexpr std::size_t CONN_ID_BYTES = 8;
void conn_id_from_match(const std::uint8_t match_id[UUID7_BYTES], std::uint8_t slot, std::uint8_t out[CONN_ID_BYTES]) noexcept;

} // namespace mh_net_proto
