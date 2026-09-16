#include "sim/sim_unit_state_takeoff.h"

#include <string>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- ORDER-sensitive shared log ------------------------------------------------------------------
// Every recorder below also pushes a short tag here, in call order, so a case can pin the SEQUENCE
// the .asm walks (e.g. the landing-complete dock arm: unlink -> dock_list_append -> ai_adjust ->
// set_event -> fow_remove_sight -> set_state), not just which calls happened.
std::vector<std::string> g_call_log;

std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) {
    g_set_state_calls.push_back(new_state);
    g_call_log.push_back("set_state:" + std::to_string(new_state));
}

struct unlink_call {
    uint32_t player;
    uint16_t unit_index;
};
std::vector<unlink_call> g_unlink_calls;
void                     rec_unit_unlink_tile(uint32_t unit_player, uint16_t unit_index) {
    g_unlink_calls.push_back({unit_player, unit_index});
    g_call_log.push_back("unlink_tile");
}

struct dock_list_append_call {
    uint16_t player;
    uint16_t unit_idx;
    uint16_t storage_slot;
};
std::vector<dock_list_append_call> g_dock_list_append_calls;
void                               rec_storage_dock_list_append(uint16_t player, uint16_t unit_idx, uint16_t storage_slot) {
    g_dock_list_append_calls.push_back({player, unit_idx, storage_slot});
    g_call_log.push_back("dock_list_append");
}

struct ai_adjust_call {
    uint32_t player;
    uint32_t unit_index;
    uint32_t group_or_type;
    uint32_t mode;
};
std::vector<ai_adjust_call> g_ai_adjust_calls;
void                        rec_ai_group_member_count_adjust(uint32_t player, uint32_t unit_index, uint32_t group_or_type,
                                                             uint32_t mode) {
    g_ai_adjust_calls.push_back({player, unit_index, group_or_type, mode});
    g_call_log.push_back("ai_adjust:" + std::to_string(mode));
}

int32_t  g_set_event_calls     = 0;
uint32_t g_set_event_last_type = 0;
uint32_t rec_game_SetEvent(uint32_t type) {
    ++g_set_event_calls;
    g_set_event_last_type = type;
    g_call_log.push_back("set_event:" + std::to_string(type));
    return 0;
}

struct fow_remove_sight_call {
    uint32_t player;
    int32_t  x;
    int32_t  y;
    uint8_t  radius;
};
std::vector<fow_remove_sight_call> g_fow_remove_sight_calls;
void                               rec_fow_remove_sight(uint32_t player, int32_t x, int32_t y, uint8_t radius) {
    g_fow_remove_sight_calls.push_back({player, x, y, radius});
    g_call_log.push_back("fow_remove_sight");
}

struct put_on_map_call {
    uint16_t player;
    uint16_t b_id;
    uint8_t  x;
    uint8_t  y;
};
std::vector<put_on_map_call> g_put_on_map_calls;
void                         rec_map_unit_PutOnMap(uint16_t player, uint16_t b_id, uint8_t x, uint8_t y) {
    g_put_on_map_calls.push_back({player, b_id, x, y});
    g_call_log.push_back("put_on_map");
}

struct update_fow_plus_call {
    uint32_t player;
    uint32_t x;
    uint32_t y;
    uint8_t  sight;
};
std::vector<update_fow_plus_call> g_update_fow_plus_calls;
void                              rec_map_fow_UpdateFoWPlus(uint32_t player, uint32_t x, uint32_t y, uint8_t sight) {
    g_update_fow_plus_calls.push_back({player, x, y, sight});
    g_call_log.push_back("update_fow_plus");
}

// unit_state_takeoff_landing never calls this (only its sibling unit_takeoff_finalize does) -- it is
// here only because unit_state_takeoff_calls is one struct shared by both functions. A call landing
// here would itself be a finding.
int32_t g_remove_docked_unit_calls = 0;
void    rec_storage_remove_docked_unit(uint16_t, int32_t, int32_t) {
    ++g_remove_docked_unit_calls;
    g_call_log.push_back("remove_docked_unit(UNEXPECTED)");
}

const unit_state_takeoff_calls g_calls = {
    &rec_unit_set_state,
    &rec_unit_unlink_tile,
    &rec_storage_dock_list_append,
    &rec_ai_group_member_count_adjust,
    &rec_game_SetEvent,
    &rec_fow_remove_sight,
    &rec_map_unit_PutOnMap,
    &rec_map_fow_UpdateFoWPlus,
    &rec_storage_remove_docked_unit,
};

constexpr uint16_t PLAYER       = 2;
constexpr int32_t  UNIT_INDEX   = 11;
constexpr uint16_t PROTO        = 5;
constexpr int32_t  STORAGE_SLOT = 7;
constexpr int32_t  B_INDEX      = 9;  // building roster slot (storage.b_index)
constexpr int32_t  CFG_BID      = 13; // cfg building TYPE index (buildings[..].building_id)

void reset_recorders() {
    g_call_log.clear();
    g_set_state_calls.clear();
    g_unlink_calls.clear();
    g_dock_list_append_calls.clear();
    g_ai_adjust_calls.clear();
    g_set_event_calls     = 0;
    g_set_event_last_type = 0;
    g_fow_remove_sight_calls.clear();
    g_put_on_map_calls.clear();
    g_update_fow_plus_calls.clear();
    g_remove_docked_unit_calls = 0;
}

// Common wiring every case needs: CUR_UNIT/_PLAYER/_INDEX ambient globals, the storage->building
// chain the landing-complete arm walks, and the two DECLARED-NEED boot constants this function reads
// unconditionally on its very first FMUL (see the report at the bottom of this file: neither field
// exists in sim_fixture yet).
unit &make_unit(sim_fixture &fx) {
    unit &u             = fx.u(PLAYER, UNIT_INDEX);
    u.unit_proto_id     = PROTO;
    u.home_storage_slot = static_cast<uint8_t>(STORAGE_SLOT);

    unit_storage &st = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
    st.b_index       = B_INDEX;

    building &b   = fx.b(PLAYER, B_INDEX);
    b.building_id = static_cast<uint16_t>(CFG_BID);

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = PLAYER;
    fx.view_cur_index  = UNIT_INDEX;

    // FIXTURE GAP (see the report at the bottom of this file): sim_test_support.h has no
    // takeoff_landing_step_cost_scale / takeoff_landing_a_helipad_activity_bump members yet, nor a
    // view() binding for either. Real values (sim_state.cpp / this file's header banner): 2.0 / 3.0.
    fx.takeoff_landing_step_cost_scale         = 2.0;
    fx.takeoff_landing_a_helipad_activity_bump = 3.0;

    return u;
}

} // namespace

void run_unit_state_takeoff_landing_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- insufficient tick budget drains and returns BEFORE the state dispatch even looks at
    // u.state -- seeded with state==TAKEOFF and a would-be-mutated elevation to prove the ORDER: the
    // budget gate short-circuits ahead of any dispatch, so elevation must NOT move even though the
    // state value alone would otherwise climb it.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                                = make_unit(fx);
        u.state                                = UNIT_STATE_TAKEOFF;
        u.elevation                            = 100;
        fx.cfg_units[PROTO].step_speed[PLAYER] = 3.0; // cost = 3.0*2.0 = 6.0
        fx.tick_budget                         = 2.0; // < 6.0
        u.activity_clock                       = 50.0;

        sim_store own = fx.store();
        detail::unit_state_takeoff_landing(fx.view(), own, g_calls);

        ck_eq_d(u.activity_clock, 50.0 - 2.0,
                "T1: activity_clock -= the pre-drain tick_budget, 0x0048144c-0x0048144f");
        ck_eq_d(fx.tick_budget, 0.0, "T1: tick_budget zeroed, 0x00481452-0x00481466");
        ck_eq((uint32_t)u.elevation, 100u,
              "T1: elevation untouched -- budget gate returns BEFORE state dispatch even though "
              "state==TAKEOFF, ordering of 0x0048143f vs 0x0048147a");
        ck(g_call_log.empty(), "T1: no callee fires on the insufficient-budget path");
    }

    // =================================================================================================
    // T2 -- sufficient budget, else-dispatch (state<0x15, the JC path @0x0048148b), helicopter-class
    // unit type: ALL FOUR type values the .asm's four CMPs converge on skip the taxi animation
    // entirely -- no calls, no field mutation past the budget subtraction.
    // =================================================================================================
    {
        const uint32_t heli_types[] = {UNIT_TYPE_A_HELI, UNIT_TYPE_H_HELI, UNIT_TYPE_A_HELI_CARGO,
                                       UNIT_TYPE_H_HELI_CARGO};
        for (uint32_t t : heli_types) {
            fx.reset();
            reset_recorders();
            unit &u                                = make_unit(fx);
            u.state                                = 0x10; // <0x15 -- JC else-dispatch, 0x0048148b
            fx.cfg_units[PROTO].step_speed[PLAYER] = 3.0;
            fx.cfg_units[PROTO].type               = t;
            fx.tick_budget                         = 20.0;
            u.move_heading                         = 1;
            u.move_microstep                       = 5;
            u.x                                    = 44;
            u.y                                    = 55;

            sim_store own = fx.store();
            detail::unit_state_takeoff_landing(fx.view(), own, g_calls);

            ck_eq_d(fx.tick_budget, 20.0 - 6.0,
                    "T2: tick_budget -= cost still happens before the type check, 0x0048146b");
            const std::string heli_msg = "T2: heli-class type (" + std::to_string(t) +
                                         ") skips the taxi animation entirely, 0x004816f4-0x00481758";
            ck(g_call_log.empty(), heli_msg.c_str());
            ck_eq((uint32_t)u.move_microstep, 5u, "T2: move_microstep untouched on the heli-skip path");
            ck_eq((uint32_t)u.x, 44u, "T2: x untouched on the heli-skip path");
        }
    }

    // =================================================================================================
    // T3 -- sufficient budget, else-dispatch (state<0x15, JC path), non-heli type, mid sub-tile
    // animation (move_microstep<0x1f): microstep advances, BOTH facing bytes come from the SAME
    // table entry at the POST-increment index (a decoy at the OLD index proves the wrong-index case
    // would fail), no callee fires, x/y untouched.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                                                    = make_unit(fx);
        u.state                                                    = 0x10; // <0x15, JC else-dispatch
        fx.cfg_units[PROTO].step_speed[PLAYER]                     = 3.0;
        fx.cfg_units[PROTO].type                                   = 1; // non-heli
        fx.tick_budget                                             = 20.0;
        u.move_heading                                             = 3;
        u.move_microstep                                           = 10;
        u.x                                                        = 77;
        u.y                                                        = 88;
        fx.move_microsteps[3 * MICROSTEPS_PER_HEADING + 11].facing = 111; // POST-increment index (10+1)
        fx.move_microsteps[3 * MICROSTEPS_PER_HEADING + 0].facing  = 222; // decoy, must NOT be read

        sim_store own = fx.store();
        detail::unit_state_takeoff_landing(fx.view(), own, g_calls);

        ck_eq_d(fx.tick_budget, 20.0 - 6.0, "T3: tick_budget -= cost, 0x0048146b-0x00481474");
        ck_eq((uint32_t)u.move_microstep, 11u, "T3: move_microstep incremented (10 -> 11), 0x0048176b");
        ck_eq((uint32_t)u.facing_target, 111u,
              "T3: facing_target = move_microsteps[heading][11].facing (POST-increment idx), "
              "0x0048179a-0x004817a0");
        ck_eq((uint32_t)u.facing_current, 111u,
              "T3: facing_current = the SAME table entry, two separate writes, 0x004817c7-0x004817cd");
        ck_eq((uint32_t)u.x, 77u, "T3: x untouched on the mid-microstep taxi path");
        ck_eq((uint32_t)u.y, 88u, "T3: y untouched on the mid-microstep taxi path");
        ck(g_call_log.empty(), "T3: no callee fires on the mid-microstep taxi path");
    }

    // =================================================================================================
    // T4 -- sufficient budget, else-dispatch (state>0x16, the direct JMP path @0x0048149b), non-heli
    // type, SETTLED sub-tile animation (move_microstep==0x1f): unlink -> fow_remove_sight at the OLD
    // (x,y) -> PutOnMap at the NEW (x,y) -> UpdateFoWPlus at the NEW (x,y), in that order; the new
    // tile is computed via dir_step_offsets (signed dx/dy) masked by width_mask/height_mask (TWO
    // DIFFERENT masks) with an actual wraparound; move_microstep resets to 0 and facing resamples
    // from the NEW (zero) index, NOT the settled (0x1f) one a decoy at that index would catch.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                                = make_unit(fx);
        u.state                                = 0x22; // >0x16, direct-JMP else-dispatch, 0x0048149b
        fx.cfg_units[PROTO].step_speed[PLAYER] = 3.0;
        fx.cfg_units[PROTO].type               = 2; // non-heli
        fx.cfg_units[PROTO].sight              = 9;
        fx.tick_budget                         = 20.0;
        u.move_heading                         = 4;
        u.move_microstep                       = 0x1f;
        u.x                                    = 250;
        u.y                                    = 60;
        fx.dir_step_offsets[4].dx              = 10;
        fx.dir_step_offsets[4].dy              = 5;
        // geom.width_mask=0xff, geom.height_mask=0x3f (sim_fixture::reset() defaults):
        // new_col = (250+10) & 0xff = 260 & 0xff = 4 (wraps); new_row = (60+5) & 0x3f = 65 & 0x3f = 1 (wraps)
        fx.move_microsteps[4 * MICROSTEPS_PER_HEADING + 0].facing  = 77;  // resample index (microstep=0)
        fx.move_microsteps[4 * MICROSTEPS_PER_HEADING + 31].facing = 199; // decoy at the OLD (settled) idx

        sim_store own = fx.store();
        detail::unit_state_takeoff_landing(fx.view(), own, g_calls);

        ck_eq_d(fx.tick_budget, 20.0 - 6.0, "T4: tick_budget -= cost, 0x0048146b-0x00481474");
        ck(g_call_log.size() == 4 && g_call_log[0] == "unlink_tile" &&
               g_call_log[1] == "fow_remove_sight" && g_call_log[2] == "put_on_map" &&
               g_call_log[3] == "update_fow_plus",
           "T4: ORDER unlink_tile -> fow_remove_sight(OLD) -> put_on_map(NEW) -> update_fow_plus(NEW), "
           "0x0048182e/0x00481856/0x00481893/0x004818d9");
        ck(g_fow_remove_sight_calls.size() == 1 && g_fow_remove_sight_calls[0].x == 250 &&
               g_fow_remove_sight_calls[0].y == 60 && g_fow_remove_sight_calls[0].radius == 9,
           "T4: fow_remove_sight uses the OLD (x,y) before the move, 0x00481842-0x00481856");
        ck_eq((uint32_t)u.x, 4u, "T4: x = (250+10) & width_mask(0xff) = 4, wraps, 0x00481861-0x0048186c");
        ck_eq((uint32_t)u.y, 1u, "T4: y = (60+5) & height_mask(0x3f) = 1, wraps (DIFFERENT mask), 0x00481872-0x0048187a");
        ck(g_put_on_map_calls.size() == 1 && g_put_on_map_calls[0].x == 4 && g_put_on_map_calls[0].y == 1,
           "T4: PutOnMap gets the NEW (x,y), 0x0048187d-0x00481893");
        ck(g_update_fow_plus_calls.size() == 1 && g_update_fow_plus_calls[0].x == 4 &&
               g_update_fow_plus_calls[0].y == 1 && g_update_fow_plus_calls[0].sight == 9,
           "T4: UpdateFoWPlus gets the NEW (x,y) and cfg sight, 0x004818c5-0x004818d9");
        ck_eq((uint32_t)u.move_microstep, 0u, "T4: move_microstep reset to 0, 0x004818de-0x004818e3");
        ck_eq((uint32_t)u.move_heading, 4u,
              "T4: move_heading re-affirmed to the SAME value, 0x004818ed-0x004818f6");
        ck_eq((uint32_t)u.facing_target, 77u,
              "T4: facing resampled from move_microsteps[heading][0] -- the NEW microstep, not the "
              "settled 0x1f decoy, 0x00481920-0x00481926");
        ck_eq((uint32_t)u.facing_current, 77u, "T4: facing_current the SAME resample, 0x0048194d-0x00481953");
    }

    // =================================================================================================
    // T5 -- TAKEOFF (state==0x15), elevation increments but stays strictly BELOW the per-proto
    // ceiling (a SIGNED, negative-side boundary: -6+1=-5 < -3): no clamp, no unit_set_state -- falls
    // into the taxi continuation instead (proven by the microstep/facing mutation actually firing).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                                                   = make_unit(fx);
        u.state                                                   = UNIT_STATE_TAKEOFF;
        u.elevation                                               = -6;
        fx.cfg_units[PROTO].elevation                             = -3; // ceiling, signed
        fx.cfg_units[PROTO].step_speed[PLAYER]                    = 3.0;
        fx.cfg_units[PROTO].type                                  = 1; // non-heli, so taxi_continue is observable
        fx.tick_budget                                            = 20.0;
        u.order                                                   = 0x77; // must NOT be consumed on this path
        u.move_heading                                            = 2;
        u.move_microstep                                          = 0;
        fx.move_microsteps[2 * MICROSTEPS_PER_HEADING + 1].facing = 55;

        sim_store own = fx.store();
        detail::unit_state_takeoff_landing(fx.view(), own, g_calls);

        ck_eq((uint32_t)u.elevation, (uint32_t)-5,
              "T5: elevation += 1 (-6 -> -5), still < ceiling(-3), signed JL, 0x004814aa-0x004814cb");
        ck(g_set_state_calls.empty(),
           "T5: no clamp/unit_set_state -- elevation has not reached the ceiling yet");
        ck_eq((uint32_t)u.move_microstep, 1u,
              "T5: falls into the taxi continuation (ORIGIN 2 of 3), 0x004814cb JL taken -> 0x004816f4");
        ck_eq((uint32_t)u.facing_target, 55u, "T5: taxi continuation actually ran and set facing");
    }

    // =================================================================================================
    // T6 -- TAKEOFF, elevation reaches the ceiling EXACTLY (the other side of T5's boundary:
    // -4+1=-3 == ceiling(-3)): clamp to the ceiling, unit_set_state(u.order) -- the unit's OWN queued
    // order value, NOT a literal -- and return WITHOUT entering the taxi continuation at all (sentinel
    // microstep/facing must stay untouched).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                                                   = make_unit(fx);
        u.state                                                   = UNIT_STATE_TAKEOFF;
        u.elevation                                               = -4;
        fx.cfg_units[PROTO].elevation                             = -3; // ceiling, boundary EXACT after the increment
        fx.cfg_units[PROTO].step_speed[PLAYER]                    = 3.0;
        fx.tick_budget                                            = 20.0;
        u.order                                                   = 0x77;
        u.move_heading                                            = 2;
        u.move_microstep                                          = 0;  // sentinel: must stay 0 (taxi_continue NOT entered)
        fx.move_microsteps[2 * MICROSTEPS_PER_HEADING + 1].facing = 55; // decoy, must NOT apply

        sim_store own = fx.store();
        detail::unit_state_takeoff_landing(fx.view(), own, g_calls);

        ck_eq((uint32_t)u.elevation, (uint32_t)-3,
              "T6: elevation clamped to the ceiling exactly at the boundary, 0x004814cd-0x004814d6");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x77,
           "T6: unit_set_state(u.order=0x77) -- u.order, NOT a literal, 0x004814f0-0x004814f4");
        ck_eq((uint32_t)u.move_microstep, 0u,
              "T6: taxi continuation NOT entered on the clamp path -- microstep untouched");
        ck_eq((uint32_t)u.facing_target, 0u,
              "T6: facing_target untouched (fixture default) -- taxi continuation not entered");
    }

    // =================================================================================================
    // T7 -- LANDING (state==0x16), elevation decrements but stays strictly ABOVE the per-proto floor
    // (signed boundary, positive side: 8-1=7 > 3): no dock sequence, no pop bookkeeping -- falls into
    // the taxi continuation instead (ORIGIN 3 of 3).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                                                   = make_unit(fx);
        u.state                                                   = UNIT_STATE_LANDING;
        u.elevation                                               = 8;
        fx.cfg_units[PROTO].elevation_2                           = 3; // floor, signed
        fx.cfg_units[PROTO].step_speed[PLAYER]                    = 3.0;
        fx.cfg_units[PROTO].type                                  = 1; // non-heli
        fx.tick_budget                                            = 20.0;
        fx.population[PLAYER].human                               = 40;
        fx.population[PLAYER].human_in_field                      = 90;
        u.move_heading                                            = 6;
        u.move_microstep                                          = 0;
        fx.move_microsteps[6 * MICROSTEPS_PER_HEADING + 1].facing = 66;

        sim_store own = fx.store();
        detail::unit_state_takeoff_landing(fx.view(), own, g_calls);

        ck_eq((uint32_t)u.elevation, 7u,
              "T7: elevation -= 1 (8 -> 7), still > floor(3), signed JG, 0x00481508-0x00481529");
        ck(g_call_log.empty(), "T7: no dock callee fires -- floor not yet reached");
        ck_eq((uint32_t)fx.population[PLAYER].human, 40u, "T7: pop.human untouched -- dock arm not reached");
        ck_eq((uint32_t)fx.population[PLAYER].human_in_field, 90u,
              "T7: pop.human_in_field untouched -- dock arm not reached");
        ck_eq((uint32_t)u.move_microstep, 1u,
              "T7: falls into the taxi continuation (ORIGIN 3 of 3), 0x00481529 JG taken -> 0x004816f4");
        ck_eq((uint32_t)u.facing_target, 66u, "T7: taxi continuation actually ran and set facing");
    }

    // =================================================================================================
    // T8 -- LANDING complete (elevation reaches the floor EXACTLY: 4-1=3==floor(3)), docking at an
    // H_HELIPAD(0x1e) building: full dock sequence in ORDER (unlink -> dock_list_append -> ai_adjust
    // (mode=1u, the OPPOSITE literal from unit_takeoff_finalize's mode=0u) -> game_SetEvent(7u)
    // UNCONDITIONALLY -> fow_remove_sight) then unit_set_state(0x2b) and an IMMEDIATE return -- the
    // H_HELIPAD arm never even reaches the A_HELIPAD/activity_clock-bump check (sentinel activity_clock
    // must stay untouched). pop.human/.human_in_field seeded asymmetrically so a swapped +=/-= fails.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                                = make_unit(fx);
        u.state                                = UNIT_STATE_LANDING;
        u.elevation                            = 4;
        fx.cfg_units[PROTO].elevation_2        = 3; // floor, boundary EXACT after the decrement
        fx.cfg_units[PROTO].step_speed[PLAYER] = 3.0;
        fx.cfg_units[PROTO].human              = 6;
        fx.cfg_units[PROTO].sight              = 9;
        fx.tick_budget                         = 20.0;
        u.x                                    = 111;
        u.y                                    = 122;
        u.activity_clock                       = 17.5; // sentinel: must stay untouched (H_HELIPAD arm)
        fx.population[PLAYER].human            = 40;
        fx.population[PLAYER].human_in_field   = 90;
        fx.cfg_buildings[CFG_BID].type         = BUILDING_TYPE_H_HELIPAD;

        sim_store own = fx.store();
        detail::unit_state_takeoff_landing(fx.view(), own, g_calls);

        ck_eq((uint32_t)u.elevation, 3u, "T8: elevation clamped to the floor exactly, 0x0048152f-0x0048154a");
        ck_eq((uint32_t)fx.population[PLAYER].human, 46u,
              "T8: pop.human += cfg_units[proto].human (40+6), 0x0048154d-0x0048156d");
        ck_eq((uint32_t)fx.population[PLAYER].human_in_field, 84u,
              "T8: pop.human_in_field -= the SAME cfg human value (90-6), 0x00481573-0x00481593");
        ck(g_call_log.size() == 6 && g_call_log[0] == "unlink_tile" &&
               g_call_log[1] == "dock_list_append" && g_call_log[2] == "ai_adjust:1" &&
               g_call_log[3] == "set_event:7" && g_call_log[4] == "fow_remove_sight" &&
               g_call_log[5] == "set_state:43",
           "T8: ORDER unlink -> dock_list_append -> ai_adjust(mode=1) -> set_event(7) -> "
           "fow_remove_sight -> set_state(0x2b=43), 0x004815a7-0x00481685");
        ck(g_dock_list_append_calls.size() == 1 && g_dock_list_append_calls[0].storage_slot == STORAGE_SLOT,
           "T8: storage_dock_list_append(player, index, home_storage_slot=7), 0x004815ca");
        ck(g_ai_adjust_calls.size() == 1 && g_ai_adjust_calls[0].group_or_type == (uint32_t)STORAGE_SLOT &&
               g_ai_adjust_calls[0].mode == 1u,
           "T8: ai_group_member_count_adjust(..., storage_slot=7, mode=1u), 0x004815e5");
        ck_eq(g_set_event_last_type, 7u, "T8: game_SetEvent(7u) fires UNCONDITIONALLY, 0x004815ea-0x004815ef");
        ck(g_fow_remove_sight_calls.size() == 1 && g_fow_remove_sight_calls[0].x == 111 &&
               g_fow_remove_sight_calls[0].y == 122 && g_fow_remove_sight_calls[0].radius == 9,
           "T8: fow_remove_sight(player, u.x=111, u.y=122, cfg sight=9), 0x00481603-0x00481629");
        ck_eq_d(u.activity_clock, 17.5,
                "T8: activity_clock untouched -- H_HELIPAD returns before the bump check ever runs, "
                "0x0048167b-0x00481685");
        ck(g_put_on_map_calls.empty() && g_update_fow_plus_calls.empty(),
           "T8: PutOnMap/UpdateFoWPlus belong to the taxi continuation, not the dock-complete arm");
    }

    // =================================================================================================
    // T9 -- LANDING complete, docking at a building whose type is NEITHER H_HELIPAD(0x1e) NOR
    // A_HELIPAD(0xa): unit_set_state(0x2a) fires, but activity_clock does NOT bump (sentinel stays
    // untouched) -- isolates the plain-dock arm from T10's A_HELIPAD bump below.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                                = make_unit(fx);
        u.state                                = UNIT_STATE_LANDING;
        u.elevation                            = 2;
        fx.cfg_units[PROTO].elevation_2        = 1; // floor, boundary EXACT (2-1=1==1)
        fx.cfg_units[PROTO].step_speed[PLAYER] = 3.0;
        fx.cfg_units[PROTO].human              = 6;
        fx.cfg_units[PROTO].sight              = 9;
        fx.tick_budget                         = 20.0;
        u.activity_clock                       = 17.5; // sentinel: must stay untouched (no bump)
        fx.population[PLAYER].human            = 40;
        fx.population[PLAYER].human_in_field   = 90;
        fx.cfg_buildings[CFG_BID].type         = 5; // neither H_HELIPAD(0x1e) nor A_HELIPAD(0xa)

        sim_store own = fx.store();
        detail::unit_state_takeoff_landing(fx.view(), own, g_calls);

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_DOCK_TAXI_A,
           "T9: unit_set_state(0x2a=DOCK_TAXI_A), building type is neither helipad, 0x00481687-0x0048168c");
        ck_eq_d(u.activity_clock, 17.5,
                "T9: activity_clock untouched -- building type != A_HELIPAD, no bump, 0x004816d5-0x004816dc");
    }

    // =================================================================================================
    // T10 -- LANDING complete, docking at an A_HELIPAD(0xa) building (the SAME building the storage
    // slot resolves to as T9, different cfg type): unit_set_state(0x2a) fires (same as T9) AND, this
    // time, activity_clock += the DECLARED-NEED boot-constant bump (3.0) -- the additional effect T9's
    // seed proves is NOT unconditional.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u                                = make_unit(fx);
        u.state                                = UNIT_STATE_LANDING;
        u.elevation                            = 2;
        fx.cfg_units[PROTO].elevation_2        = 1; // floor, boundary EXACT (2-1=1==1)
        fx.cfg_units[PROTO].step_speed[PLAYER] = 3.0;
        fx.cfg_units[PROTO].human              = 6;
        fx.cfg_units[PROTO].sight              = 9;
        fx.tick_budget                         = 20.0;
        u.activity_clock                       = 17.5;
        fx.population[PLAYER].human            = 40;
        fx.population[PLAYER].human_in_field   = 90;
        fx.cfg_buildings[CFG_BID].type         = BUILDING_TYPE_A_HELIPAD;

        sim_store own = fx.store();
        detail::unit_state_takeoff_landing(fx.view(), own, g_calls);

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_DOCK_TAXI_A,
           "T10: unit_set_state(0x2a=DOCK_TAXI_A) fires once regardless of the extra A_HELIPAD check, "
           "0x00481687-0x0048168c");
        ck_eq_d(u.activity_clock, 17.5 + 3.0,
                "T10: activity_clock += takeoff_landing_a_helipad_activity_bump(3.0), literal FLD/FADD/"
                "FSTP term order, 0x004816de-0x004816ec");
    }
}

} // namespace mh::sim::test

// =====================================================================================================
// REPORT TO THE CONDUCTOR
// =====================================================================================================
//
// file written: src/mh_dll/libmh_test/sim_unit_state_takeoff_landing_selftest.cpp
// run_* name:   mh::sim::test::run_unit_state_takeoff_landing_tests()
//
// FIXTURE FIELDS NEEDED THAT sim_test_support.h DOES NOT HAVE YET (load-bearing -- every single test
// case calls unit_state_takeoff_landing, which dereferences BOTH pointers unconditionally on its very
// first FMUL at 0x0048142a, before any branch; without these two fields every case null-derefs):
//   1. `double takeoff_landing_step_cost_scale;`         -- add to sim_fixture, seed 2.0 in the ctor/
//      reset() (real read-memory-confirmed value, DAT_00501428), bind
//      `v.takeoff_landing_step_cost_scale = &takeoff_landing_step_cost_scale;` in view().
//   2. `double takeoff_landing_a_helipad_activity_bump;` -- add to sim_fixture, seed 3.0 (DAT_00501430),
//      bind `v.takeoff_landing_a_helipad_activity_bump = &takeoff_landing_a_helipad_activity_bump;` in
//      view().
//   Both are already declared on sim_view (sim_state.h lines 923-924) and already referenced by
//   sim_unit_state_takeoff.cpp's live path; only the sim_fixture side (test-only) is missing, same
//   shape as every other `taxi_takeoff_step_speed_mult`-style boot constant already in the fixture.
//   This file's make_unit() sets fx.takeoff_landing_step_cost_scale / fx.takeoff_landing_a_helipad_
//   activity_bump directly (matching sim_test_support.h's own naming convention for the sibling
//   TU's taxi_takeoff_step_speed_mult) -- it will not compile until the conductor adds them.
//
// .cpp vs .asm: NO DIVERGENCE FOUND. Traced every branch (TICK_BUDGET gate, the three-way state
// dispatch including both the JC and the direct-JMP paths to the same else target, both the TAKEOFF
// and LANDING elevation clamps, the full landing-complete dock sequence including the H_HELIPAD/
// A_HELIPAD building-type checks and the activity_clock bump, and all three origins into the taxi
// continuation including the four-way helicopter-type skip, the mid-microstep increment, and the
// settled/step-one-tile arm) against the raw instruction stream; every field offset, call argument
// order, and literal (including the mode=1u vs the sibling's mode=0u, and the H_HELIPAD(0x1e)/
// A_HELIPAD(0xa) building-type constants) matches the .cpp exactly. The two "double re-computation
// of the same table index" idioms (facing_target/facing_current written via two independent index
// recomputations in the .asm, once each) and the "re-affirm move_heading with its own just-read
// value" no-op are cosmetic Watcom codegen shape, not behavioural divergence -- the .cpp's single
// cached `src` local / single `u.move_heading = heading` assignment are equivalent.
//
// UNRESOLVED FROM THE LISTING: none. Every address cited above was walked to a concrete instruction;
// no jump target was ambiguous and no field offset needed a guess beyond what the header's own
// CORRECTION notes already settled.
//
// PER-CASE SUMMARY (see the case bodies above for the full address citations):
//   T1  -- insufficient tick_budget drains + returns BEFORE state dispatch (ORDER), 0x00481409-0x00481466/0x0048147a
//   T2  -- all four heli-class unit types skip the taxi continuation entirely, 0x004816f4-0x00481758
//   T3  -- mid-microstep: increment + facing_target/current from the SAME post-increment table entry, 0x0048175d-0x004817d0
//   T4  -- settled: unlink->fow_remove_sight(OLD)->PutOnMap(NEW)->UpdateFoWPlus(NEW) in order, wrap via TWO masks, 0x004817d5-0x00481953
//   T5  -- TAKEOFF below ceiling (signed, negative side) -> taxi continuation, no clamp, 0x004814a5-0x004814cb
//   T6  -- TAKEOFF at ceiling (signed boundary) -> clamp + unit_set_state(u.order, not a literal), 0x004814cd-0x004814f9
//   T7  -- LANDING above floor (signed, positive side) -> taxi continuation, no dock, 0x00481503-0x00481529
//   T8  -- LANDING at floor, H_HELIPAD dock: full ordered call sequence, mode=1u, immediate return before the bump check, 0x0048152f-0x00481685
//   T9  -- LANDING at floor, non-helipad dock: set_state(0x2a), no bump, 0x00481687-0x004816dc
//   T10 -- LANDING at floor, A_HELIPAD dock: set_state(0x2a) AND activity_clock += bump(3.0), 0x004816de-0x004816ec
