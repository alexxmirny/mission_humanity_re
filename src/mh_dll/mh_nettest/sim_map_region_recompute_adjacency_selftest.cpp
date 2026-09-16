//
// sim_map_region_recompute_adjacency_selftest.cpp -- `simtest` offline oracle for
// llm_map_region_recompute_adjacency (sim/sim_map_region_recompute_adjacency.{h,cpp}, RI-SIM / SIM1F).
//
// Two write surfaces, both exercised here: (1) the reset pass -- zero every ACTIVE-list node's own
// `neighbor_count` via a direct heap-pointer walk of `.next`; (2) the `[0,*map_width) x
// [0,*map_height)` grid scan -- for each non-null cell, call `add_adjacency_edge(cell, right)` /
// `add_adjacency_edge(cell, down)` whenever that wrap-masked neighbor is a DIFFERENT, non-null region.
// The one outward callee (llm_map_region_add_adjacency_edge) is recorded via a stub, not run for real
// -- its own effects are proven separately by sim_region_adjacency_edge_selftest.cpp.
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_map_region_recompute_adjacency_00422f0c.asm). Grid index = x*MAP_GRID_DIM + y (x
// OUTER), matching sim_store::region_cell_at(); the wrap masks (`*v.width_m`/`*v.height_m`) are a
// SEPARATE global pair from the scan bounds (`*v.map_width`/`*v.map_height`) -- G2/G3 below are the
// regression lock for a translation that confused the two.
//
#include "sim/sim_map_region_recompute_adjacency.h"

#include "sim_test_support.h"

#include <utility>
#include <vector>

namespace mh::sim::test {
namespace {
using namespace mh::sim;

std::vector<std::pair<llm_map_region *, llm_map_region *>> g_edges;
void                                                       stub_add_edge(llm_map_region *a, llm_map_region *b) { g_edges.push_back({a, b}); } // typed by committed prototype (was void*; synced 2026-08-19)
const map_region_recompute_adjacency_calls                 g_calls = {stub_add_edge};

} // namespace

void run_region_recompute_adjacency_tests() {
    // ---- R1: the reset pass in isolation -- map_width=0 so the grid scan never runs; three chained
    // nodes with DISTINCT nonzero neighbor_count all get zeroed, and add_adjacency_edge is never
    // called. -----------------------------------------------------------------------------------------
    {
        sim_fixture fx;
        fx.reset();
        llm_map_region n1{}, n2{}, n3{};
        n1.neighbor_count   = 5;
        n2.neighbor_count   = 7;
        n3.neighbor_count   = 9;
        n1.next             = &n2;
        n2.next             = &n3;
        n3.next             = nullptr;
        fx.region_list_head = &n1;
        fx.map_width        = 0;
        fx.map_height       = 0;

        sim_view  v   = fx.view();
        sim_store own = fx.store();
        g_edges.clear();
        detail::region_recompute_adjacency(v, own, g_calls);

        ck_eq(n1.neighbor_count, 0u, "R1: n1.neighbor_count reset to 0");
        ck_eq(n2.neighbor_count, 0u, "R1: n2.neighbor_count reset to 0");
        ck_eq(n3.neighbor_count, 0u, "R1: n3.neighbor_count reset to 0");
        ck(g_edges.empty(), "R1: empty grid scan -> zero add_adjacency_edge calls");
    }

    // ---- R2: empty list + empty grid -- no crash, no calls. ----------------------------------------
    {
        sim_fixture fx;
        fx.reset();
        fx.region_list_head = nullptr;
        fx.map_width        = 0;
        fx.map_height       = 0;

        sim_view  v   = fx.view();
        sim_store own = fx.store();
        g_edges.clear();
        detail::region_recompute_adjacency(v, own, g_calls);

        ck(g_edges.empty(), "R2: empty list + empty grid -> no calls");
    }

    // ---- G1: null-cell skip (continue), self-skip via a wrap that lands back on the same cell
    // (height_m=0 forces every down-neighbor to be the cell itself), and one basic right-neighbor
    // edge -- all in a single deterministic 3x1 row. -------------------------------------------------
    {
        sim_fixture fx;
        fx.reset();
        fx.map_width  = 3;
        fx.map_height = 1;
        fx.width_m    = 3; // (x+1)&3: 0->1, 1->2, 2->3(out of the populated 3 cells -> null)
        fx.height_m   = 0; // (y+1)&0 == 0 always -> down-neighbor is always the cell itself

        llm_map_region regA{}, regB{};
        sim_view       v                = fx.view();
        sim_store      own              = fx.store();
        own.region_cell_at(0, 0).region = &regA;
        own.region_cell_at(1, 0).region = &regB;
        // (2,0) left null (default) -- the cell==null skip.

        g_edges.clear();
        detail::region_recompute_adjacency(v, own, g_calls);

        ck_eq((uint32_t)g_edges.size(), 1u, "G1: exactly one edge recorded");
        ck(g_edges.size() == 1 && g_edges[0].first == &regA && g_edges[0].second == &regB,
           "G1: call(regA,regB) from x=0's right-neighbor");
        // x=0's down-neighbor (self, via height_m=0) is correctly NOT an edge; x=1's right-neighbor is
        // null (x=2 unpopulated) and its down-neighbor is self -- neither fires; x=2 is null -> skipped
        // entirely. Only the single right-edge above fires.
    }

    // ---- G2: proves the RIGHT-neighbor wrap uses *v.width_m, NOT *v.map_width-1. width_m=3 wraps
    // (1+1)&3=2 (a real, distinct third cell); a translation that used map_width-1(=1) instead would
    // wrap back to x=0 and record (regB,regA) instead of (regB,regD). ---------------------------------
    {
        sim_fixture fx;
        fx.reset();
        fx.map_width  = 2; // only x=0,1 are SCANNED...
        fx.map_height = 1;
        fx.width_m    = 3; // ...but the wrap mask reaches x=2 as a neighbor, distinct from map_width-1=1
        fx.height_m   = 0; // keep down-neighbor self-skip, isolating the right-only check

        llm_map_region regA{}, regB{}, regD{};
        sim_view       v                = fx.view();
        sim_store      own              = fx.store();
        own.region_cell_at(0, 0).region = &regA;
        own.region_cell_at(1, 0).region = &regB;
        own.region_cell_at(2, 0).region = &regD; // reachable only via width_m=3, never scanned itself

        g_edges.clear();
        detail::region_recompute_adjacency(v, own, g_calls);

        ck_eq((uint32_t)g_edges.size(), 2u, "G2: two right-edges recorded");
        ck(g_edges.size() >= 1 && g_edges[0].first == &regA && g_edges[0].second == &regB,
           "G2 call0: (regA,regB) from x=0, right_x=(0+1)&3=1");
        ck(g_edges.size() >= 2 && g_edges[1].first == &regB && g_edges[1].second == &regD,
           "G2 call1: (regB,regD) from x=1, right_x=(1+1)&3=2 -- proves width_m, not map_width-1, drives the wrap");
    }

    // ---- G3: the symmetric proof for the DOWN-neighbor wrap using *v.height_m, NOT *v.map_height-1. --
    {
        sim_fixture fx;
        fx.reset();
        fx.map_width  = 1;
        fx.map_height = 2; // only y=0,1 are SCANNED...
        fx.width_m    = 0; // keep right-neighbor self-skip, isolating the down-only check
        fx.height_m   = 3; // ...but the wrap mask reaches y=2 as a neighbor, distinct from map_height-1=1

        llm_map_region regA{}, regB{}, regD{};
        sim_view       v                = fx.view();
        sim_store      own              = fx.store();
        own.region_cell_at(0, 0).region = &regA;
        own.region_cell_at(0, 1).region = &regB;
        own.region_cell_at(0, 2).region = &regD; // reachable only via height_m=3, never scanned itself

        g_edges.clear();
        detail::region_recompute_adjacency(v, own, g_calls);

        ck_eq((uint32_t)g_edges.size(), 2u, "G3: two down-edges recorded");
        ck(g_edges.size() >= 1 && g_edges[0].first == &regA && g_edges[0].second == &regB,
           "G3 call0: (regA,regB) from y=0, down_y=(0+1)&3=1");
        ck(g_edges.size() >= 2 && g_edges[1].first == &regB && g_edges[1].second == &regD,
           "G3 call1: (regB,regD) from y=1, down_y=(1+1)&3=2 -- proves height_m, not map_height-1, drives the wrap");
    }

    // Mutation notes:
    //  - R1 catches a translation that skips the reset pass, or that walks the wrong pointer field.
    //  - G1 catches a translation that fires an edge for a null neighbor, or that fails to skip a null
    //    cell's own neighbor checks (both would add spurious calls beyond the expected single entry).
    //  - G2/G3 are the regression lock for width_m/height_m vs. map_width/map_height confusion -- the
    //    single most tempting mixup here, since both pairs are read in the exact same instruction shape.
}

} // namespace mh::sim::test
