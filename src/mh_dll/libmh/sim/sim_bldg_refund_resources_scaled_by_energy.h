#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other module here: a direct mh::call:: inside a detail::
// body reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
//
// utils_math_trunc is deliberately NOT a member here -- see the header banner above; reproduced
// inline in the .cpp instead.
struct bldg_refund_resources_scaled_by_energy_calls {
    void (*resource_add)(int32_t player, int32_t resource_index,
                         int32_t amount); // llm_resource_add @0x00497f4a
};

const bldg_refund_resources_scaled_by_energy_calls &live_bldg_refund_resources_scaled_by_energy_calls();

namespace detail {

// llm_strat_bldg_refund_resources_scaled_by_energy @0x00479493. See the header banner above and the
// .cpp for the full per-instruction derivation. No sim-state writes: a pure read over
// `buildings`/`cfg_buildings` plus one outward call.
void bldg_refund_resources_scaled_by_energy(const sim_view                                     &v,
                                            const bldg_refund_resources_scaled_by_energy_calls &c,
                                            int32_t player, int32_t index);

} // namespace detail

// Live wrapper: the logic applied to state() and live_bldg_refund_resources_scaled_by_energy_calls().
// Matches the original's committed __watcall(player, index) shape.
void bldg_refund_resources_scaled_by_energy(int32_t player, int32_t index);

namespace detail {
} // namespace detail

} // namespace mh::sim
