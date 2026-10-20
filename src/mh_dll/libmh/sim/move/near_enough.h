//
// sim/move/near_enough.h -- mods:M1H lane C: [net] move_near_enough (movement plan 2b) and the "stands on its goal" stop that
// [net] move_distinct_goals shares, header-only: the retail splice (mh/seams/move_proto_arrival.cpp, arrival_retry_check) and the libmh
// twin (sim_unit_state_group_marshal.cpp) call the same decision.
//
// SITE: the marshal's "path exhausted or first step blocked" branch, right after `++path_blocked_retry_count` (0x00482b5b). Retail
// gives up (STOP_TO_DEFAULT, notify 0x66, passable = 0) once the counter reaches 4, i.e. after 4 x 2 s of retries. When the decision
// below says STOP the counter is set to 4 so the retail give-up branch runs at once. Only for a plain move (order 0x0f, or 0x01 once the
// planner replanned it). Stop when:
//   * the unit stands ON its goal (an empty route to its own tile; retail retries 4 x 2 s and then stops anyway) -- with either key on; or
//   * move_near_enough = R > 0, the goal tile holds a settled unit of the same player that is not walking or regrouping (state not
//     0xa / 0xc / 0xf), and the unit is within R + isqrt(N) tiles of the goal (Chebyshev, torus); N = the units of this player alive with
//     the same move_group_id (1 for id 0).
// Integer arithmetic, units in table order, no clock, no rand. The energy read compares the double as raw bits.
//
#pragma once

#include <cstdint>
#include <cstring>

#include "addr/mh_structs.gen.h"

namespace mh::sim::mvc {

constexpr uint16_t NE_ORDER_MOVE = 0x0f, NE_ORDER_STOP_TO_DEFAULT = 0x01, NE_ST_MARSHAL = 0x0a, NE_ST_MOVE_HOP = 0x0c, NE_ST_MOVE = 0x0f;
constexpr int      NE_ROSTER = 100; // the retail scans count 100 units per player (the stock roster), never the raised cap

inline bool ne_is_plain_move(uint16_t order) { return order == NE_ORDER_MOVE || order == NE_ORDER_STOP_TO_DEFAULT; }

inline uint32_t ne_isqrt(uint32_t v) {
    uint32_t r = 0;
    while ((r + 1) * (r + 1) <= v) ++r;
    return r;
}

inline int ne_wrap_dist(int a, int b, int size) {
    int d = a - b;
    if (d < 0) d = -d;
    if (size > 0 && d > size / 2) d = size - d;
    return d;
}

// energy > 0.0, read as bits (no FPU)
inline bool ne_alive(const double &e) {
    uint64_t b;
    std::memcpy(&b, &e, sizeof(b));
    return (b >> 63) == 0 && (b & 0x7fffffffffffffffull) != 0;
}

enum class ne_stop { no,
                     on_goal,
                     near_goal };

// W: unit(player, idx) -> const unit * (null past the roster); pass(x, y) -> uint8_t; tile(x, y) -> const tile object;
//    map_w() / map_h() -> int32_t (the map extent in tiles).
// `p` = the marshal's player, `idx` = the stuck member's unit index, `near_r` = move_near_enough R (clamped 0..10, 0 = off).
template <class W>
inline ne_stop near_enough_check(W &w, uint32_t p, int32_t idx, int near_r) {
    const auto *u = w.unit(p, idx);
    if (!u) return ne_stop::no;
    if (!ne_is_plain_move(u->order)) return ne_stop::no;
    const int x = u->x, y = u->y, gx = u->goal_x, gy = u->goal_y;
    if (x == gx && y == gy) return ne_stop::on_goal;
    if (near_r <= 0) return ne_stop::no;
    if (w.pass(static_cast<uint32_t>(gx), static_cast<uint32_t>(gy)) != 0) return ne_stop::no;
    const auto &t = w.tile(static_cast<uint32_t>(gx), static_cast<uint32_t>(gy));
    if (t.class_owner != static_cast<uint8_t>(0x80u | p)) return ne_stop::no;
    const uint32_t occ = t.building;
    if (occ >= static_cast<uint32_t>(NE_ROSTER) || occ == static_cast<uint32_t>(idx)) return ne_stop::no;
    const auto *o = w.unit(p, static_cast<int32_t>(occ));
    if (!o) return ne_stop::no;
    const uint16_t ost = o->state;
    if (ost == NE_ST_MARSHAL || ost == NE_ST_MOVE_HOP || ost == NE_ST_MOVE) return ne_stop::no;
    const int32_t gid = u->move_group_id;
    uint32_t      n   = 1;
    if (gid != 0) {
        n = 0;
        for (int i = 0; i < NE_ROSTER; ++i) {
            const auto *q = w.unit(p, i);
            if (q && q->move_group_id == gid && ne_alive(q->energy)) ++n;
        }
        if (n < 1) n = 1;
    }
    const int dx = ne_wrap_dist(x, gx, w.map_w()), dy = ne_wrap_dist(y, gy, w.map_h());
    const int d = dx > dy ? dx : dy;
    if (d > near_r + static_cast<int>(ne_isqrt(n))) return ne_stop::no;
    return ne_stop::near_goal;
}

} // namespace mh::sim::mvc
