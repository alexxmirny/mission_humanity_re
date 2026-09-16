//
// sim_storage_launch_parked_to_orbit_selftest.cpp -- offline `simtest` oracle for
// llm_strat_storage_launch_parked_to_orbit @0x0048f952 (sim/sim_storage_launch_parked_to_orbit.h/.cpp,
// RI-SIM / SIM1D batch D).
//
// arm_ready:false -- shadow_region_closure.py bounds this at 2 functions / 1 region, but its only
// caller (llm_strat_prod_bldg_depart_finalize) is a DO-NOT-ARM still-original-executing caller, and a
// 15000-step all-AI soak recorded ZERO live calls into this function (reaching it needs a shuttle
// completing a full depart-and-park cycle). This offline oracle is the ONLY evidence this function
// will ever have -- see the header banner's "DECLARED NEED" section.
//
// EXPECTED BEHAVIOUR from the header banner's derivation (sim_storage_launch_parked_to_orbit.h):
//   sub_id = buildings[player][building_index].sub_id;                       // 0x0048f971-0x0048f98b
//   for (i = 0; i < unit_storage[player][sub_id].docked_count; ++i) {        // 0x0048f995-0x0048f9b1
//       unit_index = unit_storage[player][sub_id].docked_units[i];           // 0x0048f9c0-0x0048f9e1
//       if (units[player][unit_index].state == PARKED(0x1f)) {               // 0x0048f9f7-0x0048f9ff
//           llm_unit_force_disembark(player, unit_index);                    // 0x0048fa01-0x0048fa0d
//           units[player][unit_index].order = ASCEND_TO_ORBIT(0x31);         // 0x0048fa11-0x0048fa29
//           return unit_index;                                               // 0x0048fa2c-0x0048fa44
//       }
//   }
//   return 0;                                                                // 0x0048fa33-0x0048fa44
//
// NOTE ON THE TASK BRIEFING'S park_x/park_y LEAD: the briefing that assigned this oracle said the
// function "reads map_object_unit_storage's park_x / park_y at +0xdc / +0xe0" and asked for a
// swap/shift-sensitive case on that pair. The .asm (tmp/decomp_sim/llm_strat_storage_launch_parked_
// to_orbit_0048f952.asm) does NOT touch those offsets anywhere -- the only unit_storage fields this
// function reads are docked_count (+0x4, literal 0xc727c4) and docked_units[i] (+0x8, literal
// 0xc727c8); the only building field is sub_id (+0xc6, literal 0xc3d366); the only unit fields are
// state (+0x6, literal 0xdd8c4e) and order (+0x4, literal 0xdd8c4c). This is flagged as a DIVERGENCE
// in the returned report rather than silently honoured or silently dropped -- no park_x/park_y case is
// written here because the .asm gives no such case to write. (The park_x/park_y GAP-CLOSE the briefing
// refers to almost certainly belongs to a sibling in the same batch --
// sim_storage_dock_unit_at_building.cpp / sim_storage_exit_placement.cpp both genuinely read that pair
// at exactly +0xdc/+0xe0.)
//
#include "sim/sim_storage_launch_parked_to_orbit.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- the one outward call this function makes -----------------------------------------------------
struct disembark_call {
    uint32_t player;
    int32_t  unit_index;
};
std::vector<disembark_call> g_disembark_calls;

// T6's order-before/after-the-callee probe: a pointer the recorder reads THROUGH, so it observes
// whatever `own.unit_at(...).order` currently holds at the moment the callee runs -- same backing
// memory as `fx.u()` (both index the same `units` vector by player*UNITS_PER_PLAYER+index).
unit    *g_order_probe_unit        = nullptr;
uint16_t g_order_seen_at_disembark = 0xffff; // sentinel: "the probe was never read"

void rec_unit_force_disembark(uint32_t player, int32_t unit_index) {
    g_disembark_calls.push_back({player, unit_index});
    if (g_order_probe_unit != nullptr) g_order_seen_at_disembark = g_order_probe_unit->order;
}

const storage_launch_parked_to_orbit_calls g_calls = {
    &rec_unit_force_disembark,
};

// player != building_index != sub_id, all distinct, so an index-confusion translation (using
// building_index as the storage slot, or hardcoding player 0) disagrees with the fixture.
constexpr uint16_t PLAYER         = 3;
constexpr int32_t  BUILDING_INDEX = 6;
constexpr uint8_t  SUB_ID         = 4;

constexpr uint16_t UNIT_STATE_PARKED          = 0x1f;
constexpr uint16_t UNIT_STATE_ASCEND_TO_ORBIT = 0x31;

void reset_recorders() {
    g_disembark_calls.clear();
    g_order_probe_unit        = nullptr;
    g_order_seen_at_disembark = 0xffff;
}

void seed_building(sim_fixture &fx) { fx.b(PLAYER, BUILDING_INDEX).sub_id = SUB_ID; }

unit_storage &storage_row(sim_fixture &fx) { return fx.storage[PLAYER * STORAGE_PER_PLAYER + SUB_ID]; }

} // namespace

void run_storage_launch_parked_to_orbit_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- empty docked list (docked_count == 0): exhaustion return 0, disembark never called. Also
    // covers the "return 0" arm of the load-bearing return value.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_building(fx);
        storage_row(fx).docked_count = 0;

        sim_store own    = fx.store();
        int32_t   result = detail::storage_launch_parked_to_orbit(fx.view(), own, g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)result, 0u, "T1: empty docked list -> return 0, 0x0048fa33-0x0048fa44");
        ck_eq((uint32_t)g_disembark_calls.size(), 0u,
              "T1: unit_force_disembark never called on an empty list, 0x0048fa01");
    }

    // =================================================================================================
    // T2 -- state boundary from BOTH sides (0x1e just below PARKED, 0x20 just above) never match; the
    // exact PARKED(0x1f) match at the LAST scanned entry (index 2 of 3) is found, proving the loop
    // walks past non-matches instead of stopping (LAB_0048f9b8 continue) and that the returned value is
    // docked_units[i], NOT the loop counter i.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_building(fx);
        unit_storage     &st = storage_row(fx);
        constexpr int32_t UA = 21, UB = 34, UC = 55; // distinct unit indices, none equal to their slot i
        st.docked_count        = 3;
        st.docked_units[0]     = UA;
        st.docked_units[1]     = UB;
        st.docked_units[2]     = UC;
        fx.u(PLAYER, UA).state = 0x1e;              // just BELOW PARKED -- must not match
        fx.u(PLAYER, UB).state = 0x20;              // just ABOVE PARKED -- must not match
        fx.u(PLAYER, UC).state = UNIT_STATE_PARKED; // exact match, third/last entry
        fx.u(PLAYER, UA).order = 0x11;
        fx.u(PLAYER, UB).order = 0x22;
        fx.u(PLAYER, UC).order = 0x33; // must be overwritten to 0x31

        sim_store own    = fx.store();
        int32_t   result = detail::storage_launch_parked_to_orbit(fx.view(), own, g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)result, (uint32_t)UC,
              "T2: returns docked_units[2] (UC=55), not the loop index 2, 0x0048fa2c");
        ck_eq((uint32_t)g_disembark_calls.size(), 1u, "T2: unit_force_disembark called exactly once, 0x0048fa01");
        ck_eq((uint32_t)g_disembark_calls[0].unit_index, (uint32_t)UC,
              "T2: disembark called with UC, not UA/UB, 0x0048fa04 EDX<-EBP-0x14");
        ck_eq((uint32_t)fx.u(PLAYER, UC).order, (uint32_t)UNIT_STATE_ASCEND_TO_ORBIT,
              "T2: UC.order set to ASCEND_TO_ORBIT(0x31), 0x0048fa20");
        ck_eq((uint32_t)fx.u(PLAYER, UA).order, 0x11u,
              "T2: UA.order untouched -- state 0x1e (below PARKED) never matched, 0x0048f9ff JNZ");
        ck_eq((uint32_t)fx.u(PLAYER, UB).order, 0x22u,
              "T2: UB.order untouched -- state 0x20 (above PARKED) never matched, 0x0048f9ff JNZ");
    }

    // =================================================================================================
    // T3 -- first-match short-circuit: when docked_units[0] is ALREADY parked, the function returns
    // immediately and never re-examines the rest of the list (a second PARKED unit at index 1 is left
    // completely untouched).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_building(fx);
        unit_storage     &st = storage_row(fx);
        constexpr int32_t UD = 12, UE = 88;
        st.docked_count        = 2;
        st.docked_units[0]     = UD;
        st.docked_units[1]     = UE;
        fx.u(PLAYER, UD).state = UNIT_STATE_PARKED;
        fx.u(PLAYER, UE).state = UNIT_STATE_PARKED;
        fx.u(PLAYER, UE).order = 0x44;

        sim_store own    = fx.store();
        int32_t   result = detail::storage_launch_parked_to_orbit(fx.view(), own, g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)result, (uint32_t)UD,
              "T3: first match wins (docked_units[0]=UD), not the later UE, 0x0048f9ff early exit");
        ck_eq((uint32_t)g_disembark_calls.size(), 1u,
              "T3: exactly one disembark call -- loop does not continue past the first match");
        ck_eq((uint32_t)g_disembark_calls[0].unit_index, (uint32_t)UD, "T3: disembark called with UD only");
        ck_eq((uint32_t)fx.u(PLAYER, UE).order, 0x44u, "T3: UE.order untouched -- loop never reached index 1");
    }

    // =================================================================================================
    // T4 -- loop bound is EXCLUSIVE (i < docked_count, 0x0048f9ab CMP/0x0048f9b1 JL): an entry planted
    // AT index==docked_count that WOULD match is never scanned.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_building(fx);
        unit_storage     &st = storage_row(fx);
        constexpr int32_t UF = 7, UG = 19, UH = 41;
        st.docked_count        = 2;
        st.docked_units[0]     = UF;
        st.docked_units[1]     = UG;
        st.docked_units[2]     = UH; // beyond docked_count -- must never be read
        fx.u(PLAYER, UF).state = 0x10;
        fx.u(PLAYER, UG).state = 0x10;
        fx.u(PLAYER, UH).state = UNIT_STATE_PARKED; // would match under an off-by-one (i<=docked_count)

        sim_store own    = fx.store();
        int32_t   result = detail::storage_launch_parked_to_orbit(fx.view(), own, g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)result, 0u,
              "T4: exhausted at docked_count==2 -- docked_units[2] never scanned, 0x0048f9ab/0x0048f9b1 JL not JLE");
        ck_eq((uint32_t)g_disembark_calls.size(), 0u,
              "T4: disembark never called -- the entry past docked_count is ignored");
    }

    // =================================================================================================
    // T5 -- storage row is selected via building.sub_id (0x0048f984), NOT via building_index directly.
    // The CORRECT row (indexed by sub_id) is empty; a decoy PARKED unit is planted in the row that
    // building_index would name if the sub_id indirection were skipped.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_building(fx);                // b(PLAYER,BUILDING_INDEX).sub_id = SUB_ID (4 != BUILDING_INDEX 6)
        storage_row(fx).docked_count = 0; // the CORRECT (by sub_id) row is empty

        unit_storage     &wrong_row = fx.storage[PLAYER * STORAGE_PER_PLAYER + BUILDING_INDEX];
        constexpr int32_t UI        = 60;
        wrong_row.docked_count      = 1;
        wrong_row.docked_units[0]   = UI;
        fx.u(PLAYER, UI).state      = UNIT_STATE_PARKED;

        sim_store own    = fx.store();
        int32_t   result = detail::storage_launch_parked_to_orbit(fx.view(), own, g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)result, 0u,
              "T5: storage row picked via building.sub_id, not building_index itself, 0x0048f984 sub_id read");
        ck_eq((uint32_t)g_disembark_calls.size(), 0u, "T5: the wrong-row's PARKED decoy (UI) is never reached");
    }

    // =================================================================================================
    // T6 -- ORDER: the direct `order` store (0x0048fa20) happens AFTER unit_force_disembark returns
    // (0x0048fa01-0x0048fa0d), not before. The mock reads the unit's CURRENT order the instant it
    // runs; if the order write had already happened, it would see 0x31 instead of the pre-call
    // sentinel.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_building(fx);
        unit_storage     &st = storage_row(fx);
        constexpr int32_t UJ = 33;
        st.docked_count      = 1;
        st.docked_units[0]   = UJ;
        unit &u              = fx.u(PLAYER, UJ);
        u.state              = UNIT_STATE_PARKED;
        u.order              = 0x77; // pre-call sentinel, distinct from ASCEND_TO_ORBIT(0x31)

        g_order_probe_unit = &u;

        sim_store own    = fx.store();
        int32_t   result = detail::storage_launch_parked_to_orbit(fx.view(), own, g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)result, (uint32_t)UJ, "T6: found unit returned, 0x0048fa2c");
        ck_eq((uint32_t)g_order_seen_at_disembark, 0x77u,
              "T6: order STILL the pre-call sentinel (0x77) DURING unit_force_disembark -- the direct order "
              "store at 0x0048fa20 runs AFTER the call at 0x0048fa01, not before");
        ck_eq((uint32_t)u.order, (uint32_t)UNIT_STATE_ASCEND_TO_ORBIT,
              "T6: order == ASCEND_TO_ORBIT(0x31) once the call returns, 0x0048fa20");
    }

    // =================================================================================================
    // T7 -- `player` is threaded through every index (buildings/storage/units), not hardcoded to 0.
    // An identical building_index/sub_id/unit_index shape is seeded at player 0 as a decoy with a
    // DIFFERENT unit; calling with PLAYER(3) must reach player 3's own unit and player, not player 0's.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        seed_building(fx);
        unit_storage     &st   = storage_row(fx);
        constexpr int32_t UK   = 50;
        st.docked_count        = 1;
        st.docked_units[0]     = UK;
        fx.u(PLAYER, UK).state = UNIT_STATE_PARKED;

        // Decoy at player 0: same building_index/sub_id, a DIFFERENT unit index, also PARKED.
        fx.b(0, BUILDING_INDEX).sub_id = SUB_ID;
        unit_storage     &decoy_st     = fx.storage[0 * STORAGE_PER_PLAYER + SUB_ID];
        constexpr int32_t DECOY        = 91;
        decoy_st.docked_count          = 1;
        decoy_st.docked_units[0]       = DECOY;
        fx.u(0, DECOY).state           = UNIT_STATE_PARKED;

        sim_store own    = fx.store();
        int32_t   result = detail::storage_launch_parked_to_orbit(fx.view(), own, g_calls, PLAYER, BUILDING_INDEX);

        ck_eq((uint32_t)result, (uint32_t)UK,
              "T7: reaches player 3's own unit (UK=50), not player 0's decoy, 0x0048f971 player*0x6aa4 / "
              "0x0048f995 player*0x17d4 / 0x0048fa08 player*0x5b04");
        ck_eq((uint32_t)g_disembark_calls[0].player, (uint32_t)PLAYER,
              "T7: disembark called with player=3, not player 0, 0x0048fa04 AX<-EBP-0x10");
        ck_eq((uint32_t)g_disembark_calls[0].unit_index, (uint32_t)UK, "T7: disembark called for UK, not DECOY");
        ck_eq((uint32_t)fx.u(0, DECOY).order, 0u, "T7: decoy (player 0) unit's order left untouched");
    }
}

} // namespace mh::sim::test
