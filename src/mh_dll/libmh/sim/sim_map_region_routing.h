#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The two ORIGINAL callees `llm_map_region_find_route` still calls out to. Indirected for the same
// reason as every other multi-callee region TU in this batch (sim_map_region_split.h,
// sim_map_region_helpers.h, sim_group_plan_formation_positions.h): a direct `mh::call::` inside a
// `detail::` body reaches into the live game image, which makes the body untestable by
// `net_selftest.exe simtest`.
struct map_region_routing_calls {
    void (*route_prepass)();                  // llm_map_region_route_prepass @0x00424e6e, no args
    void (*route_mark_shared_nodes)(int32_t); // llm_map_region_route_mark_shared_nodes @0x00423638
};

const map_region_routing_calls &live_map_region_routing_calls();

namespace detail {

// llm_map_region_route_search @0x004236c2. See the header banner above for the full derivation,
// including the shipped direction-7 out-of-bounds read (DECLARED NEED #3) and the scratch-slot
// byte-layout disagreement with the existing manifest note (uncertainties[]). `out_path` is a
// caller-owned buffer of {dir_code, run_length} byte pairs, written directly (NOT through
// sim_store) -- same posture as `pathfind_plan_group_route`'s `out_col`/`out_row`.
int32_t region_route_search(const sim_view &v, sim_store &own, uint16_t start_region,
                            int16_t target_region, uint8_t *out_path);

// llm_map_region_find_route @0x00423d86. See the header banner above.
int32_t region_find_route(const sim_view &v, sim_store &own, const map_region_routing_calls &c,
                          uint32_t to_tile_idx, uint32_t from_tile_idx);

// llm_map_region_flood_reachable @0x00424ee0. See the header banner above.
int32_t region_flood_reachable(const sim_view &v, sim_store &own, int32_t query_cell,
                               int32_t start_cell);

} // namespace detail

// Public wrappers. Signatures match the committed prototypes / mh::exp::sig_llm_map_region_*
// exactly (`out_path` carries the committed `uint8_t *` pointee -- TACT1-P C6, 2026-09-04).
int32_t region_route_search(uint16_t start_region, int16_t target_region, uint8_t *out_path);
int32_t region_find_route(uint32_t to_tile_idx, uint32_t from_tile_idx);
int32_t region_flood_reachable(int32_t query_cell, int32_t start_cell);

namespace detail {
} // namespace detail

} // namespace mh::sim
