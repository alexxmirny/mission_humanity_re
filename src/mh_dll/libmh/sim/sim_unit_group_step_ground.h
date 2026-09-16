#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. All
// twenty-one are ORIGINAL entry points (one of them, group_scratch_compute_centroid, is already
// shadow-replaced by this module's own reimplementation, which is exactly why it is still reached
// through its game address rather than called directly). Signatures copied VERBATIM from
// addr/mh_calls.gen.h -- the aggregate-initializer binding in live_unit_group_step_ground_calls()
// needs an exact function-pointer-type match, so the out-params carry the committed pointee type
// (TACT1-P C6, 2026-09-04).
struct unit_group_step_ground_calls {
    void (*unit_get_coords)(uint16_t player, int32_t unit_index, int32_t *out_fine_x,
                            int32_t *out_fine_y); // @0x0044b141
    void (*bldg_get_coords)(uint16_t player, int32_t building_index, int32_t *out_fine_x,
                            int32_t *out_fine_y);                         // @0x00449b8a
    void (*unit_set_state_order)(uint16_t new_order, uint16_t new_state); // @0x00486657
    void (*unit_set_state)(uint16_t new_state);                           // @0x004866c9
    void (*unit_set_state_of)(int32_t player, int32_t unit_index,
                              int16_t new_state);                                        // @0x004869b0
    int32_t (*unit_calc_range_approach_point)(uint32_t *io_tile_x, uint32_t *io_tile_y); // @0x00488291
    void (*target_release_ref)(uint32_t player_idx, int32_t unit_idx, uint32_t mode);    // @0x004dac44
    void (*group_scratch_add_unit_and_normalize_heading)(int32_t player, int32_t unit_index,
                                                         int32_t *io_count); // @0x0048d283
    void (*group_scratch_compute_centroid)(int32_t player, int32_t member_count, int32_t *out_x,
                                           int32_t *out_y); // @0x0048d36f (already migrated + armed)
    int32_t (*unit_get_ready_home_building)();              // @0x00484a14
    uint8_t *(*trace_greedy_path)(int32_t start_col, int32_t start_row, int32_t mode, int32_t goal_col,
                                  int32_t goal_row, int32_t heading); // __cdecl @0x0066a9ac
    int32_t (*pathtrace_remove_loops)();                              // @0x0066b376
    int32_t (*path_find_free_slot)(int32_t player);                   // @0x00495f1b
    void (*path_free_slot)(uint16_t player, int32_t unit_index);      // @0x004969e8
    void (*path_write_from_solver)(uint32_t player, int32_t unit_index, uint32_t src_x,
                                   uint32_t src_y, int32_t free_slot);               // @0x0049581d
    int32_t (*heading_candidate_find_slot)(int32_t heading, int32_t turn_delta);     // @0x0048d2f4
    void (*unit_unlink_tile)(uint32_t unit_player, uint16_t unit_index);             // @0x00486f41
    void (*fow_remove_sight)(uint32_t player, int32_t x, int32_t y, uint8_t radius); // @0x00496868
    void (*map_unit_PutOnMap)(uint16_t player, uint16_t b_id, uint8_t x, uint8_t y); // @0x00486c6a
    void (*map_fow_UpdateFoWPlus)(uint32_t player, uint32_t x, uint32_t y,
                                  uint8_t sight); // @0x0049681a
    void (*unit_notify_status)(uint32_t player, int32_t unit_index,
                               uint32_t status_code); // @0x004dae0e
};

const unit_group_step_ground_calls &live_unit_group_step_ground_calls();

namespace detail {

// llm_strat_unit_group_step_ground @0x00483011. See the header derivation above for the full shape.
// Zero-arg, ambient cur_player/cur_index/cur_unit, matching the original's void(void) signature.
void unit_group_step_ground(const sim_view &v, sim_store &own,
                            const unit_group_step_ground_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_group_step_ground_calls(). Matches the
// committed prototype (sig_llm_strat_unit_group_step_ground) exactly.
void unit_group_step_ground();

namespace detail {
} // namespace detail

} // namespace mh::sim
