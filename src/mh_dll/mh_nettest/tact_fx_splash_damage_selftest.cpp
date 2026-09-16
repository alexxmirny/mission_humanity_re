#include "tact/tact_fx_splash_damage.h"

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {
using namespace mh::tact;

struct splash_recorder {
    std::vector<std::pair<int32_t, uint8_t>> unit_set_anim_state_args; // (building_id, state)
    std::vector<int32_t>                     unit_refresh_ui_slot_args;
    std::vector<std::pair<int32_t, int32_t>> facing_to_delta_args_out; // (out_dx, out_dy) RETURNED
    int32_t                                  facing_to_delta_dx = 0, facing_to_delta_dy = 0;
    void                                     reset() { *this = splash_recorder{}; }
};
splash_recorder g_rec;

fx_splash_damage_calls rec_calls() {
    return {
        [](int32_t /*facing_dir*/, int32_t *out_dx, int32_t *out_dy) {
            *out_dx = g_rec.facing_to_delta_dx;
            *out_dy = g_rec.facing_to_delta_dy;
        },
        [](int32_t building_id, uint8_t state) {
            g_rec.unit_set_anim_state_args.push_back({building_id, state});
        },
        [](int32_t building_id) { g_rec.unit_refresh_ui_slot_args.push_back(building_id); },
    };
}

constexpr int32_t TARGET = 9;

} // namespace

void run_fx_splash_damage_tests() {
    // T1: NOT lethal (hp > damage) -> hp -= damage, unit_refresh_ui_slot called, NO kill-stamp, NO
    // unit_set_anim_state. Radius 0 so exactly one tile (col,row) is scanned.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t COL = 20, ROW = 30;
        fx.planes().tile_object_at(COL, ROW).building = TARGET;
        tact_unit &tu                                 = fx.units[TARGET];
        tu.hp                                         = 100;
        tu.anim_state                                 = 0;
        tu.owner                                      = 3;

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::fx_splash_damage(v, own, rec_calls(), COL, ROW, /*radius=*/0, /*damage=*/40);

        ck_eq((uint32_t)own.unit_at(TARGET).hp, 60u, "T1: hp -= damage (NOT lethal), 0x0043162a");
        ck_eq((uint32_t)g_rec.unit_refresh_ui_slot_args.size(), 1u,
              "T1: unit_refresh_ui_slot called once, 0x0043163b");
        ck_eq((uint32_t)g_rec.unit_set_anim_state_args.size(), 0u,
              "T1: unit_set_anim_state NOT called (not lethal)");
    }

    // T2: LETHAL (hp <= damage), anim_state<=1 AND progress>0 -> the kill-stamp fires (passable
    // writes) as well as hp=0/progress=0/set_anim_state(0x1f).
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t COL = 5, ROW = 5;
        fx.planes().tile_object_at(COL, ROW).building = TARGET;
        tact_unit &tu                                 = fx.units[TARGET];
        tu.hp                                         = 30;
        tu.anim_state                                 = 0; // <=1
        tu.progress                                   = 7; // >0
        tu.facing_dir                                 = 3;
        g_rec.facing_to_delta_dx                      = 1;
        g_rec.facing_to_delta_dy                      = -1;
        fx.planes().passable_at(COL + 1, ROW - 1)     = mh::state::PASSABLE_BLOCKED; // will flip to DEFAULT
        fx.planes().passable_at(COL, ROW)             = mh::state::PASSABLE_DEFAULT; // will flip to BLOCKED

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::fx_splash_damage(v, own, rec_calls(), COL, ROW, /*radius=*/0, /*damage=*/50);

        ck_eq((uint32_t)own.unit_at(TARGET).hp, 0u, "T2: hp = 0 (lethal), 0x004315e3");
        ck_eq((uint32_t)own.unit_at(TARGET).progress, 0u, "T2: progress = 0 (lethal), 0x004315f3");
        ck_eq((uint32_t)g_rec.unit_set_anim_state_args.size(), 1u,
              "T2: unit_set_anim_state called once, 0x004315fa");
        if (g_rec.unit_set_anim_state_args.size() == 1) {
            ck_eq((uint32_t)g_rec.unit_set_anim_state_args[0].second, 0x1fu,
                  "T2: unit_set_anim_state(target, 0x1f), 0x004315fa");
        }
        ck_eq((uint32_t)fx.planes().passable_at(COL + 1, ROW - 1), (uint32_t)mh::state::PASSABLE_DEFAULT,
              "T2: kill-stamp: passable_at(col+dx,row+dy) = DEFAULT, 0x004315b4-0x004315c5");
        ck_eq((uint32_t)fx.planes().passable_at(COL, ROW), (uint32_t)mh::state::PASSABLE_BLOCKED,
              "T2: kill-stamp: passable_at(col,row) = BLOCKED, 0x004315cc-0x004315d5");
        ck_eq((uint32_t)g_rec.unit_refresh_ui_slot_args.size(), 1u,
              "T2: unit_refresh_ui_slot called once (lethal arm too), 0x00431607");
    }

    // T3: LETHAL but progress==0 -> the kill-stamp is SKIPPED (the two original tests are ANDed);
    // hp/progress/anim_state/refresh still happen.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t COL = 8, ROW = 8;
        fx.planes().tile_object_at(COL, ROW).building = TARGET;
        tact_unit &tu                                 = fx.units[TARGET];
        tu.hp                                         = 10;
        tu.anim_state                                 = 0;
        tu.progress                                   = 0; // == 0 -> no kill-stamp
        fx.planes().passable_at(COL, ROW)             = mh::state::PASSABLE_DEFAULT;

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::fx_splash_damage(v, own, rec_calls(), COL, ROW, /*radius=*/0, /*damage=*/50);

        ck_eq((uint32_t)own.unit_at(TARGET).hp, 0u, "T3: hp = 0 (lethal) even without the stamp");
        ck_eq((uint32_t)fx.planes().passable_at(COL, ROW), (uint32_t)mh::state::PASSABLE_DEFAULT,
              "T3: kill-stamp SKIPPED (progress==0) -- passable at (col,row) untouched");
    }

    // T4: LETHAL but anim_state > 1 -> the kill-stamp is SKIPPED too (both conditions ANDed).
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t COL = 12, ROW = 12;
        fx.planes().tile_object_at(COL, ROW).building = TARGET;
        tact_unit &tu                                 = fx.units[TARGET];
        tu.hp                                         = 10;
        tu.anim_state                                 = 4; // > 1 -> no kill-stamp
        tu.progress                                   = 9;
        fx.planes().passable_at(COL, ROW)             = mh::state::PASSABLE_DEFAULT;

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::fx_splash_damage(v, own, rec_calls(), COL, ROW, /*radius=*/0, /*damage=*/50);

        ck_eq((uint32_t)fx.planes().passable_at(COL, ROW), (uint32_t)mh::state::PASSABLE_DEFAULT,
              "T4: kill-stamp SKIPPED (anim_state>1) -- passable at (col,row) untouched");
    }

    // T5: target == 0 (empty tile) -> skipped entirely, no damage anywhere; and anim_state 2/3
    // (already kneeling/turning) is ALSO a full skip regardless of hp. Both checked at radius 1 so
    // the toroidal wrap loop genuinely runs the 3x3 neighborhood.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t COL = 50, ROW = 50;
        // Leave every tile's .building at its default 0 -- nothing should be touched.
        tact_unit &sentinel = fx.units[TARGET];
        sentinel.hp         = 999;

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::fx_splash_damage(v, own, rec_calls(), COL, ROW, /*radius=*/1, /*damage=*/50);

        ck_eq((uint32_t)own.unit_at(TARGET).hp, 999u, "T5: empty tiles -> no unit touched");
        ck_eq((uint32_t)g_rec.unit_refresh_ui_slot_args.size(), 0u, "T5: no outward calls at all");
    }

    // T6: anim_state == 2 or 3 (mid transition) skips damage entirely, even though hp <= damage.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t COL = 60, ROW = 60;
        fx.planes().tile_object_at(COL, ROW).building = TARGET;
        tact_unit &tu                                 = fx.units[TARGET];
        tu.hp                                         = 5;
        tu.anim_state                                 = 3; // mid-transition -> full skip

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::fx_splash_damage(v, own, rec_calls(), COL, ROW, /*radius=*/0, /*damage=*/999);

        ck_eq((uint32_t)own.unit_at(TARGET).hp, 5u, "T6: anim_state 2/3 -> full skip, hp untouched");
        ck_eq((uint32_t)g_rec.unit_refresh_ui_slot_args.size(), 0u, "T6: no outward calls (full skip)");
    }
}

} // namespace mh::tact::test
