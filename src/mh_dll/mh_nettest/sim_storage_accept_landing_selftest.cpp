//
// sim_storage_accept_landing_selftest.cpp -- `simtest` oracle for llm_strat_storage_accept_landing
// @0x0048ba1a (sim/sim_storage_dock.h/.cpp, RI-SIM / SIM1-G3).
//
// VACUOUS under a per-call shadow arm: this function writes NOTHING tracked directly -- its only
// state changes happen inside two ORIGINAL callees (setup_approach_path, unit_ctrlgroup_remove_member)
// -- so a shadow site would compare nothing even under a clean run (`sim_migration.json`'s
// `roster_write_via` lists both callees, `writes_shared` for this function itself is empty besides
// what those callees own). Every assertion here is on the RECORDED CALL SEQUENCE, same pattern as
// sim_storage_scrap_home_docked_units_selftest.cpp / sim_bldg_scrap_stored_units_selftest.cpp.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM tmp/decomp_sim/llm_strat_storage_accept_landing_0048ba1a.asm
// (not the .cpp):
//   1. (0x0048ba3b-0x0048ba48) ALWAYS: setup_approach_path(player, unit_index, path_slot,
//      storage_slot) -- passed through in ACCEPT_LANDING's OWN parameter names/order (register trace:
//      ECX/EBX each land in the callee's own path_slot/storage_slot slots despite the two functions
//      declaring them in OPPOSITE positional order -- see the .h banner's SHAPE note. NOT a swap).
//   2. (0x0048ba50-0x0048ba86) If player(narrowed to uint16_t) == PlayerSide AND ctrl_groups[0]
//      contains unit_index: remove unit_index from ctrl_groups[0], then game_SetEvent
//      (EVENT_INFO_REFRESH=6). The ctrl_group_contains_unit CALL ITSELF only fires when the player
//      check already passed (short-circuit &&) -- a foreign player never even queries the group.
//   3. (0x0048ba86-0x0048ba97) ALWAYS, regardless of step 2's outcome: unit_set_state_of(player,
//      unit_index, state=UNIT_STATE_LANDING=0x16).
//   `player` is reloaded via a 16-bit MOVZX at every one of its uses in the original (never a plain
//   32-bit reload) -- a case seeds player with garbage above bit 15 to prove the narrowing.
//
#include "sim/sim_storage_dock.h"

#include <cstdint>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct call_log {
    enum kind_t { SETUP_APPROACH_PATH,
                  CTRL_GROUP_CONTAINS,
                  CTRL_GROUP_REMOVE,
                  SET_EVENT,
                  SET_STATE };
    struct ev {
        kind_t   kind;
        uint32_t a, b, c, d; // meaning depends on kind; unused args left 0
    };
    std::vector<ev> events;
    void            reset() { events.clear(); }
};
call_log g_log;

// Returns whatever this case pre-set as the "unit is in the group" verdict -- so a case can drive
// both the contains-true and contains-false arms without a real ctrl_groups walk.
int32_t g_contains_result = 0;

const storage_accept_landing_calls &recording_calls() {
    static const storage_accept_landing_calls c = {
        [](uint32_t player, int32_t unit_index, int32_t path_slot, int32_t storage_slot) -> void {
            g_log.events.push_back(call_log::ev{call_log::SETUP_APPROACH_PATH, player,
                                                (uint32_t)unit_index, (uint32_t)path_slot,
                                                (uint32_t)storage_slot});
        },
        [](uint32_t unit_id, int32_t count, int32_t group_index) -> int32_t {
            g_log.events.push_back(
                call_log::ev{call_log::CTRL_GROUP_CONTAINS, unit_id, (uint32_t)count,
                             (uint32_t)group_index, 0});
            return g_contains_result;
        },
        [](uint32_t unit_idx, int32_t *count_ptr, int32_t group_idx) -> void {
            (void)count_ptr;
            g_log.events.push_back(
                call_log::ev{call_log::CTRL_GROUP_REMOVE, unit_idx, (uint32_t)group_idx, 0, 0});
        },
        [](uint32_t type) -> uint32_t {
            g_log.events.push_back(call_log::ev{call_log::SET_EVENT, type, 0, 0, 0});
            return 0;
        },
        [](int32_t player, int32_t unit_index, int16_t state) -> void {
            g_log.events.push_back(call_log::ev{call_log::SET_STATE, (uint32_t)player,
                                                (uint32_t)unit_index, (uint32_t)(uint16_t)state, 0});
        },
    };
    return c;
}

void ck_kind(int32_t n, call_log::kind_t kind, const char *what) {
    if ((size_t)n >= g_log.events.size()) {
        ck(false, what);
        return;
    }
    ck(g_log.events[(size_t)n].kind == kind, what);
}

} // namespace

// ---- S1: local player, unit IS in ctrl_groups[0] -> the full 5-call sequence, in order, with every
// argument checked. Distinct path_slot/storage_slot values so a swap between the two would be caught.
void test_local_player_in_group_full_sequence() {
    sim_fixture fx;
    fx.player_side                = 3;
    g_contains_result             = 1;                                   // "unit is in the group"
    constexpr uint16_t player     = 3;                                   // == player_side
    constexpr int32_t  unit_index = 42, storage_slot = 5, path_slot = 9; // distinct from each other

    const sim_view v   = fx.view();
    sim_store      own = fx.store();
    g_log.reset();
    detail::storage_accept_landing(v, own, recording_calls(), player, unit_index, storage_slot,
                                   path_slot);

    ck_eq((uint32_t)g_log.events.size(), 5u, "S1: local player + in-group -> all 5 calls fire");
    ck_kind(0, call_log::SETUP_APPROACH_PATH, "S1[0]: setup_approach_path");
    ck_eq(g_log.events[0].a, player, "S1[0]: setup_approach_path player");
    ck_eq(g_log.events[0].b, (uint32_t)unit_index, "S1[0]: setup_approach_path unit_index");
    ck_eq(g_log.events[0].c, (uint32_t)path_slot,
          "S1[0] SHAPE: setup_approach_path's 3rd arg is THIS function's path_slot (9), not "
          "storage_slot -- despite the two functions' opposite positional param order");
    ck_eq(g_log.events[0].d, (uint32_t)storage_slot,
          "S1[0] SHAPE: setup_approach_path's 4th arg is THIS function's storage_slot (5)");
    ck_kind(1, call_log::CTRL_GROUP_CONTAINS, "S1[1]: ctrl_group_contains_unit queried");
    ck_eq(g_log.events[1].a, (uint32_t)unit_index, "S1[1]: contains queried with unit_index");
    ck_kind(2, call_log::CTRL_GROUP_REMOVE, "S1[2]: unit removed from ctrl_groups[0]");
    ck_eq(g_log.events[2].a, (uint32_t)unit_index, "S1[2]: remove_member's unit_idx");
    ck_kind(3, call_log::SET_EVENT, "S1[3]: game_SetEvent fires after the removal");
    ck_eq(g_log.events[3].a, EVENT_INFO_REFRESH, "S1[3]: event type is EVENT_INFO_REFRESH (6)");
    ck_kind(4, call_log::SET_STATE, "S1[4]: unit_set_state_of fires LAST, unconditionally");
    ck_eq(g_log.events[4].a, player, "S1[4]: set_state player");
    ck_eq(g_log.events[4].b, (uint32_t)unit_index, "S1[4]: set_state unit_index");
    ck_eq(g_log.events[4].c, 0x16u, "S1[4]: set_state state == UNIT_STATE_LANDING (0x16)");
}

// ---- S2: local player, unit NOT in ctrl_groups[0] -- contains_unit is still QUERIED (the && short-
// circuits on the player check, not on the query itself), but remove/set_event never fire.
void test_local_player_not_in_group_skips_removal() {
    sim_fixture fx;
    fx.player_side            = 3;
    g_contains_result         = 0; // "unit is NOT in the group"
    constexpr uint16_t player = 3;

    const sim_view v   = fx.view();
    sim_store      own = fx.store();
    g_log.reset();
    detail::storage_accept_landing(v, own, recording_calls(), player, /*unit_index=*/1,
                                   /*storage_slot=*/2, /*path_slot=*/3);

    ck_eq((uint32_t)g_log.events.size(), 3u,
          "S2: local player, not in group -> setup_approach_path + contains-query + set_state only");
    ck_kind(0, call_log::SETUP_APPROACH_PATH, "S2[0]: setup_approach_path");
    ck_kind(1, call_log::CTRL_GROUP_CONTAINS, "S2[1]: the group IS queried");
    ck_kind(2, call_log::SET_STATE, "S2[2]: set_state still fires -- ALWAYS, not gated on group membership");
}

// ---- S3: player != PlayerSide -- the group is NEVER EVEN QUERIED (short-circuit && on the player
// check itself, before ctrl_group_contains_unit is called at all). set_state still always fires.
void test_foreign_player_never_queries_group() {
    sim_fixture fx;
    fx.player_side            = 3;
    g_contains_result         = 1; // would say "yes" if queried -- proving it is NOT queried
    constexpr uint16_t player = 7; // != player_side (3)

    const sim_view v   = fx.view();
    sim_store      own = fx.store();
    g_log.reset();
    detail::storage_accept_landing(v, own, recording_calls(), player, /*unit_index=*/1,
                                   /*storage_slot=*/2, /*path_slot=*/3);

    ck_eq((uint32_t)g_log.events.size(), 2u,
          "S3: foreign player -> setup_approach_path + set_state only, no group query at all");
    ck_kind(0, call_log::SETUP_APPROACH_PATH, "S3[0]: setup_approach_path");
    ck_kind(1, call_log::SET_STATE, "S3[1]: set_state still fires unconditionally");
    ck_eq(g_log.events[1].a, player, "S3[1]: set_state gets the (narrowed) player, not player_side");
}

// ---- S4: the 16-bit PLAYER NARROWING finding. player carries garbage above bit 15 (0x10003); the
// original reloads it via MOVZX at every use, so every call must see the NARROWED value (3), matching
// player_side (3) -- proving the comparison AND every downstream call use the narrowed value.
void test_player_narrowed_to_16_bits_at_every_use() {
    sim_fixture fx;
    fx.player_side                = 3;
    g_contains_result             = 1;
    constexpr int32_t player_wide = 0x10003; // low 16 bits == 3 == player_side

    const sim_view v   = fx.view();
    sim_store      own = fx.store();
    g_log.reset();
    detail::storage_accept_landing(v, own, recording_calls(), player_wide, /*unit_index=*/1,
                                   /*storage_slot=*/2, /*path_slot=*/3);

    ck_eq((uint32_t)g_log.events.size(), 5u,
          "S4: narrowed player (3) == player_side -> the full in-group sequence fires, proving the "
          "comparison used the narrowed value, not the raw 0x10003");
    ck_eq(g_log.events[0].a, 3u, "S4[0]: setup_approach_path sees the NARROWED player (3)");
    ck_eq(g_log.events[4].a, 3u, "S4[4]: set_state sees the NARROWED player (3), not 0x10003");
}

void run_storage_accept_landing_tests() {
    test_local_player_in_group_full_sequence();
    test_local_player_not_in_group_skips_removal();
    test_foreign_player_never_queries_group();
    test_player_narrowed_to_16_bits_at_every_use();
}

} // namespace mh::sim::test
