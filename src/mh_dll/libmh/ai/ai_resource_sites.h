//
// ai/ai_resource_sites.h -- the resource-deposit build-site scanner (RI-AI / AI1A layer 5).
//
// One function: for a player's tracked resource-deposit list (player_data::ai_resource_sites /
// ai_resource_site_count), finds a placement site for a mine-type building next to the FIRST
// still-open, threshold-meeting deposit and appends it to the shared build-site candidate list
// (ai_view::site_candidates / ai_store::site_candidates -- see ai_state.h "batch A layer 5").
//
// Unlike its two siblings (the grid-fit and free-area scanners), this one:
//   - appends AT MOST ONE candidate per call and returns immediately on the first hit -- there is
//     no whole-map sweep and no 0x1000 count trap here;
//   - walks a bounded spiral disc (radius 5) around each deposit rather than the whole torus;
//   - can WRITE player_data on a path that adds no candidate at all: a deposit whose disc turns up
//     no buildable tile is marked invalidated (status = 0xffff) and the scan moves on to the next
//     deposit.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrapper below is this applied to state(); the split costs one inlined call.
namespace detail {

// llm_strat_ai_bldg_scan_resource_site_candidates @0x004e6396.
//
// `building_type` is a cfg Building[] TYPE id (the caller passes cfg::t::building_index and this
// body IMULs it by the 0x842 Building stride to index cfg_buildings) -- NOT a resource kind, despite
// the Ghidra-committed parameter name before 2026-08-01.
//
// Outer loop over player_data[player].ai_resource_sites[0 .. ai_resource_site_count):
//   - skip a deposit whose status != RESOURCE_SITE_STATUS_OPEN (no write at all on this path);
//   - convert its COARSE quarter-tile grid_x/grid_y to fine tile coords (fine = grid*4 + 2);
//   - gc.resource_site_meets_threshold(player, building_type, fine_x, fine_y) == 0 skips this
//     deposit entirely, again with NO status write;
//   - otherwise walk the radius-5 spiral disc around (fine_x, fine_y). Each candidate tile that
//     passes all three gates (grid_stencil_all_near_unthreatened, then
//     footprint_scan_for_blocked_cell, then bldg_check_placement_encloses_neighbors) becomes the
//     one candidate this call produces: appended to site_candidates with kind = SITE_KIND_RESOURCE,
//     the deposit's build_tile_x/_y are stamped, and the function returns immediately;
//   - if the disc is exhausted with no hit, the deposit's status is set to
//     RESOURCE_SITE_STATUS_INVALID and the outer loop continues to the next deposit.
void bldg_scan_resource_site_candidates(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                        uint32_t player, int32_t building_type);

} // namespace detail

void bldg_scan_resource_site_candidates(uint32_t player, int32_t building_type);

} // namespace mh::ai
