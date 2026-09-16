//
// sim_unit_path_queue_count_selftest.cpp -- `simtest` offline oracle for
// llm_strat_unit_path_queue_count (sim/sim_unit_path_queue_count.h/.cpp, RI-SIM / SIM1-G2).
//
// NOT SHADOWABLE (0 tracked write cells -- pure query, writes nothing) -- this offline oracle is
// its only evidence.
//
// Case (b) below is the load-bearing one: reimpl-verify.js independently confirmed a real, surprising
// finding about the original binary -- the two loop exits return DIFFERENT things. Exit A (the loop
// runs to max_len without an early exit) returns `i` (== max_len). Exit B (an empty waypoint slot
// found early) does NOT reload EAX with `i` -- the last value the original leaves in EAX on that path
// is `2 * flat_index` (a byte-offset address computation that fed the preceding CMP), not the loop
// counter the function's own name ("count") would suggest. Both readings were independently re-derived
// from the raw .asm bytes (translator + 3 adversarial reviewers), not copied from each other.
//
#include "sim/sim_unit_path_queue_count.h"

#include "sim_test_support.h"

namespace mh::sim::test {

void run_unit_path_queue_count_tests() {
    constexpr int32_t OWNER = 2;
    constexpr int32_t UNIT  = 7;

    // ---- (a) EXIT A: every slot in [0,max_len) is occupied -> returns max_len (the honest count). ---
    {
        sim_fixture fx;
        fx.reset();
        fx.group_order_owner = OWNER;
        for (int32_t i = 0; i < 4; ++i) {
            fx.path_buffers[(size_t)(OWNER * PATH_WAYPOINTS_PER_PLAYER +
                                     UNIT * PATH_WAYPOINTS_PER_SLOT + i)]
                .run_length = 1;
        }
        sim_view      v = fx.view();
        const int32_t r = detail::unit_path_queue_count(v, UNIT, 4);
        ck_eq((uint32_t)r, 4u, "unit_path_queue_count: max_len exhausted, no early exit -> max_len");
    }

    // ---- (b) EXIT B: slot 2 is empty (run_length==0) -> returns 2*flat_index(i=2), NOT i=2. ----------
    {
        sim_fixture fx;
        fx.reset();
        fx.group_order_owner = OWNER;
        // slots 0 and 1 occupied; slot 2 empty (default reset already zeroes run_length, so nothing
        // further to set for slot 2 itself) -- but seed 0/1 explicitly so the early exit is really
        // "first empty slot found", not "everything happened to be empty from the start".
        for (int32_t i = 0; i < 2; ++i) {
            fx.path_buffers[(size_t)(OWNER * PATH_WAYPOINTS_PER_PLAYER +
                                     UNIT * PATH_WAYPOINTS_PER_SLOT + i)]
                .run_length = 1;
        }
        sim_view      v          = fx.view();
        const int32_t r          = detail::unit_path_queue_count(v, UNIT, 10);
        const int32_t flat_index = OWNER * PATH_WAYPOINTS_PER_PLAYER + UNIT * PATH_WAYPOINTS_PER_SLOT + 2;
        ck_eq((uint32_t)r, (uint32_t)(2 * flat_index),
              "unit_path_queue_count: early exit at slot 2 -> 2*flat_index, NOT the slot count 2 -- "
              "an original-binary quirk, preserved literally");
    }

    // ---- (c) EXIT B at i=0 (the very first slot is already empty). -----------------------------------
    {
        sim_fixture fx;
        fx.reset();
        fx.group_order_owner = OWNER;
        // slot 0 stays empty (run_length==0) by fx.reset()'s zeroing -- no setup needed.
        sim_view      v          = fx.view();
        const int32_t r          = detail::unit_path_queue_count(v, UNIT, 10);
        const int32_t flat_index = OWNER * PATH_WAYPOINTS_PER_PLAYER + UNIT * PATH_WAYPOINTS_PER_SLOT + 0;
        ck_eq((uint32_t)r, (uint32_t)(2 * flat_index),
              "unit_path_queue_count: first slot already empty -> 2*flat_index(i=0), not 0");
    }

    // ---- (d) max_len == 0: the loop never runs at all -> EXIT A immediately, returns 0. --------------
    {
        sim_fixture fx;
        fx.reset();
        fx.group_order_owner = OWNER;
        sim_view      v      = fx.view();
        const int32_t r      = detail::unit_path_queue_count(v, UNIT, 0);
        ck_eq((uint32_t)r, 0u, "unit_path_queue_count: max_len==0 -> loop never runs, returns 0");
    }
}

} // namespace mh::sim::test
