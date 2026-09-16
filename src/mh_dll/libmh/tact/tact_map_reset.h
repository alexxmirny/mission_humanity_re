//
// tact/tact_map_reset.h -- TACT1A batch A: wipe the tactical sub-block of the shared planes and
// hand off to the mission loader.
//
//   llm_tact_map_reset @0x0042e713 (0x176)
//
// Reserves path-pool slot 0 (the tactical-mission-only owner index into the shared
// _G_LLM_STRAT_PATH_SLOT_FLAGS pool -- slot 0 marked USED, slots 1-99 marked FREE), then wipes the
// top-left 128x128 sub-block of the shared [256][256] tile grid: every tile_object reset to
// {flags={2,0x40}, building=0, unit={0,0}, class_owner=0, visibility=0}, passable set to
// PASSABLE_DEFAULT (2), and the mission's 8-slot-per-cell tile-height-sprite cache zeroed. Finally
// tail-calls llm_tact_mission_load(mission_name), discarding its return value (the original does
// not save EAX after the call either).
//
// review_required:true (manifest) -- gated: see tact_squad_assault_resolve.h's header for why
// (structurally un-shadow-armable, its only outward call -- mission_load -- is itself blocked on
// the open, non-autonomous TACT-CUT2 item, so this function inherits the same hazard transitively;
// BFS over call_graph_no_crt.json confirms it reaches the identical 5 ungated effectful callees as
// mission_load: GetResourseFilePtr, llm_rand, llm_gfx_convert_pixels_565_to_555,
// llm_tlo_palette_convert_565_to_555, llm_fatal_cleanup). The mission_load call is mocked via a
// `_calls` struct (this codebase's sim/ pattern) so the proof never executes the real loader or
// anything beneath it; the region-writing half (path slots, tile grid, passable, height sprites) is
// asserted directly and offline, per TACT1A's own done_when.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The one outward call this function makes, indirected for offline testability -- same shape as
// tact_squad_assault_resolve_calls.
struct map_reset_calls {
    void (*mission_load)(char *mission_name); // llm_tact_mission_load @0x0043717f
};

const map_reset_calls &live_map_reset_calls();

namespace detail {

// llm_tact_map_reset @0x0042e713.
//
// 1. @0x0042e72e-0x0042e765: path_slot_flag_at(0, 0) = 1 (this mission's own reserved slot);
//    path_slot_flag_at(0, j) = 0 for j in [1, 100) (every OTHER slot of owner 0 freed).
// 2. @0x0042e767-0x0042e86d: for col in [0, 128), for row in [0, 128):
//    - tile_object_at(col, row) reset: flags = {2, 0x40}, building = 0, unit = {0, 0},
//      class_owner = 0, visibility = 0 (@0x0042e7af-0x0042e823).
//    - passable_at(col, row) = PASSABLE_DEFAULT (@0x0042e82c).
//    - for k in [0, 8): map_tile_height_sprites()[col*1024 + row*8 + k] = 0 (@0x0042e84a-0x0042e86b).
// 3. @0x0042e877-0x0042e87a: tail-call mission_load(mission_name), return value discarded.
void map_reset(tact_store &own, const map_reset_calls &c, char *mission_name);

} // namespace detail

void map_reset(char *mission_name);


} // namespace mh::tact
