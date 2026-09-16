//
// sim_map_region_split_selftest.cpp -- `simtest` offline oracle for llm_map_region_split
// (sim/sim_map_region_split.{h,cpp}, RI-SIM / SIM1F).
//
// Allocates one fresh `llm_map_region` via the one outward callee (get_next_block, stubbed here to
// return a locally-owned node -- not run for real, since the real allocator is untranslated), seeds it
// unconditionally at (seed_col, seed_row) WITHOUT wrap-masking the seed cell (unlike every BFS-derived
// neighbor), then 4-neighbor (+x,-x,+y,-y, in that order) wrapped-BFS-floods through the private
// `region_bfs_queue()` scratch, absorbing every cell still owned by `old_region` into the new node.
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY (tmp/decomp/llm_map_region_split_00423fb8.asm).
//
#include "sim/sim_map_region_split.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {
using namespace mh::sim;

// Recording stub: returns whatever the test currently points `g_next_block` at. Must be a plain
// (non-capturing) function -- `map_region_split_calls::get_next_block` is `mh_llm_map_region *(*)()`
// (typed by the committed prototype; was void* before the 2026-08-19 calls-header sync).
llm_map_region              *g_next_block = nullptr;
llm_map_region              *stub_get_next_block() { return g_next_block; }
const map_region_split_calls g_calls = {stub_get_next_block};

} // namespace

void run_region_split_tests() {
    // ---- A: seed-only, no absorption -- proves (1) new_region->x/y/cell_count are seeded correctly,
    // (2) the seed cell write is NOT wrap-masked (unlike every neighbor lookup): with width_m=height_m
    // =3, seed (10,12) writes straight to grid(10,12), NOT to the wrap-masked grid(10&3,12&3)=grid(2,0)
    // -- a sentinel planted there must survive untouched. `old_region` never matches any grid cell here
    // (nothing was seeded with its address), so the flood finds nothing and cell_count stays 1. --------
    {
        sim_fixture fx;
        fx.reset();
        fx.width_m  = 3;
        fx.height_m = 3;

        llm_map_region newNodeA{};
        llm_map_region oldRegion{}; // identity-only: never dereferenced by the original, only compared
        llm_map_region sentinel{};  // marks the wrap-masked-equivalent cell, to prove it's untouched
        g_next_block = &newNodeA;

        sim_view  v                     = fx.view();
        sim_store own                   = fx.store();
        own.region_cell_at(2, 0).region = &sentinel; // (10&3, 12&3) -- must NOT be touched

        llm_map_region *ret = detail::region_split(v, own, g_calls, &oldRegion, /*seed_col*/ 10,
                                                   /*seed_row*/ 12);

        ck(ret == &newNodeA, "A: returns the pointer get_next_block() handed back");
        ck_eq((uint32_t)newNodeA.x, 10u, "A: new_region->x = seed_col");
        ck_eq((uint32_t)newNodeA.y, 12u, "A: new_region->y = seed_row");
        ck_eq(newNodeA.cell_count, 1u, "A: cell_count = 1 (seed only, no absorption)");
        ck(own.region_cell_at(10, 12).region == &newNodeA, "A: seed cell (unmasked coords) claimed");
        ck(own.region_cell_at(2, 0).region == &sentinel,
           "A: wrap-masked-equivalent cell (10&3,12&3)=(2,0) untouched -- seed write is NOT wrap-masked");
    }

    // ---- B: full flood-fill -- unconditional seed overwrite, all four directions, second-hop
    // propagation, a DIAMOND merge point reachable via two independent paths (must be absorbed exactly
    // ONCE, not twice), and both kinds of boundary stop (a different non-null owner, and a null owner).
    //
    //           (10,9)=O
    //              |
    // (9,10)=O -- (10,10)=priorOwner(seed) -- (11,10)=O -- (12,10)=O -- (13,10)=boundary
    //              |                            |
    //           (10,11)=O ------------------ (11,11)=O   <- diamond: reachable via (11,10)->down
    //              |                                         AND (10,11)->right
    //           (10,12)=null (boundary)
    //
    // width_m/height_m are generous (0xff) so nothing here wraps. ------------------------------------
    {
        sim_fixture fx;
        fx.reset();
        fx.width_m  = 0xff;
        fx.height_m = 0xff;

        llm_map_region newNodeB{};
        llm_map_region oldRegion{};  // "O" -- the region being split
        llm_map_region priorOwner{}; // seed cell's owner BEFORE the call -- overwritten unconditionally
        llm_map_region boundary{};   // a DIFFERENT, non-null owner: must NOT be absorbed
        g_next_block = &newNodeB;

        sim_view  v                       = fx.view();
        sim_store own                     = fx.store();
        own.region_cell_at(10, 10).region = &priorOwner; // seed's prior owner (not O)
        own.region_cell_at(11, 10).region = &oldRegion;  // right of seed
        own.region_cell_at(9, 10).region  = &oldRegion;  // left of seed
        own.region_cell_at(10, 11).region = &oldRegion;  // down of seed
        own.region_cell_at(10, 9).region  = &oldRegion;  // up of seed
        own.region_cell_at(12, 10).region = &oldRegion;  // second hop: right of (11,10)
        own.region_cell_at(11, 11).region = &oldRegion;  // DIAMOND: down of (11,10) AND right of (10,11)
        own.region_cell_at(13, 10).region = &boundary;   // stop: non-null, non-O owner
        // (8,10), (10,12), (10,8) left null (default) -- the null-owner stop.

        llm_map_region *ret = detail::region_split(v, own, g_calls, &oldRegion, 10, 10);

        ck(ret == &newNodeB, "B: returns get_next_block()'s pointer");
        ck_eq((uint32_t)newNodeB.x, 10u, "B: new_region->x = seed_col");
        ck_eq((uint32_t)newNodeB.y, 10u, "B: new_region->y = seed_row");
        ck_eq(newNodeB.cell_count, 7u, "B: cell_count = 1 seed + 6 absorbed (diamond counted ONCE)");
        ck(own.region_cell_at(10, 10).region == &newNodeB,
           "B: seed cell claimed, overwriting priorOwner unconditionally");
        ck(own.region_cell_at(11, 10).region == &newNodeB, "B: right absorbed");
        ck(own.region_cell_at(9, 10).region == &newNodeB, "B: left absorbed");
        ck(own.region_cell_at(10, 11).region == &newNodeB, "B: down absorbed");
        ck(own.region_cell_at(10, 9).region == &newNodeB, "B: up absorbed");
        ck(own.region_cell_at(12, 10).region == &newNodeB,
           "B: second-hop right absorbed (BFS continues past the first ring)");
        ck(own.region_cell_at(11, 11).region == &newNodeB, "B: diamond-merge cell absorbed exactly once");
        ck(own.region_cell_at(13, 10).region == &boundary,
           "B: boundary cell (different non-null owner) NOT absorbed, flood stops there");
        ck(own.region_cell_at(8, 10).region == nullptr, "B: null-owner cell NOT absorbed (left boundary)");
        ck(own.region_cell_at(10, 12).region == nullptr, "B: null-owner cell NOT absorbed (down boundary)");
        ck(own.region_cell_at(10, 8).region == nullptr, "B: null-owner cell NOT absorbed (up boundary)");
    }

    // Mutation notes:
    //  - A's sentinel check catches a translation that (wrongly) wrap-masks the seed cell like every
    //    other neighbor lookup -- it would write grid(2,0) instead of grid(10,12), flipping both
    //    assertions.
    //  - B's cell_count==7 (not 8) is the single strongest check in this file: if the absorption guard
    //    compared against `new_region` instead of `old_region` (or omitted the check), the diamond cell
    //    (11,11) would be visited and re-absorbed a second time via its other path, inflating
    //    cell_count to 8 and leaving the queue in a different state.
    //  - B's boundary checks catch a translation that absorbs indiscriminately (dropping either the
    //    null check or the old_region-equality check) -- (13,10)/(8,10)/(10,12)/(10,8) would all flip
    //    to &newNodeB.
    //
    // NOT reachable offline: the queue-index wrap at `& 0x7ff` (2048 entries) would require flooding
    // over 2048 live cells before the read index catches the write index -- far beyond what a
    // hand-traceable fixture can build; this branch is only reachable via a live/shadow run, if ever.
}

} // namespace mh::sim::test
