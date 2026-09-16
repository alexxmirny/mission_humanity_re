//
// tact_unit_get_muzzle_offset_selftest.cpp -- offline oracle for
//   llm_tact_unit_get_muzzle_offset @0x00432105 (libmh/tact/tact_unit_get_muzzle_offset.cpp)
//
// WHY OFFLINE, NOT RIG: see tact_unit_get_muzzle_offset.h's banner -- writes ONLY through its two
// out-pointers, into the CALLER's stack; there is no tracked state region a shadow site could
// compare. No calls struct: the function makes no outward calls.
//
#include "tact/tact_unit_get_muzzle_offset.h"

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {
using namespace mh::tact;

constexpr int32_t UNIT_ID = 7;

} // namespace

void run_unit_get_muzzle_offset_tests() {
    // T1: weapon_slot == active_gun -> mount1 arm, @0x00432150-0x004321d3.
    {
        tact_fixture fx;
        tact_unit   &u = fx.units[UNIT_ID];
        u.sprite_id    = 3;
        u.active_gun   = 0;
        u.pos_col      = 5;
        u.pos_row      = 9;
        u.type         = 2;

        sprite_meta &sm = fx.sprite_meta_table[3];
        sm.origin_x     = 11;
        sm.origin_y     = 13;
        sm.mount1_x     = 17;
        sm.mount1_y     = 19;
        sm.mount2_x     = 23;
        sm.mount2_y     = 29;

        fx.character_types[2].height_gun1 = 31;
        fx.character_types[2].height_gun2 = 37;

        tact_view v     = fx.view();
        int32_t   out_x = -1, out_y = -1;
        detail::unit_get_muzzle_offset(v, UNIT_ID, &out_x, &out_y, /*weapon_slot=*/0);

        // (pos_col<<5)+0x10 - origin_x + mount1_x = (5<<5)+16-11+17 = 160+16-11+17 = 182
        ck_eq((uint32_t)out_x, 182u, "T1: mount1 out_x, 0x00432150-0x00432185");
        // pos_row*0x18+0xc - origin_y + mount1_y + height_gun1 = 9*24+12-13+19+31 = 216+12-13+19+31 = 265
        ck_eq((uint32_t)out_y, 265u, "T1: mount1 out_y (+height_gun1, NOT height_gun2), 0x00432187-0x004321d3");
    }

    // T2: weapon_slot != active_gun -> mount2 arm, @0x004321da-0x0043225d. SAME unit/sprite as T1
    // but the OTHER slot, so a translation that hardcoded the mount1 branch would fail here.
    {
        tact_fixture fx;
        tact_unit   &u = fx.units[UNIT_ID];
        u.sprite_id    = 3;
        u.active_gun   = 0;
        u.pos_col      = 5;
        u.pos_row      = 9;
        u.type         = 2;

        sprite_meta &sm = fx.sprite_meta_table[3];
        sm.origin_x     = 11;
        sm.origin_y     = 13;
        sm.mount1_x     = 17;
        sm.mount1_y     = 19;
        sm.mount2_x     = 23;
        sm.mount2_y     = 29;

        fx.character_types[2].height_gun1 = 31;
        fx.character_types[2].height_gun2 = 37;

        tact_view v     = fx.view();
        int32_t   out_x = -1, out_y = -1;
        detail::unit_get_muzzle_offset(v, UNIT_ID, &out_x, &out_y, /*weapon_slot=*/1);

        // (5<<5)+16-11+23 = 160+16-11+23 = 188
        ck_eq((uint32_t)out_x, 188u, "T2: mount2 out_x, 0x004321da-0x0043220f");
        // 9*24+12-13+29+37 = 216+12-13+29+37 = 281
        ck_eq((uint32_t)out_y, 281u, "T2: mount2 out_y (+height_gun2, NOT height_gun1), 0x00432211-0x0043225d");
    }

    // T3: active_gun == 1, weapon_slot == 1 -> XOR is 0 -> mount1 arm again, proving the branch is
    // gated on (active_gun ^ weapon_slot), not on weapon_slot alone.
    {
        tact_fixture fx;
        tact_unit   &u = fx.units[UNIT_ID];
        u.sprite_id    = 5;
        u.active_gun   = 1;
        u.pos_col      = 2;
        u.pos_row      = 4;
        u.type         = 0;

        sprite_meta &sm = fx.sprite_meta_table[5];
        sm.origin_x     = 1;
        sm.origin_y     = 2;
        sm.mount1_x     = 3;
        sm.mount1_y     = 4;
        sm.mount2_x     = 99; // must NOT be used
        sm.mount2_y     = 99;

        fx.character_types[0].height_gun1 = 6;
        fx.character_types[0].height_gun2 = 66;

        tact_view v     = fx.view();
        int32_t   out_x = -1, out_y = -1;
        detail::unit_get_muzzle_offset(v, UNIT_ID, &out_x, &out_y, /*weapon_slot=*/1);

        // (2<<5)+16-1+3 = 64+16-1+3 = 82
        ck_eq((uint32_t)out_x, 82u, "T3: active_gun==weapon_slot==1 (XOR 0) -> mount1, out_x");
        // 4*24+12-2+4+6 = 96+12-2+4+6 = 116
        ck_eq((uint32_t)out_y, 116u, "T3: active_gun==weapon_slot==1 (XOR 0) -> mount1, out_y");
    }
}

} // namespace mh::tact::test
