//
// sim_game_add_to_available_buildings_selftest.cpp -- `simtest` offline oracle for
// game_AddToAvailableBuildings / game_AddToAvailableBuildingsWithCheck
// (sim/sim_game_add_to_available_buildings.{h,cpp}, RI-SIM / SIM1F).
//
// Both functions' only observable effect is through the one-member-plus-one `_calls` struct
// (insert_item_in_player_array / set_event), so both are fully offline-coverable via the recording
// stub below -- no live-image call, no float, no roster write.
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY (tmp/decomp/game_AddToAvailableBuildings_004141a1.asm,
// tmp/decomp/game_AddToAvailableBuildingsWithCheck_00440049.asm) per the header's own branch-by-branch
// derivation, not the .cpp.
//
#include "sim/sim_game_add_to_available_buildings.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct InsertCall {
    int32_t *arr;
    int32_t  size;
    uint32_t player;
    int32_t  item;
    int32_t  new_item;
};
std::vector<InsertCall> g_inserts;
std::vector<uint32_t>   g_set_event;

void stub_insert(int32_t *arr, int32_t size, uint32_t player, int32_t item, int32_t new_item) {
    g_inserts.push_back({arr, size, player, item, new_item});
}
uint32_t stub_set_event(uint32_t type) {
    g_set_event.push_back(type);
    return 0;
}

const add_to_available_buildings_calls g_calls = {stub_insert, stub_set_event};

void clear_calls() {
    g_inserts.clear();
    g_set_event.clear();
}

} // namespace

void run_add_to_available_buildings_tests() {
    sim_fixture fx;

    // ================================================================================================
    // game_AddToAvailableBuildings (bare): one call, no branches.
    // ================================================================================================

    // C1: row pointer = base + player*AVAILABLE_BUILDINGS_ROW_INTS; size is the row's own capacity;
    // item is always the constant 0; new_item is b_i verbatim.
    // Mutation note: swapping `player` and `b_i` in the call, or dropping the *player row offset,
    // would flip this check red (row ptr and new_item would no longer match).
    fx.reset();
    clear_calls();
    {
        sim_store own = fx.store();
        detail::add_to_available_buildings(own, g_calls, /*player*/ 5u, /*b_i*/ 77);
    }
    ck_eq((uint32_t)g_inserts.size(), 1u, "bare: exactly one insert call");
    {
        int32_t *expected = fx.available_buildings.data() + 5 * AVAILABLE_BUILDINGS_ROW_INTS;
        ck(g_inserts[0].arr == expected, "bare: row ptr = base + player(5)*50");
    }
    ck_eq((uint32_t)g_inserts[0].size, (uint32_t)AVAILABLE_BUILDINGS_ROW_INTS, "bare: size == row cap (50)");
    ck_eq(g_inserts[0].player, 5u, "bare: player passed through unchanged");
    ck_eq((uint32_t)g_inserts[0].item, 0u, "bare: item is the constant 0");
    ck_eq((uint32_t)g_inserts[0].new_item, 77u, "bare: new_item == b_i(77)");

    // C2: a different player selects a different row (catches a player-index-ignored bug).
    fx.reset();
    clear_calls();
    {
        sim_store own = fx.store();
        detail::add_to_available_buildings(own, g_calls, /*player*/ 2u, /*b_i*/ 9);
    }
    {
        int32_t *expected = fx.available_buildings.data() + 2 * AVAILABLE_BUILDINGS_ROW_INTS;
        ck(g_inserts[0].arr == expected, "bare player2: row ptr = base + 2*50, distinct from player5's row");
    }

    // ================================================================================================
    // game_AddToAvailableBuildingsWithCheck: five sequential gates, then the bare call above, then a
    // viewing-player-only dirty event.
    // ================================================================================================

    // Gate 1 (0x0044006d-0x00440074): upgrade_lvl != 1 -> return immediately, no insert, no event.
    fx.reset();
    clear_calls();
    fx.cfg_buildings[3].upgrade_lvl = 0; // != 1
    fx.cfg_buildings[3].type        = 1; // irrelevant -- gate 1 blocks before type is ever read
    {
        sim_store own = fx.store();
        detail::add_to_available_buildings_with_check(fx.view(), own, g_calls, /*player*/ 1, /*b_i*/ 3);
    }
    ck_eq((uint32_t)g_inserts.size(), 0u, "gate1 fail (upgrade_lvl=0): no insert");
    ck_eq((uint32_t)g_set_event.size(), 0u, "gate1 fail: no event");

    // Gate 2 (0x0044007a-0x004400a5): non-SP session + A_PORT type -> blocked.
    fx.reset();
    clear_calls();
    fx.cfg_buildings[3].upgrade_lvl = 1;
    fx.cfg_buildings[3].type        = BUILDING_TYPE_A_PORT;
    fx.session_mode                 = 3; // MP-ish, != SESSION_SP
    {
        sim_store own = fx.store();
        detail::add_to_available_buildings_with_check(fx.view(), own, g_calls, 1, 3);
    }
    ck_eq((uint32_t)g_inserts.size(), 0u, "gate2 MP+A_PORT: blocked");

    // Gate 2, the H_PORT side of the pair, still in a non-SP session -> also blocked.
    fx.reset();
    clear_calls();
    fx.cfg_buildings[3].upgrade_lvl = 1;
    fx.cfg_buildings[3].type        = BUILDING_TYPE_H_PORT;
    fx.session_mode                 = 3;
    {
        sim_store own = fx.store();
        detail::add_to_available_buildings_with_check(fx.view(), own, g_calls, 1, 3);
    }
    ck_eq((uint32_t)g_inserts.size(), 0u, "gate2 MP+H_PORT: blocked");

    // Gate 2 is SKIPPED entirely in a SESSION_SP session -- the SAME A_PORT type that just blocked
    // above now passes gate 2 (gates 3/4/5 don't reject A_PORT either, so this reaches the insert).
    // Mutation note: this is the check that would catch a translation that evaluates the port-type
    // compare unconditionally (dropping the `if (*v.session_mode != SESSION_SP)` wrapper).
    fx.reset();
    clear_calls();
    fx.cfg_buildings[3].upgrade_lvl = 1;
    fx.cfg_buildings[3].type        = BUILDING_TYPE_A_PORT;
    fx.session_mode                 = SESSION_SP;
    fx.player_side                  = 9; // != player(1), so no event fires here
    {
        sim_store own = fx.store();
        detail::add_to_available_buildings_with_check(fx.view(), own, g_calls, 1, 3);
    }
    ck_eq((uint32_t)g_inserts.size(), 1u, "gate2 SP: A_PORT is NOT evaluated in SP, so it passes");

    // Gate 3 (0x004400a5-0x004400c7): A_MOTHER -> blocked, regardless of session mode.
    fx.reset();
    clear_calls();
    fx.cfg_buildings[3].upgrade_lvl = 1;
    fx.cfg_buildings[3].type        = BUILDING_TYPE_A_MOTHER;
    fx.session_mode                 = SESSION_SP; // gate2 would not fire anyway; isolate gate3
    {
        sim_store own = fx.store();
        detail::add_to_available_buildings_with_check(fx.view(), own, g_calls, 1, 3);
    }
    ck_eq((uint32_t)g_inserts.size(), 0u, "gate3 A_MOTHER: blocked");

    // Gate 4 (0x004400c7-0x004400e9): H_MAIN_BASE -> blocked.
    fx.reset();
    clear_calls();
    fx.cfg_buildings[3].upgrade_lvl = 1;
    fx.cfg_buildings[3].type        = BUILDING_TYPE_H_MAIN_BASE;
    {
        sim_store own = fx.store();
        detail::add_to_available_buildings_with_check(fx.view(), own, g_calls, 1, 3);
    }
    ck_eq((uint32_t)g_inserts.size(), 0u, "gate4 H_MAIN_BASE: blocked");

    // Gate 5 (0x004400e9-0x0044010d): A_CIVIL -> blocked.
    fx.reset();
    clear_calls();
    fx.cfg_buildings[3].upgrade_lvl = 1;
    fx.cfg_buildings[3].type        = BUILDING_TYPE_A_CIVIL;
    {
        sim_store own = fx.store();
        detail::add_to_available_buildings_with_check(fx.view(), own, g_calls, 1, 3);
    }
    ck_eq((uint32_t)g_inserts.size(), 0u, "gate5 A_CIVIL: blocked");

    // All five gates pass -> the sibling call fires, with `player` widened uint16_t->uint32_t.
    // player != player_side here, so no event.
    fx.reset();
    clear_calls();
    fx.cfg_buildings[3].upgrade_lvl = 1;
    fx.cfg_buildings[3].type        = 0x40; // not in any exclusion set
    fx.session_mode                 = 3;    // non-SP, but type isn't a port so gate2 is moot
    fx.player_side                  = 9;    // != player(1)
    {
        sim_store own = fx.store();
        detail::add_to_available_buildings_with_check(fx.view(), own, g_calls, /*player*/ 1, /*b_i*/ 3);
    }
    ck_eq((uint32_t)g_inserts.size(), 1u, "all gates pass: insert fires");
    {
        int32_t *expected = fx.available_buildings.data() + 1 * AVAILABLE_BUILDINGS_ROW_INTS;
        ck(g_inserts[0].arr == expected, "all gates pass: row ptr uses the widened player(1)");
    }
    ck_eq((uint32_t)g_inserts[0].new_item, 3u, "all gates pass: new_item == b_i(3)");
    ck_eq((uint32_t)g_set_event.size(), 0u, "all gates pass, player != player_side: no event");

    // Viewing-player-only event: player == *v.player_side -> game_SetEvent(15) fires too.
    // (event id 15 == BUILD_BUILDINGS_REFRESH, the header's own resolved game::e::event member;
    // that constant is TU-local to the .cpp's anonymous namespace so it is reproduced literally here.)
    fx.reset();
    clear_calls();
    fx.cfg_buildings[3].upgrade_lvl = 1;
    fx.cfg_buildings[3].type        = 0x40;
    fx.player_side                  = 1; // == player below
    {
        sim_store own = fx.store();
        detail::add_to_available_buildings_with_check(fx.view(), own, g_calls, /*player*/ 1, /*b_i*/ 3);
    }
    ck_eq((uint32_t)g_inserts.size(), 1u, "viewing player: insert still fires");
    ck_eq((uint32_t)g_set_event.size(), 1u, "viewing player: event fires exactly once");
    ck_eq(g_set_event.empty() ? 0xffffffffu : g_set_event[0], 15u,
          "viewing player: event id == BUILD_BUILDINGS_REFRESH(15)");
}

} // namespace mh::sim::test
