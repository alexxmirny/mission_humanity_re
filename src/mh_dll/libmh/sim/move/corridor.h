//
// sim/move/corridor.h -- mods:M1H lane B: [net] route_corridor_mode (mods:M1C), modes 0 / 1 / 3 and the plain mode 2 widening.
//
// llm_strat_group_move_order_pathfind 0x0041ca37 (0x0041d06c) calls llm_strat_pathfind_mark_group_member_regions, which marks
// (route_mark = 2) the nav region each wave member stands in, and the region route trace has already marked the regions of the
// found route (1). The flood that follows (llm_map_region_flood_reachable) is eligible only on cells whose region carries a
// non-zero mark (and passable != 0), so it is confined to that corridor. A route that has to leave the corridor (a cliff gap
// next to it, a goal pocket one region over) is not found by the flood.
//
// route_corridor_mode widens the eligible set right after the original marking:
//   0  retail: nothing.
//   1  every region ADJACENT to a region the original marked (1 or 2) gets mark 3. The set is computed from the marks as they
//      were on entry (a region marked 3 never expands), so list order cannot matter.
//   2  every region gets mark 3 (the whole map is eligible). NOTE: the flood algorithm under mode 2 is the retail FIFO
//      label-correcting one; its cost on a large map is a separate, open decision (tmp/m1h/plan.md section 5) and is NOT
//      changed here.
//   3  widen nothing, only report (measurement).
//
// The mark list is walked through an accessor so the retail splice (live memory, raw offsets), the libmh twin (the sim_view
// region list) and the selftest (an array fake) run the same code:
//
//   struct Regions {
//       using handle = <pointer-like, null = false>;
//       handle   head();                      // the active list head
//       handle   next(handle);                // intrusive list link
//       int32_t &mark(handle);                // route_mark
//       uint32_t cells(handle);               // cell_count
//       uint32_t neighbor_count(handle);      // entries in neighbors[] (<= 128 read)
//       handle   neighbor(handle, uint32_t);  // neighbors[i]
//   };
//
// Integer only, no clock, no rand. Writes only route_mark (a scratch field the flood reads and phase 1 of the next flood resets
// per region through the marking pass; not saved state).
//
#pragma once

#include <cstdint>

namespace mh::sim::move {

struct corridor_result {
    int      regions; // regions on the list
    int      before;  // regions with a non-zero mark on entry
    int      after;   // ... after the widening
    uint32_t cells;   // sum of cell_count over the marked regions after (the eligible area)
};

constexpr uint32_t CORRIDOR_MAX_NEIGHBORS = 128;

// The setter's clamp (mh.dll applies the same one; libmh reads the raw host-synced value).
constexpr int corridor_clamp_mode(int mode) { return mode < 0 ? 0 : (mode > 3 ? 3 : mode); }

template <class Regions>
inline corridor_result corridor_widen(int mode, Regions &a) {
    corridor_result r{0, 0, 0, 0};
    auto            head = a.head();
    for (auto n = head; n; n = a.next(n)) {
        ++r.regions;
        if (a.mark(n) != 0) ++r.before;
    }
    if (mode == 1) {
        for (auto n = head; n; n = a.next(n)) {
            const int32_t m = a.mark(n);
            if (m != 1 && m != 2) continue; // only the entry marks expand (3 is ours)
            const uint32_t cnt = a.neighbor_count(n);
            for (uint32_t i = 0; i < cnt && i < CORRIDOR_MAX_NEIGHBORS; ++i) {
                auto nb = a.neighbor(n, i);
                if (nb && a.mark(nb) == 0) a.mark(nb) = 3;
            }
        }
    } else if (mode == 2) {
        for (auto n = head; n; n = a.next(n))
            if (a.mark(n) == 0) a.mark(n) = 3;
    }
    for (auto n = head; n; n = a.next(n))
        if (a.mark(n) != 0) {
            ++r.after;
            r.cells += a.cells(n);
        }
    return r;
}

} // namespace mh::sim::move
