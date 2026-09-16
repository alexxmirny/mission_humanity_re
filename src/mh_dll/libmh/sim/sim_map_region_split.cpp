//
// sim/sim_map_region_split.cpp -- see sim_map_region_split.h. Translated address-by-address from the
// DISASSEMBLY (tmp/decomp/llm_map_region_split_00423fb8.asm); the exported .c draft's field-vs-store
// reading agrees with the raw instruction sequence (it was cross-checked against
// addr/mh_structs.gen.h's mh_llm_map_region / mh_llm_map_region_cell / mh_llm_map_bfs_entry layouts,
// not taken on the draft's own say-so).
//
#include "sim/sim_map_region_split.h"

#include "addr/mh_calls.gen.h"
#include "sim/libtrans/sim_lt_map_region_pool.h" // the rebound pool entry  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"                         // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

const map_region_split_calls &live_map_region_split_calls() {
    static const map_region_split_calls c = {
        &mh::sim::get_next_block, // REBOUND 2026-09-02: translated (LT1C)
    };
    return c;
}

namespace detail {
namespace {

// One of the flood-fill's four wrapped-neighbor absorption attempts -- the structurally-identical
// 0x00424073-0x00424124 (x+1), 0x00424127-0x00424212 (x-1), 0x00424212-0x004242fb (y+1), and
// 0x004242fb-0x004243e4 (y-1) inline blocks, de-duplicated into one helper per the batch context
// doc's explicit permission ("a translation MAY factor this into a loop/helper as long as the
// observable behavior ... is unchanged"). Reads `queue[read_idx].x/.y/.depth` and `*v.width_m`/
// `*v.height_m` FRESH on every call (never through a value cached across the outer loop or across the
// four per-iteration calls) -- matching the original, which re-derives every one of these off memory
// independently at each of its four inline blocks rather than holding one register across all four.
//
// `dx`/`dy` is the direction being tried (exactly one of them non-zero, matching the original's
// x-only / y-only per-block shape); the OTHER axis reads back unmodified from the queue entry, same
// as every one of the four original blocks does.
void try_absorb_neighbor(const sim_view &v, sim_store &own, const llm_map_region *old_region,
                         llm_map_region *new_region, llm_map_bfs_entry *queue, uint32_t read_idx,
                         int32_t dx, int32_t dy, uint32_t &write_idx) {
    const uint32_t width_m  = *v.width_m;
    const uint32_t height_m = *v.height_m;

    const uint32_t nx = static_cast<uint32_t>(static_cast<int32_t>(queue[read_idx].x) + dx) & width_m;
    const uint32_t ny = static_cast<uint32_t>(static_cast<int32_t>(queue[read_idx].y) + dy) & height_m;

    llm_map_region_cell &cell = own.region_cell_at(static_cast<int32_t>(nx), static_cast<int32_t>(ny));
    if (cell.region == old_region) {
        cell.region = new_region;

        llm_map_bfs_entry &e = queue[write_idx];
        e.x                  = static_cast<uint8_t>(nx);
        e.y                  = static_cast<uint8_t>(ny);
        e.depth              = static_cast<uint16_t>(queue[read_idx].depth + 1);
        write_idx            = (write_idx + 1) & 0x7ffu;

        new_region->cell_count += 1;
    }
}

} // namespace

llm_map_region *region_split(const sim_view &v, sim_store &own, const map_region_split_calls &c,
                             llm_map_region *old_region, int32_t seed_col, int32_t seed_row) {
    // 0x00423fd7-0x00423fdc: allocate the fresh node. See the header's SHADOW-ARM ALLOCATION HAZARD
    // note -- this call runs for real under every arm, by design.
    llm_map_region *new_region = static_cast<llm_map_region *>(c.get_next_block());

    // 0x00423fdf-0x00423ff4: seed the new node's own fields (heap memory, dereferenced directly --
    // see the header's region-pool-architecture note).
    new_region->x          = static_cast<uint8_t>(seed_col);
    new_region->y          = static_cast<uint8_t>(seed_row);
    new_region->cell_count = 1;

    // 0x00423ffb-0x0042400c: claim the seed cell. NOT wrap-masked, unlike every BFS-derived neighbor
    // below -- see the header's "seed cell is not wrap-masked" note.
    own.region_cell_at(seed_col, seed_row).region = new_region;

    // 0x00424019-0x0042402b: seed the BFS queue's own entry 0 (the write index starts at 1, the read
    // index at 0 -- entry 0 is filled directly rather than through the absorb helper above).
    llm_map_bfs_entry *queue = own.region_bfs_queue();
    queue[0].x               = static_cast<uint8_t>(seed_col);
    queue[0].y               = static_cast<uint8_t>(seed_row);

    // 0x00424030-0x004243f5: the flood-fill loop itself. `write_idx != read_idx` is the original's own
    // termination test (0x00424030-0x00424036); the read index advances by exactly one entry per loop
    // pass (0x004243e4-0x004243ed), wrapping at & 0x7ff same as every write index update -- no
    // overflow check anywhere, reproduced as such (see the header banner).
    uint32_t write_idx = 1;
    for (uint32_t read_idx = 0; write_idx != read_idx; read_idx = (read_idx + 1) & 0x7ffu) {
        // Order matters (free to preserve, cheap to verify) -- x+1, x-1, y+1, y-1, matching the
        // original's own inline block order exactly.
        try_absorb_neighbor(v, own, old_region, new_region, queue, read_idx, +1, 0, write_idx);
        try_absorb_neighbor(v, own, old_region, new_region, queue, read_idx, -1, 0, write_idx);
        try_absorb_neighbor(v, own, old_region, new_region, queue, read_idx, 0, +1, write_idx);
        try_absorb_neighbor(v, own, old_region, new_region, queue, read_idx, 0, -1, write_idx);
    }

    // 0x004243f5-0x00424405.
    return new_region;
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------

llm_map_region *region_split(llm_map_region *old_region, int32_t seed_col, int32_t seed_row) {
    sim_state st = state();
    return detail::region_split(st.read, st.own, live_map_region_split_calls(), old_region, seed_col,
                                seed_row);
}


} // namespace mh::sim
