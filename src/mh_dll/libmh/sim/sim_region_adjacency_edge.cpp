//
// sim/sim_region_adjacency_edge.cpp -- see sim_region_adjacency_edge.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_map_region_add_adjacency_edge_00422e09.asm). The single-index-reused-
// across-both-loops control flow and the symmetric double-sided update were re-derived from the
// listing per house rules.
//
#include "sim/sim_region_adjacency_edge.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void region_add_adjacency_edge(llm_map_region *region_a, llm_map_region *region_b) {
    // The original walks region_a with a single signed index that it then REUSES for the inner
    // region_b scan (see the header quirk note). `i` mirrors local_18 exactly.
    int32_t i = 0;
    for (;;) {
        // 0x00422e??: append branch -- region_b was not already a neighbor of region_a.
        if (static_cast<int32_t>(region_a->neighbor_count) <= i) {
            region_a->neighbors[region_a->neighbor_count]     = region_b;
            region_a->neighbor_data[region_a->neighbor_count] = 1;
            region_a->neighbor_count                          = region_a->neighbor_count + 1;
            region_b->neighbors[region_b->neighbor_count]     = region_a;
            region_b->neighbor_data[region_b->neighbor_count] = 1;
            region_b->neighbor_count                          = region_b->neighbor_count + 1;
            return;
        }
        // Found region_b in region_a's list: bump region_a's weight, then find region_a in region_b's
        // list and bump that too (the symmetric increment).
        if (region_a->neighbors[i] == region_b) {
            region_a->neighbor_data[i] = region_a->neighbor_data[i] + 1;
            for (i = 0; i < static_cast<int32_t>(region_b->neighbor_count); i = i + 1) {
                if (region_b->neighbors[i] == region_a) {
                    region_b->neighbor_data[i] = region_b->neighbor_data[i] + 1;
                    return;
                }
            }
            // Inner scan fell through (not reachable for a symmetric graph): i == neighbor_count and
            // the outer scan resumes from there via the increment below -- the original's exact
            // behaviour, not a bug this translation introduces.
        }
        i = i + 1;
    }
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void region_add_adjacency_edge(void *region_a, void *region_b) {
    detail::region_add_adjacency_edge(static_cast<llm_map_region *>(region_a),
                                      static_cast<llm_map_region *>(region_b));
}


// ---- the rebind ABI shim -------------------------------------------------------------------------
//
// The committed export prototype types both parameters `mh_llm_map_region *`; the public wrapper
// above takes `void *`. Pointer identity, but the binder's static_assert compares types exactly, so
// the typed shape has to exist. It lived beside the differential oracle until F2D retired it, and
// was never part of it -- this is signature adaptation for the binder.
namespace rebind_arm {

void region_add_adjacency_edge(mh::game::mh_llm_map_region *region_a,
                               mh::game::mh_llm_map_region *region_b) {
    mh::sim::region_add_adjacency_edge(reinterpret_cast<void *>(region_a),
                                       reinterpret_cast<void *>(region_b));
}

} // namespace rebind_arm

} // namespace mh::sim
