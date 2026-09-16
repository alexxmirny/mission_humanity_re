//
// sim_unit_bldg_apply_scaled_damage_selftest.cpp -- OFFLINE `simtest` oracle for
// llm_unit_bldg_apply_scaled_damage @0x0049a9aa (sim/sim_unit_bldg_apply_scaled_damage.h/.cpp,
// RI-SIM SIM1E). Order-code 0xf7 (llm_strat_order_queue_dispatch Table B case 0x12); the
// debug-console command that issues it is '_DESTROY' (case 0x20).
//
// arm_ready:false -- this is a debug-console-only order (no AI/skirmish path issues it: a 15000-step
// all-AI soak made ZERO calls at this site). This offline oracle is the ONLY evidence this function
// will ever have.
//
// THE ONE THING THIS FILE MUST PIN (per reimpl-verify's own finding): the unit arm and the building
// arm are NOT a symmetric copy-paste pair. The unit arm MULTIPLIES the target's max-ENERGY by
// UNIT_DAMAGE_SCALE=0.5 (FMUL, 0x0049aa0c, resolved read-memory bytes `00 00 00 00 00 00 E0 3F`); the
// building arm DIVIDES by BLDG_DAMAGE_SCALE=3.0 (FDIV, 0x0049aa53, resolved read-memory bytes
// `00 00 00 00 00 00 08 40`). Every case below that exercises one arm uses a value where *0.5 and
// /3.0 are far apart, so a mutation that swaps the two constants (or unifies them to one value) fails
// at least one of T1/T3/T7/T8. ENERGY here is the HP-like stat (unit hit points / a
// building's construction-progress charge) -- NOT the POWER resource; this function stages damage
// into `pending_damage`, it never touches POWER.
//
// EXPECTED BEHAVIOUR from the .asm (tmp/decomp_sim/llm_unit_bldg_apply_scaled_damage_0049a9aa.asm)
// and the header's derivation (sim_unit_bldg_apply_scaled_damage.h):
//   0x0049a9c7-0x0049a9cd: player = target_selector & 0xf, computed ONCE, reused in BOTH arms.
//   0x0049a9d0-0x0049a9d7: TEST target_selector,0x80; JZ building_label -- bit 0x80 SET -> unit arm
//     (fallthrough from entry); bit 0x80 CLEAR -> building arm. No other bit of target_selector is
//     ever tested.
//   UNIT ARM (0x0049a9d9-0x0049aa1e): units[player][target_index].pending_damage +=
//     Unit[units[player][target_index].unit_proto_id].energy * 0.5. ONE FLD/FMUL/FADD/FSTP chain,
//     unconditional, no clamp against the target's own current energy (HP) and no floor on the
//     resulting pending_damage.
//   BUILDING ARM (0x0049aa20-0x0049aa65): buildings[player][target_index].pending_damage +=
//     Building[buildings[player][target_index].building_id].energy / 3.0. Same shape, same
//     unconditional/no-clamp behaviour, DIFFERENT roster strides (BUILDINGS_PER_PLAYER's 0x6aa4/0x111
//     vs UNITS_PER_PLAYER's 0x5b04/0xe9) and a DIFFERENT cfg row stride (sizeof(cfg_building)=0x842 vs
//     sizeof(cfg_unit)=0x23f).
//   No outward calls besides the inert utils_assert_stack_capacity probe -- no `_calls` struct.
//
#include "sim/sim_unit_bldg_apply_scaled_damage.h"

#include "sim_test_support.h"

namespace mh::sim::test {

void run_unit_bldg_apply_scaled_damage_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- UNIT ARM, target_index at its LOW bound (0). Pins the FMUL-0.5 constant: 40.0*0.5+7.0=27.0
    // would read 40.0/3.0+7.0=20.333... if the two arms' constants were swapped, and 40.0*3.0+7.0=127.0
    // (or 40.0/0.5+7.0=87.0) if they were unified to one value -- either mutation fails this check.
    // =================================================================================================
    {
        fx.reset();

        constexpr uint32_t TARGET_SELECTOR = 0x83u; // player=3 (low nibble), bit 0x80 SET -> unit arm
        constexpr int32_t  TARGET_INDEX    = 0;     // low bound
        constexpr uint16_t PROTO_ID        = 11;

        fx.cfg_units[PROTO_ID].energy = 40.0;
        unit &u                       = fx.u(3, TARGET_INDEX);
        u.unit_proto_id               = PROTO_ID;
        u.pending_damage              = 7.0;

        sim_store own = fx.store();
        detail::unit_bldg_apply_scaled_damage(fx.view(), own, TARGET_SELECTOR, TARGET_INDEX);

        ck_eq_d(u.pending_damage, 27.0,
                "T1: unit pending_damage = Unit[proto].energy*0.5 + pending_damage "
                "(40.0*0.5+7.0=27.0), FMUL 0.5 pinned against the BLDG arm's /3.0, 0x0049aa0c");
    }

    // =================================================================================================
    // T2 -- UNIT ARM, target_index at its HIGH bound (99, UNITS_PER_PLAYER-1). Pins the roster-index
    // arithmetic (player*0x5b04 + target_index*0xe9, 0x0049a9d9-0x0049a9f7): three neighbour slots --
    // a different player at the same index, another different player at the same index, and the same
    // player at index 98 -- are seeded with sentinels and must survive untouched.
    // =================================================================================================
    {
        fx.reset();

        constexpr uint32_t TARGET_SELECTOR = 0x81u; // player=1, bit 0x80 SET -> unit arm
        constexpr int32_t  TARGET_INDEX    = 99;    // high bound (UNITS_PER_PLAYER-1)
        constexpr uint16_t PROTO_ID        = 55;

        fx.cfg_units[PROTO_ID].energy = 20.0;
        unit &u                       = fx.u(1, TARGET_INDEX);
        u.unit_proto_id               = PROTO_ID;
        u.pending_damage              = 3.0;

        unit &neighbor_player_a          = fx.u(0, 99);
        neighbor_player_a.pending_damage = 111.0;
        unit &neighbor_player_b          = fx.u(2, 99);
        neighbor_player_b.pending_damage = 222.0;
        unit &neighbor_index             = fx.u(1, 98);
        neighbor_index.pending_damage    = 333.0;

        sim_store own = fx.store();
        detail::unit_bldg_apply_scaled_damage(fx.view(), own, TARGET_SELECTOR, TARGET_INDEX);

        ck_eq_d(u.pending_damage, 13.0,
                "T2: unit pending_damage = 20.0*0.5+3.0=13.0 at the HIGH index bound, "
                "0x0049aa06-0x0049aa18");
        ck_eq_d(neighbor_player_a.pending_damage, 111.0,
                "T2: a DIFFERENT player at the SAME index untouched -- player stride 0x5b04, "
                "0x0049a9d9");
        ck_eq_d(neighbor_player_b.pending_damage, 222.0,
                "T2: a THIRD player at the SAME index untouched -- player stride 0x5b04, 0x0049a9d9");
        ck_eq_d(neighbor_index.pending_damage, 333.0,
                "T2: the SAME player at a DIFFERENT index (98) untouched -- index stride 0xe9, "
                "0x0049a9e0");
    }

    // =================================================================================================
    // T3 -- BUILDING ARM, target_index at its LOW bound (0). Pins the FDIV-3.0 constant: 90.0/3.0+4.0
    // =34.0 would read 90.0*0.5+4.0=49.0 if the two arms' constants were swapped, and 90.0/0.5+4.0=
    // 184.0 (or 90.0*3.0+4.0=274.0) if unified to one value -- either mutation fails this check.
    // =================================================================================================
    {
        fx.reset();

        constexpr uint32_t TARGET_SELECTOR = 0x05u; // player=5, bit 0x80 CLEAR -> building arm
        constexpr int32_t  TARGET_INDEX    = 0;     // low bound
        constexpr uint16_t BID             = 33;

        fx.cfg_buildings[BID].energy = 90.0;
        building &b                  = fx.b(5, TARGET_INDEX);
        b.building_id                = BID;
        b.pending_damage             = 4.0;

        sim_store own = fx.store();
        detail::unit_bldg_apply_scaled_damage(fx.view(), own, TARGET_SELECTOR, TARGET_INDEX);

        ck_eq_d(b.pending_damage, 34.0,
                "T3: building pending_damage = Building[bid].energy/3.0 + pending_damage "
                "(90.0/3.0+4.0=34.0), FDIV 3.0 pinned against the UNIT arm's *0.5, 0x0049aa53");
    }

    // =================================================================================================
    // T4 -- BUILDING ARM, target_index at its HIGH bound (99, BUILDINGS_PER_PLAYER-1). Pins the
    // roster-index arithmetic (player*0x6aa4 + target_index*0x111, 0x0049aa20-0x0049aa3e) -- DIFFERENT
    // strides from the unit arm's -- via the same three-sentinel neighbour-isolation shape as T2.
    // =================================================================================================
    {
        fx.reset();

        constexpr uint32_t TARGET_SELECTOR = 0x02u; // player=2, bit 0x80 CLEAR -> building arm
        constexpr int32_t  TARGET_INDEX    = 99;    // high bound (BUILDINGS_PER_PLAYER-1)
        constexpr uint16_t BID             = 77;

        fx.cfg_buildings[BID].energy = 60.0;
        building &b                  = fx.b(2, TARGET_INDEX);
        b.building_id                = BID;
        b.pending_damage             = 9.0;

        building &neighbor_player_a      = fx.b(1, 99);
        neighbor_player_a.pending_damage = 444.0;
        building &neighbor_player_b      = fx.b(3, 99);
        neighbor_player_b.pending_damage = 555.0;
        building &neighbor_index         = fx.b(2, 98);
        neighbor_index.pending_damage    = 666.0;

        sim_store own = fx.store();
        detail::unit_bldg_apply_scaled_damage(fx.view(), own, TARGET_SELECTOR, TARGET_INDEX);

        ck_eq_d(b.pending_damage, 29.0,
                "T4: building pending_damage = 60.0/3.0+9.0=29.0 at the HIGH index bound, "
                "0x0049aa4d-0x0049aa5f");
        ck_eq_d(neighbor_player_a.pending_damage, 444.0,
                "T4: a DIFFERENT player at the SAME index untouched -- player stride 0x6aa4, "
                "0x0049aa20");
        ck_eq_d(neighbor_player_b.pending_damage, 555.0,
                "T4: a THIRD player at the SAME index untouched -- player stride 0x6aa4, 0x0049aa20");
        ck_eq_d(neighbor_index.pending_damage, 666.0,
                "T4: the SAME player at a DIFFERENT index (98) untouched -- index stride 0x111, "
                "0x0049aa27");
    }

    // =================================================================================================
    // T5 -- SELECTOR MASKING: garbage in every bit of target_selector OUTSIDE the low nibble and bit
    // 0x80 (upper 24 bits AND bits 0x10/0x20/0x40) must not change the outcome -- only `& 0xf` (player)
    // and `& 0x80` (arm select) are ever read, 0x0049a9c7-0x0049a9d7. Cross-arm isolation: the
    // building record at the SAME (player,index) must stay untouched since bit 0x80 selects unit here.
    // =================================================================================================
    {
        fx.reset();

        // low byte 0x96 = 1001 0110: nibble=6 (player), bit 0x80 SET (unit arm); every other bit
        // (0xDEADBE00 plus 0x10/0x20/0x40 of the low byte) is garbage that must be ignored.
        constexpr uint32_t TARGET_SELECTOR = 0xDEADBE96u;
        constexpr int32_t  TARGET_INDEX    = 50;
        constexpr uint16_t PROTO_ID        = 3;

        fx.cfg_units[PROTO_ID].energy = 8.0;
        unit &u                       = fx.u(6, TARGET_INDEX);
        u.unit_proto_id               = PROTO_ID;
        u.pending_damage              = 1.0;

        building &same_coord_bldg      = fx.b(6, TARGET_INDEX);
        same_coord_bldg.pending_damage = 999.0;

        sim_store own = fx.store();
        detail::unit_bldg_apply_scaled_damage(fx.view(), own, TARGET_SELECTOR, TARGET_INDEX);

        ck_eq_d(u.pending_damage, 5.0,
                "T5: garbage in target_selector's upper 24 bits + unused low-byte bits does not "
                "change the result (8.0*0.5+1.0=5.0), only &0xf/&0x80 are read, 0x0049a9ca/0x0049a9d0");
        ck_eq_d(same_coord_bldg.pending_damage, 999.0,
                "T5: the building record at the SAME (player,index) is untouched -- bit 0x80 SET "
                "selected the unit arm only, 0x0049a9d7");
    }

    // =================================================================================================
    // T6 -- PLAYER NIBBLE LOWER BOUND (0) with bit 0x80 SET in the SAME byte: proves `player =
    // target_selector & 0xf` (0x0049a9ca) is truly independent of the `& 0x80` arm-select test --
    // player must resolve to 0, not be corrupted by the set arm bit. Cross-arm isolation repeated.
    // =================================================================================================
    {
        fx.reset();

        constexpr uint32_t TARGET_SELECTOR = 0x80u; // player=0 (low nibble), bit 0x80 SET -> unit arm
        constexpr int32_t  TARGET_INDEX    = 10;
        constexpr uint16_t PROTO_ID        = 9;

        fx.cfg_units[PROTO_ID].energy = 6.0;
        unit &u                       = fx.u(0, TARGET_INDEX);
        u.unit_proto_id               = PROTO_ID;
        u.pending_damage              = 2.0;

        building &same_coord_bldg      = fx.b(0, TARGET_INDEX);
        same_coord_bldg.pending_damage = 888.0;

        sim_store own = fx.store();
        detail::unit_bldg_apply_scaled_damage(fx.view(), own, TARGET_SELECTOR, TARGET_INDEX);

        ck_eq_d(u.pending_damage, 5.0,
                "T6: player=0 resolved correctly from target_selector=0x80 (6.0*0.5+2.0=5.0), the "
                "nibble mask is independent of bit 0x80, 0x0049a9ca");
        ck_eq_d(same_coord_bldg.pending_damage, 888.0,
                "T6: building record at player 0's SAME index untouched -- unit arm fired, not "
                "building, 0x0049a9d7");
    }

    // =================================================================================================
    // T7 -- UNIT ARM, "below zero": the target's CURRENT energy (HP) is far SMALLER than the scaled
    // damage this call stages. This function has NO clamp against the target's own energy (no CMP
    // after the FADD/FSTP in the .asm) -- PRESERVE that: pending_damage ends up far exceeding energy,
    // which would drive the target well below zero once llm_strat_unit_apply_damage later subtracts
    // it. energy itself is untouched -- this order only STAGES damage, it never subtracts.
    // =================================================================================================
    {
        fx.reset();

        constexpr uint32_t TARGET_SELECTOR = 0x84u; // player=4, bit 0x80 SET -> unit arm
        constexpr int32_t  TARGET_INDEX    = 20;
        constexpr uint16_t PROTO_ID        = 44;

        fx.cfg_units[PROTO_ID].energy = 200.0; // cfg MAX energy (drives the scaled-damage amount)
        unit &u                       = fx.u(4, TARGET_INDEX);
        u.unit_proto_id               = PROTO_ID;
        u.energy                      = 5.0; // current HP, far below the scaled damage about to land
        u.pending_damage              = 0.0;

        sim_store own = fx.store();
        detail::unit_bldg_apply_scaled_damage(fx.view(), own, TARGET_SELECTOR, TARGET_INDEX);

        ck_eq_d(u.pending_damage, 100.0,
                "T7: PRESERVE-BUG(no clamp): pending_damage = 200.0*0.5+0.0=100.0, far exceeding the "
                "target's current energy (5.0) -- no guard exists, 0x0049aa06-0x0049aa18");
        ck_eq_d(u.energy, 5.0,
                "T7: unit.energy (current HP) is untouched -- this order only stages pending_damage, "
                "it does not subtract (llm_strat_unit_apply_damage does that later)");
    }

    // =================================================================================================
    // T8 -- BUILDING ARM, "below zero" analog, with an ALREADY-NEGATIVE pre-existing pending_damage
    // (e.g. from some earlier accounting): the unconditional FADD adds the scaled damage on top with
    // no floor, and the target's current energy is again far smaller than the result. energy itself
    // is untouched, same as T7's unit-arm case.
    // =================================================================================================
    {
        fx.reset();

        constexpr uint32_t TARGET_SELECTOR = 0x03u; // player=3, bit 0x80 CLEAR -> building arm
        constexpr int32_t  TARGET_INDEX    = 25;
        constexpr uint16_t BID             = 88;

        fx.cfg_buildings[BID].energy = 300.0; // cfg MAX energy (drives the scaled-damage amount)
        building &b                  = fx.b(3, TARGET_INDEX);
        b.building_id                = BID;
        b.energy                     = 4.0;  // current charge, far below the scaled damage about to land
        b.pending_damage             = -2.0; // already negative pre-existing value, no floor clamps it

        sim_store own = fx.store();
        detail::unit_bldg_apply_scaled_damage(fx.view(), own, TARGET_SELECTOR, TARGET_INDEX);

        ck_eq_d(b.pending_damage, 98.0,
                "T8: PRESERVE-BUG(no clamp/no floor): pending_damage = 300.0/3.0+(-2.0)=98.0, far "
                "exceeding the target's current energy (4.0), unconditional add onto a negative "
                "starting value, 0x0049aa4d-0x0049aa5f");
        ck_eq_d(b.energy, 4.0,
                "T8: building.energy (current charge) is untouched -- this order only stages "
                "pending_damage, it does not subtract (llm_strat_bldg_apply_damage does that later)");
    }
}

} // namespace mh::sim::test
