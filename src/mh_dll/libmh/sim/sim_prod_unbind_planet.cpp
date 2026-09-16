//
// sim/sim_prod_unbind_planet.cpp -- see sim_prod_unbind_planet.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_prod_unbind_planet_0048ff73.asm), not from the Ghidra .c draft.
//
#include "sim/sim_prod_unbind_planet.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const prod_unbind_planet_calls &live_prod_unbind_planet_calls() {
    static const prod_unbind_planet_calls c = {
        MH_LIBMH_BIND(llm_strat_prod_deliver_arrivals),
    };
    return c;
}

namespace detail {

void prod_unbind_planet(sim_store &own, const prod_unbind_planet_calls &c, int32_t player,
                        int32_t planet_slot) {
    // 0x0048ff90-0x0048ffa5: read the shuttle-slot index currently bound to this player's
    // production-queue slot, cached BEFORE the clear below (the assembly re-derives the same
    // players[player].prod_queue_slot[planet_slot] address twice rather than caching a pointer --
    // Watcom's usual idiom -- reproduced here as one reference held across both accesses).
    player_profile &profile = own.profile_at(player);
    int32_t         slot    = profile.prod_queue_slot[planet_slot];

    // 0x0048ffa8-0x0048ffb7: unbind the queue slot.
    profile.prod_queue_slot[planet_slot] = 0;

    // 0x0048ffc1-0x0048ffd1: clear the reservation-lock flag on the shuttle slot the queue slot WAS
    // bound to -- indexed by `slot` (the value just read), NOT by `planet_slot`.
    own.prod_shuttle_slot_at(static_cast<uint32_t>(player), slot).is_planet_bound = 0;

    // 0x0048ffdb: unconditional delegate -- re-flush the shuttle delivery queue. This fan-out's own
    // sibling, called through mh::call:: (ORIGINAL) like every other outward call in libmh/sim/.
    c.prod_deliver_arrivals();
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void prod_unbind_planet(int32_t player, int32_t planet_slot) {
    sim_state st = state();
    detail::prod_unbind_planet(st.own, live_prod_unbind_planet_calls(), player, planet_slot);
}


} // namespace mh::sim
