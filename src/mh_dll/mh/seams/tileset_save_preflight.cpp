#include "seams/tileset_save_preflight.h"
#include <array>
#include <cstring>
#include <vector>
#include "mh_lzw.h"
#include "include/mh_storage_policy_save.h"
#include "mh_mod_catalog.h"
#include "mh_tileset.h"
namespace mh::seams::maps {
namespace {
constexpr uint32_t BLOCK_CAP    = 0x927c0;
constexpr size_t   PLANETS_SIZE = 34016, PLANET_STRIDE = 0x427, TOKEN_OFFSET = 0x20e;
uint32_t           u32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
} // namespace
SaveProbeResult probe_save_resources(const SaveProbeIo &io, const uint8_t *versions,
                                     const mh::mods::Snapshot *snapshot, std::string &error) {
    error.clear();
    if (!io.file || !io.tell || !io.seek || !io.read || !versions) {
        error = "save probe unavailable";
        return SaveProbeResult::invalid_save;
    }
    const int32_t initial = io.tell(io.file);
    if (initial < 0) {
        error = "save position unavailable";
        return SaveProbeResult::invalid_save;
    }
    struct Restore {
        const SaveProbeIo &io;
        int32_t            pos;
        bool              *failed;
        ~Restore() {
            if (io.seek(io.file, pos, 0)) *failed = true;
        }
    };
    // No return from the guarded scope: restore failure also makes the probe fail.
    bool            ok             = true;
    bool            restore_failed = false;
    SaveProbeResult result         = SaveProbeResult::available;
    {
        Restore restore{io, initial, &restore_failed};
        auto    exact = [&](void *dst, uint32_t n) { return io.read(io.file, dst, n) == n; };
        uint8_t header[40];
        if (io.seek(io.file, 0, 0) || !exact(header, 40)) {
            error = "unreadable save header";
            ok    = false;
        }
        int version = -1;
        if (ok) {
            uint32_t required = 0;
            version           = mh::storage_policy::savefile::classify_header(header, versions, required);
        }
        // Unknown versions remain the original version gate's decision.
        if (ok && version >= 0) {
            const uint32_t       expected[] = {57500, 211400, 11648, 30900, 560, uint32_t(PLANETS_SIZE)};
            const int            count      = version <= 1 ? 6 : 5;
            std::vector<uint8_t> payload;
            for (int i = 0; ok && i < count; ++i) {
                const int e = (version > 1 && i >= 3) ? i + 1 : i;
                uint8_t   bh[8];
                if (!exact(bh, 8)) {
                    error = "truncated save block header";
                    ok    = false;
                    break;
                }
                const uint32_t packed = u32(bh), unpacked = u32(bh + 4);
                if (packed > BLOCK_CAP || unpacked != expected[e]) {
                    error = "invalid save block";
                    ok    = false;
                    break;
                }
                if (e != 5) {
                    // Bound skips against EOF so a corrupt length cannot jump into a later block.
                    const int32_t pos = io.tell(io.file);
                    if (pos < 0 || io.seek(io.file, 0, 2)) {
                        error = "save seek failed";
                        ok    = false;
                        break;
                    }
                    const int32_t end = io.tell(io.file);
                    if (end < pos || uint32_t(end - pos) < packed || io.seek(io.file, pos + int32_t(packed), 0)) {
                        error = "truncated save block";
                        ok    = false;
                        break;
                    }
                    continue;
                }
                payload.resize(size_t(packed) + 8);
                memcpy(payload.data(), bh, 8);
                if (!exact(payload.data() + 8, packed)) {
                    error = "truncated planets block";
                    ok    = false;
                }
            }
            std::array<uint8_t, PLANETS_SIZE + 8> planets{};
            if (ok && payload.size() >= 12 && u32(payload.data() + 8) == mh::lzw::MAGIC) {
                mh::lzw::decoder_state decoder{};
                ok = mh::lzw::decompress_bounded(decoder, payload.data() + 8, payload.size() - 8,
                                                 planets.data(), PLANETS_SIZE);
            } else if (ok) ok = mh::lzss::decompress_block_bounded(payload.data(), payload.size(),
                                                                   planets.data(), planets.size());
            if (!ok && error.empty()) error = "invalid compressed planets block";
            for (size_t i = 0; ok && i < 32; ++i) {
                const char *name = reinterpret_cast<const char *>(planets.data() + i * PLANET_STRIDE + TOKEN_OFFSET);
                if (!*name) continue;
                const char *end = static_cast<const char *>(memchr(name, 0, 13));
                std::string token;
                if (!end || !mh::mods::tileset_token(std::string_view(name, size_t(end - name)), token, error)) {
                    if (error.empty()) error = "invalid saved tileset token";
                    ok = false;
                } else if (!mh::mods::stock_tileset_index(token) && (!snapshot || !snapshot->tileset(token))) {
                    error  = "missing saved tileset: " + token;
                    result = SaveProbeResult::missing_resources;
                    ok     = false;
                }
            }
        }
    }
    if (restore_failed) {
        error = "could not restore save position";
        return SaveProbeResult::invalid_save;
    }
    if (!ok && result != SaveProbeResult::missing_resources) return SaveProbeResult::invalid_save;
    return result;
}
} // namespace mh::seams::maps
