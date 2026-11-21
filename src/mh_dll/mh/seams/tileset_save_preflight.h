#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
namespace mh::mods {
struct Snapshot;
}
namespace mh::seams::maps {
enum class SaveProbeResult { available,
                             missing_resources,
                             invalid_save };
struct SaveProbeIo {
    void *file;
    int32_t (*tell)(void *);
    int (*seek)(void *, int32_t, int);
    uint32_t (*read)(void *, void *, uint32_t);
};
// The reader position is restored on every path, including refusal.
SaveProbeResult probe_save_resources(const SaveProbeIo &, const uint8_t      *version_table,
                                     const mh::mods::Snapshot *, std::string &error);
} // namespace mh::seams::maps
