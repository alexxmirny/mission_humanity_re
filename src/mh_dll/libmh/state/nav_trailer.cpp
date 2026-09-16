//
// state/nav_trailer.cpp -- see state/nav_trailer.h for the format and why it exists.
//
#include "state/nav_trailer.h"

#include <cstring>

#include "sim/libtrans/sim_lt_map_region_pool.h" // live_lt_map_region_pool_calls -- the GAME's allocator
#include "sim/sim_state.h"

namespace mh::state::nav {
namespace {

using mh::sim::llm_map_region;
using mh::sim::llm_map_region_cell;

import_stats g_last;

// THE CELL STRIDE, and it is the single easiest way to get a wrong number in this file.
// `llm_map_region_cell` is `{llm_map_region *region; uint32_t terrain_flags;}` -- EIGHT bytes, of
// which only the FIRST dword is the pointer. A 4-byte walk over MAP_REGION_GRID mixes terrain_flags
// in and produces a partition that looks plausible and is garbage: it cost one wrong measurement
// (188 regions / 1.679% of tiles differing) before the static_assert below was written, against the
// correct 180 / 3.328%. Stride 8, offset 0, always -- and the compiler now says so.
static_assert(sizeof(llm_map_region_cell) == 8, "the nav grid cell is 8 bytes, not 4");
static_assert(offsetof(llm_map_region_cell, region) == 0, "the cell's pointer is its first dword");

// ---- the pointer <-> slot map --------------------------------------------------------------------
//
// Node addresses are individual utils_malloc results, so there is no array to index and the ONLY way
// to enumerate the pool is to walk its two lists. This is the resulting (ptr, slot) table: built in
// traversal order, then sorted once for lookup. Insertion sort over <= 4096 entries, run once per
// capture on a step the run has already hashed -- the constant factor is irrelevant, being obvious
// is not (the same reasoning world_snapshot.cpp's census states).
struct slot_map {
    const llm_map_region *ptr[MAX_NODES];
    uint16_t              slot[MAX_NODES];
    uint32_t              n = 0;

    bool add(const llm_map_region *p, uint16_t s) {
        if (n >= MAX_NODES) return false;
        ptr[n]  = p;
        slot[n] = s;
        ++n;
        return true;
    }
    void sort() {
        for (uint32_t i = 1; i < n; ++i) {
            const llm_map_region *pv = ptr[i];
            const uint16_t        sv = slot[i];
            uint32_t              j  = i;
            while (j > 0 && ptr[j - 1] > pv) {
                ptr[j]  = ptr[j - 1];
                slot[j] = slot[j - 1];
                --j;
            }
            ptr[j]  = pv;
            slot[j] = sv;
        }
    }
    // NO_SLOT for null. `found` distinguishes "null" from "not in the pool", because those two must
    // NOT be conflated: a null neighbour is a fact, an unmapped one is an incomplete capture.
    uint16_t lookup(const llm_map_region *p, bool *found) const {
        *found = true;
        if (p == nullptr) return NO_SLOT;
        uint32_t lo = 0, hi = n;
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2;
            if (ptr[mid] == p) return slot[mid];
            if (ptr[mid] < p) lo = mid + 1;
            else hi = mid;
        }
        *found = false;
        return NO_SLOT;
    }
};

uint32_t fnv1a32(const void *p, size_t n) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    uint32_t       h = 2166136261u;
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

size_t body_bytes(uint32_t node_count) {
    return static_cast<size_t>(node_count) * sizeof(trailer_node) + BY_INDEX_N * 2u + GRID_CELLS * 2u;
}

} // namespace

// ---- capture -------------------------------------------------------------------------------------

int capture(void *buf, size_t cap, size_t *out_len) {
    if (buf == nullptr || out_len == nullptr) return NAV_ERR_ARG;
    mh::sim::sim_state  st  = mh::sim::state();
    mh::sim::sim_store &own = st.own;

    // (1) ENUMERATE THE POOL, assigning slots in traversal order. Walking the active list first and
    //     the free list second makes the slot space the pool's own contents and records both list
    //     ORDERS for free. Both walks are bounded by MAX_NODES: an intrusive list read out of a
    //     running process is exactly where a silent infinite loop lives, so a chain longer than the
    //     pool can possibly be is a refusal, not a hang.
    slot_map map;
    uint32_t active_count = 0, free_count = 0;
    for (const llm_map_region *p = own.region_list_head_mut(); p != nullptr; p = p->next) {
        if (!map.add(p, static_cast<uint16_t>(map.n))) return NAV_ERR_OVERFLOW;
        ++active_count;
        if (active_count > MAX_NODES) return NAV_ERR_CYCLE;
    }
    for (const llm_map_region *p = own.region_pool_free_head(); p != nullptr; p = p->next) {
        if (!map.add(p, static_cast<uint16_t>(map.n))) return NAV_ERR_OVERFLOW;
        ++free_count;
        if (free_count > MAX_NODES) return NAV_ERR_CYCLE;
    }
    const uint32_t node_count = map.n;

    // The traversal ORDER is what we just recorded; the lookup table needs address order. Keep a
    // copy of the traversal so the emit loop can still walk slots 0..n-1 in order.
    const llm_map_region *by_slot[MAX_NODES];
    for (uint32_t i = 0; i < node_count; ++i) by_slot[map.slot[i]] = map.ptr[i];
    map.sort();

    // A node reachable from BOTH heads would have been added twice and the slot space would be a
    // lie. After sorting, duplicates are adjacent.
    for (uint32_t i = 1; i < map.n; ++i)
        if (map.ptr[i] == map.ptr[i - 1]) return NAV_ERR_INCONSISTENT;

    if (cap < sizeof(trailer_header) + body_bytes(node_count)) return NAV_ERR_ARG;

    uint8_t       *out = static_cast<uint8_t *>(buf);
    trailer_header h;
    std::memset(&h, 0, sizeof(h));
    h.magic          = TRAILER_MAGIC;
    h.version        = TRAILER_VERSION;
    h.node_count     = node_count;
    h.by_index_count = BY_INDEX_N;
    h.grid_cells     = GRID_CELLS;
    h.active_count   = active_count;
    h.free_count     = free_count;

    bool found    = false;
    h.active_head = map.lookup(own.region_list_head_mut(), &found);
    if (!found) return NAV_ERR_UNMAPPED;
    h.free_head = map.lookup(own.region_pool_free_head(), &found);
    if (!found) return NAV_ERR_UNMAPPED;

    uint8_t *body = out + sizeof(trailer_header);
    size_t   at   = 0;

    // (2) THE NODES, in slot order.
    for (uint32_t s = 0; s < node_count; ++s) {
        const llm_map_region *src = by_slot[s];
        trailer_node          nd;
        std::memset(&nd, 0, sizeof(nd));
        nd.index          = src->index;
        nd.x              = src->x;
        nd.y              = src->y;
        nd.cell_count     = src->cell_count;
        nd.neighbor_count = src->neighbor_count;

        nd.next_slot = map.lookup(src->next, &found);
        if (!found) return NAV_ERR_UNMAPPED;
        // `prev` is deliberately not carried -- see the header. It is zeroed at import.

        // Only the first `neighbor_count` entries are load-bearing -- that is the boundary
        // llm_map_load_regions itself draws (it copies exactly that many and leaves the rest as raw
        // malloc residue). Translating beyond it would turn uninitialised bytes into spurious
        // refusals, so the tail is NO_SLOT by construction.
        //
        // AND ONLY FOR ACTIVE NODES. A node on the FREE list keeps whatever `neighbor_count` and
        // `neighbors[]` it had when llm_map_region_free @0x0042239a pushed it -- neither is cleared
        // on free, and map_block_8_GetNextBlock @0x004222cf resets `neighbor_count` to 0 when it pops
        // the node for reuse. So those pointers are provably unreadable, and translating them would
        // let a stale pointer to some earlier pool generation turn a perfectly good capture into a
        // refusal. Their slots are NO_SLOT; `neighbor_count` is still carried verbatim.
        const bool     is_active = s < active_count;
        const uint32_t nn        = src->neighbor_count > 128u ? 128u : src->neighbor_count;
        for (uint32_t i = 0; i < 128u; ++i) nd.neighbors[i] = NO_SLOT;
        if (is_active) {
            for (uint32_t i = 0; i < nn; ++i) {
                nd.neighbors[i] = map.lookup(src->neighbors[i], &found);
                if (!found) return NAV_ERR_UNMAPPED; // incomplete pool walk -- never a silent NO_SLOT
            }
        }
        // neighbor_data is plain payload (no pointers), so it is carried whole: no translation means
        // no refusal risk, and carrying it all is strictly closer to the live record.
        for (uint32_t i = 0; i < 128u; ++i) nd.neighbor_data[i] = src->neighbor_data[i];

        std::memcpy(body + at, &nd, sizeof(nd));
        at += sizeof(nd);
    }

    // (3) BY_INDEX.
    llm_map_region **bi = own.region_by_index();
    for (uint32_t i = 0; i < BY_INDEX_N; ++i) {
        const uint16_t v = map.lookup(bi[i], &found);
        if (!found) return NAV_ERR_UNMAPPED;
        std::memcpy(body + at, &v, 2);
        at += 2;
    }

    // (4) THE GRID -- stride 8, offset 0 (see the static_asserts at the top of this file).
    for (int32_t x = 0; x < mh::sim::MAP_GRID_DIM; ++x) {
        for (int32_t y = 0; y < mh::sim::MAP_GRID_DIM; ++y) {
            const llm_map_region *r = own.region_cell_at(x, y).region;
            uint16_t              v;
            if (reinterpret_cast<uintptr_t>(r) == 0xffffffffu) {
                v = GRID_PENDING; // build_regions' flood-fill-pending sentinel
            } else {
                v = map.lookup(r, &found);
                if (!found) return NAV_ERR_UNMAPPED;
            }
            std::memcpy(body + at, &v, 2);
            at += 2;
        }
    }

    h.checksum = fnv1a32(body, at);
    std::memcpy(out, &h, sizeof(h));
    *out_len = sizeof(trailer_header) + at;
    return NAV_OK;
}

// ---- import ---------------------------------------------------------------------------------------

int import(const void *buf, size_t n, const mh::sim::lt_map_region_pool_calls &c) {
    if (buf == nullptr) return NAV_ERR_ARG;
    if (n < sizeof(trailer_header)) return NAV_ERR_TRUNCATED;

    trailer_header h;
    std::memcpy(&h, buf, sizeof(h));
    if (h.magic != TRAILER_MAGIC || h.version != TRAILER_VERSION) return NAV_ERR_MAGIC;
    if (h.by_index_count != BY_INDEX_N || h.grid_cells != GRID_CELLS) return NAV_ERR_MAGIC;
    if (h.node_count > MAX_NODES) return NAV_ERR_OVERFLOW;
    if (h.active_count + h.free_count != h.node_count) return NAV_ERR_INCONSISTENT;
    if (n < sizeof(trailer_header) + body_bytes(h.node_count)) return NAV_ERR_TRUNCATED;

    const uint8_t *body = static_cast<const uint8_t *>(buf) + sizeof(trailer_header);
    const size_t   blen = body_bytes(h.node_count);
    if (fnv1a32(body, blen) != h.checksum) return NAV_ERR_CHECKSUM;

    // Validate every slot BEFORE a single byte is written, so a rejected trailer cannot leave a
    // half-built pool behind -- the same discipline blob::import() applies to the block table.
    auto                slot_ok = [&h](uint16_t s) { return s == NO_SLOT || s < h.node_count; };
    const trailer_node *nodes   = reinterpret_cast<const trailer_node *>(body);
    for (uint32_t s = 0; s < h.node_count; ++s) {
        const trailer_node &nd = nodes[s];
        if (!slot_ok(nd.next_slot)) return NAV_ERR_BAD_SLOT;
        if (nd.index >= BY_INDEX_N) return NAV_ERR_BAD_SLOT;
        const uint32_t nn = nd.neighbor_count > 128u ? 128u : nd.neighbor_count;
        for (uint32_t i = 0; i < nn; ++i)
            if (!slot_ok(nd.neighbors[i])) return NAV_ERR_BAD_SLOT;
    }
    if (!slot_ok(h.active_head) || !slot_ok(h.free_head)) return NAV_ERR_BAD_SLOT;
    const uint8_t *bi_src   = body + static_cast<size_t>(h.node_count) * sizeof(trailer_node);
    const uint8_t *grid_src = bi_src + BY_INDEX_N * 2u;
    for (uint32_t i = 0; i < BY_INDEX_N; ++i) {
        uint16_t v;
        std::memcpy(&v, bi_src + i * 2u, 2);
        if (!slot_ok(v)) return NAV_ERR_BAD_SLOT;
    }
    for (uint32_t i = 0; i < GRID_CELLS; ++i) {
        uint16_t v;
        std::memcpy(&v, grid_src + i * 2u, 2);
        if (v != GRID_PENDING && !slot_ok(v)) return NAV_ERR_BAD_SLOT;
    }

    mh::sim::sim_state  st  = mh::sim::state();
    mh::sim::sim_store &own = st.own;
    llm_map_region     *by_slot[MAX_NODES];
    for (uint32_t s = 0; s < h.node_count; ++s) {
        by_slot[s] = static_cast<llm_map_region *>(c.malloc_(sizeof(llm_map_region)));
        if (by_slot[s] == nullptr) {
            for (uint32_t k = 0; k < s; ++k) c.free_(by_slot[k]);
            return NAV_ERR_ALLOC;
        }
    }
    auto ptr_of = [&](uint16_t s) -> llm_map_region * { return s == NO_SLOT ? nullptr : by_slot[s]; };

    for (uint32_t s = 0; s < h.node_count; ++s) {
        const trailer_node &nd  = nodes[s];
        llm_map_region     *dst = by_slot[s];
        std::memset(dst, 0, sizeof(*dst));
        dst->index          = nd.index;
        dst->x              = nd.x;
        dst->y              = nd.y;
        dst->cell_count     = nd.cell_count;
        dst->neighbor_count = nd.neighbor_count;
        dst->next           = ptr_of(nd.next_slot);
        // dst->prev stays null (the memset above): dead field, zeroed exactly as
        // llm_map_load_regions @0x00424a68 zeroes it. See the header.
        const uint32_t nn = nd.neighbor_count > 128u ? 128u : nd.neighbor_count;
        for (uint32_t i = 0; i < nn; ++i) dst->neighbors[i] = ptr_of(nd.neighbors[i]);
        for (uint32_t i = 0; i < 128u; ++i) dst->neighbor_data[i] = nd.neighbor_data[i];
        // route_bfs_dist / route_parent / route_mark stay zero: llm_map_load_regions zeroes the first
        // and third and leaves the second as malloc residue, which is the same statement about all
        // three -- they are per-call BFS scratch, reset by the route search before it reads them.
    }

    own.region_list_head_mut()  = ptr_of(h.active_head);
    own.region_pool_free_head() = ptr_of(h.free_head);

    // The relinked lists must reproduce the carried counts. This is the arm against a trailer whose
    // per-node links and whose header disagree -- without it a truncated chain would import quietly
    // and the pool would simply be smaller than it says.
    uint32_t walked = 0;
    for (const llm_map_region *p = own.region_list_head_mut(); p != nullptr; p = p->next) {
        if (++walked > h.node_count) return NAV_ERR_CYCLE;
    }
    if (walked != h.active_count) return NAV_ERR_INCONSISTENT;
    walked = 0;
    for (const llm_map_region *p = own.region_pool_free_head(); p != nullptr; p = p->next) {
        if (++walked > h.node_count) return NAV_ERR_CYCLE;
    }
    if (walked != h.free_count) return NAV_ERR_INCONSISTENT;

    llm_map_region **bi = own.region_by_index();
    for (uint32_t i = 0; i < BY_INDEX_N; ++i) {
        uint16_t v;
        std::memcpy(&v, bi_src + i * 2u, 2);
        bi[i] = ptr_of(v);
    }

    // THE GRID, PATCHED IN PLACE. Only the pointer dword is written: `terrain_flags` was delivered
    // correctly by the carried MAP_REGION_GRID block and must survive untouched, which is the whole
    // reason this is a field write and not a cell write.
    uint32_t grid_set = 0;
    for (int32_t x = 0; x < mh::sim::MAP_GRID_DIM; ++x) {
        for (int32_t y = 0; y < mh::sim::MAP_GRID_DIM; ++y) {
            uint16_t v;
            std::memcpy(&v, grid_src + (static_cast<size_t>(x) * mh::sim::MAP_GRID_DIM + y) * 2u, 2);
            llm_map_region *r;
            if (v == GRID_PENDING) r = reinterpret_cast<llm_map_region *>(static_cast<uintptr_t>(0xffffffffu));
            else r = ptr_of(v);
            own.region_cell_at(x, y).region = r;
            if (v != GRID_PENDING && v != NO_SLOT) ++grid_set;
        }
    }

    g_last.nodes    = h.node_count;
    g_last.active   = h.active_count;
    g_last.free     = h.free_count;
    g_last.grid_set = grid_set;
    return NAV_OK;
}

// THE GAME'S ALLOCATOR, not new/malloc: llm_map_region_pool_reset calls utils_free on every node it
// drains, and a cross-allocator free is a crash a long way from its cause. This is the same
// `utils_malloc(0x420)` llm_map_load_regions @0x00424a68 itself performs.
int import(const void *buf, size_t n) {
    return import(buf, n, mh::sim::live_lt_map_region_pool_calls());
}

const import_stats &last_import() {
    return g_last;
}

} // namespace mh::state::nav
