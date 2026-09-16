//
// sim/sim_tile_pixel_wrap_delta.h -- three independent, torus-wrapped coordinate-delta helpers
// (RI-SIM / SIM1F):
//
//   llm_strat_tile_dist_wrapped   @0x0049404e (0x5b bytes) -- Manhattan tile distance (int)
//   llm_strat_pixel_delta_wrapped @0x004944f6 (0xe8 bytes) -- wrapped (dx,dy) pixel delta, INT out
//   llm_strat_map_wrapped_delta   @0x004945de (0xf4 bytes) -- wrapped (dx,dy) pixel delta, DOUBLE out
//
// GROUPED FOR TU CONVENIENCE ONLY -- three independent pure functions, no shared state, no
// call/data dependency on each other. pixel_delta_wrapped and map_wrapped_delta are BYTE-FOR-BYTE
// the same control flow over the SAME two boot fields (sim_view::geom->big_width/big_height) and
// differ ONLY in the out-parameter width (int32_t* vs double*, the latter via an x87 FILD/FSTP
// int->double widening with no precision loss) -- translated as two SEPARATE bodies rather than one
// shared helper, per each having its own .asm listing and its own call sites (translator brief rule
// 4: "no consolidating two similar bodies").
//
// llm_strat_tile_dist_wrapped is unrelated to the other two: it is the Manhattan-distance (|dx|+|dy|)
// wrapper around the NOT-YET-MIGRATED original llm_strat_tile_delta_wrapped @0x004941b9 (called via
// mh::call::, same posture as sim_planet_distance.cpp's own tile_delta_wrapped callee -- Law 4, the
// callee stays original). It reads no sim_view member and writes no sim_store region, so its detail::
// body takes only the calls struct, no `v`/`own` at all (same "no state() call at all" shape as
// sim_ai_bldg_queue_construction.cpp's bldg_queue_construction_thunk / sim_unit_facing24_delta.cpp's
// facing24_to_delta).
//
// ---- llm_strat_tile_dist_wrapped: THE ABS IDIOM ----------------------------------------------------
// The asm computes abs(dx) via the standard two's-complement idiom `(x ^ (x>>31)) - (x>>31)` (CDQ /
// XOR / SUB), which is DEFINED 32-bit wraparound at every int32_t value including INT32_MIN (produces
// 0x80000000 unchanged -- no standards-level overflow, just XOR/SUB on bit patterns). REVIEW FINDING
// (reimpl-verify, 2026-08-17), CORRECTING an earlier version of this comment that claimed a plain
// `(x < 0) ? -x : x` conditional was "bit-for-bit identical including INT_MIN": that claim conflated
// same-bit-pattern-under-non-UB-exploiting-codegen with standards-defined equivalence -- `-x` at
// x==INT32_MIN is signed-overflow UB in C++, which an optimizer is entitled to assume never happens
// (unlike the original's XOR/SUB, which has no such escape hatch). The .cpp now reproduces the
// original's own mask idiom directly (`mask = x >> 31; abs_x = (x ^ mask) - mask;`), which is both
// bit-identical AND standards-defined at every input, so use that form here, not the conditional.
//
// OUT-POINTER ORDER (read off the push order, same derivation style as sim_planet_distance.h's own
// banner): 0x0049406f-0x00494076 pushes [EBP-0x10] FIRST, then [EBP-0x14] SECOND. Watcom pushes stack
// args right-to-left, so the LAST push is the FIRST stack argument into llm_strat_tile_delta_wrapped
// -- whose own committed signature (mh_calls.gen.h) is `(x1,y1,x2,y2,out_dx,out_dy)`, so out_dx (the
// 5th param) is pushed last and out_dy (the 6th) is pushed first: out_dx == &[EBP-0x14], out_dy ==
// &[EBP-0x10]. Confirmed by the post-call reads: 0x00494088 loads [EBP-0x14] FIRST (fed through the
// abs idiom first), then 0x00494092 loads [EBP-0x10] SECOND -- i.e. dx is read before dy, matching a
// natural `tile_delta_wrapped(x1,y1,x2,y2,&dx,&dy)` call written in argument order. (The two
// identities do not affect the final Manhattan sum either way -- |dx|+|dy| == |dy|+|dx| -- so this
// is a derivation for correctness of exposition, not a behaviour-affecting choice.)
//
// ---- pixel_delta_wrapped / map_wrapped_delta: THE SHARED SHAPE ------------------------------------
// For each axis independently (x from (x1,x2), y from (y1,y2)):
//   sign/magnitude split: if (hi < lo) { sign=-1; delta=lo-hi; } else { sign=1; delta=hi-lo; }
//   fold across half the wrap extent: half = trunc(extent/2) (C++'s signed `/2` already truncates
//     toward zero, reproducing the asm's SAR-based signed-halving idiom bit-for-bit -- same
//     equivalence sim_map_wrap_delta_row.cpp's own banner already establishes for this exact idiom);
//     if (half < delta) { delta = extent - delta; sign = -sign; }
//   *out = sign * delta (widened to double for map_wrapped_delta via a plain C++ cast, which the
//     /arch:IA32 sim TUs compile to the same FILD the asm uses -- no precision loss, int32 -> double
//     is exact).
// x-axis wraps on v.geom->big_width, y-axis on v.geom->big_height -- read directly off the asm's own
// EOL comments ("general.big_width"/"general.big_height"), the SAME two fields
// sim_bldg_footprint_random_offset.cpp's header banner distinguishes from the PIXEL-space bw_mask/
// bh_mask pair (a different sub-field of the same map_geom struct) -- this function reads the raw
// pixel extents, not the wrap mask.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one external callee llm_strat_tile_dist_wrapped reaches -- llm_strat_tile_delta_wrapped stays
// original (Law 4: not in this batch), called through mh::call:: like sim_planet_distance.cpp's own
// identical callee. Indirected for offline testability (a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest).
struct tile_dist_wrapped_calls {
    // llm_strat_tile_delta_wrapped @0x004941b9 -- wrapped (dx,dy) tile delta between (x1,y1) and
    // (x2,y2), via two int32 out-pointers. Same callee/signature sim_planet_distance.h's own
    // planet_distance_calls::tile_delta_wrapped binds independently (a second, separate `calls`
    // struct naming the SAME original function -- not a collision, mirrors this codebase's established
    // one-struct-per-TU convention).
    void (*tile_delta_wrapped)(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t *out_dx,
                               int32_t *out_dy);
};

const tile_dist_wrapped_calls &live_tile_dist_wrapped_calls();

namespace detail {

// llm_strat_tile_dist_wrapped @0x0049404e. Reads no sim_view member and writes no sim_store region --
// takes only the calls struct, no `v`/`own` parameter at all (same shape as
// sim_unit_facing24_delta.cpp's facing24_to_delta / sim_ai_bldg_queue_construction.cpp's
// bldg_queue_construction_thunk).
int32_t tile_dist_wrapped(const tile_dist_wrapped_calls &c, int32_t x1, int32_t y1, int32_t x2,
                          int32_t y2);

// llm_strat_pixel_delta_wrapped @0x004944f6. Pure out-params (neither pointee is read before being
// written).
void pixel_delta_wrapped(const sim_view &v, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                         int32_t *out_dx, int32_t *out_dy);

// llm_strat_map_wrapped_delta @0x004945de. Identical control flow to pixel_delta_wrapped; DOUBLE
// out-params (int->double widened via the x87 FILD/FSTP the asm itself uses).
void map_wrapped_delta(const sim_view &v, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                       double *out_dx, double *out_dy);

} // namespace detail

// ---- the public wrappers, matching the committed prototypes exactly (sig_llm_strat_tile_dist_wrapped
// / sig_llm_strat_pixel_delta_wrapped / sig_llm_strat_map_wrapped_delta, addr/mh_export.gen.h) --------
int32_t tile_dist_wrapped(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
void    pixel_delta_wrapped(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t *out_dx,
                            int32_t *out_dy);
void    map_wrapped_delta(int32_t x1, int32_t y1, int32_t x2, int32_t y2, double *out_dx,
                          double *out_dy);

namespace detail {
} // namespace detail

} // namespace mh::sim
