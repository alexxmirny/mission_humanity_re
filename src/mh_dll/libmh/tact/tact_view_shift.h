//
// tact/tact_view_shift.h -- TACT1E: the tactical view-scroll pixel/cache shifter, one
// direction/axis per function, all four sharing this one translation unit and ONE calls-struct
// (all four call the identical single frontier leaf, llm_tact_mark_view_tiles_dirty).
//
//   llm_tact_view_shift_col_inc @0x0043c0e1 (0x74)
//   llm_tact_view_shift_col_dec @0x0043c155 (0x88)
//   llm_tact_view_shift_row_inc @0x0043c1dd (0x89)
//   llm_tact_view_shift_row_dec @0x0043c266 (0x9d)
//
// Each function scroll-shifts the tactical view by one column (col_*) or one row (row_*) of
// on-screen pixels: it shift-copies the visible framebuffer window, then shift-copies (with a
// trailing fill) two parallel per-tile caches -- the redraw-skip map (_G_LLM_TILE_VIS_MAP_PTR) and
// the tactical LOS cache (_G_LLM_TACT_LOS_CACHE_PTR) -- and finally tail-calls
// llm_tact_mark_view_tiles_dirty (a leaf memset-only "state" function, per
// tools/data/tact_migration.json's own call-graph note on that callee: it writes 0x01 into a
// 20-row x 15-col region of yet another cache and calls nothing else) to force a redraw of the
// newly-exposed edge.
//
// ALL THREE REGIONS ARE POINTER VARIABLES, NOT ARRAYS -- this is why the site is invisible to the
// static write-cell sweep (tmp/state_matrix.json reports writes_shared=[]/writes_island=[] for all
// four, i.e. 0 direct/transitive write cells): every store in these functions goes through a
// pointer LOADED from _G_LLM_FRAMEBUFFER / _G_LLM_TILE_VIS_MAP_PTR / _G_LLM_TACT_LOS_CACHE_PTR
// first, so the sweep sees the pointer load and never the indirect store behind it -- the same
// blind spot recorded for out-pointer writes. The translation below writes
// through own.gfx_framebuffer() / own.tile_vis_map_ptr() / own.los_cache_ptr(), i.e. exactly the
// bytes the pointee points at, matching the .asm's own indirection.
//
// PROOF PATH: offline only (mocked framebuffer/tile_vis_map_ptr/los_cache_ptr buffers in the
// nettest harness, asserting the exact byte shift) -- NOT rig-armable by the static sweep's own
// evidence, and not expected to be: arming a real-game shadow site for these would compare live
// framebuffer bytes between two renderers, which is not what this migration proves. Do not read a
// zero-call/zero-diff rig report as a pass or a fail for this unit.
//
// THE FOUR ARE NOT A SIMPLE MIRROR-IMAGE PAIR (checked, not assumed):
//
//   * col_inc/col_dec's THREE regions are byte-for-byte symmetric with each other (same loop
//     counts, same per-region chunk sizes, same per-iteration stride, only the direction and the
//     fixed +/-0x40 (framebuffer) / +/-1 (caches) source/dest relationship flip).
//   * row_inc/row_dec's cache sections (tile_vis_map, los_cache) are likewise symmetric with each
//     other.
//   * row_inc/row_dec's FRAMEBUFFER section is NOT: row_inc's inner REP MOVSD copies 0xf0 (240)
//     dwords = 0x3c0 (960) bytes then ADDs 0x140 (320) more; row_dec's inner REP MOVSD copies 0xf1
//     (241) dwords = 0x3c4 (964) bytes then SUBs 0x13c (316) more. Both total the same 0x500
//     (1280-byte) per-iteration stride, so the two are NOT simply "same shape, opposite direction"
//     -- row_dec's framebuffer copy is 4 bytes (one dword / 2 pixels) WIDER than row_inc's, and 4
//     bytes less of the stride is the trailing gap. PRESERVED LITERALLY: do not "fix" row_dec's
//     copy width to 0xf0 to match row_inc, and do not "fix" row_inc's to 0xf1 to match row_dec --
//     the asymmetry is what the assembly does.
//   * The two caches' FILL VALUES do NOT swap between inc and dec: tile_vis_map is always filled
//     with 0x2 and the LOS cache is always filled with 0x0, in ALL FOUR functions -- only the
//     direction of travel (and, for col_*, whether the fill is interleaved per-row or, for row_*,
//     done once in a single trailing block) differs.
//   * col_* interleave a SINGLE fill byte after each of 20 fourteen-byte cache copies (one MOV per
//     outer iteration, so the fill happens 20 times, once per row); row_* instead run their 19
//     fifteen-byte cache copies first and then do ONE 15-byte block fill (REP STOSB) after the
//     loop exits. These are genuinely different write patterns, not the same logic transcribed two
//     ways -- preserve both shapes as given.
//
// Watcom things checked and found NOT present: no `CALL assert_stack_capacity` prologue in any of
// the four (all four start directly with PUSHAD) -- nothing to omit. No floats. No signed
// division. No struct access -- every byte offset below is raw pointer arithmetic over the three
// pointee buffers, matching the .asm's own flat indexing.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The one outward call all four functions make (the identical frontier leaf), indirected for
// offline testability -- same shape as tact_map_reset.h's map_reset_calls.
struct view_shift_calls {
    void (*mark_view_tiles_dirty)(); // llm_tact_mark_view_tiles_dirty @0x0043c303
};

const view_shift_calls &live_view_shift_calls();

namespace detail {

// llm_tact_view_shift_col_inc @0x0043c0e1. Shifts the view LEFT by one column (0x40 = 64
// framebuffer bytes = 32 px at 16bpp).
//
//  1. @0x0043c0e2-0x0043c106: framebuffer -- for 0x1e0 (480) rows, memmove 0xe0 dwords (0x380 =
//     896 bytes) from (dst+0x40) to dst, then advance dst/src by a further 0x180 (384) on top of
//     the 0x380 the copy itself covers (total step 0x500 = 1280 bytes/row).
//  2. @0x0043c108-0x0043c129: tile_vis_map -- for 0x14 (20) rows, memmove 0xe (14) bytes from
//     (dst+1) to dst, then fill the 15th byte (immediately past the copied span) with 0x2, then
//     advance both pointers a further 6 (total step 20/row).
//  3. @0x0043c12b-0x0043c14c: LOS cache -- identical shape to step 2, fill value 0x0.
//  4. @0x0043c14e: tail-call mark_view_tiles_dirty(); return value none.
void view_shift_col_inc(tact_store &own, const view_shift_calls &c);

// llm_tact_view_shift_col_dec @0x0043c155. Mirror of col_inc: shifts the view RIGHT by one column.
// Runs backward (STD) over the SAME three regions from their high-address end.
//
//  1. @0x0043c156-0x0043c181: framebuffer -- dst starts at framebuffer+0x95ebc (the highest byte
//     touched), src = dst-0x40; for 0x1e0 (480) rows, backward-copy 0xe0 dwords (0x380 bytes),
//     then the register retreats a further 0x180 (total step -0x500/row, mirroring col_inc's
//     +0x500 exactly -- SAME loop count and chunk size as col_inc, unlike the row_* pair below).
//  2. @0x0043c183-0x0043c1aa: tile_vis_map -- dst starts at tile_vis_map_ptr()+0x18a (the byte one
//     past the last one col_inc's forward pass would touch), src = dst-1; for 0x14 (20) rows,
//     backward-copy 0xe (14) bytes, fill the byte immediately past (below) the copied span with
//     0x2, retreat a further 6 (total step -20/row).
//  3. @0x0043c1ac-0x0043c1d3: LOS cache -- identical shape to step 2, fill value 0x0.
//  4. @0x0043c1d6: tail-call mark_view_tiles_dirty().
void view_shift_col_dec(tact_store &own, const view_shift_calls &c);

// llm_tact_view_shift_row_inc @0x0043c1dd. Shifts the view UP by one row (0x7800 = 30720
// framebuffer bytes = 24 scanlines at the 0x500-byte/scanline-block pitch this function's own
// stride implies).
//
//  1. @0x0043c1de-0x0043c205: framebuffer -- for 0x1c8 (456) blocks, memmove 0xf0 (240) dwords
//     (0x3c0 = 960 bytes) from (dst+0x7800) to dst, then advance a further 0x140 (320) (total step
//     0x500/block).
//  2. @0x0043c207-0x0043c225: tile_vis_map -- for 0x13 (19) rows, memmove 0xf (15) bytes from
//     (dst+0x14) to dst, then advance a further 5 (total step 20/row); NO per-row fill here.
//  3. @0x0043c227-0x0043c231: ONE trailing 15-byte block-fill (REP STOSB) of 0x2 at the final dst
//     position reached by step 2's loop.
//  4. @0x0043c233-0x0043c251: LOS cache -- identical shape to step 2.
//  5. @0x0043c253-0x0043c25d: ONE trailing 15-byte block-fill of 0x0, mirroring step 3.
//  6. @0x0043c25f: tail-call mark_view_tiles_dirty().
void view_shift_row_inc(tact_store &own, const view_shift_calls &c);

// llm_tact_view_shift_row_dec @0x0043c266. Mirror of row_inc: shifts the view DOWN by one row.
// Runs backward (STD). NOTE the framebuffer section is NOT loop-count-symmetric with row_inc's --
// see this header's banner comment above.
//
//  1. @0x0043c267-0x0043c295: framebuffer -- dst starts at framebuffer+0x95ec0, src = dst-0x7800;
//     for 0x1c8 (456) blocks, backward-copy 0xf1 (241) dwords (0x3c4 = 964 bytes -- FOUR MORE BYTES
//     than row_inc's forward copy), then retreat a further 0x13c (316 -- four LESS than row_inc's
//     0x140) (total step -0x500/block, same total as row_inc despite the different split).
//  2. @0x0043c297-0x0043c2bb: tile_vis_map -- dst starts at tile_vis_map_ptr()+0x18a, src = dst-0x14;
//     for 0x13 (19) rows, backward-copy 0xf (15) bytes, retreat a further 5 (total step -20/row).
//  3. @0x0043c2bd-0x0043c2c7: ONE trailing 15-byte block-fill of 0x2 at the final dst position.
//  4. @0x0043c2c9-0x0043c2ed: LOS cache -- identical shape to step 2.
//  5. @0x0043c2ef-0x0043c2f9: ONE trailing 15-byte block-fill of 0x0.
//  6. @0x0043c2fc: tail-call mark_view_tiles_dirty().
void view_shift_row_dec(tact_store &own, const view_shift_calls &c);

} // namespace detail

void view_shift_col_inc();
void view_shift_col_dec();
void view_shift_row_inc();
void view_shift_row_dec();

// Declared here per the module convention; DEFINED in tact_view_shift.cpp, CALLED from
// install_shadow() by the conductor (not this TU). One combined installer for all four sites --
// the brief for this unit asked for a single `install_shadow_view_shift`, not one per function.
namespace detail {
}

} // namespace mh::tact
