//
// sim_geom_toroidal_selftest.cpp -- `simtest` case for llm_strat_tile_midpoint_wrapped
// (sim/sim_geom_toroidal.h/.cpp @0x00669fe8, RI-SIM / SIM1F). Pure function of the map's
// width/height (sim_view::map_width/map_height) plus its own four stack args -- no outward calls,
// no `_calls` struct.
//
// EXPECTED VALUES ARE HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_strat_tile_midpoint_wrapped_00669fe8.asm), not read off the .cpp. Per axis
// (X shown, Y is the identical shape over height):
//   half_w = width >> 1                                  (0x00669ffa, SHR -- width always positive)
//   dx = x1 - x0                                          (0x00669ffc)
//   if (dx > half_w)  dx -= width                         (0x00669fff/0x0066a001, CMP/JLE, signed)
//   if (dx < -half_w) dx += width                         (0x0066a00b/0x0066a00d, NEG then CMP/JGE)
//   numerator = (dx >> 1) + x0 + width                    (0x0066a015-0x0066a01c, SAR -- NOT `/2`:
//       differs from C's truncating `/2` whenever dx is negative and odd, hazard #8)
//   out_x = (uint32_t)numerator % (uint32_t)width         (0x0066a01e-0x0066a023, unsigned DIV)
//
// The fixture's map_width=200, map_height=120 (sim_test_support.h's reset(), deliberately distinct
// and non-power-of-two-friendly) -- an axis-swapped translation disagrees with every case here.
//
#include "sim/sim_geom_toroidal.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

void check_midpoint(sim_fixture &fx, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t want_x,
                    int32_t want_y, const char *what_x, const char *what_y) {
    int32_t ox = 0x0badf00d, oy = 0x0badf00d;
    detail::tile_midpoint_wrapped(fx.view(), x0, y0, x1, y1, &ox, &oy);
    ck_eq((uint32_t)ox, (uint32_t)want_x, what_x);
    ck_eq((uint32_t)oy, (uint32_t)want_y, what_y);
}

} // namespace

void run_tile_midpoint_wrapped_tests() {
    sim_fixture fx;
    // width=200 (half_w=100), height=120 (half_h=60), both set by fx.reset().

    // ---- C1: direct average, no wrap fold on either axis (sanity baseline). ----
    // X: dx=50-10=40 (<=100,>=-100). numerator=(40>>1)+10+200=230, %200=30.
    // Y: dy=80-20=60 (==half_h, boundary -- see C10 below for the other axis's own boundary).
    //    numerator=(60>>1)+20+120=170, %120=50.
    fx.reset();
    check_midpoint(fx, 10, 20, 50, 80, 30, 50, "midpoint C1 direct X", "midpoint C1 direct Y");

    // ---- C2: X positive-direction wrap fold (dx > half_w=100 triggers dx -= width). ----
    // dx=101-0=101>100 -> dx=101-200=-99 (odd, negative -- exercises SAR at the same time).
    // numerator=(-99>>1)+0+200 = -50+0+200=150 (SAR: floor(-99/2)=-50). %200=150.
    // MUTATION: `>=` instead of `>` at the fold guard does not affect this case (101>100 either way)
    // but WOULD flip C4 below -- kept together so both sides of that guard are covered.
    fx.reset();
    check_midpoint(fx, 0, 0, 101, 0, 150, 0, "midpoint C2 X positive-wrap fold .x", "midpoint C2 .y (unused axis)");

    // ---- C3: X negative-direction wrap fold (dx < -half_w=-100 triggers dx += width). ----
    // dx=0-101=-101 < -100 -> dx=-101+200=99 (odd, positive).
    // numerator=(99>>1)+101+200 = 49+101+200=350 (SAR on positive is plain floor==49). %200=150.
    fx.reset();
    check_midpoint(fx, 101, 0, 0, 0, 150, 0, "midpoint C3 X negative-wrap fold .x", "midpoint C3 .y (unused axis)");

    // ---- C4: X boundary dx == +half_w (100) -- the JLE guard is `dx > half_w`, so an EXACT 100 must
    // NOT fold. dx=100-0=100. numerator=(100>>1)+0+200=250, %200=50.
    // MUTATION: `>=` here would fold (dx-=200 -> -100), producing a different numerator/result --
    // this case goes red under that mutation.
    fx.reset();
    check_midpoint(fx, 0, 0, 100, 0, 50, 0, "midpoint C4 X boundary dx==+half_w (no fold) .x",
                   "midpoint C4 .y (unused axis)");

    // ---- C5: X boundary dx == -half_w (-100) -- the JGE guard is `dx < -half_w`, so an EXACT -100
    // must NOT fold. dx=0-100=-100. numerator=(-100>>1)+100+200 = -50+100+200=250, %200=50.
    fx.reset();
    check_midpoint(fx, 100, 0, 0, 0, 50, 0, "midpoint C5 X boundary dx==-half_w (no fold) .x",
                   "midpoint C5 .y (unused axis)");

    // ---- C6: X SAR-vs-truncating-`/2` hazard, negative AND odd, no wrap fold involved. ----
    // dx=29-50=-21 (within [-100,100], no fold). numerator=(-21>>1)+50+200.
    //   SAR (correct): floor(-21/2) = -11  -> numerator=239 -> %200=39.
    //   Truncating `/2` (WRONG, what a `dx/2` mistranslation would produce): -21/2 = -10 (toward
    //   zero) -> numerator=240 -> %200=40. A `dx>>1`-to-`dx/2` mutation flips this check's expected
    //   39 to the wrong 40 -- this is the case that catches it.
    fx.reset();
    check_midpoint(fx, 50, 0, 29, 0, 39, 0, "midpoint C6 X SAR-vs-/2 hazard (neg odd) .x",
                   "midpoint C6 .y (unused axis)");

    // ---- C7: Y SAR-vs-truncating-`/2` hazard (same hazard, other axis / other dimension: height=120
    // vs width=200, so this also catches an axis-swapped divisor). ----
    // dy=25-40=-15 (within [-60,60], no fold). numerator=(-15>>1)+40+120.
    //   SAR (correct): floor(-15/2) = -8 -> numerator=152 -> %120=32.
    //   Truncating `/2` (WRONG): -15/2 = -7 -> numerator=153 -> %120=33.
    fx.reset();
    check_midpoint(fx, 0, 40, 0, 25, 0, 32, "midpoint C7 .x (unused axis)",
                   "midpoint C7 Y SAR-vs-/2 hazard (neg odd) .y");

    // ---- C8: Y positive-direction wrap fold (dy > half_h=60). ----
    // dy=61-0=61>60 -> dy=61-120=-59 (odd, negative). numerator=(-59>>1)+0+120 = -30+0+120=90
    // (SAR: floor(-59/2)=-30). %120=90.
    fx.reset();
    check_midpoint(fx, 0, 0, 0, 61, 0, 90, "midpoint C8 .x (unused axis)",
                   "midpoint C8 Y positive-wrap fold .y");

    // ---- C9: Y negative-direction wrap fold (dy < -half_h=-60). ----
    // dy=0-61=-61 < -60 -> dy=-61+120=59 (odd, positive). numerator=(59>>1)+61+120=29+61+120=210.
    // %120=90.
    fx.reset();
    check_midpoint(fx, 0, 61, 0, 0, 0, 90, "midpoint C9 .x (unused axis)",
                   "midpoint C9 Y negative-wrap fold .y");

    // ---- C10: Y boundary dy == +half_h (60) -- must NOT fold (strict `>`). dy=60-0=60.
    // numerator=(60>>1)+0+120=150, %120=30.
    fx.reset();
    check_midpoint(fx, 0, 0, 0, 60, 0, 30, "midpoint C10 .x (unused axis)",
                   "midpoint C10 Y boundary dy==+half_h (no fold) .y");

    // ---- C11: Y boundary dy == -half_h (-60) -- must NOT fold (strict `<`). dy=0-60=-60.
    // numerator=(-60>>1)+60+120 = -30+60+120=150, %120=30.
    fx.reset();
    check_midpoint(fx, 0, 60, 0, 0, 0, 30, "midpoint C11 .x (unused axis)",
                   "midpoint C11 Y boundary dy==-half_h (no fold) .y");
}

} // namespace mh::sim::test
