//
// sim/sim_unit_tick.h -- the per-unit strategic SPINE tick (RI-SIM / SIM1A).
//
// One function: llm_strat_unit_tick @0x0047c73a (0x1c9 bytes), __watcall(void) -- NO explicit
// parameters. Everything it touches is the ambient "which unit is being ticked right now" globals
// (_G_LLM_STRAT_CUR_UNIT et al.), set by an outer per-player/per-unit dispatch loop that is OUTSIDE
// this translation (not in the SIM1A manifest) before it calls this function once per live unit.
//
// SHAPE: budget sample -> soldiers cosmetic walk (scaled by cfg Unit.soldier_count) -> weapon reload
// x4 slots -> anim/rotation/target sub-ticks -> the state-machine dispatch loop (bounded by a
// runaway-state guard) -> pending-damage application. Five of the seven outward calls
// (update_soldiers, update_anim, update_rotation, target_tick, and the state-func-table dispatch's
// targets) are OTHER functions in this same session's SIM1A slice, and per the translator brief's
// standing policy they are called through mh::call:: against the ORIGINAL addresses here regardless
// -- this site must behave as if none of its siblings were promoted yet.
//
// ---- DECLARED NEEDS (see the .cpp for the full derivation) -- this TU will not compile until the
// conductor lands them, by design, same as sim_unit_passive_engage.cpp's engage_candidate_scratch_
// count precedent:
//   sim_view::unit_state_funcs   (const unit_state_fn *)  -- the ORIGINAL state-handler jump table,
//                                 _G_LLM_STRAT_UNIT_STATE_FUNCS @0x00e15a3c. Dispatched through
//                                 exactly as the assembly does; the individual handlers are NOT
//                                 resolved or inlined here.
//   sim_store::cur_unit()        (unit &)                 -- _G_LLM_STRAT_CUR_UNIT @0x00e162e0, a
//                                 `unit *` AMBIENT GLOBAL (two levels of indirection: the region is
//                                 the pointer SLOT, and the binder must dereference it once at
//                                 state()-bind time to capture which roster record this tick call
//                                 concerns).
//   sim_store::tick_budget()     (double &)                -- _G_LLM_STRAT_TICK_BUDGET @0x00ae3738.
//   sim_store::state_loop_guard() (int32_t &)               -- _G_LLM_STRAT_STATE_LOOP_GUARD
//                                 @0x00e15a38.
//   mh::sim::unit_state_fn        (using = void (*)())      -- the jump table's element type, ambient
//                                 __watcall(void) like update_soldiers/update_anim/etc.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected exactly as sim_unit_passive_engage.h's table is, for the same two reasons: testability
// under net_selftest.exe simtest, and the standing project policy that a translated function's
// siblings-still-in-flight are called through the ORIGINAL addresses, never the new mh::sim bodies,
// even when those siblings are being translated in the very same session. All seven are ORIGINAL
// functions; none is reimplemented in this file, so no stub/shadow variant is provided --
// live_unit_tick_calls() is the only binder.
struct unit_tick_calls {
    void (*update_soldiers)(); // llm_strat_unit_update_soldiers
    void (*weapon_reload_tick)(uint8_t weapon_slot, double delta_time,
                               double game_clock_unread); // llm_strat_unit_weapon_reload_tick
    void (*update_anim)();                                // llm_strat_unit_update_anim
    void (*update_rotation)();                            // llm_strat_unit_update_rotation
    void (*target_tick)();                                // llm_strat_unit_target_tick
    void (*set_state)(uint16_t new_state);                // llm_strat_unit_set_state
    void (*apply_damage)();                               // llm_strat_unit_apply_damage
};

const unit_tick_calls &live_unit_tick_calls();

namespace detail {

// llm_strat_unit_tick @0x0047c73a.
//
// ENTRY: budget = GAME_CLOCK - cur_unit.activity_clock. Ordered `<= 0.0` (matches the assembly's
// FCOM/JNC exactly, see the .cpp): if so, budget is clamped to 0.0; otherwise cur_unit.activity_clock
// is refreshed to GAME_CLOCK. Both budget and cur_unit are the REAL ambient globals -- see the
// declared needs above -- because later ORIGINAL siblings (called for real, per policy) read them
// ambiently too.
//
// SOLDIERS: if cfg Unit[cur_unit.unit_proto_id].soldier_count == 0, the weapon-reload delta is
// budget unscaled. Otherwise llm_strat_unit_update_soldiers() runs first and the delta is
// soldier_count * budget, with BOTH unit_proto_id and soldier_count RE-READ after the call (matching
// the assembly's re-fetch rather than a cached value -- see the .cpp's uncertainty note on why).
//
// WEAPONS: up to UNIT_WEAPON_SLOTS(4) slots, stopping at the first slot whose weapon_id is 0;
// enabled_2 gates the actual llm_strat_unit_weapon_reload_tick(slot, delta, GAME_CLOCK) call per
// slot (see the .cpp for why the bound-vs-id check order was safely reassembled).
//
// ANIM/ROTATION/TARGET: dmg_smoke_level != 0 gates update_anim(); update_rotation() always runs;
// target2_ref != 0 gates target_tick().
//
// STATE-MACHINE LOOP: while budget's "not ordered <=0.0" (see the .cpp -- this is the OTHER FP
// idiom, continues on a NaN budget too), with _G_LLM_STRAT_SIM_ACTIVE gating a runaway-state guard:
// the guard counts iterations, and on the iteration where its PRE-increment value exceeds 10000, the
// current state is forced -- HOVER_DISENGAGE(0x37) just zeroes the budget (ending the loop next
// check), any other state calls llm_strat_unit_set_state(REMOVE_SILENT=3) -- and the guard resets to
// 0 either way. Every iteration then dispatches _G_LLM_STRAT_UNIT_STATE_FUNCS[cur_unit.state]()
// UNCONDITIONALLY (even the iteration that just forced a new state), exactly as the assembly does --
// the forced state takes effect on the NEXT dispatch, not this one.
//
// EXIT: if cur_unit.pending_damage's "not ordered <=0.0" (same NaN-inclusive idiom), calls
// llm_strat_unit_apply_damage().
void unit_tick(const sim_view &v, sim_store &own, const unit_tick_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_tick_calls(). Matches the original's
// committed __watcall(void) shape (sig_llm_strat_unit_tick) -- no parameters.
void unit_tick();

namespace detail {
} // namespace detail

// `[promote] unit_tick=1` REPLACES the original entry outright with our body -- there is no per-call
// A/B, so nothing to under-declare; the oracle is the per-step state-hash trajectory of an
// unpromoted golden vs. this promoted run (--soak-golden). This is G13's answer for a dispatcher.
// `default_on` is the ship default, PASSED IN (a reimplementation TU may not include a seams header
// for SHIP_PROMOTE_*), same arrangement as install_promotion_dispatch. Returns 1 if it installed.
int install_promotion_unit_tick(int default_on);

} // namespace mh::sim
