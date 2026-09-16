//
// ai/ai_site_scan.h -- the two whole-map build-site scanners (RI-AI / AI1A layer 5).
//
// Two functions, one shared output: both append to the SAME shared build-site candidate scratch
// (ai_view::site_candidates / site_candidate_count) and neither resets its count -- the count is an
// INHERITED input, cleared only by the caller (llm_strat_ai_plan_construction). See ai_state.h's
// "batch A layer 5" block and _CONTEXT.md for the full brief; the short version:
//
//   - both scan the whole map, x outer over *map_width, y inner over *map_height, BOTH RE-READ FROM
//     MEMORY on every iteration (nothing here hoists them into a local);
//   - both stamp kind = SITE_KIND_GRID_FIT (0);
//   - both end their append block with `INC count; CMP count,0x1000; JZ <outer's INC>` -- this is NOT
//     a capacity cap (see SITE_CANDIDATE_BREAK_AT's comment in ai_state.h): it fires once, on `==`,
//     after the increment, and leaves only the INNER (y) loop, so the outer (x) loop resumes and
//     appends past 0x1000 on its very next pass, same as if the check were absent everywhere else.
//     Reproduce the shape exactly -- do not turn it into a bound.
//
// They differ in exactly two ways, both preserved here rather than folded into one shared body:
//   - `bldg_scan_grid_candidates` runs ONE grid_match_stencil probe per tile, at (x, y), with no
//     masking anywhere in the function. `scan_build_site_candidates` runs FOUR, at (x, y),
//     (x, (y+1)&height_m), ((x+1)&width_m, (y+1)&height_m), ((x-1)&width_m, (y+1)&height_m) -- three
//     of the four on row y+1, which the original computes ONCE, unmasked, and re-masks at each use.
//     The footprint test that follows both uses the RAW, unmasked (x, y) in both functions.
//   - `bldg_scan_grid_candidates`'s decompile renders the home-tile read as
//     `player_data[0].ai_tile_flags_grid + extraout_EDX + -0x18`, a __cdecl-cspec artifact
//     (llm_strat_ai_grid_match_stencil is __cdecl and preserves every register but EAX, so the
//     pre-call EDX = player * 0x288fc survives the call; Ghidra's cspec assumes it is clobbered and
//     invents a phantom read). The two reads are player_data[player].ai_home_tile_x/_y, exactly as
//     `scan_build_site_candidates`'s OWN decompile already renders them (that function's dist_sq call
//     site is not affected by the same artifact -- EBX, not EDX, carries the player offset there, and
//     nothing clobbers it either).
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrappers below are this applied to state(); the split costs one inlined call.
namespace detail {

// llm_strat_ai_bldg_scan_grid_candidates @0x004e6591.
//
// For every tile of the whole map: one grid_match_stencil probe at (x, y) against
// cfg_buildings[building_idx].area (target_byte 2); on a hit, one footprint_scan_for_blocked_cell
// probe (inverted polarity -- nonzero means the footprint FITS) at the same raw (x, y); on a hit,
// append {tile_x=x, tile_y=y, kind=SITE_KIND_GRID_FIT, dist_sq=toroidal_dist_sq(home, (x,y))} to the
// shared site-candidate scratch and bump its count.
void bldg_scan_grid_candidates(const ai_view &v, const ai_store &own, const ai_calls &gc,
                               uint32_t player, int32_t building_idx);

// llm_strat_ai_scan_build_site_candidates @0x004e66a4.
//
// Same skeleton as bldg_scan_grid_candidates, but the tile must pass FOUR grid_match_stencil probes
// (see the header comment above for the exact coordinates and the y+1 masking discipline) before the
// footprint test, which -- like the grid scanner -- runs against the RAW unmasked (x, y).
void scan_build_site_candidates(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                int32_t player_idx, int32_t building_idx);

} // namespace detail

void bldg_scan_grid_candidates(uint32_t player, int32_t building_idx);
void scan_build_site_candidates(int32_t player_idx, int32_t building_idx);

} // namespace mh::ai
