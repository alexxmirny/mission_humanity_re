//
// sim/sim_merge_goal.h -- mods:M1X [net] move_merge_goal_fix (follows [net] improved_movement).
//
// A squad merge (Shift/double right-click on an own soldier, order 0x33 via llm_strat_unit_order_move_relative) moves the
// squad onto the TARGET SOLDIER'S tile. That tile is impassable (the soldier stands on it), so
// llm_strat_group_move_order_pathfind 0x0041ca37 step 2 relocates the goal with llm_strat_pathfind_find_closer_visible_tile
// 0x0041fff5 (its only call, 0x0041cc69): a fixed spiral around the goal (north first) that takes the FIRST passable,
// discovered tile strictly closer (Manhattan) to the group centre -- with no reachability test. Next to an attacking group
// that tile is often walled in by the other soldiers, a tree or a building, and the router then walks the long way round to
// it (repro tmp/m1/merge_path_and_fx.md: 14 steps instead of 5).
//
// With the key on, for a wave whose units carry order 0x33, the goal is instead the NEAREST REACHABLE tile within one tile
// (Chebyshev, the merge's own step-3e adjacency) of the target: a breadth-first search from the group centre over passable
// tiles, 8 neighbours in a fixed order (N, E, S, W, NE, SE, SW, NW), a diagonal allowed when at least one of its two
// orthogonal corners is passable (the router's flank rule), bounded to Chebyshev radius (start..target + 16) around the
// start. The found tile must also be discovered by the owner (the retail pick's own rule). No tile found, or the start
// already next to the target: the retail pick runs. Integer only, fixed order, no clock, no rand; the scratch is
// function-static (one per module) and carries nothing between calls that affects the result.
//
// Header-only so the retail splice (mh/seams/move_proto_arrival.cpp) and the libmh twin (sim_group_move_order_pathfind.cpp,
// gated through reimpl_fixes -> set_fixes) compute the same tile from the same inputs.
//
#pragma once

#include <cstdint>

namespace mh::sim {

namespace merge_goal_detail {
inline bool &flag() {
    static bool on = false;
    return on;
}
inline int32_t axis_dist(uint32_t a, uint32_t b, uint32_t dim) {
    int32_t d = static_cast<int32_t>(a) - static_cast<int32_t>(b);
    if (d < 0) d = -d;
    if (static_cast<int32_t>(dim) - d < d) d = static_cast<int32_t>(dim) - d;
    return d;
}
inline int32_t cheb(uint32_t ax, uint32_t ay, uint32_t bx, uint32_t by, uint32_t w, uint32_t h) {
    const int32_t dx = axis_dist(ax, bx, w), dy = axis_dist(ay, by, h);
    return dx > dy ? dx : dy;
}
} // namespace merge_goal_detail

inline void set_move_merge_goal_fix(bool on) { merge_goal_detail::flag() = on; }
inline bool move_merge_goal_fix() { return merge_goal_detail::flag(); }

constexpr uint16_t MERGE_GOAL_ORDER = 0x33; // UNIT_STATE_SQUAD_MERGE, the order a merge mover carries

// passable(tile) -> bool, discovered(tile) -> bool; tile = (x<<8)|y. w/h = map width/height (powers of two, <= 256).
// Returns true and writes (out_x, out_y) when it found a tile; false = run the retail pick.
template <class Passable, class Discovered>
inline bool merge_goal_search(uint32_t sx, uint32_t sy, uint32_t gx, uint32_t gy, uint32_t w, uint32_t h, Passable passable,
                              Discovered discovered, uint32_t *out_x, uint32_t *out_y) {
    using namespace merge_goal_detail;
    if (w == 0 || h == 0 || w > 256 || h > 256) return false;
    const uint32_t wm = w - 1, hm = h - 1;
    sx &= wm, sy &= hm, gx &= wm, gy &= hm;
    const int32_t d0 = cheb(sx, sy, gx, gy, w, h);
    if (d0 <= 1) return false;
    int32_t radius = d0 + 16;
    if (radius > 127) radius = 127;

    static uint32_t seen[65536];
    static uint32_t gen = 0;
    static uint16_t queue[65536];
    if (++gen == 0) {
        for (uint32_t i = 0; i < 65536; ++i) seen[i] = 0;
        gen = 1;
    }
    static const int8_t DX[8] = {0, 1, 0, -1, 1, 1, -1, -1};
    static const int8_t DY[8] = {-1, 0, 1, 0, -1, 1, 1, -1};

    uint32_t       head = 0, tail = 0;
    const uint32_t s = (sx << 8) | sy;
    seen[s]          = gen;
    queue[tail++]    = static_cast<uint16_t>(s);
    while (head < tail) {
        const uint32_t t  = queue[head++];
        const uint32_t tx = t >> 8, ty = t & 0xffu;
        for (int k = 0; k < 8; ++k) {
            const uint32_t nx = (tx + static_cast<uint32_t>(DX[k])) & wm, ny = (ty + static_cast<uint32_t>(DY[k])) & hm;
            const uint32_t n = (nx << 8) | ny;
            if (seen[n] == gen) continue;
            if (cheb(nx, ny, sx, sy, w, h) > radius) continue;
            if (!passable(n)) continue;
            if (k >= 4) { // diagonal: one of the two orthogonal corners must be open
                const uint32_t c1 = (nx << 8) | ty, c2 = (tx << 8) | ny;
                if (!passable(c1) && !passable(c2)) continue;
            }
            seen[n] = gen;
            if (cheb(nx, ny, gx, gy, w, h) <= 1 && n != ((gx << 8) | gy) && discovered(n)) {
                *out_x = nx;
                *out_y = ny;
                return true;
            }
            if (tail < 65536) queue[tail++] = static_cast<uint16_t>(n);
        }
    }
    return false;
}

} // namespace mh::sim
