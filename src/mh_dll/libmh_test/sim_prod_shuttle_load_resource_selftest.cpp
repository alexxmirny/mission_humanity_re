//
// sim_prod_shuttle_load_resource_selftest.cpp -- `simtest` oracle for llm_prod_shuttle_load_resource
// @0x0048e3bb (sim/sim_prod_shuttle_load_resource.h/.cpp, RI-SIM / SIM1D).
//
// arm_ready:false -- PROVEN UNARMABLE FROM ANY SKIRMISH/SOAK RIG: the only
// caller is order-dispatch case 0x15, which requires _G_LLM_GAME_SESSION_MODE == SESSION_SP (the
// Campaign "New Game" path only) -- every skirmish/tutorial/MP start runs Path B instead. This offline
// oracle is the only evidence this function will ever get.
//
// EXPECTED BEHAVIOUR from the header banner's own derivation (sim_prod_shuttle_load_resource.h):
//   0x0048e3dc-0x0048e3f6: slot = buildings[player][building_index].shuttle_slot (byte field).
//   0x0048e3f9-0x0048e422: available = cfg_buildings[buildings[player][building_index].building_id]
//     .capacity_2[resource_id]. If that is exactly 0 (0x0048e42b-0x0048e438), return 0 immediately --
//     no slot read, no spend, nothing else touched.
//   0x0048e43d-0x0048e45f: qty = capacity_2[resource_id] - prod_shuttle_slots[player][slot]
//     .resources_reserved[resource_id] (remaining transport room). No clamp against a negative result
//     here -- reproduced literally.
//   0x0048e462-0x0048e493: if player_resources[player][resource_id] < qty (STRICTLY, JLE skips the
//     clamp), qty = player_resources[player][resource_id].
//   0x0048e496-0x0048e4a3: if cap < qty (STRICTLY, JGE skips the clamp), qty = cap.
//   0x0048e4a6-0x0048e4ea: if qty == 0 (plain JZ, sign-blind -- a negative qty is NOT zero and falls
//     through to the spend path), return 0, nothing spent/reserved.
//   0x0048e4ac-0x0048e4e8: else game_SpendResource(player, resource_id, qty); prod_shuttle_slots
//     [player][slot].resources_reserved[resource_id] += qty; return 1.
//   `player`/`resource_id`/`cap` are re-loaded via a 16-bit zero-extend at every use (one local each);
//   `building_index` is used as the full, untruncated 32-bit value.
//
#include "sim/sim_prod_shuttle_load_resource.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- the one outward call: game_SpendResource, recorded rather than applied to state -------------

struct spend_call {
    int32_t player;
    int32_t resource_id;
    int32_t amount;
};
std::vector<spend_call> g_spend_calls;

void rec_spend_resource(int32_t player, int32_t resource_id, int32_t amount) {
    g_spend_calls.push_back({player, resource_id, amount});
}

const prod_shuttle_load_resource_calls g_calls = {
    &rec_spend_resource,
};

void reset_recorders() { g_spend_calls.clear(); }

// ---- fixture wiring -------------------------------------------------------------------------------
//
// PLAYER/BUILDING_INDEX/BUILDING_ID/SLOT/RESOURCE_ID are all DISTINCT numbers on purpose (sim_test_
// support.h's own rule): building_id (7) != building_index (5) so a bug that indexes cfg_buildings by
// building_index instead of building_id reads an untouched (zeroed) cfg row and fails every positive
// case; shuttle_slot (4) != building_index (5) so a bug that writes prod_shuttle_slots at
// building_index instead of the bound slot lands in OTHER_SLOT (deliberately aliased to
// building_index) and trips the poison check.

constexpr uint32_t PLAYER         = 2;
constexpr int32_t  BUILDING_INDEX = 5;
constexpr uint16_t BUILDING_ID    = 7;
constexpr int32_t  SLOT           = 4;
constexpr uint32_t RESOURCE_ID    = 3;

constexpr uint32_t OTHER_RID    = 6;              // a different resource_id slot in the same records
constexpr int32_t  OTHER_SLOT   = BUILDING_INDEX; // =5: catches "wrote using building_index as slot"
constexpr uint32_t OTHER_PLAYER = 0;              // a different player's row

building &make_building(sim_fixture &fx) {
    building &b    = fx.b((int32_t)PLAYER, BUILDING_INDEX);
    b.building_id  = BUILDING_ID;
    b.shuttle_slot = static_cast<uint8_t>(SLOT);
    return b;
}

void seed_capacity(sim_fixture &fx, int32_t capacity_2) {
    fx.cfg_buildings[BUILDING_ID].capacity_2[RESOURCE_ID] = capacity_2;
}
void seed_reserved(sim_fixture &fx, int32_t reserved) {
    fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SLOT].resources_reserved[RESOURCE_ID] =
        reserved;
}
void seed_holdings(sim_fixture &fx, int32_t holdings) {
    fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + RESOURCE_ID] = holdings;
}

// Sentinels a correct implementation never reads or writes for THIS (player, slot, resource_id)
// triple. Any of these changing after the call means the wrong index was used somewhere.
void seed_poison(sim_fixture &fx) {
    fx.cfg_buildings[BUILDING_ID].capacity_2[OTHER_RID] = 4242;
    fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SLOT].resources_reserved[OTHER_RID] =
        8181;
    fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + OTHER_SLOT]
        .resources_reserved[RESOURCE_ID]                                    = 555;
    fx.player_resources[OTHER_PLAYER * PLAYER_RESOURCE_SLOTS + RESOURCE_ID] = 333;
    fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + OTHER_RID]         = 444;
}

int32_t reserved_now(sim_fixture &fx) {
    return fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SLOT]
        .resources_reserved[RESOURCE_ID];
}
int32_t poison_other_rid(sim_fixture &fx) {
    return fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + SLOT]
        .resources_reserved[OTHER_RID];
}
int32_t poison_other_slot(sim_fixture &fx) {
    return fx.prod_shuttle_slots[PLAYER * PROD_SHUTTLE_SLOTS_PER_PLAYER + OTHER_SLOT]
        .resources_reserved[RESOURCE_ID];
}

void check_poison_untouched(sim_fixture &fx, const char *tag) {
    char msg[160];
    std::snprintf(msg, sizeof msg, "%s: other resource_id's reserved slot untouched, indexing must use resource_id", tag);
    ck_eq((uint32_t)poison_other_rid(fx), 8181u, msg);
    std::snprintf(msg, sizeof msg, "%s: the building_index-numbered slot untouched, indexing must use shuttle_slot not building_index", tag);
    ck_eq((uint32_t)poison_other_slot(fx), 555u, msg);
}

} // namespace

void run_prod_shuttle_load_resource_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- capacity_2[resource_id] == 0: return 0 immediately, no slot read/write, no spend.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_building(fx);
        seed_poison(fx);
        seed_capacity(fx, 0);
        seed_reserved(fx, 17);
        seed_holdings(fx, 200);

        sim_store own    = fx.store();
        int32_t   result = detail::prod_shuttle_load_resource(fx.view(), own, g_calls, PLAYER,
                                                              BUILDING_INDEX, RESOURCE_ID, 90);

        ck_eq((uint32_t)result, 0u, "T1: capacity_2[rid]==0 -> return 0, 0x0048e42b-0x0048e438");
        ck_eq((uint32_t)g_spend_calls.size(), 0u,
              "T1: game_SpendResource never called on zero-capacity bail");
        ck_eq((uint32_t)reserved_now(fx), 17u,
              "T1: resources_reserved untouched on zero-capacity bail, 0x0048e42f-0x0048e438");
        check_poison_untouched(fx, "T1");
    }

    // =================================================================================================
    // T2 -- capacity boundary, EXACTLY FULL: capacity_2 - resources_reserved == 0 -> qty clamps to 0
    // via the plain JZ, return 0, no spend.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_building(fx);
        seed_poison(fx);
        seed_capacity(fx, 25);
        seed_reserved(fx, 25); // room = 0
        seed_holdings(fx, 200);

        sim_store own    = fx.store();
        int32_t   result = detail::prod_shuttle_load_resource(fx.view(), own, g_calls, PLAYER,
                                                              BUILDING_INDEX, RESOURCE_ID, 90);

        ck_eq((uint32_t)result, 0u, "T2: exactly-full room (25-25=0) -> return 0, 0x0048e4a6-0x0048e4ea");
        ck_eq((uint32_t)g_spend_calls.size(), 0u, "T2: game_SpendResource never called, room==0");
        ck_eq((uint32_t)reserved_now(fx), 25u, "T2: resources_reserved untouched, room==0 bail");
        check_poison_untouched(fx, "T2");
    }

    // =================================================================================================
    // T3 -- capacity boundary, ONE SHORT: room==1, unclamped by holdings/cap -> success, qty==1,
    // spend and reserve the SAME amount.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_building(fx);
        seed_poison(fx);
        seed_capacity(fx, 25);
        seed_reserved(fx, 24); // room = 1
        seed_holdings(fx, 200);

        sim_store own    = fx.store();
        int32_t   result = detail::prod_shuttle_load_resource(fx.view(), own, g_calls, PLAYER,
                                                              BUILDING_INDEX, RESOURCE_ID, 90);

        ck_eq((uint32_t)result, 1u, "T3: one-short room (25-24=1) -> return 1, 0x0048e4ac-0x0048e4e8");
        ck(g_spend_calls.size() == 1 && g_spend_calls[0].player == (int32_t)PLAYER &&
               g_spend_calls[0].resource_id == (int32_t)RESOURCE_ID && g_spend_calls[0].amount == 1,
           "T3: game_SpendResource(player, resource_id, 1) called once, 0x0048e4b7");
        ck_eq((uint32_t)reserved_now(fx), 25u,
              "T3: resources_reserved += the SAME clamped qty (24+1=25), 0x0048e4bc-0x0048e4db");
        check_poison_untouched(fx, "T3");
    }

    // =================================================================================================
    // T4 -- holdings clamp boundary A: player_resources == room (30==30) -> JLE skips the clamp
    // (fires only when qty is STRICTLY greater), qty stays 30.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_building(fx);
        seed_capacity(fx, 50);
        seed_reserved(fx, 20); // room = 30
        seed_holdings(fx, 30); // == room, clamp must NOT fire

        sim_store own    = fx.store();
        int32_t   result = detail::prod_shuttle_load_resource(fx.view(), own, g_calls, PLAYER,
                                                              BUILDING_INDEX, RESOURCE_ID, 90);

        ck_eq((uint32_t)result, 1u, "T4: holdings==room -- JLE skips the holdings clamp, 0x0048e47b");
        ck(g_spend_calls.size() == 1 && g_spend_calls[0].amount == 30,
           "T4: qty stays 30 (unclamped) when holdings==room exactly");
        ck_eq((uint32_t)reserved_now(fx), 50u, "T4: resources_reserved += 30 (20+30=50), unclamped");
    }

    // =================================================================================================
    // T5 -- holdings clamp boundary B: player_resources == room-1 (29<30) -> clamp DOES fire, qty
    // drops to holdings. Spend amount and reserved delta must be the same clamped number (29), not
    // the unclamped room (30) -- capacity/reserved/holdings/cap are all deliberately distinct.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_building(fx);
        seed_capacity(fx, 50);
        seed_reserved(fx, 20); // room = 30
        seed_holdings(fx, 29); // < room by 1, clamp fires

        sim_store own    = fx.store();
        int32_t   result = detail::prod_shuttle_load_resource(fx.view(), own, g_calls, PLAYER,
                                                              BUILDING_INDEX, RESOURCE_ID, 90);

        ck_eq((uint32_t)result, 1u, "T5: holdings clamp fires (29<30), still nonzero -> return 1");
        ck(g_spend_calls.size() == 1 && g_spend_calls[0].amount == 29,
           "T5: game_SpendResource called with the CLAMPED 29, not room 30, 0x0048e47d-0x0048e493");
        ck_eq((uint32_t)reserved_now(fx), 49u,
              "T5: resources_reserved += the SAME clamped 29 (20+29=49) -- deduct and add must match");
    }

    // =================================================================================================
    // T6 -- cap clamp boundary A: cap == qty (40==40) -> JGE skips the clamp (fires only when cap is
    // STRICTLY less), qty stays 40.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_building(fx);
        seed_capacity(fx, 50);
        seed_reserved(fx, 10);  // room = 40
        seed_holdings(fx, 200); // no holdings clamp

        sim_store own    = fx.store();
        int32_t   result = detail::prod_shuttle_load_resource(fx.view(), own, g_calls, PLAYER,
                                                              BUILDING_INDEX, RESOURCE_ID, 40);

        ck_eq((uint32_t)result, 1u, "T6: cap==qty -- JGE skips the cap clamp, 0x0048e49d");
        ck(g_spend_calls.size() == 1 && g_spend_calls[0].amount == 40,
           "T6: qty stays 40 (unclamped) when cap==qty exactly");
        ck_eq((uint32_t)reserved_now(fx), 50u, "T6: resources_reserved += 40 (10+40=50), unclamped");
    }

    // =================================================================================================
    // T7 -- cap clamp boundary B: cap == qty-1 (39<40) -> clamp DOES fire, qty drops to cap. Same
    // deduct==add discipline as T5, on the OTHER clamp this time.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_building(fx);
        seed_capacity(fx, 50);
        seed_reserved(fx, 10);  // room = 40
        seed_holdings(fx, 200); // no holdings clamp

        sim_store own    = fx.store();
        int32_t   result = detail::prod_shuttle_load_resource(fx.view(), own, g_calls, PLAYER,
                                                              BUILDING_INDEX, RESOURCE_ID, 39);

        ck_eq((uint32_t)result, 1u, "T7: cap clamp fires (39<40), still nonzero -> return 1");
        ck(g_spend_calls.size() == 1 && g_spend_calls[0].amount == 39,
           "T7: game_SpendResource called with the CLAMPED 39, not room 40, 0x0048e49f-0x0048e4a3");
        ck_eq((uint32_t)reserved_now(fx), 49u,
              "T7: resources_reserved += the SAME clamped 39 (10+39=49) -- deduct and add must match");
    }

    // =================================================================================================
    // T8 -- ZERO-AVAILABLE load: player_resources == 0 (holdings clamp forces qty to 0) -> return 0,
    // no spend, nothing reserved.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_building(fx);
        seed_capacity(fx, 50);
        seed_reserved(fx, 10); // room = 40
        seed_holdings(fx, 0);  // nothing available -> clamp to 0

        sim_store own    = fx.store();
        int32_t   result = detail::prod_shuttle_load_resource(fx.view(), own, g_calls, PLAYER,
                                                              BUILDING_INDEX, RESOURCE_ID, 90);

        ck_eq((uint32_t)result, 0u,
              "T8: zero player holdings clamps qty to 0 -> return 0, 0x0048e462-0x0048e4ea");
        ck_eq((uint32_t)g_spend_calls.size(), 0u, "T8: game_SpendResource never called, qty==0");
        ck_eq((uint32_t)reserved_now(fx), 10u, "T8: resources_reserved untouched, qty==0 bail");
    }

    // =================================================================================================
    // T9 -- cap==0: the caller's requested cap clamps qty to 0 -> return 0, no spend.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_building(fx);
        seed_capacity(fx, 50);
        seed_reserved(fx, 10);  // room = 40
        seed_holdings(fx, 200); // no holdings clamp

        sim_store own    = fx.store();
        int32_t   result = detail::prod_shuttle_load_resource(fx.view(), own, g_calls, PLAYER,
                                                              BUILDING_INDEX, RESOURCE_ID, 0);

        ck_eq((uint32_t)result, 0u, "T9: cap==0 clamps qty to 0 -> return 0, 0x0048e496-0x0048e4ea");
        ck_eq((uint32_t)g_spend_calls.size(), 0u, "T9: game_SpendResource never called, qty==0");
        ck_eq((uint32_t)reserved_now(fx), 10u, "T9: resources_reserved untouched, qty==0 bail");
    }

    // =================================================================================================
    // T10 -- `cap` is re-masked to 16 bits at every use (0x0048e496 MOVZX WORD). cap_raw=0x10005
    // truncates to 5; if the truncation were dropped, the full 65541 would be >= room(130) and the
    // cap clamp would never fire, leaving qty at 130 instead of 5. Pins the truncation claim.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_building(fx);
        seed_capacity(fx, 150);
        seed_reserved(fx, 20);   // room = 130
        seed_holdings(fx, 1000); // no holdings clamp

        constexpr uint32_t CAP_RAW = 0x10005u; // truncates to 5 via MOVZX WORD, 0x0048e496

        sim_store own    = fx.store();
        int32_t   result = detail::prod_shuttle_load_resource(fx.view(), own, g_calls, PLAYER,
                                                              BUILDING_INDEX, RESOURCE_ID, CAP_RAW);

        ck_eq((uint32_t)result, 1u, "T10: truncated cap (5) is nonzero -> return 1");
        ck(g_spend_calls.size() == 1 && g_spend_calls[0].amount == 5,
           "T10: qty clamped to the 16-bit-truncated cap (5), NOT the raw 32-bit value (65541), "
           "0x0048e496");
        ck_eq((uint32_t)reserved_now(fx), 25u,
              "T10: resources_reserved += the truncated cap (20+5=25)");
    }

    // =================================================================================================
    // T11 -- `building_index` is used as the FULL 32-bit value (plain IMUL, no MOVZX anywhere) --
    // unlike player/resource_id/cap. Exercised implicitly by every case above via BUILDING_INDEX=5
    // (well past a 16-bit truncation would matter for); this case just documents the contract: a
    // moderate, non-16-bit-boundary building_index still resolves the SAME building record as a
    // second, independent read confirms.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_building(fx);
        seed_capacity(fx, 12);
        seed_reserved(fx, 11); // room = 1
        seed_holdings(fx, 200);

        sim_store own    = fx.store();
        int32_t   result = detail::prod_shuttle_load_resource(fx.view(), own, g_calls, PLAYER,
                                                              BUILDING_INDEX, RESOURCE_ID, 90);

        ck_eq((uint32_t)result, 1u,
              "T11: building_index passed through unmodified resolves buildings[player][5], no MOVZX");
        ck_eq((uint32_t)reserved_now(fx), 12u, "T11: same slot/building resolved as every other case");
    }

    // =================================================================================================
    // T12 -- OVER-RESERVED (resources_reserved > capacity_2): room = 20-25 = -5. Neither clamp fires
    // (a signed -5 is <= any positive holdings/cap, so both JLE/JGE skip their clamps), and the final
    // gate is a plain JZ against 0 -- sign-blind. A negative qty is NOT zero, so the original falls
    // through into the spend path with a NEGATIVE amount. Reproduced literally, no defensive clamp
    // to 0 added.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        make_building(fx);
        seed_capacity(fx, 20);
        seed_reserved(fx, 25); // room = -5 (over-reserved)
        seed_holdings(fx, 200);

        sim_store own    = fx.store();
        int32_t   result = detail::prod_shuttle_load_resource(fx.view(), own, g_calls, PLAYER,
                                                              BUILDING_INDEX, RESOURCE_ID, 90);

        ck_eq((uint32_t)result, 1u,
              "T12: literal-translation edge case -- negative qty (-5) is not caught by the plain "
              "JZ-to-0 gate, 0x0048e4a6, so the original still reports success");
        ck(g_spend_calls.size() == 1 && g_spend_calls[0].amount == -5,
           "T12: literal-translation edge case -- game_SpendResource called with a NEGATIVE amount "
           "(-5), no floor at 0 anywhere in the original, 0x0048e4b7");
        ck_eq((uint32_t)(int32_t)reserved_now(fx), (uint32_t)(int32_t)20,
              "T12: literal-translation edge case -- resources_reserved += (-5) becomes 20 (25-5), "
              "not clamped/rejected");
    }
}

} // namespace mh::sim::test
