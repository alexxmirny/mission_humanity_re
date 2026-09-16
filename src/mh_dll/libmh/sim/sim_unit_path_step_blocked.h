#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the one outward call ----------------------------------------------------------------------
//
// Indirected for offline testability, same reasoning every other sim `calls` table gives (e.g.
// sim_unit_predict_coords.h's identical single-callee shape).
struct unit_path_step_blocked_calls {
    // llm_strat_facing24_to_delta @0x0049610f -- direction delta for a 24-position compass value.
    // Out-params carry the committed mh::call:: pointee type exactly (TACT1-P C6, 2026-09-04).
    void (*facing24_to_delta)(uint32_t facing24, int32_t *out_dx, int32_t *out_dy);
};

const unit_path_step_blocked_calls &live_unit_path_step_blocked_calls();

namespace detail {

// llm_strat_unit_path_step_blocked @0x00495406. See the header banner for the full shape; the .cpp
// carries the per-instruction derivation. No `sim_store` parameter: this function writes no sim state
// at all (see the "NOT SHADOWABLE" note above).
int32_t unit_path_step_blocked(const sim_view &v, const unit_path_step_blocked_calls &c, uint32_t player,
                               int32_t unit_idx);

} // namespace detail

// Public wrapper. Signature matches the committed call/export signature
// (sig_llm_strat_unit_path_step_blocked) exactly.
int32_t unit_path_step_blocked(uint32_t player, int32_t unit_idx);

namespace detail {
} // namespace detail

} // namespace mh::sim
