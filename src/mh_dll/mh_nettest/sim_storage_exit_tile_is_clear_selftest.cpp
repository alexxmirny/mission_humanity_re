//
// sim_storage_exit_tile_is_clear_selftest.cpp -- `simtest` oracle for
// llm_strat_storage_exit_tile_is_clear @0x00489cff (sim/sim_storage_exit_query.h/.cpp,
// RI-SIM / SIM1-G3).
//
// NOT SHADOWABLE (0 direct AND 0 transitive write cells -- confirmed by reading the whole
// 0xc5-byte body: two CMPs, two conditional branches, no CALL besides the shared, inert
// utils_assert_stack_capacity prologue). This is the sole coverage this function gets -- no rig
// run will ever cross-check it.
//
// SCOPE: `passable[(x<<8)|y] != 0 && tile_at(x,y).building == 0`, in the ORIGINAL's own
// short-circuit order (gate 1 = passable, gate 2 = no building; gate 2 is unreached if gate 1
// fails).
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM tmp/decomp_sim/llm_strat_storage_exit_tile_is_clear_00489cff.asm
// -- every assertion below cites the instruction address(es) it pins. NOT read off the .cpp body.
//
#include "sim/sim_storage_exit_query.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr uint16_t GUARD_PLAYER = 3;
constexpr int32_t  GUARD_SLOT   = 9;
constexpr int32_t  GUARD_TILE_X = 200;
constexpr int32_t  GUARD_TILE_Y = 201;

struct Seed {
    uint16_t player       = 1;
    int32_t  storage_slot = 4;
    int32_t  exit_tile_x  = 10;
    int32_t  exit_tile_y  = 20;
    uint8_t  passable_val = 1;
    uint16_t building_val = 0;
};

uint32_t seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    // Guard row/tile: never touched by the seed below, must read back untouched after every run
    // (this function writes nothing, but a corrupted fixture write would still show up here).
    unit_storage &guard_st                          = fx.storage[GUARD_PLAYER * STORAGE_PER_PLAYER + GUARD_SLOT];
    guard_st.exit_tile_x                            = 77;
    guard_st.exit_tile_y                            = 78;
    fx.passable[(GUARD_TILE_X << 8) | GUARD_TILE_Y] = 1;
    fx.t(GUARD_TILE_X, GUARD_TILE_Y).building       = 55;

    unit_storage &st = fx.storage[s.player * STORAGE_PER_PLAYER + s.storage_slot];
    st.exit_tile_x   = s.exit_tile_x;
    st.exit_tile_y   = s.exit_tile_y;

    fx.passable[(s.exit_tile_x << 8) | s.exit_tile_y] = s.passable_val;
    fx.t(s.exit_tile_x, s.exit_tile_y).building       = s.building_val;

    return detail::storage_exit_tile_is_clear(fx.view(), s.player, s.storage_slot);
}

} // namespace

void run_storage_exit_tile_is_clear_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- passable, no building: both gates pass -> 1 (0x00489d5a fallthrough, then
    // 0x00489d9e JZ taken -> 0x00489da2 -> return 1).
    // =================================================================================================
    {
        Seed s;
        s.passable_val = 1;
        s.building_val = 0;
        ck_eq(seed_and_run(fx, s), 1u, "T1: passable + no building -> clear");
    }

    // =================================================================================================
    // T2 -- NOT passable: gate 1 fails (0x00489d5a JZ taken -> 0x00489da0 -> return 0)
    // UNCONDITIONALLY, regardless of the building field -- gate 2 is never reached. Building is set
    // to 0 here (would pass gate 2 on its own) to prove gate 1 alone decides it.
    // =================================================================================================
    {
        Seed s;
        s.passable_val = 0;
        s.building_val = 0;
        ck_eq(seed_and_run(fx, s), 0u, "T2: not passable (building clear) -> blocked at gate 1");
    }

    // =================================================================================================
    // T3 -- passable but a building occupies the tile: gate 1 passes, gate 2 fails
    // (0x00489d9e JZ not taken -> falls through to 0x00489da0 -> return 0).
    // =================================================================================================
    {
        Seed s;
        s.passable_val = 1;
        s.building_val = 42;
        ck_eq(seed_and_run(fx, s), 0u, "T3: passable but building present -> blocked at gate 2");
    }

    // =================================================================================================
    // T4 -- neither passable nor clear of a building: still 0 (gate 1 short-circuits before gate 2
    // is even consulted -- distinguishes "gate 1 alone" from "both gates ANDed lazily").
    // =================================================================================================
    {
        Seed s;
        s.passable_val = 0;
        s.building_val = 42;
        ck_eq(seed_and_run(fx, s), 0u, "T4: neither passable nor building-free -> blocked");
    }

    // =================================================================================================
    // T5 -- different (player, storage_slot) picks a different unit_storage row's exit tile, proving
    // the function reads ITS OWN row's exit_tile_x/y rather than a fixed/shared location.
    // =================================================================================================
    {
        Seed s;
        s.player       = 2;
        s.storage_slot = 11;
        s.exit_tile_x  = 100;
        s.exit_tile_y  = 5;
        s.passable_val = 1;
        s.building_val = 0;
        ck_eq(seed_and_run(fx, s), 1u, "T5: a different (player,slot) row's own exit tile -> clear");
    }

    // =================================================================================================
    // T6 -- non-corruption: this function writes NOTHING tracked (0 direct/transitive write cells --
    // see the header's NOT SHADOWABLE note). The guard storage row and guard tile seeded in
    // seed_and_run() above must read back exactly as seeded after the most active case (T1).
    // =================================================================================================
    {
        Seed s;
        s.passable_val = 1;
        s.building_val = 0;
        seed_and_run(fx, s);

        const unit_storage &gst = fx.storage[GUARD_PLAYER * STORAGE_PER_PLAYER + GUARD_SLOT];
        ck_eq((uint32_t)gst.exit_tile_x, 77u, "T6: guard storage row's exit_tile_x untouched");
        ck_eq((uint32_t)gst.exit_tile_y, 78u, "T6: guard storage row's exit_tile_y untouched");
        ck_eq((uint32_t)fx.passable[(GUARD_TILE_X << 8) | GUARD_TILE_Y], 1u,
              "T6: guard tile's passable byte untouched");
        ck_eq((uint32_t)fx.t(GUARD_TILE_X, GUARD_TILE_Y).building, 55u,
              "T6: guard tile's building field untouched");
    }
}

} // namespace mh::sim::test
