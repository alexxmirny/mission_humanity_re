//
// sim_map_region_helpers_selftest.cpp -- `simtest` offline oracle for llm_map_merge_small_regions
// (sim/sim_map_region_helpers.{h,cpp}, RI-SIM / SIM1F). The sibling function in the same
// TU, llm_map_region_pick_smaller, is OUT OF SCOPE for this batch (not one of the six map-region-graph
// functions assigned) and is left untested here.
//
// llm_map_merge_small_regions restart-scans the ACTIVE region list from the head: for the first region
// under `*region_merge_threshold` with a valid merge candidate among its neighbors, it absorbs that
// candidate (cell_count add, repoint every `_G_LLM_MAP_REGION_GRID` cell the candidate owned, free it,
// recompute adjacency), then restarts from the head; it returns once a full walk finds nothing to
// merge. The two outward callees (region_free / region_recompute_adjacency) are ORIGINAL,
// already-committed functions reached through the `_calls` struct -- recorded via stubs here, not run
// for real (their own effects are proven by sibling oracles: region_recompute_adjacency's own
// selftest, and region_free is untranslated).
//
// SAFE-TERMINATION DISCIPLINE: this function's own stub for region_free is a no-op (it does not unlink
// the freed node from any list or neighbor array it might still be reachable through -- exactly like
// the REAL allocator, which the header banner explains is why a LIVE shadow arm of this function can
// hang). Every fixture below is deliberately built so that after each merge, the restart's fresh scan
// provably finds no further candidate (typically because the absorbing region's cell_count crosses
// `*region_merge_threshold`) -- constructing a fixture that would force a SECOND real restart-merge
// without this property risks an infinite loop in the test process itself, not just a failed check.
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY (tmp/decomp/llm_map_merge_small_regions_004230f7.asm).
//
#include "sim/sim_map_region_helpers.h"

#include "sim_test_support.h"

#include <cstdint>
#include <vector>

namespace mh::sim::test {
namespace {
using namespace mh::sim;

std::vector<uint32_t>          g_freed;
int                            g_recompute_calls = 0;
void                           stub_region_free(uint32_t p) { g_freed.push_back(p); }
void                           stub_recompute() { ++g_recompute_calls; }
const map_region_helpers_calls g_calls = {stub_region_free, stub_recompute};

} // namespace

void run_merge_small_regions_tests() {
    // ---- M1: THRESHOLD GATE + single-neighbor override + merge write (cell_count add, grid repoint,
    // bounds respected) + calls recorded. R1 is always >= threshold (skipped every pass); R2 is under
    // threshold with exactly one neighbor NB, which is a candidate regardless of the 0x1ea cap because
    // `region->neighbor_count == 1` (the "no other choice" override). After the merge R2.cell_count
    // (3+5=8) crosses threshold(5), so the restart's second pass finds nothing and returns cleanly. ---
    {
        sim_fixture fx;
        fx.reset();
        fx.region_merge_threshold = 5;
        fx.map_width              = 2;
        fx.map_height             = 2;

        llm_map_region R1{}, R2{}, NB{};
        R1.cell_count       = 100; // always >= threshold(5) -> always skipped
        R1.next             = &R2;
        R2.cell_count       = 3; // < threshold(5)
        R2.next             = nullptr;
        R2.neighbor_count   = 1;
        R2.neighbors[0]     = &NB;
        R2.neighbor_data[0] = 9; // irrelevant -- only candidate, weight never compared
        NB.cell_count       = 5;
        fx.region_list_head = &R1;

        sim_view  v                     = fx.view();
        sim_store own                   = fx.store();
        own.region_cell_at(0, 0).region = &NB;
        own.region_cell_at(1, 0).region = &R2;
        own.region_cell_at(0, 1).region = &NB;
        own.region_cell_at(1, 1).region = nullptr;
        own.region_cell_at(3, 3).region = &NB; // OUTSIDE the scanned [0,2)x[0,2) bounds

        g_freed.clear();
        g_recompute_calls = 0;
        detail::merge_small_regions(v, own, g_calls);

        ck_eq(R1.cell_count, 100u, "M1: R1 (>=threshold) never touched");
        ck_eq(R2.cell_count, 8u, "M1: R2.cell_count = 3+5 after absorbing NB");
        ck(own.region_cell_at(0, 0).region == &R2, "M1: grid(0,0) repointed NB->R2");
        ck(own.region_cell_at(0, 1).region == &R2, "M1: grid(0,1) repointed NB->R2");
        ck(own.region_cell_at(1, 1).region == nullptr, "M1: grid(1,1) untouched (was null)");
        ck(own.region_cell_at(3, 3).region == &NB,
           "M1: grid(3,3) OUTSIDE map_width/map_height bounds untouched (bounds, not physical grid size)");
        ck_eq((uint32_t)g_freed.size(), 1u, "M1: region_free called exactly once");
        ck(g_freed.size() == 1 && g_freed[0] == (uint32_t)(uintptr_t)&NB, "M1: region_free arg == &NB");
        ck_eq((uint32_t)g_recompute_calls, 2u,
              "M1: recompute_adjacency called twice (unconditional entry + post-merge)");
    }

    // ---- M2: CANDIDATE SELECTION -- the 0x1ea cap excludes nb_fail (huge weight 999, would otherwise
    // dominate); among the three cap-passing candidates, the FIRST (nb_A, weight 4) becomes the running
    // best, a TIE (nb_B, weight 4) does NOT replace it (JBE-on-tie), and a STRICTLY HIGHER weight
    // (nb_C, weight 9) DOES. Threshold(13) is chosen so R.cell_count (5+8=13) crosses it after exactly
    // one merge, guaranteeing safe termination. ------------------------------------------------------
    {
        sim_fixture fx;
        fx.reset();
        fx.region_merge_threshold = 13;
        fx.map_width              = 1;
        fx.map_height             = 1;

        llm_map_region R{}, nb_fail{}, nb_A{}, nb_B{}, nb_C{};
        R.cell_count        = 5;
        R.next              = nullptr;
        R.neighbor_count    = 4;
        R.neighbors[0]      = &nb_fail;
        R.neighbor_data[0]  = 999; // huge weight, but EXCLUDED by the cap below
        R.neighbors[1]      = &nb_A;
        R.neighbor_data[1]  = 4; // first candidate -> establishes the running best
        R.neighbors[2]      = &nb_B;
        R.neighbor_data[2]  = 4; // TIE with nb_A -> must NOT replace it
        R.neighbors[3]      = &nb_C;
        R.neighbor_data[3]  = 9;   // strictly higher -> DOES replace
        nb_fail.cell_count  = 490; // 0x1ea; 490+5=495 >= 0x1ea, neighbor_count!=1 -> no override -> excluded
        nb_A.cell_count     = 10;
        nb_B.cell_count     = 20;
        nb_C.cell_count     = 8;
        fx.region_list_head = &R;

        sim_view  v                     = fx.view();
        sim_store own                   = fx.store();
        own.region_cell_at(0, 0).region = &nb_C;

        g_freed.clear();
        g_recompute_calls = 0;
        detail::merge_small_regions(v, own, g_calls);

        ck_eq(R.cell_count, 13u, "M2: R absorbs nb_C (5+8=13), not nb_fail/nb_A/nb_B");
        ck(own.region_cell_at(0, 0).region == &R, "M2: grid(0,0) repointed nb_C->R");
        ck_eq((uint32_t)g_freed.size(), 1u, "M2: region_free called exactly once");
        ck(g_freed.size() == 1 && g_freed[0] == (uint32_t)(uintptr_t)&nb_C,
           "M2: region_free arg == &nb_C (weight 9 beat the tie at 4; cap excluded nb_fail's 999)");
        ck_eq((uint32_t)g_recompute_calls, 2u, "M2: recompute_adjacency called twice");
    }

    // ---- M3: no valid candidate anywhere (neighbor_count==0) -- region is skipped, walk reaches the
    // end of the list, and the function returns with ZERO merges (only the unconditional entry
    // recompute call). ----------------------------------------------------------------------------
    {
        sim_fixture fx;
        fx.reset();
        fx.region_merge_threshold = 100;

        llm_map_region R{};
        R.cell_count        = 2; // < threshold
        R.next              = nullptr;
        R.neighbor_count    = 0; // no neighbors at all -> no candidate possible
        fx.region_list_head = &R;

        sim_view  v   = fx.view();
        sim_store own = fx.store();

        g_freed.clear();
        g_recompute_calls = 0;
        detail::merge_small_regions(v, own, g_calls);

        ck_eq(R.cell_count, 2u, "M3: no candidate -> R.cell_count unchanged");
        ck(g_freed.empty(), "M3: no region_free call");
        ck_eq((uint32_t)g_recompute_calls, 1u, "M3: only the unconditional entry recompute call");
    }

    // ---- M4: empty list -- immediate clean return. --------------------------------------------------
    {
        sim_fixture fx;
        fx.reset();
        fx.region_merge_threshold = 100;
        fx.region_list_head       = nullptr;

        sim_view  v   = fx.view();
        sim_store own = fx.store();

        g_freed.clear();
        g_recompute_calls = 0;
        detail::merge_small_regions(v, own, g_calls);

        ck(g_freed.empty(), "M4: empty list -> no region_free");
        ck_eq((uint32_t)g_recompute_calls, 1u, "M4: empty list -> only the entry recompute call");
    }

    // Mutation notes:
    //  - M1 catches a translation that forgets the neighbor_count==1 override, or that repoints grid
    //    cells outside [0,*map_width)x[0,*map_height) (the (3,3) sentinel would flip to &R2).
    //  - M2 catches a translation that drops the 0x1ea cap check (nb_fail's 999 would win instead), or
    //    that uses `<=` instead of `<` for the weight replacement (the nb_A/nb_B tie would then flip
    //    to nb_B, changing which pointer region_free is called with).
    //  - M3/M4 catch a translation that merges anyway when no candidate/list exists, or that calls
    //    region_free spuriously.
}

} // namespace mh::sim::test
