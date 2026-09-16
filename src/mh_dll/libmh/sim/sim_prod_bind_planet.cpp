//
// sim/sim_prod_bind_planet.cpp -- see sim_prod_bind_planet.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_prod_bind_planet_0048feef.asm), not from the Ghidra .c draft.
//
#include "sim/sim_prod_bind_planet.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

int32_t prod_bind_planet(sim_store &own, int32_t player, int32_t queue_slot, int32_t shuttle_slot) {
    // 0x0048ff0e-0x0048ff24: guard. One reference held across both the read here and the write below
    // -- the assembly re-derives the same players[player].prod_queue_slot[queue_slot] address twice
    // rather than caching a pointer (Watcom's usual idiom, matching sim_prod_unbind_planet.cpp's own
    // precedent for the identical field).
    player_profile &profile = own.profile_at(player);
    if (profile.prod_queue_slot[queue_slot] != 0) {
        // 0x0048ff26-0x0048ff2d: already occupied -- no-op, return false.
        return 0;
    }

    // LAB_0048ff2f, 0x0048ff2f-0x0048ff61: claim the queue slot and mark the shuttle slot as
    // planet-bound.
    profile.prod_queue_slot[queue_slot]                                                   = shuttle_slot;
    own.prod_shuttle_slot_at(static_cast<uint32_t>(player), shuttle_slot).is_planet_bound = 1;
    return 1;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t prod_bind_planet(int32_t player, int32_t queue_slot, int32_t shuttle_slot) {
    sim_state st = state();
    return detail::prod_bind_planet(st.own, player, queue_slot, shuttle_slot);
}


} // namespace mh::sim
