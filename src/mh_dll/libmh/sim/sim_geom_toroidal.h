//
// sim/sim_geom_toroidal.h -- toroidal (map-wrapping) distance/midpoint helpers (RI-SIM / SIM1F):
//
//   llm_strat_toroidal_dist_sq        @0x00669f90 (0x58 bytes)
//   llm_strat_tile_midpoint_wrapped   @0x00669fe8 (0x78 bytes)
//
// Same translation unit in the original (adjacent, contiguous addresses -- 0x00669f90..0x0066a05f,
// no gap, no other function's bytes between them). Both are PURE functions of the map's width/height
// (sim_view::map_width/map_height) plus their own stack arguments: neither .asm contains a single CALL
// instruction (not even the usual inert assert_stack_capacity prologue), and neither reads or writes
// any OTHER sim state, so neither needs a `_calls` struct -- same posture as sim_unit_fine_pos.h's
// four functions / sim_bldg_defense_cost.h's total_resource_cost / sim_bldg_get_coords.h.
//
// ---- llm_strat_toroidal_dist_sq --------------------------------------------------------------------
//
// `uint __cdecl llm_strat_toroidal_dist_sq(int x1, int y1, int x2, int y2)` -- all four params are
// stack-passed __cdecl, a documented rare exception in this codebase (see the Ghidra plate's own
// 2026-07-03 note). Per axis: `d = abs(a-b); if (d > dim/2) d = dim - d;` (the SHORTER of the two wrap
// paths), then returns `dx*dx + dy*dy` as an unsigned 32-bit value -- the original's two `MUL EAX`
// squarings each discard the high dword (EDX), so this is a genuinely TRUNCATING 32x32->32 multiply,
// not a widened one. Already the shared distance primitive sim_bldg_defense_cost.h calls through a
// one-member `_calls` struct (`gc.toroidal_dist_sq(...)`) -- that binding is untouched by this
// translation; it still resolves through `mh::call::llm_strat_toroidal_dist_sq` (the marshalled VA
// wrapper) regardless of this function being promoted, per Law 4 (location/promotion-agnostic calls).
//
// ---- llm_strat_tile_midpoint_wrapped ----------------------------------------------------------------
//
// `void __cdecl llm_strat_tile_midpoint_wrapped(int x0, int y0, int x1, int y1, int *out_x, int
// *out_y)`. Per axis: the SAME shorter-of-two-paths delta as above but SIGNED, not abs'd -- the .asm's
// two-sided `CMP/JLE` + `NEG/CMP/JGE` pair folds `x1-x0` into `(-dim/2, dim/2]` rather than clamping an
// absolute value into `[0, dim/2]` -- halved by an ARITHMETIC right shift (`SAR`, i.e. floor division
// by 2), added to the start coordinate, folded back into `[0, dim)` by an unsigned modulo. `SAR` is
// NOT C's truncating `/2`: they differ whenever the halved value is negative and odd (translator brief
// hazard #8), so this must be written as `>>` on a signed int, never as `/2`.
//
// Ghidra's decompile plate shows a `CONCAT44(EDX,EAX)` return; that is the documented Watcom
// uncommitted-return-register artifact (the plate's own comment), not a real second output -- the
// function is `void` and communicates only through `*out_x`/`*out_y` (confirmed against the listing:
// nothing after each axis's `MOV dword ptr [reg],EDX` store reads EAX/EDX again before the next axis
// begins or before LEAVE/RET).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_toroidal_dist_sq @0x00669f90. Pure; no callees.
uint32_t toroidal_dist_sq(const sim_view &v, int32_t x1, int32_t y1, int32_t x2, int32_t y2);

// llm_strat_tile_midpoint_wrapped @0x00669fe8. Pure; no callees. Signature mirrors the committed
// export/call shape (out params are raw int32_t* here -- same "fine_coord"/tile-scalar convention
// sim_unit_fine_pos.h / sim_bldg_get_coords.h use -- the public wrapper below carries the same
// `int32_t*` pointee (TACT1-P C6, 2026-09-04) to match addr/mh_calls.gen.h's existing
// `llm_strat_tile_midpoint_wrapped(int32_t, int32_t, int32_t, int32_t, int32_t*, int32_t*)`).
void tile_midpoint_wrapped(const sim_view &v, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                           int32_t *out_x, int32_t *out_y);

} // namespace detail

// Public wrappers. Signatures match the committed prototypes in addr/mh_calls.gen.h exactly.
uint32_t toroidal_dist_sq(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
void     tile_midpoint_wrapped(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t *out_x,
                               int32_t *out_y);

namespace detail {
} // namespace detail

} // namespace mh::sim
