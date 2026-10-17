//
// desync/scene_dump.h -- the field-report scene dump (replay_record.h SCENE_NAME): the libmh world blob of
// THIS process, captured without libmh.dll. See scene_rename.h for why the capture code is a private copy.
//
#pragma once
#include <cstddef>
#include <cstdint>

namespace mh::desync::scene {

// Capture the world into a new heap buffer. -> the blob (free with release()) or null; *len = its bytes,
// *rc = the capture's return code (0 = ok). `game_clock` is the strategic clock in game-seconds.
uint8_t *capture(double game_clock, uint32_t step, size_t *len, int *rc);
void     release(uint8_t *blob);

// The nav capture's four live objects, bound by desync_watch.cpp (the only binder of registry regions in
// mh/desync/) and handed in once at install. `grid` = MAP_REGION_GRID.
struct nav_views {
    void **list_head;
    void **free_head;
    void **by_index;
    void  *grid;
};
void set_nav_views(const nav_views &v);

// ---- the scene REPLAY half: import a scene dump into THIS process with no libmh.dll -------------------------
//
// libmh_import_world (libmh/state/spine.cpp) is the byte engine, the fixup trailer, the nav graph and then
// SEVEN re-derives run as libmh's TRANSLATED bodies. In configuration (1) the original bodies are live, so
// the same re-derives are the game's own functions at their VAs, in the order the original uses
// (map_ReadMap -> setup_dimensions -> load_passable_plane -> pool_reset). The caller (desync_watch.cpp, which
// can name the game's functions without this TU's namespace renames) hands them in. Not re-run here,
// deliberately: register_state_handlers / register_bldg_type_callbacks -- they put THIS process's thunk
// addresses into the dispatch tables of a world recorded by a promoted run; a configuration-(1) recording
// carries the game's own addresses, valid in the same image.
struct import_ops {
    void (*pathfinder_init)();   // llm_strat_pathfinder_init   @0x004614f9
    void (*setup_dimensions)();  // llm_map_setup_dimensions    @0x004989c2
    void (*pool_reset)();        // llm_map_region_pool_reset   @0x004234b8
    void (*route_step_deltas)(); // llm_map_init_region_route_step_deltas @0x00423299
    void *(*alloc)(uint32_t n);  // utils_malloc  @0x004d0155 -- the nav pool's nodes are the game's allocations
    void (*release)(void *p);    // utils_free    @0x004d0244
    void (*log)(const char *fmt, ...);
};
// -> 0 (WORLD_OK) or a negative world/fixup/nav code. A refusal from the validation phase (a bad blob, a
// different-build dump: -23) leaves the world untouched.
int import_world(const void *blob, size_t n, const import_ops &ops);

} // namespace mh::desync::scene
