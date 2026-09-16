#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. All
// eleven are ORIGINAL functions outside this batch -- none need reimplementing here;
// live_unit_group_step_plane_calls() is the only binder. Signatures copied verbatim from
// addr/mh_calls.gen.h. Out-params carry the committed mh::call:: pointee type exactly (TACT1-P C6,
// 2026-09-04) -- the aggregate-initializer binding in live_unit_group_step_plane_calls() needs an
// exact function-pointer-type match.
struct unit_group_step_plane_calls {
    int32_t (*path_find_free_slot)(int32_t player);              // @0x00495f1b
    void (*path_free_slot)(uint16_t player, int32_t unit_index); // @0x004969e8
    void (*target_release_ref)(uint32_t player_idx, int32_t unit_idx,
                               uint32_t mode); // @0x004dac44
    void (*unit_get_coords)(uint16_t player, int32_t unit_index, int32_t *out_fine_x,
                            int32_t *out_fine_y);                         // @0x0044b141
    void (*unit_set_state_order)(uint16_t new_order, uint16_t new_state); // @0x00486657
    void (*unit_set_state)(uint16_t new_state);                           // @0x004866c9
    int32_t (*unit_get_ready_home_building)();                            // @0x00484a14
    uint8_t *(*trace_greedy_path)(int32_t start_col, int32_t start_row, int32_t mode, int32_t goal_col,
                                  int32_t goal_row, int32_t heading); // __cdecl @0x0066a9ac
    int32_t (*pathtrace_remove_loops)();                              // @0x0066b376
    void (*path_write_from_solver)(uint32_t player, int32_t unit_index, uint32_t src_x, uint32_t src_y,
                                   int32_t free_slot); // @0x0049581d
    void (*unit_notify_status)(uint32_t player, int32_t unit_index,
                               uint32_t status_code); // @0x004dae0e
};

const unit_group_step_plane_calls &live_unit_group_step_plane_calls();

namespace detail {

// llm_strat_unit_group_step_plane @0x00484b4a. See the header derivation above for the full shape.
// Zero-arg, ambient cur_player/cur_index/cur_unit, matching the original's void(void) signature.
void unit_group_step_plane(const sim_view &v, sim_store &own, const unit_group_step_plane_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_group_step_plane_calls(). Matches the
// committed prototype (sig_llm_strat_unit_group_step_plane) exactly.
void unit_group_step_plane();

namespace detail {
} // namespace detail

} // namespace mh::sim
