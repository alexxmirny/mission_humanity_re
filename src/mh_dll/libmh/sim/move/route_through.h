//
// sim/move/route_through.h -- mods:M1H lane B: [net] move_route_through_idle (mods:M1N 3e-5), the pure rule.
//
// The wave planner (llm_strat_group_move_order_pathfind: region flood, route trace, "nearest reachable tile" fallback, per-member
// walks) reads the tile `passable` byte as its only obstacle test, and a unit standing on a tile holds that byte at 0 (the tile's
// own value lives in the unit's origin_tile_was_passable). So an idle friend in a one-tile corridor is a WALL to the planner: the
// goal is shortened to the nearest tile on this side of it (a dead-end pocket) and the order is given up after ~6 s, although the
// walker (move_nudge_idle) and the marshal site would happily have made room.
//
// The wrapper makes the planner see those tiles as open for the duration of ONE call: rt_unmask() writes the tile's own value
// (origin_tile_was_passable) into passable[] for every IDLE same-player ground unit of the order's owner, rt_restore() puts the 0
// back right after. One stretch of code, so every reader of passable[] inside the planner agrees. Kept as walls: the goal tile
// itself (a goal held by an idle friend is moved as today), walking / sleeping-in-order units, enemies, allies, buildings and
// every unit of another player. Wave members are not touched (register_member already restored their tiles).
//
// mode 1 (the shipped value): an opened tile also gets the three proximity-ring bits (+ passable, 0x1d) OR-ed into its cached
// cost byte (terrain_flags low byte) so the route flood prices it like a tile pressed against a cliff and prefers a free way
// round; rt_restore() puts the byte back. mode 2 (measurement): open the tiles at the plain tile cost (no penalty).
// Needs the nudge key on (the nudge / trade is what resolves the meeting): no nudge, no change.
//
// move_route_through_moving (rt_unmask_moving, below; the user's request of 2026-10-18): the same bracket also opens the tile of a
// WALKING same-player ground unit (state 0x0f move_walker). A unit that steps vacates its old tile AT STEP START (tile_objects
// cleared, passable[old] = origin_tile_was_passable) and claims the new one at once (passable 5 while it slides, 0 once settled), so
// a walker holds exactly ONE tile; by the time a follower arrives it has usually moved on, and if not, the walker's blocked-unit
// protocol (move_block_wait_ms, nudge / swap) resolves the meeting at step time. The tile is priced by a separate cost level
// (rt_move_cost_bits) and restored exactly (passable byte AND cost byte). Needs no other key.
//
// ORDER (the trap, plan section 1): the wrapper brackets ONE call of the planner. The distinct-goals re-run of the planner (move/arrival.h)
// used to be OUTSIDE it (the rc17 dig's cause A); since mods:M1DT fix A' it opens and closes the same bracket around its own call, in the
// retail splice (mv_rt_open / mv_rt_close) and in the libmh twin alike. The formation (move/formation.h) plans inside the retail bracket.
//
// DETERMINISM: integer, units visited in index order (from 1), a fixed-size undo list, no clock, no rand; the same call on every
// peer. Header-only over a World accessor so the splice (mh/seams/move_proto_nudge.cpp, live memory), the libmh twin
// (sim_group_move_order_commit.cpp) and the selftest (a fake) run the same code:
//
//   struct World {
//       int32_t  unit_count();                                          // roster cap per player
//       bool     unit(uint32_t player, int32_t i, rt_unit *out);        // false: no such record / no config row
//       uint16_t tile_building(uint32_t flat);                          // tile_objects[flat].building
//       uint8_t  tile_class_owner(uint32_t flat);                       // tile_objects[flat].class_owner
//       uint8_t &passable(uint32_t flat);
//       uint32_t &cell_flags(uint32_t flat);                            // region grid terrain_flags
//   };
//
#pragma once

#include <cstdint>

namespace mh::sim::move {

constexpr int      RT_MAX          = 1024;
constexpr uint32_t RT_PENALTY_BITS = 0x1d; // passable + the three proximity rings
constexpr uint16_t RT_ST_IDLE      = 0x01; // unit state: standing idle
constexpr int32_t  RT_MS_SETTLED   = 0x1f; // move_microstep value of a unit that stands still on its tile
constexpr uint16_t RT_ST_WALK      = 0x0f; // unit state: move_walker (following a path)
constexpr uint16_t RT_ST_MARSHAL   = 0x0a; // unit state: group_marshal (waiting to be re-planned; mods:M1DT)
constexpr uint16_t RT_ST_MOVE_HOP  = 0x0c; // unit state: the one-tick hop of an earlier wave of the same marshal call (cause C, see rt_unmask_hop)
constexpr uint8_t  RT_PASS_SLIDING = 5;    // passable[] of a tile a unit is sliding into

// The cost levels of move_route_through_moving_cost, as the cost byte (terrain_flags low byte) bits ORed in: 0 = the plain tile (bit 0
// only), 1 = +ring 4, 2 = +rings 4 and 8, 3 = all three rings (the idle key's 0x1d), 4 = all three rings plus raw bit 2 (the flood adds
// raw bits as plain cost). Out of range clamps.
inline uint32_t rt_move_cost_bits(int level) {
    static const uint32_t bits[5] = {0x01u, 0x05u, 0x0du, 0x1du, 0x1fu};
    return bits[level < 0 ? 0 : (level > 4 ? 4 : level)];
}

// What rt_unmask needs to know about one unit (the accessor fills it; cfg_ground_idle = the unit class moves on the ground
// (move_op_code 0x0f) and rests in the ground idle state (default_op_code 1), alive = energy > 0.0 read as raw bits).
struct rt_unit {
    uint16_t state;
    uint8_t  order_queued;
    int32_t  move_microstep;
    uint8_t  origin_tile_was_passable;
    bool     cfg_ground_idle;
    bool     cfg_ground_move; // the unit class moves on the ground (move_op_code 0x0f), whatever it rests in
    bool     alive;
    uint8_t  x, y;
};

struct rt_undo {
    uint32_t flat[RT_MAX]; // flat (x << 8 | y) indices whose passable byte was lifted
    uint8_t  low[RT_MAX];  // the cell's cached cost byte (terrain_flags low byte) before the penalty
    uint8_t  pass[RT_MAX]; // the passable byte before the lift (0 idle, 0 or 5 walking)
    int      n = 0;
};

// Opens the tiles; returns how many. mode: 0 = off, 1 = with the cost penalty, 2 = plain cost. `nudge_on` = the nudge key.
template <class World>
inline int rt_unmask(World &w, rt_undo &u, int mode, bool nudge_on, uint32_t player, int32_t goal_x, int32_t goal_y, uint32_t width_mask,
                     uint32_t height_mask) {
    u.n = 0;
    if (mode == 0 || !nudge_on) return 0;
    if (player >= 8) return 0;
    const uint32_t gx = static_cast<uint32_t>(goal_x) & width_mask, gy = static_cast<uint32_t>(goal_y) & height_mask;
    const int32_t  cap = w.unit_count();
    for (int32_t i = 1; i < cap && u.n < RT_MAX; ++i) {
        rt_unit ru;
        if (!w.unit(player, i, &ru)) continue;
        if (ru.state != RT_ST_IDLE || ru.order_queued != 0 || ru.move_microstep < RT_MS_SETTLED || ru.origin_tile_was_passable == 0) continue;
        if (!ru.cfg_ground_idle || !ru.alive) continue;
        if (ru.x == gx && ru.y == gy) continue;
        const uint32_t flat = (static_cast<uint32_t>(ru.x) << 8) | ru.y;
        if (w.tile_building(flat) != i || w.tile_class_owner(flat) != static_cast<uint8_t>(player | 0x80u)) continue;
        uint8_t &p = w.passable(flat);
        if (p != 0) continue;
        u.pass[u.n] = p;
        p           = ru.origin_tile_was_passable;
        uint32_t &f = w.cell_flags(flat);
        u.low[u.n]  = static_cast<uint8_t>(f);
        if (mode == 1) f |= RT_PENALTY_BITS;
        u.flat[u.n++] = flat;
    }
    return u.n;
}

// move_route_through_moving: APPENDS to the list rt_unmask started (call it right after; no reset). `level` < 0 = off, else the cost
// level. A tile is opened for a unit that is walking (state 0x0f), a ground mover, alive, whose tile it holds (tile_objects claimed,
// passable 0 settled or 5 sliding, not already equal to its own value), not the goal tile. Group members were lifted by
// register_member (passable == origin) and are skipped by that last test.
template <class World>
inline int rt_unmask_state(World &w, rt_undo &u, int level, uint16_t state, uint32_t player, int32_t goal_x, int32_t goal_y, uint32_t width_mask,
                           uint32_t height_mask) {
    if (level < 0 || player >= 8) return 0;
    const int      n0 = u.n;
    const uint32_t gx = static_cast<uint32_t>(goal_x) & width_mask, gy = static_cast<uint32_t>(goal_y) & height_mask;
    const int32_t  cap = w.unit_count();
    for (int32_t i = 1; i < cap && u.n < RT_MAX; ++i) {
        rt_unit ru;
        if (!w.unit(player, i, &ru)) continue;
        if (ru.state != state || ru.origin_tile_was_passable == 0 || !ru.cfg_ground_move || !ru.alive) continue;
        if (ru.x == gx && ru.y == gy) continue;
        const uint32_t flat = (static_cast<uint32_t>(ru.x) << 8) | ru.y;
        if (w.tile_building(flat) != i || w.tile_class_owner(flat) != static_cast<uint8_t>(player | 0x80u)) continue;
        uint8_t &p = w.passable(flat);
        if ((p != 0 && p != RT_PASS_SLIDING) || p == ru.origin_tile_was_passable) continue;
        u.pass[u.n] = p;
        p           = ru.origin_tile_was_passable;
        uint32_t &f = w.cell_flags(flat);
        u.low[u.n]  = static_cast<uint8_t>(f);
        f |= rt_move_cost_bits(level);
        u.flat[u.n++] = flat;
    }
    return u.n - n0;
}

template <class World>
inline int rt_unmask_moving(World &w, rt_undo &u, int level, uint32_t player, int32_t goal_x, int32_t goal_y, uint32_t width_mask, uint32_t height_mask) {
    return rt_unmask_state(w, u, level, RT_ST_WALK, player, goal_x, goal_y, width_mask, height_mask);
}

// mods:M1DT fix 2, move_route_through_replanning: the same rule for an own ground unit in state 0x0a (group_marshal: it lost its path and
// waits its batch). It stands still on its tile (passable 0), but it WILL move on once its batch is planned, and neither key above opens
// it -- so the units of a whole replanning blob were walls to each other (the rc16 in-place click: a 37-tile walk for a 6-tile shift).
// Every own 0x0a unit is opened, not only the order's own group: the planner is told nothing about groups, a replanning friend of
// another order is just as temporary, and the walker's blocked-unit protocol (nudge / swap / wait) resolves the meeting at step time.
// Shares the cost level of move_route_through_moving_cost. Appends to the list like rt_unmask_moving. Also opens the 0x0c hop units (cause C, below).
//
// Cause C (rc17 dig, O8 u13; user decision 2026-10-19: FOLDED into this same key, no new key): the same rule also opens an own ground unit in
// state 0x0c (RT_ST_MOVE_HOP, llm_strat_unit_state_enter_class_move). The marshal sets 0x0c on the members of an EARLIER wave of the SAME call
// (sim_unit_state_group_marshal.cpp: set_state_of(0x0c) for a member that is not the ticking unit) and puts their tile back to passable 0
// right after the wave's commit, so the later waves' plans met them as walls: a re-order midway detoured 34 tiles round friends that were
// about to walk on (rc17 dig, dg0 arm, O8 u12/u13). TILE OWNERSHIP: a 0x0c unit has not moved yet. Its state handler only zeroes the tick budget
// and re-dispatches into the move opcode (0x0f), whose first step vacates the old tile and claims the new one in the same tick -- so a hopping
// unit holds exactly ONE tile, the one it stands on (tile_objects claimed by it, passable 0), and never two. rt_unmask_state's ownership test
// (tile_objects[x,y].building == i and the owner byte) therefore opens exactly that tile and nothing else, like a 0x0a unit. Two units planned
// onto the same tile are resolved by the walker's blocked-unit protocol at step time, as for every other opened state; the planner reserves
// no tiles in any case.
template <class World>
inline int rt_unmask_hop(World &w, rt_undo &u, int level, uint32_t player, int32_t goal_x, int32_t goal_y, uint32_t width_mask, uint32_t height_mask) {
    return rt_unmask_state(w, u, level, RT_ST_MOVE_HOP, player, goal_x, goal_y, width_mask, height_mask);
}

template <class World>
inline int rt_unmask_replanning(World &w, rt_undo &u, int level, uint32_t player, int32_t goal_x, int32_t goal_y, uint32_t width_mask, uint32_t height_mask) {
    const int n = rt_unmask_state(w, u, level, RT_ST_MARSHAL, player, goal_x, goal_y, width_mask, height_mask);
    return n + rt_unmask_hop(w, u, level, player, goal_x, goal_y, width_mask, height_mask); // cause C: the hopping units of earlier waves too
}

// Puts every lifted tile back (passable 0, cost byte as it was).
template <class World>
inline void rt_restore(World &w, rt_undo &u) {
    for (int k = 0; k < u.n; ++k) {
        w.passable(u.flat[k]) = u.pass[k];
        uint32_t &f           = w.cell_flags(u.flat[k]);
        f                     = (f & 0xffffff00u) | u.low[k];
    }
    u.n = 0;
}

} // namespace mh::sim::move
