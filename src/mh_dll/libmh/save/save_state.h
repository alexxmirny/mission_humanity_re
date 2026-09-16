//
// save/save_state.h -- WHERE mh::save's LIVE-ARM STATE IS (RI-STATE / SB-BIND T5).
//
// libmh/save/ already had a binder: `save/save_table.gen.h`, the generated block table, which is where
// every SAVE BLOCK's address comes from and is checked by tools/check_sim_addresses.py. What it
// never covered is the state the live arm reaches for AROUND the blocks -- the save directory, the
// LZW workspace, the planet clock, the camera, the version table. Those were 31 raw
// `reinterpret_cast<T *>(mh::addr::NAME)` sites across save_live.cpp and save_format.cpp: the STOCK
// .bss address, baked in, which does not move when a host binds a relocated region.
//
// This header is the other half of that binder. Same shape and same two properties as
// lockstep/lockstep_state.h:
//
//   * every pointer comes from `mh::state::ptr<T>(RID_X)`, so it follows the live bind;
//   * the accessor returns BY VALUE and re-resolves on every call, so nothing caches an address.
//     A `static` here would resolve once at whatever the bind happened to be on first use and serve
//     that forever -- stale, silently, and with the right syntax (sim_state.h states the rule).
//
// WHAT IS DELIBERATELY NOT HERE: the block addresses. They come from save_table.gen.h, resolved
// through mh::state::translate() by the save driver, and a second binding of the same bytes in a
// second place is exactly the drift the region registry exists to abolish.
//
#pragma once
#include <cstdint>

namespace mh::save {

// The live arm's non-block state, resolved from the region registry. Nothing here is stored across
// a call; every member is valid only for as long as the bind that produced it.
struct live_binds {
    // The LZW compression workspace the container writer stages into, and the running maximum it
    // records. G_LZW_TEMP_DATA is registered with size 1 -- see the note in save_state.cpp.
    uint8_t  *lzw_staging;
    uint32_t *lzw_max_compressed;

    // The two path fragments every save/load filename is built from, ANSI, NUL-terminated.
    const char *save_dir;
    const char *save_temp_dir;

    // Per-planet bookkeeping stamped at save time.
    double         *planet_time;   // planet_time[32], the per-planet clock snapshot
    int32_t        *planet_status; // G_PLANET_STATUS[32]
    const double   *game_clock;    // _G_LLM_STRAT_GAME_CLOCK, the value stamped into planet_time
    const uint32_t *planet_index;  // G_PLANET_INDEX, which planet the live arm is acting on
    const double   *last_game_time;

    // Cleared or re-published across a load, so the restored session does not inherit the previous
    // one's placement preview or camera.
    uint32_t      *build_placement_id;
    uint32_t      *build_preview_suppress;
    const int32_t *cam_col;
    const int32_t *cam_row;

    // The save-mode reset dwords the container clears. REGISTERED WITH SIZE 0 -- see save_state.cpp.
    uint32_t *save_mode_reset_dwords;

    // The version table: a fixed-stride block of ANSI version strings, selected by an index. Both
    // halves are bound because save_format.cpp builds a `version_table` out of the pair.
    const char    *version_strings;
    const int32_t *version_index;
};

// Bound in save_state.cpp. By value, re-resolved per call.
live_binds binds();

} // namespace mh::save
