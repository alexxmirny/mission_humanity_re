//
// sim_storage_can_land_selftest.cpp -- `simtest` oracle for llm_strat_storage_can_land
// @0x0048ae90 (sim/sim_storage_can.h/.cpp, RI-SIM / SIM1-G3).
//
// NOT SHADOWABLE (0 direct AND 0 transitive write cells -- confirmed by reading the whole
// ~0x18d-byte body: a pure boolean gate over buildings[]/unit_storage[] cfg fields, no calls at all
// besides the shared, inert utils_assert_stack_capacity prologue). This is the sole coverage this
// function gets, per the batch's un-armable path -- no rig run will ever cross-check it.
//
// SCOPE: the bldg_type == BUILDING_TYPE_A_PORT (0xc) special case (gate1 = online_state==1 ||
// online_state==2, vs gate1=true for every other type), then the shared door_mutex_unit /
// occupancy-cap / gate1 / built_flags chain, in the ORIGINAL's own short-circuit order.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM tmp/decomp_sim/llm_strat_storage_can_land_0048ae90.asm --
// every assertion below cites the instruction address(es) it pins. NOT read off the .cpp body.
//
#include "sim/sim_storage_can.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr uint16_t GUARD_PLAYER = 3;

struct Seed {
    uint16_t player          = 1;
    int32_t  storage_slot    = 4;
    int32_t  b_index         = 6;
    uint8_t  bldg_type       = 5; // anything != BUILDING_TYPE_A_PORT(0xc)
    int16_t  online_state    = 2;
    int32_t  door_mutex_unit = 0;
    int32_t  occupancy       = 0;
    uint8_t  built_flags     = 3; // BUILT_FLAGS_OPERATIONAL
};

int32_t seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    unit_storage &guard_st = fx.storage[GUARD_PLAYER * STORAGE_PER_PLAYER + 9];
    guard_st.b_index       = 0;

    unit_storage &st   = fx.storage[s.player * STORAGE_PER_PLAYER + s.storage_slot];
    st.b_index         = s.b_index;
    st.door_mutex_unit = s.door_mutex_unit;
    st.occupancy       = s.occupancy;

    building &b    = fx.b(s.player, s.b_index);
    b.building_id  = static_cast<int16_t>(s.b_index);
    b.online_state = s.online_state;
    b.built_flags  = s.built_flags;

    fx.cfg_buildings[s.b_index].type = s.bldg_type;

    return detail::storage_can_land(fx.view(), static_cast<int32_t>(s.player), /*unit_index=*/0,
                                    s.storage_slot);
}

} // namespace

void run_storage_can_land_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- ordinary (non-A_PORT) building, fully open (door free, occupancy 0, built_flags==3):
    // allowed (0x0048af79 -> gate1=true, then the shared chain all passes -> 1).
    // =================================================================================================
    {
        Seed s;
        s.bldg_type = 5;
        ck_eq((uint32_t)seed_and_run(fx, s), 1u, "T1: ordinary building, fully open -> allowed");
    }

    // =================================================================================================
    // T2 -- A_PORT (0xc), online_state==1 (docking): gate1 true via the A_PORT arm's first disjunct
    // (0x0048af27-0x0048af2f JZ taken) -> allowed.
    // =================================================================================================
    {
        Seed s;
        s.bldg_type    = 0x0c;
        s.online_state = 1;
        ck_eq((uint32_t)seed_and_run(fx, s), 1u, "T2: A_PORT online_state==1 (docking) -> allowed");
    }

    // =================================================================================================
    // T3 -- A_PORT (0xc), online_state==2 (loading): gate1 true via the second disjunct
    // (0x0048af31-0x0048af67 second JZ taken) -> allowed.
    // =================================================================================================
    {
        Seed s;
        s.bldg_type    = 0x0c;
        s.online_state = 2;
        ck_eq((uint32_t)seed_and_run(fx, s), 1u, "T3: A_PORT online_state==2 (loading) -> allowed");
    }

    // =================================================================================================
    // T4 -- A_PORT (0xc), online_state==0 (neither docking nor loading): gate1 FALSE
    // (0x0048af6d) -> rejected at the gate1 check (0x0048afc1), even though door/occupancy/built_flags
    // would all otherwise pass.
    // =================================================================================================
    {
        Seed s;
        s.bldg_type    = 0x0c;
        s.online_state = 0;
        ck_eq((uint32_t)seed_and_run(fx, s), 0u,
              "T4: A_PORT online_state==0 (neither docking nor loading) -> rejected (gate1 false)");
    }

    // =================================================================================================
    // T5 -- door_mutex_unit != 0: rejected FIRST, before gate1/occupancy/built_flags are even
    // consulted (0x0048af9a) -- ordinary building type so gate1 would otherwise be true.
    // =================================================================================================
    {
        Seed s;
        s.bldg_type       = 5;
        s.door_mutex_unit = 42;
        ck_eq((uint32_t)seed_and_run(fx, s), 0u, "T5: door_mutex_unit!=0 -> rejected regardless of gate1");
    }

    // =================================================================================================
    // T6 -- occupancy AT the cap (50, STORAGE_OCCUPANCY_CAP): rejected -- the comparison is `>=`, not
    // `>` (0x0048afb9).
    // =================================================================================================
    {
        Seed s;
        s.bldg_type = 5;
        s.occupancy = 50; // STORAGE_OCCUPANCY_CAP (file-local constant in sim_storage_can.cpp, not exported)
        ck_eq((uint32_t)seed_and_run(fx, s), 0u, "T6: occupancy==CAP (strict >=) -> rejected");
    }

    // =================================================================================================
    // T7 -- occupancy one below the cap: still allowed (proves the boundary is really >=, not >).
    // =================================================================================================
    {
        Seed s;
        s.bldg_type = 5;
        s.occupancy = 50 - 1; // STORAGE_OCCUPANCY_CAP-1 (file-local constant in sim_storage_can.cpp, not exported)
        ck_eq((uint32_t)seed_and_run(fx, s), 1u, "T7: occupancy==CAP-1 -> still allowed");
    }

    // =================================================================================================
    // T8 -- built_flags != BUILT_FLAGS_OPERATIONAL(3): rejected at the FINAL gate (0x0048aff8), even
    // though door/occupancy/gate1 all pass.
    // =================================================================================================
    {
        Seed s;
        s.bldg_type   = 5;
        s.built_flags = 1; // connected but not staffed
        ck_eq((uint32_t)seed_and_run(fx, s), 0u,
              "T8: built_flags(1)!=OPERATIONAL(3) -> rejected at the final gate");
    }

    // =================================================================================================
    // T9 -- non-corruption: this function writes NOTHING tracked (0 direct/transitive write cells --
    // see the header's NOT SHADOWABLE note). A guard storage row and building must read back
    // untouched across the most active case (T1).
    // =================================================================================================
    {
        Seed s;
        s.bldg_type = 5;
        seed_and_run(fx, s);

        const unit_storage &gst = fx.storage[GUARD_PLAYER * STORAGE_PER_PLAYER + 9];
        ck_eq((uint32_t)gst.b_index, 0u, "T9: guard storage row untouched");
        ck_eq((uint32_t)gst.door_mutex_unit, 0u, "T9: guard storage row's door_mutex_unit untouched");
    }
}

} // namespace mh::sim::test
