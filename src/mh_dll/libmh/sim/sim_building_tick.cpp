//
// sim/sim_building_tick.cpp -- see sim_building_tick.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_building_tick_0046fd86.asm), not from Ghidra's C draft
// (tmp/decomp/llm_strat_building_tick_0046fd86.c): the draft's brace nesting is structurally right,
// but it renders TWO of the four floating-point gates as `0.0 < x`, a form that is NOT NaN-faithful
// -- both were re-derived from the raw FLDZ/FCOMP/FNSTSW/SAHF/Jcc sequence rather than trusted from
// the decompile, per the header's citation of each address range.
//
#include "sim/sim_building_tick.h"
#include "sim/sim_register_bldg_type_callbacks.h" // note_first_bldg_type_dispatch -- the same
                                                  // one-time line for the DONE table (SIM1-BLDGCB)
#include "sim/sim_register_state_handlers.h"      // note_first_dispatch -- the one-time
                                                  // "whose handler did the tick call" line


#include "addr/mh_calls.gen.h"     // typed callables for the four ORIGINAL functions we call OUT to
#include "addr/mh_export.gen.h"    // MH_EXPORT_REPLACE -- the promotion entry thunk ([promote]
                                   // building_tick=1); the G13/G19 dispatcher oracle
#include "sim/sim_order_enqueue.h" // mh::sim::BUILDING_TYPE_A_TURRET/H_TURRET -- already pinned
                                   // there (SIM1C) from a Ghidra enum dump; reused rather than
                                   // re-declared, per the translator brief's naming rule (17a)
#include "ai/ai_state.h"           // ai_say / trace_budget -- the shared trace sink, not AI state

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>            // GetPrivateProfileIntA -- the [promote] gate, same as mh::orders / sim_dispatch / unit_tick
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const building_tick_calls &live_building_tick_calls() {
    static const building_tick_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_turret_reload_tick),
        MH_LIBMH_BIND(llm_strat_bldg_tick_animation_state),
        MH_LIBMH_BIND(llm_strat_bldg_tick_pip_anim),
        MH_LIBMH_BIND(llm_strat_bldg_apply_damage),
    };
    return c;
}

namespace detail {

void building_tick(const sim_view &v, sim_store &own, const building_tick_calls &c) {
    // ---- the current building -- the real ambient global (own.cur_building()/v.cur_building are
    // both bound from the SAME resolved _G_LLM_STRAT_CUR_BUILDING pointer value; see sim_state.cpp).
    // A plain reference, not a snapshot: the four ORIGINAL callees below mutate this same building
    // record for real (turret_reload_tick/tick_animation_state/tick_pip_anim/apply_damage all run
    // outside this TU), and this function must observe those writes exactly as the original CALL
    // sequence would. ----
    building &b      = own.cur_building();
    double   &budget = own.tick_budget();

    // ---- ENTRY (0x0046fd9e-0x0046fdcd) + GATE 1 (0x0046fdb2-0x0046fdbd, JNC->0x0046fdcf). Plain
    // ordered `<=`, matching sim_unit_tick's own ENTRY gate exactly (same idiom, same instructions,
    // same conclusion: C++'s `<=` is already ordered/false-on-NaN, so no flip is needed here). ----
    budget = *v.game_clock - b.last_tick_time;
    if (budget <= 0.0) {
        budget = 0.0;
    } else {
        b.last_tick_time = *v.game_clock;
    }

    // ---- ENERGY GATE (0x0046fde3-0x0046fdff) + GATE 2 (0x0046fdf3-0x0046fdfb, JC->0x0046fdff).
    // NaN-inclusive NEGATED form -- JC fires on ordered 0.0<energy OR unordered (NaN energy), and
    // only `!(energy<=0.0)` reproduces both; Ghidra's own `0.0 < energy` decompile text is NOT
    // NaN-faithful here (see the header). The outer `built_flags != 3` guard short-circuits the
    // whole energy check for a fully operational building (0x0046fde8's JZ skips straight past). ----
    if (b.built_flags != 3 && !(b.energy <= 0.0)) {
        budget = 0.0;
    }

    // ---- TURRET TYPE CHECK (0x0046fe13-0x0046fe62). Building[building_id].type == A_TURRET(5) or
    // H_TURRET(0x19) -- cfg_buildings is boot-loaded and has no writer in this closure, so a single
    // bound read is safe (unlike `units`/`buildings`/`players`, which the const-view rule forbids
    // caching across a call). ----
    const cfg_building &cb = v.cfg_buildings[b.building_id];
    if (cb.type == BUILDING_TYPE_A_TURRET || cb.type == BUILDING_TYPE_H_TURRET) {
        // Args per the .asm push sequence (0x0046fe43-0x0046fe5d): EAX=cur_player, EDX=cur_index
        // (NOT cur_building.building_id -- mh_calls.gen.h's generated parameter name for this slot
        // is `building_id`, but the value actually passed is the tick driver's CUR_INDEX; see the
        // .h's building_tick_calls comment), stack=budget (double, TICK_BUDGET at this point).
        c.turret_reload_tick(*v.cur_player, (uint32_t)*v.cur_index, budget);
    }

    // ---- ANIMATION STATE (0x0046fe62, unconditional) + PIP ANIM (0x0046fe67-0x0046fe88, gated on
    // pip_active_count != 0). ----
    c.tick_animation_state();
    if (b.pip_active_count != 0) {
        c.tick_pip_anim(*v.cur_player, (int32_t)*v.cur_index);
    }

    // ---- STATE-MACHINE DISPATCH LOOP (0x0046fe88-0x0046fee3). GATE 3 (0x0046fe92-0x0046fe9d,
    // JNC->0x0046fee3 the loop-EXIT target): same FCOM/JNC instructions as GATE 1 (same TICK_BUDGET
    // global) but the Jcc target is the loop EXIT this time, so the shape FLIPS to the
    // NaN-inclusive negated form -- `!(budget <= 0.0)` continues the loop on budget>0.0 OR NaN,
    // exactly matching sim_unit_tick's own loop-condition gate (same underlying fact, different
    // branch target; the naive `0.0 < budget` would incorrectly exit on a NaN budget). ----
    //
    // THE RUNAWAY-STATE GUARD: gated on _G_LLM_STRAT_SIM_ACTIVE (v.sim_active). Read into
    // `old_guard`, then incremented UNCONDITIONALLY once sim_active!=0 (0x0046fea8-0x0046ead), and
    // only THEN is old_guard (the PRE-increment value) compared against 10000 -- so the forcing
    // branch fires on the iteration where the guard was ALREADY over 10000 before this increment.
    // Unlike unit_tick's HOVER_DISENGAGE special case, THIS function has no state-dependent carve-
    // out: the .asm (0x0046febf) unconditionally forces cur_building.state =
    // mh::sim::BLDG_STATE_DISMANTLE_FINISH(3) and resets the guard to 0. When sim_active==0
    // (0x0046fea6 JZ) the guard is NEITHER incremented NOR reset -- reproduced here by simply not
    // touching it in that arm. ----
    //
    // DISPATCH: v.bldg_state_funcs[cur_building.state]() runs on EVERY iteration, including the one
    // that just forced a new state -- the forced state only takes effect on the dispatch NEXT time
    // around the loop, exactly as the assembly's straight-line fallthrough into the CALL
    // (0x0046fedb) does regardless of which path reached it. Dispatched-through only; the individual
    // state handlers are NOT resolved, named, or inlined here.
    own.state_loop_guard() = 0;
    while (!(budget <= 0.0)) {
        if (*v.sim_active != 0) {
            int32_t old_guard      = own.state_loop_guard();
            own.state_loop_guard() = old_guard + 1;
            if (old_guard > 10000) {
                b.state                = mh::sim::BLDG_STATE_DISMANTLE_FINISH;
                own.state_loop_guard() = 0;
            }
        }
        // SIM1-DISPATCH (2026-08-22): the building half of the same one-time note -- see the
        // matching comment in sim_unit_tick.cpp. Two tags, two independent one-shots, because a run
        // that only ever ticks units would otherwise report the building table as covered.
        mh::sim::note_first_dispatch("bldg", reinterpret_cast<const void *>(v.bldg_state_funcs[b.state]));
        v.bldg_state_funcs[b.state]();
    }

    // ---- EXIT (0x0046fee3-0x0046fef7). GATE 4 (0x0046fee3-0x0046fef0, JNC->0x0046fef7 the
    // skip-apply_damage target): same NaN-inclusive negated shape as GATE 3 -- fires (calls
    // apply_damage) on pending_damage>0.0 OR NaN, NOT the naive `0.0 < pending_damage`. Same field
    // name / same idiom as sim_unit_tick's own EXIT gate. ----
    if (!(b.pending_damage <= 0.0)) {
        c.apply_damage();
    }

    // ---- DONE CALLBACK (0x0046fef7-0x0046ff22). Ordinary ordered (non-FP) two-CMP guard, no NaN
    // idiom involved: only when the building is fully operational (built_flags==3) AND not in the
    // RUBBLE_SIGHT_DECAY state does the per-building-id DONE table get dispatched. Dispatched-
    // through only, same posture as bldg_state_funcs above. ----
    if (b.built_flags == 3 && b.state != mh::sim::BLDG_STATE_RUBBLE_SIGHT_DECAY) {
        // SIM1-BLDGCB (2026-08-23): the DONE table's half of the one-time ownership note. A separate
        // tag from "tick2" and a separate one-shot, because a run that never reaches the animation
        // branch would otherwise report the tick2 table as covered.
        mh::sim::note_first_bldg_type_dispatch(
            "done", reinterpret_cast<const void *>(v.bldg_done_funcs[b.building_id]));
        v.bldg_done_funcs[b.building_id]();
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void building_tick() {
    sim_state st = state();
    detail::building_tick(st.read, st.own, live_building_tick_calls());
}


// ---- the PROMOTION arm (G13/G19's answer for this dispatcher) -------------------------------------
//
// Under `[promote] building_tick=1` the original entry is overwritten with a JMP here, so every real
// call to llm_strat_building_tick runs OUR body -- no per-call snapshot/restore, and therefore none
// of the region leakage a shadow site could never scope (its closure is the whole sim: G13/G19). The
// four direct callees + both dispatch-table lookups (bldg_state_funcs/bldg_done_funcs) stay
// ORIGINAL, so this verifies OUR tick LOGIC in composition against the original, over thousands of
// steps, via the per-step state-hash trajectory (--soak-golden), which is the only oracle a
// dispatcher admits. Anonymous-namespace INTERNAL linkage, same reason sim_unit_tick.cpp's own
// promoted_arm:: is: `mh::sim::promoted_arm::active`/`mark_installed` already exist with EXTERNAL
// linkage in sim_order_dispatch.cpp, so a second external pair would be an ODR collision at link.
namespace promoted_arm {
namespace {
bool          g_installed = false;
volatile long g_calls     = 0;

void building_tick() {
    // NON-VACUITY (gates can pass vacuously): log the first call and widening
    // milestones so the run can state, in writing, that the promoted entry was reached.
    const long c = ++g_calls;
    if (c == 1 || c == 1000 || c == 100000)
        mh::ai::ai_say("; [promote] building_tick body served call #%ld from game code\n", c);
    sim_state st = state();
    detail::building_tick(st.read, st.own, live_building_tick_calls());
}

bool building_tick_promoted() { return g_installed; }
void mark_building_tick_installed(bool on) { g_installed = on; }
} // namespace
} // namespace promoted_arm

} // namespace mh::sim


MH_EXPORT_REPLACE(llm_strat_building_tick, mh::sim::promoted_arm::building_tick)

namespace mh::sim {

// `[promote] building_tick=1`. `default_on` is the ship default (SHIP_PROMOTE_BUILDING_TICK), passed
// in rather than read from a seams header here -- a reimplementation TU may not include one (the
// layering lint). Same arrangement as install_promotion_unit_tick/install_promotion_dispatch.
int install_promotion_building_tick(int default_on) {
    if (default_on == 0) return 0;
    if (!mh_export_install_llm_strat_building_tick()) {
        // The generated installer already logged WHY (entry-byte guard mismatch -> wrong-image build).
        mh::ai::ai_say("; [promote] building_tick REFUSED -- entry guard mismatch, NOT promoted\n");
        return 0;
    }
    promoted_arm::mark_building_tick_installed(true);
    mh::ai::ai_say("; [promote] building_tick: llm_strat_building_tick is LIVE -- ours IS the "
                   "function, there is no original arm in this run\n");
    return 1;
}

} // namespace mh::sim
