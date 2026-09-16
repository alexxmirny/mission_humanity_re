#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one external callee this closure reaches (game_SetEvent @0x00413a52), indirected for offline
// testability like every other multi-callee sim/ TU.
struct storage_dock_unit_at_building_calls {
    uint32_t (*set_event)(uint32_t type); // game_SetEvent @0x00413a52
};

const storage_dock_unit_at_building_calls &live_storage_dock_unit_at_building_calls();

namespace detail {

// llm_strat_storage_dock_unit_at_building @0x004636bc. See the header SHAPE/HAZARD notes above.
// `own` (not const sim_view alone) because this function writes BOTH `unit_storage` (docked_units/
// docked_count/occupancy) and `units` (home_storage_slot/state/x/y) directly -- no callee
// indirection for either write, per sim_migration.json's `roster_write_via: []`.
void storage_dock_unit_at_building(const sim_view &v, sim_store &own,
                                   const storage_dock_unit_at_building_calls &c, uint16_t player,
                                   int32_t unit_idx, int32_t storage_slot);

} // namespace detail

// Live wrapper: the logic applied to state() and live_storage_dock_unit_at_building_calls(). Matches
// the committed prototype (sig_llm_strat_storage_dock_unit_at_building) exactly.
void storage_dock_unit_at_building(uint16_t player, int32_t unit_idx, int32_t storage_slot);

namespace detail {
} // namespace detail

} // namespace mh::sim
