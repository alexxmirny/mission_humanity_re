#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The two outward calls llm_map_merge_small_regions makes -- see the header banner on why they are
// indirected rather than called directly from detail::. llm_map_region_pick_smaller has no callees.
struct map_region_helpers_calls {
    void (*region_free)(uint32_t region_ptr); // llm_map_region_free @0x0042239a
    void (*region_recompute_adjacency)();     // llm_map_region_recompute_adjacency @0x00422f0c
};

const map_region_helpers_calls &live_map_region_helpers_calls();

namespace detail {

// llm_map_region_pick_smaller @0x004229c3. See the header banner for the full derivation. No sim_view
// needed -- nothing here reads anything but the grid cell, via sim_store::region_cell_at().
llm_map_region *region_pick_smaller(sim_store &own, llm_map_region *block, int32_t x, int32_t y);

// llm_map_merge_small_regions @0x004230f7. See the header banner for the full derivation.
void merge_small_regions(const sim_view &v, sim_store &own, const map_region_helpers_calls &c);

} // namespace detail

// Public wrappers. `region_pick_smaller` takes/returns `void *` (not `llm_map_region *`) to match
// sig_llm_map_region_pick_smaller exactly -- the same reason sim_game_insert_item_in_player_array.h's
// `void *arr` param does: the shadow/export thunk machinery assigns this function directly to a
// pointer-to-function variable of that exact type, and mh_calls.gen.h's own committed prototype for the
// The committed prototype types both the parameter and the return as the real struct pointer
// (`llm_map_region *`, i.e. mh::game::mh_llm_map_region *), so the public wrapper carries it and the
// four neighbor-direction calls in llm_map_region_apply_area need no cast. The earlier `void *` here
// was the generator's blunting, not the ABI's -- see TACT1-P C6, 2026-09-04. The header banner's
// "NAMING" note says why this is `region_pick_smaller`, not `pick_smaller`.
llm_map_region *region_pick_smaller(llm_map_region *block, int32_t x, int32_t y);
void            merge_small_regions();

namespace detail {
} // namespace detail

} // namespace mh::sim
