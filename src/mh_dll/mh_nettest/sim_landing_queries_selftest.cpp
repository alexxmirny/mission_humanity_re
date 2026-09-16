//
// sim_landing_queries_selftest.cpp -- `simtest` cases for llm_strat_count_landing_spots and
// llm_strat_set_landing_site (sim/sim_landing_queries.h/.cpp, RI-SIM / SIM1F).
//
// count_landing_spots's shadow site is return-comparable but exercising it needs the map's landing
// table populated in a specific way; set_landing_site is a per-player/per-planet write likely
// unreached by a fresh soak. Both are settled here offline instead, EXPECTED VALUES HAND-DERIVED
// FROM THE DISASSEMBLY (tmp/decomp/llm_strat_count_landing_spots_00454d8d.asm,
// llm_strat_set_landing_site_00454fe3.asm):
//
//   count = length of the leading run with landing_spots[i].status != -1, capped at 16 (the loop
//           tests status BEFORE the i<16 bound, so an all-active table reads [16] then stops).
//   set:    profiles[player].landing_x[planet]=x; landing_y[planet]=y; landing_spot_index[planet]=idx;
//           return (uint8_t)idx.
//
#include "sim/sim_landing_queries.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

} // namespace

void run_landing_queries_tests() {
    sim_fixture fx;

    // ---- count_landing_spots ----------------------------------------------------------------------
    // C1: empty table -- slot 0 status == -1 -> count 0. reset() zeroes the vector, so seed the
    // sentinel explicitly.
    fx.reset();
    fx.landing_spots[0].status = -1;
    ck_eq((uint32_t)detail::count_landing_spots(fx.view()), 0u, "count C1: leading -1 -> 0");

    // C2: three active then a sentinel -> count 3.
    fx.reset();
    fx.landing_spots[0].status = 0;
    fx.landing_spots[1].status = 5;
    fx.landing_spots[2].status = 1;
    fx.landing_spots[3].status = -1;
    ck_eq((uint32_t)detail::count_landing_spots(fx.view()), 3u, "count C2: 3 active then -1 -> 3");

    // C3: cap at 16 -- all 16 slots active (status != -1). The loop reads landing_spots[16].status
    // once (faithful to the original) before the i<16 bound stops it; the fixture sizes the vector to
    // 17 so that read is valid, and slot 16's value is irrelevant (the bound stops the run at 16).
    fx.reset();
    for (int i = 0; i < 16; ++i) fx.landing_spots[i].status = i + 1; // all non-(-1)
    ck_eq((uint32_t)detail::count_landing_spots(fx.view()), 16u, "count C3: all 16 active -> cap 16");

    // C4: a -1 in the MIDDLE ends the run early (status test, not a full scan).
    fx.reset();
    fx.landing_spots[0].status = 2;
    fx.landing_spots[1].status = -1;
    fx.landing_spots[2].status = 9; // active but after the sentinel -> not counted
    ck_eq((uint32_t)detail::count_landing_spots(fx.view()), 1u, "count C4: -1 at [1] ends run at 1");

    // ---- set_landing_site -------------------------------------------------------------------------
    // S1: writes the three per-planet fields for one player and returns (uint8_t)spot_index.
    fx.reset();
    {
        sim_store own = fx.store();
        uint8_t   r   = detail::set_landing_site(own, /*player*/ 2, /*planet*/ 5, /*x*/ 111,
                                                 /*y*/ 222, /*spot_index*/ 7);
        ck_eq((uint32_t)fx.profiles[2].landing_x[5], 111u, "set S1: landing_x[5] = x(111)");
        ck_eq((uint32_t)fx.profiles[2].landing_y[5], 222u, "set S1: landing_y[5] = y(222)");
        ck_eq((uint32_t)fx.profiles[2].landing_spot_index[5], 7u, "set S1: landing_spot_index[5] = 7");
        ck_eq((uint32_t)r, 7u, "set S1: return = (uint8_t)spot_index(7)");
        // isolation: a different planet slot on the same player is untouched (still 0).
        ck_eq((uint32_t)fx.profiles[2].landing_x[4], 0u, "set S1: adjacent planet 4 untouched");
    }

    // S2: return truncates to a byte (spot_index 0x105 -> 0x05); the three stores keep the full int.
    fx.reset();
    {
        sim_store own = fx.store();
        uint8_t   r   = detail::set_landing_site(own, /*player*/ 0, /*planet*/ 0, /*x*/ 1, /*y*/ 2,
                                                 /*spot_index*/ 0x105);
        ck_eq((uint32_t)fx.profiles[0].landing_spot_index[0], 0x105u,
              "set S2: landing_spot_index keeps full int (0x105)");
        ck_eq((uint32_t)r, 0x05u, "set S2: return is byte-truncated (0x105 -> 0x05)");
    }
}

} // namespace mh::sim::test
