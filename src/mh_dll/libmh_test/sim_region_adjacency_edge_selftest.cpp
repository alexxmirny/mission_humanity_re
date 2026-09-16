//
// sim_region_adjacency_edge_selftest.cpp -- `simtest` offline oracle for
// llm_map_region_add_adjacency_edge (sim/sim_region_adjacency_edge.{h,cpp}, RI-SIM / SIM1F).
//
// Pure heap-node-graph function: `void __watcall(llm_map_region *region_a, llm_map_region *region_b)`.
// No sim_view/sim_store, no outward calls (only the inert assert_stack_capacity prologue) -- it only
// dereferences the two caller-supplied nodes directly, so this oracle builds its own small
// `llm_map_region` nodes as locals and calls `detail::region_add_adjacency_edge` straight.
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY (tmp/decomp/llm_map_region_add_adjacency_edge_00422e09.asm):
//   - not-yet-a-neighbor: append region_b to region_a's neighbors[]/neighbor_data[]=1/neighbor_count++,
//     and symmetrically append region_a to region_b's own arrays.
//   - already-a-neighbor (found by linear scan of neighbors[0..neighbor_count)): increment
//     neighbor_data[i] by 1 on BOTH sides (each side scanned/incremented independently).
//   - THE QUIRK (header's own note): the single index variable that scans region_a's list is REUSED,
//     unreset, for the inner scan of region_b's list. If region_b was found in region_a's list but
//     region_a is NOT found in region_b's list (asymmetric/corrupted graph -- "cannot happen for a
//     well-formed symmetric graph"), the outer scan resumes from index == region_b->neighbor_count
//     (not from where it left off), which can walk past a real entry and append a DUPLICATE edge. C4
//     below reproduces this deterministically (chosen so the walk provably terminates, unlike a
//     region_b->neighbor_count==0 variant of the same corruption, which would loop forever).
//
#include "sim/sim_region_adjacency_edge.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {
using namespace mh::sim;
} // namespace

void run_region_add_adjacency_edge_tests() {
    // ---- C1: brand-new edge -- both sides empty, both get appended symmetrically -------------------
    {
        llm_map_region regionA{}, regionB{};
        detail::region_add_adjacency_edge(&regionA, &regionB);
        ck_eq(regionA.neighbor_count, 1u, "C1: regionA.neighbor_count = 1");
        ck(regionA.neighbors[0] == &regionB, "C1: regionA.neighbors[0] = &regionB");
        ck_eq(regionA.neighbor_data[0], 1u, "C1: regionA.neighbor_data[0] = 1");
        ck_eq(regionB.neighbor_count, 1u, "C1: regionB.neighbor_count = 1");
        ck(regionB.neighbors[0] == &regionA, "C1: regionB.neighbors[0] = &regionA");
        ck_eq(regionB.neighbor_data[0], 1u, "C1: regionB.neighbor_data[0] = 1");
    }

    // ---- C2: duplicate edge already present at index 0 on both sides -- weight increments, count
    // stays put. DISTINCT starting weights (9 vs 15) so a translation that increments the WRONG
    // side's neighbor_data[] is caught. -------------------------------------------------------------
    {
        llm_map_region regionA{}, regionB{};
        regionA.neighbor_count   = 1;
        regionA.neighbors[0]     = &regionB;
        regionA.neighbor_data[0] = 9;
        regionB.neighbor_count   = 1;
        regionB.neighbors[0]     = &regionA;
        regionB.neighbor_data[0] = 15;

        detail::region_add_adjacency_edge(&regionA, &regionB);

        ck_eq(regionA.neighbor_count, 1u, "C2: regionA.neighbor_count unchanged (1)");
        ck_eq(regionA.neighbor_data[0], 10u, "C2: regionA.neighbor_data[0] = 9+1");
        ck_eq(regionB.neighbor_count, 1u, "C2: regionB.neighbor_count unchanged (1)");
        ck_eq(regionB.neighbor_data[0], 16u, "C2: regionB.neighbor_data[0] = 15+1");
    }

    // ---- C3: duplicate edge at a NON-ZERO index on both sides -- proves the linear scan (not a
    // hardcoded index-0 check), and that untouched slots are left alone. ----------------------------
    {
        llm_map_region regionA{}, regionB{}, dummyX{}, dummyY{}, dummyZ{}, dummyW{};
        regionA.neighbor_count   = 3;
        regionA.neighbors[0]     = &dummyX;
        regionA.neighbor_data[0] = 100;
        regionA.neighbors[1]     = &regionB; // regionB at index 1
        regionA.neighbor_data[1] = 9;
        regionA.neighbors[2]     = &dummyY;
        regionA.neighbor_data[2] = 200;

        regionB.neighbor_count   = 3;
        regionB.neighbors[0]     = &dummyZ;
        regionB.neighbor_data[0] = 300;
        regionB.neighbors[1]     = &dummyW;
        regionB.neighbor_data[1] = 400;
        regionB.neighbors[2]     = &regionA; // regionA at index 2 (requires a full scan)
        regionB.neighbor_data[2] = 15;

        detail::region_add_adjacency_edge(&regionA, &regionB);

        ck_eq(regionA.neighbor_count, 3u, "C3: regionA.neighbor_count unchanged (3)");
        ck_eq(regionA.neighbor_data[0], 100u, "C3: regionA.neighbor_data[0] (dummyX) untouched");
        ck_eq(regionA.neighbor_data[1], 10u, "C3: regionA.neighbor_data[1] (regionB slot) = 9+1");
        ck_eq(regionA.neighbor_data[2], 200u, "C3: regionA.neighbor_data[2] (dummyY) untouched");
        ck_eq(regionB.neighbor_count, 3u, "C3: regionB.neighbor_count unchanged (3)");
        ck_eq(regionB.neighbor_data[0], 300u, "C3: regionB.neighbor_data[0] (dummyZ) untouched");
        ck_eq(regionB.neighbor_data[1], 400u, "C3: regionB.neighbor_data[1] (dummyW) untouched");
        ck_eq(regionB.neighbor_data[2], 16u, "C3: regionB.neighbor_data[2] (regionA slot) = 15+1");
    }

    // ---- C4: THE QUIRK -- region_b found in region_a's list (index 1), but region_a is NOT found in
    // region_b's (single-entry, unrelated) list. The reused index resumes the outer scan at
    // region_b->neighbor_count(1)+1==2, which equals region_a->neighbor_count(2) BEFORE this call, so
    // the outer loop's "not found" (append) branch fires -- inserting a SECOND, duplicate edge on top
    // of the one just incremented. Chosen so region_b->neighbor_count > 0 (a region_b->neighbor_count
    // == 0 variant of this same corruption would loop forever re-matching regionA.neighbors[1] --
    // NOT constructed here, since that would hang the test process, not just fail a check). ----------
    {
        llm_map_region regionA{}, regionB{}, dummyQ{}, dummyR{};
        regionA.neighbor_count   = 2;
        regionA.neighbors[0]     = &dummyQ;
        regionA.neighbor_data[0] = 50;
        regionA.neighbors[1]     = &regionB; // found here
        regionA.neighbor_data[1] = 21;

        regionB.neighbor_count   = 1;
        regionB.neighbors[0]     = &dummyR; // does NOT contain regionA
        regionB.neighbor_data[0] = 77;

        detail::region_add_adjacency_edge(&regionA, &regionB);

        // regionA: the found-at-index-1 edge got incremented once, THEN a duplicate got appended.
        ck_eq(regionA.neighbor_count, 3u, "C4: regionA.neighbor_count grew to 3 (duplicate appended)");
        ck(regionA.neighbors[0] == &dummyQ, "C4: regionA.neighbors[0] untouched");
        ck_eq(regionA.neighbor_data[0], 50u, "C4: regionA.neighbor_data[0] untouched");
        ck(regionA.neighbors[1] == &regionB, "C4: regionA.neighbors[1] still regionB");
        ck_eq(regionA.neighbor_data[1], 22u, "C4: regionA.neighbor_data[1] = 21+1 (found-and-incremented)");
        ck(regionA.neighbors[2] == &regionB, "C4: regionA.neighbors[2] = regionB AGAIN (the quirk's duplicate)");
        ck_eq(regionA.neighbor_data[2], 1u, "C4: regionA.neighbor_data[2] = 1 (fresh append)");

        // regionB: the inner scan never found regionA, so it takes the append branch too.
        ck_eq(regionB.neighbor_count, 2u, "C4: regionB.neighbor_count grew to 2");
        ck(regionB.neighbors[0] == &dummyR, "C4: regionB.neighbors[0] untouched");
        ck_eq(regionB.neighbor_data[0], 77u, "C4: regionB.neighbor_data[0] untouched");
        ck(regionB.neighbors[1] == &regionA, "C4: regionB.neighbors[1] = regionA (fresh append)");
        ck_eq(regionB.neighbor_data[1], 1u, "C4: regionB.neighbor_data[1] = 1 (fresh append)");
    }

    // Mutation notes:
    //  - C1 catches a translation that appends only to region_a (or swaps which side gets `1`).
    //  - C2 catches the "increment region_b's neighbor_data instead of region_a's" swap (distinct
    //    starting weights 9 vs 15 make a cross-wire visible).
    //  - C3 catches a translation that only checks neighbors[0] (hardcoded index) instead of scanning.
    //  - C4 is the regression lock for the documented index-reuse quirk: a "cleaned up" translation
    //    that resets the outer index properly instead of reusing it would leave regionA.neighbor_count
    //    at 2 (no duplicate), diverging from this oracle's expected 3.
}

} // namespace mh::sim::test
