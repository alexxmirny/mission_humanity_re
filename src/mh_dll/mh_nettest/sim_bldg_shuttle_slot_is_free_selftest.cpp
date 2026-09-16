//
// sim_bldg_shuttle_slot_is_free_selftest.cpp -- `simtest` oracle for
// llm_strat_bldg_shuttle_slot_is_free @0x0048fa97 (sim/sim_bldg_shuttle_slot_is_free.h/.cpp,
// RI-SIM / SIM1D).
//
// arm_ready:false -- zero calls across BOTH the 15000- and 30000-step all-AI soaks (per the ledger):
// its only callers are a UI-only query (llm_strat_bldg_ui_shuttle_slot_free, needs a human
// building-panel interaction no soak drives) and the unload_resource/unload_passengers chain, gated
// the same way as their load-side counterparts. This offline oracle is
// therefore the ONLY evidence this function will ever have -- written as the primary proof, not a
// supplement.
//
// EXPECTED BEHAVIOUR from the header banner's own derivation (sim_bldg_shuttle_slot_is_free.h):
//   0x0048faa4-0x0048fb52: four-way OR gate on cfg_buildings[buildings[player][building_id]
//     .building_id].type == A_PORT(0x0c) | H_PORT(0x20) | A_MOTHER(0x06) | H_MOTHER(0x1a). A miss on
//     ALL FOUR returns -1 (0xffffffff) immediately -- WITHOUT ever reading shuttle_slot or any
//     prod_shuttle_slots record.
//   0x0048fb5e-0x0048fb78: shuttle_slot = buildings[player][building_id].shuttle_slot. shuttle_slot==0
//     (unbound) skips BOTH reservation checks entirely and returns 1 (free) -- the record at slot 0 is
//     never read.
//   0x0048fb82-0x0048fba2: prod_shuttle_slots[player*10+shuttle_slot].passengers_reserved != 0 ->
//     returns 0 (busy) immediately.
//   0x0048fbab-0x0048fbe5: for (r=1; r<10; ++r) resources_reserved[r] != 0 -> returns 0 (busy)
//     immediately on the first hit. Index 0 of the 10-entry array is NEVER visited (loop starts at 1,
//     bound stays 10) -- preserved exactly from the disassembly rather than "cleaned up" to a
//     9-iteration loop, so a resources_reserved[0]-only seed must still come back FREE.
//   0x0048fbe7: falls out of the loop with no hit -> return 1 (free).
//
#include "sim/sim_bldg_shuttle_slot_is_free.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Primary building identity used by most cases below. ROSTER_IDX (the function's OWN param_2,
// "building_id" -- an INDEX into buildings[player][]) and CFG_IDX (the value STORED in that record's
// .building_id field, the actual cfg_buildings[] lookup key) are kept DISTINCT from each other and
// from PLAYER, on purpose: a translation that confused "the roster index" with "the building's own
// .building_id field" (i.e. indexed cfg_buildings[] with param_2 directly) would read the wrong
// record and disagree with every case below.
constexpr uint16_t PLAYER     = 3;
constexpr int32_t  ROSTER_IDX = 7;
constexpr uint16_t CFG_IDX    = 13;

// A second, fully independent identity (different player row AND different building/cfg indices),
// used by the one case (S8) that pins the function actually uses the CALLER-SUPPLIED player rather
// than some hardcoded row.
constexpr uint16_t PLAYER_B     = 5;
constexpr int32_t  ROSTER_IDX_B = 11;
constexpr uint16_t CFG_IDX_B    = 19;

// Sets up buildings[player][roster_idx] with `.building_id = cfg_idx` and `.shuttle_slot =
// shuttle_slot`, and stamps `type` into cfg_buildings[cfg_idx].type -- the record the function is
// actually meant to read. cfg_buildings[roster_idx] (a DIFFERENT cfg slot, since roster_idx != cfg_idx)
// is deliberately POISONED with the OPPOSITE classification: qualifying when `type` should miss the
// four-way gate, non-qualifying when `type` should pass it. An index-confusion bug that read
// cfg_buildings[roster_idx] instead of cfg_buildings[b.building_id] would then flip the verdict
// instead of accidentally agreeing with it.
building &setup_building(sim_fixture &fx, uint16_t player, int32_t roster_idx, uint16_t cfg_idx, uint8_t type,
                         uint8_t shuttle_slot) {
    building &b                    = fx.b(player, roster_idx);
    b.building_id                  = cfg_idx;
    b.shuttle_slot                 = shuttle_slot;
    fx.cfg_buildings[cfg_idx].type = type;

    const bool should_qualify = (type == BUILDING_TYPE_A_PORT || type == BUILDING_TYPE_H_PORT ||
                                 type == BUILDING_TYPE_A_MOTHER || type == BUILDING_TYPE_H_MOTHER);
    fx.cfg_buildings[roster_idx].type =
        should_qualify ? static_cast<uint8_t>(0x00) : BUILDING_TYPE_A_PORT;
    return b;
}

prod_shuttle_slot &slot_at(sim_fixture &fx, uint16_t player, uint8_t shuttle_slot) {
    return fx.prod_shuttle_slots[player * PROD_SHUTTLE_SLOTS_PER_PLAYER + shuttle_slot];
}

int32_t query(sim_fixture &fx, uint16_t player, int32_t roster_idx) {
    return detail::bldg_shuttle_slot_is_free(fx.view(), player, roster_idx);
}

} // namespace

void run_bldg_shuttle_slot_is_free_tests() {
    sim_fixture fx;

    // =================================================================================================
    // GATE -- exact matches on all four qualifying types (shuttle_slot=0 so the only way to observe
    // "qualified" is FREE, since an unbound slot never reaches the reservation checks either).
    // =================================================================================================
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, BUILDING_TYPE_A_PORT, 0);
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 1u,
              "G1: type==A_PORT(0x0c) qualifies (CMP 0x0048fad1 / JZ 0x0048fad8), shuttle_slot=0 -> free");
    }
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, BUILDING_TYPE_H_PORT, 0);
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 1u,
              "G4: type==H_PORT(0x20) qualifies (CMP 0x0048faf7 / JNZ 0x0048fafe), shuttle_slot=0 -> free");
    }
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, BUILDING_TYPE_A_MOTHER, 0);
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 1u,
              "G7: type==A_MOTHER(0x06) qualifies (CMP 0x0048fb1f / JNZ 0x0048fb26), shuttle_slot=0 -> free");
    }
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, BUILDING_TYPE_H_MOTHER, 0);
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 1u,
              "G10: type==H_MOTHER(0x1a) qualifies (CMP 0x0048fb47 / JNZ 0x0048fb4e), shuttle_slot=0 -> free");
    }

    // =================================================================================================
    // GATE -- near-miss boundaries on BOTH sides of every one of the four constants: each must fall
    // through to the all-miss path and return -1 (0xffffffff), 0x0048fb52.
    // =================================================================================================
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, 0x0b, 0);
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 0xffffffffu,
              "G2: type=0x0b (A_PORT-1) misses the gate -> -1, 0x0048fb52");
    }
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, 0x0d, 0);
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 0xffffffffu,
              "G3: type=0x0d (A_PORT+1) misses the gate -> -1, 0x0048fb52");
    }
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, 0x1f, 0);
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 0xffffffffu,
              "G5: type=0x1f (H_PORT-1) misses the gate -> -1, 0x0048fb52");
    }
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, 0x21, 0);
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 0xffffffffu,
              "G6: type=0x21 (H_PORT+1) misses the gate -> -1, 0x0048fb52");
    }
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, 0x05, 0);
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 0xffffffffu,
              "G8: type=0x05 (A_MOTHER-1) misses the gate -> -1, 0x0048fb52");
    }
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, 0x07, 0);
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 0xffffffffu,
              "G9: type=0x07 (A_MOTHER+1) misses the gate -> -1, 0x0048fb52");
    }
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, 0x19, 0);
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 0xffffffffu,
              "G11: type=0x19 (H_MOTHER-1) misses the gate -> -1, 0x0048fb52");
    }
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, 0x1b, 0);
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 0xffffffffu,
              "G12: type=0x1b (H_MOTHER+1) misses the gate -> -1, 0x0048fb52");
    }

    // =================================================================================================
    // GATE -- generic sentinel misses (0x00, 0xff), ALSO proving the gate short-circuits: shuttle_slot
    // is set to a slot whose record is BUSY (nonzero passengers_reserved), and the result must still be
    // -1, not 0 -- a mutant that fell through to the scan on a gate miss would return 0 here instead.
    // =================================================================================================
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, 0x00, 4);
        slot_at(fx, PLAYER, 4).passengers_reserved = 99;
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 0xffffffffu,
              "G13: type=0x00 misses the gate -> -1 even though the bound slot is busy (gate "
              "short-circuits BEFORE 0x0048fb5e)");
    }
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, 0xff, 4);
        slot_at(fx, PLAYER, 4).passengers_reserved = 99;
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 0xffffffffu,
              "G14: type=0xff (max byte sentinel) misses the gate -> -1 even though the bound slot is "
              "busy, same short-circuit as G13");
    }

    // =================================================================================================
    // SCAN -- every case below uses a fixed qualifying type (A_PORT) so the gate is never in question;
    // only the shuttle-slot scan is exercised.
    // =================================================================================================

    // S1: shuttle_slot==0 (unbound) skips the scan ENTIRELY -- the slot-0 record is poisoned busy and
    // must still be ignored, 0x0048fb78 JZ 0x0048fbe7.
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, BUILDING_TYPE_A_PORT, 0);
        prod_shuttle_slot &s0    = slot_at(fx, PLAYER, 0);
        s0.passengers_reserved   = 77;
        s0.resources_reserved[3] = 88;
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 1u,
              "S1: shuttle_slot==0 skips the reservation record entirely -> free, 0x0048fb78");
    }

    // S2: shuttle_slot==1 (first slot immediately above the unbound sentinel), passengers_reserved!=0
    // -> busy immediately, 0x0048fb92-0x0048fba2, before any resources_reserved read.
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, BUILDING_TYPE_A_PORT, 1);
        slot_at(fx, PLAYER, 1).passengers_reserved = 5;
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 0u,
              "S2: shuttle_slot=1, passengers_reserved!=0 -> busy, 0x0048fb92-0x0048fba2");
    }

    // S3: shuttle_slot==9 (top of the 10-slot-per-player domain), passengers_reserved==0,
    // resources_reserved[1]!=0 -- the FIRST index the loop visits (r starts at 1) -- busy,
    // 0x0048fbd3 on the loop's first iteration.
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, BUILDING_TYPE_A_PORT, 9);
        slot_at(fx, PLAYER, 9).resources_reserved[1] = 1;
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 0u,
              "S3: resources_reserved[1] (first scanned index) != 0 -> busy, 0x0048fbd3 iter r=1");
    }

    // S4: shuttle_slot==9, resources_reserved[9]!=0 ONLY -- the LAST index the loop visits (r<10) --
    // busy. A mutant that "cleaned up" the loop bound to 9 (r<9) would miss index 9 and wrongly return
    // free here.
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, BUILDING_TYPE_A_PORT, 9);
        slot_at(fx, PLAYER, 9).resources_reserved[9] = 1;
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 0u,
              "S4: resources_reserved[9] (last scanned index, r<10 bound) != 0 -> busy, 0x0048fbd3 "
              "iter r=9 -- would wrongly read FREE under a r<9 bound");
    }

    // S5: resources_reserved[0]!=0 ONLY, everything else (incl. passengers) zero -- index 0 is NEVER
    // visited (loop starts at r=1), a quirk of the original PRESERVED here rather than "fixed": the
    // buggy-looking but faithful result is FREE, not busy.
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, BUILDING_TYPE_A_PORT, 5);
        slot_at(fx, PLAYER, 5).resources_reserved[0] = 1;
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 1u,
              "S5 [PRESERVE-BUG]: resources_reserved[0]!=0 is NEVER checked (loop starts at r=1, "
              "0x0048fba4) -> free, NOT busy -- do not 'fix' this to start at r=0");
    }

    // S6: resources_reserved[1]==0 but resources_reserved[2]!=0 -- the SECOND scanned slot qualifies
    // while the first does not, pinning that the scan walks forward in order rather than stopping (or
    // being assumed to stop) at the first slot.
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, BUILDING_TYPE_A_PORT, 5);
        slot_at(fx, PLAYER, 5).resources_reserved[2] = 1;
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 0u,
              "S6: resources_reserved[1]==0, [2]!=0 -> busy, pinning the scan reaches r=2 "
              "(0x0048fbd3 iter r=2) rather than stopping after r=1");
    }

    // S7: fully clean record (passengers==0, all 10 resources_reserved==0) -> the loop runs to
    // completion without a hit and falls out to 0x0048fbe7 -> free.
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, BUILDING_TYPE_A_PORT, 5);
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 1u,
              "S7: passengers==0, all resources_reserved==0 -> loop exhausts with no hit -> free, "
              "0x0048fbe7");
    }

    // S8: a SECOND, independent (player, roster_idx, cfg_idx) identity is set up BUSY while the
    // primary (PLAYER, ROSTER_IDX) identity from S7 is left FREE-shaped -- pins that the function
    // indexes buildings[]/prod_shuttle_slots[] with the CALLER-SUPPLIED player, not a hardcoded row:
    // a player-ignoring mutant would read PLAYER's clean record and wrongly return free.
    {
        fx.reset();
        setup_building(fx, PLAYER, ROSTER_IDX, CFG_IDX, BUILDING_TYPE_A_PORT, 5); // clean/free-shaped
        setup_building(fx, PLAYER_B, ROSTER_IDX_B, CFG_IDX_B, BUILDING_TYPE_H_MOTHER, 3);
        slot_at(fx, PLAYER_B, 3).resources_reserved[9] = 1;
        ck_eq((uint32_t)query(fx, PLAYER_B, ROSTER_IDX_B), 0u,
              "S8: a SECOND player's own record is busy while PLAYER's is clean -> busy, pinning the "
              "player argument actually selects the row (not hardcoded), 0x0048fb92/0x0048fbd3");
        ck_eq((uint32_t)query(fx, PLAYER, ROSTER_IDX), 1u,
              "S8b: PLAYER's own (unrelated) record is unaffected by PLAYER_B's setup -> still free");
    }
}

} // namespace mh::sim::test
