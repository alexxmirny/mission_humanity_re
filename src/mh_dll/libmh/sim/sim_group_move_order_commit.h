#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. Both
// signatures copied verbatim from addr/mh_calls.gen.h.
struct group_move_order_commit_calls {
    void (*group_move_order_pathfind)(int32_t mode,
                                      uint8_t target_mask); // llm_strat_group_move_order_pathfind @0x0041ca37
    void (*stack_capacity_guard_0x20)();                    // llm_stack_capacity_guard_0x20 @0x004219b7
};

const group_move_order_commit_calls &live_group_move_order_commit_calls();

// The logic over an explicit view + calls table, matching every other sim TU's split.
namespace detail {

// llm_strat_group_move_order_commit @0x0041c86c. See the header derivation above for the full shape.
// Reads v.group_move_scratch (.tile_col/.tile_row/.unit_idx), v.passable, v.map_width,
// v.group_member_tile; writes own.group_route_step_at(0), own.path_wrap_mask_mut(),
// own.group_order_goal_x_mut()/_y_mut(), own.group_anchor_x_mut()/_y_mut(),
// own.group_member_count_mut(), own.group_member_at(i), own.group_order_owner_mut(), and (step 8 only)
// own.group_move_scratch_at(i).tile_col/.tile_row.
void group_move_order_commit(const sim_view &v, sim_store &own, const group_move_order_commit_calls &c,
                             int32_t goal_x, int32_t goal_y, int32_t player_id, int32_t member_count,
                             int32_t move_group_id, uint32_t is_plain_move_flag, uint32_t target_class);

} // namespace detail

// Public wrapper. Signature matches the committed prototype (sig_llm_strat_group_move_order_commit)
// exactly.
void group_move_order_commit(int32_t goal_x, int32_t goal_y, int32_t player_id, int32_t member_count,
                             int32_t move_group_id, uint32_t is_plain_move_flag, uint32_t target_class);

namespace detail {
} // namespace detail

} // namespace mh::sim
