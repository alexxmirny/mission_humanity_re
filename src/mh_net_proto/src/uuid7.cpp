// mh_net_proto -- UUIDv7 (RFC 9562 §5.7) construction, text formatting and the derived relay
// connection id. Portable, no platform dependencies: the clock and the RNG are the CALLER's.
#include "mh_net_proto/uuid7.h"
#include <cstring>

namespace mh_net_proto {

namespace {
const char HEX[] = "0123456789abcdef";

// -1 for a non-hex character, so a parse can refuse rather than fold garbage into a nibble.
int hex_val(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace

void uuid7_make(std::uint64_t unix_ms, const std::uint8_t rand10[UUID7_RAND_MAX], std::uint8_t out[UUID7_BYTES]) noexcept {
    // 48-bit big-endian millisecond timestamp. Shifting a 64-bit value and truncating is why the
    // high 16 bits simply fall off rather than needing a range check.
    out[0] = std::uint8_t((unix_ms >> 40) & 0xff);
    out[1] = std::uint8_t((unix_ms >> 32) & 0xff);
    out[2] = std::uint8_t((unix_ms >> 24) & 0xff);
    out[3] = std::uint8_t((unix_ms >> 16) & 0xff);
    out[4] = std::uint8_t((unix_ms >> 8) & 0xff);
    out[5] = std::uint8_t(unix_ms & 0xff);
    // ver (0111) + 12 bits rand_a. The version nibble OVERWRITES the caller's top 4 random bits --
    // that is the spec, and it is why uuid7_make consumes 10 bytes but only 74 bits are random.
    out[6] = std::uint8_t(0x70 | (rand10[0] & 0x0f));
    out[7] = rand10[1];
    // var (10) + 62 bits rand_b.
    out[8] = std::uint8_t(0x80 | (rand10[2] & 0x3f));
    std::memcpy(out + 9, rand10 + 3, 7);
}

std::uint64_t uuid7_unix_ms(const std::uint8_t id[UUID7_BYTES]) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v = (v << 8) | id[i];
    return v;
}

std::uint8_t uuid7_version(const std::uint8_t id[UUID7_BYTES]) noexcept {
    return std::uint8_t((id[6] >> 4) & 0x0f);
}

std::uint8_t uuid7_variant(const std::uint8_t id[UUID7_BYTES]) noexcept {
    return std::uint8_t((id[8] >> 6) & 0x03);
}

bool uuid7_is_nil(const std::uint8_t id[UUID7_BYTES]) noexcept {
    for (std::size_t i = 0; i < UUID7_BYTES; ++i)
        if (id[i]) return false;
    return true;
}

const char *uuid7_hex(const std::uint8_t id[UUID7_BYTES], char *out, std::size_t cap) noexcept {
    if (cap < UUID7_HEX_CAP) return out; // never a PARTIAL id -- a truncated one would look real
    for (std::size_t i = 0; i < UUID7_BYTES; ++i) {
        out[i * 2]     = HEX[(id[i] >> 4) & 0x0f];
        out[i * 2 + 1] = HEX[id[i] & 0x0f];
    }
    out[32] = '\0';
    return out;
}

const char *uuid7_dashed(const std::uint8_t id[UUID7_BYTES], char *out, std::size_t cap) noexcept {
    if (cap < UUID7_TEXT_CAP) return out;
    static const int GROUP[] = {4, 2, 2, 2, 6}; // bytes per 8-4-4-4-12 group
    std::size_t      b = 0, o = 0;
    for (int g = 0; g < 5; ++g) {
        if (g) out[o++] = '-';
        for (int k = 0; k < GROUP[g]; ++k, ++b) {
            out[o++] = HEX[(id[b] >> 4) & 0x0f];
            out[o++] = HEX[id[b] & 0x0f];
        }
    }
    out[o] = '\0';
    return out;
}

bool uuid7_parse(const char *s, std::uint8_t out[UUID7_BYTES]) noexcept {
    if (!s) return false;
    std::uint8_t tmp[UUID7_BYTES]; // decode into scratch: `out` stays untouched on any refusal
    std::size_t  n = std::strlen(s);
    if (n != 32 && n != 36) return false;
    static const std::size_t DASH_AT[] = {8, 13, 18, 23};
    std::size_t              b = 0;
    for (std::size_t i = 0; i < n;) {
        if (n == 36) {
            bool is_dash = false;
            for (std::size_t d = 0; d < 4; ++d) is_dash = is_dash || (i == DASH_AT[d]);
            if (is_dash) {
                if (s[i] != '-') return false;
                ++i;
                continue;
            }
        }
        if (i + 1 >= n) return false;
        int hi = hex_val(s[i]), lo = hex_val(s[i + 1]);
        if (hi < 0 || lo < 0 || b >= UUID7_BYTES) return false;
        tmp[b++] = std::uint8_t((hi << 4) | lo);
        i += 2;
    }
    if (b != UUID7_BYTES) return false;
    std::memcpy(out, tmp, UUID7_BYTES);
    return true;
}

void conn_id_from_match(const std::uint8_t match_id[UUID7_BYTES], std::uint8_t slot, std::uint8_t out[CONN_ID_BYTES]) noexcept {
    std::memcpy(out, match_id, CONN_ID_BYTES);
    out[CONN_ID_BYTES - 1] = std::uint8_t(out[CONN_ID_BYTES - 1] ^ slot);
}

} // namespace mh_net_proto
