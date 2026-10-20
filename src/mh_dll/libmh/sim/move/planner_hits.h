//
// sim/move/planner_hits.h -- mods:M1H lane B: the hit counters of the route-planner TWINS (libmh side only).
//
// The retail splices bump g_mv_hit[] in mh.dll (seams/move_carrier.h) and the carrier report logs `; move hits ...`. The twins run
// in libmh.dll, a different image with its own statics, and the export contract carries no counter array (and a contract row is a
// libmh.def + generator change outside this lane), so the twin counts here and SAYS ITSELF through the shared line sink
// (mh::ai::ai_say -> mh_net.log):
//
//     ; move twin hits cent_x=1
//
// logged on the FIRST hit of a counter and again at 10, 100, 1000, ... so a row can assert "the key path fired" with a number and
// a long match does not flood the log. The names are the carrier's hit names (move_carrier.cpp k_hit_name), so a checker reads
// both lines the same way.
//
// NOT sim state: not saved, not hashed, never read back by the sim. Include this header from libmh .cpp bodies only (it pulls in
// ai_say); the pure rules in the other move/*.h are included by both images.
//
#pragma once

#include <cstdint>

#include "ai/ai_state.h" // ai_say

namespace mh::sim::move {

enum planner_hit_id : int {
    PH_CENT_X,
    PH_CENT_Y,
    PH_SLOT_X,
    PH_SLOT_Y,
    PH_DETOUR,
    PH_FLOOD_Q,
    PH_FLOOD_C,
    PH_CORR,
    PH_ARR_COMMIT,
    PH_RT,
    PH_RTM,
    PH_RTR,
    PH_FLOOD_EXACT,
    PH_FC,
    PH_COUNT
};

inline const char *planner_hit_name(int id) {
    switch (id) {
        case PH_CENT_X: return "cent_x";
        case PH_CENT_Y: return "cent_y";
        case PH_SLOT_X: return "slot_x";
        case PH_SLOT_Y: return "slot_y";
        case PH_DETOUR: return "detour";
        case PH_FLOOD_Q: return "flood_q";
        case PH_FLOOD_C: return "flood_c";
        case PH_CORR: return "corr";
        case PH_ARR_COMMIT: return "arr_commit";
        case PH_RT: return "route_through";
        case PH_RTM: return "route_moving";
        case PH_RTR: return "route_replanning";
        case PH_FLOOD_EXACT: return "flood_exact";
        case PH_FC: return "formation";
        default: return "?";
    }
}

namespace planner_hits_detail {
inline uint32_t *counts() {
    static uint32_t c[PH_COUNT] = {};
    return c;
}
} // namespace planner_hits_detail

// 1, 10, 100, 1000, ...
inline bool planner_hit_due(uint32_t n) {
    if (n == 0) return false;
    while (n % 10u == 0) n /= 10u;
    return n == 1;
}

inline void planner_hit(planner_hit_id id) {
    const uint32_t n = ++planner_hits_detail::counts()[id];
    if (planner_hit_due(n)) mh::ai::ai_say("; move twin hits %s=%u\n", planner_hit_name(id), n);
}

// Flood dequeues since the last corridor line (the measurement the retail splice keeps in g_flood_deq). Counted by the flood body
// on every dequeue whatever the keys, read-and-cleared by the corridor line.
inline uint32_t &flood_dequeues() {
    static uint32_t n = 0;
    return n;
}

inline uint32_t planner_hit_count(planner_hit_id id) { return planner_hits_detail::counts()[id]; }
inline void     planner_hits_reset() {
    for (int i = 0; i < PH_COUNT; ++i) planner_hits_detail::counts()[i] = 0;
}

} // namespace mh::sim::move
