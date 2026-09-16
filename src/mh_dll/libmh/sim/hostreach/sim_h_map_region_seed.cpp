//
// sim/hostreach/sim_h_map_region_seed.cpp -- see sim_h_map_region_seed.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_map_try_seed_region_at_004228e9.asm,
// tmp/decomp_sim/llm_map_seed_regions_multires_00422c54.asm); the .c drafts beside them were not
// trusted as the source (translator-brief rule 1).
//
#include "sim/hostreach/sim_h_map_region_seed.h"

#include "sim/libtrans/sim_lt_map_region_pool.h" // mh::sim::get_next_block() -- translated sibling

namespace mh::sim {

const map_region_seed_calls &live_map_region_seed_calls() {
    static const map_region_seed_calls c = {
        &mh::sim::get_next_block,
    };
    return c;
}

namespace {
// The flood-fill-pending grid-cell sentinel (mh_llm_map_region_cell::region's own field comment,
// mh_structs.gen.h). Same value/representation as sim_h_map_build_regions.cpp's own
// region_pending_sentinel() -- redeclared here because an anonymous-namespace symbol does not cross
// translation units.
inline llm_map_region *region_pending_sentinel() {
    return reinterpret_cast<llm_map_region *>(static_cast<uintptr_t>(0xffffffffu));
}
} // namespace

namespace detail {

// ---- llm_map_try_seed_region_at @0x004228e9 ----------------------------------------------------------
void try_seed_region_at(const sim_view &v, sim_store &own, const map_region_seed_calls &c, int32_t x,
                        int32_t y, int32_t max_d) {
    // 0x00422908-0x00422917: all_pending starts true (the gate passes unless something fails it).
    bool all_pending = true;

    // 0x00422918-0x00422983: THE SCAN GATE. dx, dy each range -max_d..max_d INCLUSIVE (0x0042291b /
    // 0x00422947 are JLE). wx is hoisted out of the dy loop (computed once per dx, 0x0042292a-
    // 0x00422938) -- matches the asm's [EBP-0x14] reuse across the whole inner sweep.
    for (int32_t dx = -max_d; dx <= max_d; ++dx) {
        const uint32_t wx = static_cast<uint32_t>(x + dx) & *v.width_m;
        for (int32_t dy = -max_d; dy <= max_d; ++dy) {
            const uint32_t wy = static_cast<uint32_t>(y + dy) & *v.height_m;
            // 0x0042296f-0x00422976: a NOT-pending cell fails the gate. `break` here reproduces
            // 0x0042297f/0x00422983's JMP to the OUTER loop's own increment exactly -- the outer dx
            // loop keeps running (see the header banner's non-early-exit note), which this natural
            // loop shape reproduces without any extra code.
            if (own.region_cell_at(static_cast<int32_t>(wx), static_cast<int32_t>(wy)).region !=
                region_pending_sentinel()) {
                all_pending = false;
                break;
            }
        }
    }

    // 0x00422985-0x00422989: only when EVERY cell in the square was still pending do we allocate.
    if (!all_pending) return;

    // 0x0042298b-0x004229a2: allocate + stamp the ORIGINAL (unwrapped) x/y, truncated to uint8_t by
    // the asm's own 8-bit load (MOV AL, byte ptr [x|y]) -- see the header banner's BYTE TRUNCATION note.
    llm_map_region *block = c.get_next_block();
    block->x              = static_cast<uint8_t>(x);
    block->y              = static_cast<uint8_t>(y);

    // 0x004229a5-0x004229b8: flood-fill from the ORIGINAL (x, y) and stamp the returned cell count.
    // INTRA-WAVE sibling carrying no `_calls` struct of its own (confirmed against the landed
    // sim_h_map_region_flood_fill.h) -- called as a plain detail:: function, nothing to thread.
    // NOTE (header banner): unlike assign_remaining_tiles_to_regions' own fresh-allocation branch,
    // there is no further store to region_cell_at(x, y) here -- confirmed absent from this function's
    // own disassembly. Installing the seed cell's ownership is region_flood_fill's own job (confirmed:
    // its prologue stores the grid cell before this call returns).
    block->cell_count = static_cast<uint32_t>(
        region_flood_fill(v, own, static_cast<uint32_t>(x), static_cast<uint32_t>(y), block));
}

// ---- llm_map_seed_regions_multires @0x00422c54 -------------------------------------------------------
void seed_regions_multires(const sim_view &v, sim_store &own, const map_region_seed_calls &c,
                           const map_build_regions_calls &c_build) {
    // 0x00422c6c-0x00422c8a: reset the region graph's four fixed-VA scalars -- a SECOND writer of
    // list_head/counter beyond build_regions' own reset immediately before this call, and the ONLY
    // writer (in a full rebuild) of pool_free_head/last_map_index. See the header banner's FOUR-SCALAR
    // RESET note -- transcribed literally, redundancy and all.
    own.region_list_head_mut()  = nullptr;
    own.region_pool_free_head() = nullptr;
    own.last_map_index()        = 0;
    own.region_alloc_counter()  = 1;

    // 0x00422c94-0x00422df8: five passes at decreasing max_d, x outer / y inner, stride = 1 << (max_d
    // - 1) -- see the header banner's THE MULTI-RESOLUTION SEQUENCE for the per-pass addresses.
    static constexpr int32_t kSeedMaxDs[] = {5, 4, 3, 2, 1};
    for (int32_t max_d : kSeedMaxDs) {
        const int32_t stride = 1 << (max_d - 1);
        for (int32_t x = 0; x < *v.map_width; x += stride) {
            for (int32_t y = 0; y < *v.map_height; y += stride) {
                // Intra-unit sibling call, same `c` (rule 3b/3c's same-TU shape).
                try_seed_region_at(v, own, c, x, y, max_d);
            }
        }
    }

    // 0x00422dfa: the fixpoint sweep for whatever the multi-res passes above left pending. Wave-1
    // sibling (sim_h_map_build_regions.h), a direct `detail::` call threading `c_build` in rather than
    // binding live_map_build_regions_calls() here (rule 3c).
    assign_remaining_tiles_to_regions(v, own, c_build);
}

} // namespace detail


void try_seed_region_at(int32_t x, int32_t y, int32_t max_d) {
    sim_state st = state();
    detail::try_seed_region_at(st.read, st.own, live_map_region_seed_calls(), x, y, max_d);
}

void seed_regions_multires() {
    sim_state st = state();
    detail::seed_regions_multires(st.read, st.own, live_map_region_seed_calls());
}

} // namespace mh::sim
