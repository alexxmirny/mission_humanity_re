//
// sim/sim_prod_planet_distance_factor.cpp -- see sim_prod_planet_distance_factor.h. Translated from
// the DISASSEMBLY (tmp/decomp_sim/llm_prod_planet_distance_factor_00490221.asm), not from the Ghidra
// .c draft.
//
#include "sim/sim_prod_planet_distance_factor.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

double planet_distance_factor(int32_t src_planet, int32_t dest_planet) {
    // 0x00490221-0x00490263: no branch, no loop, no state access of any kind. src_planet/dest_planet
    // are stashed to locals in the original (0x00490238/0x0049023b) and never read again -- both are
    // genuinely ignored here too, matching the original exactly rather than "cleaning up" the unused
    // parameters. See the header banner for the full FLD-of-a-constant derivation.
    (void)src_planet;
    (void)dest_planet;
    return 1.0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

double planet_distance_factor(int32_t src_planet, int32_t dest_planet) {
    return detail::planet_distance_factor(src_planet, dest_planet);
}


} // namespace mh::sim
