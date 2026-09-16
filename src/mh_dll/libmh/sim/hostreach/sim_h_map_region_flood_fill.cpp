//
// sim/hostreach/sim_h_map_region_flood_fill.cpp -- see sim_h_map_region_flood_fill.h. Translated from
// the DISASSEMBLY (tmp/decomp_sim/llm_map_region_flood_fill_00422450.asm) -- the Ghidra .c draft beside
// it was cross-checked instruction-by-instruction against the raw sequence and agrees with the header
// banner's readings (sentinel handling, the two depth compares, the queue-count normalization, the
// neighbour wrap masks, the return-value derivation).
//
#include "sim/hostreach/sim_h_map_region_flood_fill.h"

namespace mh::sim {

namespace {
// The flood-fill-pending grid-cell sentinel (mh_llm_map_region_cell::region's own field comment),
// same technique as sim_map_region_helpers.cpp's own region_pick_smaller / sim_h_map_build_regions.cpp:
// compared via uintptr_t since the sentinel is not a real address a `llm_map_region *` literal could
// portably encode.
inline bool is_pending(const llm_map_region_cell &cell) {
    return reinterpret_cast<uintptr_t>(cell.region) == 0xffffffffu;
}
} // namespace

namespace detail {

// ---- llm_map_region_flood_fill @0x00422450 -----------------------------------------------------------
int32_t region_flood_fill(const sim_view &v, sim_store &own, uint32_t x, uint32_t y, llm_map_region *block) {
    llm_map_bfs_entry *path = own.region_flood_path_queue(); // `path_`, llm_map_bfs_entry[2048]

    // 0x00422476-0x00422487: claim the seed cell for `block` immediately. NOT wrap-masked -- see the
    // header banner's "THE SEED WRITE IS NOT WRAP-MASKED" note. `region_cell_at` reproduces the same
    // `(x<<0xb)+(y<<3)` byte-address arithmetic via `x*MAP_GRID_DIM+y` element indexing.
    own.region_cell_at(static_cast<int32_t>(x), static_cast<int32_t>(y)).region = block;

    // 0x0042248d/0x00422494: tail=1, head=0 (indices into path_/region_flood_path_queue()).
    uint32_t tail = 1;
    uint32_t head = 0;

    // 0x0042249b-0x004224af: path_[0] = {depth 0, x, y}. x/y are byte-TRUNCATED (not wrap-masked) --
    // see the header banner note; matches the plain `MOV byte ptr` stores in the assembly.
    path[0].depth = 0;
    path[0].x     = static_cast<uint8_t>(x);
    path[0].y     = static_cast<uint8_t>(y);

    const uint32_t width_m  = *v.width_m;  // RID_WIDTH_M -- read-only here (llm_map_build_regions writes it)
    const uint32_t height_m = *v.height_m; // RID_HEIGHT_M

    int32_t cell_count = 1; // [EBP-0x18]: seed counted as claimed already.

    for (;;) {
        // 0x004224b4-0x004224ba: queue empty (tail caught up to head) -> done, whole BFS exhausted.
        if (tail == head) break;

        // 0x004224c0-0x004224ce: DEPTH CAP -- path_[head].depth > 0xe (14, unsigned) stops the ENTIRE
        // flood fill (jumps straight past the head-advance to the function's single exit), not just
        // this entry. See header banner "THE TWO DEPTH COMPARES".
        if (path[head].depth > 0xe) break;

        // 0x004224d4-0x004224eb: queue_count, normalized into [1, 0x800] -- see header banner.
        uint32_t queue_count = tail + 0x800u - head;
        if (queue_count > 0x800u) queue_count -= 0x800u;

        // 0x004224f2-0x00422522: FILL-RATE HEURISTIC, only evaluated for depth > 3 (<=3 always falls
        // through to expansion, 0x00422500 JBE). depth*0x234 > queue_count*0x190 -> stop the entire
        // flood fill (same exit as the depth cap above).
        if (path[head].depth > 3) {
            const uint32_t depth32 = path[head].depth;
            if (depth32 * 0x234u > queue_count * 0x190u) break;
        }

        // ---- expand the four orthogonal neighbours of path_[head] (0x00422527-0x004228c7) ----------
        const uint8_t  hx         = path[head].x;
        const uint8_t  hy         = path[head].y;
        const uint16_t next_depth = static_cast<uint16_t>(path[head].depth + 1);

        // +x (0x00422527-0x00422610)
        {
            const uint32_t       nx   = (static_cast<uint32_t>(hx) + 1u) & width_m;
            const uint32_t       ny   = static_cast<uint32_t>(hy) & height_m;
            llm_map_region_cell &cell = own.region_cell_at(static_cast<int32_t>(nx), static_cast<int32_t>(ny));
            if (is_pending(cell)) {
                cell.region      = block;
                path[tail].x     = static_cast<uint8_t>(nx);
                path[tail].y     = static_cast<uint8_t>(ny);
                path[tail].depth = next_depth;
                tail             = (tail + 1u) & 0x7ffu;
                ++cell_count;
            }
        }

        // -x (0x00422610-0x004226f9)
        {
            const uint32_t       nx   = (static_cast<uint32_t>(hx) - 1u) & width_m;
            const uint32_t       ny   = static_cast<uint32_t>(hy) & height_m;
            llm_map_region_cell &cell = own.region_cell_at(static_cast<int32_t>(nx), static_cast<int32_t>(ny));
            if (is_pending(cell)) {
                cell.region      = block;
                path[tail].x     = static_cast<uint8_t>(nx);
                path[tail].y     = static_cast<uint8_t>(ny);
                path[tail].depth = next_depth;
                tail             = (tail + 1u) & 0x7ffu;
                ++cell_count;
            }
        }

        // +y (0x004226f9-0x004227e0)
        {
            const uint32_t       nx   = static_cast<uint32_t>(hx) & width_m;
            const uint32_t       ny   = (static_cast<uint32_t>(hy) + 1u) & height_m;
            llm_map_region_cell &cell = own.region_cell_at(static_cast<int32_t>(nx), static_cast<int32_t>(ny));
            if (is_pending(cell)) {
                cell.region      = block;
                path[tail].x     = static_cast<uint8_t>(nx);
                path[tail].y     = static_cast<uint8_t>(ny);
                path[tail].depth = next_depth;
                tail             = (tail + 1u) & 0x7ffu;
                ++cell_count;
            }
        }

        // -y (0x004227e0-0x004228c7)
        {
            const uint32_t       nx   = static_cast<uint32_t>(hx) & width_m;
            const uint32_t       ny   = (static_cast<uint32_t>(hy) - 1u) & height_m;
            llm_map_region_cell &cell = own.region_cell_at(static_cast<int32_t>(nx), static_cast<int32_t>(ny));
            if (is_pending(cell)) {
                cell.region      = block;
                path[tail].x     = static_cast<uint8_t>(nx);
                path[tail].y     = static_cast<uint8_t>(ny);
                path[tail].depth = next_depth;
                tail             = (tail + 1u) & 0x7ffu;
                ++cell_count;
            }
        }

        // 0x004228c7-0x004228d0: advance head, wrap mod 0x800 -- NO overflow guard against `tail`; see
        // header banner "QUEUE DISCIPLINE".
        head = (head + 1u) & 0x7ffu;
    }

    return cell_count; // 0x004228d8-0x004228de
}

} // namespace detail


int32_t region_flood_fill(uint32_t x, uint32_t y, llm_map_region *block) {
    sim_state st = state();
    return detail::region_flood_fill(st.read, st.own, x, y, block);
}

} // namespace mh::sim
