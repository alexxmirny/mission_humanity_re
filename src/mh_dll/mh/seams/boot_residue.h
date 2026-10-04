//
// seams/boot_residue.h -- mp:D45: the pure core of "restore what a fresh process has".
//
// A process that loaded a single-player savegame (or played an earlier match) carries that game's bytes in
// hashed sim regions the D36 clear does not name: strat_players (landing entries at planets 1..13, name tails
// past the NUL), planets (lowercase campaign .mp/.tlo strings) and prod_slots (stale travel/cargo bytes in
// free slots). Retail's session_begin_multi writes only what the match needs, so the residue rides into every
// match and the in-band desync watch flags host-vs-client from step 50.
//
// The repair is the same idea as D36's, but its "fresh" value is MEASURED, not assumed: a snapshot of each
// region taken at the first idle main-menu frame (cfg loaded, no session begun) is what a fresh process enters
// a match with. Planets in particular hold cfg-loaded data, so zeroing them would be wrong; the snapshot is.
// The functions here are plain byte arithmetic so net_selftest's statetest can drive them with real struct
// layouts; launch.cpp owns the regions, the capture point and the ini knob.
//
#pragma once
#include <stdint.h>
#include <string.h>

namespace mh::boot_residue {

// Bytes of `live` that differ from `snap`.
inline uint32_t count_diff(const uint8_t *live, const uint8_t *snap, uint32_t len) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < len; ++i) n += live[i] != snap[i];
    return n;
}

// Returns the differing-byte count BEFORE the restore (the "nonzero before clear" measurement: 0 on a fresh
// process); copies the snapshot over `live` only when `restore` is true (the knob's off arm measures only).
inline uint32_t restore_region(uint8_t *live, const uint8_t *snap, uint32_t len, bool restore) {
    const uint32_t n = count_diff(live, snap, len);
    if (restore && n != 0) memcpy(live, snap, len);
    return n;
}

} // namespace mh::boot_residue
