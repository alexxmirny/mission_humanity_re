#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_revoke_invention @0x0044048c. `own.progress_at(player, progress_id).acquired = false`.
// The `player & 0xffffu` mirrors the original's own redundant `(uint)player & 0xffff` masking
// (player already arrives 16-bit, zero-extended in AX) -- kept for byte-for-byte fidelity even
// though it is observably a no-op at this width.
void revoke_invention(sim_store &own, uint16_t player, uint16_t progress_id);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Matches the committed callee prototype (addr/mh_calls.gen.h: `llm_strat_revoke_invention(uint16_t
// player, uint16_t progress_id)`, __watcall EAX:EDX) exactly.
void revoke_invention(uint16_t player, uint16_t progress_id);

namespace detail {
} // namespace detail

} // namespace mh::sim
