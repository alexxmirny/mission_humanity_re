#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls ---------------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
struct unit_fire_at_target2_if_aimed_calls {
    void (*unit_get_coords)(uint16_t player, int32_t unit_index, int32_t *out_x,
                            int32_t *out_y);                                // llm_strat_unit_get_coords @0x0044b141
    int32_t (*dir_from_to)(int32_t x1, int32_t y1, int32_t x2, int32_t y2); // @0x0049482b
    void (*unit_fire_weapon)(uint32_t player, uint32_t unit_index, uint8_t weapon_slot,
                             uint32_t target_ref, uint16_t target_index, int32_t target_fine_x,
                             int32_t target_fine_y); // llm_strat_unit_fire_weapon @0x0048bb6c
};

const unit_fire_at_target2_if_aimed_calls &live_unit_fire_at_target2_if_aimed_calls();

namespace detail {

// llm_strat_unit_fire_at_target2_if_aimed @0x004863e6. See the header banner above for the full
// derivation. No parameters -- operates on `v`'s ambient cur_unit/cur_player/cur_index alone,
// matching the original's void(void) signature.
void unit_fire_at_target2_if_aimed(const sim_view &v, const unit_fire_at_target2_if_aimed_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state().read and live_unit_fire_at_target2_if_aimed_calls().
// Matches the committed prototype (sig_llm_strat_unit_fire_at_target2_if_aimed) exactly.
void unit_fire_at_target2_if_aimed();

namespace detail {
} // namespace detail

} // namespace mh::sim
