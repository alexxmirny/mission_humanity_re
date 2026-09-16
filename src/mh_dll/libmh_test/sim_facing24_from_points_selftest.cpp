//
// sim_facing24_from_points_selftest.cpp -- `simtest` cases for llm_strat_facing24_from_points
// (sim/sim_facing24_from_points.h/.cpp), SIM1A.
//
// Pure math, no sim_view/sim_store at all -- call detail::facing24_from_points() directly with
// literal args and a real-atan mock (the ONE outward call this function has is indirected purely
// so `simtest` can drive it offline; the mock still has to compute REAL atan(), not record/stub,
// because the expected sector values below are computed the same way).
//
// Expected sector values were computed with a Python one-liner using math.atan and the EXACT same
// truncated-pi constant this file's header banner documents (3.1415926536, NOT textbook pi) --
// see the header's "CONDUCTOR-CONFIRMED" note on RAD2DEG_DEN. Hand-replicating the FP chain here
// would risk 1e-11-level drift from a slightly different rounding path; the Python computation
// used the identical formula (atan -> *180/RAD2DEG_DEN -> conditional +180 -> +7 -> conditional
// wrap -> 1+trunc(angle/15)) so the two should agree exactly at these particular sector boundaries.
//
// FINDING (not a bug, reported per the task brief): the wrap-DOWN branch (`angle_deg >=
// WRAP_LIMIT_DEG` -> `+= WRAP_SUB_DEG`) appears to be UNREACHABLE by any real (dx, dy) input to
// this formula: atan()'s codomain is (-90, 90) degrees, so after the conditional +180 (dy<0) and
// the unconditional +7 bias, the pre-wrap-up angle is bounded in (-83, 277) -- and the wrap-UP step
// only ever adds 360 when that value is already negative, landing the result strictly below 360
// (asymptotically approaching 360 from below as dx/dy -> a boundary, never reaching it). An
// exhaustive sweep of the full signed-char domain (every (from_x,from_y,to_x,to_y) combination the
// real parameter types can express) found no input that takes the wrap-down branch. This mirrors
// sim_unit_housing_count.cpp's own vestigial-compare finding (a real branch in the original that is
// nonetheless dead over its actual input domain) -- reported, not "fixed": the branch is preserved
// in the translation exactly as the original has it.
//
#include "sim/sim_facing24_from_points.h"

#include <cmath>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

const facing24_from_points_calls &real_atan_calls() {
    static const facing24_from_points_calls c = {
        [](double x) -> double { return std::atan(x); },
    };
    return c;
}

void test_facing24_no_wrap_dy_ge0() {
    // from=(0,0) to=(0,5): dx=0-0=0, dy=5-0=5 (dy>=0, no half-turn). angle = atan(0/5)*.. + 7 =
    // 7.0 exactly (atan(0)==0) -> sector = trunc(7/15 + 1) = 1.
    const uint8_t got = detail::facing24_from_points(0, 0, 0, 5, real_atan_calls());
    ck((int)got == 1, "facing24_from_points: dx=0,dy=5 (straight, dy>=0, no wrap) -> sector 1");
}

void test_facing24_half_turn_dy_lt0() {
    // from=(0,0) to=(0,-5): dx=0, dy=-5-0=-5 (dy<0 -> +180 half-turn). angle = 0 + 180 + 7 = 187.0
    // exactly -> sector = trunc(187/15 + 1) = trunc(13.4667) = 13.
    const uint8_t got = detail::facing24_from_points(0, 0, 0, -5, real_atan_calls());
    ck((int)got == 13, "facing24_from_points: dx=0,dy=-5 (dy<0, half-turn branch taken) -> sector 13");
}

void test_facing24_wrap_up_branch() {
    // from=(0,0) to=(20,1): dx=0-20=-20, dy=1-0=1 (dy>=0, no half-turn). atan(-20/1) is steeply
    // negative; +7 bias still leaves it <0.0, so the wrap-UP branch (+360) fires. Computed
    // (Python, atan + the game's truncated-pi RAD2DEG_DEN): angle == 279.862405226394856 ->
    // sector = trunc(279.8624/15 + 1) = trunc(19.657...) = 19.
    const uint8_t got = detail::facing24_from_points(0, 0, 20, 1, real_atan_calls());
    ck((int)got == 19,
       "facing24_from_points: steep negative angle (dx=-20,dy=1) triggers the wrap-UP branch -> sector 19");
}

void test_facing24_normal_points_mixed_quadrant() {
    // from=(3,3) to=(0,0): dx=3-0=3, dy=0-3=-3 (dy<0 -> half-turn). atan(3/-3)=atan(-1)=-45deg;
    // +180=135; +7=142.0 exactly -> sector = trunc(142/15 + 1) = trunc(10.4667) = 10.
    const uint8_t got = detail::facing24_from_points(3, 3, 0, 0, real_atan_calls());
    ck((int)got == 10, "facing24_from_points: dx=3,dy=-3 (45-degree diagonal, dy<0) -> sector 10");
}

void test_facing24_normal_points_second_case() {
    // from=(0,0) to=(-5,5): dx=0-(-5)=5, dy=5-0=5 (dy>=0, no half-turn). Computed: angle ==
    // 51.999999999853799 -> sector = trunc(52.0/15 + 1) = trunc(4.4667) = 4.
    const uint8_t got = detail::facing24_from_points(0, 0, -5, 5, real_atan_calls());
    ck((int)got == 4, "facing24_from_points: dx=5,dy=5 (dy>=0, diagonal) -> sector 4");
}

} // namespace

void run_facing24_from_points_tests() {
    test_facing24_no_wrap_dy_ge0();
    test_facing24_half_turn_dy_lt0();
    test_facing24_wrap_up_branch();
    test_facing24_normal_points_mixed_quadrant();
    test_facing24_normal_points_second_case();
}

} // namespace mh::sim::test
