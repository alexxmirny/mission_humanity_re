//
// sim_bldg_placement_corner_by_type_selftest.cpp -- `simtest` cases for
// llm_bldg_calc_placement_corner_from_center_by_type (sim/sim_bldg_placement_corner.h/.cpp),
// SIM1-G4. ONLY possible oracle: the shadow site is VACUOUS (writes only through its two OUT-pointer
// params, no return value, no compared region -- the header's own banner, same posture as its
// near-namesake sibling llm_bldg_calc_placement_corner_from_center).
//
// A SEPARATE FILE FROM THE SIBLING'S OWN TEST (sim_bldg_placement_corner_selftest.cpp) ON PURPOSE:
// that file tests a DIFFERENT original function (llm_bldg_calc_placement_corner_from_center
// @0x0048d054, implemented in sim_bldg_placement_preview.cpp) -- same shape, wrong body if merged.
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_bldg_calc_placement_corner_from_center_by_type_0048d0ea.asm), not read off the
// .cpp -- see sim_bldg_placement_corner.h's banner for the full derivation. Two REAL differences from
// the sibling, both pinned explicitly below:
//   (1) ADD, not SUB: *out_x = (center_x + half_w) & width_mask (0x0048d126/0x0048d132/0x0048d134),
//       *out_y = (center_y + half_h) & height_mask (0x0048d154/0x0048d15e/0x0048d160).
//   (2) DIRECT INDEX: `building_type` indexes cfg_buildings DIRECTLY (0x0048d10b-0x0048d115 /
//       0x0048d136-0x0048d140, IMUL on the raw param) -- no cfg_units[...].equivalent hop.
//   width  = cfg_buildings[building_type].width  (byte @ struct offset 0xa,  read at 0x0048d115)
//   height = cfg_buildings[building_type].height (byte @ struct offset 0x9,  read at 0x0048d140,
//            ONE LESS than width's -- adjacent fields, easy to swap in a translation)
//   half_w = truncating (width-1)/2  (SAR-with-sign-correction idiom, 0x0048d11c-0x0048d124)
//   half_h = truncating (height-1)/2                                        (0x0048d147-0x0048d14f)
//
#include "sim/sim_bldg_placement_corner.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

void check_corner_by_type(sim_fixture &fx, uint16_t building_type, int32_t center_x, int32_t center_y,
                          uint32_t want_x, uint32_t want_y, const char *what_x, const char *what_y) {
    uint32_t out_x = 0x0badf00d, out_y = 0x0badf00d;
    detail::bldg_calc_placement_corner_from_center_by_type(fx.view(), building_type, center_x, center_y,
                                                           &out_x, &out_y);
    ck_eq(out_x, want_x, what_x);
    ck_eq(out_y, want_y, what_y);
}

} // namespace

void run_bldg_placement_corner_by_type_tests() {
    sim_fixture fx;

    // ---- T1: EVEN width/height + ADD-not-SUB pin, chosen so ADD and SUB give CLEARLY different,
    // non-wrapping results (no mask collision to hide the operator). width=8 -> half_w=(8-1)/2=3
    // (trunc). height=6 -> half_h=(6-1)/2=2. If the function used SUB (like the sibling), out_x would
    // be 147 (not 153) and out_y would be 58 (not 62). --------------------------------------------
    fx.reset();
    fx.geom.width_mask         = 0xff;
    fx.geom.height_mask        = 0x3f;
    fx.cfg_buildings[9].width  = 8;
    fx.cfg_buildings[9].height = 6;
    // out_x = (150 + 3) & 0xff = 153; out_y = (60 + 2) & 0x3f = 62.
    check_corner_by_type(fx, 9, 150, 60, 153, 62,
                         "corner_by_type T1: out_x = (center_x + (width-1)/2) & width_mask, ADD not "
                         "SUB (SUB would give 147), 0x0048d126",
                         "corner_by_type T1: out_y = (center_y + (height-1)/2) & height_mask, ADD not "
                         "SUB (SUB would give 58), 0x0048d154");

    // ---- T2: ODD width/height, so (dim-1) is EVEN and the truncation is exact (no rounding to hide a
    // sign-handling bug), reconfirming ADD with fresh, non-symmetric values. width=7 -> half_w=3;
    // height=5 -> half_h=2. If SUB were used, out_x would be 47 (not 53) and out_y would be 4 (not 8).
    // ---------------------------------------------------------------------------------------------
    fx.reset();
    fx.geom.width_mask          = 0xff;
    fx.geom.height_mask         = 0x3f;
    fx.cfg_buildings[11].width  = 7;
    fx.cfg_buildings[11].height = 5;
    // out_x = (50 + 3) & 0xff = 53; out_y = (70 + 2) & 0x3f = 72 & 0x3f = 8.
    check_corner_by_type(fx, 11, 50, 70, 53, 8,
                         "corner_by_type T2: odd width truncating-halve, ADD not SUB (SUB would give "
                         "47), 0x0048d11c-0x0048d126",
                         "corner_by_type T2: odd height truncating-halve, ADD not SUB (SUB would give "
                         "4), 0x0048d147-0x0048d154");

    // ---- T3: width=0/height=0 boundary -- (dim-1) is -1, and the DEC/SAR/SUB/SAR idiom must
    // reproduce TRUNCATING division (round toward zero), not the arithmetic-shift FLOOR: (-1)/2
    // truncates to 0, but a plain SAR(-1,1) would give -1 (floor). Catches a translation that used
    // `>> 1` instead of `/ 2` on the sign-extension path -- if it had, out_x would be 19 (not 20) and
    // out_y would be 29 (not 30). ---------------------------------------------------------------
    fx.reset();
    fx.geom.width_mask         = 0xff;
    fx.geom.height_mask        = 0x3f;
    fx.cfg_buildings[4].width  = 0;
    fx.cfg_buildings[4].height = 0;
    // half_w = (0-1)/2 = -1/2 = 0 (trunc, not -1). out_x = (20 + 0) & 0xff = 20; out_y = 30.
    check_corner_by_type(fx, 4, 20, 30, 20, 30,
                         "corner_by_type T3: width==0, (dim-1)=-1 truncating-div not floor (SAR "
                         "sign-correction), 0x0048d11c-0x0048d124",
                         "corner_by_type T3: height==0, (dim-1)=-1 truncating-div not floor, "
                         "0x0048d147-0x0048d14f");

    // ---- T4: the mask actually WRAPS (center chosen near the top of narrow, DISTINCT width_mask/
    // height_mask), proving AND-after-ADD (0x0048d132/0x0048d15e) and that the two masks are read
    // independently -- a swapped-axis translation would produce a visibly different wrapped value
    // since width_mask (0x1f) != height_mask (0x0f). If SUB were used instead of ADD, out_x would be
    // 26 (not 2) and out_y would be 14 (not 0) -- this case ALSO reconfirms the ADD pin under wrap. --
    fx.reset();
    fx.geom.width_mask         = 0x1f; // 32-wide wrap
    fx.geom.height_mask        = 0x0f; // 16-wide wrap
    fx.cfg_buildings[6].width  = 10;   // half_w = (10-1)/2 = 4
    fx.cfg_buildings[6].height = 4;    // half_h = (4-1)/2 = 1
    // out_x = (30 + 4) & 0x1f = 34 & 0x1f = 2 (wraps). out_y = (15 + 1) & 0x0f = 16 & 0x0f = 0 (wraps).
    check_corner_by_type(fx, 6, 30, 15, 2, 0,
                         "corner_by_type T4: positive-wrap width_mask, AND-after-ADD, ADD not SUB "
                         "(SUB would give 26), 0x0048d129-0x0048d134",
                         "corner_by_type T4: positive-wrap height_mask, AND-after-ADD, ADD not SUB "
                         "(SUB would give 14), 0x0048d156-0x0048d160");

    // ---- T5: DIRECT INDEX pin -- building_type indexes cfg_buildings DIRECTLY (IMUL on the raw
    // param, 0x0048d10b/0x0048d136), with NO cfg_units[...].equivalent hop (unlike the sibling
    // llm_bldg_calc_placement_corner_from_center). building_type=7's OWN cfg_buildings entry
    // (width=20, height=12) differs from what a wrong "resolve via cfg_units[7].equivalent" path
    // would read: cfg_units[7].equivalent is seeded to a DIFFERENT slot (40) whose cfg_buildings
    // entry (width=99, height=88) is a decoy that must NOT be read. A translation that mistakenly
    // added the .equivalent indirection would compute half_w=(99-1)/2=49 -> out_x=149 (not 109) and
    // half_h=(88-1)/2=43 -> out_y=143 (not 105). -------------------------------------------------
    fx.reset();
    fx.geom.width_mask          = 0xff;
    fx.geom.height_mask         = 0xff;
    fx.cfg_buildings[7].width   = 20; // half_w = (20-1)/2 = 9 -- the value that MUST be read
    fx.cfg_buildings[7].height  = 12; // half_h = (12-1)/2 = 5
    fx.cfg_units[7].equivalent  = 40; // decoy indirection target -- must NOT be followed
    fx.cfg_buildings[40].width  = 99; // decoy values -- reading these means the .equivalent hop leaked in
    fx.cfg_buildings[40].height = 88;
    // out_x = (100 + 9) & 0xff = 109; out_y = (100 + 5) & 0xff = 105.
    check_corner_by_type(
        fx, 7, 100, 100, 109, 105,
        "corner_by_type T5: building_type indexes cfg_buildings DIRECTLY, no cfg_units[...].equivalent "
        "hop (decoy via slot 40 would give 149), 0x0048d10b-0x0048d115",
        "corner_by_type T5: building_type indexes cfg_buildings DIRECTLY for height too (decoy via "
        "slot 40 would give 143), 0x0048d136-0x0048d140");
}

} // namespace mh::sim::test
