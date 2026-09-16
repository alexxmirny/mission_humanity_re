#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The two outward calls this body makes that are NOT `llm_strat_time_tick` (see below): both
// frontier originals, reached through `mh::call::` via this indirection so `detail::` stays testable
// under simtest.
struct time_resync_and_tick_calls {
    uint32_t (*get_ticks_ms)();   // llm_time_get_ticks_ms @0x004d056c -- return value unused (side effect only)
    double (*get_current_time)(); // time_GetCurrentTime @0x00427616
    void (*pace_time_tick)();     // C4 chain -- see "THE PACING PRELUDE" below. No original counterpart.
    int32_t (*time_tick)();       // llm_strat_time_tick @0x0043eea3 -- THE RE-ENTRANT CALLEE (G30); return value unused
};

// ---- THE PACING PRELUDE, and why this body needs one (SIM-SAVE-DIV, 2026-09-05) -----------------
// net_lockstep.cpp's time_tick_detour owns llm_strat_time_tick's ENTRY and runs on_time_tick()
// before it -- adaptive lookahead, the STEP_SIZE / GAME_SPEED / SIM_STEP_INTERVAL pins, rx_spin,
// graceful-drop. `c.time_tick` here is MH_LIBMH_BIND(llm_strat_time_tick), so when that rebind row
// is ARMED the call goes straight to mh::lockstep::time_tick and NEVER TOUCHES THE ENTRY -- so the
// prelude does not run, and whatever the pins were holding stays as whoever wrote it last left it.
//
// THAT IS A MEASURED DIVERGENCE, not a theoretical one. A save load restores the six clock doubles
// at 0x00e587b1 from the file's SESSION_MODE block, game_speed and SIM_STEP_INTERVAL included. In
// the all-original arm the load's apply tail reaches this function, its CALL lands on the entry, the
// detour re-pins 0.01 / 10.0, and the run continues at the speed the rig asked for. In the promoted
// arm the same call went direct, the pins never re-applied, and the sim ran ONE STEP at the save's
// 0.1 / 1.0 -- visible as region `time_globals` differing at exactly the load step and nowhere else.
// (That slice is now six: SB-HOSTFREE H0 split it into one per clock region, so the same failure
// today would name `sim_step_interval` and `game_speed` specifically rather than the family.)
//
// SAME SHAPE, SAME FIX AS LT1F: sim/libtrans/sim_lt_frame.h chains this identical hook for the frame
// spine, for the identical reason (its bodies call our time_tick directly too). The difference is
// that LT1F's binding is UNCONDITIONALLY direct, so it chains unconditionally; ours is conditional,
// so the chain is gated on the row being armed -- chaining it unconditionally would run the prelude
// TWICE on the unarmed path (once from the detour, once from here), and adaptive_tick() is not
// idempotent. The gate is evaluated at CALL time, not at bind time, so it cannot disagree with the
// route the very next line takes.
//
// Default is a no-op, so a build that installs no pacing instrument behaves exactly as before, and
// the offline oracle -- which replaces the whole struct -- is unaffected.
// EXPERIMENT ARM (default off) -- disable the C4 pacing prelude. See the .cpp.
void set_time_resync_pace_disabled(bool off);

void set_time_resync_instrument_hooks(void (*pace_time_tick)());

const time_resync_and_tick_calls &live_time_resync_and_tick_calls();

namespace detail {

// llm_strat_time_resync_and_tick @0x00449e21. Reads nothing through `v` (no sim_view member is
// touched anywhere in this body). Writes LAST_GAME_TIME through `own`. Reaches every callee
// (all frontier) through `c`. void return, matching the original.
void time_resync_and_tick(const sim_view &v, sim_store &own, const time_resync_and_tick_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_time_resync_and_tick_calls().
void time_resync_and_tick();

} // namespace mh::sim
