// Shared view of the EXISTING generated native save table; no separate handwritten block grammar.
#pragma once
#include "mh_storage_policy_save.h"
#include "../../libmh/save/save_table.gen.h"

namespace mh::storage_policy::savefile {
namespace native_detail {
namespace table = mh::save::table;
static_assert(uint8_t(step_kind::block) == table::STEP_BLOCK && uint8_t(step_kind::regions) == table::STEP_REGIONS &&
              uint8_t(step_kind::progress) == table::STEP_PROGRESS && uint8_t(step_kind::members) == table::STEP_MEMBERS &&
              uint8_t(step_kind::media) == table::STEP_MEDIA && uint8_t(step_kind::tail) == table::STEP_TAIL);
inline bool read_step(void *, stream which, uint32_t index, step &out) {
    const table::step *steps = nullptr;
    uint32_t           count = 0;
    switch (which) {
        case stream::planet:
            steps = table::PLANET;
            count = table::PLANET_STEPS;
            break;
        case stream::container:
            steps = table::CONTAINER;
            count = table::CONTAINER_STEPS;
            break;
        case stream::regions:
            steps = table::REGIONS;
            count = table::REGIONS_STEPS;
            break;
    }
    if (index >= count) return false;
    const auto &s = steps[index];
    out           = {s.addr, s.size, static_cast<step_kind>(s.kind), s.ver_min, s.ver_max};
    return true;
}
inline uint32_t container_bytes(uint32_t address) {
    for (const auto &s : table::CONTAINER)
        if (s.kind == table::STEP_BLOCK && s.addr == address) return s.size;
    return 0;
}
} // namespace native_detail
inline layout native_layout() {
    namespace table = mh::save::table;
    layout n;
    n.read_step              = native_detail::read_step;
    n.packed_limit           = table::MEMBER_CHUNK;
    n.width_address          = table::PLANET[11].addr;
    n.height_address         = table::PLANET[12].addr;
    n.planets_address        = table::PLANETS_BASE;
    n.planets_bytes          = native_detail::container_bytes(n.planets_address);
    n.planet_stride          = table::PLANET_STRIDE;
    n.planet_system_offset   = table::PLANET_SYSTEM_OFF;
    n.session_address        = 0x00e58344u;
    n.session_bytes          = native_detail::container_bytes(n.session_address);
    n.current_system_address = table::CURRENT_SYSTEM_ADDR;
    n.current_planet_address = table::PLANET_INDEX_ADDR;
    n.planet_status_address  = table::PLANET_STATUS_ADDR;
    n.member_first           = table::MEMBER_FIRST;
    n.member_limit           = table::MEMBER_LIMIT;
    n.region_count_address   = table::REGION_COUNT_ADDR;
    n.region_record_bytes    = table::REGION_RECORD_BYTES;
    n.region_grid_bytes      = table::REGION_GRID_IDS_BYTES;
    n.region_terrain_bytes   = table::REGION_TERRAIN_BYTES;
    n.progress_count         = table::PROGRESS_COUNT;
    n.media_bytes            = table::MEDIA_BLOCK_BYTES;
    return n;
}
// Exact six 40-byte EN slots at005d08f4..005d09e3, scalar-byte read-only ReVA extraction2026-11-04.
// Primary /eng/mh.exe MD5 b3b389e8eb228638de8dbbc5c228118b. Each has34 printable bytes +6 zeros.
// These are format constants, not a fallback for an incomplete registry region (whose slot0 is35B).
inline constexpr char native_version_strings[6][40] = {
    " Extermination, demo version"
    "      ",
    " Extermination, ver 1.0"
    "           ",
    " Extermination, ver 1.01"
    "          ",
    " Extermination, ver 1.02"
    "          ",
    " Extermination, ver 1.03"
    "          ",
    " 'Mission: Humanity' ver 1.00"
    "     "};
static_assert(sizeof(native_version_strings) == 240);
inline const uint8_t *native_versions() {
    return reinterpret_cast<const uint8_t *>(native_version_strings);
}
} // namespace mh::storage_policy::savefile
