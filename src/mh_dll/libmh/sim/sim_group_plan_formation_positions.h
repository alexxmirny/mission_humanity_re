#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. All five
// signatures copied verbatim from addr/mh_calls.gen.h.
struct group_plan_formation_positions_calls {
    int32_t (*tile_dist_wrapped)(int32_t x1, int32_t y1, int32_t x2,
                                 int32_t y2); // llm_strat_tile_dist_wrapped @0x0049404e
    int32_t (*pathfind_build_steps)(int32_t start_x, int32_t start_y, int32_t target_range,
                                    uint32_t unused_reserved,
                                    int32_t  path_slot_index); // llm_strat_pathfind_build_steps @0x0041e836
    int32_t (*region_flood_reachable)(int32_t query_cell,
                                      int32_t start_cell); // llm_map_region_flood_reachable @0x00424ee0
    int32_t (*region_route_search)(uint16_t start_region, int16_t target_region,
                                   uint8_t *out_path); // llm_map_region_route_search @0x004236c2
    void (*stack_capacity_guard_0x20)();               // llm_stack_capacity_guard_0x20 @0x004219b7
};

const group_plan_formation_positions_calls &live_group_plan_formation_positions_calls();

namespace detail {

// llm_strat_group_plan_formation_positions @0x0041dbfe. See the header derivation above for the full
// shape. Reads v.group_member_count, v.group_move_scratch, v.units, v.cfg_weapons, v.group_anchor_x/_y,
// v.path_wrap_mask, v.region_list_head, v.group_route_steps, v.cur_player; writes region-graph heap
// nodes' `->route_mark` directly (address escape, not a sim_store region), own.path_buffer_at(),
// own.group_route_step_at(0) (via the out-param pointer handed to region_route_search), and (DECLARED
// NEED) own.group_member_tile_byte().
void group_plan_formation_positions(const sim_view &v, sim_store &own,
                                    const group_plan_formation_positions_calls &c, uint8_t target_type_mask);

} // namespace detail

// Public wrapper. Signature matches the committed prototype (llm_strat_group_plan_formation_positions
// in addr/mh_calls.gen.h) exactly.
void group_plan_formation_positions(uint8_t target_type_mask);

namespace detail {
} // namespace detail

} // namespace mh::sim
