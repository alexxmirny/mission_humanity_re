#include "sim/sim_storage_dock.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- recorders ------------------------------------------------------------------------------

std::vector<uint32_t> g_set_event_calls;
uint32_t              rec_set_event(uint32_t type) {
    g_set_event_calls.push_back(type);
    return 0;
}

struct shuttle_load_call {
    uint16_t player;
    int32_t  building_index;
    uint32_t cap;
};
std::vector<shuttle_load_call> g_shuttle_load_calls;
uint32_t                       rec_shuttle_load_passengers(uint16_t player, int32_t building_index,
                                                           uint32_t cap) {
    g_shuttle_load_calls.push_back({player, building_index, cap});
    return 0;
}

struct set_state_call {
    int32_t player;
    int32_t unit_index;
    int16_t state;
};
std::vector<set_state_call> g_set_state_calls;
// T11's order probe: when non-null, rec_unit_set_state_of snapshots THIS unit's move_microstep at
// the moment it is called, so a case can prove the 0x20 write has NOT happened yet (i.e. that the
// write really is AFTER unit_set_state_of, matching the asm/header's resolved order, not before or
// merged into the same statement by coincidence).
unit   *g_order_probe_unit             = nullptr;
int32_t g_move_microstep_at_state_call = -1;
void    rec_unit_set_state_of(int32_t player, int32_t unit_index, int16_t state) {
    g_set_state_calls.push_back({player, unit_index, state});
    if (g_order_probe_unit != nullptr) g_move_microstep_at_state_call = g_order_probe_unit->move_microstep;
}

struct fow_call {
    uint32_t player;
    int32_t  x;
    int32_t  y;
    uint8_t  radius;
};
std::vector<fow_call> g_fow_calls;
void                  rec_fow_remove_sight(uint32_t player, int32_t x, int32_t y, uint8_t radius) {
    g_fow_calls.push_back({player, x, y, radius});
}

struct contains_call {
    uint32_t unit_id;
    int32_t  count;
    int32_t  group_index;
};
std::vector<contains_call> g_contains_calls;
int32_t                    g_contains_result = 0;
int32_t                    rec_ctrl_group_contains_unit(uint32_t unit_id, int32_t count, int32_t group_index) {
    g_contains_calls.push_back({unit_id, count, group_index});
    return g_contains_result;
}

struct remove_member_call {
    uint32_t unit_idx;
    void    *count_ptr;
    int32_t  group_idx;
};
std::vector<remove_member_call> g_remove_member_calls;
void                            rec_unit_ctrlgroup_remove_member(uint32_t unit_idx, int32_t *count_ptr, int32_t group_idx) {
    g_remove_member_calls.push_back({unit_idx, count_ptr, group_idx});
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
}

const storage_board_unit_calls g_calls = {
    &rec_set_event,
    &rec_shuttle_load_passengers,
    &rec_unit_set_state_of,
    &rec_fow_remove_sight,
    &rec_ctrl_group_contains_unit,
    &rec_unit_ctrlgroup_remove_member,
    &rec_ai_group_member_count_adjust,
};

void reset_recorders() {
    g_set_event_calls.clear();
    g_shuttle_load_calls.clear();
    g_set_state_calls.clear();
    g_order_probe_unit             = nullptr;
    g_move_microstep_at_state_call = -1;
    g_fow_calls.clear();
    g_contains_calls.clear();
    g_contains_result = 0;
    g_remove_member_calls.clear();
    g_ai_adjust_calls.clear();
}

// ---- fixture wiring ---------------------------------------------------------------------------
// DISTINCT, non-symmetric constants throughout (sim_test_support.h's own rule) -- a translation
// that swapped player/unit_idx/storage_idx/b_index, or x/y, or the two shuttle-branch human values,
// disagrees with a specific case rather than passing by coincidence.
constexpr uint16_t PLAYER      = 3;
constexpr uint32_t UNIT_IDX    = 9;
constexpr uint32_t STORAGE_IDX = 5;
constexpr int32_t  B_INDEX     = 6;
constexpr uint16_t PROTO       = 2;
constexpr uint16_t BUILDING_ID = 4;
constexpr int32_t  UX          = 50;
constexpr int32_t  UY          = 60;

// Seeds everything storage_board_unit reads. `docked_count_pre` lets a case prove the append lands
// at the CURRENT count (not always index 0). Returns the unit by reference so a case can read/probe
// it after the call.
unit &seed(sim_fixture &fx, int32_t soldier_count, int32_t human, uint8_t sight, uint8_t bldg_type,
           uint8_t shuttle_slot, int32_t docked_count_pre) {
    unit &u                    = fx.u(PLAYER, UNIT_IDX);
    u.unit_proto_id            = PROTO;
    u.x                        = UX;
    u.y                        = UY;
    u.origin_tile_was_passable = 200; // distinct from the pre-seeded passable[] value below (17)
    u.move_microstep           = 3;   // distinct from the terminal write value (0x20)

    fx.cfg_units[PROTO].soldier_count = soldier_count;
    fx.cfg_units[PROTO].human         = human;
    fx.cfg_units[PROTO].sight         = sight;

    unit_storage &st = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_IDX];
    st.b_index       = B_INDEX;
    st.docked_count  = docked_count_pre;
    st.occupancy     = 100;

    building &b                        = fx.b(PLAYER, B_INDEX);
    b.building_id                      = BUILDING_ID;
    b.shuttle_slot                     = shuttle_slot;
    fx.cfg_buildings[BUILDING_ID].type = bldg_type;

    fx.population[PLAYER].human          = 1000;
    fx.population[PLAYER].human_in_field = 500;

    fx.passable[(size_t)((UX << 8) | UY)] = 17; // pre-value != origin_tile_was_passable(200)
    tile_object &t                        = fx.t(UX, UY);
    t.flags[0] = t.flags[1] = 0x7; // sentinel: never touched by this function, must survive
    t.building              = 1234;
    t.class_owner           = 0x99;
    t.visibility            = 0x55; // sentinel: never touched, must survive

    fx.player_side = 7; // != PLAYER by default; a case that needs the local-player branch sets it
    return u;
}

} // namespace

void run_storage_board_unit_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- baseline: soldier_count=5 (>0, population-adjust SKIPPED), non-shuttle building, player
    // != PlayerSide. Pins EVERY unconditional call/write in one pass, and the dock-list-append
    // occupancy "else" arm (occupancy += soldier_count, not +=1) via the real call through board_unit's
    // own (unmocked) sibling call.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u = seed(fx, /*soldier_count=*/5, /*human=*/77, /*sight=*/9, /*bldg_type=*/5,
                       /*shuttle_slot=*/1, /*docked_count_pre=*/2);

        sim_store own = fx.store();
        detail::storage_board_unit(fx.view(), own, g_calls, PLAYER, UNIT_IDX, STORAGE_IDX);

        unit_storage &st = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_IDX];
        ck_eq((uint32_t)st.docked_units[2], UNIT_IDX,
              "T1: dock_list_append appends at the PRE-call docked_count (2), 0x0048b5c6 (via board_unit's own call, 0x0048b6e5)");
        ck_eq((uint32_t)st.docked_count, 3u, "T1: docked_count += 1, 0x0048b607");
        ck_eq((uint32_t)st.occupancy, 105u,
              "T1: occupancy += soldier_count (5, the 'else' arm since soldier_count != 0), 0x0048b623-0x0048b6b2");

        ck(g_set_event_calls.size() == 1 && g_set_event_calls[0] == 7,
           "T1: game_SetEvent(BUILD_PROJECTS_REFRESH=7) ALWAYS, 0x0048b6ea-0x0048b6ef");

        ck_eq((uint32_t)fx.population[PLAYER].human, 1000u,
              "T1: population.human untouched -- soldier_count>0 skips the pop-adjust block, 0x0048b7a2 JG taken");
        ck_eq((uint32_t)fx.population[PLAYER].human_in_field, 500u, "T1: population.human_in_field untouched too");

        ck(g_shuttle_load_calls.empty(), "T1: shuttle_load_passengers NOT called -- building.type is not a shuttle type");

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0].player == PLAYER &&
               g_set_state_calls[0].unit_index == (int32_t)UNIT_IDX && g_set_state_calls[0].state == 0x26,
           "T1: unit_set_state_of(player,unit_idx,ENTER_WALK_IN=0x26) ALWAYS, 0x0048b90d-0x0048b919");

        ck_eq((uint32_t)u.move_microstep, 0x20u,
              "T1: unit.move_microstep = 0x20 ALWAYS, unconditionally, 0x0048b91e-0x0048b931 (reimpl-verify fix)");

        ck_eq((uint32_t)fx.t(UX, UY).building, 0u, "T1: tile_object_at(x,y).building = 0, 0x0048b971");
        ck_eq((uint32_t)fx.t(UX, UY).class_owner, 0u, "T1: tile_object_at(x,y).class_owner = 0, 0x0048b988");
        ck_eq((uint32_t)fx.t(UX, UY).flags[0], 0x7u, "T1: tile_object_at(x,y).flags[0] untouched (sentinel survives)");
        ck_eq((uint32_t)fx.t(UX, UY).visibility, 0x55u, "T1: tile_object_at(x,y).visibility untouched (sentinel survives)");
        ck_eq((uint32_t)fx.passable[(size_t)((UX << 8) | UY)], 200u,
              "T1: passable_at(x,y) restored to unit.origin_tile_was_passable(200), 0x0048b95d");

        ck(g_fow_calls.size() == 1 && g_fow_calls[0].player == PLAYER && g_fow_calls[0].x == UX &&
               g_fow_calls[0].y == UY && g_fow_calls[0].radius == 9,
           "T1: fow_remove_sight(player,x,y,sight=9) ALWAYS, 0x0048b98f-0x0048b9c0");

        ck(g_contains_calls.empty(),
           "T1: ctrl_group_contains_unit NEVER called -- player(3) != PlayerSide(7), short-circuited before the call, 0x0048b9cf JNZ");

        ck(g_ai_adjust_calls.size() == 1 && g_ai_adjust_calls[0].player == PLAYER &&
               g_ai_adjust_calls[0].unit_index == UNIT_IDX && g_ai_adjust_calls[0].group_or_type == STORAGE_IDX &&
               g_ai_adjust_calls[0].mode == 1,
           "T1: ai_group_member_count_adjust(player,unit_idx,storage_idx[FULL dword],mode=1) ALWAYS, 0x0048b9fe-0x0048ba0d");
    }

    // =================================================================================================
    // T2 -- soldier_count == 0 boundary (the TIGHT edge against T1's 5): population-adjust FIRES,
    // and dock_list_append's occupancy weighting takes its "== 0" arm (+=1, not += soldier_count).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed(fx, /*soldier_count=*/0, /*human=*/42, /*sight=*/9, /*bldg_type=*/5, /*shuttle_slot=*/1,
             /*docked_count_pre=*/0);

        sim_store own = fx.store();
        detail::storage_board_unit(fx.view(), own, g_calls, PLAYER, UNIT_IDX, STORAGE_IDX);

        unit_storage &st = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_IDX];
        ck_eq((uint32_t)st.occupancy, 101u,
              "T2: soldier_count==0 takes dock_list_append's '==0' arm -- occupancy += 1, NOT += soldier_count");
        ck_eq((uint32_t)fx.population[PLAYER].human, 1042u,
              "T2: soldier_count==0 is <=0 -- population.human += Unit[proto].human(42), 0x0048b7a9 JG NOT taken at the boundary");
        ck_eq((uint32_t)fx.population[PLAYER].human_in_field, 458u, "T2: population.human_in_field -= 42, 0x0048b821");
    }

    // =================================================================================================
    // T3 -- soldier_count NEGATIVE (-5): board_unit's own `> 0` population test is SIGNED, so a
    // negative count is <=0 (same pop-adjust arm as T2's zero) -- but dock_list_append's occupancy
    // weighting is an EQUALITY test (`== 0`), so a negative count takes its "else" arm and DECREASES
    // occupancy (100 + (-5) = 95), unlike T2's zero which increases it. Distinguishes the two
    // different comparisons on the same field within this one call.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed(fx, /*soldier_count=*/-5, /*human=*/13, /*sight=*/9, /*bldg_type=*/5, /*shuttle_slot=*/1,
             /*docked_count_pre=*/0);

        sim_store own = fx.store();
        detail::storage_board_unit(fx.view(), own, g_calls, PLAYER, UNIT_IDX, STORAGE_IDX);

        unit_storage &st = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_IDX];
        ck_eq((uint32_t)st.occupancy, 95u,
              "T3: soldier_count=-5 != 0 -- dock_list_append's 'else' arm, occupancy += (-5) = 95 (signed, DECREASES)");
        ck_eq((uint32_t)fx.population[PLAYER].human, 1013u,
              "T3: soldier_count=-5 is <=0 -- board_unit's own pop-adjust STILL fires (signed JG test), human += 13");
        ck_eq((uint32_t)fx.population[PLAYER].human_in_field, 487u, "T3: human_in_field -= 13");
    }

    // =================================================================================================
    // T4 -- shuttle building, type == A_SHUTTLE(0xd), shuttle_slot != 0, human_delta != 0 (soldier_
    // count==0, human=8): shuttle_load_passengers fires with cap = human_delta = 8.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed(fx, /*soldier_count=*/0, /*human=*/8, /*sight=*/9, /*bldg_type=*/0x0d, /*shuttle_slot=*/3,
             /*docked_count_pre=*/0);

        sim_store own = fx.store();
        detail::storage_board_unit(fx.view(), own, g_calls, PLAYER, UNIT_IDX, STORAGE_IDX);

        ck(g_shuttle_load_calls.size() == 1 && g_shuttle_load_calls[0].player == PLAYER &&
               g_shuttle_load_calls[0].building_index == B_INDEX && g_shuttle_load_calls[0].cap == 8u,
           "T4: A_SHUTTLE(0xd) type, shuttle_slot!=0, human_delta!=0 -> shuttle_load_passengers(player,b_index,8), 0x0048b8fa-0x0048b90a");
        ck_eq((uint32_t)fx.population[PLAYER].human, 1008u, "T4: population still adjusts too (soldier_count<=0)");
    }

    // =================================================================================================
    // T5 -- shuttle building, type == H_SHUTTLE(0x21) (the OTHER half of the OR), shuttle_slot != 0,
    // human=15 (DELIBERATELY DIFFERENT from T4's 8, so a translation reusing the wrong cached value
    // fails this case specifically).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed(fx, /*soldier_count=*/0, /*human=*/15, /*sight=*/9, /*bldg_type=*/0x21, /*shuttle_slot=*/7,
             /*docked_count_pre=*/0);

        sim_store own = fx.store();
        detail::storage_board_unit(fx.view(), own, g_calls, PLAYER, UNIT_IDX, STORAGE_IDX);

        ck(g_shuttle_load_calls.size() == 1 && g_shuttle_load_calls[0].cap == 15u,
           "T5: H_SHUTTLE(0x21) type -- the OTHER OR operand -- shuttle_load_passengers cap=15, 0x0048b847-0x0048b877 type test");
    }

    // =================================================================================================
    // T6 -- shuttle TYPE matches (0x0d) and human_delta != 0, but shuttle_slot == 0: shuttle_load
    // NOT called. Paired with T4 (same type, slot!=0, DOES fire) so this branch is pinned on BOTH
    // arms, not vacuously.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed(fx, /*soldier_count=*/0, /*human=*/8, /*sight=*/9, /*bldg_type=*/0x0d, /*shuttle_slot=*/0,
             /*docked_count_pre=*/0);

        sim_store own = fx.store();
        detail::storage_board_unit(fx.view(), own, g_calls, PLAYER, UNIT_IDX, STORAGE_IDX);

        ck(g_shuttle_load_calls.empty(), "T6: shuttle_slot==0 -> shuttle_load_passengers NOT called, 0x0048b8eb-0x0048b8f2");
        ck_eq((uint32_t)fx.population[PLAYER].human, 1008u, "T6: population adjust still fires -- independent of shuttle_slot");
    }

    // =================================================================================================
    // T7 -- shuttle TYPE matches (0x0d), shuttle_slot != 0, but soldier_count > 0 (human_delta forced
    // to 0 regardless of Unit[proto].human's actual value, seeded here to a LARGE nonzero 999 to catch
    // a translation that forgets the soldier_count>0 override): shuttle_load NOT called, population
    // NOT adjusted either.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed(fx, /*soldier_count=*/2, /*human=*/999, /*sight=*/9, /*bldg_type=*/0x0d, /*shuttle_slot=*/3,
             /*docked_count_pre=*/0);

        sim_store own = fx.store();
        detail::storage_board_unit(fx.view(), own, g_calls, PLAYER, UNIT_IDX, STORAGE_IDX);

        ck(g_shuttle_load_calls.empty(),
           "T7: soldier_count>0 forces human_delta=0 even though Unit[proto].human=999 -- shuttle_load NOT called, 0x0048b8f4 JZ taken");
        ck_eq((uint32_t)fx.population[PLAYER].human, 1000u, "T7: population.human untouched (999 NOT added)");
        unit_storage &st = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_IDX];
        ck_eq((uint32_t)st.occupancy, 102u, "T7: dock_list_append's occupancy still += soldier_count(2), independent of human_delta");
    }

    // =================================================================================================
    // T8 -- local-player ctrl-group cleanup FIRES: player == PlayerSide AND contains_unit != 0.
    // Pins the exact args passed to contains_unit/remove_member (including the count POINTER
    // identity) and the ORDER of the two SetEvent calls (7 then 6).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed(fx, /*soldier_count=*/1, /*human=*/1, /*sight=*/9, /*bldg_type=*/5, /*shuttle_slot=*/0,
             /*docked_count_pre=*/0);
        fx.player_side          = (int16_t)PLAYER; // local-player branch taken
        g_contains_result       = 1;               // "unit IS in ctrl group 0"
        fx.ctrl_groups[0].count = 42;              // distinct sentinel, not 0

        sim_store own = fx.store();
        detail::storage_board_unit(fx.view(), own, g_calls, PLAYER, UNIT_IDX, STORAGE_IDX);

        ck(g_contains_calls.size() == 1 && g_contains_calls[0].unit_id == UNIT_IDX &&
               g_contains_calls[0].count == 42 && g_contains_calls[0].group_index == 0,
           "T8: ctrl_group_contains_unit(unit_idx, ctrl_groups[0].count=42, group=0), 0x0048b9d3-0x0048b9e1");
        ck(g_remove_member_calls.size() == 1 && g_remove_member_calls[0].unit_idx == UNIT_IDX &&
               g_remove_member_calls[0].count_ptr == &fx.ctrl_groups[0].count &&
               g_remove_member_calls[0].group_idx == 0,
           "T8: unit_ctrlgroup_remove_member(unit_idx, &ctrl_group_at(0).count, 0), 0x0048b9e5-0x0048b9f4");
        ck(g_set_event_calls.size() == 2 && g_set_event_calls[0] == 7 && g_set_event_calls[1] == 6,
           "T8: SetEvent order is BUILD_PROJECTS_REFRESH(7) THEN INFO_REFRESH(6) -- step2 fires before step10, 0x0048b6ea vs 0x0048b9f9");
    }

    // =================================================================================================
    // T9 -- player == PlayerSide but contains_unit returns 0: contains_unit IS still called (the
    // player-side gate alone does not short-circuit it), but remove_member/SetEvent(6) do NOT fire.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed(fx, /*soldier_count=*/1, /*human=*/1, /*sight=*/9, /*bldg_type=*/5, /*shuttle_slot=*/0,
             /*docked_count_pre=*/0);
        fx.player_side    = (int16_t)PLAYER;
        g_contains_result = 0; // "unit is NOT in ctrl group 0"

        sim_store own = fx.store();
        detail::storage_board_unit(fx.view(), own, g_calls, PLAYER, UNIT_IDX, STORAGE_IDX);

        ck_eq((uint32_t)g_contains_calls.size(), 1u,
              "T9: ctrl_group_contains_unit IS called (player==PlayerSide alone reaches the test)");
        ck(g_remove_member_calls.empty(), "T9: unit_ctrlgroup_remove_member NOT called -- contains_unit returned 0");
        ck(g_set_event_calls.size() == 1 && g_set_event_calls[0] == 7,
           "T9: only SetEvent(7) fires -- no SetEvent(INFO_REFRESH=6)");
    }

    // =================================================================================================
    // T10 -- player != PlayerSide: contains_unit is SHORT-CIRCUITED (never called at all, not just
    // "called but returns 0" like T9) -- AND every unconditional call/write in the closure still
    // fires, proving none of them is accidentally gated behind the same local-player check (the
    // vacuous-pass trap the brief warns about: a naive oracle only exercises these with the ctrl-
    // group branch ALSO off, so a bug that wrongly gated them behind `player==PlayerSide` would
    // still pass every other case above by coincidence).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u        = seed(fx, /*soldier_count=*/3, /*human=*/50, /*sight=*/6, /*bldg_type=*/5,
                              /*shuttle_slot=*/0, /*docked_count_pre=*/0);
        fx.player_side = 99; // != PLAYER(3)

        sim_store own = fx.store();
        detail::storage_board_unit(fx.view(), own, g_calls, PLAYER, UNIT_IDX, STORAGE_IDX);

        ck(g_contains_calls.empty(), "T10: ctrl_group_contains_unit NEVER called -- player(3) != PlayerSide(99)");
        ck(g_remove_member_calls.empty(), "T10: unit_ctrlgroup_remove_member NEVER called");
        ck(g_set_event_calls.size() == 1 && g_set_event_calls[0] == 7,
           "T10: SetEvent(7) still fires (unconditional step2) even with the whole ctrl-group branch off");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0].state == 0x26,
           "T10: unit_set_state_of still fires unconditionally");
        ck_eq((uint32_t)u.move_microstep, 0x20u, "T10: move_microstep still forced to 0x20 unconditionally");
        ck_eq((uint32_t)fx.t(UX, UY).building, 0u, "T10: tile clear still fires unconditionally");
        ck_eq((uint32_t)fx.passable[(size_t)((UX << 8) | UY)], 200u,
              "T10: passable restore still fires unconditionally");
        ck(g_fow_calls.size() == 1, "T10: fow_remove_sight still fires unconditionally");
        ck(g_ai_adjust_calls.size() == 1, "T10: ai_group_member_count_adjust still fires unconditionally");
    }

    // =================================================================================================
    // T11 -- ORDER: unit.move_microstep = 0x20 happens AFTER unit_set_state_of returns, not before
    // and not merged into it. The recorder snapshots move_microstep AT THE MOMENT unit_set_state_of
    // is called; if that snapshot were already 0x20, the write would have to have happened BEFORE
    // the call, contradicting the header's resolved instruction order (0x0048b90d call, THEN
    // 0x0048b91e-0x0048b931 write).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        unit &u            = seed(fx, /*soldier_count=*/1, /*human=*/1, /*sight=*/9, /*bldg_type=*/5,
                                  /*shuttle_slot=*/0, /*docked_count_pre=*/0);
        u.move_microstep   = 3; // distinct from 0x20, already set by seed() but restated for clarity
        g_order_probe_unit = &u;

        sim_store own = fx.store();
        detail::storage_board_unit(fx.view(), own, g_calls, PLAYER, UNIT_IDX, STORAGE_IDX);

        ck_eq((uint32_t)g_move_microstep_at_state_call, 3u,
              "T11: move_microstep was STILL 3 (the pre-value) at the moment unit_set_state_of was called -- the 0x20 write happens AFTER, 0x0048b90d then 0x0048b91e-0x0048b931");
        ck_eq((uint32_t)u.move_microstep, 0x20u, "T11: ...and is 0x20 by the time the function returns");
    }
}

} // namespace mh::sim::test
