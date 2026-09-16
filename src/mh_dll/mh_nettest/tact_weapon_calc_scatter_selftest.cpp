//
// tact_weapon_calc_scatter_selftest.cpp -- offline oracle for
//   llm_tact_weapon_calc_scatter @0x00430c77 (libmh/tact/tact_weapon_calc_scatter.cpp)
//
// WHY OFFLINE, NOT RIG: see tact_weapon_calc_scatter.h's banner -- writes ONLY through its two
// out-pointers, into the CALLER's stack; there is no tracked state region a shadow site could
// compare. llm_rand is mocked to a fixed value so the case is deterministic.
//
#include "tact/tact_weapon_calc_scatter.h"

#include <cmath>

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {
using namespace mh::tact;

constexpr int32_t BUILDING_ID = 4;

int32_t                   g_rand_value = 0;
weapon_calc_scatter_calls rec_calls() {
    return {
        []() { return g_rand_value; },
        [](double x) { return std::sqrt(x); },
    };
}

} // namespace

void run_weapon_calc_scatter_tests() {
    // NOTE (fixed 2026-08-26): fx_type.precise / .precise_kneel are `uint8_t` in the live struct
    // (mh_structs.gen.h) -- the first version of this oracle seeded them with values >255 (800,
    // 5000, 400), which silently truncated at the `wt.precise = ...` assignment and made every
    // expected value below wrong before a single divergence could be observed. All seeds here are
    // <=255; expected values recomputed from the truncating-C-division formula directly (verified
    // independently in Python, not by re-reading this file).

    // T1: gun_slot = weapon_slot ^ active_gun selects number_gun2 (adjacent-byte idiom); NOT
    // kneeling -> `precise` (not `precise_kneel`); facing_dir picks octant 3 with no wrap.
    {
        tact_fixture fx;
        tact_unit   &u = fx.units[BUILDING_ID];
        u.type         = 1;
        u.active_gun   = 1;
        u.anim_state   = 0; // not kneeling -> `precise`
        u.facing_dir   = 4; // octant = (4-1)/3 + 2 = 3, no wrap

        character_type &ct = fx.character_types[1];
        ct.number_gun1     = 9; // must NOT be selected (weapon_slot=0 ^ active_gun=1 = index 1)
        ct.number_gun2     = 5; // weapon_type_id, SELECTED

        fx_type &wt      = fx.fx_type_table[5];
        wt.precise       = 200;
        wt.precise_kneel = 90; // must NOT be used (not kneeling)

        fx.dir8_delta_table[3].dx = 2;
        fx.dir8_delta_table[3].dy = -3;

        g_rand_value = 8192; // scaled = (200*8192)/16384 = 100 exactly

        tact_view v      = fx.view();
        int32_t   out_dx = -999, out_dy = -999;
        detail::weapon_calc_scatter(v, rec_calls(), BUILDING_ID, /*x=*/0, /*y=*/0, /*tx=*/3, /*ty=*/4,
                                    &out_dx, &out_dy, /*weapon_slot=*/0);

        // spread = scaled - precise = 100 - 200 = -100. dist = trunc(sqrt((0-3)^2+(0-4)^2)) =
        // trunc(sqrt(25)) = 5. pre-octant out_dx = (-100*5)/320 = -500/320 = -1 (truncates toward
        // zero). pre-octant out_dy = -500/426 = -1. Final: out_dx = -1*2 = -2, out_dy = -1*-3 = 3.
        ck_eq((uint32_t)out_dx, (uint32_t)-2, "T1: out_dx via number_gun2/precise/octant 3, 0x00430c98-0x00430dd0");
        ck_eq((uint32_t)out_dy, 3u, "T1: out_dy via number_gun2/precise/octant 3, 0x00430dd2-0x00430de6");
    }

    // T2: kneeling (anim_state==2) -> `precise_kneel`, NOT `precise`; facing_dir=24 wraps the
    // octant (9 -> 1, @0x00430db2-0x00430dbc); gun_slot selects number_gun1 this time.
    {
        tact_fixture fx;
        tact_unit   &u = fx.units[BUILDING_ID];
        u.type         = 2;
        u.active_gun   = 0;
        u.anim_state   = 2;  // kneeling -> `precise_kneel`
        u.facing_dir   = 24; // octant = (24-1)/3 + 2 = 7+2 = 9 -> wraps to 1

        character_type &ct = fx.character_types[2];
        ct.number_gun1     = 6;  // weapon_type_id, SELECTED (weapon_slot=0 ^ active_gun=0 = index 0)
        ct.number_gun2     = 60; // must NOT be selected

        fx_type &wt      = fx.fx_type_table[6];
        wt.precise       = 90; // must NOT be used (kneeling)
        wt.precise_kneel = 200;

        fx.dir8_delta_table[1].dx = -5;
        fx.dir8_delta_table[1].dy = 7;

        g_rand_value = 16384; // scaled = (200*16384)/16384 = 200 exactly -> spread = 0

        tact_view v      = fx.view();
        int32_t   out_dx = -999, out_dy = -999;
        detail::weapon_calc_scatter(v, rec_calls(), BUILDING_ID, /*x=*/10, /*y=*/10, /*tx=*/10, /*ty=*/10,
                                    &out_dx, &out_dy, /*weapon_slot=*/0);

        // spread = 200-200 = 0 -> both outputs are 0 regardless of dist/octant -- a clean case for
        // proving the kneeling branch selected precise_kneel (had it used `precise` = 90, spread
        // would be 200-90 = 110, decisively nonzero).
        ck_eq((uint32_t)out_dx, 0u, "T2: kneeling selects precise_kneel (spread==0), out_dx");
        ck_eq((uint32_t)out_dy, 0u, "T2: kneeling selects precise_kneel (spread==0), out_dy");
    }

    // T3: same kneeling case as T2 but with a genuinely nonzero spread, to confirm the octant-9
    // WRAP (not octant 9 itself, which would index out of the 8-entry table) is applied.
    {
        tact_fixture fx;
        tact_unit   &u = fx.units[BUILDING_ID];
        u.type         = 2;
        u.active_gun   = 0;
        u.anim_state   = 3;  // kneeling (the OTHER kneeling value) -> precise_kneel
        u.facing_dir   = 24; // wraps to octant 1

        character_type &ct = fx.character_types[2];
        ct.number_gun1     = 6;

        fx_type &wt      = fx.fx_type_table[6];
        wt.precise_kneel = 200;

        fx.dir8_delta_table[1].dx = 3;
        fx.dir8_delta_table[1].dy = -1;

        g_rand_value = 8192; // scaled = (200*8192)/16384 = 100 -> spread = 100-200 = -100

        tact_view v      = fx.view();
        int32_t   out_dx = -999, out_dy = -999;
        detail::weapon_calc_scatter(v, rec_calls(), BUILDING_ID, /*x=*/0, /*y=*/0, /*tx=*/3, /*ty=*/4,
                                    &out_dx, &out_dy, /*weapon_slot=*/0);

        // dist = 5 (as T1). pre-octant out_dx = -500/320 = -1; out_dy = -500/426 = -1.
        // octant 1: dx=3,dy=-1 -> final out_dx = -1*3 = -3, out_dy = -1*-1 = 1.
        ck_eq((uint32_t)out_dx, (uint32_t)-3, "T3: octant-9-wraps-to-1 applied to out_dx");
        ck_eq((uint32_t)out_dy, 1u, "T3: octant-9-wraps-to-1 applied to out_dy");
    }
}

} // namespace mh::tact::test
