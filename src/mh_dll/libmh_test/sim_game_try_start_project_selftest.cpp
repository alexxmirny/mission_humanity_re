//
// sim_game_try_start_project_selftest.cpp -- `simtest` offline oracle for game_TryStartProject
// (sim/sim_game_try_start_project.{h,cpp}, RI-SIM / SIM1F).
//
// FULL OFFLINE COVERAGE. game_TryStartProject's one outward call -- game_SpendResource, in pass 2 of
// the resource walk -- is now routed through this TU's `try_start_project_calls` struct (bound in
// production to `mh::call::game_SpendResource`, behaviour-identical), so the offline oracle stubs it
// with a recording stub and exercises EVERY path: gate 1, gate 2, pass 1's whole accumulation/collapse
// rule (which never charges), the empty-resource-list success (pass 2 breaks at i=0 before any charge),
// AND -- new here -- the success-with-real-charge branch, asserting game_SpendResource fired once per
// non-zero slot, in order, with the right (player, resource_id, amount) args.
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY (tmp/decomp/game_TryStartProject_00492cac.asm)
// per the header's own branch-by-branch derivation, not the .cpp. The pass-2 charge args come from the
// asm's own register mapping at the call site (0x00492e09 EBX=resource[i].val / 0x00492e0f EDX=
// resource_id / 0x00492e12 EAX=player-masked -> game_SpendResource(player, resource_id, val)).
//
#include "sim/sim_game_try_start_project.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// game_SpendResource, recorded. One entry per pass-2 charge, in call order.
struct SpendCall {
    int32_t player, res_id, amount;
};
std::vector<SpendCall> g_spend;

void stub_spend(int32_t player, int32_t res_id, int32_t amount) {
    g_spend.push_back({player, res_id, amount});
}

const try_start_project_calls g_calls = {stub_spend};

// Clears the recorder, then drives detail::game_try_start_project through the stubbed calls table.
int32_t call(sim_fixture &fx, uint32_t player, int32_t b_idx, uint32_t project_id) {
    g_spend.clear();
    return detail::game_try_start_project(fx.view(), g_calls, player, b_idx, project_id);
}

} // namespace

void run_try_start_project_tests() {
    sim_fixture fx;

    // ---- gate 1 (0x00492ccb-0x00492cf7): invention prerequisite ------------------------------------
    // progress[player][Projects[project_id].invention].available == 0 -> return 0x13 immediately, no
    // building check, no resource walk, regardless of anything else.
    // Mutation note: dropping this gate (or checking `.acquired` instead of `.available`) would let
    // this case fall through to gate 2 and return 0x14 instead of 0x13.
    fx.reset();
    fx.cfg_projects[5].invention                       = 20;
    fx.progress[2 * PROGRESS_ROW_COUNT + 20].available = 0; // not researched
    {
        int32_t r = call(fx, /*player*/ 2, /*b_idx*/ 0, /*project_id*/ 5);
        ck_eq((uint32_t)r, (uint32_t)PROJECT_ERR_INVENTION_NOT_RESEARCHED,
              "gate1 fail: returns 0x13 (invention not researched)");
        ck(g_spend.empty(), "gate1 fail: nothing charged");
    }

    // ---- gate 1 pass, gate 2 fail (0x00492cfc-0x00492d3d): building's project_type must match ------
    // buildings[player][b_idx].building_id indexes Building[]; Building[].project_type must equal
    // Projects[project_id].type, else -> return 0x14, before any resource is looked at.
    fx.reset();
    fx.cfg_projects[6].invention                           = 21;
    fx.progress[3 * PROGRESS_ROW_COUNT + 21].available     = 1; // researched
    fx.cfg_projects[6].type                                = 100;
    fx.buildings[3 * BUILDINGS_PER_PLAYER + 1].building_id = 50;
    fx.cfg_buildings[50].project_type                      = 200; // != proj.type(100)
    {
        int32_t r = call(fx, /*player*/ 3, /*b_idx*/ 1, /*project_id*/ 6);
        ck_eq((uint32_t)r, (uint32_t)PROJECT_ERR_TYPE_MISMATCH,
              "gate2 fail: returns 0x14 (building project_type mismatch)");
        ck(g_spend.empty(), "gate2 fail: nothing charged");
    }

    // gate 2 pass sibling: same setup but project_type MATCHES and every resource slot is zero (an
    // empty, free project) -- pass 2's loop reads resource[0].id == 0 and breaks BEFORE ever reaching
    // the game_SpendResource call site, so `return 0` is genuinely observed AND nothing is charged.
    fx.reset();
    fx.cfg_projects[6].invention                           = 21;
    fx.progress[3 * PROGRESS_ROW_COUNT + 21].available     = 1;
    fx.cfg_projects[6].type                                = 100;
    fx.buildings[3 * BUILDINGS_PER_PLAYER + 1].building_id = 50;
    fx.cfg_buildings[50].project_type                      = 100; // matches
    // fx.cfg_projects[6].resource[0].id is already 0 after reset() -- an empty resource list.
    {
        int32_t r = call(fx, /*player*/ 3, /*b_idx*/ 1, /*project_id*/ 6);
        ck_eq((uint32_t)r, 0u, "gates pass, empty resource list: returns 0 (free project)");
        ck(g_spend.empty(), "empty resource list: pass 2 breaks at i=0, nothing charged");
    }

    // ---- SUCCESS WITH A REAL CHARGE (pass 2, 0x00492dcd-0x00492e23) --------------------------------
    // Gates pass, every populated slot is affordable (holdings >= cost, since the shortage compare is
    // strictly `<`), so pass 1 finishes with error==0 and pass 2 actually charges each non-zero slot.
    // Two distinct resources so a swap of any of (player, id, val) or of the two calls' order is caught.
    // Mutation note:
    //   * routing pass 2's call back to `mh::call::game_SpendResource` (un-doing the seam) crashes
    //     offline instead of recording -- this case is the reason the seam exists.
    //   * swapping the (player, res_id, amount) arg order in the call site flips the recorded tuple.
    //   * caching resource_id/val across the two passes (instead of re-reading proj.resource[i]) would
    //     still record the same values here, but dropping the second slot (an off-by-one loop bound)
    //     would leave g_spend.size()==1, caught below.
    fx.reset();
    fx.cfg_projects[6].invention                           = 21;
    fx.progress[3 * PROGRESS_ROW_COUNT + 21].available     = 1;
    fx.cfg_projects[6].type                                = 100;
    fx.buildings[3 * BUILDINGS_PER_PLAYER + 1].building_id = 50;
    fx.cfg_buildings[50].project_type                      = 100; // matches
    fx.cfg_projects[6].resource[0].id                      = 2;
    fx.cfg_projects[6].resource[0].val                     = 30;
    fx.cfg_projects[6].resource[1].id                      = 5;
    fx.cfg_projects[6].resource[1].val                     = 45;
    fx.cfg_projects[6].resource[2].id                      = 0;   // terminator -- two real slots
    fx.player_resources[3 * PLAYER_RESOURCE_SLOTS + 2]     = 100; // >= 30 -> affordable
    fx.player_resources[3 * PLAYER_RESOURCE_SLOTS + 5]     = 60;  // >= 45 -> affordable
    {
        int32_t r = call(fx, /*player*/ 3, /*b_idx*/ 1, /*project_id*/ 6);
        ck_eq((uint32_t)r, 0u, "success+charge: returns 0 (all affordable)");
        ck(g_spend.size() == 2, "success+charge: game_SpendResource fired once per non-zero slot (2)");
        if (g_spend.size() == 2) {
            ck(g_spend[0].player == 3 && g_spend[0].res_id == 2 && g_spend[0].amount == 30,
               "success+charge: slot 0 charged (player=3, res=2, amount=30)");
            ck(g_spend[1].player == 3 && g_spend[1].res_id == 5 && g_spend[1].amount == 45,
               "success+charge: slot 1 charged (player=3, res=5, amount=45)");
        }
    }

    // affordability boundary in pass 2's own success path: holdings == cost is AFFORDABLE (compare is
    // strictly `<`), so an exactly-funded slot still charges. Distinct id/val from the case above.
    fx.reset();
    fx.cfg_projects[6].invention                           = 21;
    fx.progress[3 * PROGRESS_ROW_COUNT + 21].available     = 1;
    fx.cfg_projects[6].type                                = 100;
    fx.buildings[3 * BUILDINGS_PER_PLAYER + 1].building_id = 50;
    fx.cfg_buildings[50].project_type                      = 100;
    fx.cfg_projects[6].resource[0].id                      = 4;
    fx.cfg_projects[6].resource[0].val                     = 70;
    fx.cfg_projects[6].resource[1].id                      = 0;
    fx.player_resources[3 * PLAYER_RESOURCE_SLOTS + 4]     = 70; // == cost -> NOT short -> charges
    {
        int32_t r = call(fx, /*player*/ 3, /*b_idx*/ 1, /*project_id*/ 6);
        ck_eq((uint32_t)r, 0u, "success boundary: holdings==cost is affordable -> returns 0");
        ck(g_spend.size() == 1 && g_spend[0].res_id == 4 && g_spend[0].amount == 70,
           "success boundary: exactly-funded slot still charged (res=4, amount=70)");
    }

    // ---- pass 1 (0x00492d3d-0x00492dbf): single shortage -> resource_id + 0x89 ---------------------
    fx.reset();
    fx.cfg_projects[7].invention                           = 22;
    fx.progress[4 * PROGRESS_ROW_COUNT + 22].available     = 1;
    fx.cfg_projects[7].type                                = 300;
    fx.buildings[4 * BUILDINGS_PER_PLAYER + 2].building_id = 60;
    fx.cfg_buildings[60].project_type                      = 300;
    fx.cfg_projects[7].resource[0].id                      = 1;
    fx.cfg_projects[7].resource[0].val                     = 50;
    fx.cfg_projects[7].resource[1].id                      = 0;  // terminator -- only one real slot
    fx.player_resources[4 * PLAYER_RESOURCE_SLOTS + 1]     = 10; // holdings(10) < cost(50) -> short
    {
        int32_t r = call(fx, /*player*/ 4, /*b_idx*/ 2, /*project_id*/ 7);
        ck_eq((uint32_t)r, (uint32_t)(1 + PROJECT_ERR_RESOURCE_SHORTAGE_BASE),
              "pass1 single shortage: returns resource_id(1) + 0x89");
        ck(g_spend.empty(), "pass1 single shortage: nothing charged (pass 2 skipped)");
    }

    // boundary sibling: holdings == cost is AFFORDABLE (the compare is strictly `<`), so an
    // otherwise-identical setup with holdings raised to exactly the cost must NOT be flagged short.
    // Isolated via a SECOND, genuinely-short resource so the overall result still returns before pass
    // 2 while still proving the boundary resource itself was judged affordable: if the boundary
    // resource were wrongly flagged too, the first (not second) shortage would set the error code,
    // changing which resource_id appears in the result.
    fx.reset();
    fx.cfg_projects[7].invention                           = 22;
    fx.progress[4 * PROGRESS_ROW_COUNT + 22].available     = 1;
    fx.cfg_projects[7].type                                = 300;
    fx.buildings[4 * BUILDINGS_PER_PLAYER + 2].building_id = 60;
    fx.cfg_buildings[60].project_type                      = 300;
    fx.cfg_projects[7].resource[0].id                      = 2; // boundary resource: holdings == cost -> affordable
    fx.cfg_projects[7].resource[0].val                     = 40;
    fx.cfg_projects[7].resource[1].id                      = 3; // genuinely short, so the walk still returns before pass 2
    fx.cfg_projects[7].resource[1].val                     = 25;
    fx.cfg_projects[7].resource[2].id                      = 0;
    fx.player_resources[4 * PLAYER_RESOURCE_SLOTS + 2]     = 40; // == cost -> NOT short
    fx.player_resources[4 * PLAYER_RESOURCE_SLOTS + 3]     = 5;  // < cost(25) -> short
    {
        int32_t r = call(fx, /*player*/ 4, /*b_idx*/ 2, /*project_id*/ 7);
        ck_eq((uint32_t)r, (uint32_t)(3 + PROJECT_ERR_RESOURCE_SHORTAGE_BASE),
              "boundary: holdings==cost NOT short, so resource(3) is the FIRST (and only) shortage");
        ck(g_spend.empty(), "boundary shortage: nothing charged");
    }

    // ---- pass 1: two shortages collapse to the bare sentinel 0x89, discarding which id(s) ----------
    fx.reset();
    fx.cfg_projects[8].invention                           = 23;
    fx.progress[5 * PROGRESS_ROW_COUNT + 23].available     = 1;
    fx.cfg_projects[8].type                                = 400;
    fx.buildings[5 * BUILDINGS_PER_PLAYER + 3].building_id = 70;
    fx.cfg_buildings[70].project_type                      = 400;
    fx.cfg_projects[8].resource[0].id                      = 4;
    fx.cfg_projects[8].resource[0].val                     = 40;
    fx.cfg_projects[8].resource[1].id                      = 5;
    fx.cfg_projects[8].resource[1].val                     = 25;
    fx.cfg_projects[8].resource[2].id                      = 6;
    fx.cfg_projects[8].resource[2].val                     = 15;
    fx.cfg_projects[8].resource[3].id                      = 0;
    fx.player_resources[5 * PLAYER_RESOURCE_SLOTS + 4]     = 5;   // short
    fx.player_resources[5 * PLAYER_RESOURCE_SLOTS + 5]     = 5;   // short (2nd) -> collapses to sentinel
    fx.player_resources[5 * PLAYER_RESOURCE_SLOTS + 6]     = 100; // affordable -- must not change the sentinel
    {
        int32_t r = call(fx, /*player*/ 5, /*b_idx*/ 3, /*project_id*/ 8);
        ck_eq((uint32_t)r, (uint32_t)PROJECT_ERR_RESOURCE_SHORTAGE_BASE,
              "pass1 double shortage: collapses to the bare sentinel 0x89 (not resource_id-specific)");
        ck(g_spend.empty(), "pass1 double shortage: nothing charged");
    }

    // ---- the id==0 sentinel stops the scan: a shortage placed AFTER the terminator must NOT be seen -
    // resource[0] is short (sets the specific-code error); resource[1] is the terminator; resource[2]
    // holds an even larger, definitely-short amount that must never be examined. If the walk
    // incorrectly continued past the terminator, the second shortage would collapse the result to the
    // bare sentinel 0x89 instead of the specific code below.
    fx.reset();
    fx.cfg_projects[9].invention                           = 24;
    fx.progress[6 * PROGRESS_ROW_COUNT + 24].available     = 1;
    fx.cfg_projects[9].type                                = 500;
    fx.buildings[6 * BUILDINGS_PER_PLAYER + 4].building_id = 80;
    fx.cfg_buildings[80].project_type                      = 500;
    fx.cfg_projects[9].resource[0].id                      = 9;
    fx.cfg_projects[9].resource[0].val                     = 50;
    fx.cfg_projects[9].resource[1].id                      = 0; // terminator
    fx.cfg_projects[9].resource[2].id                      = 8; // must never be read -- lies past the terminator
    fx.cfg_projects[9].resource[2].val                     = 9999;
    fx.player_resources[6 * PLAYER_RESOURCE_SLOTS + 9]     = 10; // short -> specific code 9+0x89
    fx.player_resources[6 * PLAYER_RESOURCE_SLOTS + 8]     = 0;  // would ALSO be short if wrongly read
    {
        int32_t r = call(fx, /*player*/ 6, /*b_idx*/ 4, /*project_id*/ 9);
        ck_eq((uint32_t)r, (uint32_t)(9 + PROJECT_ERR_RESOURCE_SHORTAGE_BASE),
              "sentinel stops the scan: post-terminator entry never examined (still the specific code)");
    }

    // ---- all 7 resource slots populated, none zero: the walk processes exactly the declared 7 -------
    // (boundary coverage for CFG_RESOURCE_SLOTS==7; the phantom 8th read the original's own
    // id-then-bound check order derives is left at its post-reset() zero value here rather than
    // deliberately forced non-zero, to avoid relying on out-of-declared-bounds struct reinterpretation
    // in an offline test -- the header's own byte-level derivation already settles that ordering.)
    fx.reset();
    fx.cfg_projects[10].invention                          = 25;
    fx.progress[7 * PROGRESS_ROW_COUNT + 25].available     = 1;
    fx.cfg_projects[10].type                               = 600;
    fx.buildings[7 * BUILDINGS_PER_PLAYER + 5].building_id = 90;
    fx.cfg_buildings[90].project_type                      = 600;
    for (int i = 0; i < CFG_RESOURCE_SLOTS; ++i) {
        fx.cfg_projects[10].resource[i].id                       = (uint32_t)(i + 1); // ids 1..7
        fx.cfg_projects[10].resource[i].val                      = 10;
        fx.player_resources[7 * PLAYER_RESOURCE_SLOTS + (i + 1)] = 10; // exactly affordable
    }
    fx.cfg_projects[10].resource[6].val                = 999; // last slot: force a shortage
    fx.player_resources[7 * PLAYER_RESOURCE_SLOTS + 7] = 10;  // < 999
    {
        int32_t r = call(fx, /*player*/ 7, /*b_idx*/ 5, /*project_id*/ 10);
        ck_eq((uint32_t)r, (uint32_t)(7 + PROJECT_ERR_RESOURCE_SHORTAGE_BASE),
              "7-slot walk: all 7 populated entries reached, last one's shortage reported");
    }

    // ---- all 7 slots affordable: pass 2 charges all 7, in order (charge-side 7-slot boundary) -------
    fx.reset();
    fx.cfg_projects[10].invention                          = 25;
    fx.progress[7 * PROGRESS_ROW_COUNT + 25].available     = 1;
    fx.cfg_projects[10].type                               = 600;
    fx.buildings[7 * BUILDINGS_PER_PLAYER + 5].building_id = 90;
    fx.cfg_buildings[90].project_type                      = 600;
    for (int i = 0; i < CFG_RESOURCE_SLOTS; ++i) {
        fx.cfg_projects[10].resource[i].id                       = (uint32_t)(i + 1);       // ids 1..7
        fx.cfg_projects[10].resource[i].val                      = (int32_t)(10 * (i + 1)); // vals 10,20,..70
        fx.player_resources[7 * PLAYER_RESOURCE_SLOTS + (i + 1)] = 1000;                    // all affordable
    }
    {
        int32_t r = call(fx, /*player*/ 7, /*b_idx*/ 5, /*project_id*/ 10);
        ck_eq((uint32_t)r, 0u, "7-slot charge: all affordable -> returns 0");
        ck(g_spend.size() == (size_t)CFG_RESOURCE_SLOTS, "7-slot charge: 7 charges recorded");
        bool ok = (g_spend.size() == (size_t)CFG_RESOURCE_SLOTS);
        for (size_t i = 0; ok && i < g_spend.size(); ++i)
            ok = (g_spend[i].player == 7 && g_spend[i].res_id == (int32_t)(i + 1) &&
                  g_spend[i].amount == (int32_t)(10 * (i + 1)));
        ck(ok, "7-slot charge: each slot charged with its own (player=7, id=i+1, amount=10*(i+1))");
    }

    // ---- player is masked to its low 16 bits at every use site (roster/progress/resource indexing) -
    // A high-bit-polluted player value must behave identically to its low-16-bit value.
    fx.reset();
    fx.cfg_projects[11].invention                      = 26;
    fx.progress[5 * PROGRESS_ROW_COUNT + 26].available = 0; // player index 5, NOT researched
    {
        int32_t r = call(fx, /*player*/ 0x00070005u, /*b_idx*/ 0, /*project_id*/ 11);
        ck_eq((uint32_t)r, (uint32_t)PROJECT_ERR_INVENTION_NOT_RESEARCHED,
              "player masking: 0x00070005 behaves as player=5 (gate1 fail, not an OOB index)");
    }

    // player masking reaches the charge site too: a polluted player charges under the LOW-16 value.
    fx.reset();
    fx.cfg_projects[6].invention                           = 21;
    fx.progress[3 * PROGRESS_ROW_COUNT + 21].available     = 1;
    fx.cfg_projects[6].type                                = 100;
    fx.buildings[3 * BUILDINGS_PER_PLAYER + 1].building_id = 50;
    fx.cfg_buildings[50].project_type                      = 100;
    fx.cfg_projects[6].resource[0].id                      = 2;
    fx.cfg_projects[6].resource[0].val                     = 30;
    fx.cfg_projects[6].resource[1].id                      = 0;
    fx.player_resources[3 * PLAYER_RESOURCE_SLOTS + 2]     = 100;
    {
        int32_t r = call(fx, /*player*/ 0x00990003u, /*b_idx*/ 1, /*project_id*/ 6);
        ck_eq((uint32_t)r, 0u, "player masking (charge): 0x00990003 behaves as player=3");
        ck(g_spend.size() == 1 && g_spend[0].player == 3 && g_spend[0].res_id == 2 &&
               g_spend[0].amount == 30,
           "player masking (charge): charged under masked player=3");
    }
}

} // namespace mh::sim::test
