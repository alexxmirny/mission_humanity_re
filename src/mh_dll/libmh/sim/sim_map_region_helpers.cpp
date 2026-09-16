//
// sim/sim_map_region_helpers.cpp -- see sim_map_region_helpers.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_map_region_pick_smaller_004229c3.asm,
// tmp/decomp/llm_map_merge_small_regions_004230f7.asm) -- region_pick_smaller's own four-way boolean
// chain and merge_small_regions' goto-loop were both independently re-traced register-by-register
// against the raw instruction sequence; the Ghidra .c drafts happen to agree with both readings
// (cross-checked, not rubber-stamped) except for the control-flow SHAPE of merge_small_regions' outer
// goto, which is translated here as an equivalent restart loop per house rules (the batch context's own
// instruction), not transcribed as a literal goto.
//
#include "sim/sim_map_region_helpers.h"

#include "addr/mh_calls.gen.h"
#include "sim/libtrans/sim_lt_map_region_pool.h" // the rebound pool entry  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"                         // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h"                  // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const map_region_helpers_calls &live_map_region_helpers_calls() {
    static const map_region_helpers_calls c = {
        &mh::sim::region_free, // REBOUND 2026-09-02: translated (LT1C)
        MH_LIBMH_BIND(llm_map_region_recompute_adjacency),
    };
    return c;
}

namespace detail {

llm_map_region *region_pick_smaller(sim_store &own, llm_map_region *block, int32_t x, int32_t y) {
    // 0x004229e2-0x004229f6: _G_LLM_MAP_REGION_GRID[x][y].region -- x outer, y inner.
    llm_map_region *cell_region = own.region_cell_at(x, y).region;

    // 0x004229f9-0x00422a05: the flood-fill-pending sentinel (0xffffffff, not a real pointer) is
    // treated identically to "no region" (0x0) -- both return `block` unchanged. Compared via uintptr_t
    // since the sentinel is not a real address a `llm_map_region *` literal could portably encode.
    const bool cell_is_sentinel = reinterpret_cast<uintptr_t>(cell_region) == 0xffffffffu;
    if (cell_is_sentinel || cell_region == nullptr) return block;

    // 0x00422a0d-0x00422a19: the cell's region is real, but the caller passed no candidate of its own
    // -- the cell's own region wins by default.
    if (block == nullptr) return cell_region;

    // 0x00422a1b-0x00422a37: both real -- keep whichever has FEWER cells. Strict `<`; a tie or
    // cell_region->cell_count >= block->cell_count keeps `block` (JNC at 0x00422a27 skips the update).
    if (cell_region->cell_count < block->cell_count) return cell_region;
    return block;
}

void merge_small_regions(const sim_view &v, sim_store &own, const map_region_helpers_calls &c) {
    // 0x0042310f: unconditional, runs before the scan below looks at anything.
    c.region_recompute_adjacency();

    for (;;) {
        // 0x00423114 (LAB_00423114): re-read the ACTIVE list head fresh on every restart (W2: never
        // cache an address across iterations).
        llm_map_region *region   = *v.region_list_head;
        llm_map_region *absorbed = nullptr;

        // 0x0042311c-0x0042328a: walk the active list via `.next` for the first region that is both
        // under the merge threshold and has a valid merge candidate.
        for (; region != nullptr; region = region->next) {
            // 0x0042313b-0x00423141: JNC (unsigned) -- not under the threshold, skip this region.
            if (region->cell_count >= *v.region_merge_threshold) continue;

            absorbed             = nullptr;
            uint32_t best_weight = 0;
            for (int32_t i = 0; i < static_cast<int32_t>(region->neighbor_count); ++i) {
                llm_map_region *nb        = region->neighbors[i];
                const uint32_t  nb_weight = region->neighbor_data[i];
                // 0x00423182-0x00423191: candidate iff the merged size stays under 0x1ea cells, OR
                // this is the region's ONLY neighbor (no other choice exists).
                if ((nb->cell_count + region->cell_count < 0x1eau) || (region->neighbor_count == 1u)) {
                    // 0x00423193-0x004231ee: first candidate always wins; afterwards only a STRICTLY
                    // higher weight replaces it (JBE at 0x004231ab skips on a tie or a lower weight).
                    if (absorbed == nullptr) {
                        absorbed    = nb;
                        best_weight = nb_weight;
                    } else if (best_weight < nb_weight) {
                        absorbed    = nb;
                        best_weight = nb_weight;
                    }
                }
            }
            if (absorbed != nullptr) break; // 0x004231f6-0x004231fa: candidate found, stop scanning
        }

        // 0x0042311c/0x00423122: the list is exhausted with nothing to merge anywhere -- the loop's
        // only exit.
        if (region == nullptr) return;

        // 0x00423200-0x0042320c: region absorbs `absorbed`'s cell_count.
        region->cell_count += absorbed->cell_count;

        // 0x00423213-0x00423276: repoint every grid cell owned by `absorbed` to `region`. x is the
        // OUTER loop, matching sim_store::region_cell_at()'s own x-outer convention and every original
        // site's own `_G_LLM_MAP_REGION_GRID[x][y]` indexing. *v.map_width/*v.map_height are RID_WIDTH/
        // RID_HEIGHT (0x00825084/0x00825064, confirmed by address against addr/mh_regions.gen.h) -- NOT
        // the wrap-mask pair sim_view::width_m/height_m bind (a different region; see that member's own
        // comment in sim_state.h).
        for (int32_t x = 0; x < *v.map_width; ++x) {
            for (int32_t y = 0; y < *v.map_height; ++y) {
                llm_map_region_cell &cell = own.region_cell_at(x, y);
                if (cell.region == absorbed) cell.region = region;
            }
        }

        // 0x00423278-0x00423285: free the absorbed node (an ALREADY-EXISTING pointer, not a fresh
        // allocation -- no shadow-arm pointer-identity hazard, unlike llm_map_region_split), re-run
        // adjacency, and restart the whole scan from the list head (the original's `goto LAB_00423114`).
        c.region_free(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(absorbed)));
        c.region_recompute_adjacency();
    }
}

} // namespace detail

// ---- the public wrappers ------------------------------------------------------------------------

llm_map_region *region_pick_smaller(llm_map_region *block, int32_t x, int32_t y) {
    sim_state st = state();
    return detail::region_pick_smaller(st.own, block, x, y);
}

void merge_small_regions() {
    sim_state st = state();
    detail::merge_small_regions(st.read, st.own, live_map_region_helpers_calls());
}


} // namespace mh::sim
