//
// sim_map_apply_area_selftest.cpp -- `simtest` offline oracle for the two footprint-vs-region-grid
// stampers in sim/sim_map_apply_area.{h,cpp} (RI-SIM / SIM1F):
//
//   map_ApplyAreaToMap      @0x00424406 -> detail::apply_area_to_map   (run_apply_area_to_map_tests)
//   llm_map_region_apply_area @0x004245c3 -> detail::region_apply_area (run_region_apply_area_tests)
//
// Both reach translated siblings (region_split / region_pick_smaller / merge_small_regions) and two
// originals (region_free / get_next_block) through this TU's `map_apply_area_calls` struct -- ALL FIVE
// are stubbed here with recording stubs, so these cases exercise ONLY each function's OWN logic (grid
// index math, the .prev-as-counter reuse, the two-pass alloc-gate state machine) and assert the siblings
// fired with the right args. The siblings have their own oracles (sim_map_region_split_selftest /
// sim_map_region_helpers_selftest).
//
// The pick_smaller stub is a faithful-enough recorder: it records (block,x,y) AND reads the live grid
// cell's owner (via a captured sim_store*), returning `block` when the cell has no region -- this is the
// real llm_map_region_pick_smaller semantics (minus the 0xffffffff sentinel, which no fixture plants),
// and it is what lets region_apply_area's multi-pass neighbor-inheritance terminate offline. The sentinel
// path and pick_smaller's own smaller-cell_count selection are pick_smaller's concern, tested in its own
// oracle, not here.
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY (tmp/decomp/map_ApplyAreaToMap_00424406.asm,
// tmp/decomp/llm_map_region_apply_area_004245c3.asm). Grid index math confirmed from both .asm files and
// sim_state.h: region_cell_at(x,y) = region_grid_[x*256 + y], element stride 8 bytes -- the .asm computes
// the byte offset as (x<<11)+(y<<3) = (x*256 + y)*8 into _G_LLM_MAP_REGION_GRID (0x688ac4).
//
#include "sim/sim_map_apply_area.h"

#include "sim_test_support.h"

#include <cstdint>
#include <vector>

namespace mh::sim::test {
namespace {
using namespace mh::sim;

// ---- recording stubs for all five map_apply_area_calls members -------------------------------------
struct pick_call {
    void   *block;
    int32_t x;
    int32_t y;
};
struct split_call {
    llm_map_region *old_region;
    int32_t         seed_col;
    int32_t         seed_row;
};

std::vector<pick_call>  g_pick;
std::vector<split_call> g_split;
std::vector<uint32_t>   g_freed;
llm_map_region         *g_split_ret  = nullptr;
llm_map_region         *g_next       = nullptr;
int                     g_next_count = 0;
int                     g_merge      = 0;
sim_store              *g_store      = nullptr; // so the pick stub can read the live grid cell owner

void            stub_region_free(uint32_t p) { g_freed.push_back(p); }
llm_map_region *stub_get_next_block() { // typed by the committed prototype (was void*; synced 2026-08-19)
    ++g_next_count;
    return g_next;
}
llm_map_region *stub_region_split(llm_map_region *old_region, int32_t seed_col, int32_t seed_row) {
    g_split.push_back({old_region, seed_col, seed_row});
    return g_split_ret;
}
llm_map_region *stub_region_pick_smaller(llm_map_region *block, int32_t x, int32_t y) {
    g_pick.push_back({block, x, y});
    llm_map_region *cell_region = g_store->region_cell_at(x, y).region; // faithful: read the cell's owner
    if (cell_region == nullptr)
        return block; // no region here -> keep the incoming candidate
    if (block == nullptr)
        return cell_region; // cell's region wins by default
    // both real candidates: strictly-smaller cell_count wins, else keep block (never hit by these fixtures)
    return (cell_region->cell_count < block->cell_count) ? cell_region : block;
}
void stub_merge() { ++g_merge; }

const map_apply_area_calls g_calls = {stub_region_free, stub_get_next_block, stub_region_split,
                                      stub_region_pick_smaller, stub_merge};

void reset_records() {
    g_pick.clear();
    g_split.clear();
    g_freed.clear();
    g_split_ret  = nullptr;
    g_next       = nullptr;
    g_next_count = 0;
    g_merge      = 0;
    g_store      = nullptr;
}

} // namespace

// ====================================================================================================
// map_ApplyAreaToMap -- CLEAR strategy: zero every region's .prev counter, clear masked cells (bumping
// the former owner's .prev counter + shrinking cell_count), then rescan the whole map and split off the
// surviving piece of every touched region, freeing any remainder that shrank to zero.
// ====================================================================================================
void run_apply_area_to_map_tests() {
    // ---- A: full walk -- phase-1 zeroes .prev on EVERY listed region (R and its .next sibling S),
    // phase-2 covers all three inner branches (masked cell WITH owner -> counter++/cell_count--/null;
    // masked cell with NO owner -> just re-null, no counter; unmasked cell -> skipped), phase-3 splits a
    // region whose counter is nonzero (surviving cell (2,3)) and SKIPS a region whose counter stayed 0
    // (S at (3,4), never touched). Split remainder stays nonzero -> region_free NOT called. -------------
    {
        sim_fixture fx;
        fx.reset();
        fx.width_m    = 0xff; // no wrap in phase 2
        fx.height_m   = 0xff;
        fx.map_width  = 8; // phase-3 rescan bound
        fx.map_height = 8;

        llm_map_region R{}, S{}, splitA{};
        R.cell_count        = 10;
        R.next              = &S;
        R.prev              = (llm_map_region *)0x999; // junk -- phase 1 must zero it
        S.cell_count        = 50;
        S.next              = nullptr;
        S.prev              = (llm_map_region *)0x888; // junk on the SECOND list node -- proves .next walk
        splitA.cell_count   = 3;                       // the piece region_split "peels off"
        fx.region_list_head = &R;

        sim_view  v   = fx.view();
        sim_store own = fx.store();
        // x_0=5,y_0=7: masked footprint cells (5,7),(6,7),(7,7).
        own.region_cell_at(5, 7).region = &R;      // masked, owned by R -> cleared, counter++
        own.region_cell_at(6, 7).region = &R;      // masked, owned by R -> cleared, counter++
        own.region_cell_at(7, 7).region = nullptr; // masked, NO owner -> re-nulled, no counter
        own.region_cell_at(2, 3).region = &R;      // UNMASKED survivor of R -> phase-3 split target
        own.region_cell_at(3, 4).region = &S;      // UNMASKED, S untouched (counter 0) -> phase-3 skip

        uint8_t mask[100] = {};
        mask[0 * 10 + 0]  = 1; // (dx=0,dy=0) -> (5,7)
        mask[1 * 10 + 0]  = 1; // (dx=1,dy=0) -> (6,7)
        mask[2 * 10 + 0]  = 1; // (dx=2,dy=0) -> (7,7)

        reset_records();
        g_store     = &own;
        g_split_ret = &splitA;
        detail::apply_area_to_map(v, own, g_calls, /*x_0*/ 5, /*y_0*/ 7, mask);

        ck_eq((uint32_t)(uintptr_t)R.prev, 2u, "A: R.prev counter = 2 (two masked R-cells cleared)");
        ck_eq(R.cell_count, 5u, "A: R.cell_count = 10 -2(clear) -3(split_off) = 5");
        ck((uintptr_t)S.prev == 0, "A: S.prev zeroed by phase 1 (proves .next walk reaches second node)");
        ck_eq(S.cell_count, 50u, "A: S untouched (counter stayed 0 -> phase-3 skipped it)");
        ck(own.region_cell_at(5, 7).region == nullptr, "A: masked owned cell (5,7) cleared");
        ck(own.region_cell_at(6, 7).region == nullptr, "A: masked owned cell (6,7) cleared");
        ck(own.region_cell_at(7, 7).region == nullptr, "A: masked no-owner cell (7,7) stays null");
        ck_eq((uint32_t)g_split.size(), 1u, "A: region_split called exactly once (only the touched R)");
        ck(g_split.size() == 1 && g_split[0].old_region == &R && g_split[0].seed_col == 2 &&
               g_split[0].seed_row == 3,
           "A: region_split(old=&R, seed_col=2, seed_row=3) -- first surviving R cell in x-outer scan");
        ck(g_freed.empty(), "A: region_free NOT called (remainder cell_count 5 != 0)");
        ck_eq((uint32_t)g_merge, 1, "A: merge_small_regions called once at the tail");
    }

    // ---- B: split remainder hits ZERO -> region_free fires with the region pointer. Same shape as A but
    // R.cell_count is tuned so 5 -2(clear) -3(split_off) == 0. -----------------------------------------
    {
        sim_fixture fx;
        fx.reset();
        fx.width_m    = 0xff;
        fx.height_m   = 0xff;
        fx.map_width  = 8;
        fx.map_height = 8;

        llm_map_region R{}, splitB{};
        R.cell_count        = 5;
        R.next              = nullptr;
        R.prev              = (llm_map_region *)0x777;
        splitB.cell_count   = 3;
        fx.region_list_head = &R;

        sim_view  v                     = fx.view();
        sim_store own                   = fx.store();
        own.region_cell_at(5, 7).region = &R; // masked owned
        own.region_cell_at(6, 7).region = &R; // masked owned
        own.region_cell_at(2, 3).region = &R; // surviving -> split target

        uint8_t mask[100] = {};
        mask[0 * 10 + 0]  = 1; // (5,7)
        mask[1 * 10 + 0]  = 1; // (6,7)

        reset_records();
        g_store     = &own;
        g_split_ret = &splitB;
        detail::apply_area_to_map(v, own, g_calls, 5, 7, mask);

        ck_eq(R.cell_count, 0u, "B: R.cell_count = 5 -2 -3 = 0");
        ck_eq((uint32_t)g_split.size(), 1u, "B: region_split called once");
        ck(g_split.size() == 1 && g_split[0].old_region == &R && g_split[0].seed_col == 2 &&
               g_split[0].seed_row == 3,
           "B: region_split(&R, 2, 3)");
        ck_eq((uint32_t)g_freed.size(), 1u, "B: region_free called exactly once (remainder hit 0)");
        ck(g_freed.size() == 1 && g_freed[0] == (uint32_t)(uintptr_t)&R,
           "B: region_free arg == (uint32_t)&R");
        ck_eq((uint32_t)g_merge, 1, "B: merge_small_regions called once");
    }

    // ---- C: phase-2 wrap masking -- with width_m=height_m=7 and origin (6,6), mask cell (dx=3,dy=3)
    // resolves to ((6+3)&7,(6+3)&7)=(1,1), NOT (9,9). Proves BOTH `& width_m` and `& height_m` are
    // applied: the region planted at (1,1) is cleared (counter++), while (9,9) -- where a dropped mask
    // would have written -- stays untouched. R is fully cleared -> phase 3 finds no surviving cell. ----
    {
        sim_fixture fx;
        fx.reset();
        fx.width_m    = 7;
        fx.height_m   = 7;
        fx.map_width  = 10;
        fx.map_height = 10;

        llm_map_region R{};
        R.cell_count        = 4;
        R.next              = nullptr;
        R.prev              = (llm_map_region *)0x555;
        fx.region_list_head = &R;

        sim_view  v                     = fx.view();
        sim_store own                   = fx.store();
        own.region_cell_at(1, 1).region = &R; // wrap-masked target
        // (9,9) deliberately left null -- a dropped wrap mask would clear it instead.

        uint8_t mask[100] = {};
        mask[3 * 10 + 3]  = 1; // (dx=3,dy=3)

        reset_records();
        g_store = &own;
        detail::apply_area_to_map(v, own, g_calls, /*x_0*/ 6, /*y_0*/ 6, mask);

        ck(own.region_cell_at(1, 1).region == nullptr,
           "C: wrap-masked cell (1,1) cleared -- proves & width_m/& height_m applied in phase 2");
        ck(own.region_cell_at(9, 9).region == nullptr,
           "C: unmasked (9,9) untouched -- a dropped wrap mask would have cleared here instead");
        ck_eq((uint32_t)(uintptr_t)R.prev, 1u, "C: R.prev counter = 1 (one cell cleared, at the wrapped coord)");
        ck_eq(R.cell_count, 3u, "C: R.cell_count = 4 - 1 = 3 (no phase-3 split -- R has no surviving cell)");
        ck(g_split.empty(), "C: region_split NOT called (no surviving R cell in the rescan)");
        ck(g_freed.empty(), "C: region_free NOT called");
        ck_eq((uint32_t)g_merge, 1, "C: merge_small_regions called once");
    }

    // Mutation notes:
    //  - A's g_split[0] args and the S.cell_count==50 check catch a phase-3 that splits every region cell
    //    instead of gating on region->prev!=0 (S would be split too), or that seeds the split with the
    //    wrong (x,y). R.prev==2 catches losing the counter increment.
    //  - B's g_freed check catches dropping the `cell_count==0 -> region_free` branch, or freeing the
    //    wrong pointer.
    //  - C catches dropping `& width_m`/`& height_m` in phase 2 (both cell asserts flip, and R.prev goes
    //    to 0 because the buggy coord (9,9) has no owner to count).
}

// ====================================================================================================
// llm_map_region_apply_area -- INHERIT strategy: for every masked, region-less cell, chain four
// pick_smaller neighbor queries; allocate a fresh region only when stuck AND the two-pass alloc gate is
// armed (armed only while NO cell anywhere in this call has yet been assigned). Tail: merge_small_regions.
// ====================================================================================================
void run_region_apply_area_tests() {
    // ---- R1: single masked cell inherits from its +x neighbor. Proves the masked+region-less gate, the
    // FOUR neighbor queries in order (+x,-x,+y,-y) with block chained through, the assignment write, and
    // single-pass exit when nothing is left unassigned. No wrap (width_m/height_m = 0xff). --------------
    {
        sim_fixture fx;
        fx.reset();
        fx.width_m  = 0xff;
        fx.height_m = 0xff;

        llm_map_region NB{}; // the neighbor region to inherit
        NB.cell_count = 0;

        sim_view  v                     = fx.view();
        sim_store own                   = fx.store();
        own.region_cell_at(6, 8).region = &NB; // +x neighbor of (5,8)

        uint8_t mask[100] = {};
        mask[0 * 10 + 0]  = 1; // (dx=0,dy=0) -> cell (5,8)

        reset_records();
        g_store = &own;
        detail::region_apply_area(v, own, g_calls, /*col*/ 5, /*row*/ 8, (const char *)mask);

        ck(own.region_cell_at(5, 8).region == &NB, "R1: masked cell (5,8) inherits neighbor NB");
        ck_eq(NB.cell_count, 1u, "R1: NB.cell_count incremented on assignment");
        ck_eq((uint32_t)g_pick.size(), 4u, "R1: exactly four pick_smaller queries for the one cell");
        ck(g_pick.size() == 4 && g_pick[0].block == nullptr && g_pick[0].x == 6 && g_pick[0].y == 8,
           "R1: query[0] = pick(null, +x=6, 8)");
        ck(g_pick.size() == 4 && g_pick[1].block == &NB && g_pick[1].x == 4 && g_pick[1].y == 8,
           "R1: query[1] = pick(NB, -x=4, 8) -- block chained from prior return");
        ck(g_pick.size() == 4 && g_pick[2].block == &NB && g_pick[2].x == 5 && g_pick[2].y == 9,
           "R1: query[2] = pick(NB, 5, +y=9)");
        ck(g_pick.size() == 4 && g_pick[3].block == &NB && g_pick[3].x == 5 && g_pick[3].y == 7,
           "R1: query[3] = pick(NB, 5, -y=7)");
        ck_eq((uint32_t)g_next_count, 0, "R1: get_next_block NOT called (inherited, never allocated)");
        ck_eq((uint32_t)g_merge, 1, "R1: merge_small_regions called once at the tail");
    }

    // ---- R2: allocation gate over TWO passes. Pass 1: the lone masked cell finds no neighbor and
    // allow_alloc is false -> unassigned. End of pass 1: unassigned>0 && assigned_total==0 -> arm. Pass 2:
    // same cell allocates via get_next_block, is seeded (x,y,cell_count 0 then ++), allow_alloc consumed.
    // The 8 (=4x2) pick_smaller queries prove alloc did NOT short-circuit pass 1. --------------------
    {
        sim_fixture fx;
        fx.reset();
        fx.width_m  = 0xff;
        fx.height_m = 0xff;

        llm_map_region allocNode{}; // what get_next_block hands back

        sim_view  v   = fx.view();
        sim_store own = fx.store();
        // no neighbor regions anywhere -> every pick_smaller returns null

        uint8_t mask[100] = {};
        mask[0 * 10 + 0]  = 1; // cell (3,4)

        reset_records();
        g_store = &own;
        g_next  = &allocNode;
        detail::region_apply_area(v, own, g_calls, /*col*/ 3, /*row*/ 4, (const char *)mask);

        ck_eq((uint32_t)g_next_count, 1, "R2: get_next_block called exactly once (pass 2 only)");
        ck_eq((uint32_t)allocNode.x, 3u, "R2: fresh node seeded x = tile col");
        ck_eq((uint32_t)allocNode.y, 4u, "R2: fresh node seeded y = tile row");
        ck_eq(allocNode.cell_count, 1u, "R2: fresh node cell_count 0 then ++ on assignment");
        ck(own.region_cell_at(3, 4).region == &allocNode, "R2: cell (3,4) claims the fresh region");
        ck_eq((uint32_t)g_pick.size(), 8u,
              "R2: 8 pick queries (4/pass x 2 passes) -- proves NO allocation happened in pass 1");
        ck_eq((uint32_t)g_merge, 1, "R2: merge_small_regions called once");
    }

    // ---- R3: the "once any cell is assigned, alloc never re-arms" gate. Two masked cells: B=(5,5)
    // (processed first, x-outer) is stuck in pass 1 (its +x neighbor A=(6,5) is not yet assigned); A=(6,5)
    // then inherits a pre-placed SEED at (7,5). End pass 1: unassigned>0 BUT assigned_total>0 -> allow_
    // alloc stays FALSE. Pass 2: B now inherits SEED via the just-assigned (6,5); A is skipped (grid
    // non-null). get_next_block is NEVER called -- the proof that the gate did not re-arm. -------------
    {
        sim_fixture fx;
        fx.reset();
        fx.width_m  = 0xff;
        fx.height_m = 0xff;

        llm_map_region SEED{};
        SEED.cell_count = 2;

        sim_view  v                     = fx.view();
        sim_store own                   = fx.store();
        own.region_cell_at(7, 5).region = &SEED; // A's (+x) neighbor -- the only pre-placed region

        uint8_t mask[100] = {};
        mask[0 * 10 + 0]  = 1; // (dx=0,dy=0) -> B = (5,5), processed FIRST
        mask[1 * 10 + 0]  = 1; // (dx=1,dy=0) -> A = (6,5), processed SECOND

        reset_records();
        g_store = &own;
        g_next  = nullptr; // if the gate wrongly re-armed, get_next_block would be called (and return null)
        detail::region_apply_area(v, own, g_calls, /*col*/ 5, /*row*/ 5, (const char *)mask);

        ck_eq((uint32_t)g_next_count, 0,
              "R3: get_next_block NEVER called -- alloc gate did not re-arm after assigned_total>0");
        ck(own.region_cell_at(6, 5).region == &SEED, "R3: A=(6,5) inherited SEED in pass 1");
        ck(own.region_cell_at(5, 5).region == &SEED,
           "R3: B=(5,5) inherited SEED in pass 2 (from the region pass 1 placed at (6,5))");
        ck_eq(SEED.cell_count, 4u, "R3: SEED.cell_count = 2 + 2 assignments (A and B)");
        ck_eq((uint32_t)g_merge, 1, "R3: merge_small_regions called once");
    }

    // ---- R4: neighbor-coordinate wrap. width_m=height_m=7, origin (0,0). Cell (0,0)'s -x neighbor is
    // (0-1)&7 = 7 and its -y neighbor is (0-1)&7 = 7 -- a region planted at the wrapped (7,0) is inherited
    // via the -x query. Asserts the recorded neighbor coords include the wrapped 7s (not 0xffffffff). ---
    {
        sim_fixture fx;
        fx.reset();
        fx.width_m  = 7;
        fx.height_m = 7;

        llm_map_region WNB{};
        WNB.cell_count = 0;

        sim_view  v                     = fx.view();
        sim_store own                   = fx.store();
        own.region_cell_at(7, 0).region = &WNB; // (0,0)'s -x neighbor, wrapped

        uint8_t mask[100] = {};
        mask[0 * 10 + 0]  = 1; // cell (0,0)

        reset_records();
        g_store = &own;
        detail::region_apply_area(v, own, g_calls, /*col*/ 0, /*row*/ 0, (const char *)mask);

        ck(own.region_cell_at(0, 0).region == &WNB, "R4: (0,0) inherits the wrapped -x neighbor WNB");
        ck_eq(WNB.cell_count, 1u, "R4: WNB.cell_count incremented");
        ck_eq((uint32_t)g_pick.size(), 4u, "R4: four neighbor queries");
        ck(g_pick.size() == 4 && g_pick[0].x == 1 && g_pick[0].y == 0, "R4: query[0] +x = (1,0)");
        ck(g_pick.size() == 4 && g_pick[1].x == 7 && g_pick[1].y == 0,
           "R4: query[1] -x wraps to (7,0), NOT (0xffffffff,0)");
        ck(g_pick.size() == 4 && g_pick[2].x == 0 && g_pick[2].y == 1, "R4: query[2] +y = (0,1)");
        ck(g_pick.size() == 4 && g_pick[3].x == 0 && g_pick[3].y == 7,
           "R4: query[3] -y wraps to (0,7), NOT (0,0xffffffff)");
        ck_eq((uint32_t)g_merge, 1, "R4: merge_small_regions called once");
    }

    // Mutation notes:
    //  - R1's g_pick[0..3] coord/block sequence catches a translation that reorders the four neighbor
    //    queries or fails to thread `cand` (block) from one call into the next.
    //  - R2's g_pick.size()==8 catches allow_alloc starting TRUE (it would allocate in pass 1, leaving
    //    only 4 queries); the seeded x/y/cell_count catch a mis-seeded fresh node.
    //  - R3's g_next_count==0 is the strongest check: a translation that arms allow_alloc whenever a cell
    //    is unassigned (ignoring assigned_total==0) would ALLOCATE for B in pass 2 -> get_next_block
    //    called once, red.
    //  - R4's query[1]/query[3] coords catch dropping `& width_m`/`& height_m` on the -x/-y neighbor
    //    computations (they would record 0xffffffff and index far out of bounds).
}

} // namespace mh::sim::test
