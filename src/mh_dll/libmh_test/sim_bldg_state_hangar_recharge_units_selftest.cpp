//
// sim_bldg_state_hangar_recharge_units_selftest.cpp -- `simtest` cases for
// llm_strat_bldg_state_hangar_recharge_units (sim/sim_bldg_state_hangar.h/.cpp @0x00472b9d), the
// HANGAR_RECHARGE_UNITS building_tick handler.
//
// THIS FUNCTION IS DO-NOT-ARM (its write closure reaches game_SetEvent's ~780-function UI/gfx/snd/
// menu-teardown over-approximation) -- there is no rig run backing this up. This oracle is the ONLY
// evidence for the translation.
//
// EXPECTED BEHAVIOUR, HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_hangar_recharge_units_00472b9d.asm), cross-checked against
// sim/sim_bldg_state_hangar.h/.cpp's own per-line address citations -- NOT read off the .cpp body
// alone:
//
//   cycle_progress = tick_budget*efficiency + cycle_progress; tick_budget = 0.0 -- UNCONDITIONAL,
//   runs even when the gate below is not taken (0x00472bb5-0x00472bd9).
//   FP gate (0x00472be8-0x00472bf4): `if (HANGAR_RECHARGE_PERIOD <= cycle_progress)` (JC @0x00472bf4
//   skips straight to the epilogue @0x00472c38 when cycle_progress < PERIOD -- see the header's
//   FP-COMPARISON note: the "skip iff <" and "skip iff unordered" readings COINCIDE here, so the
//   straightforward `<=` matches every input, ordered or not).
//   Inside the gate (0x00472bf6-0x00472c33, all unconditional once entered):
//     hangar_recharge_pulse(cur_player, cur_index) (cur_player/cur_index READ FRESH at this call site,
//     0x00472bf6/0x00472bfd, BEFORE the decrement below runs);
//     cycle_progress += HANGAR_RECHARGE_PERIOD_NEG (0x00472c11, an ADD of the NEGATIVE constant --
//     NOT a re-derived `-= PERIOD`, see C3 below);
//     state = HANGAR_RECHARGE_CHECK (0x78) (0x00472c1f);
//     bldg_notify_ui(cur_player, cur_index) (cur_player/cur_index READ FRESH AGAIN at THIS call site,
//     0x00472c25/0x00472c2c -- not values cached before the branch or before the pulse call).
//   Else (gate not taken): falls straight to the epilogue -- NO further reads/writes/calls at all
//   (confirmed by the JC target landing directly on the shared epilogue, not on any intermediate code).
//   Unlike hangar_recharge_check (the sibling in the same TU), this function makes NO
//   llm_strat_refresh_building call on either path, and NO call fires on both branches -- everything
//   past the accumulation is gated behind the single FP compare.
//
// sim/sim_bldg_state_hangar.h's DECLARED NEED (sim_view::hangar_recharge_period/_neg) is already
// resolved in sim_test_support.h (SIM1-G4, real read-memory-confirmed 20.0/-20.0) --
// nothing to add to the fixture for this file.
//
#include "sim/sim_bldg_state_hangar.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: proves call ORDER (and that a call did/did not fire at all) ------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- fixture handle + mutation knob, for the fresh-read/order case (C6) --------------------------
// The pulse recorder needs to (a) observe cur_building's cycle_progress AT THE MOMENT it fires (to
// pin that the call happens BEFORE the +=PERIOD_NEG decrement), and (b) optionally mutate
// view_cur_player/view_cur_index mid-call to prove the SECOND call (notify_ui) re-reads them fresh
// rather than using values cached before the branch.
sim_fixture *g_fx                  = nullptr;
bool         g_mutate_during_pulse = false;
uint16_t     g_mutate_player_to    = 0;
uint16_t     g_mutate_index_to     = 0;

struct PulseCall {
    uint32_t player;
    int32_t  index;
    double   cycle_progress_at_call; // b.cycle_progress observed AT the instant pulse fires
};
std::vector<PulseCall> g_pulse_calls;
void                   rec_hangar_recharge_pulse(uint32_t player, int32_t index) {
    tr("pulse");
    const double cp = g_fx ? g_fx->b(static_cast<int32_t>(player), index).cycle_progress : 0.0;
    g_pulse_calls.push_back({player, index, cp});
    if (g_mutate_during_pulse && g_fx) {
        g_fx->view_cur_player = g_mutate_player_to;
        g_fx->view_cur_index  = g_mutate_index_to;
    }
}

struct NotifyUiCall {
    uint16_t player;
    uint32_t index;
};
std::vector<NotifyUiCall> g_notify_ui_calls;
void                      rec_bldg_notify_ui(uint16_t player, uint32_t index) {
    tr("notify_ui");
    g_notify_ui_calls.push_back({player, index});
}

const bldg_state_hangar_recharge_units_calls g_calls = {
    &rec_hangar_recharge_pulse,
    &rec_bldg_notify_ui,
};

void reset_observations() {
    g_trace.clear();
    g_pulse_calls.clear();
    g_notify_ui_calls.clear();
}

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player     = 0;
    int32_t  index      = 1;
    uint16_t seed_state = 0x55; // sentinel building state, distinct from BLDG_STATE_HANGAR_RECHARGE_CHECK(0x78)

    // Distinct, non-symmetric values: accumulated = tick_budget_in*efficiency + cycle_progress_in.
    // 2.0*3.0+4.0 = 10.0 is reachable ONLY by this exact operand order -- every swapped-operand
    // reading (efficiency*cycle_progress_in+tick_budget_in, tick_budget_in+efficiency*cycle_progress_in,
    // etc.) produces 14.0 instead.
    double efficiency        = 3.0;
    double cycle_progress_in = 4.0;
    double tick_budget_in    = 2.0;

    // The two boot-constant doubles. Default to the fixture's own real read-memory-confirmed values
    // (20.0/-20.0); a case overrides one or both to probe the gate boundary or to distinguish
    // "add PERIOD_NEG" from a re-derived "subtract PERIOD".
    double period     = 20.0;
    double period_neg = -20.0;
};

// mutate_during_pulse/mutate_player_to/mutate_index_to: armed ONLY by C6, to prove cur_player/
// cur_index are read fresh at each call site rather than cached once before the branch.
void seed_and_run(sim_fixture &fx, const Seed &s, bool mutate_during_pulse = false,
                  uint16_t mutate_player_to = 0, uint16_t mutate_index_to = 0) {
    fx.reset();

    building &b      = fx.b(s.player, s.index);
    b.cycle_progress = s.cycle_progress_in;
    b.efficiency     = s.efficiency;
    b.state          = s.seed_state;

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = s.player;
    fx.view_cur_index   = static_cast<uint16_t>(s.index);
    fx.tick_budget      = s.tick_budget_in;

    fx.hangar_recharge_period     = s.period;
    fx.hangar_recharge_period_neg = s.period_neg;

    g_fx = &fx;
    reset_observations();
    g_mutate_during_pulse = mutate_during_pulse;
    g_mutate_player_to    = mutate_player_to;
    g_mutate_index_to     = mutate_index_to;

    sim_store own = fx.store();
    detail::bldg_state_hangar_recharge_units(fx.view(), own, g_calls);
}

} // namespace

void run_bldg_state_hangar_recharge_units_tests() {
    sim_fixture fx;

    // =================================================================================================
    // C1 -- gate NOT taken (accumulated 10.0 < default PERIOD 20.0): the unconditional accumulation
    // still runs and tick_budget still zeroes, but NOTHING past the FCOMP fires -- no pulse, no
    // notify_ui, no state write.
    // =================================================================================================
    {
        Seed s; // efficiency=3.0, cycle_progress_in=4.0, tick_budget_in=2.0 -> accumulated = 10.0
        seed_and_run(fx, s);

        ck_eq_d(fx.b(s.player, s.index).cycle_progress, 10.0,
                "C1: cycle_progress = tick_budget*efficiency + cycle_progress_in = 2.0*3.0+4.0 = 10.0 "
                "(0x00472bc0 FLD tick_budget / 0x00472bc6 FMUL efficiency / 0x00472bc9 FADD "
                "cycle_progress / 0x00472bcc FSTP) -- a swapped operand order would read 14.0");
        ck_eq_d(fx.tick_budget, 0.0, "C1: tick_budget zeroed unconditionally (0x00472bcf/0x00472bd9)");
        ck(trace_eq({}), "C1: gate not taken -- neither pulse nor notify_ui fires (0x00472bf4 JC to 0x00472c38)");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, (uint32_t)s.seed_state,
              "C1: building.state UNCHANGED -- the 0x78 write at 0x00472c1f is inside the gate only");
    }

    // =================================================================================================
    // C2 -- gate TAKEN at the exact `<=` boundary (accumulated == PERIOD, pins `<=` not `<`): the full
    // pulse/decrement/state/notify_ui sequence fires, in order, with the real 20.0/-20.0 boot values.
    // =================================================================================================
    {
        Seed s;
        s.efficiency        = 1.0;
        s.cycle_progress_in = 20.0; // == period, tick_budget_in=0 keeps accumulation exact
        s.tick_budget_in    = 0.0;
        seed_and_run(fx, s);

        ck(trace_eq({"pulse", "notify_ui"}),
           "C2: exact call order at the boundary (0x00472bf6 pulse -> 0x00472c11 decrement -> "
           "0x00472c1f state write -> 0x00472c33 notify_ui)");

        ck(g_pulse_calls.size() == 1 && g_pulse_calls[0].player == s.player &&
               g_pulse_calls[0].index == s.index,
           "C2: hangar_recharge_pulse(cur_player, cur_index) (0x00472bf6-0x00472c04)");
        // Guarded on size() -- a mutant that skips the gate at exact equality (e.g. `<` for `<=`)
        // leaves g_pulse_calls EMPTY, and the check above already fails that case; indexing [0]
        // unconditionally here would be an out-of-bounds read on an empty vector instead of a second
        // named failure (caught mutation-testing this row: it crashed the whole suite in Release).
        if (!g_pulse_calls.empty()) {
            ck_eq_d(g_pulse_calls[0].cycle_progress_at_call, 20.0,
                    "C2 ORDER: pulse observes cycle_progress STILL AT the accumulated 20.0 -- the "
                    "+=PERIOD_NEG decrement at 0x00472c11 has NOT run yet when pulse fires at 0x00472c04");
        } else {
            ck(false, "C2 ORDER: pulse observes cycle_progress STILL AT the accumulated 20.0 -- "
                      "SKIPPED, hangar_recharge_pulse never fired (see the check above)");
        }

        ck_eq_d(fx.b(s.player, s.index).cycle_progress, 0.0,
                "C2: cycle_progress == PERIOD exactly -> gate taken at equality (0x00472beb FCOMP / "
                "0x00472bf4 JC NOT taken when equal), then cycle_progress = 20.0 + (-20.0) = 0.0 "
                "(0x00472c11 FADD period_neg)");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0x78u,
              "C2: state = HANGAR_RECHARGE_CHECK (0x00472c1f, `MOV word ptr [EAX+0xd],0x78`)");
        ck(g_notify_ui_calls.size() == 1 && g_notify_ui_calls[0].player == s.player &&
               g_notify_ui_calls[0].index == (uint32_t)s.index,
           "C2: bldg_notify_ui(cur_player, cur_index) (0x00472c25-0x00472c33)");
        ck_eq_d(fx.tick_budget, 0.0, "C2: tick_budget zeroed unconditionally, gate taken or not");
    }

    // =================================================================================================
    // C3 -- gate taken, PERIOD and PERIOD_NEG deliberately NOT arithmetic negatives of each other
    // (8.0 / -3.0): pins that the decrement is `cycle_progress += *v.hangar_recharge_period_neg`
    // literally (0x00472c11 FADD [PERIOD_NEG]) and NOT a re-derived `-= *v.hangar_recharge_period`.
    // A translation that recomputed the decrement from PERIOD would read 9.0-8.0=1.0 instead of the
    // correct 9.0+(-3.0)=6.0.
    // =================================================================================================
    {
        Seed s;
        s.period            = 8.0;
        s.period_neg        = -3.0; // NOT -period -- the distinguishing choice
        s.efficiency        = 1.0;
        s.cycle_progress_in = 9.0; // > period(8.0), clear of the boundary so this case isolates ONLY
                                   // the period_neg-vs-negated-period question, not the `<=` edge
        s.tick_budget_in = 0.0;
        seed_and_run(fx, s);

        ck(trace_eq({"pulse", "notify_ui"}), "C3: gate taken (9.0 > 8.0)");
        ck_eq_d(fx.b(s.player, s.index).cycle_progress, 6.0,
                "C3: cycle_progress = 9.0 + PERIOD_NEG(-3.0) = 6.0 (0x00472c11 FADD reads the NEG "
                "global directly) -- a `-= PERIOD` translation would produce 1.0 instead");
    }

    // =================================================================================================
    // C4 -- one ULP BELOW the default PERIOD(20.0): the FP gate's `<=` must NOT fire here. Pins the
    // boundary from the low side (paired with C5's high side and C2's exact-equality side).
    // =================================================================================================
    {
        Seed s;
        s.cycle_progress_in = std::nextafter(20.0, -std::numeric_limits<double>::infinity());
        s.tick_budget_in    = 0.0;
        s.efficiency        = 1.0;
        seed_and_run(fx, s);

        ck(trace_eq({}), "C4: cycle_progress one ULP below PERIOD -- gate NOT taken (0x00472bf4 JC taken)");
        ck_eq_d(fx.b(s.player, s.index).cycle_progress, s.cycle_progress_in,
                "C4: cycle_progress left exactly as accumulated -- no +=PERIOD_NEG applied");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, (uint32_t)s.seed_state,
              "C4: state UNCHANGED one ULP below the boundary");
    }

    // =================================================================================================
    // C5 -- one ULP ABOVE the default PERIOD(20.0): the gate MUST fire here (paired with C4).
    // =================================================================================================
    {
        Seed s;
        s.cycle_progress_in = std::nextafter(20.0, std::numeric_limits<double>::infinity());
        s.tick_budget_in    = 0.0;
        s.efficiency        = 1.0;
        seed_and_run(fx, s);

        ck(trace_eq({"pulse", "notify_ui"}), "C5: cycle_progress one ULP above PERIOD -- gate taken");
        ck_eq_d(fx.b(s.player, s.index).cycle_progress, s.cycle_progress_in + s.period_neg,
                "C5: cycle_progress = (PERIOD + 1ulp) + PERIOD_NEG(-20.0)");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0x78u, "C5: state = HANGAR_RECHARGE_CHECK one ULP above the boundary");
    }

    // =================================================================================================
    // C6 -- ORDER: cur_player/cur_index are read FRESH at EACH call site (0x00472bf6/0x00472bfd before
    // pulse, 0x00472c25/0x00472c2c before notify_ui), not cached once before the branch. The pulse mock
    // mutates view_cur_player/view_cur_index; notify_ui must observe the NEW values while pulse itself
    // must have observed the ORIGINAL ones.
    // =================================================================================================
    {
        Seed s;
        s.player            = 3;
        s.index             = 7;
        s.cycle_progress_in = 25.0; // > period(20.0) -- gate taken
        s.tick_budget_in    = 0.0;
        s.efficiency        = 1.0;

        const uint16_t mutate_player_to = 1;
        const uint16_t mutate_index_to  = 99;
        seed_and_run(fx, s, /*mutate_during_pulse=*/true, mutate_player_to, mutate_index_to);

        ck(trace_eq({"pulse", "notify_ui"}), "C6: both calls still fire under the mutation knob");
        ck(g_pulse_calls.size() == 1 && g_pulse_calls[0].player == s.player &&
               g_pulse_calls[0].index == s.index,
           "C6: pulse(cur_player, cur_index) sees the ORIGINAL (3, 7) -- read at 0x00472bf6/0x00472bfd "
           "BEFORE the mock mutates the globals");
        ck(g_notify_ui_calls.size() == 1 && g_notify_ui_calls[0].player == mutate_player_to &&
               g_notify_ui_calls[0].index == (uint32_t)mutate_index_to,
           "C6: notify_ui(cur_player, cur_index) sees the MUTATED (1, 99) -- read fresh at "
           "0x00472c25/0x00472c2c, proving neither value was cached before the branch or before the "
           "pulse call");
    }

    // =================================================================================================
    // C7 -- a REAL NaN operand (not just an ordered near-boundary value -- cycle_progress is a plain
    // double fixture field, directly settable to NaN through the public API). Pins the header's
    // FP-COMPARISON claim precisely: x87's JC-after-FCOMP fires on both "<" and UNORDERED, and IEEE 754
    // `PERIOD <= NaN` is ALSO false, so the two readings coincide -- the gate must NOT fire here, same
    // as the ordinary "not taken" arm, with no special-casing required in the translation.
    // =================================================================================================
    {
        Seed s;
        s.cycle_progress_in = std::numeric_limits<double>::quiet_NaN();
        s.tick_budget_in    = 0.0; // 0.0*efficiency + NaN = NaN (NaN propagates through the accumulation)
        s.efficiency        = 1.0;
        seed_and_run(fx, s);

        ck(trace_eq({}),
           "C7: cycle_progress == NaN -- gate NOT taken (0x00472be8 FCOMP/0x00472bf4 JC also fires on "
           "an UNORDERED result, matching IEEE `PERIOD <= NaN` == false -- no negated restatement needed)");
        ck(std::isnan(fx.b(s.player, s.index).cycle_progress),
           "C7: cycle_progress left as NaN (propagated by the accumulation, untouched by the skipped "
           "+=PERIOD_NEG)");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, (uint32_t)s.seed_state,
              "C7: state UNCHANGED when cycle_progress is NaN");
    }
}

} // namespace mh::sim::test
