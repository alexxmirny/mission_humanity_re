//
// sim_map_region_route_marking_selftest.cpp -- `simtest` offline oracle for the four functions in
// sim/sim_map_region_route_marking.h (RI-SIM / SIM1-G2), all NOT SHADOWABLE (0 tracked
// write cells per the write-closure derivation -- route_mark_shared_nodes's only write is a
// heap-node field, route_prepass writes nothing at all), so this is their only execution-proof
// evidence, alongside adversarial review.
//
#include "sim/sim_map_region_route_marking.h"

#include <cstring>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {
using namespace mh::sim;
} // namespace

void run_map_region_route_marking_tests() {
    // ---- llm_map_region_neighbor_edge_value -----------------------------------------------------
    {
        llm_map_region node{}, target{}, dummyA{}, dummyB{};
        node.neighbor_count   = 3;
        node.neighbors[0]     = &dummyA;
        node.neighbor_data[0] = 5;
        node.neighbors[1]     = &target;
        node.neighbor_data[1] = 10;
        node.neighbors[2]     = &dummyB;
        node.neighbor_data[2] = 7;

        ck_eq((uint32_t)detail::region_neighbor_edge_value(&node, &target), 10u,
              "neighbor_edge_value: found at index 1 -> neighbor_data[1]");

        llm_map_region notANeighbor{};
        ck_eq((uint32_t)detail::region_neighbor_edge_value(&node, &notANeighbor), 0u,
              "neighbor_edge_value: not in the list -> 0");
    }

    // ---- llm_map_region_first_shared_node -------------------------------------------------------
    {
        llm_map_region nodeA{}, nodeB{}, shared{}, onlyA{}, onlyB{};
        nodeA.neighbor_count = 2;
        nodeA.neighbors[0]   = &onlyA;
        nodeA.neighbors[1]   = &shared;
        nodeB.neighbor_count = 2;
        nodeB.neighbors[0]   = &onlyB;
        nodeB.neighbors[1]   = &shared;

        ck((detail::region_first_shared_node(&nodeA, &nodeB) == &shared),
           "first_shared_node: finds the one common neighbor");

        llm_map_region nodeC{};
        nodeC.neighbor_count = 1;
        nodeC.neighbors[0]   = &onlyB; // disjoint from nodeA's list
        ck((detail::region_first_shared_node(&nodeA, &nodeC) == nullptr),
           "first_shared_node: no common neighbor -> nullptr");

        // Outer-i-first / inner-j-second order: nodeD shares BOTH onlyA (nodeA index 0) and shared
        // (nodeA index 1) -- must return the OUTER-index-0 match (onlyA), not the later one.
        llm_map_region nodeD{};
        nodeD.neighbor_count = 2;
        nodeD.neighbors[0]   = &shared; // inner index 0
        nodeD.neighbors[1]   = &onlyA;  // inner index 1
        ck((detail::region_first_shared_node(&nodeA, &nodeD) == &onlyA),
           "first_shared_node: outer index 0 (onlyA) wins even though shared is found at inner "
           "index 0 first for nodeA's LATER outer index");
    }

    // ---- llm_map_region_route_mark_shared_nodes ---------------------------------------------------
    {
        // Case 1: a 2-node ->route_parent chain, edge<16, one common neighbor -> gets route_mark=2.
        llm_map_region node0{}, node1{}, sharedNeighbor{}, otherA{}, otherB{};
        node0.route_parent        = &node1;
        node1.route_parent        = nullptr; // chain terminates here
        node0.neighbor_count      = 3;
        node0.neighbors[0]        = &sharedNeighbor;
        node0.neighbors[1]        = &node1;
        node0.neighbor_data[1]    = 10; // edge value < 16
        node0.neighbors[2]        = &otherA;
        node1.neighbor_count      = 3;
        node1.neighbors[0]        = &otherB;
        node1.neighbors[1]        = &sharedNeighbor;
        node1.neighbors[2]        = &otherA;
        sharedNeighbor.route_mark = 0;

        detail::region_route_mark_shared_nodes(&node0);
        ck_eq((uint32_t)sharedNeighbor.route_mark, 2u,
              "route_mark_shared_nodes: shared neighbor of an adjacent chain pair gets route_mark=2");
        ck_eq((uint32_t)otherA.route_mark, 0u,
              "route_mark_shared_nodes: a non-shared neighbor stays untouched");

        // Case 2: edge>=16 -- must SKIP the shared-node lookup entirely, even though a shared
        // neighbor genuinely exists (a mutation that drops the guard would wrongly mark it).
        llm_map_region node2{}, node3{}, wouldBeShared{};
        node2.route_parent       = &node3;
        node3.route_parent       = nullptr;
        node2.neighbor_count     = 2;
        node2.neighbors[0]       = &node3;
        node2.neighbor_data[0]   = 20; // edge value >= 16 -> skip
        node2.neighbors[1]       = &wouldBeShared;
        node3.neighbor_count     = 1;
        node3.neighbors[0]       = &wouldBeShared;
        wouldBeShared.route_mark = 0;

        detail::region_route_mark_shared_nodes(&node2);
        ck_eq((uint32_t)wouldBeShared.route_mark, 0u,
              "route_mark_shared_nodes: edge>=16 skips the mark even though a shared neighbor exists");

        // Case 3: no common neighbor at all -- must not crash, must not mark anything.
        llm_map_region node4{}, node5{}, unrelated{};
        node4.route_parent     = &node5;
        node5.route_parent     = nullptr;
        node4.neighbor_count   = 1;
        node4.neighbors[0]     = &node5;
        node4.neighbor_data[0] = 1;
        node5.neighbor_count   = 1;
        node5.neighbors[0]     = &unrelated;
        unrelated.route_mark   = 0;

        detail::region_route_mark_shared_nodes(&node4);
        ck_eq((uint32_t)unrelated.route_mark, 0u,
              "route_mark_shared_nodes: no shared neighbor -> nothing marked, no crash");

        // Case 4: route_parent==nullptr immediately (start of chain has no next) -- must return
        // without touching anything.
        llm_map_region lone{};
        lone.route_parent = nullptr;
        detail::region_route_mark_shared_nodes(&lone); // must not crash
        ck(true, "route_mark_shared_nodes: a start node with no route_parent is a safe no-op");
    }

    // ---- llm_map_region_route_prepass: genuine no-op -- snapshot every field, call, compare -------
    {
        sim_fixture fx;
        fx.reset();

        llm_map_region regionA{}, regionB{};
        regionA.index            = 1;
        regionA.cell_count       = 10;
        regionA.neighbor_count   = 2;
        regionA.neighbors[0]     = &regionB;
        regionA.neighbor_data[0] = 3;
        regionA.next             = &regionB;
        regionA.route_bfs_dist   = 7;
        regionA.route_parent     = nullptr;
        regionA.route_mark       = 1;

        regionB.index            = 2;
        regionB.cell_count       = 20;
        regionB.neighbor_count   = 1;
        regionB.neighbors[0]     = &regionA;
        regionB.neighbor_data[0] = 4;
        regionB.next             = nullptr;
        regionB.route_bfs_dist   = 8;
        regionB.route_parent     = &regionA;
        regionB.route_mark       = 2;

        fx.region_list_head = &regionA;
        sim_view v          = fx.view();

        const llm_map_region snapA = regionA;
        const llm_map_region snapB = regionB;

        detail::region_route_prepass(v);

        ck((std::memcmp(&regionA, &snapA, sizeof(llm_map_region)) == 0),
           "route_prepass: regionA byte-for-byte unchanged (genuine no-op)");
        ck((std::memcmp(&regionB, &snapB, sizeof(llm_map_region)) == 0),
           "route_prepass: regionB byte-for-byte unchanged (genuine no-op)");
    }
}

} // namespace mh::sim::test
