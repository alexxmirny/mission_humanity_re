//
// tact_fov_raycast_selftest.cpp -- offline oracle for llm_tact_fov_raycast_stencil (TACT1E,
// 2026-08-28). See tact/tact_fov.h for the derivation and the `uint8_t col/row` wrap fix this
// oracle exists to pin.
//
// ---------------------------------------------------------------------------------------------
// HISTORY: T1-T3 WERE WRITTEN WHILE THE DELTA TABLES WERE UNMOCKABLE. THAT IS FIXED. READ ON.
// ---------------------------------------------------------------------------------------------
//
// tact_fov.cpp's select_fov_ray_plan() used to return a pointer built from a RAW, HARDCODED
// ABSOLUTE ADDRESS (TACT_FOV_DELTA_TABLE_{72,120,360}_ADDR = 0x00559428 / 0x005594b8 /
// 0x005595a8 -- the three .rdata delta tables), dereferenced directly
// (`plan.table[angle_index*2]`, `[...+1]`) on EVERY ray, unconditionally, before the first wall
// check ever ran. `net_selftest.exe tacttest` is a standalone process (net_selftest.cpp's own
// header: "no game needed") -- mh.exe is never loaded into it, so those addresses used to read two
// bytes of net_selftest.exe's OWN COMPILED CODE, unstable across every relink. That blocked any
// case from controlling ray_dx/ray_dy, so T1-T3 below (first authored under that constraint) stick
// to what was observable WITHOUT the DDA stepping ever running for real: the trackers'
// unconditional per-cast reset, the stencil zero-fill + always-marked center cell, the FIRST wall
// check (which fires before ray_dx is ever used), and the three-tier table-SELECTION cascade.
// Their own case comments still explain this reasoning; it is accurate for T1-T3 and left as-is.
//
// FIXED 2026-08-28: select_fov_ray_plan is now `select_fov_ray_plan(const tact_view &v,
// int32_t vision_dist)`, returning `v.fov_delta_table_72/_120/_360` -- view-bound, exactly like
// every sibling table (dir8_delta_table, snd_cfg_table, ...) -- and tact_test_support.h grew
// matching seedable fixture vectors of the same names. T4 onward (below T3) seed these directly
// and drive the REAL DDA stepping: the map-edge wrap (the whole reason this oracle exists -- see
// the file's opening line), all four X/Y sub-step arms, the stencil index arithmetic tied to a
// step, the no-carry case, and the fov_update_nearest_target unit==0 vacuity trap. See the second
// banner, just above `run_fov_raycast_tests()`'s T4, for the carry-timing trick they all use.
//
#include "tact/tact_fov.h"
#include "tact_test_support.h"

namespace mh::tact::test {

namespace {

// Mirrors tact_fov.cpp's own anonymous-namespace constants (not exposed via the header): the
// stencil's 64x64 shape and its fixed center index, 31*64+31.
constexpr int32_t FOV_STENCIL_SLOTS  = 64 * 64;
constexpr int32_t FOV_STENCIL_CENTER = 31 * 64 + 31; // 0x7df, @0x00437005

// A distinctive, non-{0,1} sentinel so an untouched-by-the-zero-fill stencil slot is observably
// different from both "correctly zeroed" (0) and "the center mark" (1).
constexpr uint8_t POISON_STENCIL = 200;

void poison_stencil(tact_fixture &fx) {
    for (auto &b : fx.fov_stencil) b = POISON_STENCIL;
}

// Blocks EVERY ray at its own starting tile via the FIRST wall check (@0x0043705e-0x00437065),
// which fires before ray_dx/ray_dy are ever used for stepping -- see the file banner. With this
// set, a case needs no knowledge of the corrupted table read's content.
void wall_off_start_tile(mh::state::mode_planes &p, int32_t col, int32_t row) {
    p.tile_object_at(col, row).flags[1] |= 0x20;
}

// EXTENDED 2026-08-28: the delta-table blocker above is FIXED -- select_fov_ray_plan() now reads
// v.fov_delta_table_72/_120/_360 (view-bound, fixture-seedable), not a raw absolute address. T4
// onward below seed these directly to drive real, controlled DDA stepping -- the cases the banner
// above said were impossible. All of them park in the 72-ray tier (fov_dist <= 0xc) with
// fov_angle_base=0, fov_angle_width=0 so exactly ONE ray fires at angle_index==0 (angle_start =
// 0 - 0/2 = 0, not negative, so no @0x00436fc6-dd wrap is needed either) -- the single ray reads
// fov_delta_table_72[0]/[1] as its {dx,dy}, which every case below seeds explicitly.
//
// THE CARRY-TIMING TRICK: bl_accum/bh_accum are uint8_t and reset to 0 at the top of each ray
// (see the file banner above). With a per-step magnitude M, accum after step k (before any carry
// resets it) is exactly (k+1)*M mod 256, and a carry fires exactly when floor(k*M/256) increases
// -- i.e. a PERIODIC, fully deterministic schedule once M is fixed. Two magnitudes are used
// throughout: M=128 (0x80) carries on every 2nd step starting at step index 1 (steps 1,3,5,...),
// and M=127 (0x7f, the largest magnitude reachable in the POSITIVE branch, since the raw byte must
// stay non-negative to avoid the `& 0x80` sign check taking the other arm) carries starting at
// step index 2 (steps 2,4,6,...). Every case below states which schedule it relies on inline.
//
// assert_stencil_exact checks the WHOLE 4096-slot stencil against an exact expected-on set in one
// pass (both directions: nothing outside the set may be 1, nothing inside may be 0) -- stronger
// than sampling, and cheap.
void assert_stencil_exact(tact_store &own, std::initializer_list<int32_t> expected_on,
                          const char *what) {
    bool want[FOV_STENCIL_SLOTS] = {};
    for (int32_t idx : expected_on) want[(size_t)idx] = true;
    int32_t mismatches = 0;
    for (int32_t i = 0; i < FOV_STENCIL_SLOTS; ++i) {
        const bool got = own.fov_stencil_at(i) != 0;
        if (got != want[(size_t)i]) ++mismatches;
    }
    ck_eq((uint32_t)mismatches, 0u, what);
}

} // namespace

void run_fov_raycast_tests() {
    // T1: the per-cast reset of the nearest-target trackers (@0x00436f31-0x00436f4d) is
    // unconditional -- and since every tile_object.unit[0] is 0 in this fixture by default (no
    // seeded unit anywhere in the 256x256 grid), fov_update_nearest_target can never fire for ANY
    // cell a ray happens to touch (its own guard: `if (tile_object_at(...).unit[0] != 0)`), so the
    // reset values are also the FINAL values -- independent of where the (uncontrollable) ray
    // stepping actually goes. Deliberately a non-trivial step budget (fov_dist=6) to make the
    // point that this holds regardless of how much stepping happens.
    {
        tact_fixture fx;
        fx.fov_nearest_hibit_cell = 111;
        fx.fov_nearest_low_cell   = 222;
        fx.fov_nearest_hibit_dist = 33;
        fx.fov_nearest_low_dist   = 44;
        fx.fov_col                = 10;
        fx.fov_row                = 20;
        fx.fov_angle_base         = 0;
        fx.fov_angle_width        = 8;
        fx.fov_dist               = 6;

        tact_view              v   = fx.view();
        tact_store             own = fx.store();
        mh::state::mode_planes p   = fx.planes();
        detail::fov_raycast_stencil(v, own, p);

        ck_eq((uint32_t)(uint16_t)own.fov_nearest_hibit_cell(), 0u,
              "T1: fov_nearest_hibit_cell reset to 0, 0x00436f3a");
        ck_eq((uint32_t)(uint16_t)own.fov_nearest_low_cell(), 0u,
              "T1: fov_nearest_low_cell reset to 0, 0x00436f31");
        ck_eq((uint32_t)own.fov_nearest_hibit_dist(), 0xffu,
              "T1: fov_nearest_hibit_dist reset to 0xff, 0x00436f43");
        ck_eq((uint32_t)own.fov_nearest_low_dist(), 0xffu,
              "T1: fov_nearest_low_dist reset to 0xff, 0x00436f4d");
    }

    // T2: the 64x64 stencil zero-fill (@0x00436fab-0x00436fb2), the unconditional center mark
    // (@0x00437005), and the FIRST wall check (@0x0043705e-0x00437065) stopping every ray dead at
    // its own starting tile -- "no further cell is marked" checked EXHAUSTIVELY (all other 4095
    // slots), not by sampling. Also proves this function never writes flags (only reads flags[1]).
    {
        tact_fixture fx;
        poison_stencil(fx);
        fx.fov_col         = 100;
        fx.fov_row         = 150;
        fx.fov_angle_base  = 0;
        fx.fov_angle_width = 40; // several rays -- all must be blocked identically
        fx.fov_dist        = 9;  // a real step budget the wall must preempt entirely

        tact_view              v   = fx.view();
        tact_store             own = fx.store();
        mh::state::mode_planes p   = fx.planes();
        wall_off_start_tile(p, fx.fov_col, fx.fov_row);
        p.tile_object_at(fx.fov_col, fx.fov_row).flags[0] = 0x11; // must survive untouched

        detail::fov_raycast_stencil(v, own, p);

        ck_eq((uint32_t)own.fov_stencil_at(FOV_STENCIL_CENTER), 1u,
              "T2: center cell (0x7df=31*64+31) always marked, 0x00437005");
        int32_t bad = 0;
        for (int32_t i = 0; i < FOV_STENCIL_SLOTS; ++i) {
            if (i == FOV_STENCIL_CENTER) continue;
            if (own.fov_stencil_at(i) != 0) ++bad;
        }
        ck_eq((uint32_t)bad, 0u,
              "T2: every non-center slot is 0 (zero-fill @0x00436fab-b2; no ray got past the wall "
              "check @0x0043705e)");
        ck_eq((uint32_t)p.tile_object_at(fx.fov_col, fx.fov_row).flags[0], 0x11u,
              "T2: flags[0] untouched -- fov_raycast_stencil never writes flags");
    }

    // T3a: vision_dist==0xc, the LOWER boundary (JBE @0x00436f6e keeps it in the 72-ray tier).
    {
        tact_fixture fx;
        fx.fov_col         = 5;
        fx.fov_row         = 5;
        fx.fov_angle_base  = 0;
        fx.fov_angle_width = 20; // divisible by every angle_step used below (5, 3, 1)
        fx.fov_dist        = 0xc;

        tact_view              v   = fx.view();
        tact_store             own = fx.store();
        mh::state::mode_planes p   = fx.planes();
        wall_off_start_tile(p, fx.fov_col, fx.fov_row);
        const int32_t angle_width_in = fx.fov_angle_width;
        detail::fov_raycast_stencil(v, own, p);

        ck_eq((uint32_t)own.fov_angle_width(), (uint32_t)(angle_width_in / 5) + 1u,
              "T3a: vision_dist==0xc (boundary, JBE) -> table_72/angle_step=5, "
              "0x00436f6b/0x00436f6e, width overwrite 0x00436fee-0x00437000");
    }

    // T3b: vision_dist==0xd, one past the 0xc boundary -> the 120-ray tier.
    {
        tact_fixture fx;
        fx.fov_col         = 5;
        fx.fov_row         = 5;
        fx.fov_angle_base  = 0;
        fx.fov_angle_width = 20;
        fx.fov_dist        = 0xd;

        tact_view              v   = fx.view();
        tact_store             own = fx.store();
        mh::state::mode_planes p   = fx.planes();
        wall_off_start_tile(p, fx.fov_col, fx.fov_row);
        const int32_t angle_width_in = fx.fov_angle_width;
        detail::fov_raycast_stencil(v, own, p);

        ck_eq((uint32_t)own.fov_angle_width(), (uint32_t)(angle_width_in / 3) + 1u,
              "T3b: vision_dist==0xd (just past the 0xc boundary) -> table_120/angle_step=3, "
              "0x00436f70-0x00436f7f");
    }

    // T3c: vision_dist==0x14, the UPPER boundary (JBE @0x00436f82 keeps it in the 120-ray tier).
    {
        tact_fixture fx;
        fx.fov_col         = 5;
        fx.fov_row         = 5;
        fx.fov_angle_base  = 0;
        fx.fov_angle_width = 20;
        fx.fov_dist        = 0x14;

        tact_view              v   = fx.view();
        tact_store             own = fx.store();
        mh::state::mode_planes p   = fx.planes();
        wall_off_start_tile(p, fx.fov_col, fx.fov_row);
        const int32_t angle_width_in = fx.fov_angle_width;
        detail::fov_raycast_stencil(v, own, p);

        ck_eq((uint32_t)own.fov_angle_width(), (uint32_t)(angle_width_in / 3) + 1u,
              "T3c: vision_dist==0x14 (boundary, JBE) -> table_120/angle_step=3, "
              "0x00436f7f/0x00436f82");
    }

    // T3d: vision_dist==0x15, one past the 0x14 boundary -> the 360-ray tier.
    {
        tact_fixture fx;
        fx.fov_col         = 5;
        fx.fov_row         = 5;
        fx.fov_angle_base  = 0;
        fx.fov_angle_width = 20;
        fx.fov_dist        = 0x15;

        tact_view              v   = fx.view();
        tact_store             own = fx.store();
        mh::state::mode_planes p   = fx.planes();
        wall_off_start_tile(p, fx.fov_col, fx.fov_row);
        const int32_t angle_width_in = fx.fov_angle_width;
        detail::fov_raycast_stencil(v, own, p);

        ck_eq((uint32_t)own.fov_angle_width(), (uint32_t)(angle_width_in / 1) + 1u,
              "T3d: vision_dist==0x15 (just past the 0x14 boundary) -> table_360/angle_step=1, "
              "0x00436f84-0x00436f8e");
    }

    // T4 (FLAGSHIP): the map-edge wrap, WEST/negative-X arm (`ray_dx & 0x80` @0x0043706a taken,
    // DEC AH @0x0043709a). Single ray, table_72[0..1] seeded {dx=-128,dy=0} -> M=128 carry
    // schedule (carries at zero-based steps 1,3,5). fov_col starts at 1; three carries walk it
    // 1 -> 0 -> 255 (THE WRAP) -> 254, budget=7 (fov_dist=3) covers exactly those three.
    //
    // THIS CASE MUST FAIL IF `uint8_t col/row` IN fov_raycast_stencil IS REVERTED TO int32_t: the
    // second carry (1->0 is ordinary; 0->-1 is where int32_t and uint8_t diverge) would compute
    // tile_object_at(-1, 60) -> mode_planes.h's `tile_objects_[(tile_x<<8)|tile_y]` with
    // tile_x=-1 gives a large NEGATIVE index (a real heap-buffer-underflow, ASan-fatal) instead of
    // reading (255,60) -- so own.fov_nearest_hibit_cell() below would never become (255<<8)|60
    // (the process crashes first under ASan, or the assertion simply fails without it).
    {
        tact_fixture fx;
        fx.fov_col               = 1;
        fx.fov_row               = 60;
        fx.fov_angle_base        = 0;
        fx.fov_angle_width       = 0; // -> exactly 1 ray, angle_index==0 (see the banner above T4)
        fx.fov_dist              = 3; // step_budget = 2*3+1 = 7
        fx.fov_candidate_dist    = 17;
        fx.fov_delta_table_72[0] = (int8_t)0x80; // ray_dx = -128 -> negative (west) arm
        fx.fov_delta_table_72[1] = 0;            // ray_dy = 0 -> Y accumulator never carries

        tact_view              v   = fx.view();
        tact_store             own = fx.store();
        mh::state::mode_planes p   = fx.planes();
        // (255,60): the WRAPPED cell -- reached by the THIRD carry (0 -> 255). Hibit branch
        // (occ>=0x80) so it lands in a tracker pair the (254,60) seed below cannot collide with.
        p.tile_object_at(255, 60).unit[0] = 0x85;
        // (254,60): reached by the FOURTH step position after the wrap (255 -> 254, a perfectly
        // ordinary decrement) -- proves the walk continues correctly PAST the wrap, not just AT it.
        // Low branch (occ<=0x20) so it cannot collide with the hibit write above.
        p.tile_object_at(254, 60).unit[0] = 0x11;
        // (0,60) is visited too (the FIRST carry, 1->0) but is left unseeded (unit[0]==0):
        // 0 is representable identically in both a correct and a reverted implementation, so
        // seeding it would not discriminate the bug -- only the SECOND carry (0->255) does.

        detail::fov_raycast_stencil(v, own, p);

        ck_eq((uint32_t)(uint16_t)own.fov_nearest_hibit_cell(), (uint32_t)((255 << 8) | 60),
              "T4 FLAGSHIP: col wraps 0->255 (DEC AH @0x0043709a, carry#2) -> tile_object_at(255,60) "
              "read via unit!=0 @0x004370a3 + CALL @0x004370aa, NOT tile_object_at(-1,60)");
        ck_eq((uint32_t)own.fov_nearest_hibit_dist(), 17u,
              "T4: fov_candidate_dist(17) recorded for the wrapped cell, 0x00437138-0x0043715b");
        ck_eq((uint32_t)(uint16_t)own.fov_nearest_low_cell(), (uint32_t)((254 << 8) | 60),
              "T4: the walk continues normally past the wrap, col 255->254 (ordinary DEC AH, carry#3)");
        ck_eq((uint32_t)own.fov_nearest_low_dist(), 17u, "T4: fov_candidate_dist(17) at (254,60)");
        assert_stencil_exact(own, {2015, 1951, 1887},
                             "T4: stencil marks exactly {center, center-0x40, center-0x80} "
                             "(stencil_idx -= 0x40 per X carry @0x00437081/0x004370a0)");
    }

    // T5: the map-edge wrap, NORTH/negative-Y arm (`ray_dy & 0x80` @0x004370b8 taken, DEC AL
    // @0x004370e6). Symmetric to T4 on the row axis: table_72 seeded {dx=0,dy=-128}, fov_row
    // starts at 1, same M=128 schedule, same budget=7 -> row walks 1 -> 0 -> 255 (THE WRAP) -> 254.
    {
        tact_fixture fx;
        fx.fov_col               = 70;
        fx.fov_row               = 1;
        fx.fov_angle_base        = 0;
        fx.fov_angle_width       = 0;
        fx.fov_dist              = 3;
        fx.fov_candidate_dist    = 17;
        fx.fov_delta_table_72[0] = 0;            // ray_dx = 0 -> X accumulator never carries
        fx.fov_delta_table_72[1] = (int8_t)0x80; // ray_dy = -128 -> negative (north) arm

        tact_view              v          = fx.view();
        tact_store             own        = fx.store();
        mh::state::mode_planes p          = fx.planes();
        p.tile_object_at(70, 255).unit[0] = 0x85; // the WRAPPED cell (carry#2, row 0->255)
        p.tile_object_at(70, 254).unit[0] = 0x11; // one step past it (carry#3, ordinary 255->254)

        detail::fov_raycast_stencil(v, own, p);

        ck_eq((uint32_t)(uint16_t)own.fov_nearest_hibit_cell(), (uint32_t)((70 << 8) | 255),
              "T5: row wraps 0->255 (DEC AL @0x004370e6, carry#2) -> tile_object_at(70,255) read "
              "via unit!=0 @0x004370ef + CALL @0x004370f6, NOT tile_object_at(70,-1)");
        ck_eq((uint32_t)own.fov_nearest_hibit_dist(), 17u, "T5: fov_candidate_dist(17) at (70,255)");
        ck_eq((uint32_t)(uint16_t)own.fov_nearest_low_cell(), (uint32_t)((70 << 8) | 254),
              "T5: walk continues past the wrap, row 255->254 (ordinary DEC AL, carry#3)");
        ck_eq((uint32_t)own.fov_nearest_low_dist(), 17u, "T5: fov_candidate_dist(17) at (70,254)");
        assert_stencil_exact(own, {2015, 2014, 2013},
                             "T5: stencil marks exactly {center, center-1, center-2} "
                             "(stencil_idx -= 1 per Y carry @0x004370cf/0x004370ec)");
    }

    // T6: the map-edge wrap, the INC direction (positive-X arm, `ray_dx & 0x80` @0x0043706a NOT
    // taken, INC AH @0x0043707b). table_72 seeded {dx=+127,dy=0} -- 0x7f is the largest magnitude
    // reachable in the POSITIVE branch (a raw byte must stay non-negative to take this arm at
    // all), which carries on a DIFFERENT schedule than T4/T5's M=128 (zero-based steps 2,4,6 --
    // see the banner above T4). fov_col starts at 253; three carries walk it 253 -> 254 -> 255
    // (ordinary) -> 0 (THE WRAP), budget=7 (fov_dist=3) covers exactly those three.
    {
        tact_fixture fx;
        fx.fov_col               = 253;
        fx.fov_row               = 80;
        fx.fov_angle_base        = 0;
        fx.fov_angle_width       = 0;
        fx.fov_dist              = 3; // step_budget = 7; M=127 carries at steps 2,4,6
        fx.fov_candidate_dist    = 17;
        fx.fov_delta_table_72[0] = (int8_t)0x7f; // ray_dx = +127 -> positive (east) arm
        fx.fov_delta_table_72[1] = 0;

        tact_view              v   = fx.view();
        tact_store             own = fx.store();
        mh::state::mode_planes p   = fx.planes();
        // (255,80): reached by carry#2 (254->255, ordinary) -- low branch.
        p.tile_object_at(255, 80).unit[0] = 0x11;
        // (0,80): THE WRAPPED cell, reached by carry#3 (255->0) -- hibit branch, distinct pair.
        p.tile_object_at(0, 80).unit[0] = 0x85;

        detail::fov_raycast_stencil(v, own, p);

        ck_eq((uint32_t)(uint16_t)own.fov_nearest_low_cell(), (uint32_t)((255 << 8) | 80),
              "T6: col 254->255 (ordinary INC AH @0x0043707b, carry#2)");
        ck_eq((uint32_t)own.fov_nearest_low_dist(), 17u, "T6: fov_candidate_dist(17) at (255,80)");
        ck_eq((uint32_t)(uint16_t)own.fov_nearest_hibit_cell(), (uint32_t)((0 << 8) | 80),
              "T6: col wraps 255->0 (INC AH @0x0043707b, carry#3) -> tile_object_at(0,80) read via "
              "unit!=0 @0x00437084 + CALL @0x0043708b, NOT tile_object_at(256,80)");
        ck_eq((uint32_t)own.fov_nearest_hibit_dist(), 17u, "T6: fov_candidate_dist(17) at (0,80)");
        assert_stencil_exact(own, {2015, 2079, 2143},
                             "T6: stencil marks exactly {center, center+0x40, center+0x80} "
                             "(stencil_idx += 0x40 per X carry @0x00437081)");
    }

    // T7: the no-carry case -- table_72 left at its fixture DEFAULT (all-zero), so ray_dx=ray_dy=0
    // and bl_accum/bh_accum stay 0 forever (0+0 is never > 0xff). The start tile (50,90) is seeded
    // nonzero on purpose: if it were ever re-checked, fov_update_nearest_target WOULD fire -- so
    // the trackers staying at their reset values proves the carry-gated block (including the
    // unit!=0 check @0x00437084/0x004370a3/0x004370d0/0x004370ef) is never reached even once in a
    // real 11-step budget, i.e. the position genuinely never changes.
    {
        tact_fixture fx;
        fx.fov_col            = 50;
        fx.fov_row            = 90;
        fx.fov_angle_base     = 0;
        fx.fov_angle_width    = 0;
        fx.fov_dist           = 5; // step_budget = 11, generous -- still never carries
        fx.fov_candidate_dist = 17;
        // fov_delta_table_72 left at its fixture default (all-zero): dx=dy=0.

        tact_view              v         = fx.view();
        tact_store             own       = fx.store();
        mh::state::mode_planes p         = fx.planes();
        p.tile_object_at(50, 90).unit[0] = 0x85; // would fire the hibit branch IF ever re-checked

        detail::fov_raycast_stencil(v, own, p);

        ck_eq((uint32_t)(uint16_t)own.fov_nearest_hibit_cell(), 0u,
              "T7: no-carry (dx=dy=0) -> X sub-step @0x0043706a and Y sub-step @0x004370b8 never "
              "mutate col/row -- (50,90) never re-checked despite unit[0]!=0");
        ck_eq((uint32_t)own.fov_nearest_hibit_dist(), 0xffu, "T7: hibit_dist stays at its reset value");
        assert_stencil_exact(own, {2015},
                             "T7: stencil has ONLY the unconditional center mark, 0x00437005 -- no "
                             "X/Y sub-step ever wrote past it");
    }

    // T8: the last of the four sub-step arms, positive-Y (`ray_dy & 0x80` @0x004370b8 NOT taken,
    // INC AL @0x004370c9) -- a plain (non-wrapping) case, since T6 already covers the INC-direction
    // wrap. table_72 seeded {dx=0,dy=+127}, M=127 schedule (carries at steps 2,4), budget=5
    // (fov_dist=2). fov_row starts at 100; two carries walk it 100 -> 101 -> 102.
    {
        tact_fixture fx;
        fx.fov_col               = 40;
        fx.fov_row               = 100;
        fx.fov_angle_base        = 0;
        fx.fov_angle_width       = 0;
        fx.fov_dist              = 2; // step_budget = 5; M=127 carries at steps 2,4
        fx.fov_candidate_dist    = 17;
        fx.fov_delta_table_72[0] = 0;
        fx.fov_delta_table_72[1] = (int8_t)0x7f; // ray_dy = +127 -> positive (south) arm

        tact_view              v          = fx.view();
        tact_store             own        = fx.store();
        mh::state::mode_planes p          = fx.planes();
        p.tile_object_at(40, 101).unit[0] = 0x85; // carry#1 (100->101) -- hibit branch
        p.tile_object_at(40, 102).unit[0] = 0x11; // carry#2 (101->102) -- low branch

        detail::fov_raycast_stencil(v, own, p);

        ck_eq((uint32_t)(uint16_t)own.fov_nearest_hibit_cell(), (uint32_t)((40 << 8) | 101),
              "T8: row 100->101 (INC AL @0x004370c9, carry#1)");
        ck_eq((uint32_t)own.fov_nearest_hibit_dist(), 17u, "T8: fov_candidate_dist(17) at (40,101)");
        ck_eq((uint32_t)(uint16_t)own.fov_nearest_low_cell(), (uint32_t)((40 << 8) | 102),
              "T8: row 101->102 (INC AL @0x004370c9, carry#2)");
        ck_eq((uint32_t)own.fov_nearest_low_dist(), 17u, "T8: fov_candidate_dist(17) at (40,102)");
        assert_stencil_exact(own, {2015, 2016},
                             "T8: stencil marks exactly {center, center+1} "
                             "(stencil_idx += 1 per Y carry @0x004370cf)");
    }

    // T9: the VACUITY TRAP -- fov_update_nearest_target fires only when tile_object_at(col,row)
    // .unit[0] != 0 (@0x00437084/0x004370a3/0x004370d0/0x004370ef). T7 proves the guard's code is
    // unreached when there is no carry at all, which is a WEAKER claim than this one: here TWO
    // real carries genuinely happen (proven independently via the stencil, which is written
    // unconditionally on every carry, NOT gated by unit!=0), landing on cells whose unit[0] is left
    // at 0 -- and the trackers must still show nothing happened. A test that only checked "trackers
    // unchanged" without also proving real stepping occurred would pass vacuously even if the
    // unit!=0 guard were deleted outright (nothing would ever seed a nonzero unit[0] to notice).
    {
        tact_fixture fx;
        fx.fov_col               = 60;
        fx.fov_row               = 110;
        fx.fov_angle_base        = 0;
        fx.fov_angle_width       = 0;
        fx.fov_dist              = 2; // step_budget = 5; M=128 carries at steps 1,3 (two carries)
        fx.fov_candidate_dist    = 17;
        fx.fov_delta_table_72[0] = (int8_t)0x80; // ray_dx = -128 -> negative (west) arm
        fx.fov_delta_table_72[1] = 0;
        // (59,110) and (58,110), the two cells the carries land on, are left unseeded (unit[0]==0
        // by fixture default) -- deliberately, so the guard has something real to suppress.

        tact_view              v   = fx.view();
        tact_store             own = fx.store();
        mh::state::mode_planes p   = fx.planes();
        detail::fov_raycast_stencil(v, own, p);

        assert_stencil_exact(own, {2015, 1951},
                             "T9: two REAL carries happened (col 60->59->58, stencil_idx -= 0x40 "
                             "unconditionally on carry, 0x00437081/0x00437084) -- stepping is not "
                             "in question here, only the guard is");
        ck_eq((uint32_t)(uint16_t)own.fov_nearest_hibit_cell(), 0u,
              "T9: unit[0]==0 at both landed cells -> fov_update_nearest_target's own guard "
              "(@0x00437084/@0x004370a3) suppresses the call; hibit_cell stays at its reset value");
        ck_eq((uint32_t)own.fov_nearest_hibit_dist(), 0xffu, "T9: hibit_dist stays at its reset value");
        ck_eq((uint32_t)(uint16_t)own.fov_nearest_low_cell(), 0u, "T9: low_cell stays at its reset value");
        ck_eq((uint32_t)own.fov_nearest_low_dist(), 0xffu, "T9: low_dist stays at its reset value");
    }

    // ---------------------------------------------------------------------------------------------
    // T10-T11: TACT1D's still-open acceptance clause -- "a case proves the shared-scratch hazard is
    // not live in our version: the nearest-target globals hold the values the AI read expects after a
    // vision_cone_setup call is interleaved." fov_nearest_{low,hibit}_{cell,dist} are SHARED SCRATCH
    // (own.fov_nearest_*(), aliasing the four globals RESET UNCONDITIONALLY on every entry at
    // @0x00436f31-0x00436f4d, with no per-caller isolation): both casts in each case below run
    // through the SAME tact_fixture/tact_view/tact_store/mode_planes -- i.e. the same "globals" --
    // via TWO interleaved calls to detail::vision_cone_setup (@0x0042e1c9, which stashes its five
    // scratch args @0x0042e1ed-0x0042e20d then falls straight into fov_raycast_stencil via the plain
    // same-TU call @0x0042e212 -> @0x00436f30; see tact_fov.h/.cpp). This is the exact pipeline the
    // clause names, not a stand-in for it.
    // ---------------------------------------------------------------------------------------------

    // T10: cast A leaves distinctive, non-default, non-reset values in ALL FOUR trackers; cast B (a
    // DIFFERENT cone -- different position, opposite-sign delta, different candidate_dist, and
    // therefore different cells) is interleaved on the SAME store/view before anything reads cast
    // A's result. The final globals must hold ONLY cast B's values in every one of the four fields --
    // proving the reset+refill at @0x00436f31-0x00436f4d fully overwrites rather than merging with or
    // leaking the prior caller's result. If cast B's reset/refill were skipped entirely, the globals
    // would still read cast A's values below; if it only partially applied (e.g. hibit refilled but
    // low left stale), the low_cell/low_dist checks below would catch that on their own.
    {
        tact_fixture           fx;
        tact_view              v   = fx.view();
        tact_store             own = fx.store();
        mh::state::mode_planes p   = fx.planes();

        // ---- cast A: negative-X (west) arm, 3 carries (M=128 schedule, see the banner above T4) --
        // col walks 50->49(carry1,hibit)->48(carry2,unseeded)->47(carry3,low).
        fx.fov_delta_table_72[0]          = (int8_t)0x80; // ray_dx=-128 -> negative arm, 0x0043706a/0x00437092
        fx.fov_delta_table_72[1]          = 0;
        fx.fov_candidate_dist             = 17;
        p.tile_object_at(49, 200).unit[0] = 0x85; // hibit branch
        p.tile_object_at(47, 200).unit[0] = 0x11; // low branch
        detail::vision_cone_setup(v, own, p, /*col=*/50, /*row=*/200, /*angle_base=*/0,
                                  /*angle_width=*/0, /*vision_dist=*/3, nullptr, nullptr, nullptr,
                                  nullptr); // cast A: @0x0042e1c9 -> @0x00436f30

        ck_eq((uint32_t)(uint16_t)own.fov_nearest_hibit_cell(), (uint32_t)((49 << 8) | 200),
              "T10 cast A: hibit_cell=(49,200) (DEC AH @0x0043709a, negative-X arm)");
        ck_eq((uint32_t)own.fov_nearest_hibit_dist(), 17u,
              "T10 cast A: hibit_dist=candidate_dist(17), 0x00437138-0x0043715b");
        ck_eq((uint32_t)(uint16_t)own.fov_nearest_low_cell(), (uint32_t)((47 << 8) | 200),
              "T10 cast A: low_cell=(47,200)");
        ck_eq((uint32_t)own.fov_nearest_low_dist(), 17u, "T10 cast A: low_dist=candidate_dist(17)");

        // ---- cast B, INTERLEAVED on the same v/own/p: positive-X (east) arm, 2 carries (M=127
        // schedule, see the banner above T4/T8). Different position, opposite delta sign, different
        // candidate_dist -- so a leaked cast-A value or a skipped/partial reset is distinguishable
        // from a correct overwrite in every one of the four fields, not just one.
        fx.fov_delta_table_72[0]          = (int8_t)0x7f; // ray_dx=+127 -> positive arm, 0x00437073/0x0043707b
        fx.fov_delta_table_72[1]          = 0;
        fx.fov_candidate_dist             = 5;    // deliberately != cast A's 17
        p.tile_object_at(151, 80).unit[0] = 0x85; // hibit branch
        p.tile_object_at(152, 80).unit[0] = 0x11; // low branch
        detail::vision_cone_setup(v, own, p, /*col=*/150, /*row=*/80, /*angle_base=*/0,
                                  /*angle_width=*/0, /*vision_dist=*/2, nullptr, nullptr, nullptr,
                                  nullptr); // cast B, INTERLEAVED: reset @0x00436f31-0x00436f4d fires again

        ck_eq((uint32_t)(uint16_t)own.fov_nearest_hibit_cell(), (uint32_t)((151 << 8) | 80),
              "T10: after the interleaved cast B, hibit_cell holds B's (151,80), NOT A's (49,200) -- "
              "reset @0x00436f3a + refill @0x00437084/0x0043708b, no leak from cast A");
        ck_eq((uint32_t)own.fov_nearest_hibit_dist(), 5u,
              "T10: hibit_dist holds B's candidate_dist(5), NOT A's 17");
        ck_eq((uint32_t)(uint16_t)own.fov_nearest_low_cell(), (uint32_t)((152 << 8) | 80),
              "T10: low_cell holds B's (152,80), NOT A's (47,200) -- reset @0x00436f31 + refill "
              "@0x00437084/0x0043708b");
        ck_eq((uint32_t)own.fov_nearest_low_dist(), 5u,
              "T10: low_dist holds B's candidate_dist(5), NOT A's 17");
    }

    // T11: the arm the acceptance clause is really about -- the reset fires UNCONDITIONALLY at
    // @0x00436f31-0x00436f4d (it is not gated behind the cast finding anything), so an interleaved
    // cast that finds NOTHING must still leave the globals at their reset values (0/0/0xff/0xff), NOT
    // cast A's leftovers. A naive oracle would gate its check behind "did this cast hit something"
    // and pass vacuously even if the reset were made conditional or deleted outright. Cast B uses
    // dx=dy=0 (T7's proven no-carry setup) so it is a REAL, unconditional zero-hit cast -- the
    // position never even changes across its full step budget -- not merely one that happens not to
    // find a seeded unit.
    {
        tact_fixture           fx;
        tact_view              v   = fx.view();
        tact_store             own = fx.store();
        mh::state::mode_planes p   = fx.planes();

        // cast A: identical to T10's, distinctive non-default values in all four trackers -- the
        // leftover the reset below must clear.
        fx.fov_delta_table_72[0]          = (int8_t)0x80;
        fx.fov_delta_table_72[1]          = 0;
        fx.fov_candidate_dist             = 17;
        p.tile_object_at(49, 200).unit[0] = 0x85;
        p.tile_object_at(47, 200).unit[0] = 0x11;
        detail::vision_cone_setup(v, own, p, 50, 200, 0, 0, 3, nullptr, nullptr, nullptr, nullptr);

        ck_eq((uint32_t)(uint16_t)own.fov_nearest_hibit_cell(), (uint32_t)((49 << 8) | 200),
              "T11 cast A: hibit_cell=(49,200), sets up the leftover the reset must clear");
        ck_eq((uint32_t)own.fov_nearest_hibit_dist(), 17u, "T11 cast A: hibit_dist=17");
        ck_eq((uint32_t)(uint16_t)own.fov_nearest_low_cell(), (uint32_t)((47 << 8) | 200),
              "T11 cast A: low_cell=(47,200)");
        ck_eq((uint32_t)own.fov_nearest_low_dist(), 17u, "T11 cast A: low_dist=17");

        // cast B, INTERLEAVED: dx=dy=0 -> zero carries, zero calls into fov_update_nearest_target,
        // over a real 11-step budget (fov_dist=5) at a position with no seeded units anywhere near
        // it. candidate_dist is set to a third, distinct decoy value (42, != 17 and != 0xff) that
        // must never surface below -- if it did, cast B secretly recorded a hit it should not have.
        fx.fov_delta_table_72[0] = 0;
        fx.fov_delta_table_72[1] = 0;
        fx.fov_candidate_dist    = 42;
        detail::vision_cone_setup(v, own, p, /*col=*/90, /*row=*/90, /*angle_base=*/0,
                                  /*angle_width=*/0, /*vision_dist=*/5, nullptr, nullptr, nullptr,
                                  nullptr); // cast B, INTERLEAVED, no-hit -- reset still fires @0x00436f31-4d

        ck_eq((uint32_t)(uint16_t)own.fov_nearest_hibit_cell(), 0u,
              "T11: cast B finds NOTHING -> hibit_cell reads back its RESET value 0 (0x00436f3a), "
              "NOT cast A's leftover (49,200) -- the reset is unconditional, not gated on a hit");
        ck_eq((uint32_t)own.fov_nearest_hibit_dist(), 0xffu,
              "T11: hibit_dist reads back its RESET value 0xff (0x00436f43), NOT cast A's 17");
        ck_eq((uint32_t)(uint16_t)own.fov_nearest_low_cell(), 0u,
              "T11: low_cell reads back its RESET value 0 (0x00436f31), NOT cast A's leftover (47,200)");
        ck_eq((uint32_t)own.fov_nearest_low_dist(), 0xffu,
              "T11: low_dist reads back its RESET value 0xff (0x00436f4d), NOT cast A's 17");
    }
}

} // namespace mh::tact::test
