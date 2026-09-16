//
// sim_bldg_placement_corner_selftest.cpp -- `simtest` cases for llm_bldg_calc_placement_corner_from_center
// (sim/sim_bldg_placement_preview.h/.cpp), SIM1B. ONLY possible oracle: the shadow site is VACUOUS
// (writes only through its two OUT-pointer params, no return value, no compared region -- the header's
// own banner; 1 call armed / 0 compared in the placement/roster-queries slice's soak).
//
// PURE FUNCTION OF cfg_units[unit_index].equivalent -> cfg_buildings[..].width/height and the geom
// masks (v.geom->width_mask/height_mask via map_width_mask()/map_height_mask() -- NOT width_m/height_m,
// confirmed against the .asm below: the reads are of _G's general.width_mask/height_mask at
// 0x00e15398/0x00e153b0, the SAME globals map_width_mask()/map_height_mask() bind, distinct from the
// width_m/height_m pair sim_bldg_placement_enclosure.cpp's functions read).
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_bldg_calc_placement_corner_from_center_0048d054.asm), not read off the .cpp:
//   equivalent = cfg_units[unit_index].equivalent            (0x0048d07f/0x0048d0b5, dword)
//   width      = cfg_buildings[equivalent].width  (byte @ offset 0xd9ec8a in the struct)
//   height     = cfg_buildings[equivalent].height (byte @ offset 0xd9ec89, ONE LESS than width's --
//                adjacent fields, easy to swap in a translation)
//   half_w = SAR-with-sign-correction((width-1))  == truncating (width-1)/2  (0x0048d090-0x0048d098)
//   half_h = truncating (height-1)/2                                        (0x0048d0c6-0x0048d0ce)
//   *out_col = (center_x - half_w) & width_mask   (SUB then AND, in that order -- 0x0048d09d/a7)
//   *out_row = (center_y - half_h) & height_mask  (SUB then AND, in that order -- 0x0048d0d3/dd)
//
#include "sim/sim_bldg_placement_preview.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

void check_corner(sim_fixture &fx, uint16_t unit_index, int32_t center_x, int32_t center_y,
                  uint32_t want_col, uint32_t want_row, const char *what_col, const char *what_row) {
    uint32_t col = 0x0badf00d, row = 0x0badf00d;
    detail::bldg_calc_placement_corner_from_center(fx.view(), unit_index, center_x, center_y, &col,
                                                   &row);
    ck_eq(col, want_col, what_col);
    ck_eq(row, want_row, what_row);
}

} // namespace

void run_bldg_placement_corner_tests() {
    sim_fixture fx;

    // ---- S1: EVEN width/height (the (dim-1)/2 truncation matters -- 8x6 footprint, not 9x7). ------
    // width=8 -> half_w = (8-1)/2 = 3 (trunc). height=6 -> half_h = (6-1)/2 = 2 (trunc).
    // Distinct width_mask/height_mask (0xff/0x3f) AND center_x chosen ABOVE the height_mask (147 >
    // 0x3f) so a swapped-axis translation (col computed with height_mask instead of width_mask)
    // disagrees on ITS OWN, independent of S5's confirmation below -- mutation-tested 2026-08-12
    // (dropping this margin let the swap slip through undetected here).
    fx.reset();
    fx.geom.width_mask         = 0xff;
    fx.geom.height_mask        = 0x3f;
    fx.cfg_units[5].equivalent = 9;
    fx.cfg_buildings[9].width  = 8;
    fx.cfg_buildings[9].height = 6;
    // out_col = (150 - 3) & 0xff = 147; out_row = (60 - 2) & 0x3f = 58.
    check_corner(fx, 5, 150, 60, 147, 58, "corner S1 even w/h .col", "corner S1 even w/h .row");

    // ---- S2: ODD width/height, so (dim-1) is EVEN and the truncation is exact (no rounding to hide
    // a sign-handling bug). width=7 -> half_w=3; height=5 -> half_h=2. -----------------------------
    fx.reset();
    fx.geom.width_mask         = 0xff;
    fx.geom.height_mask        = 0x3f;
    fx.cfg_units[5].equivalent = 9;
    fx.cfg_buildings[9].width  = 7;
    fx.cfg_buildings[9].height = 5;
    check_corner(fx, 5, 50, 60, 47, 58, "corner S2 odd w/h .col", "corner S2 odd w/h .row");

    // ---- S3: width=1 (half_w = (1-1)/2 = 0, the degenerate case: corner == center on that axis). --
    fx.reset();
    fx.geom.width_mask         = 0xff;
    fx.geom.height_mask        = 0x3f;
    fx.cfg_units[2].equivalent = 4;
    fx.cfg_buildings[4].width  = 1;
    fx.cfg_buildings[4].height = 1;
    check_corner(fx, 2, 20, 30, 20, 30, "corner S3 width==1 degenerate .col",
                 "corner S3 width==1 degenerate .row");

    // ---- S4: width=0 -- (0-1) is -1, and the SAR/SUB/SAR idiom must reproduce TRUNCATING division,
    // not the arithmetic-shift floor: (-1)/2 truncates to 0, but a plain SAR(-1,1) would give -1
    // (floor). This is the case that catches a translation using `>> 1` instead of `/ 2`. -----------
    fx.reset();
    fx.geom.width_mask         = 0xff;
    fx.geom.height_mask        = 0x3f;
    fx.cfg_units[2].equivalent = 4;
    fx.cfg_buildings[4].width  = 0;
    fx.cfg_buildings[4].height = 0;
    // half_w = (0-1)/2 = -1/2 = 0 (trunc, not -1). out_col = (20 - 0) & 0xff = 20.
    check_corner(fx, 2, 20, 30, 20, 30, "corner S4 width==0 truncating-div (not floor) .col",
                 "corner S4 width==0 truncating-div (not floor) .row");

    // ---- S5: the mask actually wraps (negative center - half), proving AND-after-SUB (not the
    // other order) and that the two masks are read independently (width_mask=0x1f narrow here, so a
    // swapped-axis translation would produce a visibly different wrapped value). -------------------
    fx.reset();
    fx.geom.width_mask         = 0x1f; // 32-wide wrap
    fx.geom.height_mask        = 0x0f; // 16-wide wrap
    fx.cfg_units[3].equivalent = 6;
    fx.cfg_buildings[6].width  = 10; // half_w = 9/2 = 4
    fx.cfg_buildings[6].height = 4;  // half_h = 3/2 = 1
    // out_col = (2 - 4) & 0x1f = (-2) & 0x1f = 30. out_row = (1 - 1) & 0x0f = 0.
    check_corner(fx, 3, 2, 1, 30, 0, "corner S5 negative-wrap width_mask .col",
                 "corner S5 negative-wrap width_mask .row");

    // ---- S6: unit_index and its `.equivalent` are DIFFERENT indices -- if a translation read
    // cfg_buildings[unit_index] directly (skipping the indirection through cfg_units[unit_index]
    // .equivalent), this would read building slot 7's zeroed width/height instead of slot 40's. -----
    fx.reset();
    fx.geom.width_mask          = 0xff;
    fx.geom.height_mask         = 0xff;
    fx.cfg_units[7].equivalent  = 40; // NOT 7 -- the indirection is the point
    fx.cfg_buildings[40].width  = 20;
    fx.cfg_buildings[40].height = 12;
    // half_w = 19/2 = 9; half_h = 11/2 = 5.
    check_corner(fx, 7, 100, 100, 91, 95,
                 "corner S6 unit_index != equivalent (indirection through cfg_units) .col",
                 "corner S6 unit_index != equivalent (indirection through cfg_units) .row");
}

} // namespace mh::sim::test
