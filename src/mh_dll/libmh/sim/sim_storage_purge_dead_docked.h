#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call this function makes. See the header banner on why this is a one-member struct
// rather than a direct `mh::call::` inside `detail::` -- same shape as
// sim_storage_cancel_pending_docked.h's `storage_cancel_pending_docked_calls` /
// sim_bldg_grant_type_resources.h's `grant_type_resources_calls`.
struct storage_purge_dead_docked_calls {
    void (*storage_remove_docked_unit)(uint16_t player, int32_t unit_index,
                                       int32_t storage_slot); // llm_strat_storage_remove_docked_unit @0x00489dc4
};

const storage_purge_dead_docked_calls &live_storage_purge_dead_docked_calls();

namespace detail {

// llm_strat_storage_purge_dead_docked @0x0049b8f5. See the header banner above for the full
// derivation; the .cpp carries the per-branch address citation.
void storage_purge_dead_docked(const sim_view &v, sim_store &own, const storage_purge_dead_docked_calls &c,
                               int32_t player, int32_t storage_sub_id);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (addr/mh_calls.gen.h's llm_strat_storage_purge_dead_docked) exactly.
void storage_purge_dead_docked(int32_t player, int32_t storage_sub_id);

namespace detail {
} // namespace detail

} // namespace mh::sim
