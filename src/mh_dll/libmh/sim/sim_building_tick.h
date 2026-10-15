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

// patching:PT-FIX1 -- skip the turret reload tick for a building that does not own its turret slot
// (turrets[player][sub_id].b_index != its own index): a destroyed turret's rubble whose slot a new
// turret reused. Default false = the faithful retail rule; set from reimpl_fixes::turret_reload_owner_fix
// by lockstep::set_fixes, so [net] turret_reload_owner_fix is the control.
void set_turret_reload_owner_fix(bool on);
bool turret_reload_owner_fix();

// hotfix rc9.4 -- `[net] bldg_pip_guard` (field crashes 0x0047908b / 0x004793e1, triage
// tmp/report_triage_2026-10-15/crash_7908b.md). Retail indexes the 4-entry fire-animation table A_OGIEN
// (0x00c3873c + level*4) with a building's pip_level[slot] in llm_strat_bldg_update_charge_pips and
// llm_strat_bldg_tick_pip_anim, with no range check. Legal: every pip_level[i] in 0..4 and pip_active_count in
// 0..16 (pip_slot_count <= 4 slots x 4 levels). A record holding anything else (source not identified; suspected
// a planet/save restore) faults on the next damage or animation chain restart. With the key on, building_tick
// (before the pip-anim gate, = the 0x0046fe67 splice) and update_charge_pips (on entry, = the 0x00478ecb splice)
// zero the pip state exactly as map_CreateBuilding does (frame 0, level 0, timer = GAME_CLOCK, active 0) and log
// one line. SIM-AFFECTING only when the state is already illegal, the same on every peer. Default false = retail.
// Set from reimpl_fixes::bldg_pip_guard by lockstep::set_fixes.
inline constexpr int32_t BLDG_PIP_LEVEL_MAX  = 4;
inline constexpr int32_t BLDG_PIP_ACTIVE_MAX = 16;
void                     set_bldg_pip_guard(bool on);
bool                     bldg_pip_guard();
bool                     bldg_pip_state_is_bad(const building &b);
// When the state is illegal: zero it (map_CreateBuilding's values, timer = `now`), log one line naming `site`, and
// return true. A legal state is untouched (false). Runs regardless of the key -- callers test bldg_pip_guard().
bool bldg_pip_sanitize(const sim_view &v, building &b, uint32_t player, int32_t index, const char *site);

// patching:PT-FIX4 -- `[net] turret_clip_fix`: map_CreateBuilding's turret boot sets ammo = Weapon[weapon_id].ammo and
// resupply_cycles_remaining = -1 (retail leaves both 0 / stale), and llm_strat_bldg_turret_reload_tick refills
// when ammo <= 0 (retail: == 0), so the magazine and the long reload engage and a negative-ammo turret heals.
// Default off = retail. Set from reimpl_fixes by lockstep::set_fixes.
void set_turret_clip_fix(bool on);
bool turret_clip_fix();

// patching:PT-FIX5 -- `[net] turret_fire_poll_fix`: llm_strat_bldg_state_turret_attack charges every aim pass
// (per_shot_cost * 0.5 = 0.05 s) against the step's tick budget and fires only with budget left over, so a finished
// reload waited up to ~0.06 s for the poll. With the key on, a ready turret that is already within the aim
// tolerance skips the aim loop and fires on the step its reload completes; turning keeps its 0.05 s per heading.
// Default off = retail. Set from reimpl_fixes by lockstep::set_fixes.
void set_turret_fire_poll_fix(bool on);
bool turret_fire_poll_fix();

// patching:PT-FIX2 -- the turret reload timer drains by REAL elapsed sim time per step (GAME_TIME_DELTA =
// the step's dt, in both mode 3's fixed SIM_STEP_INTERVAL and the single-player variable-dt step) times
// `turret_fire_rate_mult`, instead of the re-counted budget GAME_CLOCK - last_tick_time that retail hands
// llm_strat_bldg_turret_reload_tick (the attack state gives an unspent budget back, so that budget runs
// dt, 2dt .. K*dt and the timer drains ~(K+1)/2 x real time at our small steps). Default off = retail.
// Set from reimpl_fixes by lockstep::set_fixes ([net] turret_reload_realtime_fix / turret_fire_rate_mult).
inline constexpr double TURRET_FIRE_RATE_MULT_MIN = 0.1;
inline constexpr double TURRET_FIRE_RATE_MULT_MAX = 10.0;
double                  clamp_turret_fire_rate_mult(double m); // NaN -> 2.0 (the shipped default); else clamped to [MIN, MAX]
void                    set_turret_reload_realtime(bool on, double mult);
bool                    turret_reload_realtime_fix();
double                  turret_fire_rate_mult();
// The drain handed to the reload tick for a step of real length `dt`: dt * mult. ONE definition, called by
// the twin AND by the retail stub (net_lockstep.cpp), so both carriers round identically.
double turret_reload_drain(double dt);

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
