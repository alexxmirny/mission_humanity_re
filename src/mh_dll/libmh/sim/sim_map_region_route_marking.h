//
// sim/sim_map_region_route_marking.h -- four small map-region-graph query/mark primitives (RI-SIM /
// SIM1-G2), the last of SIM1-G2's own pathfinding-backend residue:
//
//   llm_map_region_neighbor_edge_value    @0x0042352e (0x73 bytes)
//   llm_map_region_first_shared_node      @0x004235a1 (0x97 bytes)
//   llm_map_region_route_mark_shared_nodes @0x00423638 (0x8a bytes)
//   llm_map_region_route_prepass          @0x00424e6e (0x6d bytes)
//
// All four are pure HEAP-NODE-GRAPH functions on `llm_map_region` -- no sim_view/sim_store field
// they write (route_mark_shared_nodes's only write, `route_mark`, is a heap-node field with no
// sim_store accessor, "region-pool architecture" posture per sim_map_region_split.h /
// sim_region_adjacency_edge.h), so `tmp/state_matrix.json`'s write-closure sees 0 write cells for
// all four -- matches the batch's own write-set preflight (NOT SHADOWABLE). Evidence is an offline
// oracle (three of the four need no sim_view/sim_store fixture at all, only hand-built
// `llm_map_region` nodes) plus adversarial review, same posture as
// sim_region_adjacency_edge.h's own heap-only function.
//
// ---- llm_map_region_neighbor_edge_value @0x0042352e --------------------------------------------
// `int __watcall llm_map_region_neighbor_edge_value(llm_map_region *node, llm_map_region *target)`
// (params committed as int32_t in addr/mh_calls.gen.h; pointer-as-int32 per the established idiom
// sim_map_region_routing.cpp's route_mark_shared_nodes(to_region) call site uses). Linear scan of
// `node->neighbors[0..neighbor_count)`; on the first index where `neighbors[i]==target`, returns
// `node->neighbor_data[i]` (the shared-border tile-edge count). Returns 0 if `target` is not in
// `node`'s neighbor list at all.
//
// ---- llm_map_region_first_shared_node @0x004235a1 -----------------------------------------------
// `llm_map_region * __watcall llm_map_region_first_shared_node(llm_map_region *node_a,
// llm_map_region *node_b)`. Nested scan, node_a's neighbor index OUTER (i), node_b's neighbor index
// INNER (j, reset to 0 each outer iteration): the FIRST pair where `node_a->neighbors[i] ==
// node_b->neighbors[j]` returns that shared pointer. Returns nullptr if no common neighbor exists in
// either list.
//
// ---- llm_map_region_route_mark_shared_nodes @0x00423638 ------------------------------------------
// `void __watcall llm_map_region_route_mark_shared_nodes(llm_map_region *start)`. Walks the
// `->route_parent` chain from `start` (the struct comment: "walked BACKWARD from goal to
// reconstruct the path and FORWARD as a 'next' by route_mark_shared_nodes over the reconstructed
// chain" -- this IS that walk). For each consecutive pair (node, next=node->route_parent) with
// next!=nullptr: if `neighbor_edge_value(node,next) < 16` (the border-length-count field, an
// unsigned JNC-guarded compare in the asm), look up `shared = first_shared_node(node,next)`; if
// found, `shared->route_mark = 2` (offset 0x418, matching the struct's own documented "sets 2 for a
// shared neighbor of two adjacent path regions"). 0x00423661/0x00423664 re-derive `node->route_parent`
// a SECOND time to advance the outer walk -- CSE'd here as a single `next` read (nothing between the
// two reads in the original mutates `node->route_parent`, so both reads are provably identical).
//
// ---- llm_map_region_route_prepass @0x00424e6e ------------------------------------------------------
// `void __watcall llm_map_region_route_prepass(void)`. Walks EVERY region in `*v.region_list_head`'s
// `->next` chain and, for each, loops `i` from 0 to `neighbor_count`, reading `neighbors[i]` each
// time -- but the CMP that reads it (0x00424ec9) is followed by an UNCONDITIONAL JMP, never a Jcc:
// the loaded value's flags are never consumed by any branch, and nothing in the body writes to any
// field, local, or global that survives the loop. Re-traced the CFG twice to confirm no missed edge
// (see uncertainties[]/dead-ends note below) -- this is a genuine, total no-op, same class as the
// PlayerSide dead-flags-read SIM1-G2 found and deliberately did NOT reproduce. The
// detail:: body below is empty; the offline oracle proves it by snapshotting a small region list's
// every field, calling it, and asserting byte-for-byte equality.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_map_region_neighbor_edge_value @0x0042352e. Pure; no sim_view/sim_store, no callees.
int32_t region_neighbor_edge_value(const llm_map_region *node, const llm_map_region *target);

// llm_map_region_first_shared_node @0x004235a1. Pure; no sim_view/sim_store, no callees.
llm_map_region *region_first_shared_node(const llm_map_region *node_a, const llm_map_region *node_b);

// llm_map_region_route_mark_shared_nodes @0x00423638. Writes only heap-node `route_mark` fields (no
// sim_view/sim_store needed); calls the two functions above directly (both are translated in this
// same slice -- no live-image seam needed for an internal-to-batch call).
void region_route_mark_shared_nodes(llm_map_region *start);

// llm_map_region_route_prepass @0x00424e6e. See the header banner: a genuine no-op. Takes the
// sim_view only for signature parity with every other detail:: function in this closure and so the
// offline oracle can inject a test region list via sim_test_support's fixture; the body reads
// nothing observable and writes nothing.
void region_route_prepass(const sim_view &v);

} // namespace detail

// Public wrappers. Pointer<->int32 casts at the boundary match addr/mh_calls.gen.h's committed
// int32_t signatures for the first three; route_prepass takes no params (matches the committed
// void(void) signature exactly).
int32_t region_neighbor_edge_value(int32_t node, int32_t target);
int32_t region_first_shared_node(int32_t node_a, int32_t node_b);
void    region_route_mark_shared_nodes(int32_t start);
void    region_route_prepass();

} // namespace mh::sim
