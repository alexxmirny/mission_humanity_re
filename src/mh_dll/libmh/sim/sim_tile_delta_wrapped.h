//
// sim/sim_tile_delta_wrapped.h -- signed toroidal per-axis delta (RI-SIM / SIM1F):
//
//   llm_strat_tile_delta_wrapped   @0x004941b9 (0xe8 bytes)
//
// `void __watcall llm_strat_tile_delta_wrapped(int x1, int y1, int x2, int y2, int *out_dx, int
// *out_dy)`. The signed sibling of sim_geom_toroidal.h's tile_midpoint_wrapped: for each axis it
// takes the SHORTER of the two cylindrical-wrap paths from (x1,y1) to (x2,y2) and writes the signed
// component to *out_dx / *out_dy. Per axis:
//   sign = (start > end) ? -1 : +1;  d = |end - start|  (computed as the positive subtraction on the
//   branch the sign selects, NOT via abs);  if (dim/2 < d) { d = dim - d; sign = -sign; }
//   *out = sign * d;
// PURE apart from the map width/height (sim_view::map_width/map_height @0x00825084/0x00825064, the
// same two globals tile_midpoint_wrapped reads) -- the only CALL in the .asm is the inert
// assert_stack_capacity prologue, and it touches no other sim state, so no `_calls` struct.
//
// dim/2 is `(dim - (dim >> 31)) >> 1` in the listing (0x00494204-0x00494214: SAR EDX,0x1f / SUB /
// SAR EAX,1 -- round-toward-zero halving); reproduced as such though `dim` (a map dimension) is
// always positive, so it equals `dim >> 1`. This is a comparison threshold on a positive value, not
// the hazard-#8 SAR-vs-/2 rounding of a possibly-negative quantity -- `d` itself is always >= 0.
//
// Ghidra's plate marks this "fixed calling convention 2026-07-03, 6-param mixed register+stack
// __watcall, no floats"; the committed prototype (addr/mh_calls.gen.h) is
// `void(int32_t,int32_t,int32_t,int32_t, void*, void*)`, which the public wrapper below matches.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_tile_delta_wrapped @0x004941b9. Pure (map width/height only); no callees.
void tile_delta_wrapped(const sim_view &v, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                        int32_t *out_dx, int32_t *out_dy);


} // namespace detail

// Public wrapper. Signature matches the committed prototype in addr/mh_calls.gen.h exactly.
void tile_delta_wrapped(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t *out_dx,
                        int32_t *out_dy);

} // namespace mh::sim
