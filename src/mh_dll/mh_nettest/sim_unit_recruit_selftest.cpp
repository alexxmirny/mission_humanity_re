//
// sim_unit_recruit_selftest.cpp -- `simtest` cases for llm_unit_recruit (sim/sim_unit_recruit.{h,cpp}),
//
// Own recording calls struct (4 members), same shape as sim_unit_passive_engage_selftest.cpp's
// log_t -- one static instance, reconfigured per case. One case per named RECRUIT_ERR_*/RECRUIT_OK
// outcome, plus boundary/addend cases the header's own hazard notes call out explicitly.
//
#include "sim/sim_unit_recruit.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct log_t {
    int     housing_count_add_calls = 0;
    int32_t last_housing_add_player = -1;
    int32_t last_housing_add_type   = -1;

    int      spawn_docked_calls  = 0;
    uint16_t last_spawn_proto    = 0;
    uint16_t last_spawn_player   = 0;
    uint32_t last_spawn_probe    = 0xffffffffu;
    int32_t  spawn_docked_return = 0;

    int      add_docked_calls  = 0;
    uint32_t last_add_proto    = 0;
    uint16_t last_add_player   = 0;
    uint32_t last_add_probe    = 0xffffffffu;
    int32_t  add_docked_return = 0;

    int      notify_calls        = 0;
    uint16_t last_notify_player  = 0;
    uint16_t last_notify_type    = 0;
    uint32_t last_notify_unit_id = 0;
    uint32_t last_notify_mode    = 0;

    void reset() { *this = log_t{}; }
};
log_t g_log;

const unit_recruit_calls &recording_calls() {
    static const unit_recruit_calls c = {
        [](int32_t player, int32_t unit_type_id) -> void {
            ++g_log.housing_count_add_calls;
            g_log.last_housing_add_player = player;
            g_log.last_housing_add_type   = unit_type_id;
        },
        [](uint16_t unit_proto_id, uint16_t player, uint32_t probe_slot) -> int32_t {
            ++g_log.spawn_docked_calls;
            g_log.last_spawn_proto  = unit_proto_id;
            g_log.last_spawn_player = player;
            g_log.last_spawn_probe  = probe_slot;
            return g_log.spawn_docked_return;
        },
        [](uint32_t unit_proto_id, uint16_t player, uint32_t probe_slot) -> int32_t {
            ++g_log.add_docked_calls;
            g_log.last_add_proto  = unit_proto_id;
            g_log.last_add_player = player;
            g_log.last_add_probe  = probe_slot;
            return g_log.add_docked_return;
        },
        [](uint16_t player, uint16_t unit_type_id, uint32_t unit_id, uint32_t mode) -> void {
            ++g_log.notify_calls;
            g_log.last_notify_player  = player;
            g_log.last_notify_type    = unit_type_id;
            g_log.last_notify_unit_id = unit_id;
            g_log.last_notify_mode    = mode;
        },
    };
    return c;
}

// Bundles the fixture + store + the two IDs every case needs, pre-seeded so a case only names the
// fields it actually cares about. player(6) / unit_type_id(30) are distinct and non-symmetric so a
// swapped-argument bug (player<->unit_type_id, or player<->unit_id) does not accidentally cancel out.
struct ctx {
    sim_fixture f;
    sim_store   own;
    int32_t     player       = 6;
    uint32_t    unit_type_id = 30;

    ctx() : own(f.store()) {
        f.u(player, 0).order                    = 0;                   // order-queue-length counter, room to spare
        f.cfg_units[unit_type_id].type          = UNIT_TYPE_UNDEFINED; // no housing check by default
        f.cfg_units[unit_type_id].soldier_count = 0;                   // step (3) a no-op by default
        f.cfg_units[unit_type_id].move_op_code  = MOVE_OP_CODE_GROUND;
        f.player_race                           = 0;
    }
    const sim_view v() { return f.view(); }
};

// ---- (1) order-queue-length cap (units[player][0].order, the QUEUE-LENGTH idiom) -------------------
void test_queue_full() {
    ctx c;
    c.f.u(c.player, 0).order = RECRUIT_ORDER_QUEUE_CAP; // 90; +1 (91) > 90 -> full

    g_log.reset();
    int32_t ret = detail::unit_recruit(c.v(), c.own, recording_calls(), c.player, c.unit_type_id);

    ck(ret == RECRUIT_ERR_QUEUE_FULL, "recruit: units[player][0].order+1 > 0x5a -> RECRUIT_ERR_QUEUE_FULL");
    ck(g_log.housing_count_add_calls == 0 && g_log.spawn_docked_calls == 0 && g_log.add_docked_calls == 0,
       "recruit: queue-full is checked FIRST -- no callee fires at all");
}

void test_queue_not_yet_full_boundary() {
    ctx c;
    c.f.u(c.player, 0).order                   = RECRUIT_ORDER_QUEUE_CAP - 1; // 89; +1 == 90, NOT > 90 -> passes
    c.f.cfg_units[c.unit_type_id].move_op_code = 0;                           // non-ground -> add_docked (cheap OK path)

    g_log.reset();
    g_log.add_docked_return = 55;
    int32_t ret             = detail::unit_recruit(c.v(), c.own, recording_calls(), c.player, c.unit_type_id);

    ck(ret == RECRUIT_OK,
       "recruit: order+1 == cap is the STRICT boundary (not >=), so 89 -> passes through to OK");
}

// ---- (2) per-category housing caps -------------------------------------------------------------------
void test_soldier_cap_rejected_with_addend() {
    ctx c;
    c.f.cfg_units[c.unit_type_id].type           = 5; // 0 < 5 < UNIT_TYPE_A_WALKER(0xb) -> SOLDIER branch
    c.f.cfg_units[c.unit_type_id].soldier_count  = 2; // the addend
    c.f.unit_housing[c.player].used_soldiers     = 5;
    c.f.unit_housing[c.player].cap_prev_soldiers = 6; // 6 < 5+2(=7) -- would PASS as plain 5<6 (no addend)

    g_log.reset();
    int32_t ret = detail::unit_recruit(c.v(), c.own, recording_calls(), c.player, c.unit_type_id);

    ck(ret == RECRUIT_ERR_SOLDIER_CAP,
       "recruit: SOLDIER housing compares cap against used+soldier_count (the ADDEND) -- 6 < 5+2 rejects "
       "where a plain 5<6 (no addend) would have passed");
    ck(g_log.housing_count_add_calls == 0, "recruit: housing-cap rejection short-circuits before the grant path");
}

void test_soldier_cap_ok_at_strict_boundary() {
    ctx c;
    c.f.cfg_units[c.unit_type_id].type           = 5;
    c.f.cfg_units[c.unit_type_id].soldier_count  = 2;
    c.f.unit_housing[c.player].used_soldiers     = 5;
    c.f.unit_housing[c.player].cap_prev_soldiers = 7; // 7 < 5+2(=7) is FALSE (strict <) -> passes
    c.f.cfg_units[c.unit_type_id].move_op_code   = MOVE_OP_CODE_GROUND;

    g_log.reset();
    g_log.spawn_docked_return = 1;
    int32_t ret               = detail::unit_recruit(c.v(), c.own, recording_calls(), c.player, c.unit_type_id);

    ck(ret == RECRUIT_OK, "recruit: SOLDIER cap check is STRICT < -- cap==used+addend passes, not rejected");
}

void test_vehicle_cap_rejected_no_addend_boundary() {
    ctx c;
    c.f.cfg_units[c.unit_type_id].type           = UNIT_TYPE_A_WALKER; // the VEHICLE branch's lower bound
    c.f.unit_housing[c.player].used_vehicles     = 5;
    c.f.unit_housing[c.player].cap_prev_vehicles = 5; // <= : the STRICT boundary, no addend at all

    g_log.reset();
    int32_t ret = detail::unit_recruit(c.v(), c.own, recording_calls(), c.player, c.unit_type_id);

    ck(ret == RECRUIT_ERR_VEHICLE_CAP,
       "recruit: VEHICLE housing is plain cap<=used (no soldier_count addend), rejected at cap==used");
}

void test_heli_cap_rejected() {
    ctx c;
    c.f.cfg_units[c.unit_type_id].type        = UNIT_TYPE_A_HELI; // the HELI branch's lower bound
    c.f.unit_housing[c.player].used_helis     = 3;
    c.f.unit_housing[c.player].cap_prev_helis = 3; // <=

    g_log.reset();
    int32_t ret = detail::unit_recruit(c.v(), c.own, recording_calls(), c.player, c.unit_type_id);

    ck(ret == RECRUIT_ERR_HELI_CAP, "recruit: HELI branch (A_HELI..A_PLANE) rejects at cap<=used");
}

void test_plane_cap_rejected() {
    ctx c;
    c.f.cfg_units[c.unit_type_id].type         = UNIT_TYPE_A_PLANE; // the PLANE branch's lower bound
    c.f.unit_housing[c.player].used_planes     = 1;
    c.f.unit_housing[c.player].cap_prev_planes = 1; // <=

    g_log.reset();
    int32_t ret = detail::unit_recruit(c.v(), c.own, recording_calls(), c.player, c.unit_type_id);

    ck(ret == RECRUIT_ERR_PLANE_CAP, "recruit: PLANE branch (A_PLANE..A_HELI_MOTHER) rejects at cap<=used");
}

void test_no_housing_check_below_undefined_and_above_heli_mother() {
    // type == UNIT_TYPE_UNDEFINED (0): no housing check at all -- ctx's default. Exercises the OK
    // grant path via the GROUND dispatch (unit_spawn_docked).
    {
        ctx c; // type left at ctx's default UNDEFINED
        g_log.reset();
        g_log.spawn_docked_return = 77;

        int32_t ret = detail::unit_recruit(c.v(), c.own, recording_calls(), c.player, c.unit_type_id);
        ck(ret == RECRUIT_OK, "recruit: type == UNDEFINED skips the housing check entirely -> OK");
    }
    // type >= UNIT_TYPE_A_HELI_MOTHER: also no housing check. Exercises the non-ground dispatch
    // (unit_add_docked) and the FULL (uint32) unit_proto_id argument at the same time.
    {
        ctx c;
        c.f.cfg_units[c.unit_type_id].type         = UNIT_TYPE_A_HELI_MOTHER;
        c.f.cfg_units[c.unit_type_id].move_op_code = 0x0a; // non-ground

        g_log.reset();
        g_log.add_docked_return = 88;
        int32_t ret             = detail::unit_recruit(c.v(), c.own, recording_calls(), c.player, c.unit_type_id);
        ck(ret == RECRUIT_OK, "recruit: type >= A_HELI_MOTHER also skips the housing check -> OK");
        ck(g_log.add_docked_calls == 1 && g_log.last_add_proto == c.unit_type_id,
           "recruit: add_docked's first arg is the FULL uint32 unit_type_id, not truncated to 16 bits "
           "(unlike spawn_docked's)");
    }
}

// ---- (3) soldier-headcount cap, INDEPENDENT of the category branch above ---------------------------
void test_soldier_headcount_race1() {
    ctx c;
    c.f.cfg_units[c.unit_type_id].type                          = UNIT_TYPE_UNDEFINED; // no housing check, isolate step 3
    c.f.cfg_units[c.unit_type_id].soldier_count                 = 3;
    c.f.soldiers[c.player * SOLDIERS_PER_PLAYER + 0].owner_unit = 96; // 96+3+1 == 100 >= 100 cap
    c.f.player_race                                             = 1;

    g_log.reset();
    int32_t ret = detail::unit_recruit(c.v(), c.own, recording_calls(), c.player, c.unit_type_id);

    ck(ret == RECRUIT_ERR_SOLDIER_HEADCOUNT_RACE1,
       "recruit: projected headcount >= 100 and _G_LLM_STRAT_PLAYER_RACE == 1 -> RACE1 rejection code");
    ck(g_log.housing_count_add_calls == 0, "recruit: headcount rejection short-circuits before the grant path");
}

void test_soldier_headcount_other_race() {
    ctx c;
    c.f.cfg_units[c.unit_type_id].type                          = UNIT_TYPE_UNDEFINED;
    c.f.cfg_units[c.unit_type_id].soldier_count                 = 3;
    c.f.soldiers[c.player * SOLDIERS_PER_PLAYER + 0].owner_unit = 96;
    c.f.player_race                                             = 2; // != 1

    g_log.reset();
    int32_t ret = detail::unit_recruit(c.v(), c.own, recording_calls(), c.player, c.unit_type_id);

    ck(ret == RECRUIT_ERR_SOLDIER_HEADCOUNT_OTHER,
       "recruit: same over-cap projection, PLAYER_RACE != 1 -> OTHER rejection code");
}

void test_soldier_headcount_boundary_passes_below_cap() {
    ctx c;
    c.f.cfg_units[c.unit_type_id].type                          = UNIT_TYPE_UNDEFINED;
    c.f.cfg_units[c.unit_type_id].soldier_count                 = 3;
    c.f.soldiers[c.player * SOLDIERS_PER_PLAYER + 0].owner_unit = 95; // 95+3+1 == 99, < 100 -> passes
    c.f.player_race                                             = 1;

    g_log.reset();
    g_log.spawn_docked_return = 1;
    int32_t ret               = detail::unit_recruit(c.v(), c.own, recording_calls(), c.player, c.unit_type_id);

    ck(ret == RECRUIT_OK, "recruit: projected headcount 99 < 100 -> the cap does not reject");
}

void test_soldier_headcount_skipped_when_soldier_count_zero() {
    // cfg_units[type].soldier_count == 0 -- step (3) is a no-op regardless of the used-slot scalar,
    // even when that scalar is already sky-high.
    ctx c;
    c.f.cfg_units[c.unit_type_id].type                          = UNIT_TYPE_UNDEFINED;
    c.f.cfg_units[c.unit_type_id].soldier_count                 = 0;
    c.f.soldiers[c.player * SOLDIERS_PER_PLAYER + 0].owner_unit = 999; // would blow the cap if checked

    g_log.reset();
    g_log.spawn_docked_return = 1;
    int32_t ret               = detail::unit_recruit(c.v(), c.own, recording_calls(), c.player, c.unit_type_id);

    ck(ret == RECRUIT_OK,
       "recruit: soldier_count == 0 -> the headcount check never runs, no matter the used-slot count");
}

// ---- (4) the grant path ---------------------------------------------------------------------------
void test_grant_ground_spawn_ok() {
    ctx c;
    c.f.cfg_units[c.unit_type_id].move_op_code = MOVE_OP_CODE_GROUND;
    c.f.u(c.player, 0).order                   = 10;

    g_log.reset();
    g_log.spawn_docked_return = 123;
    int32_t ret               = detail::unit_recruit(c.v(), c.own, recording_calls(), c.player, c.unit_type_id);

    ck(ret == RECRUIT_OK, "recruit: successful ground spawn -> RECRUIT_OK");
    ck(g_log.housing_count_add_calls == 1 && g_log.last_housing_add_player == c.player &&
           g_log.last_housing_add_type == (int32_t)c.unit_type_id,
       "recruit: housing_count_add(player, unit_type_id) fires FIRST on the grant path, args not swapped");
    ck(c.own.unit_at(c.player, 0).order == 11,
       "recruit: units[player][0].order += STOP_TO_DEFAULT(1) -- the queue-length COUNTER idiom, NOT a "
       "real order-state assignment");
    ck(g_log.spawn_docked_calls == 1 && g_log.last_spawn_proto == c.unit_type_id &&
           g_log.last_spawn_player == c.player && g_log.last_spawn_probe == 0,
       "recruit: move_op_code == GROUND -> unit_spawn_docked(unit_type_id16, player16, probe_slot=0)");
    ck(g_log.add_docked_calls == 0, "recruit: the ground path never calls add_docked");
    ck(g_log.notify_calls == 1 && g_log.last_notify_player == c.player &&
           g_log.last_notify_type == c.unit_type_id && g_log.last_notify_unit_id == 123 &&
           g_log.last_notify_mode == 4,
       "recruit: success -> ai_notify_unit_lifecycle(player, unit_type_id, unit_id, mode=4)");
}

void test_grant_ground_spawn_failed() {
    ctx c;
    c.f.cfg_units[c.unit_type_id].move_op_code = MOVE_OP_CODE_GROUND;

    g_log.reset();
    g_log.spawn_docked_return = 0; // spawn failed
    int32_t ret               = detail::unit_recruit(c.v(), c.own, recording_calls(), c.player, c.unit_type_id);

    ck(ret == RECRUIT_ERR_SPAWN_FAILED, "recruit: unit_spawn_docked returning 0 -> RECRUIT_ERR_SPAWN_FAILED");
    ck(g_log.notify_calls == 0, "recruit: a failed spawn never reaches the notify call");
    ck(g_log.housing_count_add_calls == 1,
       "recruit: housing_count_add and the order-counter bump already happened BEFORE the spawn attempt "
       "-- a failed spawn does not roll them back");
}

void test_grant_non_ground_add_docked_ok() {
    ctx c;
    c.f.cfg_units[c.unit_type_id].move_op_code = 0x0a; // != GROUND(0xf)

    g_log.reset();
    g_log.add_docked_return = 200;
    int32_t ret             = detail::unit_recruit(c.v(), c.own, recording_calls(), c.player, c.unit_type_id);

    ck(ret == RECRUIT_OK, "recruit: successful non-ground add_docked -> RECRUIT_OK");
    ck(g_log.spawn_docked_calls == 0, "recruit: the non-ground path never calls spawn_docked");
    ck(g_log.add_docked_calls == 1 && g_log.last_add_proto == c.unit_type_id &&
           g_log.last_add_player == c.player && g_log.last_add_probe == 0,
       "recruit: unit_add_docked(unit_type_id AS FULL uint32, player16, probe_slot=0)");
    ck(g_log.notify_calls == 1 && g_log.last_notify_unit_id == 200,
       "recruit: notify fires with the add_docked-returned unit id");
}

void test_grant_non_ground_add_docked_failed() {
    ctx c;
    c.f.cfg_units[c.unit_type_id].move_op_code = 0x0a;

    g_log.reset();
    g_log.add_docked_return = 0;
    int32_t ret             = detail::unit_recruit(c.v(), c.own, recording_calls(), c.player, c.unit_type_id);

    ck(ret == RECRUIT_ERR_SPAWN_FAILED, "recruit: unit_add_docked returning 0 -> RECRUIT_ERR_SPAWN_FAILED too");
    ck(g_log.notify_calls == 0, "recruit: failed add_docked never reaches notify");
}

} // namespace

void run_unit_recruit_tests() {
    test_queue_full();
    test_queue_not_yet_full_boundary();
    test_soldier_cap_rejected_with_addend();
    test_soldier_cap_ok_at_strict_boundary();
    test_vehicle_cap_rejected_no_addend_boundary();
    test_heli_cap_rejected();
    test_plane_cap_rejected();
    test_no_housing_check_below_undefined_and_above_heli_mother();
    test_soldier_headcount_race1();
    test_soldier_headcount_other_race();
    test_soldier_headcount_boundary_passes_below_cap();
    test_soldier_headcount_skipped_when_soldier_count_zero();
    test_grant_ground_spawn_ok();
    test_grant_ground_spawn_failed();
    test_grant_non_ground_add_docked_ok();
    test_grant_non_ground_add_docked_failed();
}

} // namespace mh::sim::test
