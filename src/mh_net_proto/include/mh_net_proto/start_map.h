#pragma once
// FLAG_START trailer: gameplay bytes, the selected native map header, then
// 'MHMP', format=1 (u16 LE), header size=0x17c (u16 LE). One reliable message
// publishes slots/settings/map together; periodic lobby previews are not authority.
#include "gameplay_set.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mh_net_proto {
constexpr std::size_t  START_MAP_BYTES           = 0x17c;
constexpr std::uint8_t START_MAP_FOOTER[]        = {'M', 'H', 'M', 'P', 1, 0, 0x7c, 1};
constexpr std::size_t  START_MAP_SUFFIX_BYTES    = START_MAP_BYTES + sizeof(START_MAP_FOOTER);
constexpr std::size_t  START_TRAILER_MAX_ENCODED = GAMEPLAY_MAX_ENCODED + START_MAP_SUFFIX_BYTES;

// Gameplay is already encoded at out[0..gameplay_size). Refuse before writing
// if the complete suffix cannot fit. Returns the complete trailer size, or 0.
inline std::size_t start_map_append(std::uint8_t *out, std::size_t cap,
                                    std::size_t gameplay_size, const std::uint8_t *map) noexcept {
    if (!out || !map || gameplay_size > GAMEPLAY_MAX_ENCODED ||
        cap < gameplay_size + START_MAP_SUFFIX_BYTES) return 0;
    std::memcpy(out + gameplay_size, map, START_MAP_BYTES);
    std::memcpy(out + gameplay_size + START_MAP_BYTES, START_MAP_FOOTER, sizeof(START_MAP_FOOTER));
    return gameplay_size + START_MAP_SUFFIX_BYTES;
}

// The map view borrows the received trailer. Missing/unknown/short envelopes
// are refused, never replaced by an independently received lobby preview.
// Native map-content validation remains the main-thread admission owner's job.
inline bool start_map_split(const std::uint8_t *in, std::size_t size,
                            std::size_t &gameplay_size, const std::uint8_t *&map) noexcept {
    gameplay_size = 0;
    map           = nullptr;
    if (!in || size < START_MAP_SUFFIX_BYTES || size > START_TRAILER_MAX_ENCODED ||
        std::memcmp(in + size - sizeof(START_MAP_FOOTER), START_MAP_FOOTER, sizeof(START_MAP_FOOTER)))
        return false;
    gameplay_size = size - START_MAP_SUFFIX_BYTES;
    map           = in + gameplay_size;
    return true;
}
} // namespace mh_net_proto
