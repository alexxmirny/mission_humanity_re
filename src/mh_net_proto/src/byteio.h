// mh_net_proto -- internal little-endian byte read/write helpers (portable, no platform deps).
// Private to the library implementation (kept in src/, not shipped in include/).
#pragma once
#include <cstdint>

namespace mh_net_proto {
namespace detail {

inline void put_u16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = std::uint8_t(v); p[1] = std::uint8_t(v >> 8);
}
inline void put_u32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = std::uint8_t(v);       p[1] = std::uint8_t(v >> 8);
    p[2] = std::uint8_t(v >> 16); p[3] = std::uint8_t(v >> 24);
}
inline std::uint16_t get_u16(const std::uint8_t* p) noexcept {
    return std::uint16_t(std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8));
}
inline std::uint32_t get_u32(const std::uint8_t* p) noexcept {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8)
         | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

} // namespace detail
} // namespace mh_net_proto
