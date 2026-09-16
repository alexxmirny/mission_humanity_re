//
// sim_dir_sector_to_selftest.cpp -- `simtest` offline oracle for llm_strat_dir_sector_to
// (sim/sim_dir_headings.h/.cpp, RI-SIM / SIM1F). dir_from_to lives in the same header/TU but is NOT
// covered here (a separate oracle's job).
//
// dir_sector_to has two outward calls (llm_strat_map_wrapped_delta, llm_math_atan), both ORIGINAL
// functions outside this batch and not re-verified here -- wrapped_delta is stubbed to RECORD the
// call and hand back a caller-chosen (dx,dy) directly (isolating dir_sector_to's own trig/sector
// math from wrapped_delta's own torus-wrap logic, which this batch does not have the .asm for), and
// atan is a REAL std::atan mock for the naturally-reachable cases (same posture
// sim_facing24_from_points_selftest.cpp's header documents for its own sibling shape), plus a
// raw-value OVERRIDE for one case that forces an otherwise-unreachable branch (see C6).
//
// EXPECTED VALUES computed by an INDEPENDENT re-derivation of the formula from the header/.asm
// (expected_sector128[_raw] below), not by calling into detail::dir_sector_to or its private
// angle_div_sector_trunc helper -- so a translation bug in the production formula disagrees with
// this file's own math, not merely with itself.
//
#include "sim/sim_dir_headings.h"

#include <cmath>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- wrapped_delta: recording stub, hands back a caller-chosen (dx,dy) -------------------------
struct WDCall {
    int32_t x1, y1, x2, y2;
};
std::vector<WDCall> g_wd_calls;
double              g_stub_dx = 0.0, g_stub_dy = 0.0;

void stub_wrapped_delta(int32_t x1, int32_t y1, int32_t x2, int32_t y2, double *out_dx, double *out_dy) {
    g_wd_calls.push_back({x1, y1, x2, y2});
    *out_dx = g_stub_dx;
    *out_dy = g_stub_dy;
}

// ---- atan: real std::atan by default; a raw-value override for C6 (forcing the otherwise
// unreachable wrap-DOWN branch) ---------------------------------------------------------------
double g_atan_override     = 0.0;
bool   g_use_atan_override = false;

double stub_atan(double x) { return g_use_atan_override ? g_atan_override : std::atan(x); }

const dir_headings_calls test_calls_v = {stub_wrapped_delta, stub_atan};

// ---- independent re-derivation of the formula (header/.asm), not the .cpp ----------------------
// dir128 boot constants, matching sim_fixture's own read-memory-confirmed defaults:
//   rad2deg_num=180.0, rad2deg_den=3.1415926536, half_turn=180.0, bias=1.4062,
//   wrap_add=360.0, wrap_limit=360.0, wrap_sub=-360.0, sector_deg=2.8125.
int32_t expected_sector128_raw(double atan_result_rad, bool dy_is_negative) {
    double angle_deg = atan_result_rad * 180.0 / 3.1415926536; // rad2deg_num/_den
    if (dy_is_negative) angle_deg += 180.0;                    // half_turn_deg
    angle_deg += 1.4062;                                       // bias_deg
    if (angle_deg < 0.0) angle_deg += 360.0;                   // wrap_add_deg
    if (angle_deg >= 360.0) angle_deg += -360.0;               // wrap_sub_deg
    return (int32_t)std::trunc(angle_deg / 2.8125) + 1;        // sector_deg, then integer +1
}

int32_t expected_sector128(double dx, double dy) {
    return expected_sector128_raw(std::atan(dx / dy), dy < 0.0);
}

} // namespace

void run_dir_sector_to_tests() {
    sim_fixture fx;

    // ---- call-order: dir_sector_to(x0,y0,x1,y1) must call wrapped_delta as (x1,y0,x0,y1,&dx,&dy)
    // -- a DIFFERENT scramble from dir_from_to's own (per the header). Distinct, non-symmetric args
    // so a reversion to the natural (x0,y0,x1,y1) order is visible.
    fx.reset();
    g_wd_calls.clear();
    g_use_atan_override = false;
    g_stub_dx           = 3.0;
    g_stub_dy           = 4.0; // arbitrary here; only the call order is under test
    (void)detail::dir_sector_to(fx.view(), test_calls_v, /*x0*/ 11, /*y0*/ 22, /*x1*/ 33, /*y1*/ 44);
    ck(g_wd_calls.size() == 1, "dir_sector_to: wrapped_delta called exactly once");
    ck(g_wd_calls[0].x1 == 33 && g_wd_calls[0].y1 == 22 && g_wd_calls[0].x2 == 11 &&
           g_wd_calls[0].y2 == 44,
       "dir_sector_to: wrapped_delta call order is (x1,y0,x0,y1), NOT the natural (x0,y0,x1,y1)");

    // ---- C1: dx=0,dy=5 (dy>=0, no half-turn). angle=0+bias(1.4062); 1.4062/2.8125<0.5 -> sector 1.
    fx.reset();
    g_use_atan_override = false;
    g_stub_dx           = 0.0;
    g_stub_dy           = 5.0;
    {
        int32_t got = detail::dir_sector_to(fx.view(), test_calls_v, 0, 0, 0, 0);
        ck_eq((uint32_t)got, (uint32_t)expected_sector128(0.0, 5.0),
              "dir_sector_to C1: dx=0,dy=5 matches the independent formula");
        ck_eq((uint32_t)got, 1u, "dir_sector_to C1: sector is literally 1");
    }

    // ---- C2: dy<0 half-turn branch. dx=0,dy=-5: atan(-0.0)=-0.0; +180=180; +bias=181.4062 -> 65.
    fx.reset();
    g_use_atan_override = false;
    g_stub_dx           = 0.0;
    g_stub_dy           = -5.0;
    {
        int32_t got = detail::dir_sector_to(fx.view(), test_calls_v, 0, 0, 0, 0);
        ck_eq((uint32_t)got, (uint32_t)expected_sector128(0.0, -5.0),
              "dir_sector_to C2: dy<0 half-turn matches the independent formula");
        ck_eq((uint32_t)got, 65u, "dir_sector_to C2: dy=-5 half-turn -> sector 65");
    }

    // ---- C3: dy==0.0 EXACTLY. The half-turn guard is "0.0<=dy skip" (JBE), so dy==0 does NOT take
    // the half-turn branch, AND the division is UNGUARDED (header): dx>0,dy=0 -> +inf -> atan=pi/2.
    fx.reset();
    g_use_atan_override = false;
    g_stub_dx           = 7.0;
    g_stub_dy           = 0.0;
    {
        int32_t got = detail::dir_sector_to(fx.view(), test_calls_v, 0, 0, 0, 0);
        ck_eq((uint32_t)got, (uint32_t)expected_sector128(7.0, 0.0),
              "dir_sector_to C3: dy=0 unguarded division (dx>0), no half-turn, matches the formula");
    }

    // ---- C4: dy==0.0, dx<0 -- same boundary, opposite sign -> -inf -> atan=-pi/2 -> angle=-90+bias
    // is still negative -> the wrap-UP branch (+360) fires; still no half-turn (dy==0 skips it).
    fx.reset();
    g_use_atan_override = false;
    g_stub_dx           = -7.0;
    g_stub_dy           = 0.0;
    {
        int32_t got = detail::dir_sector_to(fx.view(), test_calls_v, 0, 0, 0, 0);
        ck_eq((uint32_t)got, (uint32_t)expected_sector128(-7.0, 0.0),
              "dir_sector_to C4: dy=0,dx<0 -> wrap-UP branch, matches the formula");
    }

    // ---- C5: a large steep-negative case with dy>0 drives atan toward -90 and forces the wrap-UP
    // branch through the ordinary (non-boundary) path.
    fx.reset();
    g_use_atan_override = false;
    g_stub_dx           = -100000.0;
    g_stub_dy           = 1.0;
    {
        int32_t got = detail::dir_sector_to(fx.view(), test_calls_v, 0, 0, 0, 0);
        ck_eq((uint32_t)got, (uint32_t)expected_sector128(-100000.0, 1.0),
              "dir_sector_to C5: steep negative dx, dy>0 -> wrap-UP branch, matches the formula");
    }

    // ---- C6: the wrap-DOWN branch (angle>=360 -> -=360) is UNREACHABLE via any real atan() output
    // for this formula -- same FINDING sim_facing24_from_points_selftest.cpp already reports for its
    // own sibling shape: atan()'s codomain is bounded to (-90,90) degrees, the half-turn conditional
    // adds at most 180, and the bias here is tiny (1.4062), so the pre-wrap angle is bounded in
    // (-88.6, 271.4) -- never reaching 360. Exercised here anyway for CODE-PATH coverage by
    // injecting a raw atan "radians" value (not a real atan(dx/dy)) that pushes the post-bias angle
    // past 360 -- this specific numeric combination cannot occur from real (dx,dy) coordinates in
    // the live game, so this is a translation-fidelity check on the instruction sequence, not a
    // reachable gameplay case.
    fx.reset();
    g_use_atan_override = true;
    g_atan_override     = 6.9; // arbitrary "radians"; NOT a real atan() output -- see above
    g_stub_dx           = 1.0;
    g_stub_dy           = 1.0; // dy>=0 -> no half-turn; dx/dy itself is irrelevant under override
    {
        int32_t got  = detail::dir_sector_to(fx.view(), test_calls_v, 0, 0, 0, 0);
        int32_t want = expected_sector128_raw(6.9, /*dy_is_negative*/ false);
        ck_eq((uint32_t)got, (uint32_t)want,
              "dir_sector_to C6: forced wrap-DOWN branch (unreachable via real atan; code-path check only)");
    }
    g_use_atan_override = false;

    // ---- MUTATION NOTES ------------------------------------------------------------------------
    // 1. Reverting the wrapped_delta call order to the natural (x0,y0,x1,y1,...) fails the very
    //    first check above immediately.
    // 2. Dropping the "if (dy<0.0) angle += half_turn_deg" add makes C2 compute sector 1 instead of
    //    65 (the same as C1, since without it dy's sign has no effect).
    // 3. Dropping the trailing integer "+1" (matching facing24's alternate float-domain "+1" form
    //    instead of the INC-after-trunc this function actually uses) shifts every sector value here
    //    down by exactly one (C1: 1->0, C2: 65->64, ...).
    // 4. Reading the dir24_* constant block instead of dir128_* (e.g. sector_deg=15.0 instead of
    //    2.8125) changes every computed sector value here by roughly a factor of 5, a very visible
    //    red across the whole file.
}

} // namespace mh::sim::test
