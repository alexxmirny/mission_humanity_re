//
// sim_planet_distance_selftest.cpp -- `simtest` offline oracle for llm_strat_planet_distance
// (sim/sim_planet_distance.h/.cpp, RI-SIM / SIM1F).
//
// planet_distance saves map_width/map_height, forces BOTH to 100000 (defeating the torus wrap for
// the interplanetary starfield), calls the ORIGINAL llm_strat_tile_delta_wrapped for a (dx,dy),
// restores the two saved originals, then returns sqrt(dx*dx+dy*dy). tile_delta_wrapped is stubbed
// to RECORD the call (including width/height AS OBSERVED AT THE MOMENT OF THE CALL, via a captured
// fixture pointer) and hand back a caller-chosen (dx,dy) directly -- isolating this function's own
// save/force/restore/sqrt logic from tile_delta_wrapped's own torus-wrap math, which this batch does
// not have the .asm for. sqrt is a REAL std::sqrt mock (same posture
// sim_facing24_from_points_selftest.cpp's header documents for a marshallable-but-indirected
// callee).
//
#include "sim/sim_planet_distance.h"

#include <cmath>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct TDWCall {
    int32_t x1, y1, x2, y2;
    int32_t width_at_call, height_at_call;
};
std::vector<TDWCall> g_tdw_calls;
int32_t              g_stub_dx = 0, g_stub_dy = 0;
sim_fixture         *g_fx = nullptr; // set once per run so the stub can observe live width/height

void stub_tile_delta_wrapped(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t *out_dx,
                             int32_t *out_dy) {
    g_tdw_calls.push_back({x1, y1, x2, y2, g_fx->map_width, g_fx->map_height});
    *out_dx = g_stub_dx;
    *out_dy = g_stub_dy;
}

double stub_sqrt(double x) { return std::sqrt(x); } // real sqrt

const planet_distance_calls test_calls_v = {stub_tile_delta_wrapped, stub_sqrt};

} // namespace

void run_planet_distance_tests() {
    sim_fixture fx;
    g_fx = &fx;

    // ---- C1: width/height are forced to 100000 AT THE CALL, and restored to the ORIGINAL
    // (non-default, width != height) values afterward.
    fx.reset();
    fx.map_width  = 777;
    fx.map_height = 444;
    g_tdw_calls.clear();
    g_stub_dx = 0;
    g_stub_dy = 0;
    {
        sim_store own = fx.store();
        (void)detail::planet_distance(fx.view(), own, test_calls_v, /*x1*/ 11, /*y1*/ 22, /*x2*/ 33,
                                      /*y2*/ 44);
    }
    ck(g_tdw_calls.size() == 1, "planet_distance C1: tile_delta_wrapped called exactly once");
    ck(g_tdw_calls[0].x1 == 11 && g_tdw_calls[0].y1 == 22 && g_tdw_calls[0].x2 == 33 &&
           g_tdw_calls[0].y2 == 44,
       "planet_distance C1: (x1,y1,x2,y2) passed through unchanged");
    ck_eq((uint32_t)g_tdw_calls[0].width_at_call, 100000u,
          "planet_distance C1: width forced to 100000 AT the call");
    ck_eq((uint32_t)g_tdw_calls[0].height_at_call, 100000u,
          "planet_distance C1: height forced to 100000 AT the call");
    ck_eq((uint32_t)fx.map_width, 777u,
          "planet_distance C1: width restored to the ORIGINAL (777) after return");
    ck_eq((uint32_t)fx.map_height, 444u,
          "planet_distance C1: height restored to the ORIGINAL (444) after return");

    // ---- C2: return = sqrt(dx*dx + dy*dy). 3-4-5 pythagorean pair -> EXACTLY representable
    // (ck_eq_d compares exactly).
    fx.reset();
    g_stub_dx = 3;
    g_stub_dy = 4;
    {
        sim_store own = fx.store();
        double    got = detail::planet_distance(fx.view(), own, test_calls_v, 0, 0, 0, 0);
        ck_eq_d(got, 5.0, "planet_distance C2: sqrt(3^2+4^2) = 5.0 exactly");
    }

    // ---- C3: sign of dx/dy must not matter (squared).
    fx.reset();
    g_stub_dx = -3;
    g_stub_dy = 4;
    {
        sim_store own = fx.store();
        double    got = detail::planet_distance(fx.view(), own, test_calls_v, 0, 0, 0, 0);
        ck_eq_d(got, 5.0, "planet_distance C3a: sign of dx ignored (squared) -> still 5.0");
    }
    fx.reset();
    g_stub_dx = 3;
    g_stub_dy = -4;
    {
        sim_store own = fx.store();
        double    got = detail::planet_distance(fx.view(), own, test_calls_v, 0, 0, 0, 0);
        ck_eq_d(got, 5.0, "planet_distance C3b: sign of dy ignored (squared) -> still 5.0");
    }

    // ---- C4: a second exact triple with distinct magnitudes.
    fx.reset();
    g_stub_dx = 5;
    g_stub_dy = 12;
    {
        sim_store own = fx.store();
        double    got = detail::planet_distance(fx.view(), own, test_calls_v, 0, 0, 0, 0);
        ck_eq_d(got, 13.0, "planet_distance C4: sqrt(5^2+12^2) = 13.0 exactly");
    }

    // ---- C5: a DIFFERENT starting width/height than C1's, so a translation that hardcoded the
    // restore to a fixed pair (e.g. the fixture's OWN reset() defaults, 200/120, or C1's own 777/444)
    // rather than genuinely saving/restoring would pass C1 by coincidence and fail here.
    fx.reset();
    fx.map_width  = 9001;
    fx.map_height = 42;
    g_tdw_calls.clear();
    g_stub_dx = 0;
    g_stub_dy = 0;
    {
        sim_store own = fx.store();
        (void)detail::planet_distance(fx.view(), own, test_calls_v, 1, 2, 3, 4);
    }
    ck_eq((uint32_t)g_tdw_calls[0].width_at_call, 100000u,
          "planet_distance C5: width forced to 100000 (second, different, starting pair)");
    ck_eq((uint32_t)fx.map_width, 9001u,
          "planet_distance C5: width restored to 9001 (not C1's value, not the fixture default)");
    ck_eq((uint32_t)fx.map_height, 42u,
          "planet_distance C5: height restored to 42 (not C1's value, not the fixture default)");

    // ---- MUTATION NOTES ------------------------------------------------------------------------
    // 1. Forgetting to force width/height to 100000 before the callee call: C1's width_at_call/
    //    height_at_call checks would read 777/444 instead of 100000/100000.
    // 2. Forgetting to restore afterward: C5's post-call fx.map_width/height checks would read
    //    100000/100000 instead of 9001/42.
    // 3. Computing dx*dy instead of dx*dx+dy*dy (or dx+dy): C2/C4 would read a wrong, non-5.0/13.0
    //    value (e.g. sqrt(12)=3.46... for dx*dy=3*4, not 5.0).
    // 4. Hardcoding the restore to a fixed pair instead of the saved originals: passes C1 (which
    //    happens to differ from the fixture's 200/120 default) but fails C5's distinct second pair.
}

} // namespace mh::sim::test
