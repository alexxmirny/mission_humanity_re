//
// sim_tile_pixel_wrap_delta_selftest.cpp -- `simtest` cases for llm_strat_pixel_delta_wrapped
// (@0x004944f6) and llm_strat_map_wrapped_delta (@0x004945de), both in sim/sim_tile_pixel_wrap_delta.h
// /.cpp (RI-SIM / SIM1F). Byte-for-byte the same control flow over the SAME two boot
// fields sim_view::geom->big_width/big_height -- the only difference is the out-parameter width
// (int32_t* vs double*, the latter an exact int->double widen). Neither has a callee beyond the
// inert stack-probe prologue and neither reads/writes any other sim state, so no `_calls` struct
// for either.
//
// EXPECTED VALUES ARE HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_strat_pixel_delta_wrapped_004944f6.asm,
// tmp/decomp/llm_strat_map_wrapped_delta_004945de.asm), not read off the .cpp. Per axis (X shown,
// Y is the identical shape over big_height):
//   if (x2 < x1) { sign = -1; delta = x1 - x2; }             (0x00494517/1a, CMP/JL, signed)
//   else         { sign = +1; delta = x2 - x1; }
//   big_width = v.geom->big_width                             (0x00494541, always a positive extent)
//   if (big_width / 2 < delta) { delta = big_width - delta; sign = -sign; }   (0x00494553/56)
//   *out = sign * delta;   (int store for pixel_delta_wrapped, 0x00494571-576; FILD/FSTP widen to
//                           double for map_wrapped_delta, 0x0049465f-664 -- exact, no precision loss)
// `delta` is always >= 0 by construction (the sign/magnitude split above), so `big_width / 2` here is
// a threshold on a NONNEGATIVE value -- C's truncating `/2` and an arithmetic right shift agree for
// any nonnegative dividend, so (unlike sim_geom_toroidal.h's tile_midpoint_wrapped) there is no
// SAR-vs-`/2` rounding hazard to probe in either function; the boundary/fold coverage below is what
// matters.
//
// The fixture's geom.big_width=1000 (half=500), geom.big_height=600 (half=300) -- deliberately
// distinct and unrelated to map_width/map_height (200/120) or to each other/their halves, so an
// axis-swapped or field-swapped (map_width instead of geom->big_width) translation disagrees with
// every case here. reset() zeroes `geom`, so every case below sets big_width/big_height explicitly.
//
#include "sim/sim_tile_pixel_wrap_delta.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr int32_t kBigWidth  = 1000; // half = 500
constexpr int32_t kBigHeight = 600;  // half = 300

void seed_geom(sim_fixture &fx) {
    fx.reset();
    fx.geom.big_width  = (uint32_t)kBigWidth;
    fx.geom.big_height = (uint32_t)kBigHeight;
}

void check_pixel(sim_fixture &fx, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t want_dx,
                 int32_t want_dy, const char *what_dx, const char *what_dy) {
    int32_t odx = 0x0badf00d, ody = 0x0badf00d;
    detail::pixel_delta_wrapped(fx.view(), x1, y1, x2, y2, &odx, &ody);
    ck_eq((uint32_t)odx, (uint32_t)want_dx, what_dx);
    ck_eq((uint32_t)ody, (uint32_t)want_dy, what_dy);
}

void check_map(sim_fixture &fx, int32_t x1, int32_t y1, int32_t x2, int32_t y2, double want_dx,
               double want_dy, const char *what_dx, const char *what_dy) {
    double odx = -12345.0, ody = -12345.0;
    detail::map_wrapped_delta(fx.view(), x1, y1, x2, y2, &odx, &ody);
    ck_eq_d(odx, want_dx, what_dx);
    ck_eq_d(ody, want_dy, what_dy);
}

} // namespace

void run_pixel_delta_wrapped_tests() {
    sim_fixture fx;

    // ---- E1: X axis, x2>=x1 branch (sign=+1), no fold. x1=10,x2=200: delta=190<=500. result=+190.
    seed_geom(fx);
    check_pixel(fx, 10, 0, 200, 0, 190, 0, "pixel E1 X x2>=x1 no-fold .dx", "pixel E1 .dy (unused)");

    // ---- E2: X axis, x2<x1 branch (sign=-1), no fold. x1=300,x2=10: delta=290<=500. result=-290.
    // MUTATION: swapping the sign chosen by the two branches flips E1's +190/E2's -290 to the wrong
    // sign -- the pair pins both.
    seed_geom(fx);
    check_pixel(fx, 300, 0, 10, 0, -290, 0, "pixel E2 X x2<x1 no-fold .dx", "pixel E2 .dy (unused)");

    // ---- E3: X axis fold, x2>=x1 branch. x1=0,x2=700: sign=+1,delta=700. half=500<700 -> fold:
    // delta=1000-700=300, sign flips to -1. result=-300.
    seed_geom(fx);
    check_pixel(fx, 0, 0, 700, 0, -300, 0, "pixel E3 X x2>=x1 fold .dx", "pixel E3 .dy (unused)");

    // ---- E4: X axis fold, x2<x1 branch. x1=700,x2=0: sign=-1,delta=700. half=500<700 -> fold:
    // delta=300, sign flips to +1. result=+300.
    // MUTATION: dropping the `sign = -sign` flip on fold leaves either E3 or E4 with the pre-fold
    // sign -- caught by whichever branch's expected sign actually flipped.
    seed_geom(fx);
    check_pixel(fx, 700, 0, 0, 0, 300, 0, "pixel E4 X x2<x1 fold .dx", "pixel E4 .dy (unused)");

    // ---- E5: X boundary delta == half (500), x2>=x1 branch -- guard is `half < delta` (strict), so
    // an EXACT 500 must NOT fold. x1=0,x2=500: sign=+1,delta=500. result=+500.
    // MUTATION: `half <= delta` here would fold (delta=500,sign=-1 -> result -500) -- goes red.
    seed_geom(fx);
    check_pixel(fx, 0, 0, 500, 0, 500, 0, "pixel E5 X boundary delta==half (no fold) .dx",
                "pixel E5 .dy (unused)");

    // ---- E6: X boundary delta == half (500), x2<x1 branch. x1=500,x2=0: sign=-1,delta=500.
    // result=-500.
    seed_geom(fx);
    check_pixel(fx, 500, 0, 0, 0, -500, 0, "pixel E6 X boundary delta==half (no fold) .dx",
                "pixel E6 .dy (unused)");

    // ---- E7: X fold at delta==half+1 (501), confirms the fold DOES trigger just past the boundary.
    // x1=0,x2=501: sign=+1,delta=501. half=500<501 -> fold: delta=1000-501=499, sign=-1.
    // result=-499.
    seed_geom(fx);
    check_pixel(fx, 0, 0, 501, 0, -499, 0, "pixel E7 X fold at half+1 .dx", "pixel E7 .dy (unused)");

    // ---- E8: Y axis, y2>=y1 branch, no fold (other field: big_height=600, half=300 -- also catches
    // an axis-swapped `big_width`/`big_height` read). y1=10,y2=200: delta=190<=300. result=+190.
    seed_geom(fx);
    check_pixel(fx, 0, 10, 0, 200, 0, 190, "pixel E8 .dx (unused)", "pixel E8 Y y2>=y1 no-fold .dy");

    // ---- E9: Y axis fold, y2<y1 branch. y1=400,y2=10: sign=-1,delta=390. half=300<390 -> fold:
    // delta=600-390=210, sign flips to +1. result=+210.
    seed_geom(fx);
    check_pixel(fx, 0, 400, 0, 10, 0, 210, "pixel E9 .dx (unused)", "pixel E9 Y y2<y1 fold .dy");

    // ---- E10: Y boundary delta == half (300) -- must NOT fold. y1=0,y2=300: sign=+1,delta=300.
    // result=+300.
    seed_geom(fx);
    check_pixel(fx, 0, 0, 0, 300, 0, 300, "pixel E10 .dx (unused)",
                "pixel E10 Y boundary delta==half (no fold) .dy");
}

void run_map_wrapped_delta_tests() {
    sim_fixture fx;
    // Same control flow as pixel_delta_wrapped (see run_pixel_delta_wrapped_tests above for the
    // per-case derivations); only the store differs (int->double widen, exact for these magnitudes).

    // ---- M1: X axis, x2>=x1 branch, no fold. ----
    seed_geom(fx);
    check_map(fx, 10, 0, 200, 0, 190.0, 0.0, "map M1 X x2>=x1 no-fold .dx", "map M1 .dy (unused)");

    // ---- M2: X axis, x2<x1 branch, no fold. ----
    seed_geom(fx);
    check_map(fx, 300, 0, 10, 0, -290.0, 0.0, "map M2 X x2<x1 no-fold .dx", "map M2 .dy (unused)");

    // ---- M3: X axis fold, x2>=x1 branch. ----
    seed_geom(fx);
    check_map(fx, 0, 0, 700, 0, -300.0, 0.0, "map M3 X x2>=x1 fold .dx", "map M3 .dy (unused)");

    // ---- M4: X axis fold, x2<x1 branch. ----
    // MUTATION: same sign-flip-on-fold mutation as pixel E3/E4 -- caught the same way here.
    seed_geom(fx);
    check_map(fx, 700, 0, 0, 0, 300.0, 0.0, "map M4 X x2<x1 fold .dx", "map M4 .dy (unused)");

    // ---- M5: X boundary delta == half (500) -- must NOT fold (strict `<`). ----
    seed_geom(fx);
    check_map(fx, 0, 0, 500, 0, 500.0, 0.0, "map M5 X boundary delta==half (no fold) .dx",
              "map M5 .dy (unused)");

    // ---- M6: X boundary delta == half (500), other branch. ----
    seed_geom(fx);
    check_map(fx, 500, 0, 0, 0, -500.0, 0.0, "map M6 X boundary delta==half (no fold) .dx",
              "map M6 .dy (unused)");

    // ---- M7: X fold at delta==half+1 (501). ----
    seed_geom(fx);
    check_map(fx, 0, 0, 501, 0, -499.0, 0.0, "map M7 X fold at half+1 .dx", "map M7 .dy (unused)");

    // ---- M8: Y axis, no fold (other field, big_height=600 -- also catches an axis-swapped read). --
    seed_geom(fx);
    check_map(fx, 0, 10, 0, 200, 0.0, 190.0, "map M8 .dx (unused)", "map M8 Y y2>=y1 no-fold .dy");

    // ---- M9: Y axis fold. ----
    seed_geom(fx);
    check_map(fx, 0, 400, 0, 10, 0.0, 210.0, "map M9 .dx (unused)", "map M9 Y y2<y1 fold .dy");

    // ---- M10: Y boundary delta == half (300) -- must NOT fold. ----
    // MUTATION: dropping the `(double)` widen in favour of e.g. truncating through an int
    // intermediate would not show up at these magnitudes (all values are exactly representable and
    // already integral), but a WRONG axis field (using big_width's 1000/half=500 here instead of
    // big_height's 600/half=300) WOULD show up: with half=500 this delta(300) still doesn't fold, so
    // that particular swap needs M9 (delta=390, folds under half=300 but not under half=500) to catch
    // it -- confirm M9's expected value (+210, i.e. a fold happened) is the one that goes red.
    seed_geom(fx);
    check_map(fx, 0, 0, 0, 300, 0.0, 300.0, "map M10 .dx (unused)",
              "map M10 Y boundary delta==half (no fold) .dy");
}

} // namespace mh::sim::test
