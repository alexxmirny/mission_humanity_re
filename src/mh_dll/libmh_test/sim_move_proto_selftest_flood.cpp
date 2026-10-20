//
// sim_move_proto_selftest_flood.cpp -- `simtest` cases for [net] route_flood_exact (mods:M1H, the bucket-queue region flood).
//
// The key replaces phase 2 of llm_map_region_flood_reachable (the FIFO label-correcting relaxation over a 4096-slot ring) by Dial's
// bucket queue, sim/move/flood.h, shared by the mh.dll splice and the libmh twin (region_flood_reachable). This suite pins:
//
//   F1  no lap          on random maps whose FIFO cannot lap its ring (<= 1024 cells), the exact field equals the FIFO field cell for
//                       cell (retail cost table and a random table with zero weights), and a second run on the same scratch is identical
//                       (the scratch drains).
//   F2  lap             on a 256x256 map where the retail FIFO DOES lap (pending reaches the ring size), the exact field is the true
//                       shortest-path field (a reference Dijkstra over the same chained neighbours), and differs from the lapped FIFO.
//   F3  overflow clamp  a snake corridor with weight 4096 per edge exhausts the 24-bit distance: the relaxations that would pass 0xffffff
//                       are dropped (the cell stays unreached), the overflow flag is set, everything before the limit is exact.
//   F4  libmh twin      region_flood_reachable with the key on: same field as the reference on a no-lap map and on the lap map; key
//                       off keeps the retail FIFO field; the hit counter counts; the query answer (1 = reachable) is the same.
//
#include "sim/move/flood.h"
#include "sim/move/planner_hits.h"
#include "sim/move/route_cost.h"
#include "sim/sim_map_region_routing.h"
#include "sim/sim_move_fixes.h"

#include <algorithm>
#include <cstdint>
#include <queue>
#include <utility>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

namespace mv = mh::sim::move;

struct fixes_scope {
    move_fixes saved;
    fixes_scope() : saved(move_fix()) {}
    ~fixes_scope() { set_move_fixes(saved); }
};

constexpr uint32_t N = mv::FLOOD_FLAT_CELLS;

struct flat_cells {
    std::vector<uint32_t> f = std::vector<uint32_t>(N, 0);
    uint32_t             &flags(uint32_t flat) { return f[flat]; }
};

uint32_t g_seed = 1;
uint32_t rnd() {
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

// Phase 1 as the original leaves it: eligible = 0xffffff00 | low, not eligible = low (high 24 bits 0).
void init_cells(flat_cells &c, uint32_t dim, const std::vector<uint8_t> &low, const std::vector<uint8_t> &elig) {
    std::fill(c.f.begin(), c.f.end(), 0u);
    for (uint32_t x = 0; x < dim; ++x)
        for (uint32_t y = 0; y < dim; ++y) {
            const uint32_t i = (x << 8) | y;
            c.f[i]           = low[i] | (elig[i] ? 0xFFFFFF00u : 0u);
        }
}

// The retail phase 2, transcribed over the flat array: FIFO ring of 4096, no guard. `lapped` = the pending count ever reached the ring.
void fifo_ref(flat_cells &c, uint32_t seed, uint32_t mask, const uint32_t cost[256], bool &lapped, uint32_t &deq) {
    std::vector<uint32_t> ring(4096, 0);
    uint32_t              rd = 0, wr = 1;
    lapped    = false;
    deq       = 0;
    c.f[seed] = (c.f[seed] & 0xFFu) | 0x100u;
    ring[0]   = seed;
    while (rd != wr) {
        if (((wr - rd) & 0xfffu) >= 0xff8u) lapped = true;
        const uint32_t cur = ring[rd];
        ++deq;
        const uint32_t raw  = c.f[cur];
        const uint32_t prop = (raw & 0xFFFFFF00u) + (cost[raw & 0xFFu] << 8);
        uint32_t       nb[4];
        mv::flood_neighbors(cur, mask, nb);
        for (int k = 0; k < 4; ++k) {
            const uint32_t nraw = c.f[nb[k]];
            const uint32_t nh   = nraw & 0xFFFFFF00u;
            if (nh == 0 || prop >= nh) continue;
            ring[wr]   = nb[k];
            wr         = (wr + 1) & 0xfffu;
            c.f[nb[k]] = (prop & 0xFFFFFF00u) | (nraw & 0xFFu);
        }
        rd = (rd + 1) & 0xfffu;
    }
}

// Reference Dijkstra over the same chained neighbours.
void dijkstra_ref(flat_cells &c, uint32_t seed, uint32_t mask, const uint32_t cost[256]) {
    using item = std::pair<uint32_t, uint32_t>; // (distance, cell)
    std::priority_queue<item, std::vector<item>, std::greater<item>> q;
    c.f[seed] = (c.f[seed] & 0xFFu) | 0x100u;
    q.push({1u, seed});
    while (!q.empty()) {
        const auto [d, cur] = q.top();
        q.pop();
        if ((c.f[cur] >> 8) != d) continue; // stale
        const uint32_t nd = d + cost[c.f[cur] & 0xFFu];
        uint32_t       nb[4];
        mv::flood_neighbors(cur, mask, nb);
        for (int k = 0; k < 4; ++k) {
            const uint32_t nraw = c.f[nb[k]];
            const uint32_t nbd  = nraw >> 8;
            if (nbd == 0 || nd >= nbd) continue;
            c.f[nb[k]] = (nd << 8) | (nraw & 0xFFu);
            q.push({nd, nb[k]});
        }
    }
}

bool same_field(const flat_cells &a, const flat_cells &b) { return a.f == b.f; }

void random_map(uint32_t dim, int elig_pct, std::vector<uint8_t> &low, std::vector<uint8_t> &elig, uint32_t max_low) {
    low.assign(N, 0);
    elig.assign(N, 0);
    for (uint32_t x = 0; x < dim; ++x)
        for (uint32_t y = 0; y < dim; ++y) {
            const uint32_t i = (x << 8) | y;
            elig[i]          = (rnd() % 100u) < static_cast<uint32_t>(elig_pct) ? 1 : 0;
            low[i]           = static_cast<uint8_t>(rnd() % (max_low + 1u));
        }
}

void raw_cost(uint32_t cost[256]) {
    for (uint32_t i = 0; i < 256; ++i) cost[i] = i;
}

void f1_no_lap() {
    uint32_t cost_retail[256];
    raw_cost(cost_retail);
    int      same = 0, total = 0, lapped_any = 0;
    uint32_t settled_sum = 0, deq_sum = 0;
    for (int it = 0; it < 120; ++it) {
        const uint32_t       dim  = (it & 1) ? 32u : 16u;
        const uint32_t       mask = ((dim - 1) << 8) | (dim - 1);
        std::vector<uint8_t> low, elig;
        random_map(dim, 70 + (it % 4) * 8, low, elig, (it % 3 == 0) ? 29u : 255u);
        uint32_t cost[256];
        if (it % 5 == 0) {
            // a random table with zero-weight edges
            for (auto &w : cost) w = rnd() % 40u;
        } else {
            raw_cost(cost);
        }
        const uint32_t seed = ((rnd() % dim) << 8) | (rnd() % dim);
        flat_cells     a, b;
        init_cells(a, dim, low, elig);
        init_cells(b, dim, low, elig);
        bool     lapped;
        uint32_t deq;
        fifo_ref(a, seed, mask, cost, lapped, deq);
        const mv::flood_exact_result r = mv::flood_exact(b, seed, mask, cost);
        ++total;
        if (lapped) ++lapped_any;
        if (same_field(a, b)) ++same;
        settled_sum += r.settled;
        deq_sum += deq;
        // the scratch drains: a second run from the same state gives the same field
        flat_cells b2;
        init_cells(b2, dim, low, elig);
        mv::flood_exact(b2, seed, mask, cost);
        if (!same_field(b, b2)) ck(false, "F1: a second run on the same scratch differs");
    }
    ck_eq(static_cast<uint32_t>(lapped_any), 0u, "F1: none of the small maps lapped the 4096 ring (so the FIFO is the exact field)");
    ck_eq(static_cast<uint32_t>(same), static_cast<uint32_t>(total), "F1: the exact field equals the FIFO field cell for cell on every random no-lap map");
    ck(settled_sum <= deq_sum, "F1: the exact flood settles no more cells than the FIFO dequeues");
    (void)cost_retail;
}

void f2_lap() {
    g_seed = 12345;
    std::vector<uint8_t> low(N, 0), elig(N, 0);
    for (uint32_t i = 0; i < N; ++i) {
        low[i]  = static_cast<uint8_t>(1 + (rnd() % 60u));
        elig[i] = 1;
    }
    uint32_t cost[256];
    raw_cost(cost);
    const uint32_t seed = (128u << 8) | 128u;
    flat_cells     fifo, exact, ref;
    init_cells(fifo, 256, low, elig);
    init_cells(exact, 256, low, elig);
    init_cells(ref, 256, low, elig);
    bool     lapped;
    uint32_t deq;
    fifo_ref(fifo, seed, 0xffffu, cost, lapped, deq);
    const mv::flood_exact_result r = mv::flood_exact(exact, seed, 0xffffu, cost);
    dijkstra_ref(ref, seed, 0xffffu, cost);
    ck(lapped, "F2: the retail FIFO reaches the ring limit on this 256x256 map (the test is meaningful)");
    ck(same_field(exact, ref), "F2: the exact field is the true shortest-path field (reference Dijkstra), cell for cell");
    ck(!same_field(fifo, exact), "F2: ... and it differs from the lapped FIFO field");
    ck_eq(r.settled, 65536u, "F2: every cell of the open map is settled exactly once (65536)");
    ck(!r.overflow && !r.clamped, "F2: no overflow, no clamp");
    if (trace_on()) printf("  F2: fifo dequeues=%u exact settled=%u buckets=%u\n", deq, r.settled, r.buckets);
}

void f3_overflow() {
    // A snake corridor (even rows open, odd rows a single connector alternating ends) with weight 4096 per edge: after 4095 steps the
    // 24-bit distance is exhausted.
    std::vector<uint8_t>  low(N, 0), elig(N, 0);
    std::vector<uint32_t> order; // cells in path order
    for (uint32_t y = 0; y < 256; y += 2) {
        // columns 1..254 only, so the 16-bit wrap mask gives no shortcut from one end of a row to the other
        for (uint32_t k = 1; k <= 254; ++k) {
            const uint32_t x = (((y / 2) & 1) == 0) ? k : 255 - k;
            order.push_back((x << 8) | y);
        }
        if (y + 1 < 255) order.push_back((((((y / 2) & 1) == 0) ? 254u : 1u) << 8) | (y + 1));
    }
    for (uint32_t c : order) {
        elig[c] = 1;
        low[c]  = 1;
    }
    uint32_t cost[256];
    for (auto &w : cost) w = 4096;
    flat_cells f;
    init_cells(f, 256, low, elig);
    const mv::flood_exact_result r = mv::flood_exact(f, order[0], 0xffffu, cost);
    ck(r.overflow, "F3: the 24-bit distance overflow is reported");
    bool exact_before = true, unreached_after = true;
    for (size_t k = 0; k < order.size(); ++k) {
        const uint64_t want = 1ull + 4096ull * k;
        const uint32_t got  = f.f[order[k]] >> 8;
        if (want < 0xffffffull) {
            if (got != want) exact_before = false;
        } else if (got != 0xffffffu) {
            unreached_after = false;
        }
    }
    ck(exact_before, "F3: every cell before the limit carries its exact distance (1 + 4096*k)");
    ck(unreached_after, "F3: every cell the distance could not reach stays unreached (0xffffff), no wrap-around");
    // an over-large weight is clamped, flagged
    uint32_t big[256];
    for (auto &w : big) w = 100000;
    flat_cells g;
    init_cells(g, 256, low, elig);
    const mv::flood_exact_result r2 = mv::flood_exact(g, order[0], 0xffffu, big);
    ck(r2.clamped, "F3: a weight above the clamp is reported clamped");
}

// ---- the libmh twin through the fixture --------------------------------------------------------------------------------------
struct twin_world {
    sim_fixture    fx;
    llm_map_region region{};
    explicit twin_world(uint32_t dim) {
        fx.reset();
        fx.map_width              = static_cast<int32_t>(dim);
        fx.map_height             = static_cast<int32_t>(dim);
        fx.region_coord_wrap_mask = (dim == 256) ? 0xffffu : ((dim - 1) << 8) | (dim - 1);
        std::fill(fx.passable.begin(), fx.passable.end(), static_cast<uint8_t>(0));
        region.route_mark   = 1;
        fx.region_list_head = &region;
    }
    void load(const std::vector<uint8_t> &low, const std::vector<uint8_t> &elig, uint32_t dim) {
        for (uint32_t x = 0; x < dim; ++x)
            for (uint32_t y = 0; y < dim; ++y) {
                const uint32_t       i = (x << 8) | y;
                llm_map_region_cell &c = fx.region_grid[static_cast<size_t>(x * MAP_GRID_DIM + y)];
                c.region               = &region;
                c.terrain_flags        = low[i];
                fx.passable[i]         = elig[i] ? 1 : 0;
            }
    }
    int32_t run(int32_t query, int32_t start) {
        sim_store own = fx.store();
        sim_view  v   = fx.view();
        return detail::region_flood_reachable(v, own, query, start);
    }
    flat_cells snapshot(uint32_t dim) {
        flat_cells out;
        for (uint32_t x = 0; x < dim; ++x)
            for (uint32_t y = 0; y < dim; ++y) out.f[(x << 8) | y] = fx.region_grid[static_cast<size_t>(x * MAP_GRID_DIM + y)].terrain_flags;
        return out;
    }
};

void f4_twin() {
    fixes_scope scope;
    uint32_t    cost[256];
    raw_cost(cost);
    // (a) a small no-lap map: key off = the FIFO field, key on = the same field
    {
        g_seed = 777;
        std::vector<uint8_t> low, elig;
        random_map(16, 85, low, elig, 29);
        elig[0]             = 1;
        const uint32_t seed = 0;
        const int32_t  q    = (15 << 8) | 15;
        flat_cells     ref;
        init_cells(ref, 16, low, elig);
        bool     lapped;
        uint32_t deq;
        fifo_ref(ref, seed, 0x0f0fu, cost, lapped, deq);

        set_move_fixes(move_fixes{});
        twin_world off(16);
        off.load(low, elig, 16);
        const int32_t r_off = off.run(q, static_cast<int32_t>(seed));
        ck(same_field(off.snapshot(16), ref), "F4: key off: the twin writes the retail FIFO field");

        move_fixes mf;
        mf.route_flood_exact = true;
        set_move_fixes(mf);
        mv::planner_hits_reset();
        twin_world on(16);
        on.load(low, elig, 16);
        const int32_t r_on = on.run(q, static_cast<int32_t>(seed));
        ck(same_field(on.snapshot(16), ref), "F4: key on: the twin's exact field equals the FIFO field on a no-lap map");
        ck_eq(static_cast<uint32_t>(r_on), static_cast<uint32_t>(r_off), "F4: ... and the reachability answer is the same");
        ck(mv::planner_hit_count(mv::PH_FLOOD_EXACT) == 1, "F4: the twin counted its exact flood");
    }
    // (b) the lap map: key on = the true shortest-path field
    {
        g_seed = 12345;
        std::vector<uint8_t> low(N, 0), elig(N, 0);
        for (uint32_t i = 0; i < N; ++i) {
            low[i]  = static_cast<uint8_t>(1 + (rnd() % 60u));
            elig[i] = 1;
        }
        flat_cells ref;
        init_cells(ref, 256, low, elig);
        dijkstra_ref(ref, (128u << 8) | 128u, 0xffffu, cost);
        move_fixes mf;
        mf.route_flood_exact = true;
        set_move_fixes(mf);
        twin_world on(256);
        on.load(low, elig, 256);
        on.run((255 << 8) | 255, (128 << 8) | 128);
        ck(same_field(on.snapshot(256), ref), "F4: key on, lapping map: the twin's field is the true shortest-path field");
        // with route_cost_mode on and a custom table the twin weights edges by the table
        mf.route_cost_mode   = 1;
        mf.route_cost_base   = 2;
        mf.route_cost_ring4  = 1;
        mf.route_cost_ring8  = 3;
        mf.route_cost_ring16 = 5;
        set_move_fixes(mf);
        uint32_t                    tbl[256];
        const mv::route_cost_params p = mv::route_cost_params_clamped(2, 1, 3, 5, 0);
        for (uint32_t l = 0; l < 256; ++l) tbl[l] = mv::route_cost_calc(p, l, 0);
        flat_cells ref2;
        init_cells(ref2, 256, low, elig);
        dijkstra_ref(ref2, (128u << 8) | 128u, 0xffffu, tbl);
        twin_world on2(256);
        on2.load(low, elig, 256);
        on2.run((255 << 8) | 255, (128 << 8) | 128);
        ck(same_field(on2.snapshot(256), ref2), "F4: key on + a custom route_cost table: the exact field uses the table's weights");
    }
}

} // namespace

void run_move_proto_flood_tests() {
    f1_no_lap();
    f2_lap();
    f3_overflow();
    f4_twin();
}

} // namespace mh::sim::test
