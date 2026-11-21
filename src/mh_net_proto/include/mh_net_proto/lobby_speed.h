// Fixed typed FLAG_ANNOUNCE subtype. Portable so both transports enforce host-link authority.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mh_net_proto {

inline constexpr std::uint8_t ANNOUNCE_LOBBY_SPEED = 4;
inline constexpr std::uint8_t LOBBY_SPEED_FORMAT = 1;
inline constexpr std::size_t LOBBY_SPEED_BYTES = 12;

constexpr bool lobby_speed_valid(int percent) noexcept {
    return percent >= 100 && percent <= 200 && (percent - 100) % 25 == 0;
}

struct LobbySpeed {
    std::uint32_t lobby_tag = 0;
    std::uint32_t revision = 0;
    std::uint16_t percent = 100;
};

// Wire: kind, format, u32 lobby tag, u32 revision, u16 percent; all little-endian.
// Exact length and nonzero identity/revision required. No write to out on failure.
inline bool lobby_speed_decode(const std::uint8_t *in, std::size_t len, LobbySpeed &out) noexcept {
    if (!in || len != LOBBY_SPEED_BYTES || in[0] != ANNOUNCE_LOBBY_SPEED || in[1] != LOBBY_SPEED_FORMAT) return false;
    LobbySpeed v;
    v.lobby_tag = static_cast<std::uint32_t>(in[2]) | (static_cast<std::uint32_t>(in[3]) << 8) |
                  (static_cast<std::uint32_t>(in[4]) << 16) | (static_cast<std::uint32_t>(in[5]) << 24);
    v.revision = static_cast<std::uint32_t>(in[6]) | (static_cast<std::uint32_t>(in[7]) << 8) |
                 (static_cast<std::uint32_t>(in[8]) << 16) | (static_cast<std::uint32_t>(in[9]) << 24);
    v.percent = static_cast<std::uint16_t>(in[10] | (static_cast<std::uint16_t>(in[11]) << 8));
    if (v.lobby_tag == 0 || v.revision == 0 || !lobby_speed_valid(v.percent)) return false;
    out = v;
    return true;
}

inline std::size_t lobby_speed_encode(const LobbySpeed &v, std::uint8_t *out) noexcept {
    if (!out || v.lobby_tag == 0 || v.revision == 0 || !lobby_speed_valid(v.percent)) return 0;
    out[0] = ANNOUNCE_LOBBY_SPEED;
    out[1] = LOBBY_SPEED_FORMAT;
    for (unsigned i = 0; i < 4; ++i) {
        out[2 + i] = static_cast<std::uint8_t>(v.lobby_tag >> (8 * i));
        out[6 + i] = static_cast<std::uint8_t>(v.revision >> (8 * i));
    }
    out[10] = static_cast<std::uint8_t>(v.percent);
    out[11] = static_cast<std::uint8_t>(v.percent >> 8);
    return LOBBY_SPEED_BYTES;
}

} // namespace mh_net_proto
