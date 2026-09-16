//
// sim/libtrans/sim_lt_frame.cpp -- see sim_lt_frame.h. Translated from the DISASSEMBLY
// (tmp/decomp_lib_trans/llm_strat_frame_0043ecfa.asm,
// llm_strat_frame_redraw_behind_dialog_0043ed98.asm). 158 bytes between them; the value of this
// file is the ROUTING (header banner), not the logic.
//
#include "sim/libtrans/sim_lt_frame.h"


#include "addr/mh_calls.gen.h"    // the walled callee (input_update)
#include "addr/mh_export.gen.h"   // MH_EXPORT_REPLACE -- the two entry installs
#include "ai/ai_state.h"          // ai_say -- the shared trace sink, not AI state
#include "lockstep/turn_engine.h" // mh::lockstep::pump / time_tick / sim_tick -- OUR spine bodies
#include "state/host_api.h"
#include "state/host_events.h" // SIMABI-NOTIFY: the two frame-present records

namespace mh::sim {

namespace detail {
// Parseable one-line adapters for the binder below (lint_translation's pair_calls reads
// `mh::call::X` / `detail::X` tokens IN ORDER; a bare `&mh::lockstep::X` is invisible to it --
// these three carry DETAIL_CALL_ALIAS rows mapping them to the Ghidra callee names). The two
// hook defaults are no-ops: correct for any run that never promotes the frame (the bodies are
// only reachable promoted, or under the offline oracle, which replaces the whole struct).
void    lt_pump_ours() { mh::lockstep::pump(); }
int32_t lt_time_tick_ours() { return mh::lockstep::time_tick(); }
void    lt_sim_tick_ours() { mh::lockstep::sim_tick(); }
void    lt_frame_pace_hook_default() {}
void    lt_frame_harness_hook_default() {}
} // namespace detail

const lt_frame_calls &live_lt_frame_calls() {
    static lt_frame_calls c = {
        mh::host().apply_frame_input,           // WALL
        &detail::lt_pump_ours,                  // OURS -- direct (routing decision, header banner)
        &detail::lt_frame_pace_hook_default,    // set by set_lt_frame_instrument_hooks
        &detail::lt_time_tick_ours,             // OURS -- direct
        &detail::lt_frame_harness_hook_default, // set by set_lt_frame_instrument_hooks
        &detail::lt_sim_tick_ours,              // OURS -- direct
        &mh::state::evt::strat_frame_present,   // NOTIFY (SCR_STRAT_FRAME_PRESENT)
        &mh::state::evt::strat_frame_redraw,    // NOTIFY (SCR_STRAT_FRAME_REDRAW)
    };
    return c;
}

// EXPERIMENT KNOB, default OFF and never set by shipping code. The frame's first call is the WALL --
// mh::host().apply_frame_input, which hosted is the ORIGINAL llm_strat_input_update @0x00441b88 and
// standalone is an empty function (libref_host binds `{}` AND sets input_update = noop). That makes
// it the one structural difference left on the order path between the two arms of the LIB-REF
// replay, and the only honest way to ask whether it MATTERS is to remove it from the hosted arm and
// see whether the recorded trajectory still reproduces. Hence a setter rather than an #ifdef: the
// experiment has to run in a real, shipping-configured process.
void set_lt_frame_input_override(void (*input_update)()) {
    lt_frame_calls &c = const_cast<lt_frame_calls &>(live_lt_frame_calls());
    if (input_update) c.input_update = input_update;
}

void set_lt_frame_instrument_hooks(void (*pace_time_tick)(), void (*harness_sim_tick)()) {
    lt_frame_calls &c = const_cast<lt_frame_calls &>(live_lt_frame_calls());
    if (pace_time_tick) c.pace_time_tick = pace_time_tick;
    if (harness_sim_tick) c.harness_sim_tick = harness_sim_tick;
}

namespace detail {

void frame(const sim_view &v, sim_store &own, const lt_frame_calls &c) {
    c.input_update();                 // 0x0043ed12 -- unconditional, BEFORE the mode gate
    if (own.game_mode() == 6) return; // 0x0043ed17 JZ out -- tactical mission owns the frame
    if (*v.session_mode == 3) {       // 0x0043ed20 -- MP lockstep session
        c.pump();                     // 0x0043ed29
        // 0x0043ed2e: GAME_MODE is RE-READ after the pump (it can change it -- teardown); only
        // mode 2 (strategic) proceeds to tick. The redraw sibling has NO such recheck.
        if (own.game_mode() != 2) return;
    }
    c.pace_time_tick();   // C4 chain -- the entry prelude our direct call would otherwise skip
    (void)c.time_tick();  // 0x0043ed37 -- int32 result unread, matching the original
    c.harness_sim_tick(); // C6 chain
    c.sim_tick();         // 0x0043ed3c
    c.render_present();   // 0x0043ed41
}

void frame_redraw_behind_dialog(const sim_view &v, sim_store &own, const lt_frame_calls &c) {
    // No input_update on this path -- the dialog host owns input.
    if (own.game_mode() == 6) return;   // 0x0043edb0
    if (*v.session_mode == 3) c.pump(); // 0x0043edb9/0x0043edc2 -- NO mode-2 recheck after
    c.pace_time_tick();                 // C4 chain
    (void)c.time_tick();                // 0x0043edc7
    c.harness_sim_tick();               // C6 chain
    c.sim_tick();                       // 0x0043edcc
    c.render_view();                    // 0x0043edd1
}

} // namespace detail

// ---- the promoted arms ----------------------------------------------------------------------------
//
// This body is un-differentially-verifiable and always was (header banner): it ADVANCES THE SIM, so
// a second execution of it is not a comparison but a second turn. The first-call
// and milestone lines are the liveness instrument LIB-TRANS-P's asymmetric run reads.
// The arm functions carry a distinct lt_frame_ prefix and the bookkeeping stays anonymous-namespace
// -- mh::sim::promoted_arm::active/mark_installed are ALREADY defined by sim_order_dispatch.cpp,
// and a second namespace-scope definition is the exact ODR collision the 2026-09-02 promotion wave
// hit (mh::ai::detail::notify_report).
namespace promoted_arm {
namespace {
bool g_lt_frame_installed = false;

void say_milestone(const char *fn, long n) {
    if (n == 1 || n % 100000 == 0)
        mh::ai::ai_say("; [promote] lib_trans_frame: %s call #%ld (OURS is live)\n", fn, n);
}
} // namespace

void lt_frame() {
    static long n = 0;
    say_milestone("llm_strat_frame", ++n);
    sim_state st = state();
    detail::frame(st.read, st.own, live_lt_frame_calls());
}

void lt_frame_redraw_behind_dialog() {
    static long n = 0;
    say_milestone("llm_strat_frame_redraw_behind_dialog", ++n);
    sim_state st = state();
    detail::frame_redraw_behind_dialog(st.read, st.own, live_lt_frame_calls());
}

} // namespace promoted_arm

} // namespace mh::sim

MH_EXPORT_REPLACE(llm_strat_frame, mh::sim::promoted_arm::lt_frame)
MH_EXPORT_REPLACE(llm_strat_frame_redraw_behind_dialog,
                  mh::sim::promoted_arm::lt_frame_redraw_behind_dialog)

namespace mh::sim {

bool lt_frame_promotion_active() { return promoted_arm::g_lt_frame_installed; }

int install_promotion_lt_frame(int default_on, int spine_promoted,
                               void (*pace_time_tick_hook)(), void (*harness_sim_tick_hook)()) {
    if (default_on == 0) return 0;

    // THE SPINE INTERLOCK (header banner): our bodies call OUR pump/time_tick/sim_tick
    // unconditionally, so promoting the frame under an unpromoted spine would silently swap
    // engines. Refuse by name rather than run a configuration nobody asked for.
    if (!spine_promoted) {
        mh::ai::ai_say("; [promote] lib_trans_frame REFUSED -- the spine is not promoted this run "
                       "([promote] lockstep=1 AND sim_tick=1 must both be INSTALLED first; our "
                       "frame calls OUR pump/time_tick/sim_tick directly). Roll the frame back "
                       "before rolling back the spine.\n");
        return 0;
    }

    set_lt_frame_instrument_hooks(pace_time_tick_hook, harness_sim_tick_hook);

    const int ok_frame  = mh_export_install_llm_strat_frame();
    const int ok_redraw = mh_export_install_llm_strat_frame_redraw_behind_dialog();
    if (!ok_frame || !ok_redraw) {
        // A PARTIAL pair is neither engine (the L1-P rule). The generated installers already
        // logged why; an installed half cannot be un-patched, so say exactly what this run is.
        mh::ai::ai_say("; [promote] lib_trans_frame PARTIAL (frame=%d redraw=%d) -- TREAT THIS RUN "
                       "AS INVALID: one orchestrator is ours and the other is the original.\n",
                       ok_frame, ok_redraw);
        return 0;
    }
    promoted_arm::g_lt_frame_installed = true;
    mh::ai::ai_say("; [promote] lib_trans_frame: llm_strat_frame + redraw_behind_dialog are LIVE -- "
                   "the strategic frame spine is OURS end-to-end this run\n");
    return 1;
}

} // namespace mh::sim
