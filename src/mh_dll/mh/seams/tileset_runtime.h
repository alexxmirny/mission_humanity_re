#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mh::mods {
struct Snapshot;
}
namespace mh::tileset_runtime {
struct Entry {
    std::string  token;
    std::wstring label;
    uint8_t      stock_index;
    uint8_t      tree_bank;
    uint8_t      soldier_offset;
};
struct Prepared {
    std::vector<Entry> entries;
};
// Prepare privately, arm the guarded sites, then publish only after all boot checks.
bool           prepare(const mh::mods::Snapshot &, Prepared &, std::string &error);
bool           install();
void           publish(Prepared &&) noexcept;
const wchar_t *label(const char *token);
bool           apply_planet_graphics(int32_t planet, const char *token);
// A failed admission is an internal invariant violation, not a resource fallback.
void require_planet_graphics(int32_t planet, const char *token);
} // namespace mh::tileset_runtime
