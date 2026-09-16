//
// sim/resid/sim_time_resync.cpp -- see sim_time_resync.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_time_resync_and_tick_00449e21.asm), the exported `.c` being a
// draft.
//
#include "sim/resid/sim_time_resync.h"

#include "addr/mh_calls.gen.h" // typed callables for the frontier originals we still call OUT to
#include "state/host_api.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

namespace {
// The instrument's own prelude, handed over at install time (on_time_tick is TU-local to
// net_lockstep.cpp, so it can only arrive as a pointer). Null until then -- and null is correct for
// any build that installs no pacing instrument.
void (*g_pace_hook)() = nullptr;
// EXPERIMENT ARM (harness skip_pace_hook=1), default off. Set INDEPENDENTLY of the hook pointer so
// it cannot lose a race with the installer: net_lockstep hands the hook over during seam install and
// the harness reads its config at a different moment, so a design that cleared g_pace_hook would be
// order-dependent and could silently re-arm. A flag consulted at CALL time cannot.
bool g_pace_disabled = false;
} // namespace

namespace detail {

// C4 chain, GATED ON THE ROUTE `c.time_tick` will actually take (see the header's pacing-prelude
// banner). Armed row -> the call below goes direct and skips the entry detour, so the prelude has to
// run here. Unarmed row -> it goes through llm_strat_time_tick's entry and the detour runs the
// prelude itself, so running it here as well would run adaptive_tick() twice in one frame. The check
// is at CALL time so it cannot disagree with the binding the next statement uses.
void time_resync_pace_chain() {
    if (g_pace_disabled) return;
    if (g_pace_hook && ::mh::rebind::armed(::mh::rebind::ROW_llm_strat_time_tick)) g_pace_hook();
}

} // namespace detail

// EXPERIMENT ARM: make the pacing prelude a no-op for the rest of the process, which is the state
// the standalone reference host runs in permanently (libref_host installs no pacing instrument).
void set_time_resync_pace_disabled(bool off) {
    g_pace_disabled = off;
}

void set_time_resync_instrument_hooks(void (*pace_time_tick)()) {
    if (pace_time_tick) g_pace_hook = pace_time_tick;
}

const time_resync_and_tick_calls &live_time_resync_and_tick_calls() {
    static const time_resync_and_tick_calls c = {
        mh::host().ticks_ms,
        MH_LIBMH_BIND(time_GetCurrentTime),
        &detail::time_resync_pace_chain,
        MH_LIBMH_BIND(llm_strat_time_tick),
    };
    return c;
}

namespace detail {

// ---- llm_strat_time_resync_and_tick @0x00449e21 ------------------------------------------------
void time_resync_and_tick(const sim_view &v, sim_store &own, const time_resync_and_tick_calls &c) {
    (void)v; // no sim_view member is read anywhere in this body

    // 0x00449e39: re-latch the ms tick counter. SIDE EFFECT ONLY -- the .asm calls this and then
    // immediately overwrites EAX with the next CALL's result, so nothing here ever reads the return
    // value (matches the Ghidra .c draft, which renders it as a bare statement too).
    c.get_ticks_ms();

    // 0x00449e3e-0x00449e43: LAST_GAME_TIME = time_GetCurrentTime(). Plain 8-byte FSTP store, no
    // arithmetic -- own.last_game_time() is RID_LAST_GAME_TIME's write accessor, already bound in
    // sim_state.h with this exact function cited in its own field comment.
    own.last_game_time() = c.get_current_time();

    // C4 chain -- NOT in the original. The entry prelude our direct call would otherwise skip; see
    // the header's pacing-prelude banner and SIM-SAVE-DIV. Self-gating: a no-op unless the
    // llm_strat_time_tick rebind row is armed, i.e. unless the call below really does bypass the
    // entry detour that would have run it.
    c.pace_time_tick();

    // 0x00449e49: llm_strat_time_tick(). THE DOMAIN'S ONE NESTED RE-ENTRY (G30): time_tick reaches
    // back into time_resync_and_tick (this function) -- reason the site is never shadow-armed, not a
    // reason to restructure this body. Its return value (int32_t per mh_calls.gen.h's own binding)
    // is unused here, matching the .asm (no EAX read after this CALL) and the Ghidra .c draft.
    c.time_tick();
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void time_resync_and_tick() {
    sim_state st = state();
    detail::time_resync_and_tick(st.read, st.own, live_time_resync_and_tick_calls());
}

} // namespace mh::sim
