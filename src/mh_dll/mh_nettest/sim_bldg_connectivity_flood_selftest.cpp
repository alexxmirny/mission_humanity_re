//
// sim_bldg_connectivity_flood_selftest.cpp -- `simtest` cases for llm_strat_bldg_connectivity_flood_fill
// (sim/sim_bldg_placement_enclosure.h/.cpp), SIM1B. ONLY possible oracle: the shadow site is VACUOUS
// (writes only through the caller-owned `flag_array` out-buffer -- no return value, no tracked region
// -- confirmed at arm time by mh_shadow.gen.h's own "ARMED BUT VACUOUS" banner and by the
// placement/roster-queries slice's soak: armed, 0 regions compared). Its sibling in the same file,
// check_placement_encloses_neighbors, is NOT vacuous (a real return value + the enclosure-scratch
// region) and already has real rig T1 evidence (59 calls / 0 divergences) -- this file covers only
// the vacuous one.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_connectivity_flood_fill_004d77cc.asm), not read off the .cpp:
//
//   SEED PASS (0x004d77ec-0x004d787f): walks buildings[player][slot] for slot=1.. via the
//   "buildings[player][0].index is the live occupied-slot COUNT" idiom. An OCCUPIED slot
//   (building_id != 0) gets flag_array[slot] = 1 iff cfg_buildings[building_id].type is
//   BUILDING_TYPE_A_MOTHER (0x6) or _H_MOTHER (0x1a), else 0; an EMPTY slot is skipped entirely
//   (no write, no decrement of the remaining-count -- it does not consume budget).
//
//   FLOOD PASS (0x004d787f-0x004d7a2d), a do-while(changed) fixpoint: re-walks the same occupied-slot
//   scan every outer iteration. Per occupied slot: if slot == exclude_bldg_idx OR flag_array[slot]==0,
//   skip the window scan (still counts toward remaining). Otherwise scans the building's own 31x31
//   zone (dy,dx in [-15,15], centre = cfg_buildings[building_id].{height,width}/2 + building.{y,x}):
//   for every tile whose class_owner == (player | ORDER_KIND_BUILDING) and whose OWN flag_array entry
//   (indexed by tile.building) is still 0, sets that entry to 1 and marks the pass changed. No early
//   return anywhere -- always runs to a fixpoint, returns void.
//
#include "sim/sim_bldg_placement_enclosure.h"

#include <cstring>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr uint8_t kSentinel = 0xAA;

void run_flood(sim_fixture &fx, uint32_t player, int32_t exclude, uint8_t *flags) {
    std::memset(flags, kSentinel, BUILDINGS_PER_PLAYER);
    detail::connectivity_flood_fill(fx.view(), player, exclude, flags);
}

} // namespace

void run_bldg_connectivity_flood_tests() {
    sim_fixture fx;
    uint8_t     flags[BUILDINGS_PER_PLAYER];

    // ---- S1: seed + a 2-HOP propagation chain, with empty slots interleaved to prove the seed scan
    // skips them WITHOUT consuming the occupied-slot budget (count=3 covers slots 1, 4, 7 even though
    // 2, 3, 5, 6 are empty in between). Distinct, non-overlapping 31x31 zones (width_mask/height_mask
    // widened to 0xff so no axis wraps) so propagation can only happen in the direction this test
    // sets up, not by coincidental wraparound. -----------------------------------------------------
    //
    //   slot 1: MOTHER (type 0x6) at (20,20) -- flagged by the SEED pass alone.
    //   slot 4: non-mother (type 1) at (80,80) -- flagged only once the FLOOD pass discovers the tile
    //           at (30,30) [within slot 1's zone: dx=10,dy=10] tagged class_owner=player|0x40,
    //           building=4.
    //   slot 7: non-mother (type 2) at (150,150) -- flagged only once slot 4 is ITSELF flagged and its
    //           own zone (centred on 80,80) is scanned, discovering the tile at (90,90)
    //           [dx=10,dy=10 from slot 4] tagged building=7. This is the case that fails if the flood
    //           pass is a single sweep instead of a do-while(changed) fixpoint -- slot 4 is not
    //           flagged until partway through the first outer iteration, so slot 7 needs a SECOND one.
    fx.reset();
    fx.width_m                  = 0xff;
    fx.height_m                 = 0xff;
    fx.b(0, 0).index            = 3; // occupied-slot count for the seed/flood scans
    fx.b(0, 1).building_id      = 11;
    fx.b(0, 1).x                = 20;
    fx.b(0, 1).y                = 20;
    fx.cfg_buildings[11].type   = BUILDING_TYPE_A_MOTHER;
    fx.cfg_buildings[11].width  = 0;
    fx.cfg_buildings[11].height = 0;
    fx.b(0, 4).building_id      = 12;
    fx.b(0, 4).x                = 80;
    fx.b(0, 4).y                = 80;
    fx.cfg_buildings[12].type   = 1;
    fx.cfg_buildings[12].width  = 0;
    fx.cfg_buildings[12].height = 0;
    fx.b(0, 7).building_id      = 13;
    fx.b(0, 7).x                = 150;
    fx.b(0, 7).y                = 150;
    fx.cfg_buildings[13].type   = 2;
    fx.cfg_buildings[13].width  = 0;
    fx.cfg_buildings[13].height = 0;
    fx.t(30, 30).class_owner    = (uint8_t)(0u | ORDER_KIND_BUILDING);
    fx.t(30, 30).building       = 4;
    fx.t(90, 90).class_owner    = (uint8_t)(0u | ORDER_KIND_BUILDING);
    fx.t(90, 90).building       = 7;

    run_flood(fx, 0, /*exclude=*/-1, flags);
    ck_eq(flags[1], 1, "flood S1: slot 1 (MOTHER) flagged by the seed pass");
    ck_eq(flags[4], 1, "flood S1: slot 4 discovered via slot 1's zone (1-hop)");
    ck_eq(flags[7], 1, "flood S1: slot 7 discovered via slot 4's zone -- needs a SECOND fixpoint pass");
    ck_eq(flags[2], kSentinel, "flood S1: EMPTY slot 2 is never touched (no write, not even a zero)");
    ck_eq(flags[3], kSentinel, "flood S1: EMPTY slot 3 is never touched");
    ck_eq(flags[5], kSentinel, "flood S1: EMPTY slot 5 is never touched");
    ck_eq(flags[6], kSentinel, "flood S1: EMPTY slot 6 is never touched");
    ck_eq(flags[8], kSentinel, "flood S1: slot 8 (beyond the 3-occupied budget) is never touched");

    // ---- S2: SAME fixture, but exclude_bldg_idx == slot 1. The seed pass is unaffected (exclude is
    // only checked in the flood pass), so flag[1] is STILL 1 -- but the flood pass skips slot 1's
    // window scan entirely, so nothing propagates: flag[4] and flag[7] stay at their SEED values (0,
    // since both are non-mother), not sentinel (the seed pass still ran) and not 1 (flood never fired).
    run_flood(fx, 0, /*exclude=*/1, flags);
    ck_eq(flags[1], 1, "flood S2: exclude does not affect the SEED pass -- slot 1 still flagged");
    ck_eq(flags[4], 0, "flood S2: exclude blocks slot 1's window scan -- slot 4 stays at its seed value");
    ck_eq(flags[7], 0, "flood S2: with slot 4 never flagged, the 2nd hop to slot 7 never fires either");

    // ---- S3: a SINGLE mother building with no partner in range -- the flood pass must still run (it
    // always scans to a fixpoint) but find nothing, and terminate rather than looping forever. Also
    // proves the PLAYER ROW: an occupied slot in player 1's row must not be read while scanning
    // player 0. -------------------------------------------------------------------------------------
    fx.reset();
    fx.width_m                  = 0xff;
    fx.height_m                 = 0xff;
    fx.b(0, 0).index            = 1;
    fx.b(0, 1).building_id      = 21;
    fx.b(0, 1).x                = 5;
    fx.b(0, 1).y                = 5;
    fx.cfg_buildings[21].type   = BUILDING_TYPE_H_MOTHER;
    fx.cfg_buildings[21].width  = 0;
    fx.cfg_buildings[21].height = 0;
    // A decoy in player 1's row, same slot number -- must not be read while player=0 is scanned.
    fx.b(1, 0).index          = 1;
    fx.b(1, 1).building_id    = 99;
    fx.cfg_buildings[99].type = BUILDING_TYPE_A_MOTHER;

    run_flood(fx, 0, -1, flags);
    ck_eq(flags[1], 1, "flood S3: lone H_MOTHER (0x1a) flagged by seed, fixpoint terminates with no partner");
    // Re-run for player 1 with a FRESH buffer -- must read player 1's own row, not player 0's.
    uint8_t flags_p1[BUILDINGS_PER_PLAYER];
    run_flood(fx, 1, -1, flags_p1);
    ck_eq(flags_p1[1], 1, "flood S3: player 1's own row (slot 1, building 99, A_MOTHER) read independently");
}

} // namespace mh::sim::test
