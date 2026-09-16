// mh_net_proto -- wire framing (de)serialization. Portable, no platform/socket dependencies.
#include "mh_net_proto/net_wire.h"

namespace mh_net_proto {

namespace {
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
} // namespace

void wire_hdr_encode(const WireHdr& h, std::uint8_t* out) noexcept {
    put_u16(out + 0, h.magic);
    put_u16(out + 2, h.flags);
    put_u16(out + 4, std::uint16_t(h.src));
    put_u16(out + 6, std::uint16_t(h.dst));
    put_u32(out + 8, h.len);
}

bool wire_hdr_decode(const std::uint8_t* in, WireHdr& out) noexcept {
    out.magic = get_u16(in + 0);
    out.flags = get_u16(in + 2);
    out.src   = std::int16_t(get_u16(in + 4));
    out.dst   = std::int16_t(get_u16(in + 6));
    out.len   = get_u32(in + 8);
    return out.magic == WIRE_MAGIC;
}

} // namespace mh_net_proto
