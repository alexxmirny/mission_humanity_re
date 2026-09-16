//
// sim_reason_to_housing_bldg_selftest.cpp -- `simtest` oracle for llm_strat_reason_to_housing_bldg
// @0x0047401c (sim/sim_reason_to_housing_bldg.h/.cpp, RI-SIM / SIM1-G4).
//
// This function is a PURE QUERY (no writes, no calls besides the inert stack-capacity probe) -- per
// the header banner it is shadowable by compare_return alone (same posture as
// llm_strat_hangar_any_unit_needs_energy, whose selftest this file imitates in shape). This file
// exists anyway as extra, cheap, directly-address-cited evidence for the switch's four race-split
// arms and the bounded 1..99 scan -- the kind of thing compare_return alone would only catch by
// accident if a live game state happened to exercise the exact branch.
//
// EXPECTED BEHAVIOUR, from the disassembly (tmp/decomp_sim/llm_strat_reason_to_housing_bldg_
// 0047401c.asm) and the header banner's derivation:
//   0x00474057-0x0047405b (CMP [reason-0xf],0x3 / JA default): reason outside {0xf,0x10,0x11,0x12}
//     falls straight to 0x004740e9-0x004740f0 (return 0), the scan setup at 0x004740f2 is never
//     reached.
//   Four switch arms, each an independent `race==2` CMP/JNZ (0x0047406e/0x0047408f/0x004740ad/
//     0x004740cb -- NOT a shared branch):
//       reason==0xf  -> race==2: target=A_BARRAKS(7)  (0x00474074); else target=H_BARRACKS(0x1b) (0x0047407d)
//       reason==0x10 -> race==2: target=A_GARAGE(8)   (0x00474095); else target=H_GARAGE(0x1c)   (0x0047409e)
//       reason==0x11 -> race==2: target=A_HELIPAD(0xa)(0x004740b3); else target=H_HELIPAD(0x1e)  (0x004740bc)
//       reason==0x12 -> race==2: target=A_AIRFIELD(9) (0x004740d1); else target=H_AIRFIELD(0x1d) (0x004740da)
//   0x004740f2-0x0047412d: `for (i = 1; i < 100; ++i) if (Building[i].type == target) return i; return 0;`
//     -- scan starts at index 1 (0x004740f2 MOV [i],1), NOT 0; loop bound is the literal 100
//     (0x004740f9 CMP ...,0x64), not a live count global; the Building[i].type read is at 0x00474109-
//     0x00474110 (IMUL i,0x842 / MOVZX byte); a match returns i immediately (0x0047411c-0x00474122,
//     no further scanning); exhaustion falls to 0x00474126 (return 0).
//
// The .cpp under test (sim/sim_reason_to_housing_bldg.cpp) agrees branch-for-branch with the
// assembly above -- no divergence found while writing this oracle.
//
#include "sim/sim_reason_to_housing_bldg.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// The eight case constants the switch arms compare/select against -- see sim_reason_to_housing_bldg.cpp's
// own file-local BLDG_TYPE_ constants (re-declared here so this TU has no dependency on that file's
// anonymous namespace). Values transcribed from the header banner / .asm, not invented.
constexpr uint8_t BLDG_TYPE_A_BARRAKS  = 7;
constexpr uint8_t BLDG_TYPE_H_BARRACKS = 0x1b;
constexpr uint8_t BLDG_TYPE_A_GARAGE   = 8;
constexpr uint8_t BLDG_TYPE_H_GARAGE   = 0x1c;
constexpr uint8_t BLDG_TYPE_A_HELIPAD  = 0x0a;
constexpr uint8_t BLDG_TYPE_H_HELIPAD  = 0x1e;
constexpr uint8_t BLDG_TYPE_A_AIRFIELD = 9;
constexpr uint8_t BLDG_TYPE_H_AIRFIELD = 0x1d;

constexpr int32_t RACE_ALIEN = 2;

int32_t call(sim_fixture &fx, uint32_t reason, int32_t race) {
    const sim_view v = fx.view();
    return detail::reason_to_housing_bldg(v, reason, race);
}

} // namespace

void run_reason_to_housing_bldg_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- all 8 (reason,race) -> target-type mappings, proven INDEPENDENTLY. Every one of the 8
    // BLDG_TYPE_ values is placed at its OWN distinct cfg_buildings index (10..17), so a call that
    // returns the wrong index catches a swapped mapping (e.g. reason 0xf/race==2 wrongly selecting
    // A_GARAGE instead of A_BARRAKS would land on index 12, not 10, and fail here) rather than
    // accidentally passing because two target values coincide.
    // =================================================================================================
    {
        fx.reset();
        fx.cfg_buildings[10].type = BLDG_TYPE_A_BARRAKS;
        fx.cfg_buildings[11].type = BLDG_TYPE_H_BARRACKS;
        fx.cfg_buildings[12].type = BLDG_TYPE_A_GARAGE;
        fx.cfg_buildings[13].type = BLDG_TYPE_H_GARAGE;
        fx.cfg_buildings[14].type = BLDG_TYPE_A_HELIPAD;
        fx.cfg_buildings[15].type = BLDG_TYPE_H_HELIPAD;
        fx.cfg_buildings[16].type = BLDG_TYPE_A_AIRFIELD;
        fx.cfg_buildings[17].type = BLDG_TYPE_H_AIRFIELD;

        ck_eq((uint32_t)call(fx, 0xf, RACE_ALIEN), 10u,
              "T1: reason=0xf, race==2(ALIEN) -> A_BARRAKS(7), 0x0047406e/0x00474074");
        ck_eq((uint32_t)call(fx, 0xf, 0), 11u,
              "T1: reason=0xf, race=0(!=2) -> H_BARRACKS(0x1b), 0x0047406e JNZ/0x0047407d");
        ck_eq((uint32_t)call(fx, 0x10, RACE_ALIEN), 12u,
              "T1: reason=0x10, race==2(ALIEN) -> A_GARAGE(8), 0x0047408f/0x00474095");
        ck_eq((uint32_t)call(fx, 0x10, 1), 13u,
              "T1: reason=0x10, race=1(!=2) -> H_GARAGE(0x1c), 0x0047408f JNZ/0x0047409e");
        ck_eq((uint32_t)call(fx, 0x11, RACE_ALIEN), 14u,
              "T1: reason=0x11, race==2(ALIEN) -> A_HELIPAD(0xa), 0x004740ad/0x004740b3");
        ck_eq((uint32_t)call(fx, 0x11, 0), 15u,
              "T1: reason=0x11, race=0(!=2) -> H_HELIPAD(0x1e), 0x004740ad JNZ/0x004740bc");
        ck_eq((uint32_t)call(fx, 0x12, RACE_ALIEN), 16u,
              "T1: reason=0x12, race==2(ALIEN) -> A_AIRFIELD(9), 0x004740cb/0x004740d1");
        ck_eq((uint32_t)call(fx, 0x12, 1), 17u,
              "T1: reason=0x12, race=1(!=2) -> H_AIRFIELD(0x1d), 0x004740cb JNZ/0x004740da");
    }

    // =================================================================================================
    // T2 -- unrecognised reason codes return 0 IMMEDIATELY, without ever running the scan. Proven with
    // cfg_buildings otherwise unseeded (all-zero from fx.reset()): if the switch instead fell through
    // to the scan with an uninitialised/zero target, Building[1].type==0 (also zero by default) would
    // match on the FIRST scan iteration and wrongly return 1, not 0 -- so a pass here is real evidence
    // the scan setup at 0x004740f2 was never reached, not a lucky default. reason=0 is well inside the
    // unsigned-bounds-checked default range; 0xe and 0x13 are the two values immediately outside the
    // recognised [0xf,0x12] band (the JA boundary at 0x00474057-0x0047405b), each checked the same way.
    // =================================================================================================
    {
        fx.reset();
        ck_eq((uint32_t)call(fx, 0, 0), 0u,
              "T2: reason=0 (unrecognised) -> return 0 without running the scan, 0x004740e9-0x004740f0");
        ck_eq((uint32_t)call(fx, 0, RACE_ALIEN), 0u,
              "T2: reason=0, race==2 -- still unrecognised regardless of race, 0x004740e9-0x004740f0");
        ck_eq((uint32_t)call(fx, 0xe, 0), 0u,
              "T2: reason=0xe (one below the recognised band) -> return 0, JA boundary 0x0047405b");
        ck_eq((uint32_t)call(fx, 0x13, 0), 0u,
              "T2: reason=0x13 (one above the recognised band) -> return 0, JA boundary 0x0047405b");
    }

    // =================================================================================================
    // T3 -- the scan itself: a match at an ordinary interior index (50) is found and its index returned.
    // =================================================================================================
    {
        fx.reset();
        fx.cfg_buildings[50].type = BLDG_TYPE_H_BARRACKS;

        ck_eq((uint32_t)call(fx, 0xf, 0), 50u,
              "T3: Building[50].type==target -> return 50, scan match at 0x00474109-0x00474122");
    }

    // =================================================================================================
    // T4 -- the scan starts at index 1, NOT 0: cfg_buildings[0] is seeded to match the target, but
    // every index 1..99 is left non-matching (zeroed by fx.reset(), and the target here, H_GARAGE=0x1c,
    // is non-zero) -- index 0 must never be checked, so the correct result is 0 (exhausted), not 0
    // (found-at-0) by coincidence: if index 0 WERE checked, a mistranslation would still print 0, so
    // this case additionally confirms via T4b that the scan really walks 1..99 and finds a REAL match
    // there when one exists, ruling out "the loop never runs at all" as an alternative explanation.
    // =================================================================================================
    {
        fx.reset();
        fx.cfg_buildings[0].type = BLDG_TYPE_H_GARAGE; // would match target if index 0 were checked

        ck_eq((uint32_t)call(fx, 0x10, 0), 0u,
              "T4: Building[0].type==target but index 0 is never checked -> return 0 (exhausted), "
              "loop starts at i=1, 0x004740f2");

        // T4b -- same target, but ALSO seed a genuine match at index 1 (the first index the loop DOES
        // check): must now return 1, proving the prior 0 was "index 0 skipped", not "scan never runs".
        fx.cfg_buildings[1].type = BLDG_TYPE_H_GARAGE;
        ck_eq((uint32_t)call(fx, 0x10, 0), 1u,
              "T4b: Building[1].type==target -> return 1, confirms the scan DOES walk from i=1");
    }

    // =================================================================================================
    // T5 -- the scan exhausts without any match across the whole 1..99 range: return 0. No
    // cfg_buildings entry is seeded with the target type at all (default-zero table, non-zero target).
    // =================================================================================================
    {
        fx.reset();
        // Deliberately no entry set to BLDG_TYPE_A_HELIPAD anywhere in cfg_buildings[0..99].

        ck_eq((uint32_t)call(fx, 0x11, RACE_ALIEN), 0u,
              "T5: no Building[1..99] entry matches target -> scan exhausts, return 0, 0x00474126");
    }

    // =================================================================================================
    // T6 -- two matching entries: the scan returns the FIRST (lowest index) match, not the last. Seeds
    // indices 20 and 80 with the same target; 20 must win, proving the loop returns on first hit
    // (0x0047411c-0x00474122) rather than continuing to overwrite a "last match" candidate.
    // =================================================================================================
    {
        fx.reset();
        fx.cfg_buildings[20].type = BLDG_TYPE_A_AIRFIELD;
        fx.cfg_buildings[80].type = BLDG_TYPE_A_AIRFIELD;

        ck_eq((uint32_t)call(fx, 0x12, RACE_ALIEN), 20u,
              "T6: two matches (20 and 80) -> lowest index (20) wins, first-match-wins scan");
    }
}

} // namespace mh::sim::test
