#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "addr/mh_structs.gen.h"

namespace mh::ui::detail {

// Only fields consumed by the map-info finalizer or used to locate/check its map.
// Normalize string tails after NUL; unlike a raw-header memcmp, stale padding is not identity.
struct MapInfoIdentity {
    uint32_t width = 0, height = 0, file_size = 0, checksum = 0;
    int32_t  player_count = 0;
    uint8_t  seed         = 0;
    char     path[offsetof(mh::game::mh_cfg_struct_map_header, map_name) -
              offsetof(mh::game::mh_cfg_struct_map_header, path_unc)]{};
    char     title[sizeof(mh::game::mh_cfg_struct_map_header::map_name)]{};
    char     tileset[13]{};
    uint8_t  footer[sizeof(mh::game::mh_cfg_struct_map_header::footer)]{};
};

enum class MapInfoRefresh { unchanged,
                            details,
                            slots };

class MapInfoLatch {
    MapInfoIdentity shown_{};
    bool            valid_ = false;
    template <size_t N>
    static void text(char (&dst)[N], const char *src) {
        for (size_t i = 0; i < N && src[i]; ++i) dst[i] = src[i];
    }

public:
    MapInfoRefresh update(const mh::game::mh_cfg_struct_map_header &header) {
        if (header.player_count < 1) return MapInfoRefresh::unchanged;
        const char *raw     = reinterpret_cast<const char *>(&header);
        const char *path    = raw + offsetof(mh::game::mh_cfg_struct_map_header, path_unc);
        const char *tileset = raw + offsetof(mh::game::mh_cfg_struct_map_header, tlo_name);
        const bool  slots   = !valid_ || header.player_count != shown_.player_count;
        if (!slots && header.width == shown_.width && header.height == shown_.height &&
            header.file_size == shown_.file_size && header.checksum == shown_.checksum && header.seed == shown_.seed &&
            std::strncmp(path, shown_.path, sizeof(shown_.path)) == 0 &&
            std::strncmp(header.map_name, shown_.title, sizeof(shown_.title)) == 0 &&
            std::strncmp(tileset, shown_.tileset, sizeof(shown_.tileset)) == 0 &&
            std::memcmp(header.footer, shown_.footer, sizeof(shown_.footer)) == 0)
            return MapInfoRefresh::unchanged;
        // Normalize and copy only when a meaningful field actually changes.
        shown_              = {};
        shown_.width        = header.width;
        shown_.height       = header.height;
        shown_.file_size    = header.file_size;
        shown_.checksum     = header.checksum;
        shown_.player_count = header.player_count;
        shown_.seed         = header.seed;
        text(shown_.path, path);
        text(shown_.title, header.map_name);
        text(shown_.tileset, tileset);
        std::memcpy(shown_.footer, header.footer, sizeof(shown_.footer));
        valid_ = true;
        return slots ? MapInfoRefresh::slots : MapInfoRefresh::details;
    }
};
} // namespace mh::ui::detail
