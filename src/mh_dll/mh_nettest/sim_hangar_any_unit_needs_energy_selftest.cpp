//
// sim_hangar_any_unit_needs_energy_selftest.cpp -- `simtest` oracle for
// llm_strat_hangar_any_unit_needs_energy @0x0047b0db (sim/sim_hangar_energy.h/.cpp, RI-SIM / SIM1-G4).
//
// This function is a PURE QUERY (no writes, no calls besides the inert stack-capacity probe) --
// per the header banner it is shadowable by compare_return alone and does not strictly NEED an
// offline oracle for arming. This file exists anyway as extra, cheap, directly-address-cited
// evidence for the loop/gate logic itself (empty-hangar short circuit, the `>=` boundary, the
// sub_id-vs-index indirection, and the per-unit proto lookup) -- the kind of thing compare_return
// alone would only catch by accident if a live game state happened to exercise the exact branch.
//
// EXPECTED BEHAVIOUR, from the disassembly (tmp/decomp_sim/llm_strat_hangar_any_unit_needs_energy_
// 0047b0db.asm) and the header banner's derivation:
//   0x0047b0f8-0x0047b112: sub_id = buildings[player][index].sub_id (the hangar's STORAGE SLOT, NOT
//     `index` itself); loop cursor i starts at 0.
//   0x0047b129-0x0047b134 (CMP EAX,[EDX+0xc727c4] / JL 0x47b141 / JMP 0x47b1ab): `i < docked_count`,
//     SIGNED compare -- docked_count<=0 (0 or a negative sentinel) falls straight to the JMP and
//     returns 0 without ever reaching the docked_units/unit/cfg-unit reads below.
//   0x0047b141-0x0047b172: docked_units[i] read (0x0047b159), then units[player][unit_idx] addressed
//     and unit_proto_id read (0x0047b172, MOVZX) -- per docked unit, not cached across iterations.
//   0x0047b18f-0x0047b19e (FLD energy / FCOMP cfg_units[proto].energy / FNSTSW / SAHF / JNC
//     0x0047b1a9): JNC fires (CF=0) exactly on ORDERED `energy >= max`, and also agrees with IEEE-754
//     `>=` on any unordered/NaN pair (x87 sets CF=1 on unordered, so JNC is not taken either way) --
//     a faithful direct `if (u.energy >= cu.energy)` transcription for every input.
//   0x0047b1a9 (JNC target) -> JMP 0x0047b139: this unit is already full, advance i, loop back --
//     the "continue" arm, no found flag set.
//   0x0047b1a0-0x0047b1a7 (JNC NOT taken, i.e. fallthrough): found=1, jump straight to the return at
//     0x0047b1b2 -- the loop does NOT keep scanning once a needy unit is found.
//   0x0047b1ab (loop exhausted OR docked_count<=0 short-circuit): found=0, falls into the same return.
//
#include "sim/sim_hangar_energy.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// PLAYER/B_INDEX/STORAGE_SLOT are deliberately DISTINCT non-zero, non-equal values: B_INDEX (the
// `index` param, a BUILDING index) and STORAGE_SLOT (the value put in building.sub_id, the actual
// storage-array slot) must differ so a translation that conflates "index" with "sub_id" is caught by
// T8 below rather than accidentally agreeing because the two numbers happen to match.
constexpr uint16_t PLAYER       = 2;
constexpr int32_t  B_INDEX      = 7;
constexpr int32_t  STORAGE_SLOT = 3;

// Points buildings[PLAYER][B_INDEX].sub_id at STORAGE_SLOT -- every case needs this, so it is
// factored out; each case then seeds storage[PLAYER][STORAGE_SLOT] (docked_count/docked_units) and
// the individual units/cfg_units entries itself.
void seed_building(sim_fixture &fx) { fx.b(PLAYER, B_INDEX).sub_id = static_cast<uint8_t>(STORAGE_SLOT); }

unit_storage &hangar_storage(sim_fixture &fx) { return fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT]; }

int32_t call(sim_fixture &fx) {
    const sim_view v = fx.view();
    return detail::hangar_any_unit_needs_energy(v, PLAYER, B_INDEX);
}

} // namespace

void run_hangar_any_unit_needs_energy_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- empty hangar, docked_count == 0: returns 0 immediately, WITHOUT reading any docked unit.
    // Proven by seeding docked_units[0] with a unit that WOULD read as "needs energy" (energy 1.0 <
    // cap 100.0) if the loop body were ever entered -- if this fires, the observed result would be 1,
    // not 0, so a pass here is real evidence the read never happened, not just a lucky default.
    // =================================================================================================
    {
        fx.reset();
        seed_building(fx);
        unit_storage &s                = hangar_storage(fx);
        s.docked_count                 = 0;
        s.docked_units[0]              = 10;
        fx.u(PLAYER, 10).unit_proto_id = 1;
        fx.u(PLAYER, 10).energy        = 1.0;
        fx.cfg_units[1].energy         = 100.0;

        ck_eq((uint32_t)call(fx), 0u,
              "T1: docked_count==0 -> return 0 without reading docked_units[0], 0x0047b12c-0x0047b134");
    }

    // =================================================================================================
    // T2 -- empty hangar via a NEGATIVE docked_count sentinel (-1): the `i < docked_count` compare at
    // 0x0047b12c is SIGNED (JL), so -1 also short-circuits on the very first iteration (0 < -1 is
    // false) -- pins that a translation didn't compare docked_count as unsigned (which would turn -1
    // into a huge trip count and walk far past docked_units[50]). Same "seed a would-be-1 unit at
    // slot 0" proof as T1.
    // =================================================================================================
    {
        fx.reset();
        seed_building(fx);
        unit_storage &s                = hangar_storage(fx);
        s.docked_count                 = -1;
        s.docked_units[0]              = 10;
        fx.u(PLAYER, 10).unit_proto_id = 1;
        fx.u(PLAYER, 10).energy        = 1.0;
        fx.cfg_units[1].energy         = 100.0;

        ck_eq((uint32_t)call(fx), 0u,
              "T2: docked_count==-1 (negative sentinel) -> return 0, SIGNED compare, 0x0047b12c JL");
    }

    // =================================================================================================
    // T3 -- one docked unit, energy(5) < its type's cap(10), ordered: returns 1 (needs energy).
    // =================================================================================================
    {
        fx.reset();
        seed_building(fx);
        unit_storage &s                = hangar_storage(fx);
        s.docked_count                 = 1;
        s.docked_units[0]              = 10;
        fx.u(PLAYER, 10).unit_proto_id = 1;
        fx.u(PLAYER, 10).energy        = 5.0;
        fx.cfg_units[1].energy         = 10.0;

        ck_eq((uint32_t)call(fx), 1u,
              "T3: energy(5) < cap(10) -> needs energy, return 1, 0x0047b18f-0x0047b1a7");
    }

    // =================================================================================================
    // T4 -- one docked unit, energy == cap EXACTLY: the gate is `energy >= max` (JNC fires on ordered
    // equality too), so this counts as already-full -> returns 0. Pins against a translation that used
    // strict `>` for the "needs energy" test (which would wrongly return 1 here).
    // =================================================================================================
    {
        fx.reset();
        seed_building(fx);
        unit_storage &s                = hangar_storage(fx);
        s.docked_count                 = 1;
        s.docked_units[0]              = 10;
        fx.u(PLAYER, 10).unit_proto_id = 1;
        fx.u(PLAYER, 10).energy        = 10.0;
        fx.cfg_units[1].energy         = 10.0;

        ck_eq((uint32_t)call(fx), 0u,
              "T4: energy(10) == cap(10) exactly -> already full (>=), return 0, JNC boundary 0x0047b19e");
    }

    // =================================================================================================
    // T5 -- one docked unit, energy(12) > cap(10) (over-cap; the compare doesn't special-case this):
    // still counts as full -> returns 0.
    // =================================================================================================
    {
        fx.reset();
        seed_building(fx);
        unit_storage &s                = hangar_storage(fx);
        s.docked_count                 = 1;
        s.docked_units[0]              = 10;
        fx.u(PLAYER, 10).unit_proto_id = 1;
        fx.u(PLAYER, 10).energy        = 12.0;
        fx.cfg_units[1].energy         = 10.0;

        ck_eq((uint32_t)call(fx), 0u,
              "T5: energy(12) > cap(10), over-cap -- still >= max, full, return 0, 0x0047b19e");
    }

    // =================================================================================================
    // T6 -- THREE docked units, ALL full (energy >= their own cap): must check every one before
    // concluding 0 -- a mutant that stops after the first "full" verdict and returns 0 early would
    // still pass this one alone, but a mutant that (wrongly) returns 1 on ANY full unit would not, so
    // this pins "checking is exhaustive, not vacuously true".
    // =================================================================================================
    {
        fx.reset();
        seed_building(fx);
        unit_storage &s                = hangar_storage(fx);
        s.docked_count                 = 3;
        s.docked_units[0]              = 10;
        s.docked_units[1]              = 11;
        s.docked_units[2]              = 12;
        fx.u(PLAYER, 10).unit_proto_id = 1;
        fx.u(PLAYER, 10).energy        = 10.0;
        fx.cfg_units[1].energy         = 10.0; // exactly full
        fx.u(PLAYER, 11).unit_proto_id = 2;
        fx.u(PLAYER, 11).energy        = 25.0;
        fx.cfg_units[2].energy         = 20.0; // over-cap full
        fx.u(PLAYER, 12).unit_proto_id = 3;
        fx.u(PLAYER, 12).energy        = 7.0;
        fx.cfg_units[3].energy         = 7.0; // exactly full

        ck_eq((uint32_t)call(fx), 0u, "T6: 3 docked units, all full by their own cap -> return 0");
    }

    // =================================================================================================
    // T7 -- THREE docked units, only the LAST one needs energy (the first two are full): pins that the
    // scan doesn't stop early / doesn't only check slot 0 -- a translation that only inspected
    // docked_units[0] (or bailed after the first "full" verdict without continuing) would wrongly
    // return 0 here.
    // =================================================================================================
    {
        fx.reset();
        seed_building(fx);
        unit_storage &s                = hangar_storage(fx);
        s.docked_count                 = 3;
        s.docked_units[0]              = 10;
        s.docked_units[1]              = 11;
        s.docked_units[2]              = 12;
        fx.u(PLAYER, 10).unit_proto_id = 1;
        fx.u(PLAYER, 10).energy        = 10.0;
        fx.cfg_units[1].energy         = 10.0; // full
        fx.u(PLAYER, 11).unit_proto_id = 2;
        fx.u(PLAYER, 11).energy        = 25.0;
        fx.cfg_units[2].energy         = 20.0; // full (over-cap)
        fx.u(PLAYER, 12).unit_proto_id = 3;
        fx.u(PLAYER, 12).energy        = 3.0;
        fx.cfg_units[3].energy         = 7.0; // NEEDS energy

        ck_eq((uint32_t)call(fx), 1u,
              "T7: only docked_units[2] (the last) needs energy -> scan reaches it, return 1, "
              "0x0047b139 loop-back / 0x0047b1a0 found");
    }

    // =================================================================================================
    // T8 -- the storage slot comes from buildings[player][index].sub_id, NOT from `index` itself.
    // B_INDEX(7) != STORAGE_SLOT(3): storage slot B_INDEX is seeded to look EMPTY (docked_count==0),
    // while the REAL slot (sub_id==STORAGE_SLOT) has one unit that needs energy. A translation that
    // used `index` directly as the storage slot would read the empty slot at 7 and wrongly return 0;
    // the correct translation reads sub_id's slot (3) and returns 1.
    // =================================================================================================
    {
        fx.reset();
        seed_building(fx);
        // The WRONG slot (== index, B_INDEX==7): looks empty, so using it directly would return 0.
        unit_storage &wrong_slot = fx.storage[PLAYER * STORAGE_PER_PLAYER + B_INDEX];
        wrong_slot.docked_count  = 0;
        // The RIGHT slot (== sub_id, STORAGE_SLOT==3): one unit that needs energy.
        unit_storage &right_slot       = hangar_storage(fx);
        right_slot.docked_count        = 1;
        right_slot.docked_units[0]     = 10;
        fx.u(PLAYER, 10).unit_proto_id = 1;
        fx.u(PLAYER, 10).energy        = 2.0;
        fx.cfg_units[1].energy         = 10.0;

        ck_eq((uint32_t)call(fx), 1u,
              "T8: storage slot comes from buildings[].sub_id, not index directly -- return 1 "
              "(index's own slot is empty), 0x0047b0f8-0x0047b112 sub_id read");
    }

    // =================================================================================================
    // T9 -- the proto id (and therefore the cap) is read PER docked unit, not assumed uniform. Unit 0
    // is full under ITS OWN (small) cap; unit 1 needs energy under ITS OWN (larger, DIFFERENT) cap. A
    // translation that reused unit 0's proto/cap for unit 1 would wrongly judge unit 1 "full"
    // (15 >= 10) instead of correctly reading its own cap (15 < 50) and returning 1.
    // =================================================================================================
    {
        fx.reset();
        seed_building(fx);
        unit_storage &s                = hangar_storage(fx);
        s.docked_count                 = 2;
        s.docked_units[0]              = 10;
        s.docked_units[1]              = 11;
        fx.u(PLAYER, 10).unit_proto_id = 2;
        fx.u(PLAYER, 10).energy        = 10.0;
        fx.cfg_units[2].energy         = 10.0; // unit 0: full under proto 2's cap
        fx.u(PLAYER, 11).unit_proto_id = 6;
        fx.u(PLAYER, 11).energy        = 15.0;
        fx.cfg_units[6].energy         = 50.0; // unit 1: needs energy under proto 6's (DIFFERENT) cap

        ck_eq((uint32_t)call(fx), 1u,
              "T9: proto id / cap re-read per docked unit, not carried over from unit 0 -- return 1, "
              "0x0047b172 MOVZX unit_proto_id");
    }
}

} // namespace mh::sim::test
