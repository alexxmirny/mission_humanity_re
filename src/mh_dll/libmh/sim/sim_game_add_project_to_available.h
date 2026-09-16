#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// AvailableProjects' bucket geometry -- see DECLARED NEED 1 above for the derivation. No backing
// Ghidra enum for the project "type" domain (rule 17a checked, none found).
inline constexpr int32_t AVAILABLE_PROJECTS_TYPES_PER_PLAYER = 2;    // 400 bytes/player / 200 bytes/bucket
inline constexpr int32_t AVAILABLE_PROJECTS_SLOTS_PER_BUCKET = 0x32; // 50 int32 slots (200 bytes) -- matches
                                                                     // game_InsertItemInPlayerArray's own
                                                                     // `size` argument at this call site

// The call site's constant `item` argument -- opaque without game_InsertItemInPlayerArray's own body
// (untranslated, not in this batch); reproduced literally rather than guessed at.
inline constexpr int32_t ITEM_UNUSED = 0;

// Indirected for the same reason as every other module here (see the header banner's CALLEES note).
struct add_project_to_available_calls {
    void (*insert_item_in_player_array)(int32_t *arr, int32_t size, uint32_t player, int32_t item,
                                        int32_t new_item); // game_InsertItemInPlayerArray @0x00414106
                                                           // (committed pointee int32_t *, TACT1-P C6, 2026-09-04)
};

const add_project_to_available_calls &live_add_project_to_available_calls();

namespace detail {

// game_AddProjectToAvailable @0x0041422e. See the header banner above for the full derivation.
void add_project_to_available(const sim_view &v, sim_store &own,
                              const add_project_to_available_calls &c, uint32_t player, uint32_t p_i);

// game_AddProjectToAvailableWithCheck @0x00440174. Pure forwarder -- narrows player to 16 bits, then
// calls add_project_to_available() above directly (same TU, in-batch sibling).
void add_project_to_available_with_check(const sim_view &v, sim_store &own,
                                         const add_project_to_available_calls &c, uint16_t player,
                                         uint32_t p_i);

} // namespace detail

// Live wrappers: the logic applied to state() and live_add_project_to_available_calls(). Match the
// committed prototypes (sig_game_AddProjectToAvailable / sig_game_AddProjectToAvailableWithCheck)
// exactly.
void add_project_to_available(uint32_t player, uint32_t p_i);
void add_project_to_available_with_check(uint16_t player, uint32_t p_i);

namespace detail {
} // namespace detail

} // namespace mh::sim
