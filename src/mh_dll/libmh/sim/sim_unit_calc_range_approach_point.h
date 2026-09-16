#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// target_class is an ALREADY-REIMPLEMENTED sibling (sim_target_class.h), bound to its own public
// wrapper. tile_dist_wrapped/wrap_delta_x/wrap_delta_y are ORIGINAL functions outside this batch.
struct unit_calc_range_approach_point_calls {
    int32_t (*target_class)(uint32_t owner_and_kind_flag,
                            int32_t  roster_slot);                                 // mh::sim::target_class (sibling) @0x004495f9
    int32_t (*tile_dist_wrapped)(int32_t x1, int32_t y1, int32_t x2, int32_t y2); // @0x0049404e
    int32_t (*wrap_delta_x)(int32_t pos_a, uint32_t unused_param, int32_t pos_b); // llm_map_wrap_delta_x @0x004940a9
    int32_t (*wrap_delta_y)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);      // llm_map_wrap_delta_y @0x00494131
};

const unit_calc_range_approach_point_calls &live_unit_calc_range_approach_point_calls();

namespace detail {

// llm_strat_unit_calc_range_approach_point @0x00488291. See the header banner above for the full
// derivation, including the DECLARED DIVERGENCE on the uninitialised `best_delta`-update edge case.
// Pure read of sim state (no `own`) -- the only writes are through the caller's own out-pointers.
int32_t unit_calc_range_approach_point(const sim_view &v, const unit_calc_range_approach_point_calls &c,
                                       int32_t *io_target_x, int32_t *io_target_y);

} // namespace detail

// Live wrapper: the logic applied to state().read and live_unit_calc_range_approach_point_calls().
// Matches the committed prototype (sig_llm_strat_unit_calc_range_approach_point) exactly -- the
// committed row is `uint *io_target_x, uint *io_target_y` (signed vs. unsigned int, same width);
// detail::unit_calc_range_approach_point keeps its verified `int32_t *` params, so this wrapper
// reinterpret_casts into it (TACT1-P C6, 2026-09-04).
int32_t unit_calc_range_approach_point(uint32_t *io_target_x, uint32_t *io_target_y);

} // namespace mh::sim
