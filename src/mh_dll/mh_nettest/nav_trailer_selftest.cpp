//
// mh_nettest/nav_trailer_selftest.cpp -- `net_selftest navtest`, the offline oracle for the world
// blob's FORMAT 2 NAV TRAILER (tracker LIB-REF).
//
// WHAT THIS IS FOR. The nav trailer carries the map-region decomposition as slot indices so an
// importing host can rebuild the pointer graph instead of rebuilding the PARTITION from the
// `passable` plane -- a rebuild that is measurably not the recording's (2181 of 65536 tiles differ;
// state/nav_trailer.h has the reasoning). The end-to-end proof of that is a 5000-step standalone
// replay against a re-recorded fixture, which costs rig time and a re-record. This suite is the part
// that does not: it builds a SYNTHETIC pool with a known shape, round-trips it through
// capture/import, and asserts the reconstruction is SLOT-EXACT rather than merely equivalent.
//
// WHY SLOT-EXACT AND NOT "an equivalent partition". The free list's ORDER is load-bearing:
// map_block_8_GetNextBlock @0x004222cf POPS the free head, so when the replay places a building and
// region_split asks for a node, WHICH node it gets -- and therefore which `index` the new region
// carries -- is decided by that order. A round-trip that preserved the partition but reordered the
// free list would pass a weaker test and diverge in the replay hundreds of steps later. So the arms
// below compare both list ORDERS, not just membership.
//
// THE MUTATION ARMS ARE THE POINT of the second half. Every refusal this format has is exercised by
// actually corrupting a trailer and watching it be refused -- a guard never seen red is not a guard.
//
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../libmh/include/libmh.h"

#include "addr/mh_regions.gen.h"
#include "sim/libtrans/sim_lt_map_region_pool.h"
#include "sim/sim_state.h"
#include "state/nav_trailer.h"
#include "state/region_runtime.h"

namespace {

using mh::sim::llm_map_region;
namespace nav = mh::state::nav;

int g_checks = 0, g_fails = 0;

// net_selftest.exe compiles the module sources WITHOUT MH_LIBMH_BUILD and has no game image mapped,
// so live_lt_map_region_pool_calls() would resolve utils_malloc to a fixed VA that is not mapped
// here. The suite supplies its own pair and hands it to nav::import, which is also what makes the
// pool it builds legal to free.
void                                    *t_malloc(uint32_t n) { return std::malloc(n); }
void                                     t_free(void *p) { std::free(p); }
const mh::sim::lt_map_region_pool_calls &test_calls() {
    static const mh::sim::lt_map_region_pool_calls c = {t_malloc, t_free};
    return c;
}

void ck(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::printf("  FAIL: %s\n", what);
    }
}

// The same whole-registry arena libref_host binds, for the same reason: every region in its OWN
// allocation, so an over-index reads a neighbour rather than more of itself.
struct arena {
    uint8_t *mem = nullptr;
    size_t   len = 0;

    bool bind_all() {
        const int count = static_cast<int>(libmh_region_count());
        for (int i = 0; i < count; ++i)
            len += (mh::state::reach_of(static_cast<mh::state::region_id>(i)) + 15u) & ~15u;
        mem = static_cast<uint8_t *>(std::malloc(len));
        if (mem == nullptr) return false;
        std::memset(mem, 0, len);
        libmh_region_bind *b =
            static_cast<libmh_region_bind *>(std::malloc(sizeof(libmh_region_bind) * count));
        if (b == nullptr) return false;
        size_t off = 0;
        for (int i = 0; i < count; ++i) {
            const uint32_t sz = mh::state::reach_of(static_cast<mh::state::region_id>(i));
            b[i].region_id    = static_cast<uint32_t>(i);
            b[i].base         = sz ? (mem + off) : nullptr;
            b[i].size         = sz;
            b[i].count        = 0;
            off += (sz + 15u) & ~15u;
        }
        const int rc = libmh_bind_regions(b, static_cast<size_t>(count));
        std::free(b);
        return rc == count;
    }
    ~arena() { std::free(mem); }
};

// ---- the synthetic pool --------------------------------------------------------------------------
//
// Six nodes: FOUR on the active list and TWO on the free list, with the two orders deliberately
// DIFFERENT from each other and from index order, so a round-trip that quietly normalised either
// would show up. Indices are non-contiguous for the same reason -- a reconstruction that rebuilt
// BY_INDEX by position instead of by the carried index would pass a contiguous test.
constexpr uint16_t ACTIVE_IDX[4] = {7, 3, 19, 11};
constexpr uint16_t FREE_IDX[2]   = {31, 23};

// A handful of tiles per region plus one PENDING and one left null, at scattered coordinates so a
// stride bug cannot line up. `terrain_flags` gets its own pattern, because the single most important
// property of the grid import is that it writes the POINTER DWORD ONLY and leaves the flags alone.
uint32_t flags_pattern(int x, int y) {
    return static_cast<uint32_t>((x * 7919 + y * 104729 + 13) & 0x7fffffff);
}

struct built {
    llm_map_region *active[4];
    llm_map_region *freen[2];
};

void build_pool(built *out) {
    mh::sim::sim_state                       st  = mh::sim::state();
    mh::sim::sim_store                      &own = st.own;
    const mh::sim::lt_map_region_pool_calls &c   = test_calls();

    for (int i = 0; i < 4; ++i) {
        llm_map_region *n = static_cast<llm_map_region *>(c.malloc_(sizeof(llm_map_region)));
        std::memset(n, 0, sizeof(*n));
        n->index       = ACTIVE_IDX[i];
        n->x           = static_cast<uint8_t>(10 + i * 3);
        n->y           = static_cast<uint8_t>(200 - i * 5);
        n->cell_count  = static_cast<uint32_t>(100 + i);
        out->active[i] = n;
    }
    for (int i = 0; i < 2; ++i) {
        llm_map_region *n = static_cast<llm_map_region *>(c.malloc_(sizeof(llm_map_region)));
        std::memset(n, 0, sizeof(*n));
        n->index      = FREE_IDX[i];
        out->freen[i] = n;
        // A free node keeps whatever neighbour state it had when it was pushed -- neither
        // llm_map_region_free nor the push clears it. Seed that here (a non-zero neighbor_count whose
        // pointers we never translate) so the capture's "free nodes are not translated" rule is
        // exercised rather than assumed: with a stale pointer that is NOT in the pool, a capture that
        // translated it would refuse, and this arm would go red.
        n->neighbor_count = 2;
        n->neighbors[0]   = reinterpret_cast<llm_map_region *>(static_cast<uintptr_t>(0xdeadbeefu));
        n->neighbors[1]   = out->active[0];
    }

    // Neighbour graph among the active four: a ring plus one chord, so neighbor_count varies.
    out->active[0]->neighbor_count = 2;
    out->active[0]->neighbors[0]   = out->active[1];
    out->active[0]->neighbors[1]   = out->active[3];
    out->active[1]->neighbor_count = 3;
    out->active[1]->neighbors[0]   = out->active[0];
    out->active[1]->neighbors[1]   = out->active[2];
    out->active[1]->neighbors[2]   = out->active[3];
    out->active[2]->neighbor_count = 1;
    out->active[2]->neighbors[0]   = out->active[1];
    out->active[3]->neighbor_count = 2;
    out->active[3]->neighbors[0]   = out->active[0];
    out->active[3]->neighbors[1]   = out->active[1];
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 128; ++k) out->active[i]->neighbor_data[k] = static_cast<uint32_t>(i * 1000 + k);

    // The lists. Active order is 0,1,2,3 as built; free order is 0,1 -- both distinct from index
    // order (ACTIVE_IDX is 7,3,19,11 and FREE_IDX is 31,23).
    for (int i = 0; i < 3; ++i) out->active[i]->next = out->active[i + 1];
    out->active[3]->next        = nullptr;
    out->freen[0]->next         = out->freen[1];
    out->freen[1]->next         = nullptr;
    own.region_list_head_mut()  = out->active[0];
    own.region_pool_free_head() = out->freen[0];

    for (int i = 0; i < 4; ++i) own.region_by_index()[ACTIVE_IDX[i]] = out->active[i];

    // The grid: flags everywhere, regions on a scattered handful, one PENDING, the rest null.
    for (int x = 0; x < mh::sim::MAP_GRID_DIM; ++x)
        for (int y = 0; y < mh::sim::MAP_GRID_DIM; ++y) {
            own.region_cell_at(x, y).region        = nullptr;
            own.region_cell_at(x, y).terrain_flags = flags_pattern(x, y);
        }
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 5; ++k)
            own.region_cell_at(i * 37 + k, 251 - i * 11).region = out->active[i];
    own.region_cell_at(128, 128).region =
        reinterpret_cast<llm_map_region *>(static_cast<uintptr_t>(0xffffffffu));
}

void wipe_pool(const built &b) {
    mh::sim::sim_state                       st  = mh::sim::state();
    mh::sim::sim_store                      &own = st.own;
    const mh::sim::lt_map_region_pool_calls &c   = test_calls();
    for (int i = 0; i < 4; ++i) c.free_(b.active[i]);
    for (int i = 0; i < 2; ++i) c.free_(b.freen[i]);
    own.region_list_head_mut()  = nullptr;
    own.region_pool_free_head() = nullptr;
    for (uint32_t i = 0; i < nav::BY_INDEX_N; ++i) own.region_by_index()[i] = nullptr;
    for (int x = 0; x < mh::sim::MAP_GRID_DIM; ++x)
        for (int y = 0; y < mh::sim::MAP_GRID_DIM; ++y) own.region_cell_at(x, y).region = nullptr;
    // terrain_flags is deliberately NOT wiped: the import must leave it alone, and an arm that
    // cleared it first could not tell "preserved" from "rewritten identically".
}

} // namespace

int run_navtest() {
    std::printf("=== navtest (LIB-REF: the world blob's FORMAT 2 nav trailer, round-tripped "
                "SLOT-EXACT and refused on every corruption) ===\n");
    arena a;
    if (!a.bind_all()) {
        std::printf("  FAIL: could not bind the registry\n");
        return 1;
    }

    built b;
    build_pool(&b);

    uint8_t  *buf = static_cast<uint8_t *>(std::malloc(nav::MAX_TRAILER_BYTES));
    size_t    len = 0;
    const int crc = nav::capture(buf, nav::MAX_TRAILER_BYTES, &len);
    ck(crc == nav::NAV_OK, "capture serializes the synthetic pool");
    if (crc != nav::NAV_OK) {
        std::printf("        capture rc=%d\n", crc);
        std::free(buf);
        return 1;
    }

    nav::trailer_header th;
    std::memcpy(&th, buf, sizeof(th));
    ck(th.magic == nav::TRAILER_MAGIC, "the trailer carries MHNV");
    ck(th.node_count == 6u, "six nodes: four active + two free");
    ck(th.active_count == 4u && th.free_count == 2u, "the two list lengths are carried");
    ck(th.active_head == 0u, "slot 0 is the active head (traversal order defines the slot space)");
    ck(th.free_head == 4u, "the free list starts at slot 4, right after the active nodes");

    wipe_pool(b);
    ck(mh::sim::state().own.region_list_head_mut() == nullptr, "the wipe really emptied the pool");

    const int irc = nav::import(buf, len, test_calls());
    ck(irc == nav::NAV_OK, "import rebuilds the pool");
    if (irc != nav::NAV_OK) std::printf("        import rc=%d\n", irc);

    // ---- SLOT-EXACT reconstruction --------------------------------------------------------------
    mh::sim::sim_state  st2 = mh::sim::state();
    mh::sim::sim_store &own = st2.own;

    int  seen     = 0;
    bool order_ok = true;
    for (llm_map_region *p = own.region_list_head_mut(); p != nullptr; p = p->next) {
        if (seen >= 4 || p->index != ACTIVE_IDX[seen]) {
            order_ok = false;
            break;
        }
        ++seen;
    }
    ck(order_ok && seen == 4, "the active list comes back in its ORIGINAL order, by index");

    seen     = 0;
    order_ok = true;
    for (llm_map_region *p = own.region_pool_free_head(); p != nullptr; p = p->next) {
        if (seen >= 2 || p->index != FREE_IDX[seen]) {
            order_ok = false;
            break;
        }
        ++seen;
    }
    ck(order_ok && seen == 2, "the FREE list comes back in its original order (get_next_block pops it)");

    bool bi_ok = true;
    for (uint32_t i = 0; i < nav::BY_INDEX_N; ++i) {
        bool want_set = false;
        for (int k = 0; k < 4; ++k)
            if (ACTIVE_IDX[k] == i) want_set = true;
        const llm_map_region *g = own.region_by_index()[i];
        if (want_set ? (g == nullptr || g->index != i) : (g != nullptr)) {
            bi_ok = false;
            break;
        }
    }
    ck(bi_ok, "BY_INDEX is refilled at the carried indices and nowhere else");

    bool fields_ok = true;
    for (llm_map_region *p = own.region_list_head_mut(); p != nullptr; p = p->next) {
        int k = -1;
        for (int i = 0; i < 4; ++i)
            if (ACTIVE_IDX[i] == p->index) k = i;
        if (k < 0) {
            fields_ok = false;
            break;
        }
        if (p->x != static_cast<uint8_t>(10 + k * 3) || p->y != static_cast<uint8_t>(200 - k * 5) ||
            p->cell_count != static_cast<uint32_t>(100 + k)) {
            fields_ok = false;
            break;
        }
        if (p->neighbor_data[7] != static_cast<uint32_t>(k * 1000 + 7)) {
            fields_ok = false;
            break;
        }
        if (p->route_bfs_dist != 0 || p->route_parent != nullptr || p->route_mark != 0 ||
            p->prev != nullptr) {
            fields_ok = false;
            break;
        }
    }
    ck(fields_ok, "every carried field round-trips, and the four scratch fields come back zeroed");

    // The neighbour graph, re-resolved to POINTERS -- the half an index-only format has to get right.
    llm_map_region *a1 = own.region_by_index()[ACTIVE_IDX[1]];
    ck(a1 != nullptr && a1->neighbor_count == 3u && a1->neighbors[0] == own.region_by_index()[ACTIVE_IDX[0]] &&
           a1->neighbors[1] == own.region_by_index()[ACTIVE_IDX[2]] &&
           a1->neighbors[2] == own.region_by_index()[ACTIVE_IDX[3]],
       "neighbour slots are re-resolved to the right node POINTERS");

    // ---- the grid, and the flags it must not touch ------------------------------------------------
    bool grid_ok = true, flags_ok = true;
    for (int x = 0; x < mh::sim::MAP_GRID_DIM && grid_ok && flags_ok; ++x)
        for (int y = 0; y < mh::sim::MAP_GRID_DIM; ++y) {
            const llm_map_region *want = nullptr;
            for (int i = 0; i < 4; ++i)
                for (int k = 0; k < 5; ++k)
                    if (x == i * 37 + k && y == 251 - i * 11) want = own.region_by_index()[ACTIVE_IDX[i]];
            const llm_map_region *got = own.region_cell_at(x, y).region;
            if (x == 128 && y == 128) {
                if (reinterpret_cast<uintptr_t>(got) != 0xffffffffu) {
                    grid_ok = false;
                    break;
                }
            } else if (got != want) {
                grid_ok = false;
                break;
            }
            if (own.region_cell_at(x, y).terrain_flags != flags_pattern(x, y)) {
                flags_ok = false;
                break;
            }
        }
    ck(grid_ok, "every grid cell points at the region it pointed at, and PENDING survives as PENDING");
    ck(flags_ok, "terrain_flags is UNTOUCHED -- the import writes the pointer dword only");

    // ---- the refusals, each one actually made to fire ---------------------------------------------
    {
        uint8_t *m = static_cast<uint8_t *>(std::malloc(len));

        std::memcpy(m, buf, len);
        m[0] ^= 0xffu;
        ck(nav::import(m, len, test_calls()) == nav::NAV_ERR_MAGIC, "a wrong trailer magic is refused");

        std::memcpy(m, buf, len);
        m[4] = 99u; // version
        ck(nav::import(m, len, test_calls()) == nav::NAV_ERR_MAGIC, "an unknown trailer version is refused");

        std::memcpy(m, buf, len);
        m[sizeof(nav::trailer_header) + 3u] ^= 0x01u; // one bit, deep in the first node
        ck(nav::import(m, len, test_calls()) == nav::NAV_ERR_CHECKSUM, "a ONE-BIT body corruption is refused");

        std::memcpy(m, buf, len);
        ck(nav::import(m, len - 1u, test_calls()) == nav::NAV_ERR_TRUNCATED, "a truncated trailer is refused");

        // A slot pointing past the pool. Patch node 0's next_slot and repair the checksum, so the
        // arm proves the RANGE check fires rather than re-proving the checksum.
        std::memcpy(m, buf, len);
        nav::trailer_header hh;
        std::memcpy(&hh, m, sizeof(hh));
        const size_t   nslot = sizeof(nav::trailer_header) + offsetof(nav::trailer_node, next_slot);
        const uint16_t bad   = 4000u;
        std::memcpy(m + nslot, &bad, 2);
        uint32_t h2 = 2166136261u;
        for (size_t i = sizeof(nav::trailer_header); i < len; ++i) {
            h2 ^= m[i];
            h2 *= 16777619u;
        }
        hh.checksum = h2;
        std::memcpy(m, &hh, sizeof(hh));
        ck(nav::import(m, len, test_calls()) == nav::NAV_ERR_BAD_SLOT, "a slot past the end of the pool is refused");

        std::free(m);
    }

    std::free(buf);
    std::printf("=== navtest: %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
