//
// sim_path_solver_selftest.cpp -- `simtest` offline oracle for llm_strat_path_find_free_slot
// (sim/sim_path_solver.h/.cpp, RI-SIM / SIM1-G2).
//
// NOT SHADOWABLE (0 tracked write cells -- pure query, writes nothing) -- this offline oracle is
// its only evidence. llm_strat_path_write_from_solver (the other function in this TU) IS rig-armed
// (the shadow manifest / the per-call shadow arm's output, SIM1-G2).
//
#include "sim/sim_path_solver.h"

#include "sim_test_support.h"

namespace mh::sim::test {

void run_path_solver_tests() {
    // ---- (a) all 100 slots occupied for the player under test -> -1, and a DIFFERENT player's own
    // all-free row is untouched by the scan (proves the player stride, not just the scan). ----------
    {
        sim_fixture fx;
        fx.reset();
        sim_store own = fx.store();
        for (int32_t i = 0; i < 100; ++i) own.path_slot_flag_at(2, i) = 1;
        // player 3's row stays all-zero (free) -- must not leak into player 2's answer.
        const int32_t r = detail::path_find_free_slot(own, 2);
        ck_eq((uint32_t)r, (uint32_t)-1, "path_find_free_slot: all 100 slots occupied -> -1");
    }

    // ---- (b) exactly one free slot, in the middle of the row -> that exact index. -------------------
    {
        sim_fixture fx;
        fx.reset();
        sim_store own = fx.store();
        for (int32_t i = 0; i < 100; ++i) own.path_slot_flag_at(1, i) = 1;
        own.path_slot_flag_at(1, 37) = 0;
        const int32_t r              = detail::path_find_free_slot(own, 1);
        ck_eq((uint32_t)r, 37u, "path_find_free_slot: single free slot at 37 -> 37");
    }

    // ---- (c) two free slots (5 and 10) -> the LOWER index wins (proves scan direction is 0->99, not
    // e.g. a reverse or unordered scan). --------------------------------------------------------------
    {
        sim_fixture fx;
        fx.reset();
        sim_store own = fx.store();
        for (int32_t i = 0; i < 100; ++i) own.path_slot_flag_at(0, i) = 1;
        own.path_slot_flag_at(0, 5)  = 0;
        own.path_slot_flag_at(0, 10) = 0;
        const int32_t r              = detail::path_find_free_slot(own, 0);
        ck_eq((uint32_t)r, 5u, "path_find_free_slot: free at both 5 and 10 -> lower index 5 wins");
    }

    // ---- (d) the very first slot (index 0) is free -> 0, not treated as a falsy/absent-index case.
    {
        sim_fixture fx;
        fx.reset();
        sim_store own = fx.store();
        for (int32_t i = 1; i < 100; ++i) own.path_slot_flag_at(4, i) = 1;
        // slot 0 stays 0 (free) by fx.reset()'s zeroing.
        const int32_t r = detail::path_find_free_slot(own, 4);
        ck_eq((uint32_t)r, 0u, "path_find_free_slot: slot 0 free -> returns 0, not treated as absent");
    }

    // ---- (e) the very last slot (index 99) is the only free one -> 99, proves the loop's upper
    // bound is exactly 99 inclusive (100 exclusive), not off-by-one short. ----------------------------
    {
        sim_fixture fx;
        fx.reset();
        sim_store own = fx.store();
        for (int32_t i = 0; i < 99; ++i) own.path_slot_flag_at(6, i) = 1;
        const int32_t r = detail::path_find_free_slot(own, 6);
        ck_eq((uint32_t)r, 99u, "path_find_free_slot: only slot 99 free -> 99 (upper bound inclusive)");
    }
}

} // namespace mh::sim::test
