//
// sim/resid/sim_invasion_alert_reset.cpp -- see sim_invasion_alert_reset.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim_resid/llm_strat_invasion_alert_reset_all_0049b447.asm).
//
#include "sim/resid/sim_invasion_alert_reset.h"

#include "addr/mh_calls.gen.h"  // typed callables for the effectful/frontier originals we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const invasion_alert_reset_calls &live_invasion_alert_reset_calls() {
    static const invasion_alert_reset_calls c = {
        MH_LIBMH_BIND(llm_strat_invasion_alert_clear),
    };
    return c;
}

namespace detail {

// ---- llm_strat_invasion_alert_reset_all @0x0049b447 -------------------------------------------
void invasion_alert_reset_all(sim_store &own, const invasion_alert_reset_calls &c) {
    // 0x0049b45f-0x0049b47e: i = 0; while (i < 0x20) { invasion_alert_clear(i); ++i; }. The
    // increment is a separate basic block the body jumps back to (see the header note) -- 32
    // calls, i == 0..31 inclusive.
    for (int32_t i = 0; i < 32; ++i) {
        c.invasion_alert_clear(i);
    }

    // 0x0049b480/0x0049b48a: both dwords of _G_LLM_STRAT_ADVISOR_NEXT_TIME zeroed -- the +0.0 bit
    // pattern (see the header note).
    own.advisor_next_time() = 0.0;
}

} // namespace detail

// ---- the public wrapper -------------------------------------------------------------------------

void invasion_alert_reset_all() {
    sim_state st = state();
    detail::invasion_alert_reset_all(st.own, live_invasion_alert_reset_calls());
}

} // namespace mh::sim
