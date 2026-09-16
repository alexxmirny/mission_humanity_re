#include "sim/sim_map_region_route_marking.h"

namespace mh::sim {

namespace detail {

int32_t region_neighbor_edge_value(const llm_map_region *node, const llm_map_region *target) {
    for (uint32_t i = 0; i < node->neighbor_count; ++i) {
        if (node->neighbors[i] == target) return static_cast<int32_t>(node->neighbor_data[i]);
    }
    return 0;
}

llm_map_region *region_first_shared_node(const llm_map_region *node_a, const llm_map_region *node_b) {
    for (uint32_t i = 0; i < node_a->neighbor_count; ++i) {
        for (uint32_t j = 0; j < node_b->neighbor_count; ++j) {
            if (node_a->neighbors[i] == node_b->neighbors[j]) return node_a->neighbors[i];
        }
    }
    return nullptr;
}

void region_route_mark_shared_nodes(llm_map_region *start) {
    llm_map_region *node = start;
    while (node != nullptr) {
        llm_map_region *next = node->route_parent; // 0x0042366f-0x00423678
        if (next != nullptr) {
            const int32_t edge = region_neighbor_edge_value(node, next);       // 0x00423687
            if (edge < 16) {                                                   // 0x0042368f-0x00423693 (JNC skip)
                llm_map_region *shared = region_first_shared_node(node, next); // 0x0042369b
                if (shared != nullptr) shared->route_mark = 2;                 // 0x004236ac
            }
        }
        // 0x00423661/0x00423664: the original re-reads node->route_parent a second time to advance --
        // provably identical to `next` (nothing above mutates node->route_parent), CSE'd to one read.
        node = next;
    }
}

void region_route_prepass(const sim_view &v) {
    // Genuine no-op -- see the header banner. Reproducing the dead read/loop would add nothing
    // observable (it cannot fault: i stays within [0, neighbor_count) which is always <= the fixed
    // 128-slot array) and the CMP's flags are never consumed by a branch, so there is nothing this
    // body could do differently by NOT looping. (void)v documents the parity-only parameter.
    (void)v;
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

int32_t region_neighbor_edge_value(int32_t node, int32_t target) {
    return detail::region_neighbor_edge_value(reinterpret_cast<const llm_map_region *>(static_cast<intptr_t>(node)),
                                              reinterpret_cast<const llm_map_region *>(
                                                  static_cast<intptr_t>(target)));
}

int32_t region_first_shared_node(int32_t node_a, int32_t node_b) {
    llm_map_region *shared = detail::region_first_shared_node(
        reinterpret_cast<const llm_map_region *>(static_cast<intptr_t>(node_a)),
        reinterpret_cast<const llm_map_region *>(static_cast<intptr_t>(node_b)));
    return static_cast<int32_t>(reinterpret_cast<intptr_t>(shared));
}

void region_route_mark_shared_nodes(int32_t start) {
    detail::region_route_mark_shared_nodes(
        reinterpret_cast<llm_map_region *>(static_cast<intptr_t>(start)));
}

void region_route_prepass() {
    sim_state st = state();
    detail::region_route_prepass(st.read);
}

} // namespace mh::sim
