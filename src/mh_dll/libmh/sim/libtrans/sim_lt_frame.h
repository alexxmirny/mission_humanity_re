//
// sim/libtrans/sim_lt_frame.h -- the two strategic-frame orchestrators (the unshadowable pair).
//
//   void __watcall llm_strat_frame(void)                       @ 0x0043ecfa (size 86 B)
//   void __watcall llm_strat_frame_redraw_behind_dialog(void)  @ 0x0043ed98 (size 72 B)
//
// lib_trans batch F, unit f1 (LT1F -- translate LAST, deps on every other batch). Translated from
// the DISASSEMBLY (tmp/decomp_lib_trans/llm_strat_frame_0043ecfa.asm, .._redraw_behind_dialog_
// 0043ed98.asm). Pure orchestrators: no state of their own beyond two mode reads.
//
//   frame:                    input_update(); if (mode != 6) { if (session == 3) { pump();
//                             if (mode != 2) goto out; } time_tick(); sim_tick();
//                             render_present(); }
//   redraw_behind_dialog:     if (mode != 6) { if (session == 3) pump(); time_tick(); sim_tick();
//                             render_view(); }
//
// THE ASYMMETRY IS REAL AND DELIBERATE (0x0043ed2e vs 0x0043edc2): after the pump only `frame`
// re-checks GAME_MODE == 2 before ticking; the redraw variant ticks regardless (its dialog host
// already pinned the mode). GAME_MODE is re-READ after pump on the frame path -- the pump can
// change it (session teardown), and the translation re-reads the live byte the same way.
//
// ---- THE ROUTING DECISION (user, 2026-09-02 -- the question LT1F was parked on) ------------------
//
// pump / time_tick / sim_tick are DIRECT C++ calls into our own bodies (mh::lockstep::pump /
// time_tick / sim_tick), NOT calls through the original entries -- and the instrument work that
// lives in the entry detours is CHAINED explicitly into this spine:
//
//   * C4 (pacing): net_lockstep.cpp's time_tick_detour runs on_time_tick() (adaptive lookahead,
//     STEP_SIZE/GAME_SPEED pins, rx_spin, graceful-drop) before every time_tick entry. Our spine
//     calls the same hook through `pace_time_tick` BEFORE our time_tick body -- ARMED-GATED: the
//     hook wrapper runs on_time_tick() only when the pacing detour armed this run (g_tt_tramp),
//     so a config with no pacing knobs behaves identically through either path.
//   * C6 (determinism harness): harness.cpp's sim_tick_detour runs on_sim_tick() (fixed-step pin,
//     TEV_SIMTICK) before every sim_tick entry. Same chain, same armed gate
//     (MH_Harness_OnSimTick, a no-op when no harness armed).
//
// CONSEQUENCE -- THE SPINE INTERLOCK. Our bodies call OUR pump/time_tick/sim_tick unconditionally,
// so promoting the frame pair in a config where the spine is NOT promoted would silently swap
// engines ([promote] sim_tick is OPT-IN and NOT in the lockstep closure -- turn_engine.h's C6
// note). install_promotion_lt_frame therefore REFUSES, with a named log line, unless the caller
// (net_lockstep.cpp's lockstep_install_core) reports the spine promoted: [promote] lockstep=1
// (pump + time_tick) AND [promote] sim_tick=1 both actually installed. Rollback configs must roll
// the frame back FIRST (rollback_original.ini carries the keys together).
//
// input_update stays a WALL (adjudicated host-callback class), called through the mh::call:: thunk.
// render_present / render_view were walls beside it until SIMABI-NOTIFY (2026-09-10) and are now
// NOTIFY RECORDS on the screen channel -- LIBMH_EVK_SCR_STRAT_FRAME_PRESENT / _REDRAW, the
// strategic siblings of SCR_TACT_FRAME_PRESENT. Each is the LAST statement of its body, so there is
// no same-frame readback to defer (R3b) and no return to consume (R3); the input pump at the head
// of `frame` is the one that stays required, under R10.
//
// NO SHADOW SITE EVER EXISTS for either body: both ADVANCE THE SIM (pump + time_tick + sim_tick),
// so a shadow arm would double-step the world. Translation-stage evidence is the offline t16
// cases over detail:: + adversarial review; the composite proof (every determinism oracle running
// ON our spine) is LIB-TRANS-P's clause. Ledger rows: verified/T2 "unshadowable, composite proof
// at LIB-TRANS-P".
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"
#include "state/host_api.h"

namespace mh::sim {

// Every out-edge of the pair, indirected so detail:: is drivable offline (rule 3b). The two
// instrument hooks are members like the callees they precede -- the offline oracle asserts their
// ORDER relative to the tick bodies, which is the whole point of the chain.
struct lt_frame_calls {
    void (*input_update)();     // WALL: mh::host().apply_frame_input @0x00441b88
    void (*pump)();             // OURS: mh::lockstep::pump (direct -- routing decision above)
    void (*pace_time_tick)();   // C4 chain: on_time_tick iff the pacing detour armed this run
    int32_t (*time_tick)();     // OURS: mh::lockstep::time_tick (int32 result unread, as original)
    void (*harness_sim_tick)(); // C6 chain: on_sim_tick iff the harness armed this run
    void (*sim_tick)();         // OURS: mh::lockstep::sim_tick
    void (*render_present)();   // NOTIFY: evt::strat_frame_present (was @0x0044e578)
    void (*render_view)();      // NOTIFY: evt::strat_frame_redraw (was @0x0044e5a4)
};

const lt_frame_calls &live_lt_frame_calls();

// Called by the seams installer (net_lockstep.cpp) before promotion: the two hooks are TU-local
// to net_lockstep.cpp / harness.cpp and reach this unit as pointers, so no seams header is
// included here (the layering rule).
// EXPERIMENT KNOB (default off) -- replace the frame's input WALL. See the .cpp.
void set_lt_frame_input_override(void (*input_update)());

void set_lt_frame_instrument_hooks(void (*pace_time_tick)(), void (*harness_sim_tick)());

namespace detail {

void frame(const sim_view &v, sim_store &own, const lt_frame_calls &c);
void frame_redraw_behind_dialog(const sim_view &v, sim_store &own, const lt_frame_calls &c);

} // namespace detail

// [promote] lib_trans_frame -- installs BOTH entries or refuses. `spine_promoted` is computed by
// the caller (lockstep_install_core) from what actually installed this run; the ship default is
// passed in (SHIP_PROMOTE_LIB_TRANS_FRAME, net_internal.h -- 0 until LIB-TRANS-P).
int  install_promotion_lt_frame(int default_on, int spine_promoted,
                                void (*pace_time_tick_hook)(), void (*harness_sim_tick_hook)());
bool lt_frame_promotion_active();

} // namespace mh::sim
