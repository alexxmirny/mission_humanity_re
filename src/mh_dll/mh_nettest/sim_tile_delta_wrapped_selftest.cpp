//
// sim_tile_delta_wrapped_selftest.cpp -- `simtest` case for llm_strat_tile_delta_wrapped
// (sim/sim_tile_delta_wrapped.h/.cpp @0x004941b9, RI-SIM / SIM1F). Pure function of the map's
// width/height (sim_view::map_width/map_height) plus its own four args -- no outward calls (the
// only CALL in the .asm is the inert assert_stack_capacity prologue), no `_calls` struct.
//
// EXPECTED VALUES ARE HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_strat_tile_delta_wrapped_004941b9.asm), not read off the .cpp. Per axis (X shown,
// Y is the identical shape over height, 0x00494239-0x00494294):
//   if (start > end) { sign = -1; d = start - end; }        (0x004941dd/e0, CMP/JLE, signed)
//   else              { sign = +1; d = end - start; }
//   half = dim >> 1                                          (0x00494204-0x00494214; dim is always
//       positive here, so the asm's `(dim - (dim>>31))>>1` round-toward-zero form == plain `dim>>1` --
//       this is a threshold on the always-nonnegative `d`, NOT the hazard-#8 SAR-on-a-possibly-
//       negative-value the sibling tile_midpoint_wrapped has, so no `/2` rounding hazard applies here)
//   if (half < d) { d = dim - d; sign = -sign; }             (0x00494216/0x00494219)
//   *out = sign * d;                                         (0x0049422d-0x00494237)
//
// The fixture's map_width=200, map_height=120 (sim_test_support.h's reset(), deliberately distinct
// and non-power-of-two-friendly) -- an axis-swapped translation disagrees with every case here.
//
#include "sim/sim_tile_delta_wrapped.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

void check_delta(sim_fixture &fx, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t want_dx,
                 int32_t want_dy, const char *what_dx, const char *what_dy) {
    int32_t odx = 0x0badf00d, ody = 0x0badf00d;
    detail::tile_delta_wrapped(fx.view(), x1, y1, x2, y2, &odx, &ody);
    ck_eq((uint32_t)odx, (uint32_t)want_dx, what_dx);
    ck_eq((uint32_t)ody, (uint32_t)want_dy, what_dy);
}

} // namespace

void run_tile_delta_wrapped_tests() {
    sim_fixture fx;
    // width=200 (half=100), height=120 (half=60), both set by fx.reset().

    // ---- D1: X axis, end>start branch (sign=+1), no fold. x1=10,x2=50: d=40<=100. result=+40.
    fx.reset();
    check_delta(fx, 10, 0, 50, 0, 40, 0, "delta D1 X end>start no-fold .dx", "delta D1 .dy (unused)");

    // ---- D2: X axis, start>end branch (sign=-1), no fold. x1=50,x2=10: d=40<=100. result=-40.
    // MUTATION: swapping the sign assignment between the two branches (JLE mistranslated the wrong
    // way) flips D1's +40 to -40 or this case's -40 to +40 -- the pair together pins BOTH signs.
    fx.reset();
    check_delta(fx, 50, 0, 10, 0, -40, 0, "delta D2 X start>end no-fold .dx", "delta D2 .dy (unused)");

    // ---- D3: X axis fold, end>start branch. x1=0,x2=150: sign=+1,d=150. half=100<150 -> fold:
    // d=200-150=50, sign flips to -1. result=-50.
    fx.reset();
    check_delta(fx, 0, 0, 150, 0, -50, 0, "delta D3 X end>start fold .dx", "delta D3 .dy (unused)");

    // ---- D4: X axis fold, start>end branch. x1=150,x2=0: sign=-1,d=150. half=100<150 -> fold:
    // d=50, sign flips to +1. result=+50.
    // MUTATION: forgetting the `sign=-sign` flip on fold leaves D3 at +150/-150-scaled or D4 at
    // -50 instead of +50 -- either fold case catches a missing flip.
    fx.reset();
    check_delta(fx, 150, 0, 0, 0, 50, 0, "delta D4 X start>end fold .dx", "delta D4 .dy (unused)");

    // ---- D5: X boundary d == half (100) from the end>start branch -- guard is `half < d` (strict),
    // so an EXACT 100 must NOT fold. x1=0,x2=100: sign=+1,d=100. result=+100.
    // MUTATION: `half <= d` here would fold (d=200-100=100, sign=-1 -> result -100) -- goes red.
    fx.reset();
    check_delta(fx, 0, 0, 100, 0, 100, 0, "delta D5 X boundary d==half (no fold) .dx",
                "delta D5 .dy (unused)");

    // ---- D6: X boundary d == half (100) from the start>end branch -- same guard, other sign.
    // x1=100,x2=0: sign=-1,d=100. result=-100.
    fx.reset();
    check_delta(fx, 100, 0, 0, 0, -100, 0, "delta D6 X boundary d==half (no fold) .dx",
                "delta D6 .dy (unused)");

    // ---- D7: X fold at d==half+1 (101), confirms the fold DOES trigger just past the boundary.
    // x1=0,x2=101: sign=+1,d=101. half=100<101 -> fold: d=200-101=99, sign=-1. result=-99.
    fx.reset();
    check_delta(fx, 0, 0, 101, 0, -99, 0, "delta D7 X fold at half+1 .dx", "delta D7 .dy (unused)");

    // ---- D8: Y axis, end>start branch, no fold (other dimension: height=120, half=60 -- also
    // catches an axis-swapped `dim` argument). y1=10,y2=50: d=40<=60. result=+40.
    fx.reset();
    check_delta(fx, 0, 10, 0, 50, 0, 40, "delta D8 .dx (unused)", "delta D8 Y end>start no-fold .dy");

    // ---- D9: Y axis fold, start>end branch. y1=90,y2=10: sign=-1,d=80. half=60<80 -> fold:
    // d=120-80=40, sign flips to +1. result=+40.
    fx.reset();
    check_delta(fx, 0, 90, 0, 10, 0, 40, "delta D9 .dx (unused)", "delta D9 Y start>end fold .dy");

    // ---- D10: Y boundary d == half (60) -- must NOT fold. y1=0,y2=60: sign=+1,d=60. result=+60.
    fx.reset();
    check_delta(fx, 0, 0, 0, 60, 0, 60, "delta D10 .dx (unused)",
                "delta D10 Y boundary d==half (no fold) .dy");
}

} // namespace mh::sim::test
