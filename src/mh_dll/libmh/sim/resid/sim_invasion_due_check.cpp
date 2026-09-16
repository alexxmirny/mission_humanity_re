//
// sim/resid/sim_invasion_due_check.cpp -- see sim_invasion_due_check.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim_resid/llm_strat_invasion_due_check_0049949d.asm), the Ghidra .c being
// a draft.
//
#include "sim/resid/sim_invasion_due_check.h"

#include "addr/mh_calls.gen.h"  // typed callables for the frontier originals we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const invasion_due_check_calls &live_invasion_due_check_calls() {
    static const invasion_due_check_calls c = {
        MH_LIBMH_BIND(llm_strat_spawn_enemy_landing),
        MH_LIBMH_BIND(llm_strat_player_presence_lost),
    };
    return c;
}

namespace detail {

// ---- llm_strat_invasion_due_check @0x0049949d -----------------------------------------------------
int32_t invasion_due_check(const sim_view &v, sim_store &own, const invasion_due_check_calls &c) {
    const int32_t planet = *v.planet_index; // 0x004994b5/0x004994ca/0x004994ed: G_PLANET_INDEX, re-read
                                            // by the original at every use; not written by either
                                            // callee, so one read here is equivalent.

    // 0x004994af-0x004994e1: armed (> 0.0, FLDZ/FCOMP/JNC) AND passed (< *v.game_clock, FLD/FCOMP/JC)
    // gate on the CURRENT planet's invasion timer. own.planet_invasion_time_at() is the ONLY binding
    // of this region (RID_STRAT_INVASION_TIME) -- no sim_view sibling -- so the read goes through
    // `own`, not `v`.
    if (own.planet_invasion_time_at(planet) > 0.0 && own.planet_invasion_time_at(planet) < *v.game_clock) {
        // 0x004994e5: fire the enemy landing.
        const int32_t landing = c.spawn_enemy_landing();

        // 0x004994f5-0x00499503: clear the timer. The two stores (low dword 0x00000000, high dword
        // 0xbff00000) are the raw 64-bit pattern 0xBFF0000000000000, which IS the IEEE-754 double
        // -1.0 -- written here as the literal, not as two half-stores.
        own.planet_invasion_time_at(planet) = -1.0;

        // 0x00499509-0x00499518: the landing could not be placed (return 0) -> force a presence-lost
        // check on the local player, mode 0. EAX=local_player_slot (MOVZX word -> uint32), EDX=0.
        if (landing == 0) {
            c.player_presence_lost(static_cast<uint32_t>(*v.local_player_slot), 0);
        }

        // 0x0049951d: the fired arm sets the flag to 1.
        return 1;
    }
    // 0x00499526: the not-due arm sets it to 0. Both arms fall into 0x0049952d
    // `MOV EAX,[EBP-0x18]`, which is the return.
    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t invasion_due_check() {
    sim_state st = state();
    return detail::invasion_due_check(st.read, st.own, live_invasion_due_check_calls());
}

} // namespace mh::sim
