//
// sim/sim_unit_tick.cpp -- see sim_unit_tick.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_tick_0047c73a.asm), not from Ghidra's C draft: the draft's brace nesting
// is structurally right on the two outer if/else pairs but silently gets the WHILE LOOP's and the
// FINAL apply_damage guard's floating-point boundary backwards for a NaN input (`0.0 < x` where the
// assembly's JNC target is really `!(x <= 0.0)` -- see the FP section below), so both were re-derived
// from the raw FCOM/FNSTSW/SAHF/Jcc sequence rather than trusted from the decompile.
//
// ---- DECLARED NEEDS: none of this function's ambient state has a home in sim_state.h yet ---------
//
// Unlike sim_unit_passive_engage.cpp's single missing accessor, THIS function's entire subject matter
// -- "which unit is being ticked" and its scratch globals -- is absent from both sim_view and
// sim_store. Per the translator brief ("stop and declare it -- do not work around it with an
// offset"), this TU references all four as if they already existed; it will not link until the
// conductor adds them. None of the four addresses appears anywhere under src/mh_dll/mh/addr/ today
// (checked by grep against mh_addrs.gen.h / mh_regions.gen.h) -- they need BOTH a raw address entry
// and a region/RID, in addition to the sim_state.h accessor.
//
//   (1) sim_store::cur_unit() -> unit&
//       _G_LLM_STRAT_CUR_UNIT @0x00e162e0. This is NOT a fixed array base like `units` -- the four
//       bytes AT that address hold a `unit *` VALUE that changes as the (untranslated, outside this
//       manifest) per-unit dispatch loop advances, so it needs the SAME "resolve once at state()-bind
//       time" treatment as every other sim_state.h member, but with ONE EXTRA DEREFERENCE: the
//       binder must read the pointer value stored at the region's address (not just take the
//       region's address itself) to learn which units[player][index] slot this call concerns.
//       Suggested shape, matching the other private-pointer store members:
//         sim_state.h, on sim_store:      unit &cur_unit() { return *cur_unit_; }
//                                         (private) unit *cur_unit_;
//         sim_state.cpp's state():        unit *cur_unit_ = *mh::state::ptr<unit*>(RID_STRAT_CUR_UNIT);
//                                         (passed into the sim_store constructor)
//       Read AND written here (activity_clock is the one field this function itself writes; the
//       other fields -- unit_proto_id, state, weapons[], dmg_smoke_level, target2_ref,
//       pending_damage -- are read-only to this function but mutated by the ORIGINAL siblings it
//       calls for real, so a plain mutable reference into live memory is required, not a snapshot).
//
//   (2) sim_store::tick_budget() -> double&
//       _G_LLM_STRAT_TICK_BUDGET @0x00ae3738 (immediately followed in memory by
//       _G_LLM_STRAT_MOVE_MICROSTEPS @0x00ae3740, per mh_addrs.gen.h's own note on that symbol --
//       confirms the address). A plain scalar global, same shape as sim_store::lockstep_horizon().
//       Read AND written repeatedly by this function; also read ambiently by the ORIGINAL
//       update_soldiers/weapon_reload_tick/etc. siblings this function calls (the struct-field
//       comment on unit::activity_clock in mh_structs.gen.h documents TICK_BUDGET's formula as
//       shared vocabulary), which is why it must bind to the REAL address, not a local variable.
//
//   (3) sim_store::state_loop_guard() -> int32_t&
//       _G_LLM_STRAT_STATE_LOOP_GUARD @0x00e15a38. Plain scalar global, same shape as (2). Appears
//       (from this function alone) to be private scratch to this one function's dispatch loop.
//
//   (4) sim_view::unit_state_funcs -> const unit_state_fn*  (new alias `unit_state_fn = void (*)()`,
//       suggested to live in sim_state.h alongside the record-type aliases)
//       _G_LLM_STRAT_UNIT_STATE_FUNCS @0x00e15a3c -- the ORIGINAL per-state handler jump table
//       (immediately after STATE_LOOP_GUARD in memory, consistent with the asm's back-to-back
//       0x00e15a38/0x00e15a3c pair). Read-only array of function pointers; dispatched by
//       `v.unit_state_funcs[cur_unit.state]()` exactly as the assembly's
//       `CALL dword ptr [EAX*4 + table]` does. Per the task hazard note, the individual handlers are
//       NOT resolved, named, or inlined here -- this is a genuine indirect call through ORIGINAL
//       code, same category as an mh::call:: site but keyed by runtime state rather than a fixed
//       address.
//
// ---- THE STATE-DOMAIN ENUM (translator brief 17a) --------------------------------------------
//
// state/order are `llm_strat_unit_state` per mh_structs.gen.h's field comments, and the
// 2026-07-04 session records that enum being built (45 members, including HOVER_DISENGAGE=0x37) -- but
// sim_unit_state_predicates.h/.cpp, translated earlier in this same SIM1A slice, investigated the
// SAME field and found "not backed by any enum in this tree" (Ghidra prints symbolic names in its
// OWN decompile without a real Data Type Manager enum behind them) and used literal values with
// trailing comments instead, filing the same declared_needs this file repeats below. Followed here
// for consistency with that established precedent rather than re-litigated: HOVER_DISENGAGE = 0x37,
// REMOVE_SILENT = 3 (both read directly off this function's own CMP/MOV immediates at
// 0x0047c8a0/0x0047c8bd, matching the task brief's naming).
//
#include "sim/rng_trace.h" // C-prime level 3: the handler-selection note
#include "sim/sim_unit_tick.h"
#include "sim/sim_register_state_handlers.h" // note_first_dispatch -- the one-time
                                             // "whose handler did the tick call" line


#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_export.gen.h" // MH_EXPORT_REPLACE -- the entry thunk that REPLACES the original
                                // when promoted ([promote] unit_tick=1); the G13 dispatcher oracle
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>            // GetPrivateProfileIntA -- the [promote] gate, same as mh::orders / sim_dispatch
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_tick_calls &live_unit_tick_calls() {
    static const unit_tick_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_update_soldiers),
        MH_LIBMH_BIND(llm_strat_unit_weapon_reload_tick),
        MH_LIBMH_BIND(llm_strat_unit_update_anim),
        MH_LIBMH_BIND(llm_strat_unit_update_rotation),
        MH_LIBMH_BIND(llm_strat_unit_target_tick),
        MH_LIBMH_BIND(llm_strat_unit_set_state),
        MH_LIBMH_BIND(llm_strat_unit_apply_damage),
    };
    return c;
}

namespace detail {

void unit_tick(const sim_view &v, sim_store &own, const unit_tick_calls &c) {
    // ---- the current unit, and the tick budget -- BOTH are the real ambient globals; see the
    // declared-needs block above for why a plain reference (not a snapshot) is required: later
    // ORIGINAL siblings called below mutate this same unit record and this same TICK_BUDGET address
    // for real, and this function must observe those writes exactly as the original CALL sequence
    // would. ----
    unit   &u      = own.cur_unit();
    double &budget = own.tick_budget();

    // ---- ENTRY (0x0047c752-0x0047c797): budget = GAME_CLOCK - activity_clock. ------------------
    // ORDERED `<= 0.0` here matches the assembly exactly (FCOM computes 0.0 vs budget; JNC to the
    // zero-out branch fires iff C0=0, which happens ONLY for the ordered "0.0 >= budget" case --
    // never for a NaN budget, whose unordered result sets C0=1 same as the ordered-greater case, so
    // JNC is NOT taken and execution falls to the "budget > 0" arm). C++'s own `<=` is already
    // ordered (false on NaN), so this is a plain, un-idiomed comparison -- unlike the two below.
    budget = *v.game_clock - u.activity_clock;
    if (budget <= 0.0) {
        budget = 0.0;
    } else {
        u.activity_clock = *v.game_clock;
    }

    // ---- SOLDIERS (0x0047c797-0x0047c7d2): scale the weapon-reload delta by cfg Unit.soldier_count
    // when the unit's type carries soldiers. Both unit_proto_id and soldier_count are RE-READ after
    // the update_soldiers() call rather than cached from before it (the assembly reloads CUR_UNIT
    // and re-indexes the cfg table fresh at 0x0047c7b4-0x0047c7c3, rather than reusing the values it
    // already had in registers) -- reproduced here by simply not hoisting the second read out of the
    // branch. See uncertainties: whether update_soldiers can actually change which unit CUR_UNIT
    // points at (and so change what `u` should refer to) could not be established from this function
    // alone -- it is an ORIGINAL callee outside this batch. ----
    double delta;
    if (v.cfg_units[u.unit_proto_id].soldier_count == 0) {
        delta = budget;
    } else {
        c.update_soldiers();
        delta = (double)v.cfg_units[u.unit_proto_id].soldier_count * budget;
    }

    // ---- WEAPONS (0x0047c7e4-0x0047c83d): up to UNIT_WEAPON_SLOTS(4) slots. The assembly checks
    // weapon_id!=0 BEFORE checking slot<4 (so on a hypothetical 5th pass, with all four real slots
    // occupied, it would read weapons[4].weapon_id -- one byte INTO unit::_pad_0x83, the single pad
    // byte mh_structs.gen.h places right after the 4-slot array -- before the bounds check also
    // fires and breaks the loop regardless of that byte's value). Reassembled here into the
    // short-circuited natural order (slot<4 first) because the two orders are provably
    // observationally identical: at slot==4 EITHER check alone already breaks the loop, the OOB read
    // has no side effect, and nothing computed from it survives -- see uncertainties. ----
    for (int32_t slot = 0; slot < UNIT_WEAPON_SLOTS && u.weapons[slot].weapon_id != 0; ++slot) {
        if (u.weapons[slot].enabled_2 != 0) {
            c.weapon_reload_tick((uint8_t)slot, delta, *v.game_clock);
        }
    }

    // ---- ANIM / ROTATION / TARGET (0x0047c83d-0x0047c869). ---------------------------------------
    if (u.dmg_smoke_level != 0) {
        c.update_anim();
    }
    c.update_rotation();
    if (u.target2_ref != 0) {
        c.target_tick();
    }

    // ---- STATE-MACHINE DISPATCH LOOP (0x0047c869-0x0047c8e5). ------------------------------------
    //
    // Loop condition is the OTHER FP idiom (translator-brief-documented, same shape
    // sim_unit_passive_engage.cpp and sim_unit_state_predicates.cpp already use for this exact
    // FCOM/JNC pattern): JNC exits the loop iff C0=0, i.e. iff ORDERED budget<=0.0 -- so the loop
    // BODY runs (does not exit) whenever budget>0.0 OR budget is NaN. `!(budget <= 0.0)` reproduces
    // that NaN-inclusive continuation exactly; the naive `0.0 < budget` would NOT (it is false, and
    // so would incorrectly exit, on a NaN budget) -- see uncertainties.
    //
    // THE RUNAWAY-STATE GUARD: gated on _G_LLM_STRAT_SIM_ACTIVE. The guard is read into `old_guard`
    // and incremented UNCONDITIONALLY once sim_active is nonzero (0x0047c889-0x0047c88e), and only
    // THEN is old_guard (the PRE-increment value) compared against 10000 -- so the forcing branch
    // fires on the iteration where the guard was ALREADY over 10000 before this increment, i.e. the
    // 10002nd such iteration, not the 10001st. HOVER_DISENGAGE(0x37) just zeroes the budget (ending
    // the loop on the NEXT top-of-loop check, since the dispatch below still runs this iteration);
    // any other state calls llm_strat_unit_set_state(REMOVE_SILENT=3). Both arms reset the guard to
    // 0 (0x0047c8c7), and so does sim_active being zero taking the guard-check branch out of the path
    // entirely (JZ 0x0047c887 skips straight to dispatch without touching the guard at all -- i.e.
    // the guard is NEITHER incremented NOR reset while sim_active==0; only reproduced here by simply
    // not touching it in that arm).
    //
    // DISPATCH: _G_LLM_STRAT_UNIT_STATE_FUNCS[cur_unit.state]() runs on EVERY iteration, including
    // the one that just forced a new state -- the forced state only takes effect on the dispatch
    // NEXT time around the loop, exactly as the assembly's straight-line fallthrough into the CALL
    // (0x0047c8d1) does regardless of which path reached it.
    own.state_loop_guard() = 0;
    while (!(budget <= 0.0)) {
        if (*v.sim_active != 0) {
            int32_t old_guard      = own.state_loop_guard();
            own.state_loop_guard() = old_guard + 1;
            if (old_guard > 10000) {
                if (u.state == 0x37 /* HOVER_DISENGAGE, llm_strat_unit_state -- see the header note */) {
                    budget = 0.0;
                } else {
                    c.set_state(3 /* REMOVE_SILENT, llm_strat_unit_state -- see the header note */);
                }
                own.state_loop_guard() = 0;
            }
        }
        // SIM1-DISPATCH (2026-08-22): one line, once per run, saying whether the entry this tick is
        // about to call is OURS or the game's. The table is filled by mh::sim::
        // register_state_handlers when `[promote] state_handlers=1`, and "we filled it" is an
        // install-time claim about a DIFFERENT function -- this is the only place that can report
        // what the tick actually dispatches into. Early-outs after the first call; not a trace.
        mh::sim::note_first_dispatch("unit", reinterpret_cast<const void *>(v.unit_state_funcs[u.state]));
        // C-prime level 3, tag 3: the unit's state AT HANDLER SELECTION -- the instant that decides
        // which body runs. This is the fork the draw trace pointed at: `units` is byte-identical at
        // the START of the step, so if the two arms disagree HERE something rewrote the state between
        // the step boundary and this line, and if they agree here the handler itself diverged on its
        // own inputs. Logging the handler ADDRESS too would be useless across arms (different
        // modules), so the state index -- which is the table subscript -- is what gets compared.
        mh::sim::rng_trace_add_note(3u, (uint32_t)*v.cur_player << 16 | (uint32_t)*v.cur_index,
                                    (uint32_t)u.state, (uint32_t)u.order,
                                    (uint32_t)u.order_queued << 16 | (uint32_t)u.move_microstep,
                                    (uint32_t)own.state_loop_guard(),
                                    // heading AND TILE. The tile is here because leaving it out once
                                    // already cost a round: deploy_approach derives its footprint
                                    // corner from (u.x, u.y, u.move_heading), so a note that showed
                                    // only the heading could report "identical inputs" for two calls
                                    // that were looking at different squares of the map.
                                    (uint32_t)u.move_heading << 16 | (uint32_t)u.x << 8 |
                                        (uint32_t)u.y);
        v.unit_state_funcs[u.state]();
    }

    // ---- EXIT (0x0047c8e5-0x0047c8f9): pending-damage application. Same NaN-inclusive idiom as the
    // loop condition above (JNC to the skip target at 0x0047c8f9 fires iff ORDERED pending_damage<=
    // 0.0; the call fires on pending_damage>0.0 OR NaN) -- see uncertainties. ----
    if (!(u.pending_damage <= 0.0)) {
        c.apply_damage();
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_tick() {
    sim_state st = state();
    detail::unit_tick(st.read, st.own, live_unit_tick_calls());
}


// ---- the PROMOTION arm (G13's answer for this dispatcher) -----------------------------------------
//
// Under `[promote] unit_tick=1` the original entry is overwritten with a JMP here, so every real call
// to llm_strat_unit_tick runs OUR body -- no per-call snapshot/restore, and therefore none of the
// region leakage the shadow site could never scope -- its closure is the whole sim. The seven
// callees + the state-func-table dispatch stay ORIGINAL, so this verifies OUR tick
// LOGIC in composition against the original, over thousands of steps, via the per-step state-hash
// trajectory (--soak-golden), which is the only oracle a dispatcher admits.
// EVERYTHING here is anonymous-namespace INTERNAL linkage on purpose: mh::sim::promoted_arm::active /
// mark_installed already exist with EXTERNAL linkage in sim_order_dispatch.cpp, so a second external
// pair would be an ODR collision at link. `mh::sim::promoted_arm::unit_tick` stays referable by the
// MH_EXPORT_REPLACE macro below (the anonymous namespace is transparent to lookup within this TU).
namespace promoted_arm {
namespace {
bool          g_installed = false;
volatile long g_calls     = 0;

void unit_tick() {
    // NON-VACUITY (gates can pass vacuously): a golden that "stayed identical" proves
    // nothing unless our body actually ran. Log the first call and widening milestones so the run can
    // state, in writing, that the promoted entry was reached.
    const long c = ++g_calls;
    if (c == 1 || c == 1000 || c == 100000)
        mh::ai::ai_say("; [promote] unit_tick body served call #%ld from game code\n", c);
    sim_state st = state();
    detail::unit_tick(st.read, st.own, live_unit_tick_calls());
}

bool tick_promoted() { return g_installed; }
void mark_tick_installed(bool on) { g_installed = on; }
} // namespace
} // namespace promoted_arm

} // namespace mh::sim


MH_EXPORT_REPLACE(llm_strat_unit_tick, mh::sim::promoted_arm::unit_tick)

namespace mh::sim {

// `[promote] unit_tick=1`. `default_on` is the ship default (SHIP_PROMOTE_UNIT_TICK), passed in
// rather than read from a seams header here -- a reimplementation TU may not include one (the
// layering lint). Same arrangement as install_promotion_dispatch.
int install_promotion_unit_tick(int default_on) {
    if (default_on == 0) return 0;
    if (!mh_export_install_llm_strat_unit_tick()) {
        // The generated installer already logged WHY (entry-byte guard mismatch -> wrong-image build).
        // Refuse loudly: a half-promoted run means nothing.
        mh::ai::ai_say("; [promote] unit_tick REFUSED -- entry guard mismatch, NOT promoted\n");
        return 0;
    }
    promoted_arm::mark_tick_installed(true);
    mh::ai::ai_say("; [promote] unit_tick: llm_strat_unit_tick is LIVE -- ours IS the function, there "
                   "is no original arm in this run\n");
    return 1;
}

} // namespace mh::sim
