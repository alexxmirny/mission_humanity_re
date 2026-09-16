//
// tact_fx_update_projectile_selftest.cpp -- offline oracle for
//   llm_tact_fx_update_projectile @0x00431654 (libmh/tact/tact_fx_update_projectile.cpp)
//
// WHY OFFLINE, NOT RIG: see tact_fx_update_projectile.h's banner -- this function writes the shared
// `passable` plane on the building-kill path (Map/geometry-owned, cross-mode), so it is not a
// candidate for a blind live shadow-arm. All 8 outward callees are indirected through the file's own
// `fx_update_projectile_calls` table (time/splash/spawn/facing/anim-state/refresh-ui/enqueue/sqrt) so
// this oracle can drive every substep deterministically. `llm_sqrt` and `time_GetCurrentTime` are the
// two whose RETURN VALUE the body consumes, so both are scripted, not just recorded.
//
// The fixture's fx pool + fx_live_count (own.fx_at / own.fx_live_count via tact_store, or the
// fixture's own `fx_pool`/`fx_live_count` fields directly -- same memory, see tact_test_support.h) are
// the primary state under test; `own.planes()` (tile_objects/passable) and `own.unit_at()` are the two
// secondary write surfaces this function touches (the building-kill and building-destroy branches).
//
#include "tact/tact_fx_update_projectile.h"

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {
using namespace mh::tact;

// ---- outward-call recorders --------------------------------------------------------------------

struct spawn_call {
    int32_t fx_type, owner, x, y, x2, y2, altitude;
};
struct splash_call {
    int32_t col, row, radius, damage;
};
struct facing_call {
    int32_t facing_dir;
};
struct anim_state_call {
    int32_t building_id;
    uint8_t state;
};
struct enqueue_call {
    int32_t  unit_id, op;
    uint8_t  interrupt_flag;
    int32_t  arg0;
    uint16_t arg1, arg2, arg3;
};

struct fx_recorder {
    // time_get_current_time: a scripted sequence, HELD AT THE LAST VALUE once exhausted -- a case
    // that only needs a constant clock (the common case: one substep per call) supplies a
    // single-element script and every subsequent gate check keeps reading it.
    std::vector<double> clock_script;
    size_t              clock_next = 0;
    int32_t             time_calls = 0;

    int32_t                  splash_calls = 0;
    std::vector<splash_call> splash_args;

    std::vector<spawn_call> spawn_args;
    int32_t                 spawn_return = 0;

    std::vector<facing_call> facing_args;
    int32_t                  facing_out_dx = 0, facing_out_dy = 0; // scripted (dx,dy) OUTPUT

    std::vector<anim_state_call> anim_state_args;
    std::vector<int32_t>         refresh_ui_args;

    std::vector<enqueue_call> enqueue_args;
    int32_t                   enqueue_return = 0;

    std::vector<double> sqrt_args;
    double              sqrt_return = 0.0;

    void reset() { *this = fx_recorder{}; }
};
fx_recorder g_rec;

const fx_update_projectile_calls &rec_calls() {
    static const fx_update_projectile_calls c = {
        []() -> double {
            g_rec.time_calls++;
            if (g_rec.clock_script.empty()) return 0.0;
            const size_t i =
                g_rec.clock_next < g_rec.clock_script.size() ? g_rec.clock_next
                                                             : g_rec.clock_script.size() - 1;
            if (g_rec.clock_next + 1 < g_rec.clock_script.size()) g_rec.clock_next++;
            return g_rec.clock_script[i];
        },
        [](int32_t col, int32_t row, int32_t radius_tiles, int32_t damage) {
            g_rec.splash_calls++;
            g_rec.splash_args.push_back({col, row, radius_tiles, damage});
        },
        [](int32_t fx_type, uint8_t owner, int32_t x, int32_t y, int32_t x2, int32_t y2,
           uint8_t altitude) -> int32_t {
            g_rec.spawn_args.push_back(
                {fx_type, (int32_t)owner, x, y, x2, y2, (int32_t)altitude});
            return g_rec.spawn_return;
        },
        [](int32_t facing_dir, int32_t *out_dx, int32_t *out_dy) {
            g_rec.facing_args.push_back({facing_dir});
            *out_dx = g_rec.facing_out_dx;
            *out_dy = g_rec.facing_out_dy;
        },
        [](int32_t building_id, uint8_t state) {
            g_rec.anim_state_args.push_back({building_id, state});
        },
        [](int32_t building_id) { g_rec.refresh_ui_args.push_back(building_id); },
        [](int32_t unit_id, int32_t op, uint8_t interrupt_flag, int32_t arg0, uint16_t arg1,
           uint16_t arg2, uint16_t arg3) -> int32_t {
            g_rec.enqueue_args.push_back({unit_id, op, interrupt_flag, arg0, arg1, arg2, arg3});
            return g_rec.enqueue_return;
        },
        [](double x) -> double {
            g_rec.sqrt_args.push_back(x);
            return g_rec.sqrt_return;
        },
    };
    return c;
}

// A representative fx_type_table entry, zeroed then overridden per case. speed=1.0 combined with a
// {2.0} clock_script and move_clock=0.0 always yields exactly ONE substep per call (gate: 0.0+1.0=1.0,
// 2.0<=1.0 is false so the substep runs; next iteration's gate is 1.0+1.0=2.0, 2.0<=2.0 is true so it
// returns) -- the shared default for every case below that doesn't itself need multiple substeps.
void seed_fx_type(mh::tact::fx_type &t) {
    t            = mh::tact::fx_type{};
    t.speed      = 1.0;
    t.frames     = 8;
    t.direct     = 0;
    t.power      = 1;
    t.colision1  = 0;
    t.colision2  = 0;
    t.range_max  = 0;
    t.range_min  = 0;
    t.sound      = 0;
    t.range_kill = 0;
}

} // namespace

void run_fx_update_projectile_tests() {
    // T1: the catch-up substep loop's shape (@0x00431687-0x00431d7e, the `for (;;)`) -- ONE external
    // call with the clock parked far enough ahead of move_clock to demand THREE substeps' worth of
    // catch-up if uninterrupted, but a building placed so it is hit only on the SECOND substep's
    // candidate tile (col 2), not the first (col 1, left empty). A closed-form implementation that
    // solved for the full requested delta and tested collision once at the FINAL position (col 3,
    // where there is no building) would never see the kill at all and would leave the slot alive,
    // moved the full distance. The real per-substep loop must test at EVERY intermediate tile: it
    // kills on substep 2 and never reaches substep 3, so fx.pos_x is left frozen at substep 1's
    // result (32.0, not 64.0 or 96.0) because the kill branch returns before the normal-move write at
    // 0x00431b3e ever runs a second time.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t FX_IDX = 5, TYPE_ID = 3, BUILDING_ID = 7;
        fx_entry         &e = fx.fx_pool[FX_IDX];
        e                   = fx_entry{};
        e.fx_type           = TYPE_ID; // < 0x20 -> moving projectile
        e.owner             = 9;
        e.pos_x             = 0.0;
        e.vel_x             = 32.0; // exactly one tile column per substep
        e.pos_y             = 96.0; // row 4 (96/24); vel_y=0 keeps the row fixed
        e.vel_y             = 0.0;
        e.altitude          = 0x1e;
        e.move_clock        = 0.0;
        fx.fx_live_count    = 61;

        seed_fx_type(fx.fx_type_table[TYPE_ID]);
        fx.fx_type_table[TYPE_ID].speed     = 1.0;
        fx.fx_type_table[TYPE_ID].colision2 = 0x42;
        fx.fx_type_table[TYPE_ID].power     = 30;

        // A building at tile (2,4) -- the SECOND substep's candidate tile only; tile (1,4), the
        // first substep's candidate tile, is left empty (building defaults to 0).
        fx.planes().tile_object_at(2, 4).building = BUILDING_ID;
        tact_unit &bld                            = fx.units[BUILDING_ID];
        bld                                       = tact_unit{};
        bld.owner                                 = 1; // != e.owner
        bld.anim_state                            = 0; // not 2/3 (kneel), not 0x1f (already dying)
        bld.hp                                    = 200;
        bld.def_stat                              = 0; // damage-only branch, no enqueue tail

        g_rec.clock_script = {3.5}; // 3 substeps' worth of catch-up if never interrupted

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::fx_update_projectile(tv, own, FX_IDX, rec_calls());

        ck_eq((uint32_t)g_rec.time_calls, 2u,
              "T1: exactly 2 gate checks ran (not 4) -- the loop stopped the instant substep 2's "
              "building kill returned, 0x004316a3 gate x2");
        ck_eq((uint32_t)e.fx_type, 0u, "T1: killed on substep 2's building hit, 0x004319e7");
        ck_eq_d(e.pos_x, 32.0,
                "T1: pos_x frozen at substep 1's own result, not a closed-form jump to 64 or 96 -- "
                "the kill returns before 0x00431b3e writes it a second time");
        ck_eq_d(e.pos_y, 96.0, "T1: pos_y unchanged (vel_y=0 all along)");
        ck_eq((uint32_t)fx.fx_live_count, 60u, "T1: live_count decremented exactly once, 0x004319ee");
        ck_eq((uint32_t)g_rec.spawn_args.size(), 1u, "T1: fx_spawn called exactly once");
        if (g_rec.spawn_args.size() == 1) {
            const auto &s = g_rec.spawn_args[0];
            ck_eq((uint32_t)s.fx_type, 0x42u, "T1: fx_spawn fx_type == colision2, 0x004319d7");
            ck_eq((uint32_t)s.x, 64u,
                  "T1: fx_spawn x == substep 2's OWN moved_x (32+32), not substep 1's (32) or a "
                  "3-substep closed-form (96), 0x004319ae");
            ck_eq((uint32_t)s.y, 96u, "T1: fx_spawn y == moved_y (96+0)");
        }
        ck_eq((uint32_t)bld.hp, 170u, "T1: building took damage-only (200 - power 30), 0x00431aba-d4");
        ck_eq((uint32_t)g_rec.refresh_ui_args.size(), 1u, "T1: unit_refresh_ui_slot called once");
        ck_eq((uint32_t)g_rec.anim_state_args.size(), 0u,
              "T1: damage-only path never calls unit_set_anim_state (that's the destroy branch)");
        ck_eq((uint32_t)g_rec.enqueue_args.size(), 0u, "T1: def_stat==0 skips the enqueue tail");
    }

    // T2: all FIVE kill sites. Each sub-case is its own single-substep fixture.

    // T2a: out-of-bounds kill, 0x0043175c-0x00431767 (JMP straight to it, no splash/spawn).
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t FX_IDX = 5, TYPE_ID = 1;
        fx_entry         &e = fx.fx_pool[FX_IDX];
        e                   = fx_entry{};
        e.fx_type           = TYPE_ID;
        e.pos_x             = 0.0;
        e.vel_x             = 100000.0; // candidate_x = 100000/32 = 3125, way >= grid_width (128)
        e.pos_y             = 0.0;
        e.vel_y             = 0.0;
        e.move_clock        = 0.0;
        fx.fx_live_count    = 12;

        seed_fx_type(fx.fx_type_table[TYPE_ID]);
        g_rec.clock_script = {2.0};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::fx_update_projectile(tv, own, FX_IDX, rec_calls());

        ck_eq((uint32_t)g_rec.time_calls, 1u, "T2a: exactly one substep");
        ck_eq((uint32_t)e.fx_type, 0u, "T2a: out-of-bounds kill, 0x0043175c-0x00431760");
        ck_eq((uint32_t)fx.fx_live_count, 11u, "T2a: live_count decremented, 0x00431767");
        ck_eq((uint32_t)g_rec.spawn_args.size(), 0u, "T2a: no fx_spawn on the OOB path");
        ck_eq((uint32_t)g_rec.splash_calls, 0u, "T2a: no splash on the OOB path");
    }

    // T2b: terrain-impact kill, 0x0043187c-0x00431883 (splash + spawn at the pulled-back position).
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t FX_IDX = 5, TYPE_ID = 2;
        fx_entry         &e = fx.fx_pool[FX_IDX];
        e                   = fx_entry{};
        e.fx_type           = TYPE_ID;
        e.owner             = 8;
        e.pos_x             = 64.0;
        e.vel_x             = 0.0; // candidate_x = flag_probe_x = 2 (col 2)
        e.pos_y             = 72.0;
        e.vel_y             = 0.0; // candidate_y = flag_probe_y = 3 (row 3)
        e.altitude          = 0x14;
        e.move_clock        = 0.0;
        fx.fx_live_count    = 33;

        seed_fx_type(fx.fx_type_table[TYPE_ID]);
        fx.fx_type_table[TYPE_ID].colision1  = 0x33;
        fx.fx_type_table[TYPE_ID].range_kill = 7;
        fx.fx_type_table[TYPE_ID].power      = 11;

        fx.planes().tile_object_at(2, 3).flags[1] |= 0x20; // both probes land on this one tile
        // passable_at(2,3) stays PASSABLE_BLOCKED (0, the fixture default) and building stays 0 --
        // both required alongside the flag bit for the terrain-impact branch to fire.
        g_rec.clock_script = {2.0};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::fx_update_projectile(tv, own, FX_IDX, rec_calls());

        ck_eq((uint32_t)g_rec.time_calls, 1u, "T2b: exactly one substep");
        ck_eq((uint32_t)e.fx_type, 0u, "T2b: terrain-impact kill, 0x0043187c");
        ck_eq((uint32_t)fx.fx_live_count, 32u, "T2b: live_count decremented, 0x00431883");
        ck_eq((uint32_t)g_rec.splash_calls, 1u, "T2b: splash fires (range_kill != 0), 0x004318b1");
        if (g_rec.splash_calls == 1) {
            const auto &sp = g_rec.splash_args[0];
            ck_eq((uint32_t)sp.col, 2u, "T2b: splash col == pullback_tile_x");
            ck_eq((uint32_t)sp.row, 3u, "T2b: splash row == pullback_tile_y");
            ck_eq((uint32_t)sp.radius, 7u, "T2b: splash radius == range_kill");
            ck_eq((uint32_t)sp.damage, 11u, "T2b: splash damage == power");
        }
        ck_eq((uint32_t)g_rec.spawn_args.size(), 1u, "T2b: fx_spawn called exactly once");
        if (g_rec.spawn_args.size() == 1) {
            const auto &s = g_rec.spawn_args[0];
            ck_eq((uint32_t)s.fx_type, 0x33u, "T2b: fx_spawn fx_type == colision1, 0x004318df");
            ck_eq((uint32_t)s.x, 64u, "T2b: fx_spawn x == pullback_x (pos_x - vel_x)");
            ck_eq((uint32_t)s.y, 72u, "T2b: fx_spawn y == pullback_y");
            ck_eq((uint32_t)s.x2, 64u, "T2b: fx_spawn x2 duplicates x, 0x004318b6-e6");
            ck_eq((uint32_t)s.y2, 72u, "T2b: fx_spawn y2 duplicates y");
        }
    }

    // T2c: building/unit collision kill, 0x004319e7-0x004319ee.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t FX_IDX = 5, TYPE_ID = 4, BUILDING_ID = 9;
        fx_entry         &e = fx.fx_pool[FX_IDX];
        e                   = fx_entry{};
        e.fx_type           = TYPE_ID;
        e.owner             = 2;
        e.pos_x             = 0.0;
        e.vel_x             = 32.0; // candidate_x = 1
        e.pos_y             = 96.0;
        e.vel_y             = 0.0; // candidate_y = 4
        e.altitude          = 5;
        e.move_clock        = 0.0;
        fx.fx_live_count    = 20;

        seed_fx_type(fx.fx_type_table[TYPE_ID]);
        fx.fx_type_table[TYPE_ID].colision2 = 0x55;
        fx.fx_type_table[TYPE_ID].power     = 50;

        fx.planes().tile_object_at(1, 4).building = BUILDING_ID;
        tact_unit &bld                            = fx.units[BUILDING_ID];
        bld                                       = tact_unit{};
        bld.owner                                 = 5; // != e.owner
        bld.anim_state                            = 0;
        bld.hp                                    = 300;
        bld.def_stat                              = 0; // damage-only, no enqueue tail

        g_rec.clock_script = {2.0};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::fx_update_projectile(tv, own, FX_IDX, rec_calls());

        ck_eq((uint32_t)e.fx_type, 0u, "T2c: building/unit collision kill, 0x004319e7");
        ck_eq((uint32_t)fx.fx_live_count, 19u, "T2c: live_count decremented, 0x004319ee");
        ck_eq((uint32_t)g_rec.spawn_args.size(), 1u, "T2c: fx_spawn called exactly once");
        if (g_rec.spawn_args.size() == 1) {
            const auto &s = g_rec.spawn_args[0];
            ck_eq((uint32_t)s.fx_type, 0x55u, "T2c: fx_spawn fx_type == colision2");
            ck_eq((uint32_t)s.x, 32u, "T2c: fx_spawn x == moved_x, 0x004319ae");
            ck_eq((uint32_t)s.y, 96u, "T2c: fx_spawn y == moved_y");
        }
        ck_eq((uint32_t)bld.hp, 250u, "T2c: building took damage-only (300 - power 50), 0x00431aba-d4");
        ck_eq((uint32_t)g_rec.refresh_ui_args.size(), 1u, "T2c: unit_refresh_ui_slot called once");
        ck_eq((uint32_t)g_rec.anim_state_args.size(), 0u, "T2c: not the destroy branch");
        ck_eq((uint32_t)g_rec.facing_args.size(), 0u, "T2c: not the destroy branch");
    }

    // T2d: the range_max/sqrt kill, 0x00431c55-0x00431c60 -- ASYMMETRIC: fx_type is zeroed but
    // fx_live_count is deliberately left UNCHANGED (unlike every other kill site in this function).
    // live_count is seeded to a distinct sentinel so BOTH a spurious decrement and a spurious
    // increment would be caught.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t FX_IDX = 5, TYPE_ID = 6;
        fx_entry         &e = fx.fx_pool[FX_IDX];
        e                   = fx_entry{};
        e.fx_type           = TYPE_ID;
        e.pos_x             = 0.0;
        e.vel_x             = 3.0; // 3-4-5 triangle: travel_dx=3, travel_dy=4, dist=5
        e.pos_y             = 0.0;
        e.vel_y             = 4.0;
        e.travel_dx         = 0.0;
        e.travel_dy         = 0.0;
        e.frame_counter     = 500;  // sentinel -- must survive (kill returns before 0x00431c65's INC)
        e.sprite_frame      = 6000; // sentinel -- must survive
        e.move_clock        = 0.0;
        fx.fx_live_count    = 77; // sentinel -- must survive UNCHANGED

        seed_fx_type(fx.fx_type_table[TYPE_ID]);
        fx.fx_type_table[TYPE_ID].range_max = 5;

        g_rec.clock_script = {2.0};
        g_rec.sqrt_return  = 5.0; // sqrt(travel_dx^2+travel_dy^2) = sqrt(25) = 5

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::fx_update_projectile(tv, own, FX_IDX, rec_calls());

        ck_eq((uint32_t)g_rec.sqrt_args.size(), 1u, "T2d: sqrt called exactly once, 0x00431c39");
        if (g_rec.sqrt_args.size() == 1) {
            ck_eq_d(g_rec.sqrt_args[0], 25.0,
                    "T2d: sqrt argument == travel_dx^2+travel_dy^2 (3^2+4^2), 0x00431c11-31");
        }
        ck_eq_d(e.pos_x, 3.0, "T2d: normal move ran first (pos_x += vel_x), 0x00431b3e");
        ck_eq_d(e.pos_y, 4.0, "T2d: normal move ran first (pos_y += vel_y), 0x00431b58");
        ck_eq_d(e.travel_dx, 3.0, "T2d: travel_dx += vel_x, 0x00431b72-86");
        ck_eq_d(e.travel_dy, 4.0, "T2d: travel_dy += vel_y, 0x00431b8c-a0");
        ck_eq((uint32_t)e.tile_col, 0u, "T2d: tile_col = trunc(pos_x/32) = trunc(0.09375) = 0");
        ck_eq((uint32_t)e.tile_row, 0u, "T2d: tile_row = trunc(pos_y/24) = trunc(0.1666) = 0");
        ck_eq((uint32_t)e.fx_type, 0u, "T2d: range_max reached -> killed, 0x00431c59");
        ck_eq((uint32_t)fx.fx_live_count, 77u,
              "T2d: live_count UNCHANGED -- the deliberate asymmetry against every other kill site "
              "in this function (no DEC anywhere near 0x00431c55-60)");
        ck_eq((uint32_t)(uint16_t)e.frame_counter, 500u,
              "T2d: frame_counter untouched -- the kill returns before 0x00431c65's INC");
        ck_eq((uint32_t)e.sprite_frame, 6000u, "T2d: sprite_frame untouched");
    }

    // T2e: pure-animation frame-overflow kill, 0x00431d1a-0x00431d21 -- kills but does NOT return
    // (JMP 0x00431d7e loops back to the top instead of JMP 0x00431d83). Pinned via time_calls: the
    // gate is checked a SECOND time (and stops there, since the clock isn't advanced further),
    // proving the loop really did continue after the kill rather than returning immediately.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t FX_IDX = 5, TYPE_ID = 0x25; // >= 0x20 -> pure animation effect
        fx_entry         &e = fx.fx_pool[FX_IDX];
        e                   = fx_entry{};
        e.fx_type           = TYPE_ID;
        e.frame_counter     = 2;    // -> 3 after the INC, == frames -> kill branch
        e.sprite_frame      = 7777; // sentinel -- must survive (kill branch skips the sprite write)
        e.move_clock        = 0.0;
        fx.fx_live_count    = 8;

        seed_fx_type(fx.fx_type_table[TYPE_ID]);
        fx.fx_type_table[TYPE_ID].frames = 3;
        fx.fx_type_table[TYPE_ID].direct = 0;

        g_rec.clock_script = {2.0}; // enough for iteration 1's gate to pass, not iteration 2's

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::fx_update_projectile(tv, own, FX_IDX, rec_calls());

        ck_eq((uint32_t)g_rec.time_calls, 2u,
              "T2e: the gate was checked TWICE -- the kill at 0x00431d1a looped back "
              "(JMP 0x00431d7e) instead of returning, and the second gate check is what stopped it");
        ck_eq((uint32_t)e.fx_type, 0u, "T2e: frame-overflow kill, 0x00431d1a");
        ck_eq((uint32_t)fx.fx_live_count, 7u, "T2e: live_count decremented, 0x00431d21");
        ck_eq((uint32_t)(uint16_t)e.frame_counter, 3u, "T2e: frame_counter incremented once, 0x00431cf3-f7");
        ck_eq((uint32_t)e.sprite_frame, 7777u, "T2e: sprite_frame untouched by the kill branch");
    }

    // T3: kill sites write ONLY fx_type (+ live_count where applicable) -- every OTHER fx_entry
    // field is seeded with a distinct sentinel and asserted to survive the out-of-bounds kill
    // (0x0043175c-0x00431767) untouched. move_clock is the one exception: it is expected to advance
    // to move_clock+speed, since that write (0x004316af-c7) happens unconditionally at the top of
    // EVERY substep, before any branch -- it is not part of the kill site's own write set.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t FX_IDX = 5, TYPE_ID = 9;
        fx_entry         &e = fx.fx_pool[FX_IDX];
        e                   = fx_entry{};
        e.fx_type           = TYPE_ID;
        e.owner             = 0x4c;
        e.pos_x             = 1000000.0;
        e.vel_x             = 1.0; // candidate_x way >= grid_width -> OOB kill
        e.pos_y             = 555.25;
        e.vel_y             = 2.25;
        e.travel_dx         = 11.25;
        e.travel_dy         = 22.75;
        e.tile_col          = 0x33;
        e.tile_row          = 0x44;
        e.dir24             = 7;
        e.altitude          = 9;
        e.move_clock        = 0.0;
        e.frame_counter     = 0x1234;
        e.sprite_frame      = 0xabcd;
        e.target_x          = 33.5;
        e.target_y          = 44.5;
        fx.fx_live_count    = 500;

        seed_fx_type(fx.fx_type_table[TYPE_ID]);
        g_rec.clock_script = {2.0};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::fx_update_projectile(tv, own, FX_IDX, rec_calls());

        ck_eq((uint32_t)e.fx_type, 0u, "T3: OOB kill, 0x0043175c");
        ck_eq((uint32_t)fx.fx_live_count, 499u, "T3: live_count decremented, 0x00431767");
        ck_eq_d(e.move_clock, 1.0,
                "T3: move_clock DOES advance -- that write is the loop's own gate step, not part of "
                "the kill site, 0x004316af-c7");

        ck_eq_d(e.pos_x, 1000000.0, "T3: pos_x survives -- kill sites write only fx_type(+count)");
        ck_eq_d(e.pos_y, 555.25, "T3: pos_y survives");
        ck_eq_d(e.vel_x, 1.0, "T3: vel_x survives (never written anywhere in this function)");
        ck_eq_d(e.vel_y, 2.25, "T3: vel_y survives");
        ck_eq_d(e.travel_dx, 11.25, "T3: travel_dx survives");
        ck_eq_d(e.travel_dy, 22.75, "T3: travel_dy survives");
        ck_eq((uint32_t)e.tile_col, 0x33u, "T3: tile_col survives");
        ck_eq((uint32_t)e.tile_row, 0x44u, "T3: tile_row survives");
        ck_eq((uint32_t)e.dir24, 7u, "T3: dir24 survives");
        ck_eq((uint32_t)e.altitude, 9u, "T3: altitude survives");
        ck_eq((uint32_t)e.owner, 0x4cu, "T3: owner survives");
        ck_eq((uint32_t)(uint16_t)e.frame_counter, 0x1234u, "T3: frame_counter survives");
        ck_eq((uint32_t)e.sprite_frame, 0xabcdu, "T3: sprite_frame survives");
        ck_eq_d(e.target_x, 33.5, "T3: target_x survives");
        ck_eq_d(e.target_y, 44.5, "T3: target_y survives");

        ck_eq((uint32_t)g_rec.spawn_args.size(), 0u, "T3: no outward calls on the OOB path");
        ck_eq((uint32_t)g_rec.splash_calls, 0u, "T3: no outward calls on the OOB path");
        ck_eq((uint32_t)g_rec.facing_args.size(), 0u, "T3: no outward calls on the OOB path");
        ck_eq((uint32_t)g_rec.anim_state_args.size(), 0u, "T3: no outward calls on the OOB path");
        ck_eq((uint32_t)g_rec.refresh_ui_args.size(), 0u, "T3: no outward calls on the OOB path");
        ck_eq((uint32_t)g_rec.enqueue_args.size(), 0u, "T3: no outward calls on the OOB path");
        ck_eq((uint32_t)g_rec.sqrt_args.size(), 0u, "T3: no outward calls on the OOB path");
    }

    // T4: the passable-plane write, 0x00431a6b (neighbour -> PASSABLE_DEFAULT) and 0x00431a7b
    // (candidate -> PASSABLE_BLOCKED) -- reached only in the building-DESTROY sub-branch
    // (bld.hp <= kind.power && bld.anim_state < 2 && bld.progress != 0). Seeds the candidate cell,
    // the neighbour cell (facing_to_delta's dx/dy offset), and an UNRELATED third cell as a
    // sentinel, and asserts exactly the first two changed to exactly the right values.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t FX_IDX = 5, TYPE_ID = 8, BUILDING_ID = 12;
        fx_entry         &e = fx.fx_pool[FX_IDX];
        e                   = fx_entry{};
        e.fx_type           = TYPE_ID;
        e.owner             = 1;
        e.pos_x             = 0.0;
        e.vel_x             = 320.0; // candidate_x = 10
        e.pos_y             = 0.0;
        e.vel_y             = 144.0; // candidate_y = 6
        e.altitude          = 5;
        e.move_clock        = 0.0;
        fx.fx_live_count    = 20;

        seed_fx_type(fx.fx_type_table[TYPE_ID]);
        fx.fx_type_table[TYPE_ID].colision2 = 0x66;
        fx.fx_type_table[TYPE_ID].power     = 50;

        fx.planes().tile_object_at(10, 6).building = BUILDING_ID;
        tact_unit &bld                             = fx.units[BUILDING_ID];
        bld                                        = tact_unit{};
        bld.owner                                  = 2; // != e.owner
        bld.anim_state                             = 1; // < 2 -> eligible for the passable-opening sub-branch
        bld.progress                               = 5; // != 0 -> eligible
        bld.hp                                     = 10;
        bld.facing_dir                             = 15;
        bld.def_stat                               = 0;

        // Candidate cell (10,6): sentinel 9, must become PASSABLE_BLOCKED (0).
        fx.planes().passable_at(10, 6) = 9;
        // Neighbour cell in the facing_to_delta direction, (10+1, 6+0): sentinel 7, must become
        // PASSABLE_DEFAULT (2).
        fx.planes().passable_at(11, 6) = 7;
        // An unrelated third cell: sentinel 5, must survive untouched.
        fx.planes().passable_at(9, 6) = 5;

        g_rec.facing_out_dx = 1;
        g_rec.facing_out_dy = 0;
        g_rec.clock_script  = {2.0};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::fx_update_projectile(tv, own, FX_IDX, rec_calls());

        ck_eq((uint32_t)g_rec.facing_args.size(), 1u, "T4: facing_to_delta called once, 0x00431a55");
        if (g_rec.facing_args.size() == 1) {
            ck_eq((uint32_t)g_rec.facing_args[0].facing_dir, 15u,
                  "T4: facing_to_delta(bld.facing_dir, ...)");
        }
        ck_eq((uint32_t)fx.planes().passable_at(11, 6), (uint32_t)mh::state::PASSABLE_DEFAULT,
              "T4: neighbour cell (candidate + dx,dy) -> PASSABLE_DEFAULT, 0x00431a6b");
        ck_eq((uint32_t)fx.planes().passable_at(10, 6), (uint32_t)mh::state::PASSABLE_BLOCKED,
              "T4: candidate cell -> PASSABLE_BLOCKED, 0x00431a7b");
        ck_eq((uint32_t)fx.planes().passable_at(9, 6), 5u,
              "T4: an unrelated third cell is untouched by the write");

        ck_eq((uint32_t)bld.hp, 0u, "T4: building destroyed, hp = 0, 0x00431a89");
        ck_eq((uint32_t)bld.progress, 0u, "T4: progress = 0, 0x00431a99");
        ck_eq((uint32_t)g_rec.anim_state_args.size(), 1u, "T4: unit_set_anim_state called once");
        if (g_rec.anim_state_args.size() == 1) {
            ck_eq((uint32_t)g_rec.anim_state_args[0].building_id, (uint32_t)BUILDING_ID,
                  "T4: unit_set_anim_state(building_id, ...)");
            ck_eq((uint32_t)g_rec.anim_state_args[0].state, 0x1fu,
                  "T4: unit_set_anim_state(..., 0x1f), 0x00431aa0-ad");
        }
        ck_eq((uint32_t)g_rec.refresh_ui_args.size(), 1u, "T4: unit_refresh_ui_slot called once");
        ck_eq((uint32_t)e.fx_type, 0u, "T4: the fx itself was killed too, 0x004319e7");
        ck_eq((uint32_t)fx.fx_live_count, 19u, "T4: live_count decremented once, 0x004319ee");
    }

    // T5: the four read-only double constants (0.03125 = 1/32, 24.0) -- each sub-case is built so a
    // common typo (1/16 instead of 1/32, or a wrong divisor like 12 instead of 24) flips an exact,
    // directly observable result.

    // T5a: kFineToTileColScale2 (0x00500494, @0x00431ba6-cb) and kFineToTileRowDivisor2 (0x0050049c,
    // @0x00431bd1-f6) -- both written straight to the OBSERVABLE fx.tile_col/tile_row fields.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t FX_IDX = 5, TYPE_ID = 10;
        fx_entry         &e = fx.fx_pool[FX_IDX];
        e                   = fx_entry{};
        e.fx_type           = TYPE_ID;
        e.pos_x             = 16.0;
        e.vel_x             = 32.0; // post-move pos_x = 48.0
        e.pos_y             = 0.0;
        e.vel_y             = 48.0; // post-move pos_y = 48.0
        e.move_clock        = 0.0;
        fx.fx_live_count    = 4;

        seed_fx_type(fx.fx_type_table[TYPE_ID]); // range_max = 0, skips the range-kill branch
        g_rec.clock_script = {2.0};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::fx_update_projectile(tv, own, FX_IDX, rec_calls());

        ck_eq((uint32_t)e.fx_type, (uint32_t)TYPE_ID,
              "T5a: sanity -- not killed, reached the normal-move path");
        ck_eq((uint32_t)e.tile_col, 1u,
              "T5a: tile_col = trunc(48.0 * 1/32) = trunc(1.5) = 1 -- a 1/16 typo gives trunc(3.0)=3, "
              "0x00431ba6-cb (_G_LLM_CONST_DBL_1DIV32_500494)");
        ck_eq((uint32_t)e.tile_row, 2u,
              "T5a: tile_row = trunc(48.0 / 24.0) = trunc(2.0) = 2 -- a /12 typo gives trunc(4.0)=4, "
              "0x00431bd1-f6 (_G_LLM_CONST_DBL_24_50049C)");
    }

    // T5b: kFineToTileColScale1 (0x00500484, @0x00431772-87) -- the FIRST, independent flag_probe_x,
    // computed from the CURRENT (pre-step) pos_x, not the post-step candidate. Pinned via whether the
    // terrain-impact kill fires at all: a 1/16 typo makes flag_probe_x land on a tile with no flag
    // bit set, so the kill would never trigger and fx.fx_type would stay nonzero.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t FX_IDX = 5, TYPE_ID = 11;
        fx_entry         &e = fx.fx_pool[FX_IDX];
        e                   = fx_entry{};
        e.fx_type           = TYPE_ID;
        e.owner             = 3;
        e.pos_x             = 16.0;
        e.vel_x             = 16.0; // candidate_x = 1; flag_probe_x = trunc(16.0/32) = 0
        e.pos_y             = 0.0;
        e.vel_y             = 0.0; // candidate_y = flag_probe_y = 0
        e.altitude          = 8;
        e.move_clock        = 0.0;
        fx.fx_live_count    = 6;

        seed_fx_type(fx.fx_type_table[TYPE_ID]);
        fx.fx_type_table[TYPE_ID].colision1 = 0x77;

        // Correct flag_probe_x is 0 -> the (flag_probe_x, candidate_y) probe checks tile (0,0).
        // Tile (1,0) -- what a 1/16 typo (flag_probe_x=1) OR the (candidate_x, flag_probe_y) probe
        // would check -- is deliberately left flag-clear.
        fx.planes().tile_object_at(0, 0).flags[1] |= 0x20;
        g_rec.clock_script = {2.0};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::fx_update_projectile(tv, own, FX_IDX, rec_calls());

        ck_eq((uint32_t)e.fx_type, 0u,
              "T5b: terrain kill fired, proving flag_probe_x was computed as 0 (correct 1/32 "
              "scale) -- a 1/16 typo would give 1, miss the flag, and leave this nonzero, "
              "0x00431772-87 / 0x004317ca-e8");
        ck_eq((uint32_t)fx.fx_live_count, 5u, "T5b: live_count decremented, 0x00431883");
        ck_eq((uint32_t)g_rec.spawn_args.size(), 1u, "T5b: fx_spawn called once");
        if (g_rec.spawn_args.size() == 1) {
            const auto &s = g_rec.spawn_args[0];
            ck_eq((uint32_t)s.x, 0u, "T5b: pullback_x = trunc(16-16) = 0");
            ck_eq((uint32_t)s.y, 0u, "T5b: pullback_y = trunc(0-0) = 0");
        }
    }

    // T5c: kFineToTileRowDivisor1 (0x0050048c, @0x0043178a-9f) -- the row counterpart of T5b, same
    // mechanism, discriminating the divisor instead of the column scale.
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t FX_IDX = 5, TYPE_ID = 12;
        fx_entry         &e = fx.fx_pool[FX_IDX];
        e                   = fx_entry{};
        e.fx_type           = TYPE_ID;
        e.owner             = 4;
        e.pos_x             = 0.0;
        e.vel_x             = 0.0; // candidate_x = flag_probe_x = 0
        e.pos_y             = 12.0;
        e.vel_y             = 12.0; // candidate_y = 1; flag_probe_y = trunc(12.0/24) = 0
        e.altitude          = 6;
        e.move_clock        = 0.0;
        fx.fx_live_count    = 6;

        seed_fx_type(fx.fx_type_table[TYPE_ID]);
        fx.fx_type_table[TYPE_ID].colision1 = 0x88;

        // Correct flag_probe_y is 0 -> the (candidate_x, flag_probe_y) probe checks tile (0,0).
        // Tile (0,1) -- what a /12 typo (flag_probe_y=1) OR the (flag_probe_x, candidate_y) probe
        // would check -- is deliberately left flag-clear.
        fx.planes().tile_object_at(0, 0).flags[1] |= 0x20;
        g_rec.clock_script = {2.0};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::fx_update_projectile(tv, own, FX_IDX, rec_calls());

        ck_eq((uint32_t)e.fx_type, 0u,
              "T5c: terrain kill fired, proving flag_probe_y was computed as 0 (correct /24 "
              "divisor) -- a /12 typo would give 1, miss the flag, and leave this nonzero, "
              "0x0043178a-9f / 0x004317ca-e8");
        ck_eq((uint32_t)fx.fx_live_count, 5u, "T5c: live_count decremented, 0x00431883");
        ck_eq((uint32_t)g_rec.spawn_args.size(), 1u, "T5c: fx_spawn called once");
        if (g_rec.spawn_args.size() == 1) {
            const auto &s = g_rec.spawn_args[0];
            ck_eq((uint32_t)s.x, 0u, "T5c: pullback_x = trunc(0-0) = 0");
            ck_eq((uint32_t)s.y, 0u, "T5c: pullback_y = trunc(12-12) = 0");
        }
    }

    // T6: the inline x87 trunc reproduction -- a NEGATIVE fractional operand where trunc-toward-zero
    // (what the original's FPU control word actually does: RC=11 truncate) differs from floor.
    // pos_x - vel_x = 0.1 - 1.6 = -1.5: trunc gives -1, floor gives -2. Chosen so the SUM (moved_x,
    // used for the in-bounds candidate) stays positive/in-range while the DIFFERENCE (pullback_x,
    // trunc_sub @0x00431819-32) is the negative fractional value under test, and the result is
    // observable EXACTLY via the fx_spawn x argument (no further division to launder the value).
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t FX_IDX = 5, TYPE_ID = 13;
        fx_entry         &e = fx.fx_pool[FX_IDX];
        e                   = fx_entry{};
        e.fx_type           = TYPE_ID;
        e.owner             = 4;
        e.pos_x             = 0.1;
        e.vel_x             = 1.6; // sum = 1.7 (in bounds); diff = -1.5 (negative, fractional)
        e.pos_y             = 0.0;
        e.vel_y             = 0.0;
        e.altitude          = 12;
        e.move_clock        = 0.0;
        fx.fx_live_count    = 9;

        seed_fx_type(fx.fx_type_table[TYPE_ID]);
        fx.fx_type_table[TYPE_ID].colision1 = 0x99;

        // moved_x = trunc(0.1+1.6) = 1 -> candidate_x = 0; moved_y = 0 -> candidate_y = 0.
        // flag_probe_x = trunc(0.1/32) = 0, flag_probe_y = trunc(0/24) = 0 -- all four indices are
        // (0,0), a single tile to flag.
        fx.planes().tile_object_at(0, 0).flags[1] |= 0x20;
        g_rec.clock_script = {2.0};

        tact_view  tv  = fx.view();
        tact_store own = fx.store();
        detail::fx_update_projectile(tv, own, FX_IDX, rec_calls());

        ck_eq((uint32_t)e.fx_type, 0u, "T6: terrain kill fired (sanity -- reached the pullback path)");
        ck_eq((uint32_t)fx.fx_live_count, 8u, "T6: live_count decremented, 0x00431883");
        ck_eq((uint32_t)g_rec.spawn_args.size(), 1u, "T6: fx_spawn called once");
        if (g_rec.spawn_args.size() == 1) {
            const auto &s = g_rec.spawn_args[0];
            ck_eq((uint32_t)s.x, (uint32_t)(-1),
                  "T6: pullback_x = trunc(0.1 - 1.6) = trunc(-1.5) toward zero == -1, NOT floor's "
                  "-2, 0x00431819-32 trunc_sub (x87 RC=11 truncate)");
            ck_eq((uint32_t)s.y, 0u, "T6: pullback_y = trunc(0.0 - 0.0) = 0");
        }
    }
}

} // namespace mh::tact::test
