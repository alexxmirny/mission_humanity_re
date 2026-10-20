//
// sim/move/route_cost.h -- mods:M1H lane B: [net] route_cost_mode / route_cost_base / route_cost_ring4 / route_cost_ring8 /
// route_cost_ring16 / route_gap_wave_full, the pure rule.
//
// The region flood (llm_map_region_flood_reachable 0x00424ee0, the read at 0x00424f7f) adds the dequeued cell's cost to the
// distance it propagates: `(terrain_flags & 0xff) << 8`. The low byte is bit0 = passable (cost 1) plus three obstacle-proximity
// ring bits (4 / 8 / 16: a cell touching an obstacle carries all three, 1+4+8+16 = 29). With route_cost_mode on, the low byte is
// mapped through route_cost_calc() first; the STORED bits are never changed. With the defaults (1/4/8/16, wave_full 0) the result
// equals `low`, so a table at its defaults reproduces retail exactly.
//
// route_gap_wave_full = N scales only the RING part by min(wave_size, N) / N (integer division), so a lone unit pays little for
// squeezing past an obstacle while a wave of N or more pays the full table. wave_size is the order's member count
// (_G_LLM_STRAT_GROUP_MEMBER_COUNT).
//
// Shared by the retail splice (mh/seams/net_lockstep.cpp mv_flood_c_stub) and the libmh twin (sim_map_region_routing.cpp).
// Integer only, no state: deterministic.
//
#pragma once

#include <cstdint>

namespace mh::sim::move {

struct route_cost_params {
    int base      = 1;  // cost of the passable bit (retail 1), 1..100
    int ring4     = 4;  // proximity bit 4   (retail 4), 0..100
    int ring8     = 8;  // proximity bit 8   (retail 8), 0..100
    int ring16    = 16; // proximity bit 16  (retail 16), 0..100
    int wave_full = 0;  // route_gap_wave_full N, 0 = off, 0..64
};

constexpr int rc_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The setters' clamps. mh.dll's gameplay setters apply the same ones; libmh reads the raw host-synced values, so it clamps here.
inline route_cost_params route_cost_params_clamped(int base, int ring4, int ring8, int ring16, int wave_full) {
    route_cost_params p;
    p.base      = rc_clamp(base, 1, 100);
    p.ring4     = rc_clamp(ring4, 0, 100);
    p.ring8     = rc_clamp(ring8, 0, 100);
    p.ring16    = rc_clamp(ring16, 0, 100);
    p.wave_full = rc_clamp(wave_full, 0, 64);
    return p;
}

// `low` = the cell's terrain_flags low byte, `wave_size` = the order's member count. Bits other than passable and the three rings
// stay raw.
inline uint32_t route_cost_calc(const route_cost_params &p, uint32_t low, int32_t wave_size) {
    uint32_t cost = low & ~0x1Du;
    if (low & 1u) cost += static_cast<uint32_t>(p.base);
    uint32_t ring = ((low & 4u) ? static_cast<uint32_t>(p.ring4) : 0u) + ((low & 8u) ? static_cast<uint32_t>(p.ring8) : 0u) +
                    ((low & 16u) ? static_cast<uint32_t>(p.ring16) : 0u);
    if (p.wave_full > 0) {
        int n = wave_size;
        if (n < 1) n = 1;
        if (n > p.wave_full) n = p.wave_full;
        ring = ring * static_cast<uint32_t>(n) / static_cast<uint32_t>(p.wave_full);
    }
    return cost + ring;
}

} // namespace mh::sim::move
