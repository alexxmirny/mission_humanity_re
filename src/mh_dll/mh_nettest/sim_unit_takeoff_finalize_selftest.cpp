#include "sim/sim_unit_state_takeoff.h"

#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

struct RemoveDockedCall {
    uint16_t player;
    int32_t  unit_index;
    int32_t  storage_slot;
};
std::vector<RemoveDockedCall> g_remove_docked_calls;
void                          rec_storage_remove_docked_unit(uint16_t player, int32_t unit_index, int32_t storage_slot) {
    tr("storage_remove_docked_unit");
    g_remove_docked_calls.push_back({player, unit_index, storage_slot});
}

struct PutOnMapCall {
    uint16_t player;
    uint16_t b_id;
    uint8_t  x, y;
};
std::vector<PutOnMapCall> g_put_on_map_calls;
void                      rec_map_unit_PutOnMap(uint16_t player, uint16_t b_id, uint8_t x, uint8_t y) {
    tr("map_unit_PutOnMap");
    g_put_on_map_calls.push_back({player, b_id, x, y});
}

struct FowUpdateCall {
    uint32_t player, x, y;
    uint8_t  sight;
};
std::vector<FowUpdateCall> g_fow_update_calls;
void                       rec_map_fow_UpdateFoWPlus(uint32_t player, uint32_t x, uint32_t y, uint8_t sight) {
    tr("map_fow_UpdateFoWPlus");
    g_fow_update_calls.push_back({player, x, y, sight});
}

struct GroupAdjustCall {
    uint32_t player, unit_index, group_or_type, mode;
};
std::vector<GroupAdjustCall> g_group_adjust_calls;
void                         rec_ai_group_member_count_adjust(uint32_t player, uint32_t unit_index, uint32_t group_or_type,
                                                              uint32_t mode) {
    tr("ai_group_member_count_adjust");
    g_group_adjust_calls.push_back({player, unit_index, group_or_type, mode});
}

const unit_state_takeoff_calls g_calls = {
    nullptr, // unit_set_state -- unreachable from this function
    nullptr, // unit_unlink_tile
    nullptr, // storage_dock_list_append
    &rec_ai_group_member_count_adjust,
    nullptr, // game_SetEvent
    nullptr, // fow_remove_sight
    &rec_map_unit_PutOnMap,
    &rec_map_fow_UpdateFoWPlus,
    &rec_storage_remove_docked_unit,
};

void reset_recorders() {
    g_trace.clear();
    g_remove_docked_calls.clear();
    g_put_on_map_calls.clear();
    g_fow_update_calls.clear();
    g_group_adjust_calls.clear();
}

} // namespace

void run_unit_takeoff_finalize_tests() {
    sim_fixture fx;

    // =================================================================================================
    // 1 -- the full straight-line sequence, exact call order and every argument.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();

        constexpr uint16_t PLAYER       = 1;
        constexpr int32_t  UNIT_INDEX   = 5;
        constexpr uint8_t  STORAGE_SLOT = 9;
        constexpr uint16_t PROTO        = 3;

        unit &u                   = fx.u(PLAYER, UNIT_INDEX);
        u.home_storage_slot       = STORAGE_SLOT;
        u.unit_proto_id           = PROTO;
        u.x                       = 40;
        u.y                       = 60;
        fx.cfg_units[PROTO].sight = 7;

        unit_storage &st   = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
        st.door_mutex_unit = 123; // must become 0

        sim_store own = fx.store();
        detail::unit_takeoff_finalize(fx.view(), own, g_calls, PLAYER, UNIT_INDEX);

        ck(g_trace.size() == 4 && std::strcmp(g_trace[0], "storage_remove_docked_unit") == 0 &&
               std::strcmp(g_trace[1], "map_unit_PutOnMap") == 0 &&
               std::strcmp(g_trace[2], "map_fow_UpdateFoWPlus") == 0 &&
               std::strcmp(g_trace[3], "ai_group_member_count_adjust") == 0,
           "1: exact call order (0x0048b19d/0x0048b208/0x0048b262/0x0048b286)");

        ck(g_remove_docked_calls.size() == 1 && g_remove_docked_calls[0].player == PLAYER &&
               g_remove_docked_calls[0].unit_index == UNIT_INDEX &&
               g_remove_docked_calls[0].storage_slot == STORAGE_SLOT,
           "1: storage_remove_docked_unit(player, unit_index, home_storage_slot), 0x0048b196-0x0048b19d");

        ck_eq((uint32_t)st.door_mutex_unit, 0u, "1: door_mutex_unit released to 0, 0x0048b1a2-0x0048b1c8");

        ck(g_put_on_map_calls.size() == 1 && g_put_on_map_calls[0].player == PLAYER &&
               g_put_on_map_calls[0].b_id == UNIT_INDEX && g_put_on_map_calls[0].x == 40 &&
               g_put_on_map_calls[0].y == 60,
           "1: map_unit_PutOnMap(player, unit_index, x, y), 0x0048b200-0x0048b208");

        ck(g_fow_update_calls.size() == 1 && g_fow_update_calls[0].player == PLAYER &&
               g_fow_update_calls[0].x == 40 && g_fow_update_calls[0].y == 60 &&
               g_fow_update_calls[0].sight == 7,
           "1: map_fow_UpdateFoWPlus(player, x, y, cfg_units[proto].sight), 0x0048b25f-0x0048b262");

        ck(g_group_adjust_calls.size() == 1 && g_group_adjust_calls[0].player == PLAYER &&
               g_group_adjust_calls[0].unit_index == UNIT_INDEX &&
               g_group_adjust_calls[0].group_or_type == STORAGE_SLOT && g_group_adjust_calls[0].mode == 0u,
           "1: ai_group_member_count_adjust(player, unit_index, home_storage_slot, mode=0), "
           "0x0048b267-0x0048b286 -- literal 0, OPPOSITE of takeoff_landing's own mode=1 call to the "
           "same function");
    }

    // =================================================================================================
    // 2 -- different player/index/slot/proto/coords, to rule out a hardcoded index anywhere above.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();

        constexpr uint16_t PLAYER       = 0;
        constexpr int32_t  UNIT_INDEX   = 42;
        constexpr uint8_t  STORAGE_SLOT = 3;
        constexpr uint16_t PROTO        = 17;

        unit &u                   = fx.u(PLAYER, UNIT_INDEX);
        u.home_storage_slot       = STORAGE_SLOT;
        u.unit_proto_id           = PROTO;
        u.x                       = 200;
        u.y                       = 5;
        fx.cfg_units[PROTO].sight = 12;

        unit_storage &st   = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
        st.door_mutex_unit = 1;

        sim_store own = fx.store();
        detail::unit_takeoff_finalize(fx.view(), own, g_calls, PLAYER, UNIT_INDEX);

        ck(g_remove_docked_calls.size() == 1 && g_remove_docked_calls[0].player == PLAYER &&
               g_remove_docked_calls[0].unit_index == UNIT_INDEX &&
               g_remove_docked_calls[0].storage_slot == STORAGE_SLOT,
           "2: storage_remove_docked_unit args track the (player, unit_index) arguments, not a "
           "hardcoded slot");
        ck_eq((uint32_t)st.door_mutex_unit, 0u, "2: door_mutex_unit released on the correct storage row");
        ck(g_put_on_map_calls.size() == 1 && g_put_on_map_calls[0].x == 200 && g_put_on_map_calls[0].y == 5,
           "2: map_unit_PutOnMap uses THIS unit's own x/y");
        ck(g_fow_update_calls.size() == 1 && g_fow_update_calls[0].sight == 12,
           "2: map_fow_UpdateFoWPlus uses THIS unit's own cfg_units[proto].sight");
        ck(g_group_adjust_calls.size() == 1 && g_group_adjust_calls[0].group_or_type == STORAGE_SLOT,
           "2: ai_group_member_count_adjust's third arg tracks home_storage_slot, not a hardcoded value");
    }
}

} // namespace mh::sim::test
