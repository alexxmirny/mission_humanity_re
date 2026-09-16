//
// sim_unit_target_tracking_selftest.cpp -- `simtest` cases for llm_strat_unit_update_target_tracking,
// llm_strat_unit_update_target2_tracking, and llm_strat_unit_target_tick
// (sim/sim_unit_target_tracking.{h,cpp}), SIM1A.
//
// Own recording calls struct (7 members), same shape as sim_unit_passive_engage_selftest.cpp's
// log_t -- one static instance, reconfigured per case. unit_get_coords is called from up to TWO
// sites within one invocation (target coords, then the firer's own coords for the guided-weapon
// branch), so its mock indexes a small per-call output array by call count, same idea as
// sim_unit_passive_engage_selftest.cpp's first/last-call tracking for scan_targets_for_engage.
//
#include "sim/sim_unit_target_tracking.h"

#include <limits>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct log_t {
    int      get_coords_calls       = 0;
    uint16_t last_get_coords_player = 0;
    int32_t  last_get_coords_index  = 0;
    int32_t  get_coords_write_x[4]  = {0, 0, 0, 0};
    int32_t  get_coords_write_y[4]  = {0, 0, 0, 0};

    int     weapon_pixel_distance_ratio_calls = 0;
    int32_t last_wpdr_weapon_id               = 0;
    int32_t last_wpdr_x1 = 0, last_wpdr_y1 = 0, last_wpdr_x2 = 0, last_wpdr_y2 = 0;
    double  weapon_pixel_distance_ratio_return = 0.0;

    int      predict_calls           = 0;
    uint32_t last_predict_ref        = 0;
    int32_t  last_predict_index      = 0;
    double   last_predict_time_delta = 0.0;
    int32_t  predict_write_x = 0, predict_write_y = 0;

    int      release_calls         = 0;
    uint32_t last_release_player   = 0;
    int32_t  last_release_unit_idx = 0;
    uint32_t last_release_mode     = 0;

    int      target_class_calls      = 0;
    uint32_t last_target_class_ref   = 0;
    int32_t  last_target_class_index = 0;
    int32_t  target_class_return     = 0;

    int      in_range_calls         = 0;
    int32_t  last_in_range_player   = 0;
    int32_t  last_in_range_unit_idx = 0;
    int32_t  last_in_range_tile_x   = 0;
    int32_t  last_in_range_tile_y   = 0;
    int32_t  last_in_range_class    = 0;
    uint32_t in_range_return        = 0;

    int fire_calls = 0;

    void reset() { *this = log_t{}; }
};
log_t g_log;

const unit_target_tracking_calls &recording_calls() {
    static const unit_target_tracking_calls c = {
        [](uint16_t player, int32_t unit_index, int32_t *out_x, int32_t *out_y) -> void {
            const int i = g_log.get_coords_calls < 4 ? g_log.get_coords_calls : 3;
            ++g_log.get_coords_calls;
            g_log.last_get_coords_player = player;
            g_log.last_get_coords_index  = unit_index;
            *out_x                       = g_log.get_coords_write_x[i];
            *out_y                       = g_log.get_coords_write_y[i];
        },
        [](int32_t weapon_id, int32_t x1, int32_t y1, int32_t x2, int32_t y2) -> double {
            ++g_log.weapon_pixel_distance_ratio_calls;
            g_log.last_wpdr_weapon_id = weapon_id;
            g_log.last_wpdr_x1        = x1;
            g_log.last_wpdr_y1        = y1;
            g_log.last_wpdr_x2        = x2;
            g_log.last_wpdr_y2        = y2;
            return g_log.weapon_pixel_distance_ratio_return;
        },
        [](uint32_t player, int32_t unit_idx, uint32_t /*unused_ebx*/, uint32_t /*unused_ecx*/,
           double time_delta, uint32_t *out_x, uint32_t *out_y) -> void {
            ++g_log.predict_calls;
            g_log.last_predict_ref        = player;
            g_log.last_predict_index      = unit_idx;
            g_log.last_predict_time_delta = time_delta;
            // committed llm_strat_unit_predict_coords_after_delay is uint32_t * -- TACT1-P C6,
            // 2026-09-04 -- predict_write_x/y are int32_t, so the store re-interprets the bit pattern.
            *out_x = (uint32_t)g_log.predict_write_x;
            *out_y = (uint32_t)g_log.predict_write_y;
        },
        [](uint32_t player_idx, int32_t unit_idx, uint32_t mode) -> void {
            ++g_log.release_calls;
            g_log.last_release_player   = player_idx;
            g_log.last_release_unit_idx = unit_idx;
            g_log.last_release_mode     = mode;
        },
        [](uint32_t owner_and_kind_flag, int32_t roster_slot) -> int32_t {
            ++g_log.target_class_calls;
            g_log.last_target_class_ref   = owner_and_kind_flag;
            g_log.last_target_class_index = roster_slot;
            return g_log.target_class_return;
        },
        [](int32_t player, int32_t unit_idx, int32_t tile_x, int32_t tile_y, int32_t target_class) -> uint32_t {
            ++g_log.in_range_calls;
            g_log.last_in_range_player   = player;
            g_log.last_in_range_unit_idx = unit_idx;
            g_log.last_in_range_tile_x   = tile_x;
            g_log.last_in_range_tile_y   = tile_y;
            g_log.last_in_range_class    = target_class;
            return g_log.in_range_return;
        },
        []() -> void { ++g_log.fire_calls; },
    };
    return c;
}

// ---- PRIMARY: llm_strat_unit_update_target_tracking @0x00448ee1 ------------------------------------
void test_update_target_tracking_no_guided_weapon() {
    sim_fixture    f;
    sim_store      own = f.store();
    const sim_view v   = f.view();

    unit &u                 = f.u(2, 5);
    u.selected_weapon       = 0;
    u.weapons[0].weapon_id  = 9;
    f.cfg_weapons[9].homing = 0; // not guided
    u.target_ref            = 3;
    u.target_index          = 11;

    g_log.reset();
    g_log.get_coords_write_x[0] = 111;
    g_log.get_coords_write_y[0] = 222;

    int32_t ret = detail::unit_update_target_tracking(v, own, recording_calls(), /*player=*/2, /*unit_idx=*/5);

    ck(ret == 1, "update_target_tracking: always returns 1 -- no aliveness gate on the primary target");
    ck(g_log.get_coords_calls == 1 && g_log.last_get_coords_player == 3 && g_log.last_get_coords_index == 11,
       "update_target_tracking: fetches the TARGET's coords via target_ref&0xf / target_index; homing "
       "!= 2 -> only ONE get_coords call");
    ck(g_log.weapon_pixel_distance_ratio_calls == 0 && g_log.predict_calls == 0,
       "update_target_tracking: homing != 2 -> the lead/predict pair never fires");
    ck(own.unit_at(2, 5).target_fine_x == 111 && own.unit_at(2, 5).target_fine_y == 222,
       "update_target_tracking: stores the fetched coords into target_fine_x/y verbatim");
}

void test_update_target_tracking_guided_weapon() {
    sim_fixture    f;
    sim_store      own = f.store();
    const sim_view v   = f.view();

    unit &u                 = f.u(2, 5);
    u.selected_weapon       = 1;
    u.weapons[1].weapon_id  = 4;
    f.cfg_weapons[4].homing = 2; // guided
    u.target_ref            = 3;
    u.target_index          = 11;
    f.game_clock            = 50.0;

    g_log.reset();
    g_log.get_coords_write_x[0]              = 100; // 1st call: TARGET coords
    g_log.get_coords_write_y[0]              = 200;
    g_log.get_coords_write_x[1]              = 10; // 2nd call: the FIRER's own coords
    g_log.get_coords_write_y[1]              = 20;
    g_log.weapon_pixel_distance_ratio_return = 2.5;
    g_log.predict_write_x                    = 300;
    g_log.predict_write_y                    = 400;

    int32_t ret = detail::unit_update_target_tracking(v, own, recording_calls(), /*player=*/2, /*unit_idx=*/5);

    ck(ret == 1, "update_target_tracking: guided path also returns 1");
    ck(g_log.get_coords_calls == 2,
       "update_target_tracking: homing==2 -> a SECOND get_coords call fetches the FIRER's own position");
    ck(g_log.last_get_coords_player == 2 && g_log.last_get_coords_index == 5,
       "update_target_tracking: the second get_coords call uses (player, unit_idx), not the target");
    ck(g_log.weapon_pixel_distance_ratio_calls == 1 && g_log.last_wpdr_weapon_id == 4 &&
           g_log.last_wpdr_x1 == 10 && g_log.last_wpdr_y1 == 20 && g_log.last_wpdr_x2 == 100 &&
           g_log.last_wpdr_y2 == 200,
       "update_target_tracking: weapon_pixel_distance_ratio(weapon_id, own_x, own_y, target_x, target_y)");
    ck(g_log.predict_calls == 1 && g_log.last_predict_ref == 3 && g_log.last_predict_index == 11,
       "update_target_tracking: predict_coords_after_delay re-targets target_ref&0xf / target_index");
    ck_eq_d(g_log.last_predict_time_delta, 52.5,
            "update_target_tracking: time_delta = GAME_CLOCK(50) + dist_ratio(2.5) = 52.5");
    ck(own.unit_at(2, 5).target_fine_x == 300 && own.unit_at(2, 5).target_fine_y == 400,
       "update_target_tracking: the PREDICTED coords overwrite the initially-fetched target coords");
}

// ---- SECONDARY: llm_strat_unit_update_target2_tracking @0x00449062 ---------------------------------
void test_update_target2_tracking_alive_no_guided() {
    sim_fixture    f;
    sim_store      own = f.store();
    const sim_view v   = f.view();

    unit &u                 = f.u(2, 5);
    u.selected_weapon       = 0;
    u.weapons[0].weapon_id  = 9;
    f.cfg_weapons[9].homing = 0;
    u.target2_ref           = 4;
    u.target2_index         = 8;
    f.u(4, 8).energy        = 15.0; // alive

    g_log.reset();
    g_log.get_coords_write_x[0] = 55;
    g_log.get_coords_write_y[0] = 66;

    int32_t ret = detail::unit_update_target2_tracking(v, own, recording_calls(), 2, 5);

    ck(ret == 1, "update_target2_tracking: alive target -> returns 1");
    ck(g_log.release_calls == 0, "update_target2_tracking: alive target -> release_ref never fires");
    ck(own.unit_at(2, 5).target2_fine_x == 55 && own.unit_at(2, 5).target2_fine_y == 66,
       "update_target2_tracking: stores fetched coords into target2_fine_x/y");
    ck(own.unit_at(2, 5).target2_ref == 4 && own.unit_at(2, 5).target2_index == 8,
       "update_target2_tracking: alive path leaves target2_ref/index untouched");
}

void test_update_target2_tracking_dead_releases_and_clears_both() {
    sim_fixture    f;
    sim_store      own = f.store();
    const sim_view v   = f.view();

    unit &u          = f.u(2, 5);
    u.target2_ref    = 4;
    u.target2_index  = 8;
    f.u(4, 8).energy = 0.0; // dead: ORDERED energy<=0.0

    g_log.reset();
    int32_t ret = detail::unit_update_target2_tracking(v, own, recording_calls(), 2, 5);

    ck(ret == 0, "update_target2_tracking: dead target -> returns 0");
    ck(g_log.release_calls == 1 && g_log.last_release_player == 2 && g_log.last_release_unit_idx == 5 &&
           g_log.last_release_mode == 3,
       "update_target2_tracking: dead target -> target_release_ref(player, unit_idx, mode=3)");
    ck(own.unit_at(2, 5).target2_ref == 0 && own.unit_at(2, 5).target2_index == 0,
       "update_target2_tracking: dead target -> BOTH target2_ref and target2_index are cleared "
       "(unlike target_tick's own release path, which clears target2_ref only)");
    ck(g_log.get_coords_calls == 0, "update_target2_tracking: the dead path returns before ever fetching coords");
}

void test_update_target2_tracking_nan_energy_is_alive() {
    // THE FIX THIS CASE GUARDS: the original's x87 compare is ORDERED `0.0 < energy`, ALIVE taken on
    // true OR on unordered (NaN) -- same idiom sim_unit_passive_engage.cpp's header documents for its
    // own FCOMP/JC shape. A NaN-energy target2 must be treated as ALIVE, not dead.
    sim_fixture    f;
    sim_store      own = f.store();
    const sim_view v   = f.view();

    unit &u          = f.u(2, 5);
    u.target2_ref    = 4;
    u.target2_index  = 8;
    f.u(4, 8).energy = std::numeric_limits<double>::quiet_NaN();

    g_log.reset();
    g_log.get_coords_write_x[0] = 1;
    g_log.get_coords_write_y[0] = 2;
    int32_t ret                 = detail::unit_update_target2_tracking(v, own, recording_calls(), 2, 5);

    ck(ret == 1, "update_target2_tracking: NaN energy is treated as ALIVE (ordered <=0.0 is false on NaN)");
    ck(g_log.release_calls == 0, "update_target2_tracking: NaN energy -> release_ref does NOT fire");
}

void test_update_target2_tracking_guided_weapon() {
    sim_fixture    f;
    sim_store      own = f.store();
    const sim_view v   = f.view();

    unit &u                 = f.u(2, 5);
    u.selected_weapon       = 2;
    u.weapons[2].weapon_id  = 6;
    f.cfg_weapons[6].homing = 2;
    u.target2_ref           = 4;
    u.target2_index         = 8;
    f.u(4, 8).energy        = 10.0;
    f.game_clock            = 20.0;

    g_log.reset();
    g_log.get_coords_write_x[0]              = 700; // target2 coords
    g_log.get_coords_write_y[0]              = 800;
    g_log.get_coords_write_x[1]              = 1; // own coords
    g_log.get_coords_write_y[1]              = 2;
    g_log.weapon_pixel_distance_ratio_return = 1.0;
    g_log.predict_write_x                    = 900;
    g_log.predict_write_y                    = 1000;

    int32_t ret = detail::unit_update_target2_tracking(v, own, recording_calls(), 2, 5);

    ck(ret == 1, "update_target2_tracking: guided path returns 1 too");
    ck(g_log.predict_calls == 1 && g_log.last_predict_ref == 4 && g_log.last_predict_index == 8,
       "update_target2_tracking: guided path re-targets target2_ref&0xf / target2_index");
    ck(own.unit_at(2, 5).target2_fine_x == 900 && own.unit_at(2, 5).target2_fine_y == 1000,
       "update_target2_tracking: guided predicted coords overwrite target2_fine_x/y");
}

// ---- DRIVER: llm_strat_unit_target_tick @0x0047e065 -------------------------------------------------
void test_target_tick_unit_target_alive_in_range_fires() {
    sim_fixture f;
    f.cur_unit_ptr    = &f.u(3, 5);
    f.view_cur_player = 3;
    f.view_cur_index  = 5;

    unit &cur                = f.u(3, 5);
    cur.target2_ref          = 0xa0 | 4; // UNIT-class bits set, owner 4
    cur.target2_index        = 7;
    cur.selected_weapon      = 0;
    cur.weapons[0].weapon_id = 9;
    f.cfg_weapons[9].homing  = 0;
    f.u(4, 7).energy         = 10.0; // alive

    sim_store      own = f.store();
    const sim_view v   = f.view();

    g_log.reset();
    g_log.get_coords_write_x[0] = 320; // -> tile (10, 20)
    g_log.get_coords_write_y[0] = 640;
    g_log.target_class_return   = 42;
    g_log.in_range_return       = 1; // in range -> fire

    detail::unit_target_tick(v, own, recording_calls());

    ck(g_log.release_calls == 0, "target_tick: alive + in range -> the release path never fires");
    ck(g_log.fire_calls == 1, "target_tick: in_range != 0 -> unit_fire_at_target2_if_aimed() fires");
    ck(g_log.target_class_calls == 1 && g_log.last_target_class_ref == (0xa0 | 4) &&
           g_log.last_target_class_index == 7,
       "target_tick: target_class(target2_ref, target2_index) -- the RAW unmasked packed ref");
    ck(g_log.in_range_calls == 1 && g_log.last_in_range_player == 3 && g_log.last_in_range_unit_idx == 5 &&
           g_log.last_in_range_tile_x == 10 && g_log.last_in_range_tile_y == 20 &&
           g_log.last_in_range_class == 42,
       "target_tick: unit_in_weapon_range(player, index, tile_x, tile_y, target_class_result) -- tile "
       "coords are the truncating /32 of the freshly re-tracked target2_fine_x/y");
    ck(own.unit_at(3, 5).target2_ref == (0xa0 | 4), "target_tick: the fire path leaves target2_ref untouched");
}

void test_target_tick_unit_target_calls_local_update_target2_tracking() {
    // (target2_ref & 0xa0) != 0 -> a LOCAL call to unit_update_target2_tracking, same TU (not through
    // `c`). Confirmed by its own visible side effect: target2_fine_x/y end up holding the RE-TRACKED
    // coords from THIS call's own get_coords mock, not the pre-tick sentinel.
    sim_fixture f;
    f.cur_unit_ptr    = &f.u(1, 2);
    f.view_cur_player = 1;
    f.view_cur_index  = 2;

    unit &cur                = f.u(1, 2);
    cur.target2_ref          = 0xa0 | 5;
    cur.target2_index        = 3;
    cur.target2_fine_x       = -1; // sentinel: must be overwritten by the re-track before tile math runs
    cur.target2_fine_y       = -1;
    cur.selected_weapon      = 0;
    cur.weapons[0].weapon_id = 1;
    f.cfg_weapons[1].homing  = 0;
    f.u(5, 3).energy         = 5.0;

    sim_store      own = f.store();
    const sim_view v   = f.view();

    g_log.reset();
    g_log.get_coords_write_x[0] = 64; // -> tile (2, 3)
    g_log.get_coords_write_y[0] = 96;
    g_log.in_range_return       = 0; // not in range -> falls to the release path afterward

    detail::unit_target_tick(v, own, recording_calls());

    ck(g_log.get_coords_calls == 1,
       "target_tick: the local unit_update_target2_tracking call makes its OWN get_coords call");
    ck(g_log.last_in_range_tile_x == 2 && g_log.last_in_range_tile_y == 3,
       "target_tick: tile math runs on the RE-TRACKED target2_fine_x/y (64/32=2, 96/32=3), not the "
       "pre-tick sentinel (-1)");
    ck(g_log.release_calls == 1 && g_log.last_release_mode == 3,
       "target_tick: in_range == 0 -> falls through to the release path even though the target was alive");
    ck(own.unit_at(1, 2).target2_ref == 0, "target_tick: the release path clears target2_ref");
    ck(own.unit_at(1, 2).target2_index == 3,
       "target_tick: ...but leaves target2_index untouched (the local update call's alive branch never "
       "touches it either)");
}

void test_target_tick_building_target_no_local_retrack() {
    sim_fixture f;
    f.cur_unit_ptr    = &f.u(2, 0);
    f.view_cur_player = 2;
    f.view_cur_index  = 0;

    unit &cur          = f.u(2, 0);
    cur.target2_ref    = 0x40 | 6; // BUILDING bit, owner 6
    cur.target2_index  = 9;
    cur.target2_fine_x = 320; // fixed -- the building path never re-tracks
    cur.target2_fine_y = 640;
    f.b(6, 9).energy   = 20.0; // alive building

    sim_store      own = f.store();
    const sim_view v   = f.view();

    g_log.reset();
    g_log.target_class_return = 7;
    g_log.in_range_return     = 0; // -> release path

    detail::unit_target_tick(v, own, recording_calls());

    ck(g_log.get_coords_calls == 0 && g_log.predict_calls == 0,
       "target_tick: a BUILDING target2 (0x40 bit) does NOT call unit_update_target2_tracking locally "
       "-- only a UNIT target2 (0xa0 bit) does");
    ck(g_log.last_in_range_tile_x == 10 && g_log.last_in_range_tile_y == 20,
       "target_tick: tile math uses the UNCHANGED target2_fine_x/y (building path never re-tracks)");
    ck(g_log.release_calls == 1, "target_tick: in_range == 0 -> release path");
}

void test_target_tick_no_target2_releases() {
    sim_fixture f;
    f.cur_unit_ptr    = &f.u(4, 1);
    f.view_cur_player = 4;
    f.view_cur_index  = 1;

    unit &cur         = f.u(4, 1);
    cur.target2_ref   = 0;  // no secondary target at all
    cur.target2_index = 99; // sentinel -- release only clears target2_ref, not target2_index

    sim_store      own = f.store();
    const sim_view v   = f.view();

    g_log.reset();
    detail::unit_target_tick(v, own, recording_calls());

    ck(g_log.target_class_calls == 0 && g_log.in_range_calls == 0 && g_log.fire_calls == 0,
       "target_tick: target2_ref == 0 -> neither alive clause matches, skips straight to release -- no "
       "classify/range/fire calls at all");
    ck(g_log.release_calls == 1 && g_log.last_release_player == 4 && g_log.last_release_unit_idx == 1,
       "target_tick: release still fires with (player, index) even when there was never a target2");
    ck(own.unit_at(4, 1).target2_ref == 0 && own.unit_at(4, 1).target2_index == 99,
       "target_tick: release clears target2_ref ONLY -- target2_index is left as-is (asymmetry with "
       "update_target2_tracking's own dead branch, which clears both)");
}

void test_target_tick_dead_unit_target_releases() {
    sim_fixture f;
    f.cur_unit_ptr    = &f.u(1, 1);
    f.view_cur_player = 1;
    f.view_cur_index  = 1;

    unit &cur         = f.u(1, 1);
    cur.target2_ref   = 0xa0 | 2;
    cur.target2_index = 4;
    f.u(2, 4).energy  = 0.0; // dead -- the TICK's OWN alive gate rejects it, before any local retrack

    sim_store      own = f.store();
    const sim_view v   = f.view();

    g_log.reset();
    detail::unit_target_tick(v, own, recording_calls());

    ck(g_log.get_coords_calls == 0,
       "target_tick: dead unit target2 -> alive is false, the local update_target2_tracking call never "
       "happens (this is the DRIVER's own gate, separate from update_target2_tracking's internal one)");
    ck(g_log.release_calls == 1, "target_tick: dead target -> release path");
}

} // namespace

void run_unit_target_tracking_tests() {
    test_update_target_tracking_no_guided_weapon();
    test_update_target_tracking_guided_weapon();
    test_update_target2_tracking_alive_no_guided();
    test_update_target2_tracking_dead_releases_and_clears_both();
    test_update_target2_tracking_nan_energy_is_alive();
    test_update_target2_tracking_guided_weapon();
    test_target_tick_unit_target_alive_in_range_fires();
    test_target_tick_unit_target_calls_local_update_target2_tracking();
    test_target_tick_building_target_no_local_retrack();
    test_target_tick_no_target2_releases();
    test_target_tick_dead_unit_target_releases();
}

} // namespace mh::sim::test
