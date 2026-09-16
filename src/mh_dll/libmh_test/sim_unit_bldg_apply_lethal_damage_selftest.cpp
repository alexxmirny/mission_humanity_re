//
// sim_unit_bldg_apply_lethal_damage_selftest.cpp -- `simtest` oracle for
// llm_unit_bldg_apply_lethal_damage @0x0049aa6e (sim/sim_unit_bldg_apply_lethal_damage.h/.cpp,
// RI-SIM SIM1E, order-code 0xf8 -- the "HARA KIRI" debug-console instant-kill handler).
//
// arm_ready:false -- the site was armed and made ZERO calls in a 15000-step all-AI soak (debug-
// console-only order, no AI/skirmish path issues it). This offline oracle is the ONLY evidence
// this function will ever have -- see the header banner's own note.
//
// NO CALLEES, NO LOOP: both arms are a single memory-to-memory double add
// (Unit[proto].energy / Building[id].energy + roster.pending_damage), immediately stored back.
// So there are no recorder mocks to declare -- everything rides on the exact roster cells the
// two arms touch, which is why every case below asserts the full write set (changed fields),
// the untouched set (instance ENERGY, identity ids, the OTHER roster entirely), and a
// neighbouring roster slot (to catch a wrong index rather than merely a wrong value).
//
// EXPECTED BEHAVIOUR from the header banner's own derivation (sim_unit_bldg_apply_lethal_damage.h):
//   0x0049aa8b-0x0049aa91: player = target_ref & 0xf, computed ONCE, reused in BOTH arms.
//   0x0049aa94-0x0049aa9b: TEST target_ref,0x80; JZ <building arm>. Bit SET -> unit arm (falls
//     straight through); bit CLEAR -> building arm (jump taken).
//   UNIT ARM (0x0049aa9d-0x0049aadc): units[player][target_index].unit_proto_id selects
//     Unit[proto_id].energy (cfg max-energy); units[player][target_index].pending_damage +=
//     that value (FLD cfg energy, FADD roster pending_damage, FSTP back -- an ACCUMULATE, not a
//     copy).
//   BUILDING ARM (0x0049aade-0x0049ab1d): same shape over
//     buildings[player][target_index].building_id / Building[building_id].energy /
//     .pending_damage.
//   Neither arm ever touches the roster's own instance `.energy` field (the HP/charge value) --
//   only `.pending_damage` is written; ENERGY (HP-like) and the cfg row's `.energy` (max-energy)
//   are DIFFERENT numbers from POWER (the generated/consumed resource), per the domain
//   note, and this function touches neither the instance energy nor POWER at all.
//
#include "sim/sim_unit_bldg_apply_lethal_damage.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// No callees in this body -- no recorder mocks needed. Just the roster coordinates used across
// cases, named so each case's intent reads without re-deriving the arithmetic.
constexpr int32_t UNIT_PLAYER         = 3;
constexpr int32_t UNIT_INDEX          = 9;
constexpr int32_t UNIT_NEIGHBOR_INDEX = UNIT_INDEX + 1; // same row, index+1 -- catches a wrong index

constexpr int32_t BLDG_PLAYER         = 7; // MAX_PLAYERS - 1: the highest valid player nibble
constexpr int32_t BLDG_INDEX          = 42;
constexpr int32_t BLDG_NEIGHBOR_INDEX = BLDG_INDEX + 1;

} // namespace

void run_unit_bldg_apply_lethal_damage_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- unit arm, bit 0x80 SET selects it, player extracted from the LOW NIBBLE while unrelated
    // high bits (0x30) are ignored, pending_damage ACCUMULATES (does not overwrite) the cfg row's
    // energy selected by unit_proto_id (NOT by target_index -- picked distinct on purpose), and the
    // instance's own `.energy` (HP) field, unit_proto_id, the neighbouring unit slot, and the WHOLE
    // building roster are all left untouched.
    // =================================================================================================
    {
        fx.reset();
        unit &u         = fx.u(UNIT_PLAYER, UNIT_INDEX);
        u.unit_proto_id = 7;            // distinct from UNIT_INDEX(9) -- proves the cfg row comes from the
                                        // roster's OWN proto id, not from target_index
        u.pending_damage = 5.5;         // pre-existing accumulator; must be ADDED to, not clobbered
        u.energy         = 40.0;        // instance HP-like ENERGY -- must stay untouched (this fn never
                                        // writes it, only pending_damage)
        fx.cfg_units[7].energy = 12.25; // cfg max-energy for proto 7, distinct from every other seed

        unit &neighbor          = fx.u(UNIT_PLAYER, UNIT_NEIGHBOR_INDEX);
        neighbor.pending_damage = 111.0; // sentinel: must stay untouched

        building &other_roster_slot      = fx.b(UNIT_PLAYER, UNIT_INDEX);
        other_roster_slot.pending_damage = 222.0; // sentinel: building roster untouched on unit arm

        // target_ref = 0xB3 = 1011_0011: bit 0x80 SET (unit arm), low nibble 0x3 = UNIT_PLAYER,
        // bits 0x30 are "noise" proving the player extraction really masks with 0xf (0x0049aa8e).
        sim_store own = fx.store();
        detail::unit_bldg_apply_lethal_damage(fx.view(), own, 0xB3u, UNIT_INDEX);

        ck_eq_d(u.pending_damage, 5.5 + 12.25,
                "T1: unit pending_damage = cfg_units[unit_proto_id].energy + pre-existing "
                "pending_damage (ACCUMULATE, not overwrite), 0x0049aaca-0x0049aad6");
        ck_eq_d(u.energy, 40.0,
                "T1: unit's own instance ENERGY (HP) field untouched -- only pending_damage is "
                "written, ENERGY-vs-pending_damage distinction, no write site for it in this fn");
        ck_eq((uint32_t)u.unit_proto_id, 7u,
              "T1: unit_proto_id untouched -- identity field only READ to select the cfg row");
        ck_eq_d(neighbor.pending_damage, 111.0,
                "T1: neighbouring unit slot (player 3, index 10) untouched -- row/index math "
                "(IMUL player,0x5b04 / IMUL target_index,0xe9, 0x0049aa9d-0x0049aabd) writes "
                "exactly the addressed slot");
        ck_eq_d(other_roster_slot.pending_damage, 222.0,
                "T1: building roster entirely untouched on the unit arm -- confirms the branch "
                "at 0x0049aa94-0x0049aa9b selects exactly one roster, never both");
    }

    // =================================================================================================
    // T2 -- building arm, bit 0x80 CLEAR selects it (mirrors T1's mask-invariance proof with 0x70 of
    // noise bits), player = BLDG_PLAYER (7, the highest valid nibble), pending_damage ACCUMULATES the
    // cfg row's energy selected by building_id (distinct from target_index), instance `.energy`
    // (charge/HP), building_id, the neighbouring building slot, and the WHOLE unit roster are left
    // untouched.
    // =================================================================================================
    {
        fx.reset();
        building &b                 = fx.b(BLDG_PLAYER, BLDG_INDEX);
        b.building_id               = 15;   // distinct from BLDG_INDEX(42)
        b.pending_damage            = 8.0;  // pre-existing accumulator
        b.energy                    = 77.0; // instance HP-like CHARGE -- must stay untouched
        fx.cfg_buildings[15].energy = 33.5;

        building &neighbor      = fx.b(BLDG_PLAYER, BLDG_NEIGHBOR_INDEX);
        neighbor.pending_damage = 333.0; // sentinel: must stay untouched

        unit &other_roster_slot          = fx.u(BLDG_PLAYER, BLDG_INDEX);
        other_roster_slot.pending_damage = 444.0; // sentinel: unit roster untouched on building arm

        // target_ref = 0x77 = 0111_0111: bit 0x80 CLEAR (building arm), low nibble 0x7 = BLDG_PLAYER,
        // bits 0x70 are noise proving the 0xf mask again on the building path.
        sim_store own = fx.store();
        detail::unit_bldg_apply_lethal_damage(fx.view(), own, 0x77u, BLDG_INDEX);

        ck_eq_d(b.pending_damage, 8.0 + 33.5,
                "T2: building pending_damage = cfg_buildings[building_id].energy + pre-existing "
                "pending_damage (ACCUMULATE), 0x0049ab0b-0x0049ab17");
        ck_eq_d(b.energy, 77.0,
                "T2: building's own instance ENERGY (charge/HP) field untouched -- ENERGY vs "
                "POWER/pending_damage distinction (ENERGY is not POWER), no write site here");
        ck_eq((uint32_t)b.building_id, 15u,
              "T2: building_id untouched -- identity field only READ to select the cfg row");
        ck_eq_d(neighbor.pending_damage, 333.0,
                "T2: neighbouring building slot (player 7, index 43) untouched -- row/index math "
                "(IMUL player,0x6aa4 / IMUL target_index,0x111, 0x0049aade-0x0049aafe) writes "
                "exactly the addressed slot");
        ck_eq_d(other_roster_slot.pending_damage, 444.0,
                "T2: unit roster entirely untouched on the building arm -- confirms the branch "
                "at 0x0049aa94-0x0049aa9b selects exactly one roster, never both");
    }

    // =================================================================================================
    // T3 -- exact bit boundary, unit side: target_ref == 0x80 with NO other bits set at all (player
    // nibble 0, no noise bits), and pending_damage starts at EXACTLY 0.0 -- proving the write is a
    // real ADD (0.0 + cfg_energy) rather than an accidental copy that would look identical whenever
    // the accumulator happens to start at zero elsewhere.
    // =================================================================================================
    {
        fx.reset();
        unit &u          = fx.u(0, 0);
        u.unit_proto_id  = 1;
        u.pending_damage = 0.0; // starts at zero -- ADD vs COPY are indistinguishable unless
                                // paired with T1's non-zero-start case
        fx.cfg_units[1].energy = 99.0;

        unit &neighbor          = fx.u(0, 1);
        neighbor.pending_damage = 55.0; // sentinel

        sim_store own = fx.store();
        detail::unit_bldg_apply_lethal_damage(fx.view(), own, 0x80u, 0);

        ck_eq_d(u.pending_damage, 99.0,
                "T3: target_ref==0x80 exactly (bit SET, player nibble 0) still takes the unit "
                "arm -- JZ NOT taken at 0x0049aa9b, pending_damage = 0.0 + cfg energy");
        ck_eq_d(neighbor.pending_damage, 55.0,
                "T3: neighbouring unit slot (player 0, index 1) untouched at the index-0 edge");
    }

    // =================================================================================================
    // T4 -- exact bit boundary, building side: target_ref == 0x00 (bit CLEAR, player nibble 0), at
    // target_index == 99 (BUILDINGS_PER_PLAYER-1, the LAST valid slot of player 0's row) -- proves the
    // row-stride math does not spill into player 1's row at the far edge of the row.
    // =================================================================================================
    {
        fx.reset();
        building &b                = fx.b(0, 99);
        b.building_id              = 0; // edge cfg row too
        b.pending_damage           = 1.25;
        fx.cfg_buildings[0].energy = 2.75;

        // Immediately following flat slot is buildings[1][0] -- the row boundary itself.
        building &row_boundary      = fx.b(1, 0);
        row_boundary.pending_damage = 66.0; // sentinel: must NOT be reached by a row-stride bug

        sim_store own = fx.store();
        detail::unit_bldg_apply_lethal_damage(fx.view(), own, 0x00u, 99);

        ck_eq_d(b.pending_damage, 1.25 + 2.75,
                "T4: target_ref==0x00 exactly (bit CLEAR, player nibble 0) takes the building arm "
                "-- JZ taken at 0x0049aa9b, index 99 (last slot of the row)");
        ck_eq_d(row_boundary.pending_damage, 66.0,
                "T4: player 1's row (buildings[1][0], the very next flat slot) untouched -- row "
                "stride 0x6aa4 (100 * 0x111) keeps player 0's index-99 write inside its own row");
    }
}

} // namespace mh::sim::test
