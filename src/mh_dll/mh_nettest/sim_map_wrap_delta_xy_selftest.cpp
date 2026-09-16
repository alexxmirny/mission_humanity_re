//
// sim_map_wrap_delta_xy_selftest.cpp -- `simtest` offline oracle for
// llm_map_wrap_delta_x / llm_map_wrap_delta_y (sim/sim_map_wrap_delta_xy.h, RI-SIM / SIM1-G2 sixth
// slice). Both are NOT SHADOWABLE (0 tracked write cells per the write-closure derivation), so this
// is their only execution-proof evidence, alongside adversarial review.
//
#include "sim/sim_map_wrap_delta_xy.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {
using namespace mh::sim;
} // namespace

void run_map_wrap_delta_xy_tests() {
    // ---- llm_map_wrap_delta_x: width=200, half=100 -------------------------------------------------
    {
        sim_fixture fx;
        fx.reset();
        fx.map_width  = 200;
        fx.map_height = 120; // deliberately different from width -- catches a width/height mix-up
        sim_view v    = fx.view();

        ck_eq((uint32_t)detail::wrap_delta_x(v, 10, 999, 15), 5u,
              "wrap_delta_x: pos_a<pos_b, no fold -> 5");
        ck_eq((uint32_t)detail::wrap_delta_x(v, 50, 0, 50), 0u, "wrap_delta_x: equal positions -> 0");
        ck_eq((uint32_t)(int32_t)detail::wrap_delta_x(v, 0, 0, 100), 100u,
              "wrap_delta_x: delta==half exactly -- NOT folded (strict half<delta)");
        ck_eq((uint32_t)(int32_t)detail::wrap_delta_x(v, 0, 0, 101), (uint32_t)(int32_t)-99,
              "wrap_delta_x: delta==half+1 -- folds, sign flips -> -99");
        ck_eq((uint32_t)(int32_t)detail::wrap_delta_x(v, 190, 0, 5), 15u,
              "wrap_delta_x: pos_a>pos_b with fold -> +15 (190->195->0->5 forward)");
        ck_eq((uint32_t)(int32_t)detail::wrap_delta_x(v, 199, 0, 0), 1u,
              "wrap_delta_x: pos_a>pos_b, fold flips sign back to + -> 1");
        // unused_param must never affect the result.
        ck_eq((uint32_t)detail::wrap_delta_x(v, 10, 0xdeadbeefu, 15), 5u,
              "wrap_delta_x: unused_param does not affect the result");
    }

    // ---- llm_map_wrap_delta_y: height=120, half=60 -- x1/x2 must be dead ----------------------------
    {
        sim_fixture fx;
        fx.reset();
        fx.map_width  = 200;
        fx.map_height = 120;
        sim_view v    = fx.view();

        ck_eq((uint32_t)detail::wrap_delta_y(v, 0, 10, 0, 15), 5u,
              "wrap_delta_y: y1<y2, no fold -> 5");
        ck_eq((uint32_t)detail::wrap_delta_y(v, 0, 30, 0, 30), 0u, "wrap_delta_y: equal positions -> 0");
        ck_eq((uint32_t)(int32_t)detail::wrap_delta_y(v, 0, 0, 0, 60), 60u,
              "wrap_delta_y: delta==half exactly -- NOT folded");
        ck_eq((uint32_t)(int32_t)detail::wrap_delta_y(v, 0, 0, 0, 61), (uint32_t)(int32_t)-59,
              "wrap_delta_y: delta==half+1 -- folds, sign flips -> -59");
        ck_eq((uint32_t)(int32_t)detail::wrap_delta_y(v, 0, 110, 0, 5), 15u,
              "wrap_delta_y: y1>y2 with fold -> +15 (110->115->0->5 forward)");
        // x1/x2 must be dead: wildly different values must not change the result.
        ck_eq((uint32_t)detail::wrap_delta_y(v, 12345, 10, -9876, 15), 5u,
              "wrap_delta_y: x1/x2 are dead parameters");
    }
}

} // namespace mh::sim::test
