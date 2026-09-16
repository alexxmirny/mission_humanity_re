#include "tact/tact_facing_to_delta.h"

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {

using namespace mh::tact;

// Distinct, non-symmetric (dx,dy) per octant slot so a wrong index reads as a wrong pair, and a
// swapped dx/dy reads as a wrong pair too (per-slot dx != dy, and no two slots share a value).
void seed_octants(tact_fixture &fx) {
    for (int32_t i = 0; i < 8; ++i) {
        fx.dir8_delta_table[(size_t)i].dx = 100 + i;  // 100..107
        fx.dir8_delta_table[(size_t)i].dy = -200 - i; // -200..-207
    }
}

} // namespace

void run_facing_to_delta_tests() {
    // T1: facing_dir=1 -> idx=(1-1)/3=0, slot 0.
    {
        tact_fixture fx;
        seed_octants(fx);
        const tact_view v      = fx.view();
        int32_t         out_dx = -1, out_dy = -1;
        detail::facing_to_delta(v, 1, &out_dx, &out_dy);
        ck_eq((uint32_t)out_dx, 100u, "T1: facing_dir=1 -> dir8_delta_table[0].dx, 0x004311f7");
        ck_eq((uint32_t)out_dy, (uint32_t)-200, "T1: facing_dir=1 -> dir8_delta_table[0].dy, 0x00431208");
    }

    // T2: facing_dir=3 -> idx=(3-1)/3=0 (IDIV truncates toward zero, 2/3=0) -- still slot 0, the
    // TOP of octant 0's range.
    {
        tact_fixture fx;
        seed_octants(fx);
        const tact_view v      = fx.view();
        int32_t         out_dx = -1, out_dy = -1;
        detail::facing_to_delta(v, 3, &out_dx, &out_dy);
        ck_eq((uint32_t)out_dx, 100u, "T2: facing_dir=3 -> still slot 0 (2/3 truncates to 0), 0x004311e9-0x004311ec");
    }

    // T3: facing_dir=4 -> idx=(4-1)/3=1, the BOTTOM of octant 1's range -- catches an off-by-one at
    // the octant boundary.
    {
        tact_fixture fx;
        seed_octants(fx);
        const tact_view v      = fx.view();
        int32_t         out_dx = -1, out_dy = -1;
        detail::facing_to_delta(v, 4, &out_dx, &out_dy);
        ck_eq((uint32_t)out_dx, 101u, "T3: facing_dir=4 -> slot 1, octant boundary");
        ck_eq((uint32_t)out_dy, (uint32_t)-201, "T3: facing_dir=4 -> slot 1 dy");
    }

    // T4: facing_dir=24 (dir24's own top value) -> idx=(24-1)/3=7, the LAST slot.
    {
        tact_fixture fx;
        seed_octants(fx);
        const tact_view v      = fx.view();
        int32_t         out_dx = -1, out_dy = -1;
        detail::facing_to_delta(v, 24, &out_dx, &out_dy);
        ck_eq((uint32_t)out_dx, 107u, "T4: facing_dir=24 -> slot 7 (last octant), 0x004311e9");
        ck_eq((uint32_t)out_dy, (uint32_t)-207, "T4: facing_dir=24 -> slot 7 dy");
    }

    // T5: facing_dir=0 -> idx=(0-1)/3=(-1)/3=0 by C++'s (and IDIV's) truncate-toward-zero rule, NOT
    // -1 -- an unguarded input the original leaves unhandled; preserved literally rather than
    // clamped. Pins the truncation direction specifically (a floor-style division would give -1,
    // landing off the front of the table).
    {
        tact_fixture fx;
        seed_octants(fx);
        const tact_view v      = fx.view();
        int32_t         out_dx = -1, out_dy = -1;
        detail::facing_to_delta(v, 0, &out_dx, &out_dy);
        ck_eq((uint32_t)out_dx, 100u,
              "T5: facing_dir=0 -> (-1)/3 truncates to 0 (not -1), 0x004311e9-0x004311ec IDIV");
    }
}

} // namespace mh::tact::test
