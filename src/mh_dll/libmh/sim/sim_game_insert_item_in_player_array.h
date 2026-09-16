#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
struct insert_item_in_player_array_calls {
    uint32_t (*set_event)(uint32_t type); // game_SetEvent @0x00413a52
};

const insert_item_in_player_array_calls &live_insert_item_in_player_array_calls();

namespace detail {

// game_InsertItemInPlayerArray @0x00414106. See the header banner above for the branch-by-branch
// derivation. `arr`/`size` describe the caller-owned array (an address escape, no sim_store binding);
// `player`/`item`/`new_item` match the original's __watcall parameter order exactly.
void insert_item_in_player_array(const sim_view &v, const insert_item_in_player_array_calls &c,
                                 int32_t *arr, int32_t size, uint32_t player, int32_t item,
                                 int32_t new_item);

} // namespace detail

// Live wrapper: the logic applied to state().read and live_insert_item_in_player_array_calls(). Takes
// `int32_t *arr` because that is what sig_game_InsertItemInPlayerArray now spells -- the committed
// prototype always said `int *`, and the generator's old `void *` degradation is what forced the
// weaker type here (TACT1-P C6, 2026-09-04). The shadow/export thunk machinery assigns this function
// directly to a variable of that pointer-to-function type, so the match must stay exact.
void insert_item_in_player_array(int32_t *arr, int32_t size, uint32_t player, int32_t item, int32_t new_item);

namespace detail {
} // namespace detail

} // namespace mh::sim
