#include "sim/sim_revoke_invention.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void revoke_invention(sim_store &own, uint16_t player, uint16_t progress_id) {
    // 0x004404a9-0x004404bc: one write, no branches, no callees. `player & 0xffffu` mirrors the
    // original's own redundant re-masking of an already-16-bit value (see header comment).
    own.progress_at(player & 0xffffu, progress_id).acquired = false;
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void revoke_invention(uint16_t player, uint16_t progress_id) {
    sim_state st = state();
    detail::revoke_invention(st.own, player, progress_id);
}


} // namespace mh::sim
