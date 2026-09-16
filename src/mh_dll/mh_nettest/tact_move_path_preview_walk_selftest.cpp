//
// tact_move_path_preview_walk_selftest.cpp -- offline oracle for
//   llm_tact_move_path_preview_walk @0x0042eaf9 (libmh/tact/tact_move_path_preview_walk.cpp)
//
// Rig-armable (its write closure is ONE region, tile_objects, at depth 0 -- see
// the shadow manifest's entry), but the region comparison alone cannot see the
// *out_col/*out_row values, which land on the CALLER's stack, not in any declared region -- the
// shadow generator has no out-pointer comparison (gen_dll_shadow.py). So this oracle exists
// alongside the rig arm specifically to pin the final reported position and the exact overlay-byte
// arithmetic at every step, which a region-only comparison would only partially exercise.
//
#include "tact/tact_move_path_preview_walk.h"

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {
using namespace mh::tact;

constexpr uint8_t VISIBLE_FLAG = 0x80;

void seed_visible(tact_fixture &fx, int32_t col, int32_t row, bool visible) {
    fx.tile_objects[(size_t)((col << 8) | row)].flags[1] = visible ? VISIBLE_FLAG : 0;
}

} // namespace

void run_move_path_preview_walk_tests() {
    // T1: heading == 0 at the FIRST entry ends the walk immediately at the start tile, having only
    // run the unconditional pre-loop clear (0x0042eb34-0x0042eb42).
    {
        tact_fixture fx;
        fx.tile_objects[(size_t)((3 << 8) | 3)].unit[1] = 0xff; // must be cleared, not left alone
        mh::state::mode_planes planes                   = fx.planes();
        planes.path_waypoint_at(0, 1, 0).heading        = 0;
        int32_t out_col = -1, out_row = -1;
        detail::move_path_preview_walk(planes, 3, 3, 1, &out_col, &out_row);
        ck_eq((uint32_t)out_col, 3u, "T1: out_col == start_col, empty route");
        ck_eq((uint32_t)out_row, 3u, "T1: out_row == start_row, empty route");
        ck_eq((uint32_t)mh::state::tile_overlay(planes, 3, 3), 0u,
              "T1: the start tile's overlay is cleared unconditionally, 0x0042eb42");
    }

    // T2: a 2-step run in one direction (heading 1 == row+1), fully visible throughout. Pins BOTH
    // stamp shapes: the pre-move high-nibble stamp (0x0042ebc5-0x0042ec03) and the post-move OR
    // stamp (0x0042ecf1-0x0042ed2d), at three DISTINCT tiles so no single check could pass by
    // aliasing.
    {
        tact_fixture fx;
        for (int32_t r = 5; r <= 7; ++r) seed_visible(fx, 5, r, true);
        mh::state::mode_planes planes               = fx.planes();
        planes.path_waypoint_at(0, 2, 0).heading    = 1;
        planes.path_waypoint_at(0, 2, 0).run_length = 2;
        planes.path_waypoint_at(0, 2, 1).heading    = 0;
        int32_t out_col = -1, out_row = -1;
        detail::move_path_preview_walk(planes, 5, 5, 2, &out_col, &out_row);

        ck_eq((uint32_t)out_col, 5u, "T2: heading 1 never changes col");
        ck_eq((uint32_t)out_row, 7u, "T2: two steps of heading 1, row 5 -> 7");
        // (5,5): pre-move-stamped ONCE (step 1), never revisited -> (1/3)<<4 + 0x10 | 0 == 0x10.
        ck_eq((uint32_t)mh::state::tile_overlay(planes, 5, 5), 0x10u,
              "T2: (5,5) carries only its pre-move stamp, 0x0042ec03");
        // (5,6): OR-stamped in step 1 (-> 1), then OVERWRITTEN by step 2's pre-move stamp using
        // that 1 as the preserved low nibble -> (1/3)<<4 + 0x10 | 1 == 0x11.
        ck_eq((uint32_t)mh::state::tile_overlay(planes, 5, 6), 0x11u,
              "T2: (5,6) carries step 2's pre-move stamp over step 1's OR stamp, 0x0042ebd3");
        // (5,7): only OR-stamped in step 2, never pre-move-stamped (the walk ends here) -> 1.
        ck_eq((uint32_t)mh::state::tile_overlay(planes, 5, 7), 0x1u,
              "T2: (5,7) carries only its OR stamp, 0x0042ed27");
    }

    // T3: the walk STOPS at the first tile whose flags[1] bit 0x80 is clear, reporting THAT tile --
    // and the post-move OR stamp (0x0042ecf1) must NOT run for it, only the pre-move stamp on the
    // tile stepped FROM.
    {
        tact_fixture fx;
        seed_visible(fx, 2, 2, true);
        seed_visible(fx, 1, 2, false);                          // the tile the walk steps onto and must stop at
        fx.tile_objects[(size_t)((1 << 8) | 2)].unit[1] = 0x55; // sentinel: must survive untouched
        mh::state::mode_planes planes                   = fx.planes();
        planes.path_waypoint_at(0, 3, 0).heading        = 7; // col - 1
        planes.path_waypoint_at(0, 3, 0).run_length     = 3; // more than the walk can take
        planes.path_waypoint_at(0, 3, 1).heading        = 0;
        int32_t out_col = -1, out_row = -1;
        detail::move_path_preview_walk(planes, 2, 2, 3, &out_col, &out_row);

        ck_eq((uint32_t)out_col, 1u, "T3: stops at the first not-visible tile, col 2 -> 1");
        ck_eq((uint32_t)out_row, 2u, "T3: row unchanged by heading 7");
        ck_eq((uint32_t)mh::state::tile_overlay(planes, 2, 2), (uint32_t)((7 / 3) << 4) + 0x10,
              "T3: (2,2) still gets its pre-move stamp before the stop is discovered, 0x0042ec03");
        ck_eq((uint32_t)mh::state::tile_overlay(planes, 1, 2), 0x55u,
              "T3: the not-visible stop tile's overlay is UNTOUCHED, 0x0042ecdd stops before 0x0042ecf1");
    }

    // T4: a heading outside the 8-direction set is a no-op on position (the original's own
    // fallthrough, 0x0042ec43/0x0042ec48/0x0042ec53/0x0042ec6a/0x0042ec70) -- both stamps still
    // land, on the SAME tile, since "current" and "new" coincide when nothing moves.
    {
        tact_fixture fx;
        seed_visible(fx, 9, 9, true);
        mh::state::mode_planes planes               = fx.planes();
        planes.path_waypoint_at(0, 4, 0).heading    = 2; // not in {1,4,7,0xa,0xd,0x10,0x13,0x16}
        planes.path_waypoint_at(0, 4, 0).run_length = 1;
        planes.path_waypoint_at(0, 4, 1).heading    = 0;
        int32_t out_col = -1, out_row = -1;
        detail::move_path_preview_walk(planes, 9, 9, 4, &out_col, &out_row);

        ck_eq((uint32_t)out_col, 9u, "T4: an unrecognised heading never moves col");
        ck_eq((uint32_t)out_row, 9u, "T4: an unrecognised heading never moves row");
        // pre-move: (2/3)<<4 + 0x10 | 0 == 0x10; post-move (same tile): 0x10 | (2/3 + 1) == 0x11.
        ck_eq((uint32_t)mh::state::tile_overlay(planes, 9, 9), 0x11u,
              "T4: both stamps land on the unmoved tile, in order, 0x0042ec03 then 0x0042ed27");
    }
}

} // namespace mh::tact::test
