#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// The four ORIGINAL functions this body calls directly (i.e. not through a dispatch-table pointer).
// Indirected through a calls table for the same two reasons sim_unit_tick.h's unit_tick_calls is:
// testability under net_selftest.exe simtest over heap buffers, and the standing project policy
// that a translated function's siblings-still-in-flight are called through the ORIGINAL addresses,
// never a new mh::sim body, even within this same session. live_building_tick_calls() is the only
// binder; no stub/shadow variant is provided.
struct building_tick_calls {
    // llm_strat_bldg_turret_reload_tick @0x0047c60b. Args per the .asm push sequence: EAX=player,
    // EDX=the tick driver's CUR_INDEX (mh_calls.gen.h's own generated parameter name for this is
    // `building_id`, but the value passed is _G_LLM_STRAT_CUR_INDEX, not cur_building.building_id --
    // see the .cpp), stack=budget (double).
    void (*turret_reload_tick)(uint16_t player, uint32_t index, double budget);
    void (*tick_animation_state)();                        // llm_strat_bldg_tick_animation_state @0x00476305, void(void)
    void (*tick_pip_anim)(uint16_t player, int32_t index); // llm_strat_bldg_tick_pip_anim @0x00479244
    void (*apply_damage)();                                // llm_strat_bldg_apply_damage @0x004710ba, void(void)
};

const building_tick_calls &live_building_tick_calls();

namespace detail {

// llm_strat_building_tick @0x0046fd86.
//
// ENTRY (0x0046fd9e-0x0046fdcd): budget = GAME_CLOCK - cur_building.last_tick_time. GATE 1
// (0x0046fdb2-0x0046fdbd, JNC->0x0046fdcf the zero-out branch): PLAIN ordered `<=`, matching the
// assembly's FCOM/JNC exactly and matching sim_unit_tick's own ENTRY gate (same shape, same
// instructions): `if (budget <= 0.0) budget = 0.0; else last_tick_time = GAME_CLOCK;`.
//
// ENERGY GATE (0x0046fde3-0x0046fdff, guarded by built_flags!=3): GATE 2
// (0x0046fdf3-0x0046fdfb, JC->0x0046fdff). The NaN-inclusive negated form:
// `if (built_flags != 3 && !(energy <= 0.0)) budget = 0.0;` -- NOT `0.0 < energy` (Ghidra's own
// decompile text for this branch, which is NOT NaN-faithful: JC fires on ordered 0.0<energy OR
// unordered/NaN, and only `!(energy<=0.0)` reproduces both).
//
// TURRET TYPE CHECK (0x0046fe13-0x0046fe62): Building[cur_building.building_id].type ==
// A_TURRET(5) or H_TURRET(0x19) (mh::sim::BUILDING_TYPE_A_TURRET/H_TURRET, sim_order_enqueue.h's
// own constants, pinned from a Ghidra enum dump -- reused per the translator brief's naming rule
// rather than re-declared). Gates a call to turret_reload_tick(cur_player, cur_index, budget).
//
// ANIMATION STATE (0x0046fe62, unconditional) / PIP ANIM (0x0046fe67-0x0046fe88, gated on
// pip_active_count != 0).
//
// STATE-MACHINE DISPATCH LOOP (0x0046fe88-0x0046fee3): GATE 3 (0x0046fe92-0x0046fe9d,
// JNC->0x0046fee3 the loop-EXIT target). Same FCOM/JNC instructions as GATE 1 (same TICK_BUDGET
// global) but the Jcc TARGET is the loop exit this time, so the C++ shape FLIPS to the
// NaN-inclusive negated form: `while (!(budget <= 0.0))` -- continues on budget>0.0 OR NaN, exactly
// matching sim_unit_tick's own loop-condition gate (same underlying fact, different branch target;
// Ghidra's `0.0 < TICK_BUDGET` text is again not NaN-faithful). The runaway-loop guard
// (SIM_ACTIVE-gated): the guard is read into `old_guard` and incremented UNCONDITIONALLY once
// sim_active!=0, and only THEN is old_guard (the PRE-increment value) compared against 10000 -- so
// the forcing branch fires on the iteration where the guard was ALREADY over 10000 before this
// increment. On that iteration cur_building.state is forced to
// mh::sim::BLDG_STATE_DISMANTLE_FINISH(3) (unconditionally -- unlike unit_tick's HOVER_DISENGAGE
// special case, building_tick has no such carve-out: the .asm has only the one MOV word[state],3),
// and the guard resets to 0. When sim_active==0 the guard is NEITHER incremented NOR reset (JZ
// skips straight to dispatch). DISPATCH: v.bldg_state_funcs[cur_building.state]() runs on EVERY
// iteration, including the one that just forced a new state -- the forced state only takes effect
// on the dispatch NEXT time around the loop.
//
// EXIT (0x0046fee3-0x0046fef7): pending-damage application. GATE 4 (0x0046fee3-0x0046fef0,
// JNC->0x0046fef7 the skip-apply_damage target). Same NaN-inclusive negated shape as GATE 3:
// `if (!(pending_damage <= 0.0)) apply_damage();` -- fires on pending_damage>0.0 OR NaN.
//
// DONE CALLBACK (0x0046fef7-0x0046ff22): `if (built_flags==3 && state != BLDG_STATE_RUBBLE_SIGHT_
// DECAY(4)) v.bldg_done_funcs[cur_building.building_id]();` -- an ordinary, ordered (non-FP)
// two-CMP guard, no NaN idiom involved.
void building_tick(const sim_view &v, sim_store &own, const building_tick_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_building_tick_calls(). Matches the original's
// committed __watcall(void) shape (sig_llm_strat_building_tick) -- no parameters.
void building_tick();

namespace detail {
} // namespace detail

// `[promote] building_tick=1` REPLACES the original entry outright with our body -- there is no
// per-call A/B, so nothing to under-declare; the oracle is the per-step state-hash trajectory of an
// unpromoted golden vs. this promoted run (--soak-golden). This is G13/G19's answer for this
// dispatcher. `default_on` is the ship default, PASSED IN (a reimplementation TU may not include a
// seams header for SHIP_PROMOTE_*), same arrangement as install_promotion_unit_tick. Returns 1 if it
// installed.
int install_promotion_building_tick(int default_on);

} // namespace mh::sim
