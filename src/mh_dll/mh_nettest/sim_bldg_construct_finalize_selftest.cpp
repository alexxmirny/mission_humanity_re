//
// sim_bldg_construct_finalize_selftest.cpp -- `simtest` cases for llm_bldg_construct_finalize
// (sim/sim_bldg_construct_finalize.h/.cpp), SIM1B building_tick machinery slice.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_bldg_construct_finalize_00462e66.asm), not read off the .cpp/.h bodies alone:
//
//   THE SHRINK FLAG (0x00462e8e-0x00462ea7): `param_4 == 1` sets local_14 = 9, else 0. Used in
//   EXACTLY TWO PLACES: the GARAGE-family (unit_storage) sub-slot scan bound (`0x19 - local_14`,
//   caseD_7 @0x0046302e) and the final buildings-roster scan bound (`0x64 - local_14`, @0x004630db).
//   No other case reads it.
//
//   THE TYPE SWITCH (0x00462f45-0x004630ac), a 38-entry jump table on `Building[building_id].type -
//   1` (DEC AL; JA 0x004630ac for AL > 0x25, i.e. type == 0 or type > 0x26 lands in the default arm
//   caseD_11 -- a no-op, sub_slot stays 0 from its 0x00462e87 initializer):
//     caseD_1 @0x00462f6f (PRODUCTION): scan productions[player][1..8) on dword field b_index==0.
//     caseD_2 @0x00462feb (MINE):       scan mines[player][1..32) on dword field f1==0.
//     caseD_5 @0x00462fae (TURRET):     scan turrets[player][1..32) on WORD field f1==0.
//     caseD_7 @0x00463027 (GARAGE fam): scan unit_storage[player][1..(25-shrink)) on dword b_index==0.
//     caseD_b @0x0046306a (LAB):        scan labs[player][1..25) on dword field b_index==0.
//     caseD_3 @0x004630a5 (no-scan fam: PLANT/COLONY/MOTHER/MAIN_BASE/RELAY/SILOS/CIVIL): sub_slot=1
//                                        unconditionally, no scan at all -- other rosters never touched.
//   Every scan finds the FIRST index >= 1 whose field reads 0 and stops; index 0 is never probed.
//
//   THE FINAL ROSTER SCAN AND THREE OUTCOMES (0x004630ac-0x00463181):
//     sub_slot == 0 (no category slot)              -> ai_notify_bldg_constructed(player, x_b, 0,
//                                                        building_id, y_b, 2); return 0. NO roster scan.
//     sub_slot != 0, roster scan (bound 100-shrink)
//       finds buildings[player][slot].building_id==0 -> create_building(player, slot, x_b, y_b,
//                                                        building_id, param_1, sub_slot);
//                                                        link_to_network_if_adjacent(player, slot);
//                                                        notify_state_change(player, slot);
//                                                        ai_notify_bldg_constructed(player, x_b, slot,
//                                                        building_id, y_b, 0); return slot.
//       exhausted (no free slot)                     -> SAME failure notify as sub_slot==0 (code 2,
//                                                        param_3 0); return 0.
//   param_1 is forwarded to create_building UNTOUCHED (ECX in, ECX out at 0x00463123/0x00463126) --
//   the committed prototype's `param_6` argument.
//
// WHAT THIS FILE CANNOT COVER OFFLINE: the two failure origins (sub_slot==0 skipping the roster scan
// entirely, vs. sub_slot!=0 with the roster scan running to exhaustion) are OBSERVATIONALLY IDENTICAL
// from outside -- same single ai_notify_bldg_constructed call, same return 0, no way to tell "the
// roster loop never ran" from "it ran and found nothing" without a coverage probe inside the original.
// C8 below asserts both produce that one identical call rather than claiming to distinguish them.
// map_CreateBuilding's own body is a stubbed original (out of this closure's writes_shared, per
// sim_migration.json) -- only the CALL and its args are asserted, never an effect of it.
//
#include "sim/sim_bldg_construct_finalize.h"

#include <vector>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_*/H_*
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- recording stubs, one vector of args per callee ----------------------------------------------

struct CreateCall {
    uint16_t player;
    uint32_t index;
    uint32_t x_b;
    int32_t  y_b;
    int32_t  building_id;
    uint32_t param_6;
    uint32_t sub_id;
};
struct LinkCall {
    uint16_t player;
    uint32_t index;
};
struct NotifyCall {
    uint16_t player;
    uint32_t building_id;
};
struct AiCall {
    uint32_t player;
    uint32_t x_b;
    uint32_t param_3;
    uint32_t building_id;
    uint32_t y_b;
    uint32_t param_6;
};

std::vector<CreateCall> g_create;
std::vector<LinkCall>   g_link;
std::vector<NotifyCall> g_notify;
std::vector<AiCall>     g_ai;

void rec_create(uint16_t player, uint32_t index, uint32_t x_b, int32_t y_b, int32_t building_id,
                uint32_t param_6, uint32_t sub_id) {
    g_create.push_back({player, index, x_b, y_b, building_id, param_6, sub_id});
}
void rec_link(uint16_t player, uint32_t index) { g_link.push_back({player, index}); }
void rec_notify(uint16_t player, uint32_t building_id) { g_notify.push_back({player, building_id}); }
void rec_ai(uint32_t player, uint32_t x_b, uint32_t param_3, uint32_t building_id, uint32_t y_b,
            uint32_t param_6) {
    g_ai.push_back({player, x_b, param_3, building_id, y_b, param_6});
}

const bldg_construct_finalize_calls g_calls = {&rec_create, &rec_link, &rec_notify, &rec_ai};

void reset_recorders() {
    g_create.clear();
    g_link.clear();
    g_notify.clear();
    g_ai.clear();
}

// Occupy roster slots [1, count) so the final buildings-roster scan's first free slot lands exactly
// at `count` (or leaves it exhausted if `count` is beyond the caller's own scan bound).
void occupy_roster(sim_fixture &fx, uint16_t player, int32_t first_free) {
    for (int32_t i = 1; i < first_free; ++i) fx.b(player, i).building_id = (uint16_t)(1000 + i);
}

} // namespace

void run_bldg_construct_finalize_tests() {
    sim_fixture fx;

    // ================================================================================================
    // C1 -- PRODUCTION family sub-slot selection (dword field b_index, bound 1..8) + the FULL success
    // orchestration: param_1 forwarding, all 4 outward calls fire in order with the right args, return
    // value == roster_slot. Occupied indices 1,2; first free at 3 -> sub_slot must be 3, not e.g. the
    // count of occupied slots (2) or the loop's final index (7).
    // ================================================================================================
    fx.reset();
    fx.cfg_buildings[42].type                              = BUILDING_TYPE_A_PRODUCTION;
    fx.productions[3 * PRODUCTIONS_PER_PLAYER + 1].b_index = 111; // occupied
    fx.productions[3 * PRODUCTIONS_PER_PLAYER + 2].b_index = 222; // occupied
    // index 3 left at reset's 0 -> first free
    occupy_roster(fx, 3, /*first_free=*/5); // roster slots 1..4 occupied, 5 free
    reset_recorders();
    int32_t r1 = detail::bldg_construct_finalize(fx.view(), g_calls, /*param_1=*/0xAAAA5555, /*y_b=*/77,
                                                 /*player=*/3, /*param_4=*/0, /*x_b=*/44,
                                                 /*building_id=*/42);
    ck_eq((uint32_t)r1, 5, "C1 production: returns the roster slot found (5)");
    ck_eq((uint32_t)g_create.size(), 1, "C1 production: create_building called exactly once");
    if (!g_create.empty()) {
        const CreateCall &cc = g_create[0];
        ck_eq(cc.player, 3, "C1 production: create_building player");
        ck_eq(cc.index, 5, "C1 production: create_building roster index");
        ck_eq(cc.x_b, 44, "C1 production: create_building x_b");
        ck_eq((uint32_t)cc.y_b, 77, "C1 production: create_building y_b");
        ck_eq((uint32_t)cc.building_id, 42, "C1 production: create_building building_id");
        ck_eq(cc.param_6, 0xAAAA5555u, "C1 production: param_1 forwarded UNTOUCHED as param_6");
        ck_eq(cc.sub_id, 3, "C1 production: sub_id == the sub-slot found (3, not 2 or 7)");
    }
    ck_eq((uint32_t)g_link.size(), 1, "C1 production: link_to_network_if_adjacent called once");
    if (!g_link.empty()) {
        ck_eq(g_link[0].player, 3, "C1 production: link player");
        ck_eq(g_link[0].index, 5, "C1 production: link roster index");
    }
    ck_eq((uint32_t)g_notify.size(), 1, "C1 production: notify_state_change called once");
    if (!g_notify.empty()) {
        ck_eq(g_notify[0].player, 3, "C1 production: notify player");
        ck_eq(g_notify[0].building_id, 5, "C1 production: notify arg is roster slot (5)");
    }
    ck_eq((uint32_t)g_ai.size(), 1, "C1 production: ai_notify_bldg_constructed called exactly once");
    if (!g_ai.empty()) {
        ck_eq(g_ai[0].player, 3, "C1 production: ai_notify player");
        ck_eq(g_ai[0].x_b, 44, "C1 production: ai_notify x_b");
        ck_eq(g_ai[0].param_3, 5, "C1 production: ai_notify param_3 == roster slot on SUCCESS");
        ck_eq(g_ai[0].building_id, 42, "C1 production: ai_notify building_id");
        ck_eq(g_ai[0].y_b, 77, "C1 production: ai_notify y_b");
        ck_eq(g_ai[0].param_6, 0, "C1 production: ai_notify success code == 0");
    }

    // ================================================================================================
    // C2 -- MINE family sub-slot selection (dword field f1, bound 1..32). Occupied 1..3, free at 4.
    // ================================================================================================
    fx.reset();
    fx.cfg_buildings[10].type                  = BUILDING_TYPE_A_MINE;
    fx.mines[0 * MINES_PER_PLAYER + 1].b_index = 5;
    fx.mines[0 * MINES_PER_PLAYER + 2].b_index = 6;
    fx.mines[0 * MINES_PER_PLAYER + 3].b_index = 7;
    occupy_roster(fx, 0, /*first_free=*/2);
    reset_recorders();
    int32_t r2 = detail::bldg_construct_finalize(fx.view(), g_calls, 0, 0, 0, 0, 0, 10);
    ck_eq((uint32_t)r2, 2, "C2 mine: roster slot 2");
    ck_eq((uint32_t)g_create.size(), 1, "C2 mine: create_building fired once");
    if (!g_create.empty())
        ck_eq(g_create[0].sub_id, 4, "C2 mine: sub_id == 4 (first free mine slot, dword field)");

    // ================================================================================================
    // C3 -- TURRET family sub-slot selection (WORD field f1, bound 1..32) -- proves the 16-bit field
    // width is honoured (not misread as a dword and stepping on the next struct's bytes) and that the
    // scan stops at the first free index (2) even though a LATER index (3) is also occupied.
    // ================================================================================================
    fx.reset();
    fx.cfg_buildings[20].type                      = BUILDING_TYPE_A_TURRET;
    fx.turrets[1 * TURRETS_PER_PLAYER + 1].b_index = 9; // occupied
    fx.turrets[1 * TURRETS_PER_PLAYER + 3].b_index = 9; // occupied (later, irrelevant -- must not be picked)
    occupy_roster(fx, 1, /*first_free=*/7);
    reset_recorders();
    int32_t r3 = detail::bldg_construct_finalize(fx.view(), g_calls, 0, 0, 1, 0, 0, 20);
    ck_eq((uint32_t)r3, 7, "C3 turret: roster slot 7");
    ck_eq((uint32_t)g_create.size(), 1, "C3 turret: create_building fired once");
    if (!g_create.empty())
        ck_eq(g_create[0].sub_id, 2,
              "C3 turret: sub_id == 2 (first free, index 3 being occupied too is irrelevant)");

    // ================================================================================================
    // C4 -- LAB family sub-slot selection (dword field b_index, bound 1..25). Occupied 1,2; free at 3.
    // ================================================================================================
    fx.reset();
    fx.cfg_buildings[30].type                = BUILDING_TYPE_A_LAB;
    fx.labs[2 * LABS_PER_PLAYER + 1].b_index = 1;
    fx.labs[2 * LABS_PER_PLAYER + 2].b_index = 1;
    occupy_roster(fx, 2, /*first_free=*/9);
    reset_recorders();
    int32_t r4 = detail::bldg_construct_finalize(fx.view(), g_calls, 0, 0, 2, 0, 0, 30);
    ck_eq((uint32_t)r4, 9, "C4 lab: roster slot 9");
    ck_eq((uint32_t)g_create.size(), 1, "C4 lab: create_building fired once");
    if (!g_create.empty()) ck_eq(g_create[0].sub_id, 3, "C4 lab: sub_id == 3 (first free lab slot)");

    // ================================================================================================
    // C5 -- a NO-SCAN family type (BUILDING_TYPE_A_MOTHER, caseD_3): sub_slot is ALWAYS 1, and the
    // per-family sub-rosters are never even consulted -- proven by filling every one of them (including
    // storage, in case the wrong case arm were reached) completely full and confirming the call still
    // succeeds with sub_id == 1, not a failure from a family scan that shouldn't have run at all.
    // ================================================================================================
    fx.reset();
    fx.cfg_buildings[50].type = BUILDING_TYPE_A_MOTHER;
    for (int32_t i = 1; i < PRODUCTIONS_PER_PLAYER; ++i) fx.productions[4 * PRODUCTIONS_PER_PLAYER + i].b_index = 1;
    for (int32_t i = 1; i < MINES_PER_PLAYER; ++i) fx.mines[4 * MINES_PER_PLAYER + i].b_index = 1;
    for (int32_t i = 1; i < TURRETS_PER_PLAYER; ++i) fx.turrets[4 * TURRETS_PER_PLAYER + i].b_index = 1;
    for (int32_t i = 1; i < LABS_PER_PLAYER; ++i) fx.labs[4 * LABS_PER_PLAYER + i].b_index = 1;
    for (int32_t i = 1; i < STORAGE_PER_PLAYER; ++i) fx.storage[4 * STORAGE_PER_PLAYER + i].b_index = 1;
    occupy_roster(fx, 4, /*first_free=*/1); // roster slot 1 itself is free
    reset_recorders();
    int32_t r5 = detail::bldg_construct_finalize(fx.view(), g_calls, 0, 0, 4, 0, 0, 50);
    ck_eq((uint32_t)r5, 1, "C5 no-scan family: succeeds despite every OTHER family's roster being full");
    ck_eq((uint32_t)g_create.size(), 1, "C5 no-scan family: create_building fired once");
    if (!g_create.empty())
        ck_eq(g_create[0].sub_id, 1, "C5 no-scan family: sub_id == 1 unconditionally, no scan performed");

    // ================================================================================================
    // C6 -- the SHRINK FLAG's effect on the GARAGE-family (unit_storage) sub-slot bound: 25 unshrunk,
    // 25-9=16 shrunk. Storage occupied at indices 1..15, free at 16 -- INSIDE the unshrunk bound (scan
    // reaches i=1..24) but OUTSIDE the shrunk one (scan only reaches i=1..15).
    // ================================================================================================
    fx.reset();
    fx.cfg_buildings[60].type = BUILDING_TYPE_A_GARAGE;
    for (int32_t i = 1; i <= 15; ++i) fx.storage[5 * STORAGE_PER_PLAYER + i].b_index = 1; // occupied
    // index 16 stays 0 (free) after reset()
    occupy_roster(fx, 5, /*first_free=*/1); // roster slot 1 free, for the success sub-case
    reset_recorders();
    int32_t r6a = detail::bldg_construct_finalize(fx.view(), g_calls, 0, 0, 5, /*param_4=*/0, 0, 60);
    ck_eq((uint32_t)r6a, 1, "C6a garage param_4=0: unshrunk bound (25) reaches free storage slot 16 -> succeeds");
    ck_eq((uint32_t)g_create.size(), 1, "C6a: create_building fired once");
    if (!g_create.empty())
        ck_eq(g_create[0].sub_id, 16, "C6a: sub_id == 16 (found within the UNshrunk bound)");

    reset_recorders();
    int32_t r6b = detail::bldg_construct_finalize(fx.view(), g_calls, 0, 0, 5, /*param_4=*/1, 0, 60);
    ck_eq((uint32_t)r6b, 0, "C6b garage param_4=1: shrunk bound (16) never reaches free slot 16 -> fails");
    ck_eq((uint32_t)g_create.size(), 0, "C6b: create_building never fired (sub_slot stayed 0)");
    ck_eq((uint32_t)g_link.size(), 0, "C6b: link_to_network_if_adjacent never fired");
    ck_eq((uint32_t)g_notify.size(), 0, "C6b: notify_state_change never fired");
    ck_eq((uint32_t)g_ai.size(), 1, "C6b: ai_notify_bldg_constructed fired once (failure notify)");
    if (!g_ai.empty()) {
        ck_eq(g_ai[0].param_3, 0, "C6b: ai_notify param_3 == 0 on failure");
        ck_eq(g_ai[0].param_6, 2, "C6b: ai_notify failure code == 2");
    }

    // ================================================================================================
    // C7 -- the SHRINK FLAG's SECOND, INDEPENDENT effect: the final buildings-roster scan bound (100
    // unshrunk, 100-9=91 shrunk). Uses the no-scan family (sub_slot always 1, immune to shrink itself)
    // so ONLY the roster bound is under test. Roster occupied 1..90, free at 91 -- inside the unshrunk
    // bound (scan reaches i=1..99) but outside the shrunk one (scan only reaches i=1..90).
    // ================================================================================================
    fx.reset();
    fx.cfg_buildings[70].type = BUILDING_TYPE_A_COLONY; // no-scan family, sub_slot always 1
    occupy_roster(fx, 6, /*first_free=*/91);
    reset_recorders();
    int32_t r7a = detail::bldg_construct_finalize(fx.view(), g_calls, 0, 0, 6, /*param_4=*/0, 0, 70);
    ck_eq((uint32_t)r7a, 91, "C7a param_4=0: unshrunk roster bound (100) reaches free slot 91 -> succeeds");

    reset_recorders();
    int32_t r7b = detail::bldg_construct_finalize(fx.view(), g_calls, 0, 0, 6, /*param_4=*/1, 0, 70);
    ck_eq((uint32_t)r7b, 0, "C7b param_4=1: shrunk roster bound (91) never reaches free slot 91 -> fails");
    ck_eq((uint32_t)g_create.size(), 0, "C7b: create_building never fired (roster scan exhausted)");

    // ================================================================================================
    // C8 -- the two DIFFERENT failure origins collapse to the SAME observable outcome (documented as
    // observationally indistinguishable offline -- see the file banner). Both must produce exactly one
    // ai_notify_bldg_constructed(player, x_b, 0, building_id, y_b, 2) call, return 0, and never call
    // create_building/link_to_network_if_adjacent/notify_state_change.
    // ================================================================================================
    // C8a: sub_slot == 0 immediately -- an unmatched/out-of-range building type (DEC AL > 0x25, e.g.
    // type == 0, which is not any defined BUILDING_TYPE_* constant) lands in the default arm and never
    // even reaches the roster scan.
    fx.reset();
    fx.cfg_buildings[80].type = 0;          // out-of-range -> caseD_11 default, sub_slot stays 0
    occupy_roster(fx, 7, /*first_free=*/1); // a free roster slot exists but must never be reached
    reset_recorders();
    int32_t r8a = detail::bldg_construct_finalize(fx.view(), g_calls, 0, /*y_b=*/12, 7, 0, /*x_b=*/34, 80);
    ck_eq((uint32_t)r8a, 0, "C8a unmatched type: returns 0 despite a free roster slot existing");
    ck_eq((uint32_t)g_create.size(), 0, "C8a: create_building never fired");
    ck_eq((uint32_t)g_link.size(), 0, "C8a: link_to_network_if_adjacent never fired");
    ck_eq((uint32_t)g_notify.size(), 0, "C8a: notify_state_change never fired");
    ck_eq((uint32_t)g_ai.size(), 1, "C8a: ai_notify_bldg_constructed fired exactly once");
    if (!g_ai.empty()) {
        ck_eq(g_ai[0].player, 7, "C8a: ai_notify player");
        ck_eq(g_ai[0].x_b, 34, "C8a: ai_notify x_b");
        ck_eq(g_ai[0].param_3, 0, "C8a: ai_notify param_3 == 0");
        ck_eq(g_ai[0].building_id, 80, "C8a: ai_notify building_id");
        ck_eq(g_ai[0].y_b, 12, "C8a: ai_notify y_b");
        ck_eq(g_ai[0].param_6, 2, "C8a: ai_notify failure code == 2");
    }

    // C8b: sub_slot found (production, index 1 free), but the roster scan runs to exhaustion (every
    // roster slot 1..99 occupied) -- the SAME failure notify as C8a, from the opposite root cause.
    fx.reset();
    fx.cfg_buildings[81].type = BUILDING_TYPE_A_PRODUCTION;
    // productions[0][1] left at 0 by reset() -> sub_slot resolves to 1. player 0 (a valid slot -- the
    // roster is [MAX_PLAYERS=8][BUILDINGS_PER_PLAYER=100]; player 8 would index the 9th, out of bounds).
    occupy_roster(fx, 0, /*first_free=*/100); // roster slots 1..99 ALL occupied -> scan exhausts
    reset_recorders();
    int32_t r8b = detail::bldg_construct_finalize(fx.view(), g_calls, 0, /*y_b=*/12, 0, 0, /*x_b=*/34, 81);
    ck_eq((uint32_t)r8b, 0, "C8b roster exhausted: returns 0 even though a sub-slot WAS found");
    ck_eq((uint32_t)g_create.size(), 0, "C8b: create_building never fired (no free roster slot)");
    ck_eq((uint32_t)g_link.size(), 0, "C8b: link_to_network_if_adjacent never fired");
    ck_eq((uint32_t)g_notify.size(), 0, "C8b: notify_state_change never fired");
    ck_eq((uint32_t)g_ai.size(), 1, "C8b: ai_notify_bldg_constructed fired exactly once");
    if (!g_ai.empty()) {
        ck_eq(g_ai[0].param_3, 0, "C8b: ai_notify param_3 == 0, SAME shape as C8a's failure");
        ck_eq(g_ai[0].param_6, 2, "C8b: ai_notify failure code == 2, SAME shape as C8a's failure");
    }
}

} // namespace mh::sim::test
