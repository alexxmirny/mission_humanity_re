//
// tact_calc_dir24_selftest.cpp -- offline oracle for llm_tact_calc_dir24 @0x0042e0e2
//   (libmh/tact/tact_calc_dir24.cpp)
//
// Pure math, no tact_view/tact_store at all -- call detail::calc_dir24() directly with literal args
// and a real-atan mock (the ONE outward call this function has is indirected purely so
// `net_selftest.exe tacttest` can drive it offline; the mock computes REAL atan(), not a
// record/stub, because the expected sectors below are computed the same way).
//
// Expected values were computed with a Python one-liner reproducing the EXACT formula (atan ->
// int(angle_rad*180/PI_APPROX) [truncate to int HERE, unlike the sim sibling which stays in the
// double domain until the very end] -> conditional +180 -> conditional wrap -> /15 -> +1 -> clamp),
// using the same truncated-pi constant (3.1415926536, NOT textbook pi) the header banner documents.
//
// FINDING (not a bug, same shape as sim_facing24_from_points_selftest.cpp's own): an exhaustive grid
// sweep over (x1,y1,x2,y2) in [-30,30] found the wrap-DOWN branch (`angle_deg >= 360` -> `-= 360`)
// and the `sector > 24` clamp BOTH structurally unreachable -- atan()'s codomain bounds the raw
// truncated angle to (-90, 90), so after the conditional +180 (dy<0) the value is already < 360, and
// after the +15-sector conversion the result is always in [1, 24]. Both branches are preserved in
// the translation exactly as the original has them (Law 2), not simplified away.
//
#include "tact/tact_calc_dir24.h"

#include <cmath>

#include "tact_test_support.h"

namespace mh::tact::test {
namespace {

using namespace mh::tact;

const calc_dir24_calls &real_atan_calls() {
    static const calc_dir24_calls c = {
        [](double x) -> double { return std::atan(x); },
    };
    return c;
}

} // namespace

void run_calc_dir24_tests() {
    // T1: exact coincidence -> the early-out, 0x0042e106-0x0042e11c. atan is never even reached.
    {
        const int32_t got = detail::calc_dir24(5, 5, 5, 5, real_atan_calls());
        ck_eq((uint32_t)got, 1u, "T1: (x1,y1)==(x2,y2) -> early-out return 1, 0x0042e115");
    }

    // T2: dx=0,dy=5 (dy>=0, no half-turn). angle_rad=atan(0)=0 -> angle_deg=0 -> sector=0/15+1=1.
    {
        const int32_t got = detail::calc_dir24(0, 0, 0, 5, real_atan_calls());
        ck_eq((uint32_t)got, 1u, "T2: dx=0,dy=5 (straight, dy>=0) -> sector 1");
    }

    // T3: dx=0,dy=-5 (dy<0 -> +180 half-turn). angle_deg=0+180=180 -> sector=180/15+1=13.
    {
        const int32_t got = detail::calc_dir24(0, 0, 0, -5, real_atan_calls());
        ck_eq((uint32_t)got, 13u, "T3: dx=0,dy=-5 (dy<0, half-turn branch taken) -> sector 13");
    }

    // T4: dx=0-20=-20,dy=1-0=1 (dy>=0). atan(-20/1) steeply negative; int-truncated degrees stay
    // negative (no dy<0 branch to add 180 here) -> the wrap-UP (+360) branch fires.
    // Python: angle_deg=int(atan(-20/1)*180/PI_APPROX) == -87 -> +360 == 273 -> sector=273/15+1=19.
    {
        const int32_t got = detail::calc_dir24(0, 0, 20, 1, real_atan_calls());
        ck_eq((uint32_t)got, 19u,
              "T4: steep negative angle (dx=-20,dy=1) triggers the wrap-UP branch -> sector 19");
    }

    // T5: dx=3-0=3,dy=0-3=-3 (dy<0 -> half-turn). atan(3/-3)=atan(-1)=-45deg exactly;
    // angle_deg=int(-45.0)=-45; +180=135 -> sector=135/15+1=10.
    {
        const int32_t got = detail::calc_dir24(3, 3, 0, 0, real_atan_calls());
        ck_eq((uint32_t)got, 10u, "T5: dx=3,dy=-3 (45-degree diagonal, dy<0) -> sector 10");
    }

    // T6: dx=0-(-5)=5,dy=5-0=5 (dy>=0). Python: angle_deg=int(atan(1)*180/PI_APPROX)==44 (int-trunc
    // of ~44.9999..., NOT 45 -- pins the EARLY int truncation, distinct from the sim sibling's
    // late-trunc formula which would keep 44.9999... through the /15 step) -> sector=44/15+1=3.
    {
        const int32_t got = detail::calc_dir24(0, 0, -5, 5, real_atan_calls());
        ck_eq((uint32_t)got, 3u, "T6: dx=5,dy=5 (dy>=0, diagonal) -> sector 3, pins the EARLY int trunc");
    }
}

} // namespace mh::tact::test
