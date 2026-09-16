//
// sim/sim_step.h -- the strategic simulation's DOMAIN ROOT (RI-SIM). One function:
// llm_strat_sim_step @0x0043f512 (0x90c bytes), __watcall(void) -- no explicit parameters. This is
// the fixed-timestep pump every other sim/AI tick function in the whole migration set is reached
// FROM: two head gates (AI tick, order-queue dispatch), a per-player loop doing stat-rollover
// bookkeeping + two sub-tick catch-up loops + a per-second production-ready scan + the per-unit and
// per-building tick loops, then two whole-pool walks (projectiles, fx-anims) OUTSIDE the per-player
// loop. docs/architecture.md / the sim closure measure its write closure at 946 functions naive /
// 686 cut -- by a wide margin the largest single function in this migration.
//
// THIS FILE DOES NOT ARM A SHADOW SITE. Per the task brief for this unit: a per-call snapshot/restore
// oracle is very likely the wrong shape for the domain ROOT specifically (the same class of problem
// the promoted-golden-A/B pattern exists for on ordinary dispatchers, except the closure here is far
// larger). `install_shadow_sim_step` below is a STUB that compiles and does nothing -- the conductor
// designs the real oracle strategy (promotion + golden-trajectory comparison, most likely, matching
// sim_unit_tick.cpp's own G13 answer) separately.
//
// ---- DECLARED NEEDS -- this TU will not compile until the conductor lands them, same posture as
// sim_unit_tick.cpp's own declared-needs block. Full derivation is in the .cpp; summarized here:
//
//   (1) sim_view::ai_enabled        (const int32_t*)  -- _G_LLM_STRAT_AI_ENABLED @0x005d01a0. The
//       address+region ALREADY EXIST (RID_STRAT_AI_ENABLED, region 54, mh_regions.gen.h) and are
//       already bound into ai_store::ai_enabled (ai_state.cpp) -- this is a second, independent
//       mh::sim view binding of the SAME region, matching this file's own established pattern
//       (profiles/pop_stats/storage_stats/etc. all have AI-domain siblings already).
//
//   (2) sim_view::game_time_delta   (const double*)   -- mh::addr::GAME_TIME_DELTA @0x00e587d1. The
//       address+region ALREADY EXIST (RID_GAME_TIME_DELTA, MF_VIEW|MF_MEASURED) and are already bound
//       mutably in mh::lockstep::turn_engine -- this is a second, independent, READ-ONLY mh::sim
//       binding of the SAME region (this function only ever READS it, to forward as the AI tick's dt
//       argument).
//
//   (3) sim_store::set_cur_player(uint16_t)   -- _G_LLM_STRAT_CUR_PLAYER @0x00e58144. sim_view has a
//       READ-ONLY `cur_player` pointer already (a live pointer to the scalar's real memory, so reads
//       through it are never stale); sim_store has NO mutator at all. This function is the ONE place
//       in the whole migration set that WRITES this global (the per-player loop's own index) -- every
//       other function only reads it ambiently.
//
//   (4) sim_store::set_cur_index(uint16_t)    -- _G_LLM_STRAT_CUR_INDEX @0x00e58142. Same shape as
//       (3); sim_view's `cur_index` is read-only, sim_store has no mutator. Written five separate
//       times per player (once per pointer-driven sub-loop below).
//
//   (5)-(8) sim_store::set_cur_unit_ptr(unit*) / set_cur_building_ptr(building*) /
//       set_cur_projectile_ptr(projectile*) / set_cur_fx_anim_ptr(fx_anim*) -- the four ambient
//       POINTER-VALUED globals (_G_LLM_STRAT_CUR_UNIT/_CUR_BUILDING/_CUR_PROJECTILE/_CUR_FX_ANIM).
//       CRITICAL DISTINCTION from the EXISTING cur_unit()/cur_building()/cur_projectile()/
//       cur_fx_anim() accessors: those are bound by DEREFERENCING the ambient pointer ONCE at
//       state()-bind time, capturing a VALUE snapshot -- correct for every OTHER function in the
//       migration set, which is a CONSUMER of "whichever record the driver already selected". This
//       function is the DRIVER: it must WRITE A NEW POINTER VALUE INTO THE REAL AMBIENT MEMORY SLOT
//       itself, repeatedly, so that the ORIGINAL callees it invokes via mh::call:: (llm_strat_unit_
//       tick(), llm_strat_building_tick(), llm_strat_projectile_tick(), llm_strat_fx_anim_tick() --
//       all void(void), all reading their subject ambiently) observe the record THIS function just
//       selected. No existing accessor supports writing the SLOT's address; only its pointed-to
//       record. Proposed shape: each setter writes straight through to the real memory address
//       (`*mh::state::ptr<T*>(RID_...) = p`) and needs no local-mirror bookkeeping, because this TU
//       never reads back through the existing dereferencing accessors (cur_unit()/cur_building()/…)
//       for its OWN logic -- it always addresses the roster directly via the existing per-index
//       accessors (unit_at/building_at/projectile_pool_at/fx_anim_pool_at) and only uses the new
//       setters to keep the ambient slot correct for the ORIGINAL callees. (If a future TU needs to
//       read through cur_unit()/cur_building() in the SAME sim_state instance as a call to one of
//       these setters, that local-mirror question would need revisiting then -- not needed here.)
//
//   (9) sim_store: a MUTABLE accessor for _G_LLM_STRAT_STORAGE_STATS[player] -- sim_state.h currently
//       binds `storage_stats` READ-ONLY only ("no writer in the closure, so no sim_store sibling").
//       This function is that writer: it latches `cap_prev[i] = cap_accum[i]` for i in
//       0..PLAYER_RESOURCE_SLOTS, zeroes `cap_accum[i]` at tick end, and advances `subtick_b_clock`
//       by SUBTICK_B_PERIOD_POS every sub-tick-B iteration. Suggested shape: `storage_stats
//       &storage_stats_at(uint32_t player)`, same one-record-by-reference contract as every other
//       per-player accessor in the class.
//
//   (10) Seven NEW boot-constant doubles, contiguous in memory (0x00500a30..0x00500a60, 8 bytes
//        apart, i.e. one table), none written anywhere in this function's disassembly (checked every
//        FLD/FADD/FCOMP site against every FSTP site in the .asm -- no store to any of the seven) and
//        absent from mh_addrs.gen.h / mh_regions.gen.h entirely (grepped, zero hits) -- genuinely new,
//        not a rebinding of something that already exists:
//          SUBTICK_A_PERIOD      @0x00500a30 -- sub-tick A's catch-up threshold (FCOMP against the
//                                  running GAME_CLOCK-subtick_a_clock delta).
//          SUBTICK_A_PERIOD_POS  @0x00500a38 -- added to pop_stats.subtick_a_clock each catch-up pass.
//          SUBTICK_A_PERIOD_NEG  @0x00500a40 -- added to the running delta each pass (i.e. the
//                                  same magnitude, negated, so the local decrements toward the
//                                  threshold -- NOT independently confirmed to be exactly
//                                  -SUBTICK_A_PERIOD; transcribed as its own opaque runtime double
//                                  per the no-literal-value-assumption rule).
//          SUBTICK_B_PERIOD      @0x00500a48 -- sub-tick B's catch-up threshold.
//          SUBTICK_B_PERIOD_POS  @0x00500a50 -- added to storage_stats.subtick_b_clock each pass.
//          SUBTICK_B_PERIOD_NEG  @0x00500a58 -- added to the running delta each pass.
//          PROD_CHECK_PERIOD_NEG @0x00500a60 -- added to the running delta in the per-second
//                                  production-ready scan (the threshold itself is the LITERAL FLD1
//                                  immediate 1.0 baked into the instruction stream, not a global --
//                                  only the decrement constant is a global read).
//        All seven need an mh_addrs.gen.h entry, a region/RID, and a `const double *` sim_view member
//        each (OWN_READONLY, MF_VIEW, no writer -- same posture as this file's SIM1E-slice sibling
//        constants like bldg_main_base_damage_mult).
//
// NOT declared needs (checked against sim_state.h/mh_addrs.gen.h before writing this file, listed so
// the conductor does not re-derive them):
//   * sim_view::sim_active already exists (const int32_t*) -- reused read-only, per the task brief.
//   * sim_view::prod_shuttle_slots, sim_view::cfg_buildings, sim_view::profiles, sim_view::game_clock
//     all already exist read-only and are sufficient as-is.
//   * sim_store::population_at / unit_housing_at / power_stats_at / profile_at / unit_at /
//     building_at / projectile_pool_at / fx_anim_pool_at / change_flag / change_flag2 / order_count
//     all already exist with the exact shapes this function needs.
//   * the player_profile status_flags ALIVE bit (0x2) already has a hand-verified precedent
//     (mh_structs.gen.h's own field comment cites THIS function's TEST at 0x43f580 by address) --
//     declared locally in the .cpp as another independent copy, matching
//     sim_combat_credit_planet_conquest_kills.h's STRAT_PLAYER_STATUS_ALIVE precedent exactly, not a
//     new discovery.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Every callee is either an ALREADY-TRANSLATED-AND-VERIFIED original (power_recompute), a
// not-yet-translated original (check_population_change, check_storage_overflow), an AI-domain
// function migrated separately (ai_players_tick), or a promoted dispatcher whose entry point may
// already be OUR code (order_queue_dispatch, unit_tick, building_tick) -- ALL are reached the same
// way, through mh::call:: against the ORIGINAL address, per this codebase's standing policy that
// mh::call:: is location/promotion-agnostic (see sim_unit_tick.h's identical framing). None of this
// function's siblings are called directly as detail:: bodies, regardless of their own migration
// status.
struct sim_step_calls {
    void (*ai_players_tick)(double dt);                                               // llm_strat_ai_players_tick
    void (*order_queue_dispatch)();                                                   // llm_strat_order_queue_dispatch
    void (*power_recompute)(uint16_t player);                                         // llm_strat_power_recompute
    void (*check_population_change)(uint32_t player);                                 // llm_strat_check_population_change
    void (*storage_purge_dead_docked)(int32_t player, int32_t storage_sub_id);        // llm_strat_storage_purge_dead_docked
    void (*check_storage_overflow)(int32_t player);                                   // llm_strat_check_storage_overflow
    void (*production_complete)(uint32_t player, uint32_t slot, double elapsed_time); // llm_strat_production_complete
    void (*unit_tick)();                                                              // llm_strat_unit_tick
    void (*building_tick)();                                                          // llm_strat_building_tick
    void (*projectile_tick)();                                                        // llm_strat_projectile_tick
    void (*fx_anim_tick)();                                                           // llm_strat_fx_anim_tick
    uint32_t (*set_event)(uint32_t type);                                             // game_SetEvent
};

const sim_step_calls &live_sim_step_calls();

namespace detail {

// llm_strat_sim_step @0x0043f512. See the .cpp for the full section-by-section derivation; the
// PLATE already on the exported .c draft is an exhaustive, verified-correct summary of every phase
// and is the reading aid to use alongside the .asm.
void sim_step(const sim_view &v, sim_store &own, const sim_step_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_sim_step_calls(). Matches the original's
// committed __watcall(void) shape -- no parameters.
void sim_step();

// The generated __watcall(void) entry thunk for a PROMOTED llm_strat_sim_step, for the determinism
// harness's detour to fall through to (C6, harness-mediated -- the harness owns the entry, so an
// export install is refused). reimpl_probe reads [promote] sim_step and hands this to
// MH_Harness_RebindSimStep. Taking its address installs nothing. Mirrors mh::lockstep::sim_tick_entry_thunk().
void *sim_step_entry_thunk();

// ---- ROOTS-LIVE (2026-09-04): the SHIP-CONFIG route, and the one-owner guard that bounds it -------
//
// The root used to be promoted ONLY by the rebind above, so a run with no harness armed executed the
// ORIGINAL sim_step and the 526 rows reachable only through it were inert -- G106's cause, not a
// second instance of it. The oracle that stands in for the harness trajectory in a ship-config run is
// The reimplementation plan section 3 ("The ship-config oracle"): a composition of banked equivalence, the
// UI suite as the only unpinned real-clock exercise, the determinism runs, force-armed tombstones and
// an ENUMERATED clock/RNG residual. Read that before changing the default.
//
// TWO ROUTES, EXACTLY ONE OWNER. reimpl_probe tries the rebind first (the harness detour must keep the
// entry -- it carries the per-step hash that IS the trajectory oracle) and falls back to the direct
// entry patch only when nothing owns the entry. Both mark the same flag, so the second route refuses
// rather than double-installing; hook/promoted.h's entry_owner_of is the backstop underneath.
int install_promotion_sim_step_direct();   // the fallback route; 1 if ours now owns the entry
int register_promotion_sim_step_rebound(); // the rebind route's bookkeeping half; call only AFTER it took

// Non-vacuity accessors. `sim_step_served_calls()` is the served count the run reports: a ship-config
// run has no harness stop step, so the count is emitted as a milestone ladder from the promoted body
// itself (#1/#100/#1000/#10000/#100000) and read back here by the arming report. A promoted root at 0
// calls leaves the game running the original while every gate reads clean -- the most convincing wrong
// answer this instrument can give, and the reason the count is reported rather than assumed.
// (SERVED_calls, not `sim_step_calls`: that name is already the outward-call struct above.)
bool sim_step_promoted();
long sim_step_served_calls();
void sim_step_promotion_reset_for_test(); // TEST-ONLY; the guard has no un-promote in production

// Chain an instrument onto the promoted root: `fn` runs at the top of every served call, before the
// body. PROMOTING THIS ROOT TOOK A HOOK AWAY FROM AN INSTRUMENT THAT SHIPS ON, and this gives it back
// -- the D21 desync sampler's only sampling point in a no-harness run is llm_strat_sim_step's entry
// (`[desync] enabled` defaults to 1), and a direct entry install makes that entry ours, so its
// exclusive trampoline is refused. Measured 2026-09-04 before this existed: the detector armed,
// printed its cost probe, then sampled nothing all run while blaming a harness that was not present.
// Returns 0 -- and installs nothing -- when the root is not promoted this run (install a trampoline
// instead) or when a hook is already set (two instruments in one slot is not a thing this permits).
int set_sim_step_pre_hook(void (*fn)());

namespace detail {
} // namespace detail

} // namespace mh::sim
