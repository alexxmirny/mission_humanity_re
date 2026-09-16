//
// sim_unit_state_enter_arrival_check_selftest.cpp -- `simtest` oracle for
// llm_strat_unit_state_enter_arrival_check @0x0047fe68 (sim/sim_unit_state_enter.h/.cpp,
// RI-SIM / SIM1-G3).
//
// NOT SHADOWABLE (0 tracked write cells -- its only effect is choosing which state
// llm_strat_unit_set_state commits, and that write is the CALLEE's own declared closure, not this
// site's). This is the sole coverage this function gets, per the batch's un-armable path.
//
// SCOPE: the at-exit-tile gate (0x0047fe80-0x0047fee2, comparing the unit's CURRENT (x,y) against
// storage.exit_tile_x/_y -- NOT the move-order goal, per the header's field-offset correction) at
// both boundaries (x matches/y doesn't, y matches/x doesn't, both match); the built_flags==3 gate
// (0x0047fee2-0x0047ff2a) reached only when at the exit tile; and the two possible unit_set_state
// outcomes (ENTER_STORAGE_BEGIN=0x24 vs GROUP_MARSHAL=0xa) with an exact-call-count assertion (this
// function calls unit_set_state exactly once on every path, never zero, never twice).
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM
// tmp/decomp_sim/llm_strat_unit_state_enter_arrival_check_0047fe68.asm -- every assertion below
// cites the instruction address(es) it pins. NOT read off the .cpp body.
//
#include "sim/sim_unit_state_enter.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) { g_set_state_calls.push_back(new_state); }

const unit_state_enter_calls g_calls = {
    &rec_unit_set_state,
    nullptr, // storage_can_enter -- unreachable from this function
    nullptr, // storage_board_unit
    nullptr, // unit_queue_advance
    nullptr, // dir_from_to
    nullptr, // unit_walk_step_allowed
    nullptr, // unit_soldiers_set_heading
    nullptr, // dir_step_factor
    nullptr, // target_release_ref
    nullptr, // path_free_slot
};

constexpr uint16_t GUARD_PLAYER = 3;
constexpr int32_t  GUARD_INDEX  = 7;

struct Seed {
    uint16_t player            = 1;
    int32_t  index             = 4;
    uint8_t  home_storage_slot = 2;

    uint8_t unit_x = 10, unit_y = 20;
    int32_t exit_tile_x = 10, exit_tile_y = 20;

    int32_t b_index     = 5;
    uint8_t built_flags = 3;
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    unit &g             = fx.u(GUARD_PLAYER, GUARD_INDEX);
    g.home_storage_slot = 9;

    unit &u             = fx.u(s.player, s.index);
    u.home_storage_slot = s.home_storage_slot;
    u.x                 = s.unit_x;
    u.y                 = s.unit_y;

    unit_storage &st = fx.storage[s.player * STORAGE_PER_PLAYER + s.home_storage_slot];
    st.exit_tile_x   = s.exit_tile_x;
    st.exit_tile_y   = s.exit_tile_y;
    st.b_index       = s.b_index;

    building &b   = fx.b(s.player, s.b_index);
    b.built_flags = s.built_flags;

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = s.player;
    fx.view_cur_index  = static_cast<uint16_t>(s.index);

    g_set_state_calls.clear();

    detail::unit_state_enter_arrival_check(fx.view(), g_calls);
}

} // namespace

void run_unit_state_enter_arrival_check_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- both x and y match the exit tile, built_flags==3: ENTER_STORAGE_BEGIN (0x24).
    // (0x0047fe8c-0x0047ff2a, both CMPs pass, built_flags CMP passes -> 0x0047ff3c)
    // =================================================================================================
    {
        Seed s;
        s.unit_x = s.exit_tile_x = 10;
        s.unit_y = s.exit_tile_y = 20;
        s.built_flags            = 3;
        seed_and_run(fx, s);
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x24,
           "T1: at exit tile + built_flags==3 -> unit_set_state(ENTER_STORAGE_BEGIN=0x24), called "
           "exactly once");
    }

    // =================================================================================================
    // T2 -- at the exit tile but built_flags != 3 (e.g. connected-not-staffed): GROUP_MARSHAL (0xa).
    // =================================================================================================
    {
        Seed s;
        s.unit_x = s.exit_tile_x = 11;
        s.unit_y = s.exit_tile_y = 21;
        s.built_flags            = 1; // connected but not staffed
        seed_and_run(fx, s);
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0xa,
           "T2: at exit tile, built_flags(1)!=3 -> unit_set_state(GROUP_MARSHAL=0xa)");
    }

    // =================================================================================================
    // T3 -- x matches, y does NOT: not at the exit tile -- GROUP_MARSHAL, and the built_flags==3 gate
    // is never reached (built_flags left at a value that would pass it, to prove the SHORT-CIRCUIT).
    // =================================================================================================
    {
        Seed s;
        s.unit_x      = 12;
        s.exit_tile_x = 12; // x matches
        s.unit_y      = 5;
        s.exit_tile_y = 6; // y does NOT match
        s.built_flags = 3; // would pass the second gate if reached -- it must not be
        seed_and_run(fx, s);
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0xa,
           "T3: x matches, y mismatched -> NOT at exit tile -> GROUP_MARSHAL (0x0047feae JNZ taken)");
    }

    // =================================================================================================
    // T4 -- y matches, x does NOT: the mirror of T3.
    // =================================================================================================
    {
        Seed s;
        s.unit_x      = 8;
        s.exit_tile_x = 9; // x does NOT match
        s.unit_y      = 15;
        s.exit_tile_y = 15; // y matches
        s.built_flags = 3;
        seed_and_run(fx, s);
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0xa,
           "T4: y matches, x mismatched -> NOT at exit tile -> GROUP_MARSHAL");
    }

    // =================================================================================================
    // T5 -- home_storage_slot / b_index indexing: a DIFFERENT slot/building than T1's, proving the
    // function re-derives the storage/building row from the CURRENT unit's own home_storage_slot
    // rather than reading a fixed slot 0.
    // =================================================================================================
    {
        Seed s;
        s.home_storage_slot = 7;
        s.b_index           = 21;
        s.unit_x = s.exit_tile_x = 30;
        s.unit_y = s.exit_tile_y = 31;
        s.built_flags            = 3;
        seed_and_run(fx, s);
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x24,
           "T5: non-zero home_storage_slot/b_index still resolve correctly -> ENTER_STORAGE_BEGIN");
    }

    // =================================================================================================
    // T6 -- non-corruption: this function writes NOTHING tracked (its own body has 0 write cells --
    // see the header's NOT SHADOWABLE note). A guard unit's home_storage_slot, and the guard's own
    // storage/building rows, must read back untouched across the most active case (T1).
    // =================================================================================================
    {
        Seed s;
        s.unit_x = s.exit_tile_x = 10;
        s.unit_y = s.exit_tile_y = 20;
        s.built_flags            = 3;
        seed_and_run(fx, s);

        const unit &g = fx.u(GUARD_PLAYER, GUARD_INDEX);
        ck_eq((uint32_t)g.home_storage_slot, 9u, "T6: guard unit's home_storage_slot untouched");

        const unit_storage &gst = fx.storage[GUARD_PLAYER * STORAGE_PER_PLAYER + 9];
        ck_eq((uint32_t)gst.exit_tile_x, 0u, "T6: guard storage row untouched (still zero from reset)");
        ck_eq((uint32_t)gst.b_index, 0u, "T6: guard storage row's b_index untouched");
    }
}

} // namespace mh::sim::test
