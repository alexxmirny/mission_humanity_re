#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
struct unit_chase_check_calls {
    void (*unit_get_coords)(uint16_t player, int32_t unit_index, int32_t *out_x,
                            int32_t *out_y);                                          // llm_strat_unit_get_coords @0x0044b141
    void (*target_release_ref)(uint32_t player_idx, int32_t unit_idx, uint32_t mode); // llm_strat_target_release_ref @0x004dac44
    int32_t (*target_class)(uint32_t owner_and_kind_flag, int32_t roster_slot);       // mh::sim::target_class (sibling) @0x004495f9
    uint32_t (*unit_in_weapon_range)(int32_t player, int32_t unit_idx, int32_t tile_x, int32_t tile_y,
                                     int32_t target_class);       // mh::sim::unit_in_weapon_range (sibling) @0x0044965d
    void (*unit_fire_at_target)();                                // mh::sim::unit_fire_at_target (sibling, THIS batch) @0x00486506
    void (*unit_set_state_order)(uint16_t state, uint16_t order); // mh::sim::unit_set_state_order (sibling) @0x00486657
    void (*unit_set_state)(uint16_t new_state);                   // mh::sim::unit_set_state (sibling) @0x004866c9
};

const unit_chase_check_calls &live_unit_chase_check_calls();

namespace detail {

// llm_strat_unit_chase_check @0x004866fb. See the header banner above for the full derivation. Needs
// `own` for the two direct writes into the current unit (the target_fine_x/y refresh via get_coords'
// out-pointers, and the target_ref/target_index clear) -- every OTHER write in this function happens
// inside a reimplemented sibling's own call, which manages its own `own` internally.
int32_t unit_chase_check(const sim_view &v, sim_store &own, const unit_chase_check_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_chase_check_calls(). Matches the committed
// prototype (sig_llm_strat_unit_chase_check) exactly.
int32_t unit_chase_check();

namespace detail {
} // namespace detail

} // namespace mh::sim
