#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. All
// fourteen are ORIGINAL functions outside this batch -- none need reimplementing here;
// live_unit_state_group_marshal_calls() is the only binder. Signatures copied verbatim from
// addr/mh_calls.gen.h (checked against this function's own register-order use in the .cpp).
struct unit_state_group_marshal_calls {
    int32_t (*target_class)(uint32_t owner_and_kind_flag, int32_t roster_slot);       // @0x004495f9
    void (*target_release_ref)(uint32_t player_idx, int32_t unit_idx, uint32_t mode); // @0x004dac44
    void (*unit_get_coords)(uint16_t player, int32_t unit_index, int32_t *out_x,
                            int32_t *out_y);                              // @0x0044b141
    void (*unit_set_state_order)(uint16_t new_order, uint16_t new_state); // @0x00486657
    void (*unit_notify_status)(uint32_t player, int32_t unit_index,
                               uint32_t status_code); // @0x004dae0e
    void (*group_move_register_member)(int32_t player, int32_t unit_idx,
                                       int32_t *scratch_count); // @0x0048d16b
    int32_t (*pathfind_route_leg_group_and_sort)(uint32_t leg_count, int32_t ref_x,
                                                 int32_t ref_y); // @0x004cba1e
    void (*group_move_order_commit)(int32_t goal_x, int32_t goal_y, int32_t player_id,
                                    int32_t member_count, int32_t move_group_id,
                                    uint32_t is_plain_move_flag, uint32_t target_class); // @0x0041c86c
    int32_t (*unit_path_step_blocked)(uint32_t player, int32_t unit_idx);                // @0x00495406
    void (*path_free_slot)(uint16_t player, int32_t unit_index);                         // @0x004969e8
    uint32_t (*unit_in_weapon_range)(int32_t player, int32_t unit_idx, int32_t tile_x,
                                     int32_t tile_y, int32_t attack_class); // @0x0044965d
    void (*unit_set_state_order_of)(int32_t player, int32_t unit_index, int16_t state,
                                    int16_t param); // @0x00486913
    void (*unit_order_move_auto)(uint16_t player, int32_t unit_idx, uint32_t x,
                                 uint32_t y);                                        // @0x0046a56d
    void (*unit_set_state_of)(int32_t player, int32_t unit_index, int16_t state);    // @0x004869b0
    void (*unit_set_order_param)(int32_t player, int32_t unit_index, int16_t param); // @0x0048696f
};

const unit_state_group_marshal_calls &live_unit_state_group_marshal_calls();

namespace detail {

// llm_strat_unit_state_group_marshal @0x00482451. See the header derivation above for the full shape.
// Zero-arg, ambient cur_player/cur_index/cur_unit, matching the original's void(void) signature.
void unit_state_group_marshal(const sim_view &v, sim_store &own,
                              const unit_state_group_marshal_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_state_group_marshal_calls(). Matches the
// committed prototype (sig_llm_strat_unit_state_group_marshal) exactly.
void unit_state_group_marshal();

namespace detail {
} // namespace detail

} // namespace mh::sim
