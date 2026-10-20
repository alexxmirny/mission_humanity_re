//
// sim/move/flood.h -- mods:M1H lane "flood": [net] route_flood_exact, the exact (bucket-queue) phase 2 of the region flood.
//
// llm_map_region_flood_reachable 0x00424ee0, phase 2 (0x00424f3a..0x0042506d), is a FIFO label-correcting relaxation over a
// 4096-slot ring: a cell is re-enqueued whenever a cheaper distance reaches it and nothing de-duplicates. Under route_corridor_mode
// 2 (every region eligible) that is ~4.7 dequeues per cell on a 256x256 map, and a 59k-cell front overruns the ring.
//
// Under route_flood_exact phase 2 is Dial's bucket queue instead. The relaxation rule is the retail one:
//   distance(cell) = flags >> 8 (24 bits; 0 = not eligible, 0xffffff = eligible and unreached, the seed starts at 1);
//   edge weight    = cost[low byte of the SOURCE cell] (route_cost_calc, or the raw byte), added to the source's distance;
//   a neighbour is improved only when it is eligible (distance != 0) and the new distance is strictly smaller;
//   the four neighbours are produced by the retail CHAINED byte inc/dec + wrap-mask sequence (flood_neighbors).
// The FIFO's fixpoint is the exact shortest-path field, so whenever the FIFO did not lap its ring the field written here is
// IDENTICAL cell for cell; where it laps, this is the true shortest path. Each cell is settled once, no ring, no queue limit.
//
// Buckets: weights are 0..maxw (maxw from the 256-entry cost table), so the pending distances always lie in [d, d+maxw] and
// maxw+1 circular buckets suffice (rounded up to a power of two: a mask instead of a division). A cell that is improved while queued
// is unlinked from its old bucket (doubly linked lists over one 65536-entry node array), so every cell is popped exactly once at
// its final distance. Scratch arrays are static
// working memory, not sim state: not saved, not hashed, fully drained (all links dead) on return.
//
// Integer only, no clock, no rand. Shared by the retail splice (mh/seams/net_lockstep.cpp fe_run) and the libmh twin
// (sim_map_region_routing.cpp); the cells are reached through an accessor:
//
//   struct Cells { uint32_t &flags(uint32_t flat); };   // flat = (col << 8) | row, the cell's terrain_flags word
//
#pragma once

#include <cstdint>

namespace mh::sim::move {

constexpr uint32_t FLOOD_FLAT_CELLS   = 65536;     // flat indices the scratch arrays cover
constexpr uint32_t FLOOD_UNREACHED    = 0xffffffu; // the eligible-but-unreached distance (flags 0xffffff00)
constexpr uint32_t FLOOD_MAX_WEIGHT   = 4096;      // per-edge weight clamp (route_cost values top out near 650)
constexpr uint32_t FLOOD_COST_ENTRIES = 256;

struct flood_exact_result {
    uint32_t settled;  // cells popped (the work unit the corridor measurement counts as "dequeues")
    uint32_t buckets;  // circular bucket count used (max weight + 1)
    bool     overflow; // a relaxation would have exceeded the 24-bit distance: it was dropped (the cell stays unreached)
    bool     clamped;  // a weight above FLOOD_MAX_WEIGHT was clamped
};

// The four neighbours of `cur`, exactly as the retail code computes them: byte inc/dec on the SAME register state, each followed
// by AND wrap_mask (0x00424f8a..0x00425032) -- NOT four independent deltas.
inline void flood_neighbors(uint32_t cur, uint32_t wrap_mask, uint32_t out[4]) {
    uint32_t ebx = cur;
    auto     lo  = [&](int32_t d) { ebx = (ebx & 0xFFFFFF00u) | static_cast<uint8_t>(static_cast<int32_t>(ebx & 0xFFu) + d); };
    auto     hi  = [&](int32_t d) {
        ebx = (ebx & 0xFFFF00FFu) | (static_cast<uint32_t>(static_cast<uint8_t>(static_cast<int32_t>((ebx >> 8) & 0xFFu) + d)) << 8);
    };
    lo(+1);
    ebx &= wrap_mask;
    out[0] = ebx; // row+1
    lo(-2);
    ebx &= wrap_mask;
    out[1] = ebx; // row-1
    lo(+1);
    hi(+1);
    ebx &= wrap_mask;
    out[2] = ebx; // col+1
    hi(-2);
    ebx &= wrap_mask;
    out[3] = ebx; // col-1
}

namespace flood_detail {
constexpr uint32_t NONE       = 0xffffffffu;
constexpr uint32_t NOT_QUEUED = 0xfffffffeu; // node.nxt of a cell that is in no bucket
struct node {
    uint32_t nxt; // next cell in the bucket, NONE = last, NOT_QUEUED = not in a bucket
    uint32_t prv; // previous cell, NONE = the bucket head
};
struct scratch {
    node     n[FLOOD_FLAT_CELLS];
    uint32_t head[2 * FLOOD_MAX_WEIGHT]; // the bucket count is the next power of two above the max weight (<= 8192)
    bool     clean = false;
};
inline scratch &sc() {
    static scratch s;
    return s;
}
} // namespace flood_detail

// Runs phase 2 from `seed` (< FLOOD_FLAT_CELLS; the caller checks) over cells whose phase 1 already ran (eligible = 0xffffff00,
// not eligible = high 24 bits 0). `cost[low]` is the edge weight of a source cell whose low byte is `low`.
template <class Cells>
inline flood_exact_result flood_exact(Cells &cells, uint32_t seed, uint32_t wrap_mask, const uint32_t cost[FLOOD_COST_ENTRIES]) {
    using namespace flood_detail;
    flood_exact_result r{0, 1, false, false};
    uint32_t           w[FLOOD_COST_ENTRIES];
    uint32_t           maxw = 0;
    for (uint32_t i = 0; i < FLOOD_COST_ENTRIES; ++i) {
        uint32_t c = cost[i];
        if (c > FLOOD_MAX_WEIGHT) {
            c         = FLOOD_MAX_WEIGHT;
            r.clamped = true;
        }
        w[i] = c;
        if (c > maxw) maxw = c;
    }
    // Bucket count: at least max weight + 1 (so the pending distances d..d+maxw never alias), rounded up to a power of two so the
    // bucket index is a mask, not a division. Any count >= maxw + 1 gives the same result.
    uint32_t B = 1;
    while (B < maxw + 1) B <<= 1;
    const uint32_t BM = B - 1;
    r.buckets         = B;
    scratch &s        = sc();
    if (!s.clean) {
        for (uint32_t i = 0; i < FLOOD_FLAT_CELLS; ++i) s.n[i].nxt = NOT_QUEUED; // first use: every cell out of every bucket
        s.clean = true;
    }
    for (uint32_t i = 0; i < B; ++i) s.head[i] = NONE;

    auto push = [&](uint32_t cell, uint32_t d) {
        const uint32_t b = d & BM;
        const uint32_t h = s.head[b];
        s.n[cell].prv    = NONE;
        s.n[cell].nxt    = h;
        if (h != NONE) s.n[h].prv = cell;
        s.head[b] = cell;
    };
    auto unlink = [&](uint32_t cell, uint32_t d) {
        const uint32_t p = s.n[cell].prv, nx = s.n[cell].nxt;
        if (p == NONE) s.head[d & BM] = nx;
        else s.n[p].nxt = nx;
        if (nx != NONE) s.n[nx].prv = p;
        s.n[cell].nxt = NOT_QUEUED;
    };

    uint32_t &seed_flags = cells.flags(seed);
    seed_flags           = (seed_flags & 0xFFu) | 0x100u; // distance 1
    uint32_t pending     = 1;
    push(seed, 1);

    uint32_t d = 1;
    while (pending != 0) {
        const uint32_t b = d & BM;
        while (s.head[b] != NONE) {
            const uint32_t cur = s.head[b];
            unlink(cur, d);
            --pending;
            ++r.settled;
            const uint32_t cur_flags = cells.flags(cur);
            const uint32_t nd        = d + w[cur_flags & 0xFFu];
            uint32_t       nb[4];
            flood_neighbors(cur, wrap_mask, nb);
            for (int k = 0; k < 4; ++k) {
                const uint32_t n = nb[k];
                if (n >= FLOOD_FLAT_CELLS) continue; // outside the scratch (cannot happen with a 16-bit wrap mask)
                uint32_t      &nf  = cells.flags(n);
                const uint32_t nbd = nf >> 8;
                if (nbd == 0) continue; // not flood-eligible at all
                if (nd >= nbd) {
                    if (nd >= FLOOD_UNREACHED && nbd == FLOOD_UNREACHED) r.overflow = true; // 24-bit distance exhausted: dropped
                    continue;                                                               // not an improvement
                }
                if (s.n[n].nxt != NOT_QUEUED) unlink(n, nbd);
                else ++pending;
                nf = (nd << 8) | (nf & 0xFFu);
                push(n, nd);
            }
        }
        ++d;
    }
    return r;
}

} // namespace mh::sim::move
