#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The three outward calls this function makes, all indirected via mh::call:: for testability -- same
// shape as sim_unit_add_docked.h's `unit_add_docked_calls` (this file's sibling in the same fan-out,
// calling the same two storage functions the identical way). See the header banner above for why all
// three go through `mh::call::` rather than a sibling's own public `mh::sim::` wrapper.
struct unit_spawn_docked_calls {
    // llm_strat_unit_init_record @0x004619b0 -- already-translated sibling (SIM1A), called at its
    // ORIGINAL address per house rule (not via its own new C++ name mh::sim::unit_init_record),
    // matching sim_bldg_instant_construct.h's identical precedent for a not-co-batched sibling.
    void (*unit_init_record)(int32_t unit_idx, uint32_t unit_proto_id, uint32_t player);
    // llm_strat_storage_find_home_for_unit @0x00463328 -- this fan-out's own sibling, called ORIGINAL.
    int32_t (*storage_find_home_for_unit)(uint32_t player, uint32_t unit_type, int32_t probe_slot);
    // llm_strat_storage_dock_unit_at_building @0x004636bc -- this fan-out's own sibling, ORIGINAL.
    void (*storage_dock_unit_at_building)(uint16_t player, int32_t unit_idx, int32_t storage_slot);
};

const unit_spawn_docked_calls &live_unit_spawn_docked_calls();

namespace detail {

// llm_strat_unit_spawn_docked @0x0046434c. See the header derivation above. Returns the new unit's
// roster slot (1..0x5a), or 0 on: the housing cap, an exhausted slot search, or a failed storage-home
// lookup (which does NOT retry a further slot -- see the header's early-return note).
int32_t unit_spawn_docked(const sim_view &v, const unit_spawn_docked_calls &c, uint16_t unit_proto_id,
                          uint16_t player, uint32_t probe_slot);

} // namespace detail

// Live wrapper: the logic applied to state().read. Matches the committed prototype
// (sig_llm_strat_unit_spawn_docked) exactly.
int32_t unit_spawn_docked(uint16_t unit_proto_id, uint16_t player, uint32_t probe_slot);

namespace detail {
} // namespace detail

} // namespace mh::sim
