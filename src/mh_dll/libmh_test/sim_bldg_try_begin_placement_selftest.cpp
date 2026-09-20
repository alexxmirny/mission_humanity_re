//
// sim_bldg_try_begin_placement_selftest.cpp -- `simtest` oracle for llm_strat_bldg_try_begin_placement
// @0x00448c0b (sim/resid/sim_bldg_try_begin_placement.h/.cpp, RI-SIM / sim_resid batch C+E).
//
// arm_ready:false -- NO SHADOW SITE (see the header's "NO SHADOW SITE" banner). This offline oracle
// is the only evidence.
//
// mp:D25 (2026-09-19): THE TRANSLATION DIVERGES FROM THE .asm ON PURPOSE -- the pay (0x00448c56) +
// re-grant (0x00448c90) pair is replaced by one side-effect-free probe, bldg_can_afford_build_cost,
// because the pair booked the building's cost into the gains-only resource_spent counter on the
// clicking peer alone (the header explains). So below, where the asm walkthrough says "pay" read
// "probe", and the grant call is asserted ABSENT by construction: the calls struct no longer has
// a slot for it, and player_resources is reached through the const view only.
//
// EXPECTED BEHAVIOUR straight off the .asm
// (tmp/decomp_sim_resid/llm_strat_bldg_try_begin_placement_00448c0b.asm), NOT the Ghidra .c beside it:
//   0x00448c28-0x00448c3d: gate = _G_LLM_STRAT_PLAYERS[player_idx].primary_mother_bldg[*G_PLANET_INDEX].
//   0x00448c3d-0x00448c4d: gate == 0 -> return 0 immediately. No callee, no write.
//   0x00448c4f-0x00448c5b: else call the probe (was llm_bldg_pay_build_cost)(player_idx, building_idx) -> text_id.
//   0x00448c5e-0x00448c87: text_id != 0 -> floating_msg_queue_active=0 (0x00448c64), print_queue_
//     text_id(text_id) (0x00448c71), floating_msg_queue_active=1 (0x00448c76), return 0. Order-
//     sensitive: the print must OBSERVE the flag as 0.
//   0x00448c89-0x00448cb0: text_id == 0 -> [grant_type_resources at 0x00448c90: REMOVED, D25],
//     build_placement_id = building_idx (0x00448c98), ctrl_group_at(0).count = 0
//     (0x00448c9d), ui_selected_bldg_index = 0 (0x00448ca7), return 1 (0x00448cb0).
//
#include "sim/resid/sim_bldg_try_begin_placement.h"
#include "sim/sim_bldg_pay_costs.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- the two outward calls, recorded rather than applied to state --------------------------------

struct pay_call { // the affordability PROBE (bldg_can_afford_build_cost); the name is the asm's
    uint32_t player;
    int32_t  building_type_id;
};
std::vector<pay_call> g_pay_calls;
std::vector<int32_t>  g_pay_returns; // fed back in call order

struct print_call {
    int32_t text_id;
    int32_t flag_at_call; // floating_msg_queue_active OBSERVED at the moment of this call
};
std::vector<print_call> g_print_calls;

// Set per-case before the call so rec_print_queue_text_id can read LIVE state at its own call time --
// same trick sim_planet_session_begin_selftest.cpp's recorder::own uses for its SIM_ACTIVE bracket.
sim_store *g_own = nullptr;

int32_t rec_pay_build_cost(uint32_t player, int32_t building_type_id) {
    g_pay_calls.push_back({player, building_type_id});
    const int32_t v = g_pay_returns[g_pay_calls.size() - 1];
    return v;
}

void rec_print_queue_text_id(int32_t text_id) {
    // THE SUPPRESSION BRACKET: read floating_msg_queue_active NOW, before this call returns and the
    // original sets it back to 1 (0x00448c76). This is the only vantage point that can see the 0.
    g_print_calls.push_back({text_id, g_own->floating_msg_queue_active()});
}

const bldg_try_begin_placement_calls g_calls = {
    &rec_pay_build_cost,
    &rec_print_queue_text_id,
};

void reset_recorders() {
    g_pay_calls.clear();
    g_pay_returns.clear();
    g_print_calls.clear();
    g_own = nullptr;
}

// ---- fixture wiring --------------------------------------------------------------------------------
//
// PLAYER (3) is neither 0 nor MAX_PLAYERS-1 (7); PLANET (5) is neither 0 nor the last valid planet
// index (31) -- both domains a lazily-indexed or defaulted-to-0 translation would get "accidentally
// right" if tested only at the edges.

constexpr uint32_t PLAYER          = 3;
constexpr int32_t  PLANET          = 5;
constexpr int32_t  BUILDING_IDX    = 42; // building_idx, threaded unchanged into the probe
constexpr int32_t  REFUSAL_TEXT_ID = 777;

constexpr int32_t  SENTINEL_BUILD_PLACEMENT_ID     = -12345;
constexpr int32_t  SENTINEL_CTRL_GROUP0_COUNT      = 4200; // ctrl_groups[i].count seeded to this + i
constexpr uint16_t SENTINEL_UI_SELECTED_BLDG_INDEX = 999;
constexpr int32_t  SENTINEL_FLOATING_FLAG          = 42; // distinctive: neither 0 nor 1, so BOTH stores are visible

// Seeds _G_LLM_STRAT_PLAYERS[*].primary_mother_bldg[*] with a DISTINCT, non-zero value for every
// (player, planet) cell -- (player+1)*1000 + planet + 1, range [1001, 8032] -- then overrides just
// PLAYER's PLANET cell to `gate_value`. Every OTHER player's PLANET cell, and PLAYER's OWN neighbouring
// planet cells, stay at the nonzero baseline: a wrong-player read or a wrong/defaulted planet index
// reads a nonzero baseline cell instead of `gate_value`, flipping the decision.
void seed_mother_bldg(sim_fixture &fx, int32_t gate_value) {
    for (int32_t p = 0; p < MAX_PLAYERS; ++p) {
        for (int32_t pl = 0; pl < 32; ++pl) {
            fx.profiles[p].primary_mother_bldg[pl] = (p + 1) * 1000 + pl + 1;
        }
    }
    fx.profiles[PLAYER].primary_mother_bldg[PLANET] = gate_value;
}

// Seeds every one of the four success-path write targets (plus the suppression-bracket flag) to a
// sentinel a correct implementation never produces, so "still at its sentinel" proves the write was
// never reached.
void seed_write_sentinels(sim_fixture &fx) {
    fx.build_placement_id = SENTINEL_BUILD_PLACEMENT_ID;
    for (size_t i = 0; i < fx.ctrl_groups.size(); ++i) {
        fx.ctrl_groups[i].count = SENTINEL_CTRL_GROUP0_COUNT + (int32_t)i;
    }
    fx.ui_selected_bldg_index    = SENTINEL_UI_SELECTED_BLDG_INDEX;
    fx.floating_msg_queue_active = SENTINEL_FLOATING_FLAG;
}

} // namespace

void run_bldg_try_begin_placement_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- NO MOTHERSHIP: primary_mother_bldg[player][*planet_index] == 0 -> return 0, 0x00448c44.
    // Both callees unreached; every one of the four success-path writes still at its seeded sentinel.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.planet_index = PLANET;
        seed_mother_bldg(fx, 0);
        seed_write_sentinels(fx);

        sim_store own = fx.store();
        g_own         = &own;
        const int result =
            detail::bldg_try_begin_placement(fx.view(), own, g_calls, PLAYER, BUILDING_IDX);

        ck_eq((uint32_t)result, 0u,
              "T1: no mothership on this planet -> return 0, 0x00448c3d-0x00448c4d");
        ck_eq((uint32_t)g_pay_calls.size(), 0u,
              "T1: the affordability probe NEVER called -- 0x00448c4f unreached");
        ck_eq((uint32_t)g_print_calls.size(), 0u,
              "T1: llm_ui_print_queue_text_id NEVER called -- 0x00448c71 unreached");
        ck_eq((uint32_t)fx.build_placement_id, (uint32_t)SENTINEL_BUILD_PLACEMENT_ID,
              "T1: build_placement_id still at its sentinel -- 0x00448c98 unreached");
        ck_eq((uint32_t)fx.ctrl_groups[0].count, (uint32_t)SENTINEL_CTRL_GROUP0_COUNT,
              "T1: ctrl_group_at(0).count still at its sentinel -- 0x00448c9d unreached");
        ck_eq((uint32_t)fx.ui_selected_bldg_index, (uint32_t)SENTINEL_UI_SELECTED_BLDG_INDEX,
              "T1: ui_selected_bldg_index still at its sentinel -- 0x00448ca7 unreached");
        ck_eq((uint32_t)fx.floating_msg_queue_active, (uint32_t)SENTINEL_FLOATING_FLAG,
              "T1: floating_msg_queue_active untouched -- the suppression bracket is unreached on this exit");
    }

    // =================================================================================================
    // T2 -- COST REFUSED: the probe returns a nonzero TEXT ID -> print it inside the
    // 0/1 suppression bracket, return 0. The print mock captures floating_msg_queue_active AT THE
    // MOMENT it is called -- that is the only observable proof of the bracket's write order.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.planet_index = PLANET;
        seed_mother_bldg(fx, 555); // nonzero -> gate passes
        seed_write_sentinels(fx);

        sim_store own = fx.store();
        g_own         = &own;
        g_pay_returns.push_back(REFUSAL_TEXT_ID);
        const int result =
            detail::bldg_try_begin_placement(fx.view(), own, g_calls, PLAYER, BUILDING_IDX);

        ck_eq((uint32_t)result, 0u, "T2: probe returned nonzero -> return 0, 0x00448c5e-0x00448c62");
        ck_eq((uint32_t)g_pay_calls.size(), 1u,
              "T2: the affordability probe called exactly once, 0x00448c56");
        ck(g_pay_calls[0].player == PLAYER && g_pay_calls[0].building_type_id == BUILDING_IDX,
           "T2: probe(player_idx, building_idx) -- args unchanged from the params, 0x00448c4f-0x00448c56");
        ck_eq((uint32_t)g_print_calls.size(), 1u,
              "T2: llm_ui_print_queue_text_id called EXACTLY ONCE, 0x00448c71");
        ck_eq((uint32_t)g_print_calls[0].text_id, (uint32_t)REFUSAL_TEXT_ID,
              "T2: print_queue_text_id got EXACTLY the probe's return value, 0x00448c6e-0x00448c71");
        ck_eq((uint32_t)g_print_calls[0].flag_at_call, 0u,
              "T2: floating_msg_queue_active OBSERVED AS 0 at the moment of the print call -- "
              "0x00448c64 (store 0) precedes 0x00448c71 (the call)");
        ck_eq((uint32_t)fx.floating_msg_queue_active, 1u,
              "T2: floating_msg_queue_active restored to 1 AFTER the call returns, 0x00448c76");
        ck_eq((uint32_t)fx.build_placement_id, (uint32_t)SENTINEL_BUILD_PLACEMENT_ID,
              "T2: build_placement_id still at its sentinel -- 0x00448c98 unreached");
        ck_eq((uint32_t)fx.ctrl_groups[0].count, (uint32_t)SENTINEL_CTRL_GROUP0_COUNT,
              "T2: ctrl_group_at(0).count still at its sentinel -- 0x00448c9d unreached");
        ck_eq((uint32_t)fx.ui_selected_bldg_index, (uint32_t)SENTINEL_UI_SELECTED_BLDG_INDEX,
              "T2: ui_selected_bldg_index still at its sentinel -- 0x00448ca7 unreached");
    }

    // =================================================================================================
    // T3 -- SUCCESS: the probe returns 0 -> the three success-path writes land, return 1. Every OTHER
    // ctrl_group (1..9) proven untouched -- only group 0's count. No grant (D25): the calls struct
    // has no slot for one, so a translation that re-grew it would not compile against this test.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.planet_index = PLANET;
        seed_mother_bldg(fx, 555); // nonzero -> gate passes
        seed_write_sentinels(fx);

        sim_store own = fx.store();
        g_own         = &own;
        g_pay_returns.push_back(0);
        const int result =
            detail::bldg_try_begin_placement(fx.view(), own, g_calls, PLAYER, BUILDING_IDX);

        ck_eq((uint32_t)result, 1u, "T3: probe returned 0 -> return 1, 0x00448cb0");
        ck_eq((uint32_t)g_pay_calls.size(), 1u,
              "T3: the affordability probe called exactly once, 0x00448c56");
        ck_eq((uint32_t)g_print_calls.size(), 0u,
              "T3: llm_ui_print_queue_text_id NEVER called on the success path -- 0x00448c71 unreached");
        ck_eq((uint32_t)fx.build_placement_id, (uint32_t)BUILDING_IDX,
              "T3: build_placement_id = building_idx, 0x00448c98");
        ck_eq((uint32_t)fx.ctrl_groups[0].count, 0u, "T3: ctrl_group_at(0).count = 0, 0x00448c9d");
        for (size_t i = 1; i < fx.ctrl_groups.size(); ++i) {
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "T3: ctrl_group_at(%zu).count untouched -- only group 0's dword is written, "
                          "0x00448c9d",
                          i);
            ck_eq((uint32_t)fx.ctrl_groups[i].count, (uint32_t)(SENTINEL_CTRL_GROUP0_COUNT + (int32_t)i), msg);
        }
        ck_eq((uint32_t)fx.ui_selected_bldg_index, 0u, "T3: ui_selected_bldg_index = 0, 0x00448ca7");
        ck_eq((uint32_t)fx.floating_msg_queue_active, (uint32_t)SENTINEL_FLOATING_FLAG,
              "T3: floating_msg_queue_active untouched on the success path -- the suppression bracket "
              "only wraps the refusal print (0x00448c64-0x00448c76), which this exit never reaches");
    }

    // =================================================================================================
    // T4 -- THE PROBE ITSELF (mp:D25): bldg_can_afford_build_cost is pay_build_cost's pass 1 alone.
    // Same four verdicts as the original's 0x00492ecb-0x00492f8e (0x13 / id+0x89 / bare 0x89 / 0),
    // and the holdings row is byte-for-byte untouched on EVERY verdict -- including "affordable",
    // which is exactly the case the original charged and re-granted. The Academy's real cost
    // (800 of id 1, 450 of id 2 -- the 2026-09-19 diff) is the fixture.
    // =================================================================================================
    {
        fx.reset();
        constexpr int32_t INVENTION = 77;
        cfg_building     &cb        = fx.cfg_buildings[BUILDING_IDX];
        cb.invention                = INVENTION;
        cb.resource[0]              = {1u, 800};
        cb.resource[1]              = {2u, 450};
        cb.resource[2]              = {0u, 0}; // UNDEFINED terminates the walk
        auto seed_holdings          = [&](int32_t r1, int32_t r2) {
            for (int32_t p = 0; p < MAX_PLAYERS; ++p) {
                for (int32_t r = 0; r < PLAYER_RESOURCE_SLOTS; ++r) {
                    fx.player_resources[(size_t)(p * PLAYER_RESOURCE_SLOTS + r)] = 100000 + p * 100 + r;
                }
            }
            fx.player_resources[(size_t)(PLAYER * PLAYER_RESOURCE_SLOTS + 1)] = r1;
            fx.player_resources[(size_t)(PLAYER * PLAYER_RESOURCE_SLOTS + 2)] = r2;
        };
        auto holdings_snapshot = [&] { return fx.player_resources; };

        // (a) invention not researched -> 0x13, before any resource is even read.
        fx.progress[(size_t)(PLAYER * PROGRESS_ROW_COUNT + INVENTION)].available = 0;
        seed_holdings(0, 0);
        auto before = holdings_snapshot();
        ck_eq((uint32_t)detail::bldg_can_afford_build_cost(fx.view(), PLAYER, BUILDING_IDX), 0x13u,
              "T4a: invention unavailable -> 0x13 (0x00492ecb-0x00492efb)");
        ck(holdings_snapshot() == before, "T4a: holdings untouched");

        fx.progress[(size_t)(PLAYER * PROGRESS_ROW_COUNT + INVENTION)].available = 1;

        // (b) exactly affordable (== is not <) -> 0, and NOTHING charged.
        seed_holdings(800, 450);
        before = holdings_snapshot();
        ck_eq((uint32_t)detail::bldg_can_afford_build_cost(fx.view(), PLAYER, BUILDING_IDX), 0u,
              "T4b: 800/450 against a cost of 800/450 -> affordable, 0");
        ck(holdings_snapshot() == before,
           "T4b: holdings untouched on the AFFORDABLE verdict -- the D25 fix: no pay, no re-grant");

        // (c) short on id 2 only -> 2 + 0x89.
        seed_holdings(800, 449);
        before = holdings_snapshot();
        ck_eq((uint32_t)detail::bldg_can_afford_build_cost(fx.view(), PLAYER, BUILDING_IDX), 2u + 0x89u,
              "T4c: one shortage names its resource: id 2 -> 0x8b (0x00492f60-0x00492f6c)");
        ck(holdings_snapshot() == before, "T4c: holdings untouched");

        // (d) short on both -> the bare sentinel 0x89 (the second shortage discards the first's id).
        seed_holdings(799, 449);
        before = holdings_snapshot();
        ck_eq((uint32_t)detail::bldg_can_afford_build_cost(fx.view(), PLAYER, BUILDING_IDX), 0x89u,
              "T4d: two shortages collapse to bare 0x89 (0x00492f6e-0x00492f77)");
        ck(holdings_snapshot() == before, "T4d: holdings untouched");

        // (e) the PAYING sibling still pays, through its recorded spend -- the split did not change it.
        struct spend {
            int32_t p, id, amount;
        };
        static std::vector<spend> spends;
        spends.clear();
        const pay_costs_calls paying = {
            [](int32_t p, int32_t id, int32_t amount) { spends.push_back({p, id, amount}); },
        };
        seed_holdings(800, 450);
        ck_eq((uint32_t)detail::bldg_pay_build_cost(fx.view(), paying, PLAYER, BUILDING_IDX), 0u,
              "T4e: pay_build_cost still returns 0 when affordable");
        ck_eq((uint32_t)spends.size(), 2u, "T4e: pay_build_cost charged exactly the two priced slots");
        ck(spends.size() == 2 && spends[0].p == (int32_t)PLAYER && spends[0].id == 1 && spends[0].amount == 800 &&
               spends[1].id == 2 && spends[1].amount == 450,
           "T4e: charged (1, 800) then (2, 450), in slot order (0x00492fd9)");
        seed_holdings(800, 449);
        spends.clear();
        ck_eq((uint32_t)detail::bldg_pay_build_cost(fx.view(), paying, PLAYER, BUILDING_IDX), 2u + 0x89u,
              "T4e: pay_build_cost refuses with the probe's code");
        ck_eq((uint32_t)spends.size(), 0u, "T4e: and charges nothing on a refusal");
    }
}

} // namespace mh::sim::test
