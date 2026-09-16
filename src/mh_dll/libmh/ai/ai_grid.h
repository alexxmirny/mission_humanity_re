//
// ai/ai_grid.h -- the AI grid stamp/clear/fill/flood helpers.
//
// Four tiny __cdecl leaf functions with no callees of their own. All four write only inside a
// caller-supplied grid row (handed in as a raw pointer, NOT reached through the view) and index it
// with the same X-outer/Y-inner ((x<<8)|y) packing. The first two (RI-AI / AI1A batch A layer 2) are
// used exclusively by llm_strat_ai_turret_threat_rescan (ai_turret_threat.cpp) to wipe and re-derive
// a player's "an enemy turret reaches this tile" bitmap over player_data::ai_tile_flags_grid; the
// last two (RI-AI / AI1B batch B layer 2) are used exclusively by
// llm_strat_ai_recompute_map_influence (ai_map_influence.cpp) over the SAME grid field, for the
// unrelated influence-map computation -- two different consumers sharing one per-player byte plane.
//
// THE COMMITTED GHIDRA PARAMETER NAMES ARE MISLEADING for the first two -- see ai_grid.cpp's file
// comment for the full derivation. The declarations below use the TRUE meaning (as read off the call
// sites and the index arithmetic), matching the names already committed in ai_calls (ai_state.h):
// `width`/`height` are the map's X/Y extents, not "grid rows/cols" or "grid height/width" in
// Ghidra's swapped sense. The last two never had a swap of their own -- their own plates derive the
// axis assignment directly from their sole caller's argument order.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrappers below are this applied to state(); the split costs one inlined
// call.
namespace detail {

// llm_strat_ai_grid_stamp_threat_ring @0x004b4c3f.
//
// Packs a wrap mask into the shared scratch `own.grid_wrap_mask` (low byte = height-1, next byte =
// width-1 -- a genuine two-BYTE-store side effect that survives into the read a few instructions
// later, reproduced exactly rather than folded away), then walks
// spiral_offsets[0 .. spiral_ring_cell_counts[radius]) and, for every entry, ORs
// TILE_FLAG_TURRET_THREAT into
//   grid[(((x + dx) & (width-1)) << 8) | ((y + dy) & (height-1))]
// -- the same X-outer/Y-inner packing as tile_at(). `x`/`y` are the ring's anchor tile (the turret's
// position); `radius` indexes the spiral ring-count table, it is not a tile distance.
void grid_stamp_threat_ring(const ai_view &v, const ai_store &own, uint8_t *grid, int32_t width,
                            int32_t height, int32_t x, int32_t y, int32_t radius);

// llm_strat_ai_grid_clear_threat_bit @0x004b4c94.
//
// ANDs ~TILE_FLAG_TURRET_THREAT into every grid[(x<<8)|y] for x in [0,width), y in [0,height) --
// same X-outer/Y-inner packing as its sibling above. Uses the ORIGINAL's 8-BIT WRAPPING BL/BH loop
// counters and its decrement-then-test (do-while) loop structure, not a 32-bit-safe rewrite; see
// ai_grid.cpp for why that is load-bearing.
void grid_clear_threat_bit(uint8_t *grid, int32_t width, int32_t height);

// llm_strat_ai_grid_fill_below_threshold @0x004b49da (RI-AI / AI1B batch B layer 2).
//
// Sole caller llm_strat_ai_recompute_map_influence (ai_map_influence.cpp), once, as the grid-init
// pass: fills every cell whose value is <= threshold with fill_value, over the caller-supplied
// player_data::ai_tile_flags_grid row. `width`/`height` are the map's X/Y extents (same axis
// convention as the two functions above -- see ai_map_influence.cpp for the derivation, not
// re-derived here since this function has no axis-swap of its own to resolve).
void grid_fill_below_threshold(uint8_t *grid, int32_t width, int32_t height, int32_t threshold,
                               int32_t fill_value);

// llm_strat_ai_grid_flood_step @0x004b4a10 (RI-AI / AI1B batch B layer 2).
//
// Sole caller llm_strat_ai_recompute_map_influence, 27 times per call to that function: one
// flood-fill / dilation wavefront step. For every cell whose value is < source_level and has any of
// its 4 toroidal neighbours EQUAL to source_level, sets it to (fill_value | 0x20); a second pass
// then clears the 0x20 temp mark dword-wise. THE 0x20 MARK IS LOAD-BEARING (see ai_grid.cpp): three
// of the caller's six call shapes pass source_level == fill_value, so without the mark a just-filled
// cell would seed its own neighbours and turn one wavefront step into a full flood.
//
// Shares own.grid_wrap_mask with grid_stamp_threat_ring above -- same scratch, same packing
// ((width-1)<<8)|(height-1), reproduced (not assumed) from this function's own byte stores.
void grid_flood_step(const ai_store &own, uint8_t *grid, int32_t width, int32_t height,
                     int32_t source_level, int32_t fill_value);

// llm_strat_ai_grid_stamp_seeds @0x004b4ac2 (RI-AI batch B/C, 2026-08-07). The influence-grid
// stamping primitive: callers are llm_strat_ai_notify_map_changed / _2 (batch B) and
// llm_strat_ai_notify_object_removed (batch C layer 0, already translated -- ai_notify_removed.cpp).
//
// Packs own.grid_wrap_mask the SAME way as grid_stamp_threat_ring/grid_flood_step above (low byte =
// height-1, next byte = width-1), then walks a span_x by span_y block of `stencil` -- read
// CONTIGUOUSLY, X outer / Y inner, matching cfg Building::area's byte[10][10] layout -- and for
// every NON-ZERO stencil byte writes
//   grid[(((origin_x + i) & (width-1)) << 8) | ((origin_y + j) & (height-1))]
//       = (old_byte & 0xc0) | (uint8_t)seed_value
// i.e. preserves the cell's top 2 flag bits and overwrites the rest with seed_value's low byte --
// read verbatim off the `CONCAT11(grid[idx],(char)seed_value) & 0xc0ff` sequence, which ORs in
// seed_value's WHOLE byte, not just its low 6 bits (every real caller passes seed_value < 0x40 so the
// two readings coincide in practice, but the mask is on the OLD byte, not on seed_value).
void grid_stamp_seeds(const ai_store &own, uint8_t *grid, int32_t map_width, int32_t map_height,
                      const uint8_t *stencil, int32_t span_x, int32_t span_y, int32_t origin_x,
                      int32_t origin_y, int32_t seed_value);

} // namespace detail

void grid_stamp_threat_ring(uint8_t *grid, int32_t width, int32_t height, int32_t x, int32_t y,
                            int32_t radius);
void grid_clear_threat_bit(uint8_t *grid, int32_t width, int32_t height);
void grid_fill_below_threshold(uint8_t *grid, int32_t width, int32_t height, int32_t threshold,
                               int32_t fill_value);
void grid_flood_step(uint8_t *grid, int32_t width, int32_t height, int32_t source_level,
                     int32_t fill_value);
void grid_stamp_seeds(uint8_t *grid, int32_t map_width, int32_t map_height, uint8_t *stencil,
                      int32_t span_x, int32_t span_y, int32_t origin_x, int32_t origin_y,
                      int32_t seed_value);

} // namespace mh::ai
