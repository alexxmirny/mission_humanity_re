#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call this function makes. See the header banner on why this is a one-member struct
// rather than a direct `mh::call::` inside `detail::` -- same shape as
// sim_bldg_grant_type_resources.h's `grant_type_resources_calls` / sim_unit_refund.h's own
// single-member `_calls` struct.
struct storage_cancel_pending_docked_calls {
    void (*unit_force_disembark)(uint32_t player, int32_t unit_index); // llm_unit_force_disembark @0x0046d0cd
};

const storage_cancel_pending_docked_calls &live_storage_cancel_pending_docked_calls();

namespace detail {

// llm_storage_cancel_pending_docked @0x0046ca99. See the header banner for the full derivation.
void storage_cancel_pending_docked(const sim_view &v, const storage_cancel_pending_docked_calls &c,
                                   uint32_t player, int32_t building_index);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototype in addr/mh_calls.gen.h exactly (the drift gate
// enforces this on export/shadow installation).

void storage_cancel_pending_docked(uint32_t player, int32_t building_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
