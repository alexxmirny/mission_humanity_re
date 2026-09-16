#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call this function makes -- see the header banner above for why it is indirected
// (a direct `mh::call::` inside a `detail::` body would make it untestable by net_selftest.exe
// simtest) and for what its own write surface is now known to be.
struct map_region_recompute_adjacency_calls {
    void (*add_adjacency_edge)(mh::game::mh_llm_map_region *region_a, mh::game::mh_llm_map_region *region_b); // llm_map_region_add_adjacency_edge @0x00422e09 -- typed by the committed prototype (was void*; synced 2026-08-19)
};

const map_region_recompute_adjacency_calls &live_map_region_recompute_adjacency_calls();

namespace detail {

// llm_map_region_recompute_adjacency @0x00422f0c. See the header banner for the full derivation.
void region_recompute_adjacency(const sim_view &v, sim_store &own,
                                const map_region_recompute_adjacency_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_map_region_recompute_adjacency_calls().
// Matches the original's committed `void __watcall(void)` shape.
void region_recompute_adjacency();

namespace detail {
} // namespace detail

} // namespace mh::sim
