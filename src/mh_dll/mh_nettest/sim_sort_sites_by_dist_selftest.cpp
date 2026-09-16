//
// sim_sort_sites_by_dist_selftest.cpp -- `simtest` offline oracle for
// llm_strat_sort_sites_by_dist @0x004dc117 (sim/resid/sim_spawn_ai_base.h/.cpp, RI-SIM /
// sim_resid batch E).
//
// NO SHADOW SITE (sim_resid rule 1) -- this offline oracle is the only verification. Expected
// behaviour hand-derived from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_sort_sites_by_dist_004dc117.asm):
//
//   0x004dc144-d14c  ai_resource_site_count == 0 -> return, nothing computed and nothing called.
//   0x004dc152-d18c  one toroidal_dist_sq per live site: (ai_home_tile_x, ai_home_tile_y) against
//                    the site's FINE tile coords, i.e. the coarse grid_x/grid_y shifted LEFT BY 2.
//                    grid_x/grid_y are read UNSIGNED (MOVZX word) even though the struct types them
//                    int16_t -- so a negative grid coordinate reads as a large positive number.
//   0x004dc1b1-d25a  bubble sort ASCENDING by that cached distance, with the original's early exit
//                    when a full inner pass makes no swap. The swap moves the WHOLE 10-byte record
//                    (MOVSD;MOVSD;MOVSW) and the cached distance alongside it.
//
// The distance function is mocked, which is the point: it makes the ORDERING testable without
// depending on the real torus arithmetic, and it lets a case pin exactly which coordinates were
// handed to it.
//
#include <algorithm> // std::fill over the per-case distance table

#include "sim/resid/sim_spawn_ai_base.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct DistCall {
    int32_t x1, y1, x2, y2;
};
std::vector<DistCall> g_dist_calls;
// Answers are keyed on the site's FINE x, so a case controls the ordering directly.
std::vector<uint32_t> g_dist_by_fine_x = std::vector<uint32_t>(4096, 0);
uint32_t              stub_toroidal_dist_sq(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    g_dist_calls.push_back({x1, y1, x2, y2});
    return g_dist_by_fine_x[(size_t)(x2 & 0xfff)];
}

// Only toroidal_dist_sq is reached by this function; the rest must never be called, and a null here
// would fault loudly if one were.
const spawn_ai_base_calls g_calls = {
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    &stub_toroidal_dist_sq,
};

void reset_calls() {
    g_dist_calls.clear();
    std::fill(g_dist_by_fine_x.begin(), g_dist_by_fine_x.end(), 0u);
}

void run(sim_fixture &fx, int32_t player) {
    sim_store own = fx.store();
    detail::sort_sites_by_dist(own, g_calls, player);
}

// Seed site `i` with a coarse grid position and a distance answer for it.
void seed_site(sim_fixture &fx, int32_t player, int32_t i, int16_t gx, int16_t gy, uint32_t dist,
               int16_t status) {
    auto &site                                              = fx.players[(size_t)player].ai_resource_sites[i];
    site.grid_x                                             = gx;
    site.grid_y                                             = gy;
    site.status                                             = status;
    site.build_tile_x                                       = (int16_t)(1000 + i);
    site.build_tile_y                                       = (int16_t)(2000 + i);
    g_dist_by_fine_x[(size_t)(((uint16_t)gx << 2) & 0xfff)] = dist;
}

} // namespace

void run_sort_sites_by_dist_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- COUNT == 0: return immediately. No distance call, and the site array (deliberately
    // pre-seeded out of order) is left exactly as it was, so an unguarded sort would be visible.
    // =================================================================================================
    {
        fx.reset();
        reset_calls();
        seed_site(fx, /*player=*/1, /*i=*/0, /*gx=*/5, /*gy=*/6, /*dist=*/900, /*status=*/7);
        seed_site(fx, /*player=*/1, /*i=*/1, /*gx=*/9, /*gy=*/9, /*dist=*/1, /*status=*/8);
        fx.players[1].ai_resource_site_count = 0;

        run(fx, /*player=*/1);

        ck(g_dist_calls.empty(), "T1: count 0 -> toroidal_dist_sq never called, 0x004dc144-d14c");
        ck_eq((uint32_t)fx.players[1].ai_resource_sites[0].grid_x, 5u, "T1: site 0 not moved");
        ck_eq((uint32_t)fx.players[1].ai_resource_sites[1].grid_x, 9u, "T1: site 1 not moved");
    }

    // =================================================================================================
    // T2 -- THE DISTANCE ARGUMENTS. One site, so exactly one call: home tile as the first pair, and
    // the site's coarse coords SHIFTED LEFT BY 2 as the second. Home and grid values are all distinct
    // and non-symmetric, so a swapped pair or a missing shift is visible.
    //   grid (7, 11) -> fine (28, 44); home (33, 77).
    // =================================================================================================
    {
        fx.reset();
        reset_calls();
        fx.players[2].ai_home_tile_x = 33;
        fx.players[2].ai_home_tile_y = 77;
        seed_site(fx, /*player=*/2, /*i=*/0, /*gx=*/7, /*gy=*/11, /*dist=*/5, /*status=*/0);
        fx.players[2].ai_resource_site_count = 1;

        run(fx, /*player=*/2);

        ck(g_dist_calls.size() == 1, "T2: one distance call per live site, 0x004dc152-d18c");
        if (g_dist_calls.size() == 1) {
            ck_eq((uint32_t)g_dist_calls[0].x1, 33u, "T2: arg 1 = ai_home_tile_x");
            ck_eq((uint32_t)g_dist_calls[0].y1, 77u, "T2: arg 2 = ai_home_tile_y");
            ck_eq((uint32_t)g_dist_calls[0].x2, 28u, "T2: arg 3 = grid_x << 2 (coarse -> fine)");
            ck_eq((uint32_t)g_dist_calls[0].y2, 44u, "T2: arg 4 = grid_y << 2");
        }
    }

    // =================================================================================================
    // T3 -- ASCENDING ORDER, WHOLE RECORD. Five sites seeded in a deliberately bad order; every field
    // of every record is checked after the sort, because the original swaps all 10 bytes together and
    // a per-field swap that missed one would leave a Frankenstein record that a grid_x-only check
    // would not see.
    // =================================================================================================
    {
        fx.reset();
        reset_calls();
        fx.players[0].ai_home_tile_x = 0;
        fx.players[0].ai_home_tile_y = 0;
        // (index, grid_x, dist): the intended ascending order is grid_x 3, 1, 4, 0, 2.
        seed_site(fx, 0, /*i=*/0, /*gx=*/0, /*gy=*/10, /*dist=*/400, /*status=*/10);
        seed_site(fx, 0, /*i=*/1, /*gx=*/1, /*gy=*/11, /*dist=*/200, /*status=*/11);
        seed_site(fx, 0, /*i=*/2, /*gx=*/2, /*gy=*/12, /*dist=*/500, /*status=*/12);
        seed_site(fx, 0, /*i=*/3, /*gx=*/3, /*gy=*/13, /*dist=*/100, /*status=*/13);
        seed_site(fx, 0, /*i=*/4, /*gx=*/4, /*gy=*/14, /*dist=*/300, /*status=*/14);
        fx.players[0].ai_resource_site_count = 5;

        run(fx, 0);

        const int16_t want_gx[5] = {3, 1, 4, 0, 2};
        for (int32_t i = 0; i < 5; ++i) {
            const auto &s = fx.players[0].ai_resource_sites[i];
            ck_eq((uint32_t)(int32_t)s.grid_x, (uint32_t)(int32_t)want_gx[i],
                  "T3: sites end up ascending by cached distance, 0x004dc1b1-d25a");
            // Every field travelled with its record.
            ck_eq((uint32_t)(int32_t)s.grid_y, (uint32_t)(int32_t)(10 + want_gx[i]),
                  "T3: grid_y travelled with the record");
            ck_eq((uint32_t)(int32_t)s.status, (uint32_t)(int32_t)(10 + want_gx[i]),
                  "T3: status travelled with the record");
            ck_eq((uint32_t)(int32_t)s.build_tile_x, (uint32_t)(1000 + want_gx[i]),
                  "T3: build_tile_x travelled with the record");
            ck_eq((uint32_t)(int32_t)s.build_tile_y, (uint32_t)(2000 + want_gx[i]),
                  "T3: build_tile_y travelled with the record");
        }
    }

    // =================================================================================================
    // T4 -- THE CACHED DISTANCES TRAVEL WITH THE RECORDS. The original swaps the distance array
    // alongside the record array; a sort that swapped only the records would compare stale keys and
    // settle in a different order. Six values arranged so the two orders differ:
    //   distances 6 5 4 3 2 1 -> a correct sort fully reverses the list.
    // A record-only swap leaves the key array untouched and cannot reach that permutation.
    // =================================================================================================
    {
        fx.reset();
        reset_calls();
        for (int32_t i = 0; i < 6; ++i)
            seed_site(fx, 0, i, /*gx=*/(int16_t)i, /*gy=*/0, /*dist=*/(uint32_t)(6 - i), /*status=*/0);
        fx.players[0].ai_resource_site_count = 6;

        run(fx, 0);

        for (int32_t i = 0; i < 6; ++i) {
            ck_eq((uint32_t)(int32_t)fx.players[0].ai_resource_sites[i].grid_x, (uint32_t)(5 - i),
                  "T4: a fully reversed input comes out fully reversed -- the key array is swapped too");
        }
    }

    // =================================================================================================
    // T5 -- EQUAL KEYS DO NOT SWAP (the comparison is `dist[i+1] < dist[i]`, strict). Three sites
    // share one distance; their relative order must be exactly as seeded. A `<=` here would swap
    // equal neighbours forever -- the early-exit flag would be re-armed on every pass -- so that
    // mutation shows up as a HANG rather than a FAIL. Recorded because a hanging mutant is easy to
    // misread as "the mutation was not caught".
    // =================================================================================================
    {
        fx.reset();
        reset_calls();
        seed_site(fx, 0, 0, /*gx=*/10, 0, /*dist=*/50, 0);
        seed_site(fx, 0, 1, /*gx=*/11, 0, /*dist=*/50, 0);
        seed_site(fx, 0, 2, /*gx=*/12, 0, /*dist=*/50, 0);
        fx.players[0].ai_resource_site_count = 3;

        run(fx, 0);

        ck_eq((uint32_t)(int32_t)fx.players[0].ai_resource_sites[0].grid_x, 10u,
              "T5: equal keys keep their seeded order -- the compare is strict `<`");
        ck_eq((uint32_t)(int32_t)fx.players[0].ai_resource_sites[1].grid_x, 11u, "T5: ... second");
        ck_eq((uint32_t)(int32_t)fx.players[0].ai_resource_sites[2].grid_x, 12u, "T5: ... third");
    }

    // =================================================================================================
    // T6 -- ONLY `count` SITES PARTICIPATE. Four records are seeded but the count says 2, and the two
    // beyond it carry distances that would sort to the FRONT. They must stay exactly where they are.
    // =================================================================================================
    {
        fx.reset();
        reset_calls();
        seed_site(fx, 0, 0, /*gx=*/20, 0, /*dist=*/900, 0);
        seed_site(fx, 0, 1, /*gx=*/21, 0, /*dist=*/800, 0);
        seed_site(fx, 0, 2, /*gx=*/22, 0, /*dist=*/1, 0); // past the count
        seed_site(fx, 0, 3, /*gx=*/23, 0, /*dist=*/2, 0); // past the count
        fx.players[0].ai_resource_site_count = 2;

        run(fx, 0);

        ck(g_dist_calls.size() == 2, "T6: exactly `count` distance calls, not one per record");
        ck_eq((uint32_t)(int32_t)fx.players[0].ai_resource_sites[0].grid_x, 21u, "T6: the two live sites swapped");
        ck_eq((uint32_t)(int32_t)fx.players[0].ai_resource_sites[1].grid_x, 20u, "T6: ... into ascending order");
        ck_eq((uint32_t)(int32_t)fx.players[0].ai_resource_sites[2].grid_x, 22u,
              "T6: the record past the count is untouched despite a smaller distance");
        ck_eq((uint32_t)(int32_t)fx.players[0].ai_resource_sites[3].grid_x, 23u, "T6: ... and the next one too");
    }

    // =================================================================================================
    // T7 -- COUNT == 1: one distance call, no comparison possible, record unchanged. This is the
    // boundary the inner loop's `i < count - 1` bound sits on; an `i <= count - 1` mutation reads
    // dist[1], which this case leaves at 0 and would therefore swap a live record with a dead one.
    // =================================================================================================
    {
        fx.reset();
        reset_calls();
        seed_site(fx, 0, 0, /*gx=*/30, /*gy=*/31, /*dist=*/700, /*status=*/5);
        seed_site(fx, 0, 1, /*gx=*/40, /*gy=*/41, /*dist=*/0, /*status=*/6); // dead neighbour
        fx.players[0].ai_resource_site_count = 1;

        run(fx, 0);

        ck(g_dist_calls.size() == 1, "T7: one site -> one distance call");
        ck_eq((uint32_t)(int32_t)fx.players[0].ai_resource_sites[0].grid_x, 30u,
              "T7: the single live record stays put -- the inner bound is `i < count - 1`");
        ck_eq((uint32_t)(int32_t)fx.players[0].ai_resource_sites[1].grid_x, 40u,
              "T7: the dead neighbour is not dragged in");
    }

    // =================================================================================================
    // T8 -- THE PLAYER ROW. A decoy player's site list is seeded in the opposite order; sorting the
    // wrong row would leave the target unsorted AND disturb the decoy.
    // =================================================================================================
    {
        fx.reset();
        reset_calls();
        seed_site(fx, /*player=*/6, 0, /*gx=*/50, 0, /*dist=*/999, 0);
        seed_site(fx, /*player=*/6, 1, /*gx=*/51, 0, /*dist=*/1, 0);
        fx.players[6].ai_resource_site_count = 2;
        seed_site(fx, /*player=*/7, 0, /*gx=*/60, 0, /*dist=*/999, 0);
        seed_site(fx, /*player=*/7, 1, /*gx=*/61, 0, /*dist=*/1, 0);
        fx.players[7].ai_resource_site_count = 2;

        run(fx, /*player=*/6);

        ck_eq((uint32_t)(int32_t)fx.players[6].ai_resource_sites[0].grid_x, 51u, "T8: player 6 was sorted");
        ck_eq((uint32_t)(int32_t)fx.players[7].ai_resource_sites[0].grid_x, 60u,
              "T8: player 7's list is untouched -- the row index is the parameter");
    }
}

} // namespace mh::sim::test
