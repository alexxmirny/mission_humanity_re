//
// sim_landing_spots_reroll_selftest.cpp -- `simtest` oracle for
// llm_strat_landing_spots_reroll_out_of_bounds @0x00454de5 (sim/resid/sim_landing_spots_reroll.h/
// .cpp, RI-SIM / sim_resid batch C+E).
//
// NO SHADOW SITE (see the header's banner) -- this offline oracle is the function's only evidence,
// so every uncertainty flagged in sim_landing_spots_reroll.h is pinned here with a case that would
// fail if the claim were reversed:
//
//   * THE TWO-PART GUARD (0x00454e08 status!=-1 test / 0x00454e0f JZ-exit, THEN 0x00454e11
//     index<0x10 test / 0x00454e15 JL-body) stops at the FIRST status==-1 entry, even before
//     index 16 -- it is not a bare `for (i=0;i<16;++i)`.
//   * THE SIGNED STRICTLY-GREATER BOUND (0x00454e2b/0x00454e3d entry test, re-tested per axis at
//     0x00454e4f/0x00454e77) is a PRESERVED off-by-one: a coordinate exactly AT width/height is
//     NOT rerolled; only strictly greater is.
//   * PRNG DRAW ORDER: x is tested-and-rerolled completely (0x00454e45-0x00454e6d) BEFORE y is even
//     tested (0x00454e6d-0x00454e95) -- channel-2 RNG-sequence-significant.
//   * The dead re-test at 0x00454e97-0x00454ea4 has no translation counterpart; nothing here checks
//     for it since it has zero observable effect.
//
#include "sim/resid/sim_landing_spots_reroll.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Records every llm_rand_below call, IN ORDER, and hands back a distinctive, monotonically
// increasing value per call (9001, 9038, 9075, ...) -- far outside any coordinate this file seeds,
// so a wrong draw landing in the wrong field (or the wrong field being left un-rerolled) is
// unmistakable in a diff, and a bound-order swap is caught by comparing the recorded bound
// sequence, not just the count.
struct rand_below_call {
    int32_t upper_bound;
};
std::vector<rand_below_call> g_rand_calls;
std::vector<int32_t>         g_rand_returns; // same index as g_rand_calls

int32_t rec_rand_below(int32_t upper_bound) {
    const int32_t idx = (int32_t)g_rand_calls.size();
    g_rand_calls.push_back({upper_bound});
    const int32_t ret = 9001 + idx * 37;
    g_rand_returns.push_back(ret);
    return ret;
}

const landing_spots_reroll_calls g_calls = {
    &rec_rand_below,
};

void reset_recorders() {
    g_rand_calls.clear();
    g_rand_returns.clear();
}

// Map extents used by every case: deliberately different from each other, neither a power of two,
// and (outside the cases that deliberately test the boundary) never equal to a coordinate seeded
// elsewhere, so a swapped width/height or a coincidental equality cannot pass by accident.
constexpr int32_t MAP_WIDTH  = 257;
constexpr int32_t MAP_HEIGHT = 131;

} // namespace

void run_landing_spots_reroll_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- THE TWO-PART GUARD stops at the FIRST status==-1 spot, even before index 16
    // (0x00454e08/0x00454e0f, then 0x00454e11/0x00454e15). Spots 0..3 are valid (in-bounds, status
    // != -1) so the guard walks past them; spot 4 has status==-1 and stops the loop there; spots
    // 5..9 are seeded OUT OF BOUNDS with a non-(-1) status -- a normalised `for (i=0;i<16;++i)`
    // loop (ignoring the status short-circuit) would reroll them, so their surviving untouched
    // pins the two-part guard's ORDER and its status-based short-circuit.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.map_width  = MAP_WIDTH;
        fx.map_height = MAP_HEIGHT;

        for (int32_t i = 0; i < 4; ++i) {
            fx.landing_spots[i].status = 0;          // active, not the -1 terminator
            fx.landing_spots[i].x      = 10 + i * 5; // in-bounds -- no reroll needed
            fx.landing_spots[i].y      = 20 + i * 3; // in-bounds
        }
        fx.landing_spots[4].status = -1; // the terminator the guard must stop on
        fx.landing_spots[4].x      = 333;
        fx.landing_spots[4].y      = 444;
        for (int32_t i = 5; i < 10; ++i) {
            fx.landing_spots[i].status = 0;                   // NOT -1 -- looks processable
            fx.landing_spots[i].x      = MAP_WIDTH + 50 + i;  // deliberately out of bounds
            fx.landing_spots[i].y      = MAP_HEIGHT + 50 + i; // deliberately out of bounds
        }

        sim_store own = fx.store();
        detail::landing_spots_reroll_out_of_bounds(fx.view(), own, g_calls);

        ck_eq((uint32_t)g_rand_calls.size(), 0u,
              "T1: zero draws total -- the guard stops at spot 4's status==-1 before spots 5..9 are "
              "ever reached, 0x00454e08/0x00454e0f");
        for (int32_t i = 5; i < 10; ++i) {
            char msg[300];
            std::snprintf(msg, sizeof(msg),
                          "T1: spot %d (out-of-bounds coords, status!=-1) UNCHANGED -- the two-part "
                          "guard's status check (0x00454e08) short-circuits BEFORE the index<16 test "
                          "(0x00454e11), so a normalised i<16 loop would have rerolled this spot but "
                          "the real guard never reaches it",
                          i);
            ck(fx.landing_spots[i].x == MAP_WIDTH + 50 + i && fx.landing_spots[i].y == MAP_HEIGHT + 50 + i, msg);
        }
        ck(fx.landing_spots[4].status == -1 && fx.landing_spots[4].x == 333 && fx.landing_spots[4].y == 444,
           "T1: the terminator spot 4 itself is untouched (the guard reads its status, never writes it)");
    }

    // =================================================================================================
    // T2 -- THE INDEX BOUND (0x00454e11 CMP index,0x10 / 0x00454e15 JL): all 16 spots have
    // status != -1 and are all out of bounds on both axes -> exactly 16 spots processed (32 draws,
    // width then height per spot), and index 16 (the fixture's 17th slot, sized exactly so the
    // guard's read of it is valid memory) is never processed even though the guard reads its status
    // as part of the loop-exit test.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.map_width  = MAP_WIDTH;
        fx.map_height = MAP_HEIGHT;

        for (int32_t i = 0; i < 16; ++i) {
            fx.landing_spots[i].status = 0;
            fx.landing_spots[i].x      = MAP_WIDTH + 1;  // out of bounds on x
            fx.landing_spots[i].y      = MAP_HEIGHT + 1; // out of bounds on y
        }
        fx.landing_spots[16].status = 42; // arbitrary, non -1 -- read by the loop-exit test, never processed
        fx.landing_spots[16].x      = 9999;
        fx.landing_spots[16].y      = 8888;

        sim_store own = fx.store();
        detail::landing_spots_reroll_out_of_bounds(fx.view(), own, g_calls);

        ck_eq((uint32_t)g_rand_calls.size(), 32u,
              "T2: exactly 32 draws (16 spots x 2 axes each) -- the index bound "
              "(`CMP ...,0x10` @0x00454e11, `JL` @0x00454e15) admits indices 0..15 only");
        for (int32_t i = 0; i < 16; ++i) {
            char mx[160], my[160];
            std::snprintf(mx, sizeof(mx), "T2: spot %d.x got the width-bound draw, 0x00454e57/0x00454e5c", i);
            std::snprintf(my, sizeof(my), "T2: spot %d.y got the height-bound draw, 0x00454e7f/0x00454e84", i);
            ck_eq((uint32_t)fx.landing_spots[i].x, (uint32_t)g_rand_returns[i * 2 + 0], mx);
            ck_eq((uint32_t)fx.landing_spots[i].y, (uint32_t)g_rand_returns[i * 2 + 1], my);
            ck(g_rand_calls[i * 2 + 0].upper_bound == MAP_WIDTH && g_rand_calls[i * 2 + 1].upper_bound == MAP_HEIGHT,
               "T2: per-spot bound pair is (width, height) in that order, 0x00454e57 then 0x00454e7f");
        }
        ck(fx.landing_spots[16].status == 42 && fx.landing_spots[16].x == 9999 && fx.landing_spots[16].y == 8888,
           "T2: index 16 (past the 0..15 bound) is UNTOUCHED -- nothing past index 15 is ever "
           "processed, even though the loop-exit test reads its status field, 0x00454e11/0x00454e15");
    }

    // =================================================================================================
    // T3 -- THE SIGNED STRICTLY-GREATER BOUNDARY, both axes, both sides -- the PRESERVE-BUG. A
    // coordinate exactly EQUAL to width/height is NOT rerolled (0x00454e2b/0x00454e3d JG/JLE,
    // signed); width+1/height+1 IS. This is the original's apparent off-by-one against a 0-based
    // grid and must NOT be "fixed" to >= -- a >= reading would reroll the exactly-equal spots too.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.map_width  = MAP_WIDTH;
        fx.map_height = MAP_HEIGHT;

        // spot 0: x == width exactly, y in-bounds -> untouched, zero draws (0x00454e31 JG not taken).
        fx.landing_spots[0].status = 0;
        fx.landing_spots[0].x      = MAP_WIDTH;
        fx.landing_spots[0].y      = 50;
        // spot 1: x == width+1, y in-bounds -> x rerolled (bound==width), y untouched.
        fx.landing_spots[1].status = 0;
        fx.landing_spots[1].x      = MAP_WIDTH + 1;
        fx.landing_spots[1].y      = 50;
        // spot 2: y == height exactly, x in-bounds -> untouched, zero draws (0x00454e43 JLE taken).
        fx.landing_spots[2].status = 0;
        fx.landing_spots[2].x      = 50;
        fx.landing_spots[2].y      = MAP_HEIGHT;
        // spot 3: y == height+1, x in-bounds -> y rerolled (bound==height), x untouched.
        fx.landing_spots[3].status = 0;
        fx.landing_spots[3].x      = 50;
        fx.landing_spots[3].y      = MAP_HEIGHT + 1;
        fx.landing_spots[4].status = -1; // stop the guard after the four subject spots

        sim_store own = fx.store();
        detail::landing_spots_reroll_out_of_bounds(fx.view(), own, g_calls);

        ck_eq((uint32_t)g_rand_calls.size(), 2u,
              "T3: exactly 2 draws total (spot 1's x, spot 3's y) -- the exactly-equal spots (0, 2) "
              "draw nothing");

        ck(fx.landing_spots[0].x == MAP_WIDTH && fx.landing_spots[0].y == 50,
           "T3: spot 0 (x==width exactly) UNCHANGED -- signed strictly-greater `>` at 0x00454e2b/"
           "0x00454e31 does not fire on equality; this is the original's off-by-one, preserved "
           "verbatim, not corrected to >=");
        ck(fx.landing_spots[2].x == 50 && fx.landing_spots[2].y == MAP_HEIGHT,
           "T3: spot 2 (y==height exactly) UNCHANGED -- same preserved off-by-one on the height axis, "
           "0x00454e3d/0x00454e43");

        ck(g_rand_calls[0].upper_bound == MAP_WIDTH, "T3: spot 1's draw used bound==width, 0x00454e57");
        ck_eq((uint32_t)fx.landing_spots[1].x, (uint32_t)g_rand_returns[0],
              "T3: spot 1 (x==width+1) WAS rerolled -- strictly-greater DOES fire one past the "
              "boundary, 0x00454e31/0x00454e5c");
        ck_eq((uint32_t)fx.landing_spots[1].y, 50u, "T3: spot 1's y (in-bounds) left untouched");

        ck(g_rand_calls[1].upper_bound == MAP_HEIGHT, "T3: spot 3's draw used bound==height, 0x00454e7f");
        ck_eq((uint32_t)fx.landing_spots[3].y, (uint32_t)g_rand_returns[1],
              "T3: spot 3 (y==height+1) WAS rerolled, 0x00454e43-fallthrough/0x00454e84");
        ck_eq((uint32_t)fx.landing_spots[3].x, 50u, "T3: spot 3's x (in-bounds) left untouched");
    }

    // =================================================================================================
    // T4 -- DRAW ORDER AND COUNT. x is fully tested-and-rerolled (0x00454e45-0x00454e6d) BEFORE y is
    // even tested (0x00454e6d-0x00454e95): an x-only-out spot draws exactly once (bound==width); a
    // y-only-out spot draws exactly once (bound==height); a both-out spot draws exactly TWICE, width
    // FIRST then height -- catches a transposition of the two axes or their draw order.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.map_width  = MAP_WIDTH;
        fx.map_height = MAP_HEIGHT;

        // spot 0: x out only.
        fx.landing_spots[0].status = 0;
        fx.landing_spots[0].x      = MAP_WIDTH + 10;
        fx.landing_spots[0].y      = 50;
        // spot 1: y out only.
        fx.landing_spots[1].status = 0;
        fx.landing_spots[1].x      = 50;
        fx.landing_spots[1].y      = MAP_HEIGHT + 10;
        // spot 2: both out.
        fx.landing_spots[2].status = 0;
        fx.landing_spots[2].x      = MAP_WIDTH + 20;
        fx.landing_spots[2].y      = MAP_HEIGHT + 20;
        fx.landing_spots[3].status = -1; // stop the guard after the three subject spots

        sim_store own = fx.store();
        detail::landing_spots_reroll_out_of_bounds(fx.view(), own, g_calls);

        ck_eq((uint32_t)g_rand_calls.size(), 4u,
              "T4: exactly 4 draws total (1 + 1 + 2) across the three spots");

        // spot 0: x-only-out -> one draw, bound==width, only x changes.
        ck(g_rand_calls[0].upper_bound == MAP_WIDTH, "T4: spot 0's sole draw used bound==width, 0x00454e57");
        ck_eq((uint32_t)fx.landing_spots[0].x, (uint32_t)g_rand_returns[0], "T4: spot 0.x got the draw");
        ck_eq((uint32_t)fx.landing_spots[0].y, 50u, "T4: spot 0.y (in-bounds) untouched -- exactly one draw");

        // spot 1: y-only-out -> one draw, bound==height, only y changes.
        ck(g_rand_calls[1].upper_bound == MAP_HEIGHT, "T4: spot 1's sole draw used bound==height, 0x00454e7f");
        ck_eq((uint32_t)fx.landing_spots[1].y, (uint32_t)g_rand_returns[1], "T4: spot 1.y got the draw");
        ck_eq((uint32_t)fx.landing_spots[1].x, 50u, "T4: spot 1.x (in-bounds) untouched -- exactly one draw");

        // spot 2: both out -> two draws, width THEN height (not height then width).
        ck(g_rand_calls[2].upper_bound == MAP_WIDTH && g_rand_calls[3].upper_bound == MAP_HEIGHT,
           "T4: spot 2's bound sequence is (width, height) IN THAT ORDER -- x is tested-and-rerolled "
           "completely (0x00454e45-0x00454e6d) before y is even tested (0x00454e6d-0x00454e95); a "
           "transposed order would fail this");
        ck_eq((uint32_t)fx.landing_spots[2].x, (uint32_t)g_rand_returns[2],
              "T4: spot 2.x got the FIRST returned draw value");
        ck_eq((uint32_t)fx.landing_spots[2].y, (uint32_t)g_rand_returns[3],
              "T4: spot 2.y got the SECOND returned draw value");
    }

    // =================================================================================================
    // T5 -- a spot fully in bounds on both axes: the entry test (0x00454e2b/0x00454e3d) is false on
    // both arms -- no draw at all, nothing changed.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.map_width  = MAP_WIDTH;
        fx.map_height = MAP_HEIGHT;

        fx.landing_spots[0].status = 0;
        fx.landing_spots[0].x      = 100;
        fx.landing_spots[0].y      = 60;
        fx.landing_spots[1].status = -1; // stop the guard right after

        sim_store own = fx.store();
        detail::landing_spots_reroll_out_of_bounds(fx.view(), own, g_calls);

        ck_eq((uint32_t)g_rand_calls.size(), 0u,
              "T5: a fully-in-bounds spot draws nothing -- entry test false on both arms, "
              "0x00454e31/0x00454e43");
        ck(fx.landing_spots[0].x == 100 && fx.landing_spots[0].y == 60,
           "T5: fully-in-bounds spot's x/y left exactly as seeded");
    }

    // =================================================================================================
    // T6 -- empty run: spot 0 already has status == -1 -> the guard exits on the very first check
    // (0x00454e08/0x00454e0f), zero draws, nothing touched.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.map_width  = MAP_WIDTH;
        fx.map_height = MAP_HEIGHT;

        fx.landing_spots[0].status = -1;
        fx.landing_spots[0].x      = 555;
        fx.landing_spots[0].y      = 666;

        sim_store own = fx.store();
        detail::landing_spots_reroll_out_of_bounds(fx.view(), own, g_calls);

        ck_eq((uint32_t)g_rand_calls.size(), 0u,
              "T6: empty run -- spot 0's status==-1 stops the guard immediately, 0x00454e08/0x00454e0f");
        ck(fx.landing_spots[0].status == -1 && fx.landing_spots[0].x == 555 && fx.landing_spots[0].y == 666,
           "T6: spot 0 left exactly as seeded -- the guard never enters the loop body");
    }
}

} // namespace mh::sim::test
