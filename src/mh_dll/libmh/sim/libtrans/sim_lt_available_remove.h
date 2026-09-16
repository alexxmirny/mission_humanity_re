#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call both bodies make, indirected (rule 3b) so detail:: is drivable offline.
struct lt_available_remove_calls {
    void (*insert_item_in_player_array)(int32_t *arr, int32_t size, uint32_t player, int32_t item,
                                        int32_t new_item); // game_InsertItemInPlayerArray @0x00414106
};

const lt_available_remove_calls &live_lt_available_remove_calls();

namespace detail {

void remove_from_available_buildings(sim_store &own, const lt_available_remove_calls &c,
                                     uint32_t player, int32_t b_i);
void remove_from_available_projects(const sim_view &v, sim_store &own,
                                    const lt_available_remove_calls &c, uint32_t player,
                                    uint32_t p_i);

} // namespace detail

// Public wrappers matching the committed __watcall(EAX, EDX) shapes.
void remove_from_available_buildings(uint32_t player, int32_t b_i);
void remove_from_available_projects(uint32_t player, uint32_t p_i);

} // namespace mh::sim
