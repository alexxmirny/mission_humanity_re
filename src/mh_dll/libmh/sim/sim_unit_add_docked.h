#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The three outward calls this function makes, all indirected for testability -- same shape as
// sim_storage_cancel_pending_docked.h's `storage_cancel_pending_docked_calls` /
// sim_storage_purge_dead_docked.h's `storage_purge_dead_docked_calls` / sim_bldg_defense_cost.h's
// `bldg_defense_cost_calls`. All three callees are ORIGINAL functions (map_unit_Add is already
// independently translated elsewhere with its own shadow site; the other two are this batch's own
// siblings, translated concurrently) -- see the header banner above for why all three are called
// through `mh::call::` rather than a sibling's own public `mh::sim::` wrapper, regardless of purity or
// batch membership.
struct unit_add_docked_calls {
    // NOTE: probe_slot is int32_t here (llm_strat_storage_find_home_for_unit's own committed
    // signature) but uint32_t on llm_strat_unit_add_docked itself (this function's own committed
    // export signature) -- two independently-committed prototypes for the "same" conceptual
    // parameter, not a typo; cast explicitly at the call site.
    int32_t (*storage_find_home_for_unit)(uint32_t player, uint32_t unit_type,
                                          int32_t probe_slot);            // llm_strat_storage_find_home_for_unit @0x00463328
    void (*unit_add)(uint32_t unit, int32_t unit_proto, uint16_t player); // map_unit_Add @0x00461e9e
    void (*storage_dock_unit_at_building)(uint16_t player, int32_t unit_idx,
                                          int32_t storage_slot); // llm_strat_storage_dock_unit_at_building @0x004636bc
};

const unit_add_docked_calls &live_unit_add_docked_calls();

namespace detail {

// llm_strat_unit_add_docked @0x0046443a. See the header banner above for the full derivation and for
// why all three outward calls (`c.storage_find_home_for_unit`, `c.unit_add`,
// `c.storage_dock_unit_at_building`) go through the `_calls` indirection rather than a sibling's own
// public `mh::sim::` wrapper.
int32_t unit_add_docked(const sim_view &v, const unit_add_docked_calls &c, uint32_t unit_proto_id,
                        uint16_t player, uint32_t probe_slot);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (`int llm_strat_unit_add_docked(uint unit_proto_id, ushort player, undefined4 probe_slot)`) exactly.
int32_t unit_add_docked(uint32_t unit_proto_id, uint16_t player, uint32_t probe_slot);

namespace detail {
} // namespace detail

} // namespace mh::sim
