//
// sim_hangar_recharge_pulse_selftest.cpp -- `simtest` cases for llm_strat_hangar_recharge_pulse
// (sim/sim_hangar_energy.h/.cpp @0x0047b1be), the hangar per-docked-unit recharge pulse.
//
// CONDUCTOR FIX (2026-08-22, registration pass): the original writer's `run_call()` called
// `reset_observations()` as its FIRST statement, which cleared `g_fail_units`/`g_reread_unit_idx`/
// `g_reread_to_proto` right after C2/C6/C8 had just set them and immediately before the call under
// test ran -- a TEST bug, not a production bug (confirmed: `hangar_recharge_pulse.cpp`'s own
// `pay_result != 0` / re-read logic matches the disassembly exactly). Fixed by moving
// `reset_observations()` out of `run_call()` and into each case, right after `fx.reset()` and before
// any scripted knob is set. Caught by the first `simtest` run (6 failures, all in this file); confirmed
// fixed by rerun (0 failures) and reconfirmed by the mutation-test pass below.
//
// EXPECTED BEHAVIOUR, HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_strat_hangar_recharge_pulse_0047b1be.asm), cross-checked against
// sim/sim_hangar_energy.h's own per-branch address citations -- NOT read off the .cpp body alone:
//
//   sub_id = buildings[player][index].sub_id (0x0047b1db-0x0047b1f2: two IMULs + ADD compute the
//   building's row address, then MOVZX byte ptr [.. +0xc3d366] reads sub_id -- the hangar's STORAGE
//   SLOT, NOT the building `index` argument itself); walk
//   storage_of(v, player, sub_id).docked_units[0..docked_count) (loop test 0x0047b20c-0x0047b217,
//   unit_idx fetch 0x0047b234-0x0047b23c). Per docked unit, in order:
//     1. GATE (0x0047b272 FLD unit.energy / 0x0047b278 FCOMP cfg.energy(max) / 0x0047b27e-0x0047b280
//        FNSTSW+SAHF / 0x0047b281 JNC): `energy >= max` (ordered) -> already full, skip entirely --
//        no pay call, no write, no damage-smoke call. (0x0047b255-0x0047b262 reads unit_proto_id and
//        derives the cfg row for THIS compare.)
//     2. Otherwise call unit_try_pay_action_cost(player, unit_idx) (0x0047b287-0x0047b28e). A
//        NONZERO result (0x0047b293 TEST/0x0047b295 JNZ) means payment failed -- skip, no write, no
//        damage-smoke call.
//     3. On a zero result: energy += cfg_units[proto].energy_2 (the RATE field, NOT `.energy` again
//        -- proto is RE-READ from the live unit record at 0x0047b29b-0x0047b2c2, AFTER the payment
//        call, not reused from step 1; 0x0047b2c8 FLD [energy_2] / 0x0047b2ce FADD unit.energy /
//        0x0047b2d4 FSTP unit.energy).
//     4. CLAMP (proto re-read a THIRD time, 0x0047b2da-0x0047b2f7; 0x0047b307 FLD energy(updated) /
//        0x0047b30d FCOMP cfg.energy(max) / 0x0047b313-0x0047b315 FNSTSW+SAHF / 0x0047b316 JBE):
//        fallthrough (JBE not taken) means `energy > max` ORDERED-STRICT -- set energy back down to
//        exactly the cap (proto re-read a FOURTH time, 0x0047b318-0x0047b343; 0x0047b345 FLD max /
//        0x0047b34b FSTP unit.energy). Landing exactly on the cap after the add does NOT clamp
//        (JBE fires, branch skipped) -- it is already correct.
//     5. Only on this success path (step 2's zero result): unit_update_damage_smoke(player,
//        unit_idx) (0x0047b351-0x0047b358). A unit that was already full (step 1) or whose payment
//        failed (step 2) never reaches this call.
//   Loop back-edge 0x0047b21c-0x0047b222; loop exit (docked_count<=0 or cursor exhausted)
//   0x0047b217/0x0047b362.
//
// sim/sim_hangar_energy.h's DECLARED NEED (this function reads no boot-constant doubles at all) --
// nothing to add to the fixture for this file; every field this oracle needs already exists on
// sim_fixture (units/buildings/storage/cfg_units).
//
#include "sim/sim_hangar_energy.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: proves call ORDER, ARGS, and per-unit skip/no-skip, in one structure -----------
struct TraceEntry {
    const char *tag; // "pay" or "dmg"
    uint32_t    player;
    int32_t     unit_idx;
};
std::vector<TraceEntry> g_trace;
void                    tr(const char *tag, uint32_t player, int32_t unit_idx) {
    g_trace.push_back({tag, player, unit_idx});
}

bool trace_eq(const std::vector<TraceEntry> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i) {
        if (std::strcmp(g_trace[i].tag, want[i].tag) != 0) return false;
        if (g_trace[i].player != want[i].player) return false;
        if (g_trace[i].unit_idx != want[i].unit_idx) return false;
    }
    return true;
}

// ---- fixture handle + scripted-callee knobs --------------------------------------------------------
sim_fixture *g_fx = nullptr;

// unit_try_pay_action_cost's scripted per-call outcome: any unit_idx listed here gets a nonzero
// (failed) result; every other unit_idx gets 0 (success). Real cost code is out of scope -- only the
// zero/nonzero split matters to this function.
std::vector<int32_t> g_fail_units;

// C6 (RE-DERIVE) only: when the pay call for THIS unit_idx fires, mutate the unit's live
// unit_proto_id to g_reread_to_proto before returning -- proves the add (step 3) and clamp (step 4)
// re-read unit_proto_id from the live record rather than reusing the gate's (step 1) read.
int32_t  g_reread_unit_idx = -1;
uint16_t g_reread_to_proto = 0;

std::vector<TraceEntry> g_pay_calls; // "pay" entries only, for count/arg asserts without re-filtering g_trace
std::vector<TraceEntry> g_dmg_calls; // "dmg" entries only

int32_t rec_unit_try_pay_action_cost(uint32_t player, int32_t unit_idx) {
    tr("pay", player, unit_idx);
    g_pay_calls.push_back({"pay", player, unit_idx});
    if (g_reread_unit_idx == unit_idx && g_fx != nullptr) {
        g_fx->u(static_cast<int32_t>(player), unit_idx).unit_proto_id = g_reread_to_proto;
    }
    for (int32_t f : g_fail_units)
        if (f == unit_idx) return 7; // any nonzero -- the original only branches on TEST/JNZ, no code
    return 0;
}

void rec_unit_update_damage_smoke(uint32_t player, int32_t unit_idx) {
    tr("dmg", player, unit_idx);
    g_dmg_calls.push_back({"dmg", player, unit_idx});
}

const hangar_recharge_pulse_calls g_calls = {
    &rec_unit_try_pay_action_cost,
    &rec_unit_update_damage_smoke,
};

void reset_observations() {
    g_trace.clear();
    g_pay_calls.clear();
    g_dmg_calls.clear();
    g_fail_units.clear();
    g_reread_unit_idx = -1;
    g_reread_to_proto = 0;
}

// ---- fixture seeding ---------------------------------------------------------------------------------
// One docked unit's full seed: which unit index it is, which cfg proto row it points at, that proto's
// cap (.energy) / rate (.energy_2), and the unit's starting .energy.
struct DockSeed {
    int32_t  unit_idx;
    uint16_t proto;
    double   cap;
    double   rate;
    double   energy_in;
};

// Docks `seeds` (in order) into buildings[player][bldg_index]'s hangar via storage slot `sub_id`
// (deliberately != bldg_index in every case below, per the header's sub_id-not-index rule) and seeds
// each unit's proto row. Caller must fx.reset() first for a clean slate.
void dock(sim_fixture &fx, uint16_t player, int32_t bldg_index, uint8_t sub_id,
          const std::vector<DockSeed> &seeds) {
    fx.b(player, bldg_index).sub_id = sub_id;

    unit_storage &s = fx.storage[(size_t)player * STORAGE_PER_PLAYER + sub_id];
    s.docked_count  = (int32_t)seeds.size();
    for (size_t i = 0; i < seeds.size(); ++i) {
        s.docked_units[i] = seeds[i].unit_idx;

        unit &u         = fx.u(player, seeds[i].unit_idx);
        u.unit_proto_id = seeds[i].proto;
        u.energy        = seeds[i].energy_in;

        fx.cfg_units[seeds[i].proto].energy   = seeds[i].cap;
        fx.cfg_units[seeds[i].proto].energy_2 = seeds[i].rate;
    }
}

// NOTE: this does NOT call reset_observations() -- callers set scripted knobs (g_fail_units,
// g_reread_unit_idx/_to_proto) AFTER their own reset_observations() call and BEFORE calling run_call();
// clearing those knobs here would silently discard them before the call under test ever runs (exactly
// the bug this comment replaces -- see the fix note above run_hangar_recharge_pulse_tests()).
void run_call(sim_fixture &fx, uint16_t player, int32_t bldg_index) {
    g_fx          = &fx;
    sim_view  v   = fx.view();
    sim_store own = fx.store();
    detail::hangar_recharge_pulse(v, own, player, bldg_index, g_calls);
}

} // namespace

void run_hangar_recharge_pulse_tests() {
    sim_fixture fx;

    // =================================================================================================
    // C1 -- GATE, exact boundary (energy == cap): counts as full -> skip entirely. No pay call, no
    // damage-smoke call, no write (energy left at the cap, unchanged).
    // 0x0047b272 FLD unit.energy / 0x0047b278 FCOMP cfg.energy / 0x0047b281 JNC (fires on `energy>=max`
    // ordered, including equality -- see the header's FP-compare note).
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        dock(fx, 0, 2, 9, {{50, /*proto*/ 3, /*cap*/ 100.0, /*rate*/ 15.0, /*energy_in*/ 100.0}});
        run_call(fx, 0, 2);

        ck(trace_eq({}), "C1: energy==cap -- already full, no pay/dmg call at all (0x0047b281 JNC)");
        ck_eq_d(fx.u(0, 50).energy, 100.0, "C1: energy left exactly at the cap, no write attempted");
    }

    // =================================================================================================
    // C2 -- GATE just BELOW the cap (proceeds, pins `>=` is not `>`) + PAYMENT FAILS: pay call fires
    // with the right args, nonzero result -> skip, no energy write, no damage-smoke call.
    // Gate not taken -> pay call at 0x0047b287-0x0047b28e; TEST/JNZ @0x0047b293-0x0047b295 (nonzero
    // => skip, no FADD/FSTP, no 0x0047b358 call).
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        dock(fx, 0, 2, 9, {{51, /*proto*/ 3, /*cap*/ 100.0, /*rate*/ 15.0, /*energy_in*/ 99.0}});
        g_fail_units = {51};
        run_call(fx, 0, 2);

        ck(trace_eq({{"pay", 0, 51}}),
           "C2: 99.0 < cap(100.0) -- gate NOT taken, pay call fires (0x0047b28e), nonzero result skips "
           "the rest (0x0047b295 JNZ) -- no dmg call");
        ck(g_pay_calls.size() == 1 && g_pay_calls[0].player == 0 && g_pay_calls[0].unit_idx == 51,
           "C2: unit_try_pay_action_cost(player=0, unit_idx=51) -- exact args (0x0047b287/0x0047b28a)");
        ck_eq_d(fx.u(0, 51).energy, 99.0, "C2: payment failed -- energy left untouched at 99.0, NOT 114.0 "
                                          "(99.0+rate), pinning 'no partial charge on failure'");
    }

    // =================================================================================================
    // C3 -- PAYMENT SUCCEEDS, result stays strictly BELOW the cap: no clamp. Pins the add uses
    // energy_2 (the RATE), not energy (the cap) again -- 50.0+15.0=65.0; a translation that mistakenly
    // added the cap would read 150.0 (then clamped to 100.0), a value this case distinguishes from.
    // damage-smoke fires ONLY on this success path, AFTER the pay call.
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        dock(fx, 0, 2, 9, {{52, /*proto*/ 3, /*cap*/ 100.0, /*rate*/ 15.0, /*energy_in*/ 50.0}});
        run_call(fx, 0, 2);

        ck(trace_eq({{"pay", 0, 52}, {"dmg", 0, 52}}),
           "C3: pay (0x0047b28e) then dmg (0x0047b358), in that order, both firing on the success path");
        ck_eq_d(fx.u(0, 52).energy, 65.0,
                "C3: energy = 50.0 + energy_2(15.0) = 65.0 (0x0047b2c8 FLD energy_2 / 0x0047b2ce FADD) -- "
                "NOT 150.0, which is what adding .energy (the cap) again would produce");
    }

    // =================================================================================================
    // C4 -- PAYMENT SUCCEEDS, result OVERFLOWS past the cap: clamp fires, energy set back down to
    // EXACTLY the cap (not left at the raw overflowed sum).
    // 95.0+15.0=110.0 > 100.0 -- 0x0047b316 JBE NOT taken (ordered-strict `>`) -> clamp store
    // (0x0047b345/0x0047b34b).
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        dock(fx, 0, 2, 9, {{53, /*proto*/ 3, /*cap*/ 100.0, /*rate*/ 15.0, /*energy_in*/ 95.0}});
        run_call(fx, 0, 2);

        ck(trace_eq({{"pay", 0, 53}, {"dmg", 0, 53}}), "C4: pay then dmg, success path");
        ck_eq_d(fx.u(0, 53).energy, 100.0,
                "C4: 95.0+15.0=110.0 overflows the cap(100.0) -- clamped back down to exactly 100.0 "
                "(0x0047b34b FSTP unit.energy), NOT left at the raw 110.0");
    }

    // =================================================================================================
    // C5 -- PAYMENT SUCCEEDS, result lands EXACTLY on the cap after the add: the clamp compare's JBE
    // fires (energy<=max ordered) so the clamp store is SKIPPED as a no-op -- already correct.
    // Observably indistinguishable from "clamp fired and produced the same value" (both leave 100.0),
    // but a mutant that clamps to the WRONG field/value, or a mutant that adds the wrong field so the
    // sum ISN'T exactly the cap, both disagree with this case.
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        dock(fx, 0, 2, 9, {{54, /*proto*/ 3, /*cap*/ 100.0, /*rate*/ 15.0, /*energy_in*/ 85.0}});
        run_call(fx, 0, 2);

        ck(trace_eq({{"pay", 0, 54}, {"dmg", 0, 54}}), "C5: pay then dmg, success path");
        ck_eq_d(fx.u(0, 54).energy, 100.0,
                "C5: 85.0+15.0=100.0 lands EXACTLY on the cap -- 0x0047b316 JBE fires (energy<=max), "
                "clamp store skipped as a no-op; final value already correct at 100.0");
    }

    // =================================================================================================
    // C6 -- RE-DERIVE: the pay-cost mock mutates the docked unit's unit_proto_id MID-CALL, from protoA
    // (cap=100.0/rate=15.0) to protoB (cap=200.0/rate=999.0). The gate (step 1) necessarily saw protoA
    // (the mutation only happens inside the pay call, which runs AFTER the gate). If the add/clamp
    // (steps 3-4) genuinely re-read unit_proto_id from the live record (as the header's per-address
    // citation claims: 0x0047b29b-0x0047b2c2 recomputes address+proto AFTER the payment call, and
    // 0x0047b2da-0x0047b2f7 does it again before the clamp compare) rather than reusing the gate's
    // read, the result reflects protoB: 50.0+999.0=1049.0, clamped to protoB's cap 200.0. A
    // (bug-for-bug WRONG) translation that cached cu from the gate would instead compute
    // 50.0+15.0=65.0 (no clamp, 65.0<100.0) -- a hugely different, easily distinguished number. This
    // DOES independently exercise the no-caching claim, contra the brief's fallback note that it might
    // not be constructible: the mock has a live fixture handle and can mutate the SAME unit record the
    // real code re-reads from, because v/own both alias fx's own backing vectors rather than copies.
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        dock(fx, 0, 6, 13,
             {{40, /*protoA*/ 2, /*cap*/ 100.0, /*rate*/ 15.0, /*energy_in*/ 50.0}});
        fx.cfg_units[7].energy   = 200.0; // protoB cap
        fx.cfg_units[7].energy_2 = 999.0; // protoB rate
        g_reread_unit_idx        = 40;
        g_reread_to_proto        = 7;
        run_call(fx, 0, 6);

        ck(trace_eq({{"pay", 0, 40}, {"dmg", 0, 40}}), "C6: pay then dmg, success path (mutation doesn't "
                                                       "change the pay result, only the unit's proto)");
        ck_eq((uint32_t)fx.u(0, 40).unit_proto_id, 7u,
              "C6: unit_proto_id really is mutated to protoB by the pay mock");
        ck_eq_d(fx.u(0, 40).energy, 200.0,
                "C6: add used protoB's energy_2(999.0) (50.0+999.0=1049.0), then clamped to protoB's "
                "cap(200.0) -- proves steps 3-4 re-read unit_proto_id AFTER the pay call rather than "
                "reusing the gate's protoA read (which would have produced 65.0, unclamped)");
    }

    // =================================================================================================
    // C7 -- buildings[player][index].sub_id (NOT `index` itself) selects the storage slot. Seeds a
    // POISON docked unit in storage slot `index` (the wrong slot, reachable only by a translation that
    // mistakenly used `index` as the storage slot) and the REAL docked unit in storage slot `sub_id`
    // (the correct one). Only the real unit may be touched.
    // 0x0047b1e9-0x0047b1f2: sub_id is read from buildings[player][index].sub_id (MOVZX byte ptr
    // [.. +0xc3d366]) and used as the storage-row multiplier at 0x0047b1fc (IMUL [EBP-0x14],0xf4) --
    // `index` itself never feeds the storage address after that point.
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        const uint8_t bldg_index = 2;
        const uint8_t sub_id     = 9; // != bldg_index, the point of this case
        // Poison: a docked unit seeded directly into storage slot `bldg_index` (the wrong slot), not
        // via dock() (which would also set buildings[0][2].sub_id -- we want that to stay `sub_id`).
        unit_storage &poison_slot   = fx.storage[(size_t)0 * STORAGE_PER_PLAYER + bldg_index];
        poison_slot.docked_count    = 1;
        poison_slot.docked_units[0] = 77;
        fx.u(0, 77).unit_proto_id   = 5;
        fx.u(0, 77).energy          = 0.0;
        fx.cfg_units[5].energy      = 1.0; // any cap > 0.0 so the poison unit's gate would NOT skip it
        fx.cfg_units[5].energy_2    = 1.0;

        dock(fx, 0, bldg_index, sub_id, {{30, /*proto*/ 3, /*cap*/ 100.0, /*rate*/ 15.0, /*energy_in*/ 50.0}});
        run_call(fx, 0, bldg_index);

        ck(trace_eq({{"pay", 0, 30}, {"dmg", 0, 30}}),
           "C7: only unit 30 (docked via sub_id=9) is processed -- unit 77 (docked via index=2, the "
           "wrong slot) never appears in the trace");
        ck_eq_d(fx.u(0, 30).energy, 65.0, "C7: the real docked unit's recharge applied normally (50.0+15.0)");
        ck_eq_d(fx.u(0, 77).energy, 0.0,
                "C7: the poison unit (parked in storage[index] instead of storage[sub_id]) is untouched "
                "-- energy still 0.0, proving sub_id (not index) drove the storage lookup");
    }

    // =================================================================================================
    // C8 -- MULTIPLE docked units in ONE call, processed independently in slot order: unit A already
    // full (skip), unit B payment fails (skip), unit C succeeds and clamps, unit D succeeds without
    // clamping. Asserts the EXACT call sequence/args (order matters -- an off-by-one in the
    // docked_units walk would shift or drop an entry) and each unit's final energy independently.
    // =================================================================================================
    {
        fx.reset();
        reset_observations();
        dock(fx, 0, 4, 11,
             {
                 {60, /*protoA*/ 10, /*cap*/ 50.0, /*rate*/ 999.0, /*energy_in*/ 50.0}, // A: already full (==cap)
                 {61, /*protoB*/ 11, /*cap*/ 60.0, /*rate*/ 5.0, /*energy_in*/ 10.0},   // B: pays, fails
                 {62, /*protoC*/ 12, /*cap*/ 100.0, /*rate*/ 15.0, /*energy_in*/ 95.0}, // C: pays, succeeds, clamps
                 {63, /*protoD*/ 13, /*cap*/ 80.0, /*rate*/ 10.0, /*energy_in*/ 40.0},  // D: pays, succeeds, no clamp
             });
        g_fail_units = {61};
        run_call(fx, 0, 4);

        ck(trace_eq({
               {"pay", 0, 61}, // A produced no call at all -- skipped straight past by the gate
               {"pay", 0, 62},
               {"dmg", 0, 62},
               {"pay", 0, 63},
               {"dmg", 0, 63},
           }),
           "C8: exact ordered sequence across 4 docked units, slot order 0..3 -- A silent (gate skip), "
           "B pay-only (payment fails), C pay+dmg, D pay+dmg; a shifted/dropped/reordered entry here "
           "means the docked_units walk (0x0047b1fc loop) has an off-by-one");

        ck_eq_d(fx.u(0, 60).energy, 50.0, "C8 A: already-full unit untouched (still exactly its cap)");
        ck_eq_d(fx.u(0, 61).energy, 10.0, "C8 B: payment failed -- energy untouched at 10.0");
        ck_eq_d(fx.u(0, 62).energy, 100.0, "C8 C: 95.0+15.0=110.0 clamped down to cap 100.0");
        ck_eq_d(fx.u(0, 63).energy, 50.0, "C8 D: 40.0+10.0=50.0, strictly below cap(80.0) -- no clamp");
    }
}

} // namespace mh::sim::test
