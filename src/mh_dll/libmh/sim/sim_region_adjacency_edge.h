//
// sim/sim_region_adjacency_edge.h -- add/increment one bidirectional region-adjacency edge
// (RI-SIM / SIM1F):
//
//   llm_map_region_add_adjacency_edge   @0x00422e09 (0x103 bytes)
//
// `void __watcall llm_map_region_add_adjacency_edge(llm_map_region *region_a, llm_map_region
// *region_b)`. Called once per right/down tile pair that crosses a region boundary by
// llm_map_region_recompute_adjacency to build the region-adjacency graph. Searches region_a's
// neighbor list for region_b; if found, increments the shared-border-length weight
// (neighbor_data[]) on BOTH sides symmetrically; otherwise appends a new edge (weight 1) to both
// regions' 128-slot neighbors[]/neighbor_data[] arrays and bumps both neighbor_count fields.
//
// Its entire write surface is the two caller-provided HEAP-RESIDENT llm_map_region nodes -- there is
// no fixed-VA sim region and no sim_view/sim_store state involved, so the translation dereferences
// the two `llm_map_region*` arguments directly (same posture as its caller
// sim_map_region_recompute_adjacency.cpp, whose own shadow entry documents that the heap graph is
// not a snapshot-able region). No callees (only the inert assert_stack_capacity prologue), so no
// `_calls` struct.
//
// FAITHFULLY-REPRODUCED QUIRK: the loop reuses one index variable for both the outer scan of
// region_a and the inner scan of region_b (0x00422e?? -- see the .asm). If the inner scan fails to
// find region_a in region_b's list (which cannot happen for a well-formed symmetric graph), the
// outer scan RESUMES at index == region_b->neighbor_count rather than where it left off. Reproduced
// exactly rather than "cleaned up", per house rules.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_map_region_add_adjacency_edge @0x00422e09. Operates only on the two argument nodes; no view,
// no store, no callees.
void region_add_adjacency_edge(llm_map_region *region_a, llm_map_region *region_b);


} // namespace detail

// Public wrapper. Signature matches the committed prototype in addr/mh_calls.gen.h exactly
// (`void(void*, void*)`).
void region_add_adjacency_edge(void *region_a, void *region_b);

namespace rebind_arm {
void region_add_adjacency_edge(mh::game::mh_llm_map_region *region_a,
                               mh::game::mh_llm_map_region *region_b);
} // namespace rebind_arm

} // namespace mh::sim
