//
// sim_prod_shuttle_fuel_apply_selftest.cpp -- `simtest` oracle for llm_prod_shuttle_fuel_apply
// @0x00493705 (sim/sim_prod_shuttle_fuel_apply.h/.cpp, RI-SIM / SIM1D).
//
// THIS FUNCTION HAS ZERO CALLS ACROSS BOTH THE 15000- AND 30000-STEP ALL-AI SOAKS (reaching it
// needs an AI to issue a departure order on an A_SHUTTLE/H_SHUTTLE/A_PORT/H_PORT/A_MOTHER/H_MOTHER
// building, unconfirmed for skirmish AI) -- this offline oracle is the only evidence this function
// will ever have.
//
// EXPECTED BEHAVIOUR from the header banner's own derivation (sim_prod_shuttle_fuel_apply.h):
//   0x00493724-0x00493735: G_PLANET_INDEX == dest_planet -> return 0, no fuel spent, building_id
//     lookup and fuel walk never happen at all.
//   0x0049373a-0x00493754: building_id = buildings[player][building_index].building_id (0x6aa4
//     row stride * player + 0x111 col stride * building_index + base 0xc3d2a2).
//   0x0049375e-0x004937ab: walk cb.fuel[i] (cb = cfg_buildings[building_id]), id-read-before-
//     bound-check: read fuel[i].id first, HARD BREAK (not skip/continue) on ==0, THEN test
//     i<CFG_RESOURCE_SLOTS(7) and break if not satisfied. Per surviving slot:
//     resource_add(player, fuel[i].id, fuel[i].val).
//   Return is always literal 0 on both paths.
//
// PINS THE 2026-08-14 STRUCT FIX: `cfg_final_struct_Building.fuel` was mis-declared at +0x6a5
// size [5] (this function's own address arithmetic proved the real base is +0x6a9, size [7] --
// see the header banner's RESOLVED notes). Slot index 6 (the array's LAST element) did not exist
// under the wrong 5-slot declaration -- T4 below walks all 7 slots and asserts slot 6's id/val
// specifically, so a regression back to size [5] (fuel[6] reading garbage/adjacent memory instead
// of the intended 7th cfg-authored slot) fails here.
//
// NOT EXERCISED: the loop's "read id at i==7 before the bound check" over-read (0x00493776 fires
// before 0x0049377c/0x00493780) has no fixture-observable difference from a combined-condition
// `for (i=0; i<7 && fuel[i].id; ++i)` translation in any case constructible here -- the only value
// that over-read could ever consume is whatever bytes follow `fuel[6]` in `cfg_building` (the
// `velocity` field), and `sim_fixture::reset()` memsets the whole `cfg_buildings` vector to zero,
// so that phantom index-7 read always yields id==0 and never reaches a call either way. Not pinned
// separately -- see the report to the conductor.
//
#include "sim/sim_prod_shuttle_fuel_apply.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct resource_add_call {
    int32_t player;
    int32_t resource_id;
    int32_t amount;
};
std::vector<resource_add_call> g_resource_add_calls;

void rec_resource_add(int32_t player, int32_t resource_id, int32_t amount) {
    g_resource_add_calls.push_back({player, resource_id, amount});
}

const prod_shuttle_fuel_apply_calls g_calls = {
    &rec_resource_add,
};

constexpr uint16_t PLAYER            = 3;
constexpr int32_t  BUILDING_INDEX    = 5;
constexpr uint16_t BUILDING_ID       = 42;
constexpr uint16_t OTHER_BUILDING_ID = 55; // for the player/building_index swap-check in T2
constexpr int32_t  PLANET_INDEX_VAL  = 9;
constexpr int32_t  DEST_PLANET_DIFF  = 4; // != PLANET_INDEX_VAL

void reset_recorders() { g_resource_add_calls.clear(); }

// Sets up buildings[PLAYER][BUILDING_INDEX].building_id = BUILDING_ID so building_of(v, PLAYER,
// BUILDING_INDEX) resolves cfg_buildings[BUILDING_ID].
void wire_building(sim_fixture &fx) { fx.b(PLAYER, BUILDING_INDEX).building_id = BUILDING_ID; }

void set_fuel(sim_fixture &fx, uint16_t building_id, int slot, uint32_t id, int32_t val) {
    fx.cfg_buildings[building_id].fuel[slot].id  = id;
    fx.cfg_buildings[building_id].fuel[slot].val = val;
}

} // namespace

void run_prod_shuttle_fuel_apply_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- same-planet bail: G_PLANET_INDEX == dest_planet returns 0 WITHOUT resolving building_id or
    // touching the fuel walk at all. cfg_buildings[BUILDING_ID] is seeded with a real fuel entry so a
    // broken/inverted bail check would show up as a spurious call.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.planet_index = PLANET_INDEX_VAL;
        wire_building(fx);
        set_fuel(fx, BUILDING_ID, 0, /*id=*/5, /*val=*/50);

        const int32_t ret = detail::prod_shuttle_fuel_apply(fx.view(), g_calls, PLAYER, BUILDING_INDEX,
                                                            /*dest_planet=*/PLANET_INDEX_VAL);

        ck_eq((uint32_t)ret, 0u, "T1: return is literal 0 on the same-planet path, 0x004937b4-0x004937be");
        ck(g_resource_add_calls.empty(),
           "T1: same-planet bail (planet_index==dest_planet) -- resource_add never called, "
           "0x00493724-0x0049372c");
    }

    // =================================================================================================
    // T2 -- different planet, cfg_buildings[building_id].fuel[0].id==0 (immediate sentinel): the
    // building_id lookup fires, but the walk breaks on its first read with zero calls. Also proves
    // building_of's (player, building_index) argument ORDER is not swapped: a building planted at the
    // TRANSPOSED coordinates (BUILDING_INDEX, PLAYER) resolves to a DIFFERENT cfg_buildings row with
    // real nonzero fuel -- if the lookup swapped its two index args, this case would observe calls.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.planet_index = PLANET_INDEX_VAL;
        wire_building(fx); // buildings[PLAYER][BUILDING_INDEX].building_id = BUILDING_ID, fuel all-zero

        // Swap-check decoy: buildings[BUILDING_INDEX][PLAYER] (transposed) -> a DIFFERENT building_id
        // whose cfg row has real fuel. A player/building_index swap in the 0x6aa4/0x111 address math
        // would land here instead and produce spurious calls.
        fx.b(BUILDING_INDEX, PLAYER).building_id = OTHER_BUILDING_ID;
        set_fuel(fx, OTHER_BUILDING_ID, 0, /*id=*/77, /*val=*/500);

        const int32_t ret =
            detail::prod_shuttle_fuel_apply(fx.view(), g_calls, PLAYER, BUILDING_INDEX, DEST_PLANET_DIFF);

        ck_eq((uint32_t)ret, 0u, "T2: return is literal 0 on the zero-fuel path, 0x004937ad-0x004937be");
        ck(g_resource_add_calls.empty(),
           "T2: fuel[0].id==0 -- immediate hard break, 0x00493776/0x0049377a, AND building_of(player, "
           "building_index) resolved the untransposed row (0x0049373a-0x0049374b address math)");
    }

    // =================================================================================================
    // T3 -- mixed walk: two real slots (the second with amount==0, proving val is NOT itself a
    // sentinel -- only id is), then an id==0 sentinel, then a FOURTH slot with a real nonzero id that
    // must NEVER be reached -- proving the sentinel is a hard BREAK, not a "skip this slot and keep
    // going" continue. Also pins the resource_add argument order: (player, id, val), id first.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.planet_index = PLANET_INDEX_VAL;
        wire_building(fx);
        set_fuel(fx, BUILDING_ID, 0, /*id=*/7, /*val=*/100);
        set_fuel(fx, BUILDING_ID, 1, /*id=*/8, /*val=*/0);   // amount==0 boundary -- must still call
        set_fuel(fx, BUILDING_ID, 2, /*id=*/0, /*val=*/999); // sentinel; val must be irrelevant
        set_fuel(fx, BUILDING_ID, 3, /*id=*/9, /*val=*/300); // must NEVER be read (break, not skip)

        const int32_t ret =
            detail::prod_shuttle_fuel_apply(fx.view(), g_calls, PLAYER, BUILDING_INDEX, DEST_PLANET_DIFF);

        ck_eq((uint32_t)ret, 0u, "T3: return is literal 0 on the partial-walk path, 0x004937ad-0x004937be");
        ck_eq((uint32_t)g_resource_add_calls.size(), 2u,
              "T3: exactly 2 calls -- fuel[2].id==0 is a HARD BREAK (0x00493776/0x0049377a), fuel[3] "
              "(a real nonzero id past the sentinel) is never read -- not a skip/continue");
        if (g_resource_add_calls.size() >= 2) {
            ck_eq((uint32_t)g_resource_add_calls[0].player, (uint32_t)PLAYER,
                  "T3: call[0] player arg == player, 0x0049379c");
            ck_eq((uint32_t)g_resource_add_calls[0].resource_id, 7u,
                  "T3: call[0] resource_id == fuel[0].id (offset+0), 0x0049376d/0x00493799");
            ck_eq((uint32_t)g_resource_add_calls[0].amount, 100u,
                  "T3: call[0] amount == fuel[0].val (offset+4), 0x00493793");
            ck_eq((uint32_t)g_resource_add_calls[1].resource_id, 8u,
                  "T3: call[1] resource_id == fuel[1].id == 8, slot order preserved");
            ck_eq((uint32_t)g_resource_add_calls[1].amount, 0u,
                  "T3: call[1] amount == fuel[1].val == 0 -- val==0 is NOT a sentinel, only id==0 is");
        }
    }

    // =================================================================================================
    // T4 -- REGRESSION PIN for the 2026-08-14 struct fix: cfg_final_struct_Building.fuel was corrected
    // from a mis-declared [5]-slot array at +0x6a5 to the true [7]-slot array at +0x6a9. All 7 slots
    // are given DISTINCT nonzero (id, val) pairs so index 6 -- the slot that did not exist at all under
    // the old 5-slot declaration -- is walked and its exact id/val are asserted. A regression to size
    // [5] (or any off-by-one in the corrected +0x6a9 base) makes this case fail either by never issuing
    // a 7th call or by reading slot 6 from the wrong memory.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.planet_index = PLANET_INDEX_VAL;
        wire_building(fx);
        for (int i = 0; i < CFG_RESOURCE_SLOTS; ++i) {
            set_fuel(fx, BUILDING_ID, i, /*id=*/(uint32_t)(21 + i), /*val=*/(int32_t)(201 + i));
        }

        const int32_t ret =
            detail::prod_shuttle_fuel_apply(fx.view(), g_calls, PLAYER, BUILDING_INDEX, DEST_PLANET_DIFF);

        ck_eq((uint32_t)ret, 0u, "T4: return is literal 0 on the full 7-slot walk, 0x004937ad-0x004937be");
        ck_eq((uint32_t)g_resource_add_calls.size(), 7u,
              "T4: all 7 slots produce a call -- loop bound i<CFG_RESOURCE_SLOTS(7), 0x0049377c/0x00493780");
        if (g_resource_add_calls.size() == 7) {
            for (int i = 0; i < 7; ++i) {
                char msg_id[160];
                char msg_val[160];
                char msg_player[160];
                std::snprintf(msg_id, sizeof(msg_id),
                              "T4: call[%d] resource_id == fuel[%d].id == %u, 0x0049376d", i, i, 21u + i);
                std::snprintf(msg_val, sizeof(msg_val),
                              "T4: call[%d] amount == fuel[%d].val == %d, 0x00493793", i, i, 201 + i);
                std::snprintf(msg_player, sizeof(msg_player),
                              "T4: call[%d] player arg == player (not swapped with id/val), 0x0049379c", i);
                ck_eq((uint32_t)g_resource_add_calls[i].resource_id, (uint32_t)(21 + i), msg_id);
                ck_eq((uint32_t)g_resource_add_calls[i].amount, (uint32_t)(201 + i), msg_val);
                ck_eq((uint32_t)g_resource_add_calls[i].player, (uint32_t)PLAYER, msg_player);
            }
            // The explicit regression pin: slot 6 is the array's LAST element, unreachable under the
            // old mis-declared [5]-slot layout (+0x6a5). This is the check that fails if `fuel` ever
            // regresses to size [5] or the offset drifts off +0x6a9.
            ck_eq((uint32_t)g_resource_add_calls[6].resource_id, 27u,
                  "T4 REGRESSION PIN: fuel[6].id == 27 -- slot 6 did not exist under the old mis-declared "
                  "cfg_struct_resource[5]@+0x6a5 layout; only exists at the corrected [7]@+0x6a9 (see "
                  "sim_prod_shuttle_fuel_apply.h RESOLVED notes), read at 0x0049376d");
            ck_eq((uint32_t)g_resource_add_calls[6].amount, 207u,
                  "T4 REGRESSION PIN: fuel[6].val == 207, read at 0x00493793 -- same slot-6 fix");
        }
    }
}

} // namespace mh::sim::test
