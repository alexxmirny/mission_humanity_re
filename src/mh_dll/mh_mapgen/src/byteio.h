// mh_mapgen -- explicit little-endian reads and writes (never a struct memcpy).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mh_mapgen {

inline std::uint16_t rd16(const std::uint8_t *p) { return std::uint16_t(p[0] | (p[1] << 8)); }
inline std::uint32_t rd32(const std::uint8_t *p) { return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24); }
inline std::int32_t  rdi32(const std::uint8_t *p) { return std::int32_t(rd32(p)); }

inline void wr32(std::uint8_t *p, std::uint32_t v) {
    p[0] = std::uint8_t(v);
    p[1] = std::uint8_t(v >> 8);
    p[2] = std::uint8_t(v >> 16);
    p[3] = std::uint8_t(v >> 24);
}

inline void put16(std::vector<std::uint8_t> &o, std::uint16_t v) {
    o.push_back(std::uint8_t(v));
    o.push_back(std::uint8_t(v >> 8));
}
inline void put32(std::vector<std::uint8_t> &o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(std::uint8_t(v >> (8 * i)));
}

} // namespace mh_mapgen
