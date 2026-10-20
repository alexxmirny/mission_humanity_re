//
// sim_path_slot_dist_selftest.cpp -- `simtest` offline oracle for llm_strat_slot_dist_to_ref
// (sim/sim_path_slot_dist.h/.cpp, RI-SIM / SIM1-G2).
//
// NOT SHADOWABLE (0 tracked write cells -- pure query, writes nothing) -- this offline oracle is
// its only evidence. llm_strat_claim_free_slots_within_dist (the other function in this TU) IS
// rig-armed (the shadow manifest / the per-call shadow arm's output).
//
// Uses a MOCK sqrt (std::sqrt) rather than live_slot_dist_to_ref_calls() -- the live calls struct
// invokes the ORIGINAL game function at its fixed VA (mh::call::llm_sqrt), which only exists inside
// the actual injected mh.exe process; net_selftest.exe has no such address mapped, so calling it here
// segfaults (caught by actually running this suite, not just building it, 2026-08-20/21). std::sqrt is
// the same IEEE-754 double sqrt, so the expected values are identical.
//
#include "sim/sim_path_slot_dist.h"
#include "sim/sim_move_fixes.h" // mods:M1H lane B: case (e) drives move_centroid_wrap_fix
#include "sim/move/planner_hits.h"

#include "sim_test_support.h"

#include <cmath>

namespace mh::sim::test {
namespace {
const slot_dist_to_ref_calls &mock_calls() {
    static const slot_dist_to_ref_calls c = {[](double x) -> double { return std::sqrt(x); }};
    return c;
}
} // namespace

void run_path_slot_dist_tests() {
    // ---- (a) simple 3-4-5 triangle, no wrap: TRUNCATED to an exact integer. --------------------------
    {
        sim_fixture fx;
        fx.reset();
        sim_store own                         = fx.store();
        fx.group_move_scratch[3].tile_col     = 13;
        fx.group_move_scratch[3].tile_row     = 24;
        own.group_move_dist_ref_x_mut()       = 10;
        own.group_move_dist_ref_y_mut()       = 20;
        own.group_move_dist_half_width_mut()  = 100;
        own.group_move_dist_half_height_mut() = 100;
        fx.map_width                          = 1000;
        fx.map_height                         = 1000;
        sim_view      v                       = fx.view();
        const int32_t r =
            detail::slot_dist_to_ref(v, own, mock_calls(), 3);
        ck_eq((uint32_t)r, 5u, "slot_dist_to_ref: dx=3,dy=4 (3-4-5 triangle) -> 5");
    }

    // ---- (b) negative deltas (ref ahead of the slot on both axes) square the same way. ---------------
    {
        sim_fixture fx;
        fx.reset();
        sim_store own                         = fx.store();
        fx.group_move_scratch[7].tile_col     = 10;
        fx.group_move_scratch[7].tile_row     = 20;
        own.group_move_dist_ref_x_mut()       = 13;
        own.group_move_dist_ref_y_mut()       = 24;
        own.group_move_dist_half_width_mut()  = 100;
        own.group_move_dist_half_height_mut() = 100;
        fx.map_width                          = 1000;
        fx.map_height                         = 1000;
        sim_view      v                       = fx.view();
        const int32_t r =
            detail::slot_dist_to_ref(v, own, mock_calls(), 7);
        ck_eq((uint32_t)r, 5u, "slot_dist_to_ref: dx=-3,dy=-4 -> 5 (squares regardless of sign)");
    }

    // ---- (c) TRUNC, not round: dist=sqrt(8)=2.828 must come back 2, not 3. --------------------------
    {
        sim_fixture fx;
        fx.reset();
        sim_store own                         = fx.store();
        fx.group_move_scratch[0].tile_col     = 2;
        fx.group_move_scratch[0].tile_row     = 2;
        own.group_move_dist_ref_x_mut()       = 0;
        own.group_move_dist_ref_y_mut()       = 0;
        own.group_move_dist_half_width_mut()  = 100;
        own.group_move_dist_half_height_mut() = 100;
        fx.map_width                          = 1000;
        fx.map_height                         = 1000;
        sim_view      v                       = fx.view();
        const int32_t r =
            detail::slot_dist_to_ref(v, own, mock_calls(), 0);
        ck_eq((uint32_t)r, 2u, "slot_dist_to_ref: sqrt(8)=2.828 truncates to 2, not rounds to 3");
    }

    // ---- (d) the toroidal wrap: dx > half_width on the X axis subtracts the FULL map_width, leaving a
    // SHORT wrapped delta instead of the raw (large, unwrapped) one. -----------------------------------
    {
        sim_fixture fx;
        fx.reset();
        sim_store own                         = fx.store();
        fx.group_move_scratch[9].tile_col     = 250;
        fx.group_move_scratch[9].tile_row     = 5;
        own.group_move_dist_ref_x_mut()       = 0;
        own.group_move_dist_ref_y_mut()       = 5; // dy=0, isolate the X-axis wrap
        own.group_move_dist_half_width_mut()  = 50;
        own.group_move_dist_half_height_mut() = 100;
        fx.map_width                          = 256; // dx = 250 - 0 = 250 > half_width(50) -> dx -= map_width(256) -> dx=-6
        fx.map_height                         = 1000;
        sim_view      v                       = fx.view();
        const int32_t r =
            detail::slot_dist_to_ref(v, own, mock_calls(), 9);
        ck_eq((uint32_t)r, 6u,
              "slot_dist_to_ref: X-axis wrap (dx=250>half_width=50) -> wrapped dx=-6, dist=6, "
              "NOT the raw unwrapped 250");
    }

    // ---- (e) [net] move_centroid_wrap_fix (mods:M1H lane B; the libmh twin of the retail splice at 0x004cbdb0 / 0x004cbdc3, rule in
    // sim/move/wrap.h): retail folds only a delta ABOVE +half; with the key on a delta BELOW -half is folded the other way too.
    {
        const move_fixes saved = move_fix();
        auto             run   = [&](bool on, int32_t col, int32_t row, int32_t ref_x, int32_t ref_y) {
            sim_fixture fx;
            fx.reset();
            sim_store own                         = fx.store();
            fx.group_move_scratch[2].tile_col     = col;
            fx.group_move_scratch[2].tile_row     = row;
            own.group_move_dist_ref_x_mut()       = ref_x;
            own.group_move_dist_ref_y_mut()       = ref_y;
            own.group_move_dist_half_width_mut()  = 100;
            own.group_move_dist_half_height_mut() = 40;
            fx.map_width                          = 256;
            fx.map_height                         = 128;
            move_fixes mf;
            mf.centroid_wrap_fix = on;
            set_move_fixes(mf);
            sim_view v = fx.view();
            return (uint32_t)detail::slot_dist_to_ref(v, own, mock_calls(), 2);
        };
        mh::sim::move::planner_hits_reset();
        ck_eq(run(false, 5, 7, 250, 7), 245u, "slot_dist_to_ref (e): key off: dx=-245 (< -half) is NOT folded (retail): dist 245");
        ck_eq(mh::sim::move::planner_hit_count(mh::sim::move::PH_SLOT_X), 0u, "slot_dist_to_ref (e): key off counts no hit");
        ck_eq(run(true, 5, 7, 250, 7), 11u, "slot_dist_to_ref (e): key on: dx=-245 folds by +256 -> 11");
        ck_eq(mh::sim::move::planner_hit_count(mh::sim::move::PH_SLOT_X), 1u, "slot_dist_to_ref (e): ... and counts the x hit");
        ck_eq(run(false, 3, 3, 3, 120), 117u, "slot_dist_to_ref (e): key off, y axis: dy=-117 (< -half) stays");
        ck_eq(run(true, 3, 3, 3, 120), 11u, "slot_dist_to_ref (e): key on, y axis: dy=-117 folds by +128 -> 11");
        ck_eq(mh::sim::move::planner_hit_count(mh::sim::move::PH_SLOT_Y), 1u, "slot_dist_to_ref (e): ... and counts the y hit");
        ck_eq(run(true, 250, 7, 5, 7), 11u, "slot_dist_to_ref (e): key on, the positive side (dx=245 > half) folds as in retail -> 11");
        ck_eq(run(false, 250, 7, 5, 7), 11u, "slot_dist_to_ref (e): ... and key off is the same");
        ck_eq(run(true, 0, 7, 100, 7), 100u, "slot_dist_to_ref (e): key on, dx == -half is not folded (strictly below)");
        ck_eq(run(true, 20, 7, 10, 7), 10u, "slot_dist_to_ref (e): key on, a delta inside the half extent is untouched");
        set_move_fixes(saved);
    }
}

} // namespace mh::sim::test
