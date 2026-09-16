#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // UNIT_TYPE_A_HELI
#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for offline testability (net_selftest.exe simtest), same reasoning as every other sim
// `calls` table. Both callees are pure with respect to sim state -- see the header banner above.
struct unit_predict_coords_after_delay_calls {
    // llm_strat_unit_get_coords @0x0044b141. Out-params carry the committed mh::call:: pointee type
    // exactly (TACT1-P C6, 2026-09-04; sim_unit_update_rotation.h documents the same constraint for the
    // same callee).
    void (*get_coords)(uint16_t player, int32_t unit_index, int32_t *out_x, int32_t *out_y);
    // llm_strat_facing24_to_delta @0x0049610f -- direction delta for a 24-position compass value.
    void (*facing24_to_delta)(uint32_t facing24, int32_t *out_dx, int32_t *out_dy);
};

const unit_predict_coords_after_delay_calls &live_unit_predict_coords_after_delay_calls();

// The logic over an EXPLICIT state, so `net_selftest.exe simtest` can drive it over heap buffers with
// no game and no rig -- this is the ONLY oracle this function gets (see the header banner's NO
// SIM-STATE WRITE note).
namespace detail {

// llm_strat_unit_predict_coords_after_delay @0x00448d55. See the header banner for the full shape;
// the .cpp carries the per-instruction derivation. No sim_store parameter: this function writes no
// sim state at all, only through its own out_x/out_y.
void unit_predict_coords_after_delay(const sim_view &v, const unit_predict_coords_after_delay_calls &c,
                                     uint16_t player, int32_t unit_idx, double time_delta,
                                     uint32_t *out_x, uint32_t *out_y);

} // namespace detail

// Public wrapper. Signature matches the committed call/export signature
// (sig_llm_strat_unit_predict_coords_after_delay) exactly, including the two dead register params, so
// this slots unchanged into mh_calls.gen.h's existing call shape -- sim_unit_target_tracking.cpp
// already calls the ORIGINAL through this exact shape, and any future export-replacement wiring needs
// no signature change here.
void unit_predict_coords_after_delay(uint32_t player, int32_t unit_idx, uint32_t unused_ebx,
                                     uint32_t unused_ecx, double time_delta, uint32_t *out_x,
                                     uint32_t *out_y);

} // namespace mh::sim
