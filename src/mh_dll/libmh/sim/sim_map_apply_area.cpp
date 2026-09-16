//
// sim/sim_map_apply_area.cpp -- see sim_map_apply_area.h. Translated from the DISASSEMBLY
// (tmp/decomp/map_ApplyAreaToMap_00424406.asm, tmp/decomp/llm_map_region_apply_area_004245c3.asm) --
// both .c drafts were cross-checked line-by-line against the raw instruction sequence and agree; the
// header banner documents the two non-obvious readings (the .prev-as-counter reuse and the alloc-gate
// state machine) that a plain read of either draft would not make obvious on its own.
//
#include "sim/sim_map_apply_area.h"

#include "addr/mh_calls.gen.h"
#include "sim/libtrans/sim_lt_map_region_pool.h" // the rebound pool entries  // typed callables for the two still-original callees (DECLARED NEED)
#include "ai/ai_state.h"                         // ai_say / trace_budget -- the shared trace sink, not AI state

// This slice's own translated siblings -- their PUBLIC wrappers are bound into live_map_apply_area_calls()
// below (the detail:: bodies reach them as c.region_split(...) / c.region_pick_smaller(...) /
// c.merge_small_regions(), routed through the `_calls` struct for offline testability -- see the header
// banner's "CALL ROUTING" section). DECLARED NEED: these two headers are written by other translators in
// this same batch and may not exist yet at review time; the file names follow the batch doc's own TU
// names ("sim_map_region_split TU" / "sim_map_region_helpers TU") verbatim.
#include "sim/sim_map_region_helpers.h" // mh::sim::region_pick_smaller / mh::sim::merge_small_regions
#include "sim/sim_map_region_split.h"   // mh::sim::region_split

namespace mh::sim {

const map_apply_area_calls &live_map_apply_area_calls() {
    static const map_apply_area_calls c = {
        &mh::sim::region_free,         // REBOUND 2026-09-02: translated (LT1C), ours binds directly
        &mh::sim::get_next_block,      // REBOUND 2026-09-02: same
        &mh::sim::region_split,        // translated sibling's public wrapper (see the header's CALL ROUTING)
        &mh::sim::region_pick_smaller, // translated sibling's public wrapper
        &mh::sim::merge_small_regions, // translated sibling's public wrapper
    };
    return c;
}

namespace detail {

void apply_area_to_map(const sim_view &v, sim_store &own, const map_apply_area_calls &c, int32_t x_0,
                       int32_t y_0, const uint8_t *area) {
    // Phase 1 (0x00424425-0x00424450): zero `.prev` (the repurposed per-call "cells cleared" counter,
    // see the header banner) on every region currently on the active list.
    for (llm_map_region *r = *v.region_list_head; r != nullptr; r = r->next) {
        r->prev = nullptr;
    }

    // Phase 2 (0x00424452-0x00424520): for every masked cell in the 10x10 footprint, detach it from its
    // owning region (if any) -- bump that region's cleared-cell counter and shrink cell_count -- then
    // null the grid cell. The mask pointer advances once per (dx,dy) unconditionally, matching the
    // asm's single increment site (LAB_00424510) reached on both the taken and not-taken paths.
    const uint8_t *mask = area;
    for (int32_t dx = 0; dx < 10; ++dx) {
        uint32_t x = (uint32_t)(x_0 + dx) & *v.width_m;
        for (int32_t dy = 0; dy < 10; ++dy) {
            uint32_t y = (uint32_t)(y_0 + dy) & *v.height_m;
            if (*mask != 0) {
                llm_map_region_cell &cell = own.region_cell_at((int32_t)x, (int32_t)y);
                if (cell.region != nullptr) {
                    // region->prev = (llm_map_region*)((uintptr_t)region->prev + 1) -- see the header
                    // banner's derivation of why this is an integer increment, not a pointer store.
                    cell.region->prev = (llm_map_region *)((uintptr_t)cell.region->prev + 1);
                    cell.region->cell_count -= 1;
                }
                cell.region = nullptr;
            }
            ++mask;
        }
    }

    // Phase 3 (0x00424520-0x004245b6): rescan the WHOLE map (plain map_width x map_height, NOT the
    // width_m/height_m torus masks used above -- a different global pair, see sim_state.h's own
    // distinction). Any cell still owning a region that was touched in phase 2 (region->prev != 0, i.e.
    // its cleared-cell counter is nonzero) gets split off; if the shrunk remainder is now empty, free
    // it.
    for (int32_t x = 0; x < *v.map_width; ++x) {
        for (int32_t y = 0; y < *v.map_height; ++y) {
            llm_map_region_cell &cell       = own.region_cell_at(x, y);
            llm_map_region      *old_region = cell.region;
            if (old_region != nullptr && old_region->prev != nullptr) {
                llm_map_region *split_off = c.region_split(old_region, x, y);
                old_region->cell_count -= split_off->cell_count;
                if (old_region->cell_count == 0) {
                    c.region_free((uint32_t)(uintptr_t)old_region);
                }
            }
        }
    }

    c.merge_small_regions();
}

void region_apply_area(const sim_view &v, sim_store &own, const map_apply_area_calls &c,
                       uint32_t tile_col_origin, int32_t tile_row_origin, const char *area_mask) {
    bool    allow_alloc    = false; // bVar1 / local_14 -- see the header banner's state-machine derivation
    int32_t assigned_total = 0;     // local_1c / local_18 -- CUMULATIVE, initialised once, never reset below

    for (;;) {
        int32_t     unassigned_this_pass = 0;         // local_20 / local_1c -- reset every pass
        const char *mask                 = area_mask; // equivalent to the asm's +100/-100 pointer dance

        for (uint32_t dx = 0; dx < 10; ++dx) {
            uint32_t x = (tile_col_origin + dx) & *v.width_m;
            for (uint32_t dy = 0; dy < 10; ++dy) {
                uint32_t y = ((uint32_t)tile_row_origin + dy) & *v.height_m;

                if (*mask != 0 && own.region_cell_at((int32_t)x, (int32_t)y).region == nullptr) {
                    // region_pick_smaller's committed public wrapper takes and returns the real
                    // struct pointer (sig_llm_map_region_pick_smaller), so these four calls need no
                    // cast -- see sim_map_region_helpers.h's "NAMING" banner.
                    llm_map_region *cand = nullptr;
                    cand                 = c.region_pick_smaller(cand, (int32_t)((x + 1) & *v.width_m), (int32_t)y);
                    cand                 = c.region_pick_smaller(cand, (int32_t)((x - 1) & *v.width_m), (int32_t)y);
                    cand                 = c.region_pick_smaller(cand, (int32_t)x, (int32_t)((y + 1) & *v.height_m));
                    cand                 = c.region_pick_smaller(cand, (int32_t)x, (int32_t)((y - 1) & *v.height_m));

                    if (cand == nullptr && allow_alloc) {
                        cand             = (llm_map_region *)c.get_next_block();
                        cand->x          = (uint8_t)x;
                        cand->y          = (uint8_t)y;
                        cand->cell_count = 0;
                        allow_alloc      = false;
                    }

                    if (cand == nullptr) {
                        ++unassigned_this_pass;
                    } else {
                        own.region_cell_at((int32_t)x, (int32_t)y).region = cand;
                        cand->cell_count += 1;
                        ++assigned_total;
                    }
                }
                ++mask;
            }
        }

        if (unassigned_this_pass == 0)
            break;
        if (assigned_total == 0)
            allow_alloc = true;
        // else: leave allow_alloc as-is and loop again -- a later pass's neighbor-inheritance may still
        // pick up a region an earlier pass placed (see the header banner).
    }

    c.merge_small_regions();
}

} // namespace detail

void apply_area_to_map(int32_t x_0, int32_t y_0, uint8_t *area) {
    sim_state st = state();
    detail::apply_area_to_map(st.read, st.own, live_map_apply_area_calls(), x_0, y_0, area);
}

void region_apply_area(uint32_t tile_col_origin, int32_t tile_row_origin, char *area_mask) {
    sim_state st = state();
    detail::region_apply_area(st.read, st.own, live_map_apply_area_calls(), tile_col_origin,
                              tile_row_origin, area_mask);
}


} // namespace mh::sim
