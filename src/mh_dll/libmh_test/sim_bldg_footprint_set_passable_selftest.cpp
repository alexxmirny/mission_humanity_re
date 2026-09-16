//
// sim_bldg_footprint_set_passable_selftest.cpp -- `simtest` cases for
// llm_map_bldg_footprint_set_passable (sim/sim_bldg_footprint_set_passable.h/.cpp), SIM1B
// (building_tick machinery slice).
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_map_bldg_footprint_set_passable_004938d3.asm), not read off the Ghidra .c draft
// (though in this case the draft happens to match -- re-derived independently below):
//
//   Nested 10x10 walk, ROW outer (EBP-0x14, initialised/compared/incremented at 0x004938f2 /
//   LAB_004938f9 / LAB_00493901) then COL inner (EBP-0x10, at 0x00493909 / LAB_00493910 /
//   LAB_00493918) -- two independent loop-top/increment block pairs, not assumed from the `.c`'s
//   `for` nesting.
//
//   Array read (0x00493920-0x00493937): `IMUL EDX,building_idx,0x842` (cfg_final_struct_Building's
//   size) + `IMUL EAX,row,0xa` (row-major stride 10) + `col`, then `CMP byte [EAX+0xd9ec8b],0`,
//   i.e. `cfg_buildings[building_idx].area[row][col] != 0`. area[row][col] == 0 -> JZ 0x0049395f,
//   past the write, straight to the loop-continue -- NO write happens for an unoccupied cell (not
//   even a zero-write).
//
//   Passable write (0x00493939-0x00493958): EAX(origin_x) + row, AND with dword [0x00e15398]
//   (general.width_mask), SHL 8 -> tile_x<<8; EAX(origin_y) + col, AND with dword [0x00e153b0]
//   (general.height_mask) -> tile_y; ADD the two, `MOV byte [EAX+0xb64bb0],1` -- i.e.
//   `passable[(tile_x<<8)|tile_y] = 1` where tile_x=(origin_x+row)&width_mask,
//   tile_y=(origin_y+col)&height_mask. Row feeds the X/width-masked half, col feeds the
//   Y/height-masked half -- NOT swapped (a transpose would use row for Y and col for X).
//
// This function WRITES `passable` (writes_shared per sim_migration.json; writes_island=[]), so
// every case here uses the WRITE half of the fixture (`sim_store own = fx.store()`), not just
// `sim_view`.
//
#include "sim/sim_bldg_footprint_set_passable.h"

#include <cstring>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr uint8_t kSentinel = 0xAA;

void set_area(sim_fixture &fx, int32_t building_idx, int32_t row, int32_t col, uint8_t v) {
    fx.cfg_buildings[building_idx].area[row][col] = v;
}

// tile index into fx.passable, matching own.passable_at()'s (tile_x<<8)|tile_y packing.
size_t idx(int32_t tile_x, int32_t tile_y) { return (size_t)((tile_x << 8) | tile_y); }

} // namespace

void run_bldg_footprint_set_passable_tests() {
    // ---- S1: a small sparse footprint (3 of 100 cells set), origin (0,0) so the masked add is an
    // identity -- the simplest case, and it proves the per-cell gate: exactly the 3 occupied cells
    // become passable, and a neighbouring never-set cell does not. ---------------------------------
    {
        sim_fixture fx;
        sim_store   own = fx.store();
        std::memset(fx.passable.data(), kSentinel, fx.passable.size());

        const int32_t bidx = 5;
        set_area(fx, bidx, 0, 0, 1); // -> tile (0,0)
        set_area(fx, bidx, 2, 3, 1); // -> tile (2,3)
        set_area(fx, bidx, 9, 9, 1); // -> tile (9,9)

        detail::bldg_footprint_set_passable(fx.view(), own, /*origin_x=*/0, /*origin_y=*/0, bidx);

        ck_eq(own.passable_at(0, 0), 1u, "S1: area[0][0] set -> tile (0,0) passable");
        ck_eq(own.passable_at(2, 3), 1u, "S1: area[2][3] set -> tile (2,3) passable");
        ck_eq(own.passable_at(9, 9), 1u, "S1: area[9][9] set -> tile (9,9) passable");
        // A neighbour of (2,3) that was never in the footprint must stay at the sentinel -- no
        // stray write from a miscomputed index.
        ck_eq(own.passable_at(2, 4), kSentinel, "S1: neighbour tile (2,4) untouched");
        ck_eq(own.passable_at(3, 3), kSentinel, "S1: neighbour tile (3,3) untouched");
        // A cell picked from nowhere near the footprint, to catch a gross off-by-stride bug.
        ck_eq(own.passable_at(50, 50), kSentinel, "S1: unrelated tile (50,50) untouched");
    }

    // ---- S2: an asymmetric (L-shaped) footprint with origin_x != origin_y, so ROW must feed the
    // X/width-masked half and COL the Y/height-masked half -- a translation that swapped row/col (or
    // origin_x/origin_y) would disagree with at least one of these three tiles. Masks left at the
    // fixture default (width_mask=0xff, height_mask=0x3f, both < the sums below so no wrap muddies
    // the read -- wrap is exercised separately in S3). ------------------------------------------
    {
        sim_fixture fx;
        sim_store   own = fx.store();
        std::memset(fx.passable.data(), kSentinel, fx.passable.size());

        const int32_t bidx     = 6;
        const int32_t origin_x = 10;
        const int32_t origin_y = 20;
        // L-shape: (row=1,col=0), (row=0,col=1), (row=4,col=0). Distinct row/col values everywhere
        // so a transpose lands on a genuinely different tile, not one that happens to coincide.
        set_area(fx, bidx, 1, 0, 1); // -> tile (origin_x+1, origin_y+0) = (11,20)
        set_area(fx, bidx, 0, 1, 1); // -> tile (origin_x+0, origin_y+1) = (10,21)
        set_area(fx, bidx, 4, 0, 1); // -> tile (origin_x+4, origin_y+0) = (14,20)

        detail::bldg_footprint_set_passable(fx.view(), own, origin_x, origin_y, bidx);

        ck_eq(own.passable_at(11, 20), 1u, "S2: area[1][0] -> tile (11,20) (row feeds X)");
        ck_eq(own.passable_at(10, 21), 1u, "S2: area[0][1] -> tile (10,21) (col feeds Y)");
        ck_eq(own.passable_at(14, 20), 1u, "S2: area[4][0] -> tile (14,20)");
        // The TRANSPOSED reading of area[1][0] would be tile (origin_x+0, origin_y+1) = (10,21) --
        // already asserted =1 above for a different reason (area[0][1]), so also check the
        // transposed reading of area[4][0], (origin_x+0, origin_y+4) = (10,24), which no real cell
        // maps to and must stay at the sentinel.
        ck_eq(own.passable_at(10, 24), kSentinel,
              "S2: transposed reading of area[4][0] (10,24) must NOT be set");
        // And the never-set diagonal neighbour of (11,20).
        ck_eq(own.passable_at(12, 21), kSentinel, "S2: unrelated tile (12,21) untouched");
    }

    // ---- S3: masks actually wrap, width_mask != height_mask, origin chosen near the top of each
    // axis so (origin+row)&width_mask / (origin+col)&height_mask both fold back to a LOW index --
    // a translation that swapped which mask gates which axis would land on a different tile than
    // hand computation here. ---------------------------------------------------------------------
    {
        sim_fixture fx;
        sim_store   own = fx.store();
        std::memset(fx.passable.data(), kSentinel, fx.passable.size());

        fx.geom.width_mask  = 0xff; // 256-wide axis
        fx.geom.height_mask = 0x3f; // 64-wide axis (distinct from width_mask)

        const int32_t bidx     = 7;
        const int32_t origin_x = 0xfe; // 254 -- width axis, near its wrap point
        const int32_t origin_y = 0x3e; // 62  -- height axis, near ITS (different) wrap point

        // area[3][3]: tile_x = (254+3) & 0xff = 257 & 0xff = 1 (wraps); tile_y = (62+3) & 0x3f =
        // 65 & 0x3f = 1 (wraps too, but via the OTHER mask/width). Both land on 1, which is exactly
        // why a swapped-mask translation would still (coincidentally) pass here -- so also check a
        // cell where the two masks disagree.
        set_area(fx, bidx, 3, 3, 1); // -> tile (1,1)
        // area[1][5]: tile_x = (254+1) & 0xff = 255 & 0xff = 255 (no wrap, distinct from height's
        // range); tile_y = (62+5) & 0x3f = 67 & 0x3f = 3. If width_mask/height_mask were swapped,
        // tile_x would instead be (254+1) & 0x3f = 255 & 0x3f = 63 -- a different, clearly wrong
        // value versus the real 255.
        set_area(fx, bidx, 1, 5, 1); // -> tile (255,3)

        detail::bldg_footprint_set_passable(fx.view(), own, origin_x, origin_y, bidx);

        ck_eq(own.passable_at(1, 1), 1u, "S3: area[3][3] with both axes wrapping -> tile (1,1)");
        ck_eq(own.passable_at(255, 3), 1u,
              "S3: area[1][5], width axis NOT wrapped (255), height axis wrapped (3) -- "
              "distinguishes width_mask from height_mask");
        // The mask-swapped reading of area[1][5] would be tile (63,3) instead of (255,3).
        ck_eq(own.passable_at(63, 3), kSentinel,
              "S3: mask-swapped reading of area[1][5] (63,3) must NOT be set");
        ck_eq(own.passable_at(0, 0), kSentinel, "S3: unrelated tile (0,0) untouched");
    }

    // ---- S4: an all-zero footprint -> no writes at all. The whole grid must stay at the sentinel;
    // this is also the case that would catch a translation that writes unconditionally (dropping
    // the `area[row][col] != 0` gate). --------------------------------------------------------------
    {
        sim_fixture fx;
        sim_store   own = fx.store();
        std::memset(fx.passable.data(), kSentinel, fx.passable.size());
        // fx.reset() already zeroed cfg_buildings, so building 8's area is already all-zero; set it
        // explicitly anyway so the case documents its own precondition.
        const int32_t bidx = 8;
        for (int32_t r = 0; r < 10; ++r)
            for (int32_t c = 0; c < 10; ++c) set_area(fx, bidx, r, c, 0);

        detail::bldg_footprint_set_passable(fx.view(), own, /*origin_x=*/40, /*origin_y=*/40, bidx);

        bool any_changed = false;
        for (size_t i = 0; i < fx.passable.size(); ++i) {
            if (fx.passable[i] != kSentinel) {
                any_changed = true;
                break;
            }
        }
        ck(!any_changed, "S4: all-zero area -> no cell in the whole grid is written");
        // Spot checks at the tiles the loop WOULD have hit at row=col=0 and row=col=9, for a
        // readable failure message if the blanket scan above ever regresses.
        ck_eq(own.passable_at(40, 40), kSentinel, "S4: origin tile (40,40) untouched");
        ck_eq(own.passable_at(49, 49), kSentinel, "S4: corner tile (49,49) untouched");
    }

    // ---- S5: a FULL 10x10 footprint (every cell set) -- exactly 100 tiles become passable and
    // nothing outside that 10x10 block does, catching an off-by-one on either loop bound (9 vs 10
    // iterations) in either direction. Origin (100,100), masks wide enough (0xff/0xff) that no wrap
    // folds a footprint tile onto a tile outside the block. -----------------------------------------
    {
        sim_fixture fx;
        sim_store   own = fx.store();
        std::memset(fx.passable.data(), kSentinel, fx.passable.size());
        fx.geom.width_mask  = 0xff;
        fx.geom.height_mask = 0xff;

        const int32_t bidx     = 9;
        const int32_t origin_x = 100;
        const int32_t origin_y = 100;
        for (int32_t r = 0; r < 10; ++r)
            for (int32_t c = 0; c < 10; ++c) set_area(fx, bidx, r, c, 1);

        detail::bldg_footprint_set_passable(fx.view(), own, origin_x, origin_y, bidx);

        int32_t set_count = 0;
        for (size_t i = 0; i < fx.passable.size(); ++i)
            if (fx.passable[i] == 1) ++set_count;
        ck_eq((uint32_t)set_count, 100u, "S5: full 10x10 area sets EXACTLY 100 tiles");

        // The four corners of the 10x10 block.
        ck_eq(own.passable_at(100, 100), 1u, "S5: corner (100,100) set");
        ck_eq(own.passable_at(109, 100), 1u, "S5: corner (109,100) set (row bound 9)");
        ck_eq(own.passable_at(100, 109), 1u, "S5: corner (100,109) set (col bound 9)");
        ck_eq(own.passable_at(109, 109), 1u, "S5: corner (109,109) set");
        // Just outside the block on each axis -- an off-by-one loop bound would leak into these.
        ck_eq(own.passable_at(110, 100), kSentinel, "S5: one past the row bound (110,100) untouched");
        ck_eq(own.passable_at(100, 110), kSentinel, "S5: one past the col bound (100,110) untouched");
        ck_eq(own.passable_at(99, 100), kSentinel, "S5: one before the row start (99,100) untouched");
    }
}

} // namespace mh::sim::test
