//
// sim_unit_state_exit_storage_begin_selftest.cpp -- `simtest` oracle for
// llm_strat_unit_state_exit_storage_begin @0x0047efc6 (sim/sim_unit_state_exit.h/.cpp, RI-SIM /
// SIM1-G3).
//
// EXPECTED BEHAVIOUR from the header banner's own derivation (sim_unit_state_exit.h):
//   0x0047efee-0x0047f003: can_exit = storage_can_exit(player, unit_index, storage_slot).
//   can_exit == 0 (still blocked):
//     0x0047f0b7-0x0047f199: reserved_passengers = 0; if the storage's owning building's CFG type
//       (cfg_buildings[b.building_id].type) is H_SHUTTLE(0x21)/A_SHUTTLE(0x0d), reserved_passengers =
//       prod_shuttle_slots[player][b.shuttle_slot].passengers_reserved.
//     0x0047f19c-0x0047f23b: if (player == PlayerSide) AND (population[player].human +
//       reserved_passengers < cfg_units[unit_proto_id].human) AND (cfg_units[unit_proto_id].
//       soldier_count < 1): race_alert_text_emit() (no args).
//     0x0047f240-0x0047f266: unconditionally: unit_storage[...].door_waiter_count += 1;
//       unit_set_state(EXIT_WAIT=0x22). No door_mutex_unit write on this path.
//   can_exit != 0:
//     0x0047f009-0x0047f026: unit_storage[...].door_mutex_unit = unit_index -- BEFORE the
//       ground/aircraft branch below (order-significant: the callee reads it back out).
//     0x0047f02c-0x0047f042: if cfg_units[unit_proto_id].type <= UNIT_TYPE_A_GROUND(0xe):
//       storage_place_exit_ground(player, unit_index, storage_slot); return. (CORRECTION in the
//       header banner: this compares .type, not a "capacity" field.)
//     else (aircraft-class, type > 0xe): free_slot = path_find_free_slot(player);
//       free_slot < 0 (0x0047f053 JLE -1) -> door_waiter_count += 1, unit_set_state(EXIT_WAIT) (SAME
//         tail as the can_exit==0 path); no storage_exit_air call.
//       free_slot >= 0 -> storage_exit_air(player, unit_index, storage_slot, free_slot); no
//         door_waiter_count/set_state touch.
//
#include "sim/sim_unit_state_exit.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- storage_can_exit -------------------------------------------------------------------------
int32_t g_can_exit_result = 0;
struct can_exit_call {
    int32_t  player;
    uint32_t unit_index;
    int32_t  storage_slot;
};
std::vector<can_exit_call> g_can_exit_calls;
int32_t                    rec_storage_can_exit(int32_t player, uint32_t unit_index, int32_t storage_slot) {
    g_can_exit_calls.push_back({player, unit_index, storage_slot});
    return g_can_exit_result;
}

// ---- storage_place_exit_ground -----------------------------------------------------------------
// g_watch_storage lets a case observe door_mutex_unit AT THE MOMENT this callee runs, which is what
// proves 0x0047f026's write happens BEFORE the ground/air branch rather than after or not at all.
unit_storage *g_watch_storage          = nullptr;
int32_t       g_door_mutex_seen_ground = -999;
struct ground_call {
    uint16_t player;
    int32_t  unit_index;
    int32_t  storage_slot;
};
std::vector<ground_call> g_ground_calls;
void                     rec_storage_place_exit_ground(uint16_t player, int32_t unit_index, int32_t storage_slot) {
    g_ground_calls.push_back({player, unit_index, storage_slot});
    if (g_watch_storage) g_door_mutex_seen_ground = g_watch_storage->door_mutex_unit;
}

// ---- path_find_free_slot -----------------------------------------------------------------------
int32_t g_free_slot_result = 0;
struct free_slot_call {
    int32_t player;
};
std::vector<free_slot_call> g_free_slot_calls;
int32_t                     rec_path_find_free_slot(int32_t player) {
    g_free_slot_calls.push_back({player});
    return g_free_slot_result;
}

// ---- storage_exit_air --------------------------------------------------------------------------
int32_t g_door_mutex_seen_air = -999;
struct air_call {
    uint16_t player;
    int32_t  unit_index;
    int32_t  storage_slot;
    uint32_t free_slot;
};
std::vector<air_call> g_air_calls;
void                  rec_storage_exit_air(uint16_t player, int32_t unit_index, int32_t storage_slot,
                                           uint32_t free_slot) {
    g_air_calls.push_back({player, unit_index, storage_slot, free_slot});
    if (g_watch_storage) g_door_mutex_seen_air = g_watch_storage->door_mutex_unit;
}

// ---- unit_set_state ------------------------------------------------------------------------------
std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) { g_set_state_calls.push_back(new_state); }

// ---- race_alert_text_emit --------------------------------------------------------------------------
int32_t g_race_alert_calls = 0;
void    rec_race_alert_text_emit() { ++g_race_alert_calls; }

// The four callees this function never reaches (storage_type_accepts_unit/dir_step_factor/
// map_fow_UpdateFoWPlus/ai_group_member_count_adjust/unit_set_state_order all belong to the OTHER
// three state handlers in this TU) are left null -- any invocation of one would be its own bug
// signal (a null-pointer call), not a silently-accepted stub.
const unit_state_exit_calls g_calls = {
    &rec_storage_can_exit,
    &rec_storage_place_exit_ground,
    &rec_path_find_free_slot,
    &rec_storage_exit_air,
    &rec_unit_set_state,
    &rec_race_alert_text_emit,
    nullptr, // storage_type_accepts_unit -- exit_walk_out only
    nullptr, // dir_step_factor -- exit_walk_out only
    nullptr, // map_fow_UpdateFoWPlus -- exit_walk_out only
    nullptr, // ai_group_member_count_adjust -- exit_walk_out only
    nullptr, // unit_set_state_order -- exit_walk_out only
};

constexpr uint16_t PLAYER       = 3;
constexpr int32_t  UNIT_INDEX   = 9;
constexpr int32_t  STORAGE_SLOT = 4;
constexpr int32_t  B_INDEX      = 6;
constexpr uint16_t BLDG_CFG_ID  = 10;
constexpr uint16_t UNIT_PROTO   = 20;

void reset_recorders() {
    g_can_exit_result = 0;
    g_can_exit_calls.clear();
    g_watch_storage          = nullptr;
    g_door_mutex_seen_ground = -999;
    g_ground_calls.clear();
    g_free_slot_result = 0;
    g_free_slot_calls.clear();
    g_door_mutex_seen_air = -999;
    g_air_calls.clear();
    g_set_state_calls.clear();
    g_race_alert_calls = 0;
}

// Builds a unit parked at STORAGE_SLOT/B_INDEX, seeds door_waiter_count/door_mutex_unit with
// distinct non-default sentinels (so "+= 1" vs "= 1" and "written" vs "left alone" are both
// observable), and points g_watch_storage at the same slot for the ordering checks above.
unit &make_unit(sim_fixture &fx) {
    unit &u             = fx.u(PLAYER, UNIT_INDEX);
    u.home_storage_slot = static_cast<uint8_t>(STORAGE_SLOT);
    u.unit_proto_id     = UNIT_PROTO;

    unit_storage &st     = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
    st.b_index           = B_INDEX;
    st.door_waiter_count = 3;    // distinct nonzero seed -- proves += 1, not = 1
    st.door_mutex_unit   = -777; // sentinel distinct from UNIT_INDEX/PLAYER/STORAGE_SLOT/B_INDEX

    building &b   = fx.b(PLAYER, B_INDEX);
    b.building_id = BLDG_CFG_ID;

    fx.cfg_buildings[BLDG_CFG_ID].type = 0; // default: not a shuttle -- a case that needs one sets it

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = PLAYER;
    fx.view_cur_index  = UNIT_INDEX;
    g_watch_storage    = &st;
    return u;
}

} // namespace

void run_unit_state_exit_storage_begin_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- can_exit==0, no housing alert (player != PlayerSide): door_waiter_count += 1 (3 -> 4),
    // unit_set_state(EXIT_WAIT), race_alert_text_emit NEVER called, door_mutex_unit left untouched
    // (this path never claims the door), ground/air/free-slot callees never invoked.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u           = make_unit(fx);
        fx.player_side    = static_cast<int16_t>(PLAYER + 50); // deliberate mismatch
        g_can_exit_result = 0;
        // Population/human/soldier fields set so the OTHER two conditions would fire the alert if the
        // player check were missing -- isolates the player==PlayerSide gate specifically.
        fx.population[PLAYER].human            = 1;
        fx.cfg_units[UNIT_PROTO].human         = 100;
        fx.cfg_units[UNIT_PROTO].soldier_count = 0;

        sim_store own = fx.store();
        detail::unit_state_exit_storage_begin(fx.view(), own, g_calls);

        ck((int32_t)g_can_exit_calls.size() == 1 && g_can_exit_calls[0].player == PLAYER &&
               g_can_exit_calls[0].unit_index == (uint32_t)UNIT_INDEX &&
               g_can_exit_calls[0].storage_slot == STORAGE_SLOT,
           "T1: storage_can_exit(player, unit_index, storage_slot) called once with the right args, 0x0047efee");
        ck_eq((uint32_t)g_race_alert_calls, 0u,
              "T1: race_alert_text_emit NOT called -- player != PlayerSide gate, 0x0047f1a9");
        ck((g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x22),
           "T1: unit_set_state(EXIT_WAIT=0x22) on can_exit==0, 0x0047f261");
        ck_eq((uint32_t)fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT].door_waiter_count, 4u,
              "T1: door_waiter_count += 1 (3 -> 4) on can_exit==0, 0x0047f256-0x0047f25c");
        ck_eq((uint32_t)(int32_t)fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT].door_mutex_unit,
              (uint32_t)-777,
              "T1: door_mutex_unit left untouched on can_exit==0 -- only the can_exit!=0 arm writes it");
        ck_eq((uint32_t)g_ground_calls.size(), 0u, "T1: storage_place_exit_ground never called on can_exit==0");
        ck_eq((uint32_t)g_air_calls.size(), 0u, "T1: storage_exit_air never called on can_exit==0");
        ck_eq((uint32_t)g_free_slot_calls.size(), 0u, "T1: path_find_free_slot never called on can_exit==0");
        (void)u;
    }

    // =================================================================================================
    // T2 -- can_exit==0, all three housing-alert conditions true (player==PlayerSide, population.human
    // + reserved(0, non-shuttle) < cfg.human, cfg.soldier_count < 1): race_alert_text_emit fires once,
    // then the same door_waiter_count/EXIT_WAIT tail as T1.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_unit(fx);
        fx.player_side                         = static_cast<int16_t>(PLAYER); // match
        g_can_exit_result                      = 0;
        fx.population[PLAYER].human            = 2;
        fx.cfg_units[UNIT_PROTO].human         = 5; // 2 + 0 < 5
        fx.cfg_units[UNIT_PROTO].soldier_count = 0; // < 1

        sim_store own = fx.store();
        detail::unit_state_exit_storage_begin(fx.view(), own, g_calls);

        ck_eq((uint32_t)g_race_alert_calls, 1u,
              "T2: race_alert_text_emit fires -- all three housing-alert conditions true, 0x0047f23b");
        ck_eq((uint32_t)fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT].door_waiter_count, 4u,
              "T2: door_waiter_count += 1 still runs after the alert, 0x0047f256");
    }

    // =================================================================================================
    // T3 -- can_exit==0, player matches and population is short, but cfg.soldier_count >= 1: alert
    // gate's THIRD condition alone suppresses the alert.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_unit(fx);
        fx.player_side                         = static_cast<int16_t>(PLAYER);
        g_can_exit_result                      = 0;
        fx.population[PLAYER].human            = 2;
        fx.cfg_units[UNIT_PROTO].human         = 5; // still short
        fx.cfg_units[UNIT_PROTO].soldier_count = 1; // NOT < 1 -- gate fails

        sim_store own = fx.store();
        detail::unit_state_exit_storage_begin(fx.view(), own, g_calls);

        ck_eq((uint32_t)g_race_alert_calls, 0u,
              "T3: race_alert_text_emit NOT called -- soldier_count(1) not < 1, 0x0047f221-0x0047f22c");
    }

    // =================================================================================================
    // T4 -- can_exit==0, player matches and soldier_count < 1, but population.human + reserved is NOT
    // less than cfg.human (equal, not strictly less): alert gate's SECOND condition alone suppresses.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_unit(fx);
        fx.player_side                         = static_cast<int16_t>(PLAYER);
        g_can_exit_result                      = 0;
        fx.population[PLAYER].human            = 5;
        fx.cfg_units[UNIT_PROTO].human         = 5; // 5 + 0 < 5 is FALSE (equal)
        fx.cfg_units[UNIT_PROTO].soldier_count = 0;

        sim_store own = fx.store();
        detail::unit_state_exit_storage_begin(fx.view(), own, g_calls);

        ck_eq((uint32_t)g_race_alert_calls, 0u,
              "T4: race_alert_text_emit NOT called -- population.human+reserved(5) not < cfg.human(5), 0x0047f1e7");
    }

    // =================================================================================================
    // T5 -- can_exit==0, H_SHUTTLE(0x21) building: reserved_passengers is pulled from
    // prod_shuttle_slots[player][b.shuttle_slot].passengers_reserved and ADDED before the comparison,
    // large enough to flip a would-fire alert into NOT firing. Pins both the shuttle-type read gate
    // and the addition order.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_unit(fx);
        fx.player_side                                                                        = static_cast<int16_t>(PLAYER);
        g_can_exit_result                                                                     = 0;
        fx.cfg_buildings[BLDG_CFG_ID].type                                                    = 0x21; // BUILDING_TYPE_H_SHUTTLE
        fx.b(PLAYER, B_INDEX).shuttle_slot                                                    = 2;
        fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + 2].passengers_reserved = 10;
        fx.population[PLAYER].human                                                           = 1;
        fx.cfg_units[UNIT_PROTO].human                                                        = 5; // 1 alone < 5 (would fire without reserved)
        fx.cfg_units[UNIT_PROTO].soldier_count                                                = 0;

        sim_store own = fx.store();
        detail::unit_state_exit_storage_begin(fx.view(), own, g_calls);

        ck_eq((uint32_t)g_race_alert_calls, 0u,
              "T5: H_SHUTTLE reserved_passengers(10) added -- 1+10 not < 5, alert suppressed, 0x0047f148-0x0047f1e7");
    }

    // =================================================================================================
    // T6 -- can_exit==0, A_SHUTTLE(0x0d) building (the OTHER shuttle type value), reserved_passengers
    // explicitly 0: alert fires, confirming A_SHUTTLE is also recognised by the type gate (not just
    // H_SHUTTLE) and that a 0 reservation does not spuriously suppress.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_unit(fx);
        fx.player_side                                                                        = static_cast<int16_t>(PLAYER);
        g_can_exit_result                                                                     = 0;
        fx.cfg_buildings[BLDG_CFG_ID].type                                                    = 0x0d; // BUILDING_TYPE_A_SHUTTLE
        fx.b(PLAYER, B_INDEX).shuttle_slot                                                    = 1;
        fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + 1].passengers_reserved = 0;
        fx.population[PLAYER].human                                                           = 1;
        fx.cfg_units[UNIT_PROTO].human                                                        = 5;
        fx.cfg_units[UNIT_PROTO].soldier_count                                                = 0;

        sim_store own = fx.store();
        detail::unit_state_exit_storage_begin(fx.view(), own, g_calls);

        ck_eq((uint32_t)g_race_alert_calls, 1u,
              "T6: A_SHUTTLE also gates the reserved-passengers read, 0-reservation does not suppress, 0x0047f0fa");
    }

    // =================================================================================================
    // T7 -- can_exit==0, NON-shuttle building type with a large passengers_reserved sitting at the
    // SAME slot index a shuttle building would read: the type gate must exclude it, so the alert fires
    // as if reserved_passengers were 0, proving the read is gated on building type, not unconditional.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_unit(fx);
        fx.player_side                                                                        = static_cast<int16_t>(PLAYER);
        g_can_exit_result                                                                     = 0;
        fx.cfg_buildings[BLDG_CFG_ID].type                                                    = 0x05; // NOT a shuttle type
        fx.b(PLAYER, B_INDEX).shuttle_slot                                                    = 3;
        fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + 3].passengers_reserved = 1000;
        fx.population[PLAYER].human                                                           = 1;
        fx.cfg_units[UNIT_PROTO].human                                                        = 5;
        fx.cfg_units[UNIT_PROTO].soldier_count                                                = 0;

        sim_store own = fx.store();
        detail::unit_state_exit_storage_begin(fx.view(), own, g_calls);

        ck_eq((uint32_t)g_race_alert_calls, 1u,
              "T7: non-shuttle building type -- prod_shuttle_slots NOT read, alert fires unshielded, 0x0047f0fa-0x0047f146");
    }

    // =================================================================================================
    // T8 -- can_exit != 0, cfg type == UNIT_TYPE_A_GROUND (0xe, the ground-side boundary): door_mutex_
    // unit is set BEFORE storage_place_exit_ground runs (order check via g_watch_storage), the ground
    // callee fires with the right args, and door_waiter_count/set_state are left untouched (the
    // ground/air success paths never reach the wait tail).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_unit(fx);
        g_can_exit_result             = 1;    // can_exit != 0
        fx.cfg_units[UNIT_PROTO].type = 0x0e; // == UNIT_TYPE_A_GROUND -- ground boundary, low side

        sim_store own = fx.store();
        detail::unit_state_exit_storage_begin(fx.view(), own, g_calls);

        ck((g_ground_calls.size() == 1 && g_ground_calls[0].player == PLAYER &&
            g_ground_calls[0].unit_index == UNIT_INDEX && g_ground_calls[0].storage_slot == STORAGE_SLOT),
           "T8: storage_place_exit_ground(player, unit_index, storage_slot) called on type==0xe, 0x0047f0ad");
        ck_eq((uint32_t)g_door_mutex_seen_ground, (uint32_t)UNIT_INDEX,
              "T8: door_mutex_unit already == unit_index INSIDE the ground callee -- set BEFORE the branch, 0x0047f026");
        ck_eq((uint32_t)(int32_t)fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT].door_mutex_unit,
              (uint32_t)UNIT_INDEX, "T8: door_mutex_unit == unit_index after the call, 0x0047f026");
        ck_eq((uint32_t)fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT].door_waiter_count, 3u,
              "T8: door_waiter_count untouched on the ground-exit success path");
        ck_eq((uint32_t)g_set_state_calls.size(), 0u, "T8: unit_set_state never called on the ground-exit success path");
        ck_eq((uint32_t)g_air_calls.size(), 0u, "T8: storage_exit_air never called on the ground path");
        ck_eq((uint32_t)g_free_slot_calls.size(), 0u,
              "T8: path_find_free_slot never called for a ground-class unit, 0x0047f042 JLE taken");
    }

    // =================================================================================================
    // T9 -- can_exit != 0, cfg type == 0xf (one past UNIT_TYPE_A_GROUND, the aircraft-side boundary):
    // routes to the aircraft arm (path_find_free_slot called, ground NOT called). free_slot >= 0 (5)
    // -> storage_exit_air fires with door_mutex_unit already set (order check).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_unit(fx);
        g_can_exit_result             = 1;
        fx.cfg_units[UNIT_PROTO].type = 0x0f; // aircraft boundary, high side
        g_free_slot_result            = 5;

        sim_store own = fx.store();
        detail::unit_state_exit_storage_begin(fx.view(), own, g_calls);

        ck_eq((uint32_t)g_ground_calls.size(), 0u,
              "T9: storage_place_exit_ground NOT called on type==0xf, 0x0047f042 JLE not taken");
        ck((g_free_slot_calls.size() == 1 && g_free_slot_calls[0].player == PLAYER),
           "T9: path_find_free_slot(player) called for an aircraft-class unit, 0x0047f04b");
        ck((g_air_calls.size() == 1 && g_air_calls[0].player == PLAYER &&
            g_air_calls[0].unit_index == UNIT_INDEX && g_air_calls[0].storage_slot == STORAGE_SLOT &&
            g_air_calls[0].free_slot == 5u),
           "T9: storage_exit_air(player, unit_index, storage_slot, free_slot=5) called, 0x0047f06d");
        ck_eq((uint32_t)g_door_mutex_seen_air, (uint32_t)UNIT_INDEX,
              "T9: door_mutex_unit already == unit_index INSIDE the air callee -- set BEFORE the branch, 0x0047f026");
        ck_eq((uint32_t)fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT].door_waiter_count, 3u,
              "T9: door_waiter_count untouched on the air-exit success path");
        ck_eq((uint32_t)g_set_state_calls.size(), 0u, "T9: unit_set_state never called on the air-exit success path");
    }

    // =================================================================================================
    // T10 -- can_exit != 0, aircraft-class, free_slot == 0: the sentinel-side boundary of "free_slot <
    // 0" -- 0 is NOT negative, so storage_exit_air must still fire (not the door-wait fallback).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_unit(fx);
        g_can_exit_result             = 1;
        fx.cfg_units[UNIT_PROTO].type = 0x18; // a high aircraft-family type value
        g_free_slot_result            = 0;

        sim_store own = fx.store();
        detail::unit_state_exit_storage_begin(fx.view(), own, g_calls);

        ck((g_air_calls.size() == 1 && g_air_calls[0].free_slot == 0u),
           "T10: free_slot==0 still calls storage_exit_air (0 is not < 0), 0x0047f057 JLE not taken");
        ck_eq((uint32_t)g_set_state_calls.size(), 0u, "T10: no door-wait fallback on free_slot==0");
        ck_eq((uint32_t)fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT].door_waiter_count, 3u,
              "T10: door_waiter_count untouched on free_slot==0's air-exit success path");
    }

    // =================================================================================================
    // T11 -- can_exit != 0, aircraft-class, free_slot == -1: the OTHER side of the same boundary --
    // storage_exit_air must NOT fire; falls back to the same door_waiter_count+=1 / EXIT_WAIT tail the
    // can_exit==0 path uses, even though door_mutex_unit WAS already claimed for this unit.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_unit(fx);
        g_can_exit_result             = 1;
        fx.cfg_units[UNIT_PROTO].type = 0x18;
        g_free_slot_result            = -1;

        sim_store own = fx.store();
        detail::unit_state_exit_storage_begin(fx.view(), own, g_calls);

        ck_eq((uint32_t)g_air_calls.size(), 0u,
              "T11: free_slot==-1 does NOT call storage_exit_air, 0x0047f057 JLE taken");
        ck((g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x22),
           "T11: unit_set_state(EXIT_WAIT=0x22) on the no-free-slot fallback, 0x0047f095");
        ck_eq((uint32_t)fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT].door_waiter_count, 4u,
              "T11: door_waiter_count += 1 (3 -> 4) on the no-free-slot fallback, 0x0047f08a");
        ck_eq((uint32_t)(int32_t)fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT].door_mutex_unit,
              (uint32_t)UNIT_INDEX,
              "T11: door_mutex_unit stays claimed (set at 0x0047f026, never released on this fallback)");
        ck_eq((uint32_t)g_race_alert_calls, 0u,
              "T11: race_alert_text_emit NOT called on the can_exit!=0 fallback (that gate is can_exit==0 only)");
    }
}

} // namespace mh::sim::test
