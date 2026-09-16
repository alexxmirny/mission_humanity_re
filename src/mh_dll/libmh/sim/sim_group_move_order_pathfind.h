#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -------------------------------------------------------------------------
//
// Indirected for offline testability (net_selftest.exe simtest), matching every other sim `calls`
// table. Every member is an ORIGINAL game function -- none is reimplemented here (house rule 3: the
// closure's shared helpers, including the sibling pathfinding primitives, stay original).
struct group_move_order_pathfind_calls {
    // llm_strat_pathfind_find_closer_visible_tile @0x0041fff5.
    int32_t (*find_closer_visible_tile)(uint8_t src_col, uint8_t src_row, uint32_t *out_col,
                                        uint32_t *out_row, int32_t dst_col, int32_t dst_row);
    // llm_strat_path_free_slot @0x004969e8.
    void (*path_free_slot)(uint16_t player, int32_t unit_index);
    // llm_strat_path_alloc_slot @0x0049a1ce.
    int32_t (*path_alloc_slot)(int32_t path_group_idx, int32_t entity_id);
    // llm_map_region_walk_to_valid_tile @0x004201e4.
    int32_t (*region_walk_to_valid_tile)(uint32_t src_col, uint32_t src_row, uint32_t dst_col,
                                         uint32_t dst_row, uint32_t *out_col, uint32_t *out_row);
    // llm_strat_tile_dist_wrapped @0x0049404e.
    int32_t (*tile_dist_wrapped)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
    // llm_strat_pathfind_build_steps @0x0041e836.
    int32_t (*pathfind_build_steps)(int32_t start_x, int32_t start_y, int32_t target_range,
                                    uint32_t unused_reserved, int32_t path_slot_index);
    // llm_strat_group_plan_formation_positions @0x0041dbfe.
    void (*group_plan_formation_positions)(uint8_t target_type_mask);
    // llm_strat_pathfind_target_hook_stub @0x004219dd.
    void (*pathfind_target_hook_stub)(int32_t target_col, int32_t target_row);
    // llm_map_region_find_route @0x00423d86.
    int32_t (*region_find_route)(uint32_t to_tile_idx, uint32_t from_tile_idx);
    // llm_strat_pathfind_mark_group_member_regions @0x00420919.
    void (*pathfind_mark_group_member_regions)();
    // llm_map_region_flood_reachable @0x00424ee0.
    int32_t (*region_flood_reachable)(int32_t query_cell, int32_t start_cell);
    // llm_map_region_route_search @0x004236c2.
    int32_t (*region_route_search)(uint16_t start_region, int16_t target_region, uint8_t *out_path);
    // llm_stack_capacity_guard_0x20 @0x004219b7 -- unconfirmed-lead empty-stub candidate elsewhere in
    // this codebase (see sim_group_move_order_commit.h's identical note); called for real regardless.
    void (*stack_capacity_guard_0x20)();
    // llm_map_region_find_nearest_valid_tile @0x0041ed6d.
    int32_t (*region_find_nearest_valid_tile)(uint8_t *col, uint8_t *row);
    // llm_strat_pathfind_plan_group_route @0x0042039c.
    int32_t (*pathfind_plan_group_route)(uint32_t start_col, uint32_t start_row, uint32_t dest_col,
                                         uint32_t dest_row, uint32_t *out_col, uint32_t *out_row);
    // llm_strat_pathfind_trace_route @0x0041ef3e.
    int32_t (*pathfind_trace_route)(uint8_t src_col, uint8_t src_row, uint8_t dst_col, uint8_t dst_row,
                                    void *step_ctx);
    // llm_strat_group_path_step_record @0x0041f7ef -- opaque; this is what actually appends to
    // PATH_BUFFERS and advances GROUP_PATH_BUILD_IDX.
    void (*group_path_step_record)(int32_t order_idx, uint32_t heading, int32_t col, int32_t row);
    // llm_strat_unit_path_queue_count @0x0041fea7.
    int32_t (*unit_path_queue_count)(int32_t unit_index, int32_t max_len);
};

const group_move_order_pathfind_calls &live_group_move_order_pathfind_calls();

// The logic over an explicit view + calls table, matching every other sim TU's split.
namespace detail {

// llm_strat_group_move_order_pathfind @0x0041ca37. See the header banner for the full shape.
void group_move_order_pathfind(const sim_view &v, sim_store &own,
                               const group_move_order_pathfind_calls &c, int32_t mode,
                               uint8_t target_mask);

} // namespace detail

// Public wrapper. Signature matches the committed prototype (sig_llm_strat_group_move_order_pathfind)
// exactly.
void group_move_order_pathfind(int32_t mode, uint8_t target_mask);

namespace detail {
} // namespace detail

} // namespace mh::sim
