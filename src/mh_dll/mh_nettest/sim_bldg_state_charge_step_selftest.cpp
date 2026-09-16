//
// sim_bldg_state_charge_step_selftest.cpp -- `simtest` cases for llm_strat_bldg_state_charge_step
// (sim/sim_bldg_state_charge.h/.cpp, detail::bldg_state_charge_step), SIM1B building_tick machinery.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_charge_step_00472979.asm), cross-checked against the .cpp/.h's
// own per-line address citations -- NOT read off the .cpp body alone:
//
//   0x00472996-0x004729d7+JMP: `if (cur_building->efficiency <= 0.0) { tick_budget = 0.0; return; }` --
//   NO calls, no other state touched (the FLDZ/FCOMP/JNC test at 0x00472996-0x0047299e: CF=1 iff
//   0.0 < efficiency, JNC taken -- i.e. this branch runs -- iff efficiency <= 0.0).
//
//   0x004729a0-0x004729d2: `step = (Building[bid].build_time_d / Building[bid].energy_d) /
//   cur_building->efficiency`.
//
//   0x004729f0-0x00472a08 (falls through to 0x00472af7 on either half) / 0x00472af7-0x00472b12: `if
//   (tick_budget < step || step <= 0.0) { cur_building->last_tick_time -= tick_budget; tick_budget =
//   0.0; return; }` -- an OR of two INDEPENDENT FCOMP/JC tests (0x004729f0 tick_budget-vs-step,
//   0x004729fe 0.0-vs-step), both routed to the SAME 0x00472af7 tail -- NO calls, no other state
//   touched, only `last_tick_time -= tick_budget` (FSUBR at 0x00472b02: ST0=tick_budget,
//   mem=last_tick_time -> mem = mem - ST0) then tick_budget zeroed.
//
//   0x00472a0d-0x00472af5 (the increment path): `cycle_progress += step; tick_budget -= step; energy
//   += 1.0;` then TWO INDEPENDENT (not else-if) CMP/state-store pairs, both evaluated every time and
//   both able to fire on the SAME call:
//     0x00472a37-0x00472a81: `if (Building[bid].energy <= energy) { energy = Building[bid].energy;
//     state = 0x69; }`
//     0x00472a81-0x00472acf: `if (Building[bid].build_time_d <= cycle_progress) { cycle_progress -=
//     Building[bid].build_time_d; state = 0x69; }`
//   then, ONLY on this path: `llm_strat_bldg_update_charge_pips(cur_player, cur_index)` (0x00472acf-
//   0x00472ae2, EDX=cur_index/EAX=cur_player) then `llm_strat_bldg_notify_ui(cur_player, cur_index)`
//   (0x00472ae2-0x00472af0, same register pair) -- both early-return paths above jump straight past
//   these two calls to the shared epilogue (0x00472b1c).
//
// NOTE ON THE .cpp's NaN-safety rewrite: the .cpp restates the OR as `!(tick_budget >= step) ||
// !(step > 0.0)`, documented (2026-08-22 reimpl-verify fix) as preserving the x87
// unordered-comparison-is-"less than" quirk for NaN inputs. For every ORDERARY (non-NaN) input this
// is identical to the naive `tick_budget < step || step <= 0.0` reading of the asm -- every case
// below uses ordinary finite doubles, so it does not need to (and does not) exercise that NaN path;
// it is called out here only so a reader does not mistake the negated form for a divergence.
//
// SCOPE: covers the two early-return families (efficiency<=0; tick_budget<step OR step<=0, each half
// independently), the exact step arithmetic (via the increment path's observable deltas, all
// exact-integer operands), the two independent threshold overrides firing alone and together (the
// else-if-bug catcher), and the two callees firing exactly once each, ONLY on the increment path, in
// their asm call order (update_charge_pips before notify_ui).
//
#include "sim/sim_bldg_state_charge.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// llm_strat_bldg_state's CHARGE_GATE value (0x69) -- no generated C++ enum exists yet (see the .h's
// DECLARED NEED note); redeclared file-local per this project's established per-TU precedent (the
// same value is already independently redeclared this way in sim_bldg_add_workers.cpp,
// sim_bldg_finish_order.cpp, sim_bldg_remove_workers.cpp, sim_refresh_building.cpp,
// sim_order_dispatch_bldg.cpp, and sim_bldg_state_charge.cpp itself).
constexpr uint16_t CHARGE_STEP_BLDG_STATE_CHARGE_GATE = 0x69;

// Sentinel state distinct from CHARGE_GATE(0x69) and from any other named bldg state this file
// touches, so "state unchanged" and "state -> CHARGE_GATE" are never accidentally the same value.
constexpr uint16_t SENTINEL_STATE = 0xBEEF;

// ---- shared trace: proves call ORDER between the two callees -----------------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- the two callees, recorded ------------------------------------------------------------------
struct PipsCall {
    uint16_t player;
    uint32_t building_id; // the struct field's name -- the VALUE passed is actually cur_index, see below
};
std::vector<PipsCall> g_pips_calls;
void                  rec_update_charge_pips(uint16_t player, uint32_t building_id) {
    tr("update_charge_pips");
    g_pips_calls.push_back({player, building_id});
}

struct NotifyCall {
    uint16_t player;
    uint32_t index;
};
std::vector<NotifyCall> g_notify_calls;
void                    rec_notify_ui(uint16_t player, uint32_t index) {
    tr("notify_ui");
    g_notify_calls.push_back({player, index});
}

const bldg_state_charge_step_calls g_calls = {
    &rec_update_charge_pips,
    &rec_notify_ui,
};

void reset_observations() {
    g_trace.clear();
    g_pips_calls.clear();
    g_notify_calls.clear();
}

// ---- fixture seeding ------------------------------------------------------------------------------
struct Seed {
    uint16_t player  = 0;
    int32_t  index   = 1;
    uint16_t cfg_row = 10;

    double efficiency = 2.0;

    // Building[bid]'s three config fields this function reads (all zeroed elsewhere by fx.reset()).
    double build_time_d   = 100.0;
    double energy_d       = 5.0;
    double cfg_energy_cap = 1000.0; // the completion/max threshold for cur_building->energy

    double tick_budget = 1000.0;

    // cur_building's own state, seeded with distinct, non-symmetric, nonzero sentinels so an
    // "unchanged" assertion actually proves something.
    double   last_tick_time = 999.25;
    double   cycle_progress = 5.0;
    double   energy         = 0.0;
    uint16_t state          = SENTINEL_STATE;
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    building &b      = fx.b(s.player, s.index);
    b.building_id    = s.cfg_row;
    b.efficiency     = s.efficiency;
    b.last_tick_time = s.last_tick_time;
    b.cycle_progress = s.cycle_progress;
    b.energy         = s.energy;
    b.state          = s.state;

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = s.player;
    fx.view_cur_index   = (uint16_t)s.index;

    cfg_building &cb = fx.cfg_buildings[s.cfg_row];
    cb.build_time_d  = s.build_time_d;
    cb.energy_d      = s.energy_d;
    cb.energy        = s.cfg_energy_cap;

    fx.tick_budget = s.tick_budget;

    reset_observations();

    sim_store own = fx.store();
    detail::bldg_state_charge_step(fx.view(), own, g_calls);
}

} // namespace

void run_bldg_state_charge_step_tests() {
    sim_fixture fx;

    // =================================================================================================
    // E1/E2 -- efficiency<=0.0 short-circuit (0x00472996-0x004729e1+JMP): NO calls at all; ONLY
    // tick_budget zeroes -- last_tick_time/cycle_progress/energy/state all UNCHANGED. Tested at both
    // efficiency==0.0 (the exact boundary the FCOMP/JNC test hits) and a negative value.
    // =================================================================================================
    {
        Seed s;
        s.player      = 0;
        s.index       = 1;
        s.efficiency  = 0.0;
        s.tick_budget = 42.0;
        seed_and_run(fx, s);

        ck(g_pips_calls.empty() && g_notify_calls.empty() && g_trace.empty(),
           "E1: efficiency==0.0 (@0x0047299e JNC) -- no calls at all");
        ck_eq_d(fx.tick_budget, 0.0, "E1: tick_budget zeroed (@0x004729d7)");
        ck_eq_d(fx.b(0, 1).last_tick_time, s.last_tick_time, "E1: last_tick_time UNCHANGED");
        ck_eq_d(fx.b(0, 1).cycle_progress, s.cycle_progress, "E1: cycle_progress UNCHANGED");
        ck_eq_d(fx.b(0, 1).energy, s.energy, "E1: energy UNCHANGED");
        ck_eq((uint32_t)fx.b(0, 1).state, (uint32_t)SENTINEL_STATE, "E1: state UNCHANGED");
    }
    {
        Seed s;
        s.player      = 0;
        s.index       = 1;
        s.efficiency  = -3.5;
        s.tick_budget = 17.0;
        seed_and_run(fx, s);

        ck(g_pips_calls.empty() && g_notify_calls.empty() && g_trace.empty(),
           "E2: efficiency==-3.5 (negative, same JNC branch) -- no calls at all");
        ck_eq_d(fx.tick_budget, 0.0, "E2: tick_budget zeroed");
        ck_eq_d(fx.b(0, 1).last_tick_time, s.last_tick_time, "E2: last_tick_time UNCHANGED");
        ck_eq_d(fx.b(0, 1).cycle_progress, s.cycle_progress, "E2: cycle_progress UNCHANGED");
        ck_eq_d(fx.b(0, 1).energy, s.energy, "E2: energy UNCHANGED");
        ck_eq((uint32_t)fx.b(0, 1).state, (uint32_t)SENTINEL_STATE, "E2: state UNCHANGED");
    }

    // =================================================================================================
    // E3 -- the "tick_budget < step" half of the OR (0x004729f0-0x004729fc JC), with an otherwise
    // healthy positive step. Proves this half independently of the "step<=0.0" half (E4 below).
    // last_tick_time -= tick_budget is checked as a real SUBTRACTION (sentinel minus a nonzero
    // tick_budget), not merely "changed".
    // =================================================================================================
    {
        Seed s;
        s.player         = 0;
        s.index          = 1;
        s.efficiency     = 2.0;
        s.build_time_d   = 100.0;
        s.energy_d       = 5.0; // step = (100/5)/2 = 10.0
        s.tick_budget    = 4.0; // < step(10.0) -> this half of the OR fires
        s.last_tick_time = 999.25;
        s.cycle_progress = 7.0;
        s.energy         = 3.0;
        s.state          = SENTINEL_STATE;
        seed_and_run(fx, s);

        ck(g_pips_calls.empty() && g_notify_calls.empty() && g_trace.empty(),
           "E3: tick_budget(4) < step(10) (@0x004729fc JC) -- no calls");
        ck_eq_d(fx.b(0, 1).last_tick_time, s.last_tick_time - s.tick_budget,
                "E3: last_tick_time -= tick_budget (999.25 - 4.0 = 995.25, @0x00472b02 FSUBR)");
        ck_eq_d(fx.tick_budget, 0.0, "E3: tick_budget zeroed (@0x00472b08)");
        ck_eq_d(fx.b(0, 1).cycle_progress, s.cycle_progress, "E3: cycle_progress UNCHANGED");
        ck_eq_d(fx.b(0, 1).energy, s.energy, "E3: energy UNCHANGED");
        ck_eq((uint32_t)fx.b(0, 1).state, (uint32_t)SENTINEL_STATE, "E3: state UNCHANGED");
    }

    // =================================================================================================
    // E4 -- the "step<=0.0" half of the OR (0x004729fe-0x00472a06 JC on 0.0-vs-step), reached via a
    // degenerate cfg (build_time_d==0.0 -> step==0.0) even though tick_budget is otherwise PLENTY --
    // proves this half fires independently of the tick_budget-vs-step half (E3 above), i.e. it is a
    // real OR and not e.g. only the first comparison gating the whole branch.
    // =================================================================================================
    {
        Seed s;
        s.player         = 0;
        s.index          = 1;
        s.efficiency     = 2.0;
        s.build_time_d   = 0.0; // -> step = (0/5)/2 = 0.0, degenerate
        s.energy_d       = 5.0;
        s.tick_budget    = 1000.0; // plenty -- NOT the tick_budget<step half
        s.last_tick_time = 12.5;
        s.cycle_progress = 9.0;
        s.energy         = 6.0;
        s.state          = SENTINEL_STATE;
        seed_and_run(fx, s);

        ck(g_pips_calls.empty() && g_notify_calls.empty() && g_trace.empty(),
           "E4: step<=0.0 via build_time_d==0.0 (@0x00472a06 JC), tick_budget otherwise plenty -- no calls");
        ck_eq_d(fx.b(0, 1).last_tick_time, s.last_tick_time - s.tick_budget,
                "E4: last_tick_time -= tick_budget (12.5 - 1000.0 = -987.5)");
        ck_eq_d(fx.tick_budget, 0.0, "E4: tick_budget zeroed");
        ck_eq_d(fx.b(0, 1).cycle_progress, s.cycle_progress, "E4: cycle_progress UNCHANGED");
        ck_eq_d(fx.b(0, 1).energy, s.energy, "E4: energy UNCHANGED");
        ck_eq((uint32_t)fx.b(0, 1).state, (uint32_t)SENTINEL_STATE, "E4: state UNCHANGED");
    }

    // =================================================================================================
    // E5 -- the increment path (0x00472a0d-0x00472af5) with NEITHER threshold firing: state stays
    // unchanged, both calls fire exactly once, in asm call order (update_charge_pips before
    // notify_ui, both @cur_player/cur_index). tick_budget/cycle_progress deltas pin the EXACT step
    // value (100.0) rather than merely "changed" -- tick_budget goes from 137.0 to 37.0 (not to 0),
    // which distinguishes a real subtraction from the early-return paths' zero-out.
    // =================================================================================================
    {
        Seed s;
        s.player         = 1;
        s.index          = 2;
        s.cfg_row        = 20;
        s.efficiency     = 1.0;
        s.build_time_d   = 1000.0;
        s.energy_d       = 10.0; // step = (1000/10)/1 = 100.0
        s.cfg_energy_cap = 1000.0;
        s.tick_budget    = 137.0; // >= step(100) -> increment path; remainder 37.0 pins the subtraction
        s.last_tick_time = 55.0;
        s.cycle_progress = 5.0; // + step(100) = 105.0, well under build_time_d(1000) -- no cycle fire
        s.energy         = 0.0; // + 1.0 = 1.0, well under cfg_energy_cap(1000) -- no energy fire
        s.state          = SENTINEL_STATE;
        seed_and_run(fx, s);

        ck(trace_eq({"update_charge_pips", "notify_ui"}),
           "E5: exact call order -- update_charge_pips then notify_ui (@0x00472add/0x00472af0)");
        ck(g_pips_calls.size() == 1 && g_pips_calls[0].player == 1 && g_pips_calls[0].building_id == 2u,
           "E5: update_charge_pips(cur_player=1, cur_index=2) -- fires exactly once");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].player == 1 && g_notify_calls[0].index == 2u,
           "E5: notify_ui(cur_player=1, cur_index=2) -- fires exactly once");

        ck_eq_d(fx.b(1, 2).cycle_progress, 105.0, "E5: cycle_progress += step (5.0+100.0=105.0, @0x00472a18)");
        ck_eq_d(fx.tick_budget, 37.0, "E5: tick_budget -= step (137.0-100.0=37.0, @0x00472a24)");
        ck_eq_d(fx.b(1, 2).energy, 1.0, "E5: energy += 1.0 (0.0+1.0=1.0, @0x00472a34)");
        ck_eq((uint32_t)fx.b(1, 2).state, (uint32_t)SENTINEL_STATE,
              "E5: state UNCHANGED -- neither threshold fires");
        ck_eq_d(fx.b(1, 2).last_tick_time, s.last_tick_time, "E5: last_tick_time UNCHANGED on the increment path");
    }

    // =================================================================================================
    // E6 -- energy threshold fires ALONE (0x00472a37-0x00472a81): energy clamped to EXACTLY
    // Building[bid].energy (not the pre-clamp incremented value), state -> CHARGE_GATE(0x69); the
    // cycle_progress threshold must NOT fire (cycle_progress stays at its post-increment,
    // non-decremented value) -- proving the two branches are independent, not else-if collapsed.
    // =================================================================================================
    {
        Seed s;
        s.player         = 0;
        s.index          = 1;
        s.cfg_row        = 21;
        s.efficiency     = 1.0;
        s.build_time_d   = 1000.0; // large -- cycle threshold (100.0 post-increment) never reached
        s.energy_d       = 10.0;   // step = (1000/10)/1 = 100.0
        s.cfg_energy_cap = 0.5;    // small -- energy(0.0+1.0=1.0) >= 0.5, fires
        s.tick_budget    = 100.0;  // == step -> increment path (tick_budget>=step)
        s.cycle_progress = 0.0;    // + step(100.0) = 100.0, well under build_time_d(1000) -- no cycle fire
        s.energy         = 0.0;    // + 1.0 = 1.0 -> clamped to cap(0.5)
        s.state          = SENTINEL_STATE;
        seed_and_run(fx, s);

        ck(trace_eq({"update_charge_pips", "notify_ui"}), "E6: both calls still fire on this path");
        ck_eq_d(fx.b(0, 1).energy, 0.5,
                "E6: energy clamped to EXACTLY Building[bid].energy=0.5, not the pre-clamp 1.0 (@0x00472a73)");
        ck_eq_d(fx.b(0, 1).cycle_progress, 100.0,
                "E6: cycle_progress NOT decremented (stays at post-increment 100.0) -- cycle threshold did NOT fire");
        ck_eq((uint32_t)fx.b(0, 1).state, (uint32_t)CHARGE_STEP_BLDG_STATE_CHARGE_GATE,
              "E6: state -> CHARGE_GATE(0x69) via the energy branch (@0x00472a7b)");
    }

    // =================================================================================================
    // E7 -- cycle_progress threshold fires ALONE (0x00472a81-0x00472acf): cycle_progress -=
    // Building[bid].build_time_d EXACTLY, state -> CHARGE_GATE(0x69); the energy threshold must NOT
    // fire (energy stays at its post-increment, non-clamped value).
    // =================================================================================================
    {
        Seed s;
        s.player         = 0;
        s.index          = 1;
        s.cfg_row        = 22;
        s.efficiency     = 2.0;
        s.build_time_d   = 40.0;
        s.energy_d       = 4.0;    // step = (40/4)/2 = 5.0
        s.cfg_energy_cap = 1000.0; // large -- energy(0.0+1.0=1.0) well under cap -- no energy fire
        s.tick_budget    = 5.0;    // == step -> increment path
        s.cycle_progress = 38.0;   // + step(5.0) = 43.0 >= build_time_d(40.0) -- fires
        s.energy         = 0.0;    // + 1.0 = 1.0, no clamp
        s.state          = SENTINEL_STATE;
        seed_and_run(fx, s);

        ck(trace_eq({"update_charge_pips", "notify_ui"}), "E7: both calls still fire on this path");
        ck_eq_d(fx.b(0, 1).cycle_progress, 3.0,
                "E7: cycle_progress -= build_time_d EXACTLY (43.0-40.0=3.0, @0x00472ac1)");
        ck_eq_d(fx.b(0, 1).energy, 1.0, "E7: energy NOT clamped -- stays at post-increment 1.0, not cfg cap 1000.0");
        ck_eq((uint32_t)fx.b(0, 1).state, (uint32_t)CHARGE_STEP_BLDG_STATE_CHARGE_GATE,
              "E7: state -> CHARGE_GATE(0x69) via the cycle_progress branch (@0x00472ac9)");
    }

    // =================================================================================================
    // E8 -- BOTH thresholds fire on the SAME call: the else-if-bug catcher. If the two branches were
    // collapsed into `else if`, only the energy branch (checked first in program order) would fire and
    // cycle_progress would be left at its un-decremented post-increment value (25.0, not 5.0) --
    // this case fails loudly under that mutation.
    // =================================================================================================
    {
        Seed s;
        s.player         = 0;
        s.index          = 1;
        s.cfg_row        = 23;
        s.efficiency     = 1.0;
        s.build_time_d   = 20.0;
        s.energy_d       = 2.0;  // step = (20/2)/1 = 10.0
        s.cfg_energy_cap = 0.5;  // energy(0.0+1.0=1.0) >= 0.5 -- fires
        s.tick_budget    = 10.0; // == step -> increment path
        s.cycle_progress = 15.0; // + step(10.0) = 25.0 >= build_time_d(20.0) -- fires
        s.energy         = 0.0;
        s.state          = SENTINEL_STATE;
        seed_and_run(fx, s);

        ck(trace_eq({"update_charge_pips", "notify_ui"}), "E8: both calls still fire on this path");
        ck_eq_d(fx.b(0, 1).energy, 0.5, "E8: energy branch fired -- clamped to cap 0.5");
        ck_eq_d(fx.b(0, 1).cycle_progress, 5.0,
                "E8: cycle_progress branch ALSO fired -- 25.0-20.0=5.0, NOT left at 25.0 (the else-if-bug value)");
        ck_eq((uint32_t)fx.b(0, 1).state, (uint32_t)CHARGE_STEP_BLDG_STATE_CHARGE_GATE,
              "E8: state -> CHARGE_GATE(0x69) (redundantly set by both branches)");
    }
}

} // namespace mh::sim::test
