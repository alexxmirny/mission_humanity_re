#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The three ORIGINAL callees this closure reaches, indirected for offline testability -- same reason
// as every other sim/ TU: a direct mh::call:: inside a detail:: body reaches into the live game image,
// which makes the body untestable by net_selftest.exe simtest.
struct bldg_state_rubble_decay_calls {
    void (*bldg_free_record)(uint32_t player, int32_t building_index);
    int32_t (*sight_remove_circle)(int32_t player, int32_t x, int32_t y, int32_t building_id,
                                   uint8_t radius);
    uint32_t (*sight_add_circle)(uint32_t player, int32_t x, int32_t y, int32_t building_id,
                                 uint8_t radius);
};

const bldg_state_rubble_decay_calls &live_bldg_state_rubble_decay_calls();

namespace detail {

// llm_strat_bldg_state_rubble_sight_decay @0x004738a3. See the header derivation above.
void bldg_state_rubble_sight_decay(const sim_view &v, sim_store &own, const bldg_state_rubble_decay_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_bldg_state_rubble_decay_calls(). Matches the
// original's committed void(void) prototype exactly.
void bldg_state_rubble_sight_decay();

namespace detail {
} // namespace detail

} // namespace mh::sim
