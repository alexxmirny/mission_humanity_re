//
// ai/ai_players_tick.h -- llm_strat_ai_players_tick @0x004db905 (RI-AI batch D / AI1D,
// 2026-08-28).
//
// `void __watcall llm_strat_ai_players_tick(double dt)`, ONE parameter, on the STACK
// (storage=Stack[0x4]:8; the body ends `RET 0x8`) -- see the .asm header at the top of
// tmp/decomp_ai/llm_strat_ai_players_tick_004db905.asm.
//
// WHAT THIS FUNCTION ACTUALLY IS. Despite the name, this is NOT the AI's driver/gate -- it is the
// per-player MOVE/TACTIC/STRATEGY *SCHEDULER*. It iterates every CLAIMED player slot (0 ..
// *active_player_count) and advances all four of that player's AI clocks (ai_clock,
// ai_clock_s/_t/_m) UNCONDITIONALLY, every tick, for AI and non-AI players alike. Only the PHASE
// CALLS are gated on that player's own `ai_enabled` -- and the MOVE-phase catch-up dispatches
// `llm_strat_unit_passive_engage_tick` for a NON-AI player, so this function is also what drives a
// HUMAN player's passive unit engagement. (Recorded under AI2; not
// re-derived here.)
//
// ---- THE FIVE LOOPS, IN ORDER (each `for (p = 0; p < *active_player_count; ++p)`) --------------
//
// 0. (0x004db914-0x004db925, once, before any loop) `width_m`/`height_m` @0x00fe5b40/0x00fe5b44
//    are re-stamped from the current map extents: `width - 1`, `height - 1`. NOT AI-owned (two
//    other writers exist image-wide -- llm_map_build_regions, llm_strat_planet_map_session_init;
//    see ai_store::map_width_mask/map_height_mask's own comment in ai_state.h) -- this function
//    just re-derives them every AI tick, same as every other reader/writer of the pair.
//
// 1. (0x004db92e-0x004db98d) Per player: `if (ai_map_changed_pending & ai_enabled)` ->
//    recompute_map_influence(p); then `if (ai_turret_rescan_pending & ai_enabled)` ->
//    turret_threat_rescan(p). BOTH TESTS ARE A BITWISE AND of the two player_data dwords (`TEST
//    dword [flag], EBX` with EBX == ai_enabled, reloaded fresh at each of the two sites --
//    0x004db944 and 0x004db96f -- not hoisted), not a logical `&&` -- reproduced as `&` on the two
//    int32_t fields, not `!= 0 && != 0`.
//
// 2. (0x004db991-0x004db9b7) Per player: `ai_clock += dt`. THE FP SHAPE THAT MATTERS: `FLD float
//    [ai_clock]` / `FADD double ptr [EBP+0x8]` (dt, a genuine DOUBLE addend) / `FSTP float
//    [ai_clock]` -- the accumulate happens in a wider-than-float intermediate and is stored back
//    truncated to float in ONE rounding step. Expressed here as `(float)((double)ai_clock + dt)`
//    (widen to double, add, round once to float on the store) to match that shape, per
//    translator-brief rule 9 / the Sim TUs' established `/fp:precise` idiom -- NOT
//    `ai_clock = (float)(ai_clock + (float)dt)`, which would round dt down to float BEFORE adding
//    and is a different, wrong, answer.
//
// 3. (0x004db9c3-0x004dba25) Per player, an INNER CATCH-UP `while` (not an `if` -- 0x004dba22 jumps
//    back to the top of 0x004db9c3): while `ai_clock_s < ai_clock`, `if (ai_enabled) ->
//    ai_player_tick(p)`; then `ai_clock_s += STRATEGY_PERIOD`, unconditionally, and retest. The
//    period add is float+float (both operands float in the image), so it needs no double widening
//    the way step 2 does.
//
// 4. (0x004dba31-0x004dba93) The identical catch-up shape on `ai_clock_t`, calling
//    ai_unit_group_tick(p) when enabled, advancing by TACTIC_PERIOD.
//
// 5. (0x004dba9f-0x004dbb08) The identical catch-up shape on `ai_clock_m`, but the call is CHOSEN
//    by `ai_enabled` rather than gated by it: enabled -> ai_active_unit_tick(p) (0x004dbb00),
//    NOT enabled -> unit_passive_engage_tick(p) (0x004dbaa1). Both arms fall into the SAME advance
//    (`ai_clock_m += MOVE_PERIOD`) and retest -- there is no "do nothing" arm here; one of the two
//    calls always runs while the catch-up condition holds. The label-jump shape in the assembly
//    (LAB_004dba9f -> falls into the advance at LAB_004dbaa6 -> loops to the test at
//    LAB_004dbace; the enabled arm calls then jumps straight to LAB_004dbaa6, skipping the passive
//    call) collapses to a single `if (enabled) active else passive; advance; retest` with no
//    behavioural difference -- both arms reach the identical advance-then-retest tail either way.
//
// `_G_LLM_STRAT_AI_ACTIVE_PLAYER_COUNT` @0x006616a8 is the loop bound of ALL FIVE loops and is
// RE-READ every iteration in the original (0x004db985, 0x004db9b7, 0x004dba25, 0x004dba93,
// 0x004dbb08) -- reproduced here as the `for`/`while` loop CONDITION itself (`p < *v.active_player_
// count`), never hoisted into a local, matching translator-brief's "do not cache the loop bound"
// instruction and rule 16 ("const means may-not-write, not immutable").
//
// ---- THE UNORDERED-AWARE CATCH-UP TEST (0x004db9d9-0x004db9e8 and its two siblings at
// 0x004dba47-0x004dba56 / 0x004dbae4-0x004dbaf3) -----------------------------------------------
//
// Each catch-up test is `FLD [s]` / `FCOMP [clock]` / `FNSTSW AX` / `SAHF` / `JNC <exit>`. FCOMP's
// unordered result (either operand NaN) sets C3=C2=C0=1 -- THE SAME BIT PATTERN as "ST(0) <
// operand" (C0=1, C2=C3=0 is actually the "less than" encoding; unordered is C3=C2=C0=1, so C0=1
// in BOTH cases) -- and SAHF only exposes C0 (as CF), never checking the parity flag (C2) that
// would disambiguate "less than" from "unordered". So `JNC` (jump when CF=0) fires ONLY on the
// ordered "not less than" case and treats an unordered comparison exactly like "less than": the
// loop body runs. A plain C++ `s < clock` does NOT reproduce this -- IEEE 754 makes any comparison
// with a NaN operand false, which would make `s < clock` false (exit) exactly where the original
// continues. The equivalent that DOES match, using only the ordered relation the hardware exposes
// through CF, is `!(s >= clock)`: when either operand is NaN, `s >= clock` is false (IEEE), so the
// negation is true (continue) -- matching JNC's unordered-continues behaviour; when ordered, `!(s
// >= clock)` is exactly `s < clock`. Used for all three catch-up tests below. See
// `uncertainties[]` in the translation report -- this is a real behavioural choice, not a stylistic
// one.
//
// ---- PRESERVE-BUG: THE THREE CATCH-UP LOOPS ARE UNBOUNDED -----------------------------------
// If STRATEGY_PERIOD / TACTIC_PERIOD / MOVE_PERIOD is ever <= 0 (or the clock fails to advance for
// any other reason), the corresponding `while` spins forever, exactly as the original does -- no
// bound is added here. (Loop tests: 0x004db9d9-0x004db9e8 / 0x004dba47-0x004dba56 /
// 0x004dbae4-0x004dbaf3; the adds that are supposed to make progress: 0x004dba0e-0x004dba1b /
// 0x004dba7c-0x004dba89 / 0x004dbaba-0x004dbac7.)
//
// ---- DECLARED NEED: three tuning scalars have no ai_view member yet ---------------------------
// `_G_LLM_STRAT_AI_STRATEGY_PERIOD` @0x0066931c, `_G_LLM_STRAT_AI_TACTIC_PERIOD` @0x00669320 and
// `_G_LLM_STRAT_AI_MOVE_PERIOD` @0x00669324 (all `float`, read at 0x004dba0e / 0x004dba7c /
// 0x004dbaba) are NOT yet carried by `ai_view` -- checked against the whole struct, batch A/B/C
// layers 0-6, before writing this file. Referenced below as `v.ai_strategy_period` /
// `v.ai_tactic_period` / `v.ai_move_period`; the conductor needs to add these three `const float *`
// members to `ai_view` (same run of AI.SCR scalars as the other already-carried tuning floats,
// e.g. `repair_ratio`/`silo_ratio`) before this translation unit compiles. NOT worked around with a
// literal VA -- per translator-brief rule 2 / 4b.
//
// `map_width` / `map_height` (0x00825084 / 0x00825064) and the loop-bound global
// (`active_player_count`, 0x006616a8) were ALREADY present on `ai_view` (batch A layer 2 /
// batch C layer 1 respectively) and are used as-is; only the three period scalars above are new.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The six outward calls this function makes stay ORIGINAL game calls, routed through the shared
// module-wide `ai_calls` (ai_state.h) rather than a per-unit struct -- CORRECTED 2026-08-28: the
// conductor's initial rule-3b guidance describing a per-translation-unit `<tu>_calls` struct is the
// sim/tact convention, not ai's. The ai domain binds ALL its outward edges positionally in ONE
// `struct ai_calls` (ai_state.h), live in `live_calls()` / shadow-armed in `shadow_calls()`
// (ai_state.cpp). This function's six edges now live there as `gc.recompute_map_influence` /
// `gc.turret_threat_rescan` / `gc.player_tick` / `gc.ai_unit_group_tick` (note the `ai_` prefix --
// a bare `unit_group_tick` would collide) / `gc.active_unit_tick` / `gc.passive_engage_tick`.
//
// The logic over an EXPLICIT state + call table, so `net_selftest.exe aitest` can drive it over heap
// buffers with recording stubs and no game/rig. The wrapper below is this applied to
// state()/live_calls().
namespace detail {

// llm_strat_ai_players_tick @0x004db905.
void players_tick(const ai_view &v, const ai_store &own, const ai_calls &gc, double dt);

} // namespace detail

void players_tick(double dt);

} // namespace mh::ai
