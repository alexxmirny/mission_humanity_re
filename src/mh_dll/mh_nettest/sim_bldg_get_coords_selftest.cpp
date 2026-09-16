//
// sim_bldg_get_coords_selftest.cpp -- `simtest` cases for llm_strat_bldg_get_coords (batch B,
// sim/sim_bldg_get_coords.h/.cpp). ONLY possible oracle: this shadow site writes only through the
// two OUT-pointer params (*out_x/*out_y) with no return value and no tracked/compared region -- the
// header's own "PURE / NO-WRITE, NO OUTWARD CALL" banner, same VACUOUS-by-shape posture as
// sim_bldg_placement_corner_selftest.cpp's llm_bldg_calc_placement_corner_from_center.
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_get_coords_00449b8a.asm), not read off the .cpp:
//
//   building_id = buildings[player][building_index].building_id   (MOVZX WORD, 0x00449bbe/0x00449c16)
//   width       = cfg_buildings[building_id].width                (MOVZX BYTE, 0x00449bcb, offset 0xa)
//   height      = cfg_buildings[building_id].height               (MOVZX BYTE, 0x00449c23, offset 0x9 --
//                 ONE LESS than width's, adjacent fields, easy to swap in a translation)
//   x           = buildings[player][building_index].x             (MOVZX BYTE, 0x00449bea/0x00449bf1)
//   y           = buildings[player][building_index].y             (MOVZX BYTE, 0x00449c42/0x00449c49)
//   *out_x = ((x << 5) + (width  << 4)) & general.bw_mask   (SHL/SHL/ADD then AND -- 0x00449bf1-0x00449bff)
//   *out_y = ((y << 5) + (height << 4)) & general.bh_mask   (SHL/SHL/ADD then AND -- 0x00449c49-0x00449c56)
//
// The mask pair is `general` (`_G` @ 0x00e15390/0x00e15394) read DIRECTLY as bw_mask/bh_mask (offsets
// 0/4 of llm_strat_map_geom) -- the PIXEL-space wrap pair, NOT v.geom->width_mask/height_mask (offsets
// 8/0x20, the TILE-space pair map_width_mask()/map_height_mask() read, which sim_fixture::reset()
// seeds to 0xff/0x3f by default). Every case below sets fx.geom.bw_mask/bh_mask EXPLICITLY (reset()
// zeroes them via the whole-struct memset) to values DISTINCT from the surviving width_mask/
// height_mask defaults, so a translation that grabbed the wrong mask pair produces a visibly
// different (far smaller) wrapped result rather than passing by coincidence.
//
#include "sim/sim_bldg_get_coords.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

void check_coords(sim_fixture &fx, uint16_t player, int32_t building_index, uint32_t want_x,
                  uint32_t want_y, const char *what_x, const char *what_y) {
    int32_t out_x = 0x0badf00d, out_y = 0x0badf00d;
    detail::bldg_get_coords(fx.view(), player, building_index, &out_x, &out_y);
    ck_eq(static_cast<uint32_t>(out_x), want_x, what_x);
    ck_eq(static_cast<uint32_t>(out_y), want_y, what_y);
}

} // namespace

void run_bldg_get_coords_tests() {
    sim_fixture fx;

    // ---- S1: baseline -- everything distinct (x!=y, width!=height, bw_mask!=bh_mask), masks wide
    // enough that neither axis wraps, so this case isolates the plain formula from any AND-clipping
    // concern (S2/S5 below cover that). Also the primary width_mask/height_mask-vs-bw_mask/bh_mask
    // discriminator: reset() leaves width_mask=0xff/height_mask=0x3f, both far narrower than the
    // 0xffff/0x0fff set here, so a translation reading the wrong pair would produce 160/0 instead of
    // 416/128. -----------------------------------------------------------------------------------
    fx.reset();
    fx.geom.bw_mask             = 0xffff;
    fx.geom.bh_mask             = 0x0fff;
    fx.b(0, 3).building_id      = 17; // building_index (3) != building_id (17) -- indirection present
    fx.b(0, 3).x                = 10;
    fx.b(0, 3).y                = 3;
    fx.cfg_buildings[17].width  = 6;
    fx.cfg_buildings[17].height = 2;
    // out_x = (10<<5 + 6<<4) & 0xffff = (320+96) & 0xffff = 416.
    // out_y = (3<<5 + 2<<4) & 0x0fff = (96+32) & 0x0fff = 128.
    check_coords(fx, 0, 3, 416, 128, "coords S1 baseline .x", "coords S1 baseline .y");

    // ---- S2: max byte values on x/y and width/height, narrow masks so the pre-mask sum actually
    // EXCEEDS the mask on both axes -- proves the AND genuinely clips (not just an identity mask) and,
    // with bw_mask != bh_mask, that swapping which mask feeds which axis would be caught (the swapped
    // value differs sharply from the correct one on both fields). ---------------------------------
    fx.reset();
    fx.geom.bw_mask             = 0xfff; // 4095 -- sum (12240) exceeds this
    fx.geom.bh_mask             = 0x1ff; // 511  -- sum (8000) exceeds this, and differs from bw_mask
    fx.b(0, 9).building_id      = 40;
    fx.b(0, 9).x                = 255;
    fx.b(0, 9).y                = 200;
    fx.cfg_buildings[40].width  = 255;
    fx.cfg_buildings[40].height = 100;
    // out_x = (255<<5 + 255<<4) & 0xfff = (8160+4080) & 0xfff = 12240 & 0xfff = 4048.
    // out_y = (200<<5 + 100<<4) & 0x1ff = (6400+1600) & 0x1ff = 8000 & 0x1ff = 320.
    check_coords(fx, 0, 9, 4048, 320, "coords S2 max-byte wrap .x", "coords S2 max-byte wrap .y");

    // ---- S3: building_id != building_index INDIRECTION, isolated from any wrap (masks wide enough
    // that neither sum clips). cfg_buildings[building_index] (slot 5) is left ZEROED by reset(), so a
    // translation that read cfg_buildings[building_index] directly instead of indexing by
    // building_id would compute width16=height16=0 and get 224/672 instead of 368/720. ------------
    fx.reset();
    fx.geom.bw_mask             = 0x7ff; // 2047, 368 does not wrap
    fx.geom.bh_mask             = 0x3ff; // 1023, 720 does not wrap
    fx.b(1, 5).building_id      = 50;    // building_index (5) != building_id (50)
    fx.b(1, 5).x                = 7;
    fx.b(1, 5).y                = 21;
    fx.cfg_buildings[50].width  = 9;
    fx.cfg_buildings[50].height = 3;
    // out_x = (7<<5 + 9<<4) & 0x7ff = (224+144) & 0x7ff = 368.
    // out_y = (21<<5 + 3<<4) & 0x3ff = (672+48) & 0x3ff = 720.
    check_coords(fx, 1, 5, 368, 720, "coords S3 building_id != building_index indirection .x",
                 "coords S3 building_id != building_index indirection .y");

    // ---- S4: PLAYER-ROW distinctness -- the same building_index (2) on two different players' rows,
    // each with its own building_id/x/y/width/height, so a translation that dropped or mis-scaled the
    // player term of the row address would read the wrong row's data (or alias the two together).
    // The row stride is BUILDINGS_PER_PLAYER (0x111 records, matching the asm's IMUL EDX,...,0x111). --
    fx.reset();
    fx.geom.bw_mask             = 0xffff;
    fx.geom.bh_mask             = 0xffff;
    fx.b(0, 2).building_id      = 30;
    fx.b(0, 2).x                = 1;
    fx.b(0, 2).y                = 1;
    fx.cfg_buildings[30].width  = 1;
    fx.cfg_buildings[30].height = 1;
    fx.b(1, 2).building_id      = 31;
    fx.b(1, 2).x                = 99;
    fx.b(1, 2).y                = 88;
    fx.cfg_buildings[31].width  = 50;
    fx.cfg_buildings[31].height = 60;
    // player 0, index 2: out_x = (1<<5 + 1<<4) & 0xffff = (32+16) = 48; out_y likewise 48.
    check_coords(fx, 0, 2, 48, 48, "coords S4 player-row 0 .x", "coords S4 player-row 0 .y");
    // player 1, same index 2, DIFFERENT row: out_x = (99<<5 + 50<<4) = 3168+800 = 3968;
    // out_y = (88<<5 + 60<<4) = 2816+960 = 3776.
    check_coords(fx, 1, 2, 3968, 3776, "coords S4 player-row 1 (same index, different row) .x",
                 "coords S4 player-row 1 (same index, different row) .y");

    // ---- S5: DEGENERATE width=height=0 (Building record's own footprint fields zeroed) -- the width/
    // height terms drop out entirely, so *out_x/*out_y reduce to (x<<5)&mask / (y<<5)&mask. Also
    // exercises the wrap by itself (x/y large enough that the shift-by-5 alone overflows a narrow
    // mask), independent from S2's width/height-driven wrap. --------------------------------------
    fx.reset();
    fx.geom.bw_mask            = 0xff; // 255
    fx.geom.bh_mask            = 0xff; // 255
    fx.b(0, 6).building_id     = 8;
    fx.b(0, 6).x               = 50;
    fx.b(0, 6).y               = 60;
    fx.cfg_buildings[8].width  = 0;
    fx.cfg_buildings[8].height = 0;
    // out_x = (50<<5 + 0) & 0xff = 1600 & 0xff = 64.  out_y = (60<<5 + 0) & 0xff = 1920 & 0xff = 128.
    check_coords(fx, 0, 6, 64, 128, "coords S5 width=height=0 degenerate .x",
                 "coords S5 width=height=0 degenerate .y");

    // ---- S6: building_id == 0 EDGE CASE (the first cfg_buildings record, index 0 has no special
    // handling in the asm -- straight-line code, no branch on building_id -- but worth pinning since
    // it is the array's first element and a fencepost bug would show here first). ------------------
    fx.reset();
    fx.geom.bw_mask            = 0x3ff; // 1023
    fx.geom.bh_mask            = 0x1ff; // 511 -- sum (992) exceeds this, wraps
    fx.b(2, 10).building_id    = 0;
    fx.b(2, 10).x              = 15;
    fx.b(2, 10).y              = 25;
    fx.cfg_buildings[0].width  = 4;
    fx.cfg_buildings[0].height = 12;
    // out_x = (15<<5 + 4<<4) & 0x3ff = (480+64) & 0x3ff = 544.
    // out_y = (25<<5 + 12<<4) & 0x1ff = (800+192) & 0x1ff = 992 & 0x1ff = 480.
    check_coords(fx, 2, 10, 544, 480, "coords S6 building_id==0 edge case .x",
                 "coords S6 building_id==0 edge case .y");
}

} // namespace mh::sim::test
