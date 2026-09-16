//
// tact_unit_fire_weapon_selftest.cpp -- offline oracle for
//   llm_tact_unit_fire_weapon @0x00430855 (libmh/tact/tact_unit_fire_weapon.h)
//
// WHY OFFLINE, NOT RIG: see tact_unit_fire_weapon.h's banner -- 35 functions / 45 regions reachable
// (UI/font presentation scratch via the char-panel/sidebar refresh calls), structurally expensive to
// rig-arm for no verification gain. All seven outward calls are mocked via the calls struct.
//
#include "tact/tact_unit_fire_weapon.h"

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {
using namespace mh::tact;

constexpr int32_t BUILDING_ID = 4;

struct fire_recorder {
    int32_t             calc_dir24_return = 0;
    std::vector<double> time_now_returns; // popped in order, LAST value repeats if exhausted
    size_t              time_now_idx = 0;

    int32_t rotate_step_calls      = 0;
    int32_t rotate_step_target_dir = -1;

    int32_t char_panel_refresh_calls = 0;
    int32_t sidebar_redraw_calls     = 0;

    int32_t muzzle_offset_calls = 0;
    int32_t muzzle_x_return = 0, muzzle_y_return = 0;

    int32_t scatter_calls = 0;
    int32_t scatter_x = 0, scatter_y = 0, scatter_tx = 0, scatter_ty = 0, scatter_slot = -1;
    int32_t scatter_out_dx_return = 0, scatter_out_dy_return = 0;

    int32_t fx_spawn_calls = 0;
    int32_t fx_spawn_type  = -1;
    uint8_t fx_spawn_owner = 0;
    int32_t fx_spawn_x = 0, fx_spawn_y = 0, fx_spawn_x2 = 0, fx_spawn_y2 = 0;
    uint8_t fx_spawn_altitude = 0;

    void   reset() { *this = fire_recorder{}; }
    double next_time() {
        if (time_now_returns.empty()) return 0.0;
        double v = time_now_returns[time_now_idx < time_now_returns.size() ? time_now_idx
                                                                           : time_now_returns.size() - 1];
        ++time_now_idx;
        return v;
    }
};
fire_recorder g_rec;

unit_fire_weapon_calls rec_calls() {
    return {
        []() { return g_rec.next_time(); },
        [](int32_t, int32_t, int32_t, int32_t) { return g_rec.calc_dir24_return; },
        [](int32_t, int32_t target_dir) {
            g_rec.rotate_step_calls++;
            g_rec.rotate_step_target_dir = target_dir;
        },
        [](int32_t) { g_rec.char_panel_refresh_calls++; },
        [](int32_t) { g_rec.sidebar_redraw_calls++; },
        [](int32_t, int32_t *out_x, int32_t *out_y, uint32_t) {
            g_rec.muzzle_offset_calls++;
            *out_x = g_rec.muzzle_x_return;
            *out_y = g_rec.muzzle_y_return;
        },
        [](int32_t, int32_t x, int32_t y, int32_t tx, int32_t ty, int32_t *out_dx, int32_t *out_dy,
           int32_t weapon_slot) {
            g_rec.scatter_calls++;
            g_rec.scatter_x    = x;
            g_rec.scatter_y    = y;
            g_rec.scatter_tx   = tx;
            g_rec.scatter_ty   = ty;
            g_rec.scatter_slot = weapon_slot;
            *out_dx            = g_rec.scatter_out_dx_return;
            *out_dy            = g_rec.scatter_out_dy_return;
        },
        [](int32_t fx_type, uint8_t owner, int32_t x, int32_t y, int32_t x2, int32_t y2,
           uint8_t altitude) -> int32_t {
            g_rec.fx_spawn_calls++;
            g_rec.fx_spawn_type     = fx_type;
            g_rec.fx_spawn_owner    = owner;
            g_rec.fx_spawn_x        = x;
            g_rec.fx_spawn_y        = y;
            g_rec.fx_spawn_x2       = x2;
            g_rec.fx_spawn_y2       = y2;
            g_rec.fx_spawn_altitude = altitude;
            return 0;
        },
    };
}

} // namespace

void run_unit_fire_weapon_tests() {
    // T1: facing NOT aligned, progress > 0 -- already turning/busy: zero calls, zero writes.
    // 0x004308f0-0x004308fe.
    {
        tact_fixture fx;
        g_rec.reset();
        tact_unit &u            = fx.units[BUILDING_ID];
        u.facing_dir            = 1;
        g_rec.calc_dir24_return = 5; // mismatch
        u.progress              = 10;

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::unit_fire_weapon(v, own, rec_calls(), BUILDING_ID, /*fire_arg=*/0);

        ck_eq((uint32_t)g_rec.rotate_step_calls, 0u, "T1: no rotate_step (already turning)");
        ck_eq((uint32_t)own.unit_at(BUILDING_ID).attack_cmd_op, 0u, "T1: attack_cmd_op untouched (starts 0)");
    }

    // T2: facing NOT aligned, progress == 0 -- turn toward target_dir, fire nothing this call.
    // 0x00430904-0x0043090f.
    {
        tact_fixture fx;
        g_rec.reset();
        tact_unit &u            = fx.units[BUILDING_ID];
        u.facing_dir            = 1;
        g_rec.calc_dir24_return = 5;
        u.progress              = 0;

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::unit_fire_weapon(v, own, rec_calls(), BUILDING_ID, /*fire_arg=*/0);

        ck_eq((uint32_t)g_rec.rotate_step_calls, 1u, "T2: exactly one rotate_step, 0x00430904");
        ck_eq((uint32_t)g_rec.rotate_step_target_dir, 5u, "T2: ...toward calc_dir24's result");
        ck_eq((uint32_t)g_rec.fx_spawn_calls, 0u, "T2: no fire this call");
    }

    // T3: facing aligned, COOLDOWN NOT ELAPSED -- attack_cmd_op=0, return; weapon_timer is NOT the
    // second (stamping) read, only the single cooldown-check read happens. 0x0043093b-0x0043096c.
    {
        tact_fixture fx;
        g_rec.reset();
        tact_unit &u            = fx.units[BUILDING_ID];
        u.type                  = 1;
        u.facing_dir            = 3;
        g_rec.calc_dir24_return = 3; // aligned
        u.active_gun            = 0;
        u.weapon_timer          = 100.0;

        character_type &ct = fx.character_types[1];
        ct.number_gun1     = 5; // fire_arg(0) ^ active_gun(0) = slot 0 -> number_gun1

        fx_type &wt   = fx.fx_type_table[5];
        wt.speed_fire = 10.0;

        g_rec.time_now_returns = {105.0}; // 105-100 = 5 < 10 -> cooldown not elapsed

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::unit_fire_weapon(v, own, rec_calls(), BUILDING_ID, /*fire_arg=*/0);

        ck_eq((uint32_t)own.unit_at(BUILDING_ID).attack_cmd_op, 0u, "T3: attack_cmd_op = 0, 0x00430968");
        ck_eq_d(own.unit_at(BUILDING_ID).weapon_timer, 100.0, "T3: weapon_timer NOT re-stamped, 0x0043093b-0x0043096c");
        ck_eq((uint32_t)g_rec.fx_spawn_calls, 0u, "T3: no fire");
        ck_eq((uint32_t)g_rec.char_panel_refresh_calls, 0u, "T3: no gun-switch call either");
    }

    // T4: cooldown elapsed, chosen gun AND the other gun both empty -- attack_cmd_op=0, return, but
    // weapon_timer WAS re-stamped (the second, unconditional clock read at 0x00430971-0x0043097d
    // happens BEFORE the ammo check). 0x004309d6-0x004309db.
    {
        tact_fixture fx;
        g_rec.reset();
        tact_unit &u            = fx.units[BUILDING_ID];
        u.type                  = 1;
        u.facing_dir            = 3;
        g_rec.calc_dir24_return = 3;
        u.active_gun            = 0;
        u.weapon_timer          = 100.0;
        u.gun1_bullets          = 0;
        u.gun2_bullets          = 0;

        character_type &ct = fx.character_types[1];
        ct.number_gun1     = 5;

        fx_type &wt   = fx.fx_type_table[5];
        wt.speed_fire = 10.0;

        g_rec.time_now_returns = {200.0, 201.0};

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::unit_fire_weapon(v, own, rec_calls(), BUILDING_ID, /*fire_arg=*/0);

        ck_eq((uint32_t)own.unit_at(BUILDING_ID).attack_cmd_op, 0u, "T4: attack_cmd_op = 0, 0x004309db");
        ck_eq_d(own.unit_at(BUILDING_ID).weapon_timer, 201.0, "T4: weapon_timer WAS re-stamped, 0x00430971-0x0043097d");
        ck_eq((uint32_t)g_rec.char_panel_refresh_calls, 0u, "T4: no gun-switch (other gun also empty)");
        ck_eq((uint32_t)g_rec.fx_spawn_calls, 0u, "T4: no fire");
    }

    // T5: cooldown elapsed, chosen gun empty but the OTHER gun has ammo -- toggles active_gun,
    // refreshes the char panel, does NOT fire this call. 0x004309ad-0x004309d1.
    {
        tact_fixture fx;
        g_rec.reset();
        tact_unit &u            = fx.units[BUILDING_ID];
        u.type                  = 1;
        u.facing_dir            = 3;
        g_rec.calc_dir24_return = 3;
        u.active_gun            = 0;
        u.weapon_timer          = 100.0;
        u.gun1_bullets          = 0;
        u.gun2_bullets          = 7;

        character_type &ct = fx.character_types[1];
        ct.number_gun1     = 5;

        fx_type &wt   = fx.fx_type_table[5];
        wt.speed_fire = 10.0;

        g_rec.time_now_returns = {200.0, 201.0};

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::unit_fire_weapon(v, own, rec_calls(), BUILDING_ID, /*fire_arg=*/0);

        ck_eq((uint32_t)own.unit_at(BUILDING_ID).active_gun, 1u, "T5: active_gun toggled, 0x004309ad");
        ck_eq((uint32_t)g_rec.char_panel_refresh_calls, 1u, "T5: char panel refreshed once, 0x004309b8");
        ck_eq((uint32_t)g_rec.fx_spawn_calls, 0u, "T5: does NOT fire this call, 0x004309d1 return");
    }

    // T6: normal fire, own aim point (no hover), non-kneeling -- decrement without hitting 0, no
    // reload, weapon_timer stamped once (no += repeat), fx_spawn via height_gun1, THIRD clock read
    // into move_state_timer. 0x004309f0-0x00430c6f.
    {
        tact_fixture fx;
        g_rec.reset();
        tact_unit &u            = fx.units[BUILDING_ID];
        u.type                  = 1;
        u.facing_dir            = 3;
        g_rec.calc_dir24_return = 3;
        u.active_gun            = 0;
        u.weapon_timer          = 100.0;
        u.gun1_bullets          = 3; // decrements to 2, does not hit 0
        u.gun1_magazines        = 9; // must NOT be touched (no reload)
        u.anim_state            = 0; // not kneeling -> height_gun1
        u.aim_x                 = 40;
        u.aim_y                 = 50;
        fx.hovered_unit_id      = -1; // no hover -> own aim point

        character_type &ct = fx.character_types[1];
        ct.number_gun1     = 5;
        ct.who             = 9;
        ct.height_gun1     = 33;
        ct.kneel_gun1      = 77; // must NOT be used (not kneeling)

        fx_type &wt   = fx.fx_type_table[5];
        wt.speed_fire = 10.0;

        g_rec.time_now_returns      = {200.0, 201.0, 300.0};
        g_rec.muzzle_x_return       = 111;
        g_rec.muzzle_y_return       = 222;
        g_rec.scatter_out_dx_return = 7;
        g_rec.scatter_out_dy_return = -3;

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::unit_fire_weapon(v, own, rec_calls(), BUILDING_ID, /*fire_arg=*/0);

        ck_eq((uint32_t)own.unit_at(BUILDING_ID).gun1_bullets, 2u, "T6: bullets decremented, no reload, 0x004309f0");
        ck_eq((uint32_t)own.unit_at(BUILDING_ID).gun1_magazines, 9u, "T6: magazines untouched (no reload)");
        ck_eq_d(own.unit_at(BUILDING_ID).weapon_timer, 201.0, "T6: weapon_timer NOT bumped by repeat (no reload)");
        ck_eq((uint32_t)g_rec.sidebar_redraw_calls, 1u, "T6: sidebar redrawn once, 0x00430a6d");
        ck_eq((uint32_t)g_rec.muzzle_offset_calls, 1u, "T6: muzzle offset queried once, 0x00430a99");
        ck_eq((uint32_t)g_rec.scatter_calls, 1u, "T6: scatter computed once");
        ck_eq((uint32_t)g_rec.scatter_x, 111u, "T6: scatter's (x,y) = muzzle offset, not the tile anchor");
        ck_eq((uint32_t)g_rec.scatter_y, 222u, "T6: ...");
        ck_eq((uint32_t)g_rec.scatter_tx, 40u, "T6: scatter's (tx,ty) = own aim point (no hover)");
        ck_eq((uint32_t)g_rec.scatter_ty, 50u, "T6: ...");
        ck_eq((uint32_t)g_rec.scatter_slot, 0u, "T6: scatter's weapon_slot = fire_arg (0), not gun_slot");
        ck_eq((uint32_t)g_rec.fx_spawn_calls, 1u, "T6: fx_spawn called once, 0x00430b8f");
        ck_eq((uint32_t)g_rec.fx_spawn_type, 5u, "T6: fx_spawn's fx_type = weapon_type_id (5)");
        ck_eq((uint32_t)g_rec.fx_spawn_owner, 9u, "T6: fx_spawn's owner = character_type.who");
        ck_eq((uint32_t)g_rec.fx_spawn_x, 111u, "T6: fx_spawn's (x,y) = muzzle offset");
        ck_eq((uint32_t)g_rec.fx_spawn_y, 222u, "T6: ...");
        ck_eq((uint32_t)g_rec.fx_spawn_x2, 47u, "T6: fx_spawn's x2 = tx + out_dx = 40+7");
        // THE OWN-AIM BRANCH ADDS THE ALTITUDE INTO y2 AS WELL, and this case asserted the hovered
        // branch's arithmetic until 2026-09-04 (TACT1-P C5) -- as did the body. The two fx_spawn
        // call sites differ by one instruction: `ADD EAX,[EBP-0x2c]` at 0x00430c29, present only in
        // the own-aim branch (0x00430c23-0x00430c33) and absent from the hovered one (0x00430b5c).
        // This fixture drives the own-aim path ("scatter's (tx,ty) = own aim point (no hover)"
        // above), so y2 = ty + out_dy + altitude = 50 - 3 + 33 = 80.
        ck_eq((uint32_t)g_rec.fx_spawn_y2, (uint32_t)80,
              "T6: fx_spawn's y2 = ty + out_dy + altitude = 50-3+33 = 80 (own-aim branch), 0x00430c29");
        ck_eq((uint32_t)g_rec.fx_spawn_altitude, 33u, "T6: altitude = height_gun1 (not kneeling), 0x00430b7e");
        ck_eq((uint32_t)own.unit_at(BUILDING_ID).attack_cmd_op, 0u, "T6: attack_cmd_op = 0 after fire");
        ck_eq_d(own.unit_at(BUILDING_ID).move_state_timer, 300.0, "T6: THIRD clock read -> move_state_timer, 0x00430c5d");
    }

    // T7: the reload path -- decrementing hits 0 AND a magazine remains: consume a magazine, refill
    // bullets from fx_type.bullets, and ADD fx_type.repeat onto the just-stamped weapon_timer.
    // 0x004309f9-0x00430a6a.
    {
        tact_fixture fx;
        g_rec.reset();
        tact_unit &u            = fx.units[BUILDING_ID];
        u.type                  = 1;
        u.facing_dir            = 3;
        g_rec.calc_dir24_return = 3;
        u.active_gun            = 0;
        u.weapon_timer          = 100.0;
        u.gun1_bullets          = 1; // decrements to 0 -- triggers reload check
        u.gun1_magazines        = 2;
        u.anim_state            = 0;
        u.aim_x                 = 1;
        u.aim_y                 = 1;
        fx.hovered_unit_id      = -1;

        character_type &ct = fx.character_types[1];
        ct.number_gun1     = 5;
        ct.height_gun1     = 1;

        fx_type &wt   = fx.fx_type_table[5];
        wt.speed_fire = 10.0;
        wt.bullets    = 8;
        wt.repeat     = 4.5;

        g_rec.time_now_returns = {200.0, 201.0, 300.0};

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::unit_fire_weapon(v, own, rec_calls(), BUILDING_ID, /*fire_arg=*/0);

        ck_eq((uint32_t)own.unit_at(BUILDING_ID).gun1_magazines, 1u, "T7: magazine consumed, 0x00430a10");
        ck_eq((uint32_t)own.unit_at(BUILDING_ID).gun1_bullets, 8u, "T7: bullets refilled from fx_type.bullets, 0x00430a30");
        ck_eq_d(own.unit_at(BUILDING_ID).weapon_timer, 205.5, "T7: weapon_timer = 201.0 (2nd read) + repeat (4.5), 0x00430a5c");
    }

    // T8: the hovered-enemy scatter target -- a real, different, enemy-owned hovered unit overrides
    // the own-aim-point default with the hovered unit's tile screen position. 0x00430abf-0x00430af0.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t HOVERED = 9;
        tact_unit        &u       = fx.units[BUILDING_ID];
        u.type                    = 1;
        u.facing_dir              = 3;
        g_rec.calc_dir24_return   = 3;
        u.active_gun              = 0;
        u.weapon_timer            = 100.0;
        u.gun1_bullets            = 3;
        u.anim_state              = 0;
        u.aim_x                   = 999; // must NOT be used -- the hovered branch wins
        u.aim_y                   = 999;
        u.owner                   = 1;
        fx.hovered_unit_id        = HOVERED;

        tact_unit &hu = fx.units[HOVERED];
        hu.owner      = 2; // different owner
        hu.pos_col    = 6;
        hu.pos_row    = 7;

        character_type &ct = fx.character_types[1];
        ct.number_gun1     = 5;
        ct.height_gun1     = 1;

        fx_type &wt   = fx.fx_type_table[5];
        wt.speed_fire = 10.0;

        g_rec.time_now_returns = {200.0, 201.0, 300.0};

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::unit_fire_weapon(v, own, rec_calls(), BUILDING_ID, /*fire_arg=*/0);

        // (6<<5)+0x10 = 192+16 = 208; 7*24+12 = 168+12 = 180.
        ck_eq((uint32_t)g_rec.scatter_tx, 208u, "T8: scatter's tx = hovered unit's tile-anchor X, 0x00430abf");
        ck_eq((uint32_t)g_rec.scatter_ty, 180u, "T8: scatter's ty = hovered unit's tile-anchor Y");
        // THE BRANCH DIFFERENCE, pinned from BOTH sides (added 2026-09-04, TACT1-P C5). T6 drives
        // the own-aim branch, which folds the altitude into y2; this one drives the hovered branch,
        // which does NOT (`ADD EAX,[EBP-0x2c]` exists only at 0x00430c29). Without this assertion the
        // two call sites were only ever checked on one side, which is how the body came to unify them
        // and why a POZ3 combat run was the first thing to notice. height_gun1 is 1 here, so a
        // regression that reintroduced the fold would read 181 instead of 180.
        ck_eq((uint32_t)g_rec.fx_spawn_y2, 180u,
              "T8: fx_spawn's y2 = ty + out_dy with NO altitude term (hovered branch), 0x00430b5c");
        ck_eq((uint32_t)g_rec.fx_spawn_altitude, 1u, "T8: altitude still passed as its own argument");
    }

    // T9: kneeling (anim_state 2 or 3) selects kneel_gun1/kneel_gun2 for the fx_spawn altitude
    // instead of height_gun1/height_gun2. Same shape as T6 otherwise. 0x00430b60-0x00430b7e.
    {
        tact_fixture fx;
        g_rec.reset();
        tact_unit &u            = fx.units[BUILDING_ID];
        u.type                  = 1;
        u.facing_dir            = 3;
        g_rec.calc_dir24_return = 3;
        u.active_gun            = 0;
        u.weapon_timer          = 100.0;
        u.gun1_bullets          = 3;
        u.anim_state            = 2; // kneeling
        u.aim_x                 = 1;
        u.aim_y                 = 1;
        fx.hovered_unit_id      = -1;

        character_type &ct = fx.character_types[1];
        ct.number_gun1     = 5;
        ct.height_gun1     = 33; // must NOT be used
        ct.kneel_gun1      = 55;

        fx_type &wt   = fx.fx_type_table[5];
        wt.speed_fire = 10.0;

        g_rec.time_now_returns = {200.0, 201.0, 300.0};

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::unit_fire_weapon(v, own, rec_calls(), BUILDING_ID, /*fire_arg=*/0);

        ck_eq((uint32_t)g_rec.fx_spawn_altitude, 55u, "T9: altitude = kneel_gun1 while kneeling, 0x00430b7e");
    }
}

} // namespace mh::tact::test
