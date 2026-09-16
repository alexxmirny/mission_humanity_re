#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The distinct ORIGINAL functions this closure calls out to, indirected for offline testability --
// same reasoning as scrap_stored_units_calls / dispatch_calls. `unit_teardown` is an already-migrated
// SIM1A sibling reached through its committed trampoline (never this TU's own body); every other
// member is a function outside the sim migration set entirely.
struct unmap_footprint_calls {
    // llm_strat_sight_remove_circle @0x0049694f. Return value discarded by the original.
    int32_t (*sight_remove_circle)(int32_t player, int32_t x, int32_t y, int32_t building_id,
                                   uint8_t sight);
    // llm_map_region_apply_area @0x004245c3. `area_mask` points at ORIGINAL (read-only) cfg data.
    void (*map_region_apply_area)(uint32_t x, int32_t y, char *area_mask);
    // llm_strat_bldg_unassign_workers @0x00491c08. Return value discarded by the original.
    int32_t (*unassign_workers)(uint16_t player, uint32_t building_index, uint32_t count);
    // llm_strat_unit_teardown @0x00487ba5 -- already-migrated SIM1A sibling, dispatched via its
    // committed trampoline per sim_bldg_scrap_stored_units.h's precedent, not re-entered directly.
    void (*unit_teardown)(uint32_t player, uint16_t unit_index);
    // llm_strat_bldg_power_network_recompute @0x00491c76.
    void (*power_network_recompute)(uint16_t player);
    // llm_strat_bldg_notify_ui @0x00470bdd. Called up to 3 times per invocation.
    void (*notify_ui)(uint16_t player, uint32_t building_index);
    // game_SetEvent @0x00413a52. Return value discarded by the original. Called up to twice.
    uint32_t (*set_event)(uint32_t type);
    // llm_strat_mother_reelect_primary @0x00498aad.
    int32_t (*mother_reelect_primary)(int32_t player, int32_t x, int32_t y);
    // llm_ui_print_queue_text_id @0x0049653d.
    void (*print_queue_text_id)(int32_t text_id);
    // llm_player_teardown_hook_stub @0x0049800c -- still-unnamed per the task brief; called as-is.
    int32_t (*player_teardown_hook_stub)(int32_t player_index);
};

const unmap_footprint_calls &live_unmap_footprint_calls();

namespace detail {

// llm_strat_bldg_unmap_footprint @0x0047b36b. See the header banner above for the full derivation;
// the .cpp carries the per-branch address citation.
void bldg_unmap_footprint(const sim_view &v, sim_store &own, const unmap_footprint_calls &gc,
                          uint16_t player, int32_t building_index);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Matches the committed prototype (addr/mh_calls.gen.h) exactly: void(uint16_t player, int32_t
// building_index).
void bldg_unmap_footprint(uint16_t player, int32_t building_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
