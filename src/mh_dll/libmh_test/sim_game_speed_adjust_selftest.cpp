//
// sim_game_speed_adjust_selftest.cpp -- `simtest` offline oracle for llm_game_speed_increase /
// llm_game_speed_decrease (sim/sim_game_speed_adjust.{h,cpp}, RI-SIM / SIM1F). Both are tiny
// gate-then-step mirrors over one player's _G_LLM_GAME_SPEED_PLAYER_FACTOR entry: increase steps up
// while below the shared cap, decrease steps down while above the shared floor; the gate is a single
// FCOMP/JNC (resp. JBE) with NO post-step clamp -- reproduced here exactly, including the fact that a
// step taken from just inside the bound is allowed to land outside it.
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_game_speed_increase_004976cd.asm, tmp/decomp/llm_game_speed_decrease_00497734.asm):
//   increase: if factor < max: factor *= step_up; recompute(); print();   (JNC skips when factor>=max)
//   decrease: if min < factor: factor /= step_down; recompute(); print(); (JBE skips when factor<=min)
// Every fixture factor/max/min/step value used below is overridden to an exactly-representable double
// (the fixture's own boot defaults, 1.2 for both steps, are NOT exactly representable) so ck_eq_d's
// exact compare is meaningful -- see sim_test_support.h's comment on ck_eq_d.
//
#include "sim/sim_game_speed_adjust.h"

#include "sim_test_support.h"

#include <string>
#include <vector>

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- recorded outward calls: order AND count both matter (asm calls recompute() then print(), only
// on the branch taken) ------------------------------------------------------------------------------
std::vector<const char *> g_call_order;

void stub_recompute() { g_call_order.push_back("recompute"); }
void stub_print() { g_call_order.push_back("print"); }

const game_speed_adjust_calls g_calls = {stub_recompute, stub_print};

} // namespace

void run_speed_increase_tests() {
    sim_fixture fx;

    // ---- C1: mid-range step -- factor moves by EXACTLY factor*step_up, both calls fire in order ----
    fx.reset();
    fx.game_speed_factor_max       = 8.0; // distinct from every other bound in this file
    fx.game_speed_factor_step_up   = 1.5; // distinct from step_down (2.0 below) -- catches a swap
    fx.game_speed_player_factor[3] = 2.0; // < max(8.0)
    fx.game_speed_player_factor[5] = 9.0; // a DIFFERENT player, must stay untouched
    g_call_order.clear();
    {
        sim_store own = fx.store();
        detail::game_speed_increase(fx.view(), own, g_calls, /*player*/ 3);
    }
    ck_eq_d(fx.game_speed_player_factor[3], 3.0, "inc C1: factor[3] = 2.0 * step_up(1.5) = 3.0 exactly");
    ck_eq_d(fx.game_speed_player_factor[5], 9.0, "inc C1: an unrelated player's factor is untouched");
    ck((g_call_order.size() == 2 && g_call_order[0] == std::string("recompute") &&
        g_call_order[1] == std::string("print")),
       "inc C1: recompute() then print(), exactly once each, in that order");

    // ---- C2: boundary AT the max -- factor == max is a NO-OP (gate is `<`, not `<=`) ---------------
    fx.reset();
    fx.game_speed_factor_max       = 4.0;
    fx.game_speed_factor_step_up   = 1.5;
    fx.game_speed_player_factor[2] = 4.0; // == max exactly
    g_call_order.clear();
    {
        sim_store own = fx.store();
        detail::game_speed_increase(fx.view(), own, g_calls, /*player*/ 2);
    }
    ck_eq_d(fx.game_speed_player_factor[2], 4.0, "inc C2: factor AT max is unchanged (no step)");
    ck(g_call_order.empty(), "inc C2: no calls fire when the gate is not taken");

    // ---- C3: just BELOW the max still steps -- and the step is allowed to land ABOVE max (no clamp
    // after the multiply; the gate only tests the value BEFORE the step) ----------------------------
    fx.reset();
    fx.game_speed_factor_max       = 2.0;
    fx.game_speed_factor_step_up   = 2.0; // deliberately large so the result overshoots max
    fx.game_speed_player_factor[6] = 1.5; // < max(2.0)
    g_call_order.clear();
    {
        sim_store own = fx.store();
        detail::game_speed_increase(fx.view(), own, g_calls, /*player*/ 6);
    }
    ck_eq_d(fx.game_speed_player_factor[6], 3.0,
            "inc C3: factor = 1.5 * 2.0 = 3.0, allowed to exceed max(2.0) -- gate is pre-step only");
    ck(g_call_order.size() == 2, "inc C3: the step-taken branch still fires both calls");

    // ---- C4: the gate checks ONLY max, never min -- a factor already below min still steps up ------
    fx.reset();
    fx.game_speed_factor_min       = 0.5;
    fx.game_speed_factor_max       = 10.0;
    fx.game_speed_factor_step_up   = 4.0;
    fx.game_speed_player_factor[1] = 0.25; // below min(0.5), but min is irrelevant to increase
    g_call_order.clear();
    {
        sim_store own = fx.store();
        detail::game_speed_increase(fx.view(), own, g_calls, /*player*/ 1);
    }
    ck_eq_d(fx.game_speed_player_factor[1], 1.0,
            "inc C4: factor = 0.25 * 4.0 = 1.0, min is never consulted by increase");
    ck(g_call_order.size() == 2, "inc C4: still steps -- increase does not gate on min");

    // ---- C5: player-id 16-bit narrowing is inert for the real domain -- a high-word-garbage player
    // value indexes the SAME slot as its low 16 bits (translation comment: MOVZX word re-read x3) -----
    fx.reset();
    fx.game_speed_factor_max       = 8.0;
    fx.game_speed_factor_step_up   = 3.0;
    fx.game_speed_player_factor[4] = 1.0;
    g_call_order.clear();
    {
        sim_store own = fx.store();
        detail::game_speed_increase(fx.view(), own, g_calls, /*player*/ 0x00010004u); // low16 = 4
    }
    ck_eq_d(fx.game_speed_player_factor[4], 3.0,
            "inc C5: player=0x10004 steps slot 4 (low 16 bits), factor = 1.0*3.0 = 3.0");
}

void run_speed_decrease_tests() {
    sim_fixture fx;

    // ---- D1: mid-range step -- factor moves by EXACTLY factor/step_down, both calls fire in order --
    fx.reset();
    fx.game_speed_factor_min       = 0.5; // distinct from every other bound in this file
    fx.game_speed_factor_step_down = 2.0; // distinct from step_up (1.5 above) -- catches a swap
    fx.game_speed_player_factor[3] = 6.0; // > min(0.5)
    fx.game_speed_player_factor[5] = 9.0; // a DIFFERENT player, must stay untouched
    g_call_order.clear();
    {
        sim_store own = fx.store();
        detail::game_speed_decrease(fx.view(), own, g_calls, /*player*/ 3);
    }
    ck_eq_d(fx.game_speed_player_factor[3], 3.0, "dec D1: factor[3] = 6.0 / step_down(2.0) = 3.0 exactly");
    ck_eq_d(fx.game_speed_player_factor[5], 9.0, "dec D1: an unrelated player's factor is untouched");
    ck((g_call_order.size() == 2 && g_call_order[0] == std::string("recompute") &&
        g_call_order[1] == std::string("print")),
       "dec D1: recompute() then print(), exactly once each, in that order");

    // ---- D2: boundary AT the min -- factor == min is a NO-OP (gate is `min < factor`, so == skips) -
    fx.reset();
    fx.game_speed_factor_min       = 1.0;
    fx.game_speed_factor_step_down = 2.0;
    fx.game_speed_player_factor[2] = 1.0; // == min exactly
    g_call_order.clear();
    {
        sim_store own = fx.store();
        detail::game_speed_decrease(fx.view(), own, g_calls, /*player*/ 2);
    }
    ck_eq_d(fx.game_speed_player_factor[2], 1.0, "dec D2: factor AT min is unchanged (no step)");
    ck(g_call_order.empty(), "dec D2: no calls fire when the gate is not taken (JBE skips on <=)");

    // ---- D3: just ABOVE the min still steps -- and the step is allowed to land BELOW min (no clamp
    // after the divide; the gate only tests the value BEFORE the step) ------------------------------
    fx.reset();
    fx.game_speed_factor_min       = 1.0;
    fx.game_speed_factor_step_down = 4.0; // deliberately large so the result undershoots min
    fx.game_speed_player_factor[6] = 2.0; // > min(1.0)
    g_call_order.clear();
    {
        sim_store own = fx.store();
        detail::game_speed_decrease(fx.view(), own, g_calls, /*player*/ 6);
    }
    ck_eq_d(fx.game_speed_player_factor[6], 0.5,
            "dec D3: factor = 2.0 / 4.0 = 0.5, allowed to fall below min(1.0) -- gate is pre-step only");
    ck(g_call_order.size() == 2, "dec D3: the step-taken branch still fires both calls");

    // ---- D4: the gate checks ONLY min, never max -- a factor already above max still steps down ----
    fx.reset();
    fx.game_speed_factor_max       = 2.0;
    fx.game_speed_factor_min       = 0.5;
    fx.game_speed_factor_step_down = 4.0;
    fx.game_speed_player_factor[1] = 20.0; // above max(2.0), but max is irrelevant to decrease
    g_call_order.clear();
    {
        sim_store own = fx.store();
        detail::game_speed_decrease(fx.view(), own, g_calls, /*player*/ 1);
    }
    ck_eq_d(fx.game_speed_player_factor[1], 5.0,
            "dec D4: factor = 20.0 / 4.0 = 5.0, max is never consulted by decrease");
    ck(g_call_order.size() == 2, "dec D4: still steps -- decrease does not gate on max");

    // ---- D5: player-id 16-bit narrowing is inert for the real domain, mirroring inc C5 -------------
    fx.reset();
    fx.game_speed_factor_min       = 0.5;
    fx.game_speed_factor_step_down = 2.0;
    fx.game_speed_player_factor[4] = 3.0;
    g_call_order.clear();
    {
        sim_store own = fx.store();
        detail::game_speed_decrease(fx.view(), own, g_calls, /*player*/ 0x00020004u); // low16 = 4
    }
    ck_eq_d(fx.game_speed_player_factor[4], 1.5,
            "dec D5: player=0x20004 steps slot 4 (low 16 bits), factor = 3.0/2.0 = 1.5");
}

} // namespace mh::sim::test

// ---- mutation notes (spot-verifiable by breaking the translation) --------------------------------
// - inc C1 / dec D1 (exact-step value): swapping `factor *= step_up` for `factor += step_up`, or
//   swapping step_up<->step_down between the two functions, turns 3.0 into a different number.
// - inc C2 / dec D2 (boundary no-op): changing the increase gate from `<` to `<=` (or the decrease
//   gate from `min <` to `min <=`) makes the boundary case wrongly take the step and fire the calls.
// - inc C4 / dec D4 (wrong-bound gate): swapping which bound (max vs min) increase/decrease checks
//   makes these two cases wrongly skip (or wrongly fire) the step.
// - C1's call-order check: reordering `c.game_speed_recompute()`/`c.ui_print_game_speed()` in the
//   translation flips g_call_order[0]/[1] and fails that check red.
