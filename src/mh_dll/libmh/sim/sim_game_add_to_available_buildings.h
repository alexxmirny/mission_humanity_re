#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_MOTHER/_H_MOTHER/_A_MAIN_BASE/_H_MAIN_BASE/
                                   // _A_CIVIL/_H_CIVIL/_A_PORT/_H_PORT (already committed there,
                                   // reused verbatim per rule 17a)
#include "sim/sim_event_codes.h"   // SESSION_SP
#include "sim/sim_state.h"

namespace mh::sim {

// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
struct add_to_available_buildings_calls {
    void (*insert_item_in_player_array)(int32_t *arr, int32_t size, uint32_t player, int32_t item,
                                        int32_t new_item); // game_InsertItemInPlayerArray
    uint32_t (*set_event)(uint32_t type);                  // game_SetEvent
};

const add_to_available_buildings_calls &live_add_to_available_buildings_calls();

namespace detail {

// game_AddToAvailableBuildings @0x004141a1. One call: inserts b_i into player's AvailableBuildings row.
void add_to_available_buildings(sim_store &own, const add_to_available_buildings_calls &c,
                                uint32_t player, int32_t b_i);

// game_AddToAvailableBuildingsWithCheck @0x00440049. Gates the insert above behind upgrade-tier +
// session-mode-or-not-a-port + three type-pair exclusions; on success, fires the build-page-0 dirty
// event for the viewing player only. See the header banner for the branch-by-branch derivation.
void add_to_available_buildings_with_check(const sim_view &v, sim_store &own,
                                           const add_to_available_buildings_calls &c, uint16_t player,
                                           int32_t b_i);

} // namespace detail

// Live wrappers: the logic applied to state() and live_add_to_available_buildings_calls(). Match the
// committed prototypes (sig_game_AddToAvailableBuildings / sig_game_AddToAvailableBuildingsWithCheck)
// exactly.
void add_to_available_buildings(uint32_t player, int32_t b_i);
void add_to_available_buildings_with_check(uint16_t player, int32_t b_i);

namespace detail {
} // namespace detail

} // namespace mh::sim
