//
// sim/sim_map_region_recompute_adjacency.cpp -- see sim_map_region_recompute_adjacency.h. Translated
// address-by-address from the DISASSEMBLY
// (tmp/decomp/llm_map_region_recompute_adjacency_00422f0c.asm); the exported .c draft's control flow
// (list-reset loop, then the nested width/height grid scan with a right-neighbor check followed by a
// down-neighbor check) was independently re-traced branch-by-branch against the raw instruction
// sequence and agrees with the draft exactly.
//
#include "sim/sim_map_region_recompute_adjacency.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const map_region_recompute_adjacency_calls &live_map_region_recompute_adjacency_calls() {
    static const map_region_recompute_adjacency_calls c = {
        MH_LIBMH_BIND(llm_map_region_add_adjacency_edge),
    };
    return c;
}

namespace detail {

void region_recompute_adjacency(const sim_view &v, sim_store &own,
                                const map_region_recompute_adjacency_calls &c) {
    // 0x00422f24-0x00422f4c: walk the ACTIVE region list from `*region_list_head` via `.next`
    // (offset 0x40c), zeroing each node's own `.neighbor_count` (offset 0x8). A write through the
    // pointer the view hands out, not through sim_store -- see the header banner and
    // sim_map_region_split.cpp / sim_map_region_helpers.cpp's identical precedent for
    // llm_map_region node fields. An empty list (`*region_list_head == nullptr`) skips this loop
    // entirely, matching the original's own leading null check.
    for (llm_map_region *region = *v.region_list_head; region != nullptr; region = region->next) {
        region->neighbor_count = 0;
    }

    // 0x00422f4e-0x00423081: the full `[0, *map_width) x [0, *map_height)` tile-grid scan, x OUTER /
    // y inner -- matches sim_store::region_cell_at()'s own row/col math and every original site's
    // own `_G_LLM_MAP_REGION_GRID[x][y]` indexing. *v.map_width/*v.map_height are RID_WIDTH/
    // RID_HEIGHT (0x00825084/0x00825064) -- NOT the wrap-mask pair used for the neighbor lookups
    // below (a different pair of globals; see sim_state.h's width_m/height_m comment).
    for (int32_t x = 0; x < *v.map_width; ++x) {
        for (int32_t y = 0; y < *v.map_height; ++y) {
            // 0x00422f8c-0x00422fa3: this cell's owning region. A null region (no owner claimed
            // this tile yet) skips both neighbor checks below entirely.
            llm_map_region *region_a = own.region_cell_at(x, y).region;
            if (region_a == nullptr) continue;

            // 0x00422fad-0x00423012: the RIGHT neighbor, wrap-masked on x ONLY
            // (`(x + 1) & width_m`, y unchanged). *v.width_m is RID_WIDTH_M (0x00fe5b40), the torus
            // wrap mask -- distinct from *v.map_width above.
            const uint32_t  right_x = (static_cast<uint32_t>(x) + 1u) & *v.width_m;
            llm_map_region *right =
                own.region_cell_at(static_cast<int32_t>(right_x), y).region;
            // 0x00422fc2-0x00422fe9: call the edge helper iff the right neighbor is a DIFFERENT,
            // non-null region (a shared border between two distinct claimed regions).
            if (region_a != right && right != nullptr) {
                c.add_adjacency_edge(region_a, right);
            }

            // 0x00423012-0x00423077: the DOWN neighbor, wrap-masked on y ONLY
            // (`(y + 1) & height_m`, x unchanged). *v.height_m is RID_HEIGHT_M (0x00fe5b44).
            const uint32_t  down_y = (static_cast<uint32_t>(y) + 1u) & *v.height_m;
            llm_map_region *down =
                own.region_cell_at(x, static_cast<int32_t>(down_y)).region;
            // 0x00423027-0x0042304f: same distinct-and-non-null condition as the right check.
            if (region_a != down && down != nullptr) {
                c.add_adjacency_edge(region_a, down);
            }
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void region_recompute_adjacency() {
    sim_state st = state();
    detail::region_recompute_adjacency(st.read, st.own, live_map_region_recompute_adjacency_calls());
}


} // namespace mh::sim
