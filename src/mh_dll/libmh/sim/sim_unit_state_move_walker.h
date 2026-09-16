#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. All
// nineteen are ORIGINAL functions outside this batch -- none need reimplementing here;
// live_unit_state_move_walker_calls() is the only binder. Signatures copied verbatim from
// addr/mh_calls.gen.h (checked against this function's own register-order use in the .cpp). Out-params
// carry the committed mh::call:: pointee type exactly (TACT1-P C6, 2026-09-04; the aggregate-initializer
// binding in live_unit_state_move_walker_calls() needs an exact function-pointer-type match).
struct unit_state_move_walker_calls {
    double (*dir_step_factor)(int32_t dir);                                     // llm_strat_dir_step_factor @0x00449b28
    int32_t (*target_class)(uint32_t owner_and_kind_flag, int32_t roster_slot); // @0x004495f9
    uint32_t (*unit_in_weapon_range)(int32_t player, int32_t unit_idx, int32_t tile_x, int32_t tile_y,
                                     int32_t target_class_flags); // @0x0044965d
    int32_t (*unit_goal_in_weapon_range)(int32_t player, int32_t unit_idx, int32_t tile_x,
                                         int32_t tile_y, int32_t target_class_flags); // @0x0044988f
    void (*unit_fire_at_target_if_aimed)();                                           // @0x004862c6
    void (*unit_set_state_order)(uint16_t new_order, uint16_t new_state);             // @0x00486657
    void (*unit_set_state)(uint16_t new_state);                                       // @0x004866c9
    void (*unit_notify_status)(uint32_t player, int32_t unit_index,
                               uint32_t status_code);                                 // @0x004dae0e
    void (*target_release_ref)(uint32_t player_idx, int32_t unit_idx, uint32_t mode); // @0x004dac44
    void (*unit_order_move_auto)(uint16_t player, int32_t unit_idx, uint32_t x,
                                 uint32_t y); // @0x0046a56d
    void (*unit_get_coords)(uint16_t player, int32_t unit_index, int32_t *out_fine_x,
                            int32_t *out_fine_y); // @0x0044b141
    void (*bldg_get_coords)(uint16_t player, int32_t building_index, int32_t *out_fine_x,
                            int32_t *out_fine_y); // @0x00449b8a
    void (*bldg_footprint_random_offset)(uint32_t player, uint32_t unit_idx, int32_t target_owner,
                                         int32_t target_index, uint32_t *out_fine_x,
                                         uint32_t *out_fine_y);                     // @0x00449c61
    void (*facing24_to_delta)(uint32_t facing24, int32_t *out_dx, int32_t *out_dy); // @0x0049610f
    int32_t (*path_step_check_and_request_detour)(uint32_t src_x, uint32_t src_y, int32_t dst_x,
                                                  int32_t dst_y);                    // @0x0049a23d
    void (*fow_remove_sight)(uint32_t player, int32_t x, int32_t y, uint8_t radius); // @0x00496868
    void (*map_fow_UpdateFoWPlus)(uint32_t player, uint32_t x, uint32_t y,
                                  uint8_t sight); // @0x0049681a
    void (*unit_soldiers_set_heading)(uint16_t player, int32_t unit_index,
                                      uint8_t sprite_frame);                // @0x00489ab6
    int32_t (*unit_walk_step_allowed)(uint32_t player, int32_t unit_index); // @0x0048c79a
};

const unit_state_move_walker_calls &live_unit_state_move_walker_calls();

namespace detail {

// llm_strat_unit_state_move_walker @0x0047c903. See the header derivation above for the full shape.
// Zero-arg, ambient cur_player/cur_index/cur_unit, matching the original's void(void) signature.
void unit_state_move_walker(const sim_view &v, sim_store &own, const unit_state_move_walker_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_state_move_walker_calls(). Matches the
// committed prototype (sig_llm_strat_unit_state_move_walker) exactly.
void unit_state_move_walker();

namespace detail {
} // namespace detail

} // namespace mh::sim
