#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_pathfind_route_leg_reconcile @0x00420616. See the header derivation above for the full
// shape. Reads own.group_move_scratch_at(i).{tile_col,tile_row,wave_rank}, v.map_width, v.map_height,
// v.passable, own.region_cell_at(x,y).region->index; writes own.path_wrap_mask_mut() and
// own.group_move_scratch_at(j).wave_rank.
int32_t pathfind_route_leg_reconcile(const sim_view &v, sim_store &own, int32_t step_count);

} // namespace detail

// Public wrapper. Signature matches the committed prototype (sig_llm_strat_pathfind_route_leg_reconcile)
// exactly.
int32_t pathfind_route_leg_reconcile(int32_t step_count);

namespace detail {
} // namespace detail

} // namespace mh::sim
