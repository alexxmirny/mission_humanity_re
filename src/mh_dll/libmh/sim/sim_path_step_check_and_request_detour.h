#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -------------------------------------------------------------------------
//
// Indirected for offline testability (net_selftest.exe simtest), same reasoning as every other sim
// `calls` table (e.g. sim_unit_predict_coords.h's identical two-callee shape). Both callees are
// ORIGINAL functions, called through mh::call:: at the live site -- neither is reimplemented here.
struct path_step_check_and_request_detour_calls {
    // llm_strat_facing24_to_delta @0x0049610f -- direction delta for a 24-position compass heading.
    void (*facing24_to_delta)(uint32_t facing24, int32_t *out_dx, int32_t *out_dy);
    // llm_strat_unit_path_detour @0x004209b2 -- (player, unit_idx, alt_unit_idx) -> 0 (detour found) /
    // 1 (no detour, recursed to depth 1 on alt_unit_idx). This function only ever uses the RETURN
    // VALUE of its own logic (always returns 1 once it decides to call this), not this callee's return.
    int32_t (*unit_path_detour)(int32_t player, int32_t unit_idx, int32_t alt_unit_idx);
};

const path_step_check_and_request_detour_calls &live_path_step_check_and_request_detour_calls();

// The logic over an EXPLICIT state, so `net_selftest.exe simtest` can drive it over heap buffers with
// no game and no rig.
namespace detail {

// llm_strat_path_step_check_and_request_detour @0x0049a23d. See the header banner for the full shape;
// the .cpp carries the per-instruction derivation. No sim_store parameter -- see the banner's WRITES
// STATE ONLY THROUGH ITS CALLEE note.
int32_t path_step_check_and_request_detour(const sim_view                                 &v,
                                           const path_step_check_and_request_detour_calls &c,
                                           uint32_t src_x, uint32_t src_y, int32_t dst_x, int32_t dst_y);

} // namespace detail

// Public wrapper. Signature matches the committed call/export signature
// (sig_llm_strat_path_step_check_and_request_detour) exactly.
int32_t path_step_check_and_request_detour(uint32_t src_x, uint32_t src_y, int32_t dst_x, int32_t dst_y);

namespace detail {
} // namespace detail

} // namespace mh::sim
