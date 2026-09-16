//
// tact_unit_spawn_selftest.cpp -- offline oracle for
//   llm_tact_unit_spawn @0x0042b948 (libmh/tact/tact_unit_spawn.cpp)
//
// WHY OFFLINE, NOT RIG: see tact_unit_spawn.h's banner -- llm_rand @0x004da98b is called DIRECTLY
// (0x0042bc05) to jitter the spawned unit's frame_interval, and llm_rand is a TACT-CUT2 UNGATED
// effectful shared callee (a PRNG-advance is shared state; double-firing it under a shadow
// snapshot/restore window desyncs silently, with no entry gate anywhere to close the hole). Any
// function that calls llm_rand is therefore proven OFFLINE, full stop. All five outward calls
// (llm_fatal_cleanup, utils_abort, time_GetCurrentTime x4, llm_rand, llm_tact_unit_vision_add) are
// mocked via the file's own `unit_spawn_calls` table.
//
#include "tact/tact_unit_spawn.h"

#include "tact_test_support.h"

namespace mh::tact::test {

namespace {
using namespace mh::tact;

struct spawn_recorder {
    int32_t              fatal_cleanup_calls = 0;
    std::vector<int32_t> abort_args;
    std::vector<double>  time_script; // scripted time_GetCurrentTime() return sequence
    size_t               time_cursor = 0;
    int32_t              time_calls  = 0;
    std::vector<int32_t> rand_script; // scripted llm_rand() return sequence
    size_t               rand_cursor = 0;
    int32_t              rand_calls  = 0;
    std::vector<int32_t> vision_add_args;
    void                 reset() { *this = spawn_recorder{}; }
};
spawn_recorder g_rec;

double mock_time() {
    ++g_rec.time_calls;
    const double v =
        g_rec.time_cursor < g_rec.time_script.size() ? g_rec.time_script[g_rec.time_cursor] : 0.0;
    if (g_rec.time_cursor + 1 < g_rec.time_script.size()) ++g_rec.time_cursor;
    return v;
}
int32_t mock_rand() {
    ++g_rec.rand_calls;
    const int32_t v =
        g_rec.rand_cursor < g_rec.rand_script.size() ? g_rec.rand_script[g_rec.rand_cursor] : 0;
    if (g_rec.rand_cursor + 1 < g_rec.rand_script.size()) ++g_rec.rand_cursor;
    return v;
}

const unit_spawn_calls &rec_calls() {
    static const unit_spawn_calls c = {
        []() { g_rec.fatal_cleanup_calls++; },
        [](int32_t status) { g_rec.abort_args.push_back(status); },
        mock_time,
        mock_rand,
        [](int32_t unit_idx) { g_rec.vision_add_args.push_back(unit_idx); },
    };
    return c;
}

// Seeds EVERY field of a roster record with a distinct, non-symmetric sentinel: both the fields
// this function writes (so a translation that merely left the fixture's zero-init default in place
// cannot pass by coincidence) and the 14 fields the header banner lists as fields this function
// DELIBERATELY DOES NOT INITIALISE (so a translation that zeroes -- or otherwise touches -- any of
// them is caught). `.type` is left 0: it is the free-slot marker the search predicate itself reads
// (0x0042b9af), not a field under test by this seeding.
void seed_all_sentinels(tact_unit &u) {
    u.type                 = 0;
    u.owner                = 0x10;
    u.status               = 0x11;
    u.pos_col              = 0x12;
    u.pos_row              = 0x13;
    u.anim_state           = 0x14; // written LATE (0x0042bd9f), still back-half
    u.def_stat             = 0x15;
    u.vision_angle         = 0x1617;
    u.vision_dist          = 0x18;
    u.facing_dir           = 0x19;
    u.move_state_timer     = 111.1;
    u.weapon_timer         = 111.25; // NEVER written by this function
    u.wander_check_time    = 222.2;
    u.cmd_wait_until_time  = 333.3;
    u.anim_frame_time      = 444.4;
    u.frame_index          = 0x1A;
    u.anim_cycle_time      = 555.5;
    u.frame_interval       = 666.6;
    u.progress             = 0x1B;
    u.sprite_id            = 0xBEEF; // NEVER written
    u.move_path_slot       = 0x1C1D;
    u.move_path_step       = 0x1E1F;
    u.hp                   = 0x2021;
    u.move_retry_wait      = 0x2223;
    u.move_retry_attempts  = 0x2425;
    u.move_stuck_countdown = 0x2627;
    u.cmd_index            = 0x28;
    for (int i = 0; i < 128; ++i) {
        auto &e          = u.cmd_queue[i];
        e.interrupt_flag = (uint8_t)(0x50 + i); // NEVER written (stale by its own field comment)
        e.op             = (uint16_t)(0x3000 + i);
        e.arg0           = (uint16_t)(0x4000 + i);
        e.arg1           = (uint16_t)(0x5000 + i);
        e.arg2           = (uint16_t)(0x6000 + i);
        e.arg3           = (uint16_t)(0x7000 + i);
    }
    u.attack_interrupt_flag = 0xA1; // NEVER written
    u.attack_cmd_op         = 0x2930;
    u.attack_gun_toggle     = 0x3132;
    u.attack_cmd_arg1       = 0x3334;
    u.aim_x                 = 0x3536;
    u.aim_y                 = 0x3738;
    u.face_interrupt_flag   = 0xA2;   // NEVER written
    u.face_cmd_op           = 0xA3A3; // NEVER written
    u.face_cmd_target_dir   = 0xA4A4; // NEVER written
    u.face_cmd_arg1         = 0xA5A5; // NEVER written
    u.face_cmd_arg2         = 0xA6A6; // NEVER written
    u.face_cmd_arg3         = 0xA7A7; // NEVER written
    u.move_aborted_op       = 0xA8;   // NEVER written
    u.active_gun            = 0x39;
    u.gun1_bullets          = 0x3A;
    u.gun2_bullets          = 0x3B;
    u.gun1_magazines        = 0x3C;
    u.gun2_magazines        = 0x3D;
    u.squad_group_id        = 0x3E;
    u.move_redirect_col     = 0xA9; // NEVER written
    u.move_redirect_row     = 0xAA; // NEVER written
    u.click_preview_facing  = 0xAB; // NEVER written
}

// The 14 fields the header banner names under "FIELDS THIS FUNCTION DELIBERATELY DOES NOT
// INITIALISE" (plus the per-entry cmd_queue.interrupt_flag, named there too) -- must survive
// unconditionally, on the full-success path AND on the hp_pct-rollback path alike.
void check_never_written(const tact_unit &u, const char *tag) {
    ck_eq_d(u.weapon_timer, 111.25, tag);
    ck_eq((uint32_t)u.attack_interrupt_flag, 0xA1u, tag);
    ck_eq((uint32_t)u.face_interrupt_flag, 0xA2u, tag);
    ck_eq((uint32_t)u.face_cmd_op, 0xA3A3u, tag);
    ck_eq((uint32_t)u.face_cmd_target_dir, 0xA4A4u, tag);
    ck_eq((uint32_t)u.face_cmd_arg1, 0xA5A5u, tag);
    ck_eq((uint32_t)u.face_cmd_arg2, 0xA6A6u, tag);
    ck_eq((uint32_t)u.face_cmd_arg3, 0xA7A7u, tag);
    ck_eq((uint32_t)u.move_aborted_op, 0xA8u, tag);
    ck_eq((uint32_t)u.move_redirect_col, 0xA9u, tag);
    ck_eq((uint32_t)u.move_redirect_row, 0xAAu, tag);
    ck_eq((uint32_t)u.click_preview_facing, 0xABu, tag);
    char buf[160];
    for (int i = 0; i < 128; ++i) {
        std::snprintf(buf, sizeof(buf),
                      "%s: cmd_queue[%d].interrupt_flag UNTOUCHED (spawn never writes it)", tag, i);
        ck_eq((uint32_t)u.cmd_queue[i].interrupt_flag, (uint32_t)(uint8_t)(0x50 + i), buf);
    }
    // sprite_id checked separately (uint16_t, distinct message) -- folded in here for one call site.
    ck_eq((uint32_t)u.sprite_id, 0xBEEFu, tag);
}

// The command-queue write loop, 0x0042bcc2-0x0042bd48: all 128 entries' op/arg0..arg3 cleared,
// interrupt_flag left alone (checked separately by check_never_written).
void check_cmd_queue_cleared(const tact_unit &u, const char *tag) {
    char buf[160];
    for (int i = 0; i < 128; ++i) {
        const auto &e = u.cmd_queue[i];
        std::snprintf(buf, sizeof(buf), "%s: cmd_queue[%d].op cleared, 0x0042bce5", tag, i);
        ck_eq((uint32_t)e.op, 0u, buf);
        std::snprintf(buf, sizeof(buf), "%s: cmd_queue[%d].arg0 cleared, 0x0042bcfb", tag, i);
        ck_eq((uint32_t)e.arg0, 0u, buf);
        std::snprintf(buf, sizeof(buf), "%s: cmd_queue[%d].arg1 cleared, 0x0042bd11", tag, i);
        ck_eq((uint32_t)e.arg1, 0u, buf);
        std::snprintf(buf, sizeof(buf), "%s: cmd_queue[%d].arg2 cleared, 0x0042bd27", tag, i);
        ck_eq((uint32_t)e.arg2, 0u, buf);
        std::snprintf(buf, sizeof(buf), "%s: cmd_queue[%d].arg3 cleared, 0x0042bd3d", tag, i);
        ck_eq((uint32_t)e.arg3, 0u, buf);
    }
}

// The mirror of check_cmd_queue_cleared for the hp_pct-rollback path: the queue-clear loop
// (0x0042bcbb onward) is never reached, so every entry must still hold its seed_all_sentinels()
// value.
void check_cmd_queue_untouched(const tact_unit &u, const char *tag) {
    char buf[160];
    for (int i = 0; i < 128; ++i) {
        const auto &e = u.cmd_queue[i];
        std::snprintf(buf, sizeof(buf), "%s: cmd_queue[%d].op UNTOUCHED (rollback returns early)",
                      tag, i);
        ck_eq((uint32_t)e.op, (uint32_t)(uint16_t)(0x3000 + i), buf);
        std::snprintf(buf, sizeof(buf), "%s: cmd_queue[%d].arg0 UNTOUCHED (rollback returns early)",
                      tag, i);
        ck_eq((uint32_t)e.arg0, (uint32_t)(uint16_t)(0x4000 + i), buf);
        std::snprintf(buf, sizeof(buf), "%s: cmd_queue[%d].arg1 UNTOUCHED (rollback returns early)",
                      tag, i);
        ck_eq((uint32_t)e.arg1, (uint32_t)(uint16_t)(0x5000 + i), buf);
        std::snprintf(buf, sizeof(buf), "%s: cmd_queue[%d].arg2 UNTOUCHED (rollback returns early)",
                      tag, i);
        ck_eq((uint32_t)e.arg2, (uint32_t)(uint16_t)(0x6000 + i), buf);
        std::snprintf(buf, sizeof(buf), "%s: cmd_queue[%d].arg3 UNTOUCHED (rollback returns early)",
                      tag, i);
        ck_eq((uint32_t)e.arg3, (uint32_t)(uint16_t)(0x7000 + i), buf);
    }
}

// The "back half" -- everything from the hp guard (0x0042bb2f) onward -- as it must read on the
// hp_pct-rollback path: every one of these fields is skipped by the early `return 0` at 0x0042bb90,
// so each must still hold its seed_all_sentinels() value.
void check_back_half_untouched(const tact_unit &u, const char *tag) {
    ck_eq((uint32_t)u.hp, 0x2021u, tag);
    ck_eq((uint32_t)(uint16_t)u.move_retry_wait, 0x2223u, tag);
    ck_eq((uint32_t)(uint16_t)u.move_retry_attempts, 0x2425u, tag);
    ck_eq((uint32_t)(uint16_t)u.move_stuck_countdown, 0x2627u, tag);
    ck_eq((uint32_t)u.cmd_index, 0x28u, tag);
    ck_eq_d(u.anim_frame_time, 444.4, tag);
    ck_eq((uint32_t)u.frame_index, 0x1Au, tag);
    ck_eq_d(u.anim_cycle_time, 555.5, tag);
    ck_eq_d(u.frame_interval, 666.6, tag);
    ck_eq((uint32_t)u.active_gun, 0x39u, tag);
    ck_eq((uint32_t)u.gun1_bullets, 0x3Au, tag);
    ck_eq((uint32_t)u.gun2_bullets, 0x3Bu, tag);
    ck_eq((uint32_t)u.gun1_magazines, 0x3Cu, tag);
    ck_eq((uint32_t)u.gun2_magazines, 0x3Du, tag);
    ck_eq((uint32_t)u.attack_cmd_op, 0x2930u, tag);
    ck_eq((uint32_t)u.attack_gun_toggle, 0x3132u, tag);
    ck_eq((uint32_t)u.attack_cmd_arg1, 0x3334u, tag);
    ck_eq((uint32_t)u.aim_x, 0x3536u, tag);
    ck_eq((uint32_t)u.aim_y, 0x3738u, tag);
    ck_eq((uint32_t)u.anim_state, 0x14u, tag);
    ck_eq((uint32_t)u.squad_group_id, 0x3Eu, tag);
    check_cmd_queue_untouched(u, tag);
}

} // namespace

void run_unit_spawn_tests() {
    // ---- T1: initial guard -- occupied (PASSABLE_BLOCKED) spawn tile fatal-aborts, before the slot
    // search or any state write, 0x0042b969-0x0042b982. ----
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t COL = 5, ROW = 5;
        fx.planes().passable_at(COL, ROW)             = mh::state::PASSABLE_BLOCKED;
        fx.planes().tile_object_at(COL, ROW).building = 0x7777; // sentinel: must survive

        tact_store own = fx.store();
        int32_t    ret = detail::unit_spawn(fx.view(), own, rec_calls(), 9, COL, ROW, 1, 1, 50);

        ck_eq((uint32_t)ret, 0u, "T1: occupied tile aborts, returns 0, 0x0042b97b-0x0042b982");
        ck_eq((uint32_t)g_rec.fatal_cleanup_calls, 1u, "T1: llm_fatal_cleanup called once, 0x0042b97b");
        ck_eq((uint32_t)g_rec.abort_args.size(), 1u, "T1: utils_abort called once, 0x0042b982");
        if (g_rec.abort_args.size() == 1)
            ck_eq((uint32_t)g_rec.abort_args[0], 0u, "T1: utils_abort(0), 0x0042b980-0x0042b982");
        ck_eq((uint32_t)g_rec.time_calls, 0u, "T1: no time_GetCurrentTime call before the guard trips");
        ck_eq((uint32_t)g_rec.rand_calls, 0u, "T1: no llm_rand call before the guard trips");
        ck_eq((uint32_t)g_rec.vision_add_args.size(), 0u, "T1: unit_vision_add never called");
        ck_eq((uint32_t)fx.planes().tile_object_at(COL, ROW).building, 0x7777u,
              "T1: tile.building sentinel UNTOUCHED -- guard trips before any write");
        ck_eq((uint32_t)fx.units[1].type, 0u, "T1: slot search never runs -- units[1] untouched");
    }

    // ---- T2: slot search -- slot 0 is NEVER TESTED, even free: with 1..0x80 all occupied and slot
    // 0 free, no candidate is found and nothing is touched, 0x0042b987-0x0042b9c8. ----
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t COL = 6, ROW = 6;
        fx.planes().passable_at(COL, ROW) = mh::state::PASSABLE_DEFAULT; // not blocked
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) fx.units[i].type = 7;
        fx.units[0].type                              = 0;      // free, but slot 0 must never be tested (0x0042b987: i starts at 1)
        fx.planes().tile_object_at(COL, ROW).building = 0x8888; // sentinel: must survive
        fx.unit_active_count                          = 0x1234; // sentinel: must survive

        tact_store own = fx.store();
        int32_t    ret = detail::unit_spawn(fx.view(), own, rec_calls(), 1, COL, ROW, 0, 0, 50);

        ck_eq((uint32_t)ret, 0u,
              "T2: slot 0 skipped, 1..0x80 all occupied -> no candidate, return 0, 0x0042b9c8");
        ck_eq((uint32_t)g_rec.fatal_cleanup_calls, 0u,
              "T2: no-free-slot is a plain return, not a fatal-abort");
        ck_eq((uint32_t)g_rec.abort_args.size(), 0u, "T2: utils_abort never called");
        ck_eq((uint32_t)fx.planes().tile_object_at(COL, ROW).building, 0x8888u,
              "T2: tile.building UNTOUCHED -- no free slot means no write");
        ck_eq((uint32_t)fx.unit_active_count, 0x1234u, "T2: active count UNTOUCHED");
        ck_eq((uint32_t)fx.planes().passable_at(COL, ROW), (uint32_t)mh::state::PASSABLE_DEFAULT,
              "T2: passable UNTOUCHED");
    }

    // ---- T3: slot search -- occupied slots (type!=0) are skipped and the FIRST free slot in
    // [1,0x80] is chosen, 0x0042b995-0x0042b9c2 (free predicate at 0x0042b9af/0x0042b9b6). ----
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t COL = 7, ROW = 7, CHAR_TYPE = 2;
        fx.planes().passable_at(COL, ROW) = mh::state::PASSABLE_DEFAULT;
        for (int32_t i = 1; i <= 5; ++i) fx.units[i].type = 3;                   // occupied
        fx.units[6].type = 0;                                                    // the first free slot
        for (int32_t i = 7; i <= TACT_UNIT_LAST_SLOT; ++i) fx.units[i].type = 3; // occupied
        fx.character_types[CHAR_TYPE].id     = 1;
        fx.character_types[CHAR_TYPE].who    = 1;
        fx.character_types[CHAR_TYPE].energy = 100;
        g_rec.time_script                    = {1.0, 2.0, 3.0, 4.0};
        g_rec.rand_script                    = {0};

        tact_store own = fx.store();
        int32_t    ret = detail::unit_spawn(fx.view(), own, rec_calls(), CHAR_TYPE, COL, ROW, 0, 0, 50);

        ck_eq((uint32_t)ret, 6u, "T3: first free slot (6) chosen over occupied 1..5, 0x0042b9b8");
        ck_eq((uint32_t)fx.units[5].type, 3u, "T3: occupied slot 5 left alone");
        ck_eq((uint32_t)fx.units[7].type, 3u, "T3: occupied slot 7 left alone");
        ck_eq((uint32_t)fx.units[0].type, 0u, "T3: slot 0 never touched");
    }

    // ---- T4: slot search -- slot 0x80 is REACHABLE when 1..0x7f are all occupied (the loop's
    // upper bound, 0x0042b995: CMP i,0x80 / JLE, is inclusive, not an off-by-one at 0x7f). ----
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t COL = 8, ROW = 8, CHAR_TYPE = 2;
        fx.planes().passable_at(COL, ROW) = mh::state::PASSABLE_DEFAULT;
        for (int32_t i = 1; i <= 0x7f; ++i) fx.units[i].type = 4; // occupied
        fx.units[0x80].type                  = 0;                 // only free slot
        fx.character_types[CHAR_TYPE].id     = 1;
        fx.character_types[CHAR_TYPE].who    = 1;
        fx.character_types[CHAR_TYPE].energy = 100;
        g_rec.time_script                    = {1.0, 2.0, 3.0, 4.0};
        g_rec.rand_script                    = {0};

        tact_store own = fx.store();
        int32_t    ret = detail::unit_spawn(fx.view(), own, rec_calls(), CHAR_TYPE, COL, ROW, 0, 0, 50);

        ck_eq((uint32_t)ret, 0x80u, "T4: slot 0x80 reachable -- inclusive upper bound, 0x0042b99c");
    }

    // ---- T5: character-class guard -- a free slot IS found, but character_types[char_type].id==0
    // fatal-aborts BEFORE any roster/tile/count write, 0x0042b9d4-0x0042b9e8. ----
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t COL = 9, ROW = 9, CHAR_TYPE = 7; // fx.character_types[7].id == 0 (default)
        fx.planes().passable_at(COL, ROW)             = mh::state::PASSABLE_DEFAULT;
        fx.planes().tile_object_at(COL, ROW).building = 0x9494; // sentinel: must survive
        fx.unit_active_count                          = 0x5656; // sentinel: must survive

        tact_store own = fx.store();
        int32_t    ret = detail::unit_spawn(fx.view(), own, rec_calls(), CHAR_TYPE, COL, ROW, 0, 0, 50);

        ck_eq((uint32_t)ret, 0u, "T5: unfilled character class aborts, returns 0, 0x0042b9e1-0x0042b9e8");
        ck_eq((uint32_t)g_rec.fatal_cleanup_calls, 1u, "T5: llm_fatal_cleanup called once, 0x0042b9e1");
        ck_eq((uint32_t)g_rec.abort_args.size(), 1u, "T5: utils_abort called once, 0x0042b9e8");
        ck_eq((uint32_t)fx.units[1].type, 0u,
              "T5: slot 1 WAS found by the search but its .type is untouched -- abort precedes any write");
        ck_eq((uint32_t)fx.planes().tile_object_at(COL, ROW).building, 0x9494u, "T5: tile UNTOUCHED");
        ck_eq((uint32_t)fx.unit_active_count, 0x5656u, "T5: active count UNTOUCHED");
        ck_eq((uint32_t)g_rec.time_calls, 0u, "T5: no time_GetCurrentTime call");
        ck_eq((uint32_t)g_rec.rand_calls, 0u, "T5: no llm_rand call");
    }

    // ---- T6: the full successful spawn -- every field the .asm writes, the tile/count/passable
    // side effects, the "deliberately not initialised" survivors, and the exact llm_rand-jittered
    // frame_interval, all in one closure so a translation cannot pass by patching around a single
    // check. Slot is forced to 42 (occupying 1..41) so it is provably distinct from both the seeded
    // active-count sentinel (77) and 77+1 (78) -- see T6's active-count assertion. ----
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t COL = 11, ROW = 88, CHAR_TYPE = 4, SLOT = 42;
        constexpr uint8_t FACING_DIR = 0x0B, DEF_STAT = 0x16;
        constexpr int32_t HP_PCT = 100;                          // in-range UPPER boundary (case 6's second direction)
        for (int32_t i = 1; i < SLOT; ++i) fx.units[i].type = 1; // occupy 1..41
        seed_all_sentinels(fx.units[SLOT]);

        auto &ct                                      = fx.character_types[CHAR_TYPE];
        ct.id                                         = 1;
        ct.who                                        = 0x42;
        ct.angle_see                                  = 0x1234;
        ct.distance_see                               = 77;
        ct.energy                                     = 200;
        ct.number_gun1                                = 3;
        ct.number_gun2                                = 5;
        fx.fx_type_table[3].bullets                   = 21;
        fx.fx_type_table[3].magazines                 = 6;
        fx.fx_type_table[5].bullets                   = 33;
        fx.fx_type_table[5].magazines                 = 9;
        fx.unit_anim_frame_interval_base              = 3.5;
        fx.unit_active_count                          = 77; // distinct from SLOT(42) and SLOT+1(43)
        fx.planes().passable_at(COL, ROW)             = mh::state::PASSABLE_DEFAULT;
        fx.planes().tile_object_at(COL, ROW).building = 0x9999; // sentinel: must become SLOT

        g_rec.time_script = {1000.0, 2000.0, 3000.0, 4000.0}; // 4 SEPARATE, DISTINCT calls
        g_rec.rand_script = {26212};                          // 26212 / 0x1999(6553) == 4 exactly

        tact_store own = fx.store();
        int32_t    ret =
            detail::unit_spawn(fx.view(), own, rec_calls(), CHAR_TYPE, COL, ROW, FACING_DIR,
                               DEF_STAT, HP_PCT);
        const tact_unit &u = own.unit_at(SLOT);

        ck_eq((uint32_t)ret, (uint32_t)SLOT, "T6: return value is the chosen slot, 0x0042bdbf-0x0042bdc2");
        ck_eq((uint32_t)g_rec.fatal_cleanup_calls, 0u, "T6: no fatal-abort on the success path");
        ck_eq((uint32_t)g_rec.abort_args.size(), 0u, "T6: no fatal-abort on the success path");

        // -- identity/placement, 0x0042b9ed-0x0042ba8c --
        ck_eq((uint32_t)u.type, (uint32_t)CHAR_TYPE, "T6: type = char_type, 0x0042b9f7");
        ck_eq((uint32_t)u.owner, 0x42u, "T6: owner = character_types[ct].who, 0x0042ba0e");
        ck_eq((uint32_t)u.status, 0u, "T6: status = 0, 0x0042ba1b");
        ck_eq((uint32_t)u.pos_col, (uint32_t)COL, "T6: pos_col = col, 0x0042ba2c");
        ck_eq((uint32_t)u.pos_row, (uint32_t)ROW, "T6: pos_row = row, 0x0042ba3c");
        ck_eq((uint32_t)u.vision_angle, 0x1234u, "T6: vision_angle = character.angle_see, 0x0042ba54");
        ck_eq((uint32_t)u.vision_dist, 77u, "T6: vision_dist = character.distance_see, 0x0042ba6c");
        ck_eq((uint32_t)u.facing_dir, (uint32_t)FACING_DIR, "T6: facing_dir = param, 0x0042ba7c");
        ck_eq((uint32_t)u.def_stat, (uint32_t)DEF_STAT, "T6: def_stat = param, 0x0042ba8c");

        // -- the four DISTINCT time_GetCurrentTime() stamps, in call order, 0x0042ba92-0x0042bbff --
        ck_eq((uint32_t)g_rec.time_calls, 4u, "T6: exactly four time_GetCurrentTime() calls");
        ck_eq_d(u.move_state_timer, 1000.0, "T6: move_state_timer = call #1, 0x0042ba9e");
        ck_eq_d(u.wander_check_time, 2000.0, "T6: wander_check_time = call #2 (SEPARATE), 0x0042bab0");
        ck_eq_d(u.cmd_wait_until_time, 0.0, "T6: cmd_wait_until_time = 0.0 (two dword zero-stores), 0x0042babd/0x0042bac7");
        ck_eq_d(u.anim_frame_time, 3000.0, "T6: anim_frame_time = call #3, 0x0042bbdf");
        ck_eq_d(u.anim_cycle_time, 4000.0, "T6: anim_cycle_time = call #4, 0x0042bbff");

        // -- 0x0042bad8-0x0042baff --
        ck_eq((uint32_t)u.progress, 0u, "T6: progress = 0, 0x0042bad8");
        ck_eq((uint32_t)(uint16_t)u.move_path_slot, 0u, "T6: move_path_slot = 0, 0x0042bae6");
        ck_eq((uint32_t)(uint16_t)u.move_path_step, 0u, "T6: move_path_step = 0, 0x0042baf6");

        // -- the tile stamp: a WORD write of the SLOT INDEX at tile+0x2 (.building), 0x0042bb10 --
        ck_eq((uint32_t)fx.planes().tile_object_at(COL, ROW).building, (uint32_t)SLOT,
              "T6: tile.building (word at tile+0x2) = SLOT INDEX, 0x0042bb10");

        // -- the active-count is an ASSIGNMENT, not an increment: seeded 77, must read SLOT(42), not
        // 78 (increment) and not 77 (untouched), 0x0042bb1a --
        ck_eq((uint32_t)fx.unit_active_count, (uint32_t)SLOT,
              "T6: active_count = SLOT INDEX (assignment, NOT ++77==78, NOT left at 77), 0x0042bb1a");

        // -- passable, 0x0042bb28 --
        ck_eq((uint32_t)fx.planes().passable_at(COL, ROW), (uint32_t)mh::state::PASSABLE_BLOCKED,
              "T6: passable set to PASSABLE_BLOCKED(0) exactly, 0x0042bb28");

        // -- hp, in-range path, 0x0042bb3d-0x0042bb72: (200 * 100) / 100 = 200 --
        ck_eq((uint32_t)u.hp, 200u, "T6: hp = (energy*hp_pct)/100, 0x0042bb72");

        // -- back half, 0x0042bb95-0x0042bbf3 --
        ck_eq((uint32_t)(uint16_t)u.move_retry_wait, 0u, "T6: move_retry_wait = 0, 0x0042bb9c");
        ck_eq((uint32_t)(uint16_t)u.move_retry_attempts, 0u, "T6: move_retry_attempts = 0, 0x0042bbac");
        ck_eq((uint32_t)(uint16_t)u.move_stuck_countdown, 0u, "T6: move_stuck_countdown = 0, 0x0042bbbc");
        ck_eq((uint32_t)u.cmd_index, 0u, "T6: cmd_index = 0, 0x0042bbcc");
        ck_eq((uint32_t)u.frame_index, 0u, "T6: frame_index = 0, 0x0042bbec");

        // -- frame_interval = (llm_rand()/0x1999) + UNIT_ANIM_FRAME_INTERVAL_BASE, 0x0042bc05-0x0042bc2b --
        ck_eq((uint32_t)g_rec.rand_calls, 1u, "T6: llm_rand called exactly once");
        ck_eq_d(u.frame_interval, 7.5, "T6: frame_interval = (26212/6553=4) + 3.5 = 7.5, 0x0042bc2b");

        // -- gun setup, 0x0042bc31-0x0042bcb5 --
        ck_eq((uint32_t)u.active_gun, 0u, "T6: active_gun = 0, 0x0042bc54");
        ck_eq((uint32_t)u.gun1_bullets, 21u, "T6: gun1_bullets = fx_type_table[gun1].bullets, 0x0042bc6c");
        ck_eq((uint32_t)u.gun1_magazines, 5u, "T6: gun1_magazines = fx_type_table[gun1].magazines-1, 0x0042bc85");
        ck_eq((uint32_t)u.gun2_bullets, 33u, "T6: gun2_bullets = fx_type_table[gun2].bullets, 0x0042bc9c");
        ck_eq((uint32_t)u.gun2_magazines, 8u, "T6: gun2_magazines = fx_type_table[gun2].magazines-1, 0x0042bcb5");

        // -- the 128-entry cmd_queue clear, interrupt_flag untouched, 0x0042bcbb-0x0042bd48 --
        check_cmd_queue_cleared(u, "T6");

        // -- the immediate ATTACK/AIM record payload + anim_state + squad_group_id, 0x0042bd48-0x0042bdb4 --
        ck_eq((uint32_t)u.attack_cmd_op, 0u, "T6: attack_cmd_op = 0, 0x0042bd4f");
        ck_eq((uint32_t)u.attack_gun_toggle, 0u, "T6: attack_gun_toggle = 0, 0x0042bd5f");
        ck_eq((uint32_t)u.attack_cmd_arg1, 0u, "T6: attack_cmd_arg1 = 0, 0x0042bd6f");
        ck_eq((uint32_t)u.aim_x, 0u, "T6: aim_x = 0, 0x0042bd7f");
        ck_eq((uint32_t)u.aim_y, 0u, "T6: aim_y = 0, 0x0042bd8f");
        ck_eq((uint32_t)u.anim_state, 0u, "T6: anim_state = 0, 0x0042bd9f");
        ck_eq((uint32_t)u.squad_group_id, 0xffu, "T6: squad_group_id = 0xff (unassigned), 0x0042bdad");

        // -- the outward vision call, 0x0042bdb4-0x0042bdb7 --
        ck_eq((uint32_t)g_rec.vision_add_args.size(), 1u, "T6: unit_vision_add called exactly once");
        if (g_rec.vision_add_args.size() == 1)
            ck_eq((uint32_t)g_rec.vision_add_args[0], (uint32_t)SLOT,
                  "T6: unit_vision_add(SLOT), 0x0042bdb7");

        // -- the 13 fields (+ every cmd_queue.interrupt_flag) this function deliberately never writes --
        check_never_written(u, "T6");
    }

    // ---- T7: gun1_magazines wraps to 0xff when the source magazines byte is 0 -- a plain byte
    // DEC, not a saturating subtract, 0x0042bc7c-0x0042bc85. gun2 kept distinct/nonzero so a
    // translation that wrapped BOTH guns unconditionally would still fail here. ----
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t COL = 12, ROW = 13, CHAR_TYPE = 2;
        auto             &ct              = fx.character_types[CHAR_TYPE];
        ct.id                             = 1;
        ct.who                            = 1;
        ct.energy                         = 100;
        ct.number_gun1                    = 0;
        ct.number_gun2                    = 1;
        fx.fx_type_table[0].bullets       = 50;
        fx.fx_type_table[0].magazines     = 0; // wraps to 0xff
        fx.fx_type_table[1].bullets       = 60;
        fx.fx_type_table[1].magazines     = 4; // -1 = 3
        fx.planes().passable_at(COL, ROW) = mh::state::PASSABLE_DEFAULT;

        tact_store own = fx.store();
        int32_t    ret =
            detail::unit_spawn(fx.view(), own, rec_calls(), CHAR_TYPE, COL, ROW, 0, 0, 50);
        const tact_unit &u = own.unit_at((int32_t)ret);

        ck_eq((uint32_t)ret, 1u, "T7: spawn succeeds (slot 1)");
        ck_eq((uint32_t)u.gun1_bullets, 50u, "T7: gun1_bullets unaffected by the wrap");
        ck_eq((uint32_t)u.gun1_magazines, 0xffu, "T7: gun1_magazines wraps 0-1 -> 0xff, 0x0042bc7c-0x0042bc85");
        ck_eq((uint32_t)u.gun2_bullets, 60u, "T7: gun2_bullets");
        ck_eq((uint32_t)u.gun2_magazines, 3u, "T7: gun2_magazines = 4-1 = 3 (NOT wrapped)");
    }

    // ---- T8: hp derivation -- character_type.energy is int16_t but read via MOVZX (zero-extended
    // as unsigned 16-bit), 0x0042bb41. energy=-1 (bit pattern 0xffff) must read back as 65535, not
    // -1 -- a translation using MOVSX/plain int16_t widening would compute a NEGATIVE hp instead. ----
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t COL = 14, ROW = 15, CHAR_TYPE = 2;
        auto             &ct              = fx.character_types[CHAR_TYPE];
        ct.id                             = 1;
        ct.who                            = 1;
        ct.energy                         = -1; // bit pattern 0xffff
        fx.planes().passable_at(COL, ROW) = mh::state::PASSABLE_DEFAULT;

        tact_store own = fx.store();
        int32_t    ret =
            detail::unit_spawn(fx.view(), own, rec_calls(), CHAR_TYPE, COL, ROW, 0, 0, 50);
        const tact_unit &u = own.unit_at((int32_t)ret);

        // energy_zx = (uint16_t)(-1) = 65535; hp = (65535*50)/100 = 3276750/100 = 32767 (IDIV
        // truncates toward zero; both operands non-negative here so this equals plain floor).
        ck_eq((uint32_t)ret, 1u, "T8: spawn succeeds");
        ck_eq((uint32_t)u.hp, 32767u,
              "T8: hp = (zero-extended 0xffff * 50)/100 = 32767, NOT a signed-negative result, 0x0042bb41-0x0042bb72");
    }

    // ---- T9: hp floored at 1 when the division truncates to 0, AND the in-range LOW boundary
    // (hp_pct==1) does NOT roll back -- both directions of case 6's "1..100 inclusive" range,
    // 0x0042bb2f-0x0042bb39 (JLE bb3b) and 0x0042bb5b-0x0042bb61 (floor). ----
    {
        tact_fixture fx;
        g_rec.reset();
        constexpr int32_t COL = 16, ROW = 17, CHAR_TYPE = 2;
        auto             &ct              = fx.character_types[CHAR_TYPE];
        ct.id                             = 1;
        ct.who                            = 1;
        ct.energy                         = 1;
        fx.planes().passable_at(COL, ROW) = mh::state::PASSABLE_DEFAULT;

        tact_store own = fx.store();
        int32_t    ret =
            detail::unit_spawn(fx.view(), own, rec_calls(), CHAR_TYPE, COL, ROW, 0, 0, /*hp_pct=*/1);
        const tact_unit &u = own.unit_at((int32_t)ret);

        ck_eq((uint32_t)ret, 1u, "T9: hp_pct=1 is IN RANGE -- spawn succeeds, does not roll back");
        ck_eq((uint32_t)u.type, (uint32_t)CHAR_TYPE, "T9: type NOT rolled back at the low boundary");
        ck_eq((uint32_t)u.hp, 1u, "T9: hp = (1*1)/100 = 0, floored to 1, 0x0042bb5b-0x0042bb61");
    }

    // ---- T10/T11: hp_pct OUT OF [1,100] rolls back ONLY .type; the tile stamp, passable,
    // active-count, and every roster field already written before the guard (0x0042bb2f) STAY.
    // Every field only reachable AFTER the guard (hp, move_retry_*, cmd_index, anim_*,
    // frame_interval, gun setup, the whole cmd_queue, attack/aim, anim_state, squad_group_id) is
    // left at its pre-call sentinel because the early return at 0x0042bb90 never reaches them.
    // Both directions: hp_pct=0 (below) and hp_pct=101 (above). ----
    {
        struct rollback_case {
            int32_t     hp_pct;
            int32_t     col, row;
            const char *tag;
        };
        const rollback_case cases[] = {
            {0, 21, 66, "T10 (hp_pct=0, below range)"},
            {101, 33, 99, "T11 (hp_pct=101, above range)"},
        };
        for (const auto &tc : cases) {
            tact_fixture fx;
            g_rec.reset();
            constexpr int32_t CHAR_TYPE  = 6;
            constexpr uint8_t FACING_DIR = 0x2C, DEF_STAT = 0x2D;
            seed_all_sentinels(fx.units[1]); // default fixture -> slot search picks slot 1

            auto &ct             = fx.character_types[CHAR_TYPE];
            ct.id                = 1;
            ct.who               = 0x42;
            ct.angle_see         = 0x1234;
            ct.distance_see      = 77;
            fx.unit_active_count = 77; // same "distinct" sentinel as T6 -- still an assignment, not
                                       // an increment, even on the rollback path (0x0042bb1a
                                       // precedes the 0x0042bb2f guard).
            fx.planes().passable_at(tc.col, tc.row)             = mh::state::PASSABLE_DEFAULT;
            fx.planes().tile_object_at(tc.col, tc.row).building = 0x5555;
            g_rec.time_script                                   = {5000.0, 6000.0}; // only move_state_timer/wander_check_time fire

            tact_store own = fx.store();
            int32_t    ret =
                detail::unit_spawn(fx.view(), own, rec_calls(), CHAR_TYPE, tc.col, tc.row,
                                   FACING_DIR, DEF_STAT, tc.hp_pct);
            const tact_unit &u = own.unit_at(1);

            const char *tag = tc.tag;

            ck_eq((uint32_t)ret, 0u, tag);    // "hp_pct out of range -> return 0"
            ck_eq((uint32_t)u.type, 0u, tag); // ".type rolled back to 0, 0x0042bb82"

            // -- fields written BEFORE the guard: STAY (not rolled back) --
            ck_eq((uint32_t)u.owner, 0x42u, tag);
            ck_eq((uint32_t)u.status, 0u, tag);
            ck_eq((uint32_t)u.pos_col, (uint32_t)tc.col, tag);
            ck_eq((uint32_t)u.pos_row, (uint32_t)tc.row, tag);
            ck_eq((uint32_t)u.vision_angle, 0x1234u, tag);
            ck_eq((uint32_t)u.vision_dist, 77u, tag);
            ck_eq((uint32_t)u.facing_dir, (uint32_t)FACING_DIR, tag);
            ck_eq((uint32_t)u.def_stat, (uint32_t)DEF_STAT, tag);
            ck_eq_d(u.move_state_timer, 5000.0, tag);
            ck_eq_d(u.wander_check_time, 6000.0, tag);
            ck_eq_d(u.cmd_wait_until_time, 0.0, tag);
            ck_eq((uint32_t)u.progress, 0u, tag);
            ck_eq((uint32_t)(uint16_t)u.move_path_slot, 0u, tag);
            ck_eq((uint32_t)(uint16_t)u.move_path_step, 0u, tag);
            ck_eq((uint32_t)fx.planes().tile_object_at(tc.col, tc.row).building, 1u, tag); // slot 1
            ck_eq((uint32_t)fx.unit_active_count, 1u, tag);                                // assignment, not 77, not 78
            ck_eq((uint32_t)fx.planes().passable_at(tc.col, tc.row),
                  (uint32_t)mh::state::PASSABLE_BLOCKED, tag);

            // -- everything reachable only AFTER the guard: never reached, still the seed --
            check_back_half_untouched(u, tag);
            check_never_written(u, tag);

            ck_eq((uint32_t)g_rec.time_calls, 2u, tag);             // only the two pre-guard timestamps
            ck_eq((uint32_t)g_rec.rand_calls, 0u, tag);             // llm_rand never reached
            ck_eq((uint32_t)g_rec.vision_add_args.size(), 0u, tag); // unit_vision_add never reached
            ck_eq((uint32_t)g_rec.fatal_cleanup_calls, 0u, tag);    // this is not a fatal-abort path
        }
    }
}

} // namespace mh::tact::test
