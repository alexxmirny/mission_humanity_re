//
// tact_fx_spawn_selftest.cpp -- offline oracle for
//   llm_tact_fx_spawn @0x0042bdce (libmh/tact/tact_fx_spawn.h)
//
// WHY OFFLINE, NOT RIG: see tact_fx_spawn.h's banner -- reaches the gated llm_tact_fx_play_sound;
// its internal DirectSound/retrigger-timer scratch writes live INSIDE that suppressed-in-`ours`
// body, same posture as tact_fx_update_projectile.h. All four outward calls mocked via the calls
// struct; sqrt_fn is std::sqrt (a real, exact IEEE sqrt, not a fixed stub) so the velocity checks
// below are independently computable.
//
#include "tact/tact_fx_spawn.h"

#include <cmath>

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {
using namespace mh::tact;

struct spawn_recorder {
    double  time_now_return   = 0.0;
    int32_t calc_dir24_return = 0;

    int32_t play_sound_calls   = 0;
    int32_t play_sound_fx_type = -1, play_sound_volume = -1, play_sound_pan = -1;

    void reset() { *this = spawn_recorder{}; }
};
spawn_recorder g_rec;

fx_spawn_calls rec_calls() {
    return {
        []() { return g_rec.time_now_return; },
        [](int32_t fx_type, int32_t volume, int32_t pan) {
            g_rec.play_sound_calls++;
            g_rec.play_sound_fx_type = fx_type;
            g_rec.play_sound_volume  = volume;
            g_rec.play_sound_pan     = pan;
        },
        [](int32_t, int32_t, int32_t, int32_t) { return g_rec.calc_dir24_return; },
        [](double x) { return std::sqrt(x); },
    };
}

// Common view geometry for every case below: view_tiles_w=10 (half=5), view_tiles_h=8 (half=4),
// camera at (0,0) -> center_col=5, center_row=4.
void set_view(tact_fixture &fx) {
    fx.view_tiles_w = 10;
    fx.view_tiles_h = 8;
    fx.map_cam_col  = 0;
    fx.map_cam_row  = 0;
}

} // namespace

void run_fx_spawn_tests() {
    // T1: pool full (every slot 1..0x3ff occupied) -- returns 1, allocates nothing, no calls.
    {
        tact_fixture fx;
        g_rec.reset();
        set_view(fx);
        for (int32_t i = 1; i < mh::tact::TACT_FX_POOL_SLOTS; ++i) fx.fx_pool[(size_t)i].fx_type = 7;

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        int32_t    rc  = detail::fx_spawn(v, own, rec_calls(), /*fx_type=*/1, /*owner=*/1, 0, 0, 0, 0, 0);

        ck_eq((uint32_t)rc, 1u, "T1: pool full -> returns 1, 0x0042be2d-0x0042be34");
        ck_eq((uint32_t)g_rec.play_sound_calls, 0u, "T1: no sound call when the pool is full");
        ck_eq((uint32_t)own.fx_live_count(), 0u, "T1: fx_live_count untouched");
    }

    // T2: slot allocation skips occupied slots AND slot 0 (never allocated even though it reads as
    // free); fully-in-view sound at fixed volume 100 with a positive-side pan.
    // 0x0042bdfd-0x0042be34, 0x0042bf8e-0x0042bfff.
    {
        tact_fixture fx;
        g_rec.reset();
        set_view(fx);
        fx.fx_pool[1].fx_type = 9;
        fx.fx_pool[2].fx_type = 9; // occupied -- allocation must land on slot 3
        g_rec.time_now_return = 555.0;

        constexpr int32_t X = 224, Y = 96; // tile_col=7 (dist 2<=5), tile_row=4 (dist 0<=4) -> in view

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        int32_t    rc  = detail::fx_spawn(v, own, rec_calls(), /*fx_type=*/3, /*owner=*/9, X, Y, X, Y,
                                          /*altitude=*/2);

        ck_eq((uint32_t)rc, 0u, "T2: allocation succeeds");
        ck_eq((uint32_t)own.fx_at(0).fx_type, 0u, "T2: slot 0 is NEVER allocated even though free");
        ck_eq((uint32_t)own.fx_at(3).fx_type, 3u, "T2: lands on the first free slot AFTER 1,2 -- slot 3");
        fx_entry &e = own.fx_at(3);
        ck_eq((uint32_t)e.owner, 9u, "T2: owner stamped");
        ck_eq_d(e.pos_x, (double)X, "T2: pos_x = (double)x, 0x0042be5e");
        ck_eq_d(e.pos_y, (double)Y, "T2: pos_y = (double)y");
        ck_eq_d(e.travel_dx, 0.0, "T2: travel_dx init 0");
        ck_eq_d(e.travel_dy, 0.0, "T2: travel_dy init 0");
        ck_eq((uint32_t)e.tile_col, 7u, "T2: tile_col = x/32, 0x0042be9d");
        ck_eq((uint32_t)e.tile_row, 4u, "T2: tile_row = y/24, 0x0042bebf");
        ck_eq((uint32_t)e.altitude, 2u, "T2: altitude stamped");
        ck_eq_d(e.move_clock, 555.0, "T2: move_clock = time_now(), 0x0042bee3");
        ck_eq((uint32_t)g_rec.play_sound_calls, 1u, "T2: fully-in-view -> exactly one sound call");
        ck_eq((uint32_t)g_rec.play_sound_volume, 100u, "T2: fixed volume 100, 0x0042bf9c");
        // dcol = 7-5 = 2; pan = 2*128/10 + 128 = 25+128 = 153.
        ck_eq((uint32_t)g_rec.play_sound_pan, 153u, "T2: pan = dcol*128/view_w + 128, 0x0042bfce-0x0042bfe0");
        ck_eq((uint32_t)own.fx_live_count(), 1u, "T2: fx_live_count incremented, 0x0042c25b");
    }

    // T3: too far even for a distant cue -- the sound call is skipped entirely (falls straight to
    // step 4). 0x0042bfff-0x0042c024 (the far-skip test).
    {
        tact_fixture fx;
        g_rec.reset();
        set_view(fx);
        constexpr int32_t X = 640, Y = 96; // tile_col=20 (dist 15 > view_tiles_w=10)

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::fx_spawn(v, own, rec_calls(), /*fx_type=*/3, /*owner=*/1, X, Y, X, Y, /*altitude=*/0);

        ck_eq((uint32_t)g_rec.play_sound_calls, 0u, "T3: too far -- no sound call at all");
    }

    // T4: distant-but-audible, dist_col > dist_row -- volume falls off on the COLUMN axis.
    // 0x0042c02c-0x0042c052.
    {
        tact_fixture fx;
        g_rec.reset();
        set_view(fx);
        constexpr int32_t X = 416, Y = 96; // tile_col=13 (dist 8), tile_row=4 (dist 0)

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::fx_spawn(v, own, rec_calls(), /*fx_type=*/3, /*owner=*/1, X, Y, X, Y, /*altitude=*/0);

        ck_eq((uint32_t)g_rec.play_sound_calls, 1u, "T4: distant-but-audible -- one sound call");
        // vol = 100/(dist_col-half_w) = 100/(8-5) = 33.
        ck_eq((uint32_t)g_rec.play_sound_volume, 33u, "T4: vol = 100/(dist_col-half_w), 0x0042c052");
    }

    // T5: distant-but-audible, dist_row > dist_col -- volume falls off on the ROW axis instead.
    // 0x0042c05d-0x0042c083.
    {
        tact_fixture fx;
        g_rec.reset();
        set_view(fx);
        constexpr int32_t X = 160, Y = 264; // tile_col=5 (dist 0), tile_row=11 (dist 7)

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::fx_spawn(v, own, rec_calls(), /*fx_type=*/3, /*owner=*/1, X, Y, X, Y, /*altitude=*/0);

        ck_eq((uint32_t)g_rec.play_sound_calls, 1u, "T5: one sound call");
        // vol = 100/(dist_row-half_h) = 100/(7-4) = 33.
        ck_eq((uint32_t)g_rec.play_sound_volume, 33u, "T5: vol = 100/(dist_row-half_h), 0x0042c083");
    }

    // T6: dist_col == dist_row EXACTLY -- the original reads an uninitialised stack slot here; this
    // translation substitutes a documented vol=0 rather than invoking UB. Pins the SUBSTITUTE value,
    // not a re-derivation of the garbage. 0x0042c055-0x0042c05b.
    {
        tact_fixture fx;
        g_rec.reset();
        set_view(fx);
        constexpr int32_t X = 352, Y = 240; // tile_col=11 (dist 6), tile_row=10 (dist 6)

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::fx_spawn(v, own, rec_calls(), /*fx_type=*/3, /*owner=*/1, X, Y, X, Y, /*altitude=*/0);

        ck_eq((uint32_t)g_rec.play_sound_calls, 1u, "T6: one sound call");
        ck_eq((uint32_t)g_rec.play_sound_volume, 0u,
              "T6: tie case -- documented vol=0 substitute for the UB read, not a derived value");
    }

    // T7: spawn == target -> dir24 forced to 1 (calc_dir24 NOT called), velocity (0,0), and the
    // `direct` sprite-frame formula reading dir24=1 gives frame 0 regardless of `frames`.
    // 0x0042c0fa-0x0042c11a, 0x0042c1fa-0x0042c25b.
    {
        tact_fixture fx;
        g_rec.reset();
        set_view(fx);
        constexpr int32_t X = 640, Y = 96; // the "skip" coordinates from T3 -- sound irrelevant here
        g_rec.calc_dir24_return = 99;      // must NOT be used -- same-point short-circuits calc_dir24

        fx.fx_type_table[3].direct = 1;
        fx.fx_type_table[3].frames = 5;

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::fx_spawn(v, own, rec_calls(), /*fx_type=*/3, /*owner=*/1, X, Y, X, Y, /*altitude=*/0);

        fx_entry &e = own.fx_at(1);
        ck_eq((uint32_t)e.dir24, 1u, "T7: dir24 = 1 when spawn == target, 0x0042c108");
        ck_eq_d(e.target_x, (double)X, "T7: target_x = (double)x2");
        ck_eq_d(e.target_y, (double)Y, "T7: target_y = (double)y2");
        ck_eq_d(e.vel_x, 0.0, "T7: vel_x = 0 when spawn == target, 0x0042c1a8");
        ck_eq_d(e.vel_y, 0.0, "T7: vel_y = 0");
        // direct != 0 -> sprite_frame = (dir24-1)*frames + frame_counter(0) = (1-1)*5+0 = 0.
        ck_eq((uint32_t)e.sprite_frame, 0u, "T7: sprite_frame via the direct formula, dir24=1 -> 0");
    }

    // T8: spawn != target -> calc_dir24 IS called and its result stamped; velocity is the exact
    // normalized (target-spawn) vector via a REAL sqrt (3-4-5 triangle for exact division).
    // 0x0042c108-0x0042c1a8.
    {
        tact_fixture fx;
        g_rec.reset();
        set_view(fx);
        constexpr int32_t X = 640, Y = 96, X2 = 643, Y2 = 100; // dxi=3, dyi=4 -> dist_sq=25, dist=5
        g_rec.calc_dir24_return = 12;

        fx.fx_type_table[3].direct = 1;
        fx.fx_type_table[3].frames = 6;

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::fx_spawn(v, own, rec_calls(), /*fx_type=*/3, /*owner=*/1, X, Y, X2, Y2, /*altitude=*/0);

        fx_entry &e = own.fx_at(1);
        ck_eq((uint32_t)e.dir24, 12u, "T8: dir24 = calc_dir24(...) when spawn != target, 0x0042c11a");
        ck_eq_d(e.target_x, (double)X2, "T8: target_x = (double)x2");
        ck_eq_d(e.target_y, (double)Y2, "T8: target_y = (double)y2");
        ck_eq_d(e.vel_x, 0.6, "T8: vel_x = dxi/dist = 3/5, 0x0042c194");
        ck_eq_d(e.vel_y, 0.8, "T8: vel_y = dyi/dist = 4/5, 0x0042c1a2");
        // direct != 0 -> sprite_frame = (12-1)*6 + 0 = 66.
        ck_eq((uint32_t)e.sprite_frame, 66u, "T8: sprite_frame = (dir24-1)*frames, 0x0042c220");
    }

    // T9: `direct == 0` -- sprite_frame is just frame_counter (0), regardless of dir24/frames.
    // 0x0042c1fa-0x0042c25b.
    {
        tact_fixture fx;
        g_rec.reset();
        set_view(fx);
        constexpr int32_t X = 640, Y = 96;
        g_rec.calc_dir24_return = 9;

        fx.fx_type_table[3].direct = 0;
        fx.fx_type_table[3].frames = 40; // must NOT be used

        tact_view  v   = fx.view();
        tact_store own = fx.store();
        detail::fx_spawn(v, own, rec_calls(), /*fx_type=*/3, /*owner=*/1, X, Y, X + 1, Y, /*altitude=*/0);

        fx_entry &e = own.fx_at(1);
        ck_eq((uint32_t)e.sprite_frame, 0u, "T9: direct==0 -> sprite_frame = frame_counter (0), 0x0042c236");
    }
}

} // namespace mh::tact::test
