//
// sim/move/wrap.h -- mods:M1H lane B: [net] move_centroid_wrap_fix, the pure rule.
//
// Two toroidal-wrap sites of the route planner mishandle a delta that crosses the map seam:
//
//   llm_strat_group_move_order_pathfind 0x0041ca37, the group-centre sum (x at 0x0041caf2, y at 0x0041cb51). The per-member
//   column/row delta against member 0 is wrapped with `if (d < -w/2) d += w; else if (d > w/2) d = 0x80 - d`. The retail
//   `0x80 - d` is a MIRROR (128 - d), not the fold d - w: it is wrong on every map that is not 256 wide, and even on 256 wide
//   (d = 200 gives -72, the correct fold is -56), so the centroid of a wave straddling the seam lands off-centre.
//   Fixed: `d - w` (w = map width for x, map height for y).
//
//   llm_strat_slot_dist_to_ref 0x004cbd63 (x at 0x004cbdb0, y at 0x004cbdc3). Retail folds only the POSITIVE side
//   (`if (d > half) d -= size`); a delta below -half (the slot is on the far side of the seam the other way round) stays a
//   long way. Fixed: also `else if (d < -half) d += size`.
//
// The retail splice (mh/seams/net_lockstep.cpp mv_cent_*_stub / mv_slot_*_stub) and the libmh twin
// (sim_group_move_order_pathfind.cpp, sim_path_slot_dist.cpp) both call these, so they cannot drift. Integer only, no state.
//
#pragma once

#include <cstdint>

namespace mh::sim::move {

// Group-centre delta that is past +size/2: the retail value `0x80 - d`, or with the fix `d - size`.
inline int32_t centroid_wrap_delta(int32_t d, int32_t size, bool fix) { return fix ? d - size : 0x80 - d; }

// slot_dist_to_ref's per-axis wrap. `half` = the group's half extent, `size` = the map extent on that axis.
// `*fired` (optional) is set when the fix's EXTRA fold (d < -half) ran -- the only case the fix changes.
inline int32_t slot_wrap_delta(int32_t d, int32_t half, int32_t size, bool fix, bool *fired = nullptr) {
    if (d > half) return d - size;
    if (fix && d < -half) {
        if (fired) *fired = true;
        return d + size;
    }
    return d;
}

} // namespace mh::sim::move
