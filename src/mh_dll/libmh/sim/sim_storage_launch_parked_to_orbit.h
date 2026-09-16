#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call this function makes. See the header banner on why this is a one-member struct
// rather than a direct `mh::call::` inside `detail::` -- same shape as
// sim_storage_cancel_pending_docked.h's `storage_cancel_pending_docked_calls`.
struct storage_launch_parked_to_orbit_calls {
    void (*unit_force_disembark)(uint32_t player, int32_t unit_index); // llm_unit_force_disembark @0x0046d0cd
};

const storage_launch_parked_to_orbit_calls &live_storage_launch_parked_to_orbit_calls();

namespace detail {

// llm_strat_storage_launch_parked_to_orbit @0x0048f952. See the header banner for the full derivation.
int32_t storage_launch_parked_to_orbit(const sim_view &v, sim_store &own,
                                       const storage_launch_parked_to_orbit_calls &c, uint16_t player,
                                       int32_t building_index);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototype in addr/mh_calls.gen.h exactly (the drift gate
// enforces this on export/shadow installation).

int32_t storage_launch_parked_to_orbit(uint16_t player, int32_t building_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
