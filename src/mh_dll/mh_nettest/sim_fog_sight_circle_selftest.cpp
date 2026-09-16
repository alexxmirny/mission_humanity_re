//
// sim_fog_sight_circle_selftest.cpp -- `simtest` cases for the three offline-only members of the
// SIM1E fog/sight family (sim/sim_fog_of_war.h/.cpp):
//   map_fow_ConvertSightToArea    @0x004a6792 -> detail::convert_sight_to_area
//   llm_strat_sight_add_circle    @0x004968b6 -> detail::sight_add_circle
//   llm_strat_sight_remove_circle @0x0049694f -> detail::sight_remove_circle
//
// WHY THIS FILE EXISTS (no other oracle can see these three):
//   * sight_add_circle / sight_remove_circle have VACUOUS shadow sites -- 0 measured written
//     regions (they only call out to a same-batch sibling), and their return value is excluded from
//     the differential verdict (compare_return:false in the manifest) because it is provably dead
//     register-reuse noise -- see sim_fog_of_war.cpp's own [RETURN VALUE] banner on
//     llm_strat_sight_add_circle. Arming either on the rig proves literally nothing; this file is
//     their only possible evidence.
//   * convert_sight_to_area returns a `map_t_tile_coord *` IN EBP (sim_fog_of_war.h's [EBP-RETURN]
//     banner), which the marshalling layer cannot express -- it can never be exported or shadowed at
//     all, offline is the only oracle it will ever have.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY, NOT FROM THE .cpp UNDER TEST:
//   tmp/decomp/llm_strat_sight_add_circle_004968b6.asm     (0x99 bytes)
//   tmp/decomp/llm_strat_sight_remove_circle_0049694f.asm  (0x99 bytes, byte-identical shape)
//   tmp/decomp/map_fow_ConvertSightToArea_004a6792.asm     (0x84 bytes)
// Every expected constant/bound/index/call-arg below cites its instruction address inline; that
// citation is what makes this file auditable later.
//
// SCOPE (honest, not exhaustive):
//   COVERS -- the 10-way cascade (0x004a6797-0x004a6810) incl. BOTH fallthrough edges (0 and >=10)
//     and its BYTE-WIDTH-only comparison; the footprint walk's iteration order (outer counter/dx
//     pairs with origin_x/EDX/width_mask -- 0x00496905-0x0049693a add/remove -- inner counter/dy
//     pairs with origin_y/a2-EBX/height_mask) and its 10x10 INCLUSIVE bound (JL 0xa, i.e. i<10, both
//     loops); the `area[dx][dy]` index expression `dx*10+dy + building_id*0x842` (row=dx, not
//     swapped); the torus-wrap mask pair used is the GEOM one (general.width_mask/height_mask @
//     0x00e15398/0x00e153b0), confirmed NOT the OTHER width_m/height_m pair (a sibling
//     function in this same migration batch shipped a real bug by confusing exactly
//     these two mask pairs, so this file checks it explicitly); the exactly-one-call-per-nonzero-cell
//     / zero-calls-for-an-all-zero-footprint property; full outward-call argument tuples and call
//     ORDER via a shared trace; the building_id row selection (`* 0x842`).
//   DOES NOT COVER -- the dead return value (both functions are translated void here; nothing to
//     assert -- see the .cpp's own banner); map_fow_UpdateFoWPlus/_impl and
//     llm_strat_fow_remove_sight/_apply themselves (the callees, covered by their own oracle in this
//     batch -- this file stubs them as plain arg-recorders, never runs their real bodies).
//
#include "sim/sim_fog_of_war.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {
using namespace mh::sim;

// ---- sight_add_circle's one outward call: map_fow_UpdateFoWPlus(player, x, y, sight) -----------
// (0x0049693a-0x0049693d: EAX=player, EDX=x(width-masked), EBX=y(height-masked), ECX=sight, in that
// register order, matching sight_add_circle_calls::update_fow_plus's declared param order exactly.)
struct UpdateFowCall {
    uint32_t player, x, y;
    uint8_t  sight;
};
std::vector<UpdateFowCall> g_ufp_calls;
void                       rec_update_fow_plus(uint32_t player, uint32_t x, uint32_t y, uint8_t sight) {
    g_ufp_calls.push_back({player, x, y, sight});
}
const sight_add_circle_calls g_add_calls = {&rec_update_fow_plus};

// ---- sight_remove_circle's one outward call: llm_strat_fow_remove_sight(player, x, y, radius) ---
// (0x004969d3-0x004969d6: same EAX/EDX/EBX/ECX register order as the add-circle sibling.)
struct RemoveSightCall {
    uint32_t player;
    int32_t  x, y;
    uint8_t  radius;
};
std::vector<RemoveSightCall> g_rs_calls;
void                         rec_remove_sight(uint32_t player, int32_t x, int32_t y, uint8_t radius) {
    g_rs_calls.push_back({player, x, y, radius});
}
const sight_remove_circle_calls g_remove_calls = {&rec_remove_sight};

// ==== map_fow_ConvertSightToArea @0x004a6792 ======================================================

void test_convert_sight_to_area_cascade() {
    sim_fixture fx;
    // Distinguishable first entries: sight_area_N[0].x = N (N=1..10), so the returned pointer's
    // ->x field tells us WHICH of the ten tables came back. The footprint tests below exercise the
    // tables' actual {x,y,len} walk semantics; this test only needs table IDENTITY.
    fx.sight_area_1[0].x  = 1;
    fx.sight_area_2[0].x  = 2;
    fx.sight_area_3[0].x  = 3;
    fx.sight_area_4[0].x  = 4;
    fx.sight_area_5[0].x  = 5;
    fx.sight_area_6[0].x  = 6;
    fx.sight_area_7[0].x  = 7;
    fx.sight_area_8[0].x  = 8;
    fx.sight_area_9[0].x  = 9;
    fx.sight_area_10[0].x = 10;

    sim_view  v   = fx.view();
    sim_store own = fx.store();

    // sight==1..9: SIGHT_AREA_1..9 selected in strict order -- each rung's own CMP/JZ pair
    // (0x004a6797/67a5/67b3/67c1/67cf/67dd/67eb/67f9/6807, each JZ 0x004a6815) with the matching
    // `MOV EBP,SIGHT_AREA_N` loaded immediately before its own CMP.
    for (int32_t sight = 1; sight <= 9; ++sight) {
        own.g_tmp_sight()             = sight;
        const map_t_tile_coord *table = detail::convert_sight_to_area(v, own);
        ck_eq((uint32_t)table->x, (uint32_t)sight,
              "cascade: G_TMP_SIGHT==N (1..9) selects SIGHT_AREA_N, in ladder order (0x004a6797..6807)");
    }

    // sight==0 and sight>=10: EVERY OTHER VALUE falls through to SIGHT_AREA_10. Verified against the
    // raw listing: 0x004a6810's `MOV EBP,SIGHT_AREA_10` has NO guarding CMP before it anywhere -- it
    // is the unconditional last rung of the whole ladder (the CMP at 0x004a6807 tests ==9 and jumps
    // to 0x004a6815 on a match; on NO match, execution falls straight through 0x004a6810 with nothing
    // in between). So BOTH the low edge (0) and the high edge (>=10) must land here.
    for (int32_t sight : {0, 10, 11, 200, 255}) {
        own.g_tmp_sight()             = sight;
        const map_t_tile_coord *table = detail::convert_sight_to_area(v, own);
        ck_eq((uint32_t)table->x, 10u,
              "cascade: G_TMP_SIGHT==0 or >=10 falls through to SIGHT_AREA_10 (0x004a6810, unguarded MOV)");
    }

    // The comparison is BYTE-WIDTH only (`CMP byte ptr [G_TMP_SIGHT],N` at every rung, e.g.
    // 0x004a6807) -- a translation that compared the full 32-bit G_TMP_SIGHT would disagree here.
    // 0x105's low byte is 0x05, so this must select SIGHT_AREA_5, not fall through to AREA_10.
    own.g_tmp_sight() = 0x105;
    ck_eq((uint32_t)detail::convert_sight_to_area(v, own)->x, 5u,
          "cascade: G_TMP_SIGHT compared BYTE-WIDTH only -- 0x105's low byte (0x05) selects AREA_5, "
          "not a fallthrough (a 32-bit-wide compare would never match rung 5 here)");
}

// ==== llm_strat_sight_add_circle @0x004968b6 ======================================================

void test_add_all_zero_footprint_produces_zero_calls() {
    sim_fixture fx;
    g_ufp_calls.clear();
    fx.geom.width_mask  = 0xff;
    fx.geom.height_mask = 0xff;
    // cfg_buildings[3].area is all-zero straight out of fx.reset()'s memset -- confirm every one of
    // the 100 cells is skipped (0x00496915 CMP byte[...],0 / JZ 0x00496942, per cell).
    sim_view v = fx.view();
    detail::sight_add_circle(v, g_add_calls, /*player*/ 7, /*origin_x*/ 0, /*origin_y*/ 0,
                             /*building_id*/ 3, /*sight*/ 9);
    ck_eq((uint32_t)g_ufp_calls.size(), 0u,
          "add: all-zero 10x10 footprint -> exactly 0 outward calls (0x00496915 gate skips every cell)");
}

void test_add_single_cell_produces_exactly_one_call_with_full_arg_tuple() {
    sim_fixture fx;
    g_ufp_calls.clear();
    fx.geom.width_mask             = 0xff; // no wrap in range for this case
    fx.geom.height_mask            = 0xff;
    fx.cfg_buildings[3].area[4][6] = 1; // dx=4 (outer/row), dy=6 (inner/col) -- the only nonzero cell
    sim_view v                     = fx.view();

    detail::sight_add_circle(v, g_add_calls, 0x77u, 100, 200, 3, 0x5A);

    ck_eq((uint32_t)g_ufp_calls.size(), 1u,
          "add: exactly ONE nonzero cell -> exactly ONE outward call (per-cell gate at 0x00496915)");
    if (g_ufp_calls.size() == 1) {
        const auto &c = g_ufp_calls[0];
        ck_eq(c.player, 0x77u, "add: player passthrough into EAX (0x0049693a MOV EAX,[EBP-0x20])");
        ck_eq(c.x, 104u,
              "add: x = (origin_x=100 + dx=4) & width_mask(0xff) = 104 -- dx/outer pairs with "
              "origin_x/EDX/width_mask (0x0049692e-0x00496934)");
        ck_eq(c.y, 206u,
              "add: y = (origin_y=200 + dy=6) & height_mask(0xff) = 206 -- dy/inner pairs with "
              "origin_y/a2-EBX/height_mask (0x00496922-0x00496928)");
        ck_eq((uint32_t)c.sight, 0x5Au,
              "add: sight passthrough into ECX (0x0049691e MOVZX ECX,byte[EBP+0x8], param_5)");
    }
}

void test_add_area_index_is_dx_row_dy_col_not_swapped() {
    // area[1][0] nonzero, area[0][1] zero -> the call must report x=origin_x+1 (dx=1), y=origin_y+0
    // (dy=0), never the swapped (x=origin_x+0, y=origin_y+1). Proves the index expression
    // `dx*0xa + dy` (0x0049690c IMUL EAX,[dx],0xa; 0x00496912 ADD EAX,[dy]) really uses dx as the
    // ROW multiplied by 10, not dy.
    {
        sim_fixture fx;
        g_ufp_calls.clear();
        fx.geom.width_mask             = 0xff;
        fx.geom.height_mask            = 0xff;
        fx.cfg_buildings[3].area[1][0] = 1;
        sim_view v                     = fx.view();
        detail::sight_add_circle(v, g_add_calls, 1, 0, 0, 3, 1);
        ck_eq((uint32_t)g_ufp_calls.size(), 1u, "add: area[1][0] alone -> exactly one call");
        if (!g_ufp_calls.empty())
            ck(g_ufp_calls[0].x == 1 && g_ufp_calls[0].y == 0,
               "add: area[dx=1][dy=0] -> x=origin_x+1, y=origin_y+0 (row index is dx, not dy)");
    }
    {
        sim_fixture fx;
        g_ufp_calls.clear();
        fx.geom.width_mask             = 0xff;
        fx.geom.height_mask            = 0xff;
        fx.cfg_buildings[3].area[0][1] = 1;
        sim_view v                     = fx.view();
        detail::sight_add_circle(v, g_add_calls, 1, 0, 0, 3, 1);
        ck_eq((uint32_t)g_ufp_calls.size(), 1u, "add: area[0][1] alone -> exactly one call");
        if (!g_ufp_calls.empty())
            ck(g_ufp_calls[0].x == 0 && g_ufp_calls[0].y == 1,
               "add: area[dx=0][dy=1] -> x=origin_x+0, y=origin_y+1 (col index is dy, not dx)");
    }
}

void test_add_iteration_order_is_row_major_dx_then_dy() {
    // Three nonzero cells chosen so row-major (dx outer, dy inner) and any other raster order (e.g.
    // dy outer) produce a DIFFERENT sequence: (2,5) and (2,8) share a row, (5,0) is a later row.
    // Row-major visits (2,5) then (2,8) [same outer dx=2, inner dy advances] then (5,0) [outer dx
    // advances to 5]. A column-major walk would instead visit (5,0) before (2,8).
    sim_fixture fx;
    g_ufp_calls.clear();
    fx.geom.width_mask             = 0xff;
    fx.geom.height_mask            = 0xff;
    fx.cfg_buildings[3].area[2][5] = 1;
    fx.cfg_buildings[3].area[2][8] = 1;
    fx.cfg_buildings[3].area[5][0] = 1;
    sim_view v                     = fx.view();

    detail::sight_add_circle(v, g_add_calls, 1, 0, 0, 3, 1);

    ck_eq((uint32_t)g_ufp_calls.size(), 3u, "add(order): three nonzero cells -> three calls");
    if (g_ufp_calls.size() == 3) {
        ck(g_ufp_calls[0].x == 2 && g_ufp_calls[0].y == 5, "add(order): call #1 = (dx=2,dy=5)");
        ck(g_ufp_calls[1].x == 2 && g_ufp_calls[1].y == 8, "add(order): call #2 = (dx=2,dy=8) -- same "
                                                           "outer row visited before the outer counter advances "
                                                           "(0x00496944/68fb inner-loop-then-outer-increment shape)");
        ck(g_ufp_calls[2].x == 5 && g_ufp_calls[2].y == 0,
           "add(order): call #3 = (dx=5,dy=0) -- the outer/row counter only advances after its whole "
           "inner/col sweep completes (row-major, not column-major)");
    }
}

void test_add_10x10_bound_is_inclusive_0_to_9() {
    // Both loops compare with JL against 0xa (`CMP [ctr],0xa / JL body`, 0x004968de/0x004968f5): the
    // counter range is [0,10), i.e. 0..9 inclusive, 10 iterations each. Nonzero cells at BOTH extreme
    // corners (0,0) and (9,9) must both fire -- an off-by-one bound (e.g. `< 9`) would silently drop
    // the (9,9) corner and this test would see only 1 call instead of 2.
    sim_fixture fx;
    g_ufp_calls.clear();
    fx.geom.width_mask             = 0xff;
    fx.geom.height_mask            = 0xff;
    fx.cfg_buildings[3].area[0][0] = 1;
    fx.cfg_buildings[3].area[9][9] = 1;
    sim_view v                     = fx.view();

    detail::sight_add_circle(v, g_add_calls, 1, 0, 0, 3, 1);

    ck_eq((uint32_t)g_ufp_calls.size(), 2u,
          "add(bound): both (dx=0,dy=0) and (dx=9,dy=9) corners fire -- the 10x10 walk is INCLUSIVE "
          "of index 9 on both axes (0x004968de/68f5 JL 0xa)");
    if (g_ufp_calls.size() == 2) {
        ck(g_ufp_calls[0].x == 0 && g_ufp_calls[0].y == 0, "add(bound): first corner (0,0) visited first");
        ck(g_ufp_calls[1].x == 9 && g_ufp_calls[1].y == 9, "add(bound): last corner (9,9) visited last, not dropped");
    }
}

void test_add_wrap_mask_is_geom_pair_not_the_other_width_m_pair() {
    // A sibling function in this same migration batch shipped a real bug by
    // reaching for the WRONG mask pair (width_m/height_m, a distinct pair of globals) instead of the
    // geom ones this function's own asm actually reads (general.width_mask/height_mask @
    // 0x00e15398/0x00e153b0, confirmed by the literal addresses in the listing at 0x00496928/0x00496934
    // and 0x004969c1/0x004969cd for the remove-circle sibling). Seed the two pairs to DISTINCT, easily
    // distinguished values so a translation that grabbed the wrong pair fails this test instead of
    // silently passing.
    sim_fixture fx;
    g_ufp_calls.clear();
    fx.geom.width_mask             = 0xff; // 255 -- the REAL pair this function must use
    fx.geom.height_mask            = 0x3f; // 63
    fx.width_m                     = 0x07; // 7  -- the OTHER pair (used by different functions), must NOT be read here
    fx.height_m                    = 0x03; // 3
    fx.cfg_buildings[3].area[0][0] = 1;
    sim_view v                     = fx.view();

    // origin (100,100): using geom's masks gives (100,100&0x3f=36); using width_m/height_m would give
    // (100&7=4, 100&3=0) -- a visibly different, easily-asserted pair of numbers either way.
    detail::sight_add_circle(v, g_add_calls, 1, 100, 100, 3, 1);

    ck_eq((uint32_t)g_ufp_calls.size(), 1u, "add(mask): single cell -> single call");
    if (!g_ufp_calls.empty())
        ck(g_ufp_calls[0].x == 100 && g_ufp_calls[0].y == 36,
           "add(mask): x=(100&0xff)=100, y=(100&0x3f)=36 -- uses geom.width_mask/height_mask, NOT "
           "width_m(0x07)/height_m(0x03) (would have given x=4,y=0 instead) -- G27 mask-pair-confusion "
           "check");
}

void test_add_wrap_mask_actually_wraps_the_torus() {
    // A masking translation must actually MOD, not clamp: origin_x=250 + dx=9 = 259, & 0xff = 3, not
    // saturated at 255. Proves this is a real torus AND, not a range clamp that happens to agree in
    // the non-wrapping cases above.
    sim_fixture fx;
    g_ufp_calls.clear();
    fx.geom.width_mask             = 0xff;
    fx.geom.height_mask            = 0xff;
    fx.cfg_buildings[3].area[9][0] = 1; // only dx=9 nonzero
    sim_view v                     = fx.view();

    detail::sight_add_circle(v, g_add_calls, 1, 250, 0, 3, 1);

    ck_eq((uint32_t)g_ufp_calls.size(), 1u, "add(wrap): single cell -> single call");
    if (!g_ufp_calls.empty())
        ck_eq(g_ufp_calls[0].x, 3u,
              "add(wrap): x=(250+9)&0xff=259&255=3 -- true torus wraparound, not clamped to 255");
}

void test_add_building_id_selects_correct_area_row() {
    // building_id multiplies by 0x842 (0x00496905 IMUL EDX,[building_id],0x842) to select the row.
    // Seed a DIFFERENT building_id's row nonzero too, so a translation that read the wrong row (e.g.
    // always row 0, or off by one row) would produce either the wrong coordinates or zero calls
    // instead of the expected single call.
    sim_fixture fx;
    g_ufp_calls.clear();
    fx.geom.width_mask              = 0xff;
    fx.geom.height_mask             = 0xff;
    fx.cfg_buildings[42].area[4][4] = 1; // the row this call actually targets
    fx.cfg_buildings[7].area[1][1]  = 1; // a DIFFERENT row, must be ignored
    sim_view v                      = fx.view();

    detail::sight_add_circle(v, g_add_calls, 1, 0, 0, 42, 1);

    ck_eq((uint32_t)g_ufp_calls.size(), 1u,
          "add(row): building_id=42 -> exactly 1 call, from row 42's own footprint only");
    if (!g_ufp_calls.empty())
        ck(g_ufp_calls[0].x == 4 && g_ufp_calls[0].y == 4,
           "add(row): call reports (4,4) -- building_id=7's (1,1) cell was NOT read (wrong row would "
           "either miss this cell or report (1,1) instead)");
}

// ==== llm_strat_sight_remove_circle @0x0049694f ===================================================
// Byte-identical shape to sight_add_circle (same 0x99-byte body, same loop bounds, same index
// expression, same width_mask/height_mask addresses -- see the .asm's own address comments at
// 0x004969c1/0x004969cd) -- calls llm_strat_fow_remove_sight per nonzero cell instead of
// map_fow_UpdateFoWPlus. Independently re-verified (not assumed identical), since it is a SEPARATE
// compiled body that could in principle have drifted (a wrong copy-paste of a bound or an index would
// not show up in the add-circle tests above).

void test_remove_all_zero_footprint_produces_zero_calls() {
    sim_fixture fx;
    g_rs_calls.clear();
    fx.geom.width_mask  = 0xff;
    fx.geom.height_mask = 0xff;
    sim_view v          = fx.view();
    detail::sight_remove_circle(v, g_remove_calls, /*player*/ 7, /*origin_x*/ 0, /*origin_y*/ 0,
                                /*building_id*/ 3, /*radius*/ 9);
    ck_eq((uint32_t)g_rs_calls.size(), 0u,
          "remove: all-zero 10x10 footprint -> exactly 0 outward calls (0x004969ae gate skips every cell)");
}

void test_remove_single_cell_produces_exactly_one_call_with_full_arg_tuple() {
    sim_fixture fx;
    g_rs_calls.clear();
    fx.geom.width_mask             = 0xff;
    fx.geom.height_mask            = 0xff;
    fx.cfg_buildings[3].area[4][6] = 1;
    sim_view v                     = fx.view();

    detail::sight_remove_circle(v, g_remove_calls, 0x77u, 100, 200, 3, 0x5A);

    ck_eq((uint32_t)g_rs_calls.size(), 1u,
          "remove: exactly ONE nonzero cell -> exactly ONE outward call (per-cell gate at 0x004969ae)");
    if (g_rs_calls.size() == 1) {
        const auto &c = g_rs_calls[0];
        ck_eq(c.player, 0x77u, "remove: player passthrough into EAX (0x004969d3 MOV EAX,[EBP-0x20])");
        ck_eq((uint32_t)c.x, 104u,
              "remove: x = (origin_x=100 + dx=4) & width_mask(0xff) = 104 (0x004969c7-0x004969cd)");
        ck_eq((uint32_t)c.y, 206u,
              "remove: y = (origin_y=200 + dy=6) & height_mask(0xff) = 206 (0x004969bb-0x004969c1)");
        ck_eq((uint32_t)c.radius, 0x5Au,
              "remove: radius passthrough into ECX (0x004969b7 MOVZX ECX,byte[EBP+0x8], param_5)");
    }
}

void test_remove_area_index_is_dx_row_dy_col_not_swapped() {
    sim_fixture fx;
    g_rs_calls.clear();
    fx.geom.width_mask             = 0xff;
    fx.geom.height_mask            = 0xff;
    fx.cfg_buildings[3].area[1][0] = 1;
    sim_view v                     = fx.view();
    detail::sight_remove_circle(v, g_remove_calls, 1, 0, 0, 3, 1);
    ck_eq((uint32_t)g_rs_calls.size(), 1u, "remove: area[1][0] alone -> exactly one call");
    if (!g_rs_calls.empty())
        ck(g_rs_calls[0].x == 1 && g_rs_calls[0].y == 0,
           "remove: area[dx=1][dy=0] -> x=origin_x+1, y=origin_y+0 (row index is dx, not dy)");
}

void test_remove_iteration_order_is_row_major_dx_then_dy() {
    sim_fixture fx;
    g_rs_calls.clear();
    fx.geom.width_mask             = 0xff;
    fx.geom.height_mask            = 0xff;
    fx.cfg_buildings[3].area[2][5] = 1;
    fx.cfg_buildings[3].area[2][8] = 1;
    fx.cfg_buildings[3].area[5][0] = 1;
    sim_view v                     = fx.view();

    detail::sight_remove_circle(v, g_remove_calls, 1, 0, 0, 3, 1);

    ck_eq((uint32_t)g_rs_calls.size(), 3u, "remove(order): three nonzero cells -> three calls");
    if (g_rs_calls.size() == 3) {
        ck(g_rs_calls[0].x == 2 && g_rs_calls[0].y == 5, "remove(order): call #1 = (dx=2,dy=5)");
        ck(g_rs_calls[1].x == 2 && g_rs_calls[1].y == 8, "remove(order): call #2 = (dx=2,dy=8), same row");
        ck(g_rs_calls[2].x == 5 && g_rs_calls[2].y == 0,
           "remove(order): call #3 = (dx=5,dy=0) -- row-major, outer counter advances last");
    }
}

void test_remove_10x10_bound_is_inclusive_0_to_9() {
    sim_fixture fx;
    g_rs_calls.clear();
    fx.geom.width_mask             = 0xff;
    fx.geom.height_mask            = 0xff;
    fx.cfg_buildings[3].area[0][0] = 1;
    fx.cfg_buildings[3].area[9][9] = 1;
    sim_view v                     = fx.view();

    detail::sight_remove_circle(v, g_remove_calls, 1, 0, 0, 3, 1);

    ck_eq((uint32_t)g_rs_calls.size(), 2u,
          "remove(bound): both (0,0) and (9,9) corners fire -- 10x10 walk inclusive of index 9 "
          "(0x00496977/0049698e JL 0xa)");
    if (g_rs_calls.size() == 2) {
        ck(g_rs_calls[0].x == 0 && g_rs_calls[0].y == 0, "remove(bound): first corner (0,0) visited first");
        ck(g_rs_calls[1].x == 9 && g_rs_calls[1].y == 9, "remove(bound): last corner (9,9) visited last, not dropped");
    }
}

void test_remove_wrap_mask_is_geom_pair_not_the_other_width_m_pair() {
    sim_fixture fx;
    g_rs_calls.clear();
    fx.geom.width_mask             = 0xff;
    fx.geom.height_mask            = 0x3f;
    fx.width_m                     = 0x07; // the OTHER pair, must not be read here either
    fx.height_m                    = 0x03;
    fx.cfg_buildings[3].area[0][0] = 1;
    sim_view v                     = fx.view();

    detail::sight_remove_circle(v, g_remove_calls, 1, 100, 100, 3, 1);

    ck_eq((uint32_t)g_rs_calls.size(), 1u, "remove(mask): single cell -> single call");
    if (!g_rs_calls.empty())
        ck(g_rs_calls[0].x == 100 && g_rs_calls[0].y == 36,
           "remove(mask): x=(100&0xff)=100, y=(100&0x3f)=36 -- geom.width_mask/height_mask "
           "(0x004969cd/0x004969c1), NOT width_m/height_m -- G27 mask-pair-confusion check, "
           "re-verified independently for THIS function's own body");
}

void test_remove_building_id_selects_correct_area_row() {
    sim_fixture fx;
    g_rs_calls.clear();
    fx.geom.width_mask              = 0xff;
    fx.geom.height_mask             = 0xff;
    fx.cfg_buildings[42].area[4][4] = 1;
    fx.cfg_buildings[7].area[1][1]  = 1;
    sim_view v                      = fx.view();

    detail::sight_remove_circle(v, g_remove_calls, 1, 0, 0, 42, 1);

    ck_eq((uint32_t)g_rs_calls.size(), 1u, "remove(row): building_id=42 -> exactly 1 call, row 42 only");
    if (!g_rs_calls.empty())
        ck(g_rs_calls[0].x == 4 && g_rs_calls[0].y == 4,
           "remove(row): call reports (4,4), not building_id=7's (1,1)");
}

} // namespace

void run_fog_sight_circle_tests() {
    printf("-- fog/sight circle family (offline-only oracle: convert_sight_to_area / "
           "sight_add_circle / sight_remove_circle) --\n");

    test_convert_sight_to_area_cascade();

    test_add_all_zero_footprint_produces_zero_calls();
    test_add_single_cell_produces_exactly_one_call_with_full_arg_tuple();
    test_add_area_index_is_dx_row_dy_col_not_swapped();
    test_add_iteration_order_is_row_major_dx_then_dy();
    test_add_10x10_bound_is_inclusive_0_to_9();
    test_add_wrap_mask_is_geom_pair_not_the_other_width_m_pair();
    test_add_wrap_mask_actually_wraps_the_torus();
    test_add_building_id_selects_correct_area_row();

    test_remove_all_zero_footprint_produces_zero_calls();
    test_remove_single_cell_produces_exactly_one_call_with_full_arg_tuple();
    test_remove_area_index_is_dx_row_dy_col_not_swapped();
    test_remove_iteration_order_is_row_major_dx_then_dy();
    test_remove_10x10_bound_is_inclusive_0_to_9();
    test_remove_wrap_mask_is_geom_pair_not_the_other_width_m_pair();
    test_remove_building_id_selects_correct_area_row();
}

} // namespace mh::sim::test
