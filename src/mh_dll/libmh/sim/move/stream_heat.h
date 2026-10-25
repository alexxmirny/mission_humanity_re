//
// sim/move/stream_heat.h -- mods:M1WL v2: [net] move_stream_heat, a congestion cost for a LONE unit's plan, the pure rule.
//
// THE PROBLEM (rc7 field replay, tmp/u022/rc7/trace). A unit that leaves a barracks is a wave of ONE: group_move_order_pathfind
// (0x0041ca37) floods the cost field from the goal and walks it by steepest descent with a fixed neighbour order, so every lone unit from
// one door gets the IDENTICAL route (49/49, 27/27, 18/18 identical first 12 path entries in the replay): one tile wide, limited by the
// head of the file (one unit per ~56 steps through the corridor diagonal), while four doors emit one unit per ~12 steps. The rc7 side lanes
// (rewrite the finished route sideways) staircased, walked backwards and met the replan detour cap: retired.
//
// THE RULE. The planner already prices a tile by its cached cost byte (terrain_flags low byte, read by the flood at 0x00424f7f). While a
// lone unit is planned, the tiles its GROUPMATES are about to walk are made dearer, so the flood finds the route round them: a stream of
// lone units spreads over the open ground instead of stacking on one line.
//
// WHICH BIT (measured on the u022_stream row, rc7 field map): the first draft OR-ed the proximity-ring bits (0x04 / 0x08 / 0x10) like the
// route-through keys do (1 mate -> ring4, 2..3 -> ring4 + ring8, >= 4 -> all three). With the shipped route table (rings 1 / 2 / 3,
// route_gap_wave_full 6) a LONE unit pays the ring part scaled by 1/6 (route_cost_calc: sum of the rings * min(wave, 6) / 6), i.e. +1 for all
// three rings together and +0 for one or two: the heat was priced at nothing and the column did not move (maxtile 41 of 45, key 0: 42). The
// raw bits are not touched by that table ("bits other than passable and the three rings stay raw"), so the heat is the raw bit 0x02: +2 on top of
// the tile's own cost, for every counted tile whatever its count. Graded raw levels (+2 / +32 / +64) spread the stream as well but let a plan
// take a detour of tens of tiles to avoid a corridor of friends (the 2-peer fixture's mover behind a column: 891 -> 1510 steps); +2 does not
// (918 steps) and the monotone staircases of a 4-connected flood leave the cold side free, so +2 is all the spreading needs.
//
//   * Fires for a wave of exactly ONE member on a plain move (the unit's order is 0x0f / 0x01; the commit's mode flag is non-zero: no
//     attack / merge) with a non-zero move_group_id, when at least SH_MIN_MATES OTHER own alive units of that group are walking (state
//     0x0f, plain move order, a path), replanning (0x0a / 0x0c) or stored (0x20..0x2b except 0x23: leaving / entering a storage).
//   * For each walking mate (unit index order; the planning unit is never a mate) the next SH_WINDOW tiles of its current path are
//     collected (the walker's own cursor rule: a spent leg is skipped once, heading 0 ends the path).
//   * Each counted tile (whatever its count) gets the raw cost bit 0x02 OR-ed into its cost byte: +2.
//   * The caller plans, then sh_close() puts every cost byte back exactly. Nothing survives the call (no save, no hash).
//
// It rides the route-through bracket: sh_open() runs after the rt_unmask_* calls, sh_close() before rt_restore() (LIFO), in the retail
// splice (mh/seams/move_proto_nudge.cpp, the stub at 0x0041c9ce; the flood reads the cost at 0x00424f7f) and in the libmh twin
// (sim_group_move_order_commit.cpp). The 2a re-run of the planner for a lone unit opens the same bracket.
//
// DETERMINISM: integer arithmetic only, units visited in index order from 1, fixed arrays, no clock, no rand; the same call on every peer.
//
//   struct World {
//       int32_t  unit_count();                                          // roster cap per player
//       bool     unit(uint32_t player, int32_t i, sh_unit *out);        // false: no such record
//       void     wp_get(uint32_t player, uint32_t slot, uint32_t entry, uint8_t *heading, uint8_t *run);
//       void     dir_delta(int heading, int *dx, int *dy);              // signed tile delta of a heading code 1..24
//       uint32_t &cell_flags(uint32_t flat);                            // region grid terrain_flags
//       int32_t  member_count();                                        // the wave being planned
//       uint32_t member_handle(int32_t m);                              // its unit index
//   };
//
#pragma once

#include <cstdint>

#include "sim/move/flood.h" // mods:M1WL v5: the margin rule re-floods the unheated field

namespace mh::sim::move {

constexpr int      SH_MIN_MATES      = 8;    // groupmates walking / replanning / stored before the rule fires (the key's default)
constexpr int      SH_WINDOW         = 120;  // path steps of each walking mate that are priced (the key's default; 48 until mods:M1WL v5)
constexpr int      SH_MAX_TILES      = 8192; // distinct priced tiles per plan (the rest are not priced)
constexpr int      SH_PATH_ENTRIES   = 299;  // waypoints per path row (the walker reads entry 299 as the end)
constexpr uint32_t SH_HEAT_BIT       = 0x02; // raw cost bit: +2 on the tile, untouched by the route table (see WHICH BIT above)
constexpr int      SH_COST_MAX       = 226;  // the largest legal cost (0xE2 = 128 + 64 + 32 + 2)
constexpr int      SH_WINDOW_MAX     = 255;
constexpr int      SH_MATES_MAX      = 255;
constexpr int      SH_TURN_MAX       = 64; // the largest turn penalty (cost units: an open tile costs 1)
constexpr int      SH_TURN_DEFAULT   = 4;  // [net] move_stream_heat_turn -1 (measured on the 200-unit field run)
constexpr int      SH_SCALE_CAP      = 16; // scale 1: the cost is multiplied by min(count, cap); the key's default (4 until mods:M1WL v5)
constexpr int      SH_SCALE_CAP_MAX  = 255;
constexpr int      SH_MARGIN_DEFAULT = 50; // [net] move_stream_heat_margin -1: a heated route may be 50% longer than the unheated one
constexpr int      SH_MARGIN_MAX     = 1000;
constexpr uint16_t SH_ST_WALK        = 0x0f;
constexpr uint16_t SH_ST_MARSHAL     = 0x0a;
constexpr uint16_t SH_ST_MOVE_HOP    = 0x0c;
constexpr uint16_t SH_ORDER_MOVE     = 0x0f;
constexpr uint16_t SH_ORDER_STOP     = 0x01;

// THE TUNING KEYS ([net] move_stream_heat_cost / _window / _min_mates / _scale). The defaults reproduce the shipped rule exactly.
//
// LEGAL COSTS. The cost byte the flood reads is `passable (0x01) + rings (0x04 / 0x08 / 0x10) + raw bits`; route_cost_calc maps the first
// two groups through the route table (scaled for a lone unit) and passes every other bit through RAW. The stored flags never carry
// 0x02 / 0x20 / 0x40 / 0x80, so those four bits are the only room for a cost that survives the table, and the heat is OR-ed into them (an OR
// puts back exactly; an ADD would carry into the passable / ring bits and be mapped by the table). The legal costs are therefore the sums of
// {2, 32, 64, 128}: 0, 2, 32, 34, 64, 66, ... 224, 226 (16 values). Any other value is rounded DOWN to the legal set (3 -> 2, 31 -> 2, 33 -> 32,
// 1000 -> 226). 0 prices nothing (the rule does not fire). A tile's own cost is 1 (open ground) to 29 (against an obstacle), so 2 triples an
// open tile, 32 is a 33-tile detour and 226 a wall.
//
// scale = 1 (the default since mods:M1WL v5): a tile walked by `count` groupmates costs legal(cost * min(count, scale_cap)), scale_cap 16 by default. With
// cost 2 the products 2 .. 30 all round down to 2 and 32 is reached at 16 mates: a tile 15 mates are about to walk costs +2, from 16 on +32 (graded heat).
// With cost 32 the tile costs 32 / 64 / 96 / 128 for 1 .. 4 mates.
inline int sh_legal_cost(int raw) {
    if (raw < 2) return 0;
    if (raw > SH_COST_MAX) raw = SH_COST_MAX;
    return (raw & 0xE0) | ((raw & 0x1F) >= 2 ? 0x02 : 0);
}

struct sh_params {
    int cost      = 2;            // legal cost per priced tile (sh_legal_cost)
    int window    = SH_WINDOW;    // path steps of each walking mate that are priced, 1 .. SH_WINDOW_MAX
    int min_mates = SH_MIN_MATES; // groupmates needed before the rule fires, 1 .. SH_MATES_MAX
    int scale     = 1;            // 0 flat, 1 cost x min(count, scale_cap) (mods:M1WL v5: on by default)
    int retrace   = 0;            // mods:M1WL v4: 1 = re-derive a heated plan's route charging the tile stepped onto (sh_retrace)
    int turn      = 0;            // mods:M1WL v4: extra cost of a retrace step that turns more than 45 degrees (0 .. SH_TURN_MAX)
    int scale_cap = SH_SCALE_CAP; // mods:M1WL v5: the mate count at which the scaled cost stops growing, 1 .. SH_SCALE_CAP_MAX
    int margin    = 0;            // mods:M1WL v5: percent; a heated route longer than (100 + margin)% of the unheated one is replaced by it (0 = off, sh_margin)
};

// The setters' clamps; mh.dll's hooks and the libmh twin both go through here, so the two images read one rule.
inline sh_params sh_params_clamped(int cost, int window, int min_mates, int scale, int retrace = 0, int turn = 0, int scale_cap = SH_SCALE_CAP, int margin = 0) {
    sh_params p;
    p.cost      = sh_legal_cost(cost);
    p.window    = window < 1 ? 1 : (window > SH_WINDOW_MAX ? SH_WINDOW_MAX : window);
    p.min_mates = min_mates < 1 ? 1 : (min_mates > SH_MATES_MAX ? SH_MATES_MAX : min_mates);
    p.scale     = scale != 0 ? 1 : 0;
    p.retrace   = retrace != 0 ? 1 : 0;
    p.turn      = turn < 0 ? 0 : (turn > SH_TURN_MAX ? SH_TURN_MAX : turn);
    p.scale_cap = scale_cap < 1 ? 1 : (scale_cap > SH_SCALE_CAP_MAX ? SH_SCALE_CAP_MAX : scale_cap);
    p.margin    = margin < 0 ? 0 : (margin > SH_MARGIN_MAX ? SH_MARGIN_MAX : margin);
    return p;
}

// The cost a tile walked by `count` mates pays under `p`.
inline uint32_t sh_tile_cost(const sh_params &p, int count) {
    if (p.scale == 0) return static_cast<uint32_t>(p.cost);
    const int m = count < p.scale_cap ? count : p.scale_cap;
    return static_cast<uint32_t>(sh_legal_cost(p.cost * (m < 1 ? 1 : m)));
}

struct sh_unit {
    uint16_t order = 0, state = 0;
    int32_t  group = 0; // move_group_id
    bool     alive = false;
    uint8_t  x = 0, y = 0;
    uint8_t  path_slot   = 0xff; // 0xff = no usable path
    int32_t  path_cursor = 0;
};

// One plan's scratch: the tiles collected so far, the priced tiles and the cost bytes to put back.
struct sh_state {
    uint8_t  seen[65536]; // mates walking the tile (0 = not on the list; saturates at 255; cleared again by sh_open)
    uint32_t flat[SH_MAX_TILES];
    uint8_t  low[SH_MAX_TILES];
    uint8_t  cnt[SH_MAX_TILES]; // mates walking flat[k], 0 when the tile was left alone (cost byte 0): what the display reads
    int      n     = 0;         // tiles priced (the undo list)
    int      mates = 0;         // groupmates that satisfied the gate
    int      tiles = 0;         // distinct tiles collected
    sh_state() {
        for (int i = 0; i < 65536; ++i) seen[i] = 0;
    }
};

// Returns the number of tiles priced (0 = the rule did not fire; nothing was touched). `group_id` / `plain` are the commit's move_group_id
// and its is_plain_move_flag; `owner` the order's owner; the masks wrap the tile coordinates (map width / height - 1).
template <class W>
inline int sh_open(W &w, sh_state &s, uint32_t owner, int32_t group_id, uint32_t plain, uint32_t mask_x, uint32_t mask_y,
                   const sh_params &p = sh_params()) {
    s.n     = 0;
    s.mates = 0;
    s.tiles = 0;
    if (group_id == 0 || plain == 0 || owner >= 8 || p.cost == 0 || w.member_count() != 1) return 0;
    const int32_t me = static_cast<int32_t>(w.member_handle(0));
    sh_unit       pu;
    if (me < 1 || !w.unit(owner, me, &pu) || (pu.order != SH_ORDER_MOVE && pu.order != SH_ORDER_STOP)) return 0;
    const int32_t cap = w.unit_count();

    // the gate: groupmates (not the planning unit) walking / replanning / stored with this group's order
    int mates = 0;
    for (int32_t i = 1; i < cap; ++i) {
        if (i == me) continue;
        sh_unit u;
        if (!w.unit(owner, i, &u) || !u.alive || u.group != group_id) continue;
        const bool walking = u.state == SH_ST_WALK && (u.order == SH_ORDER_MOVE || u.order == SH_ORDER_STOP);
        const bool replan  = u.state == SH_ST_MARSHAL || u.state == SH_ST_MOVE_HOP;
        const bool stored  = u.state >= 0x20 && u.state <= 0x2b && u.state != 0x23;
        if (walking || replan || stored) ++mates;
    }
    s.mates = mates;
    if (mates < p.min_mates) return 0;

    // count the next SH_WINDOW tiles of every walking mate's path
    int ntile = 0;
    for (int32_t i = 1; i < cap; ++i) {
        if (i == me) continue;
        sh_unit u;
        if (!w.unit(owner, i, &u) || !u.alive || u.group != group_id) continue;
        if (u.state != SH_ST_WALK || (u.order != SH_ORDER_MOVE && u.order != SH_ORDER_STOP) || u.path_slot == 0xff) continue;
        int e = u.path_cursor;
        if (e < 0 || e >= SH_PATH_ENTRIES) continue;
        uint8_t h, r;
        w.wp_get(owner, u.path_slot, static_cast<uint32_t>(e), &h, &r);
        if (r == 0) ++e; // a spent leg, skipped once like the walker does
        uint32_t x = u.x, y = u.y;
        int      steps = 0;
        for (; e < SH_PATH_ENTRIES && steps < p.window; ++e) {
            w.wp_get(owner, u.path_slot, static_cast<uint32_t>(e), &h, &r);
            if (h == 0) break;
            int dx = 0, dy = 0;
            if (h <= 24) w.dir_delta(h, &dx, &dy);
            for (int k = 0; k < r && steps < p.window; ++k, ++steps) {
                x                = (x + static_cast<uint32_t>(dx)) & mask_x;
                y                = (y + static_cast<uint32_t>(dy)) & mask_y;
                const uint32_t t = ((x & 0xffu) << 8) | (y & 0xffu);
                if (s.seen[t] == 0) {
                    if (ntile >= SH_MAX_TILES) continue;
                    s.seen[t]       = 1;
                    s.flat[ntile++] = t;
                } else if (s.seen[t] < 255) {
                    ++s.seen[t];
                }
            }
        }
    }
    s.tiles = ntile;

    // price the tiles (and clear the counts as they are consumed)
    for (int k = 0; k < ntile; ++k) {
        const uint32_t t = s.flat[k];
        const int      c = s.seen[t];
        s.seen[t]        = 0;
        uint32_t &f      = w.cell_flags(t);
        s.low[k]         = static_cast<uint8_t>(f);
        s.cnt[k]         = 0;
        if ((f & 0xffu) != 0) { // a cost byte of 0 is not a walkable cell for the flood: left alone
            f |= sh_tile_cost(p, c);
            s.cnt[k] = static_cast<uint8_t>(c);
        }
    }
    s.n = ntile;
    return ntile;
}

// Puts every cost byte back (the low byte only: the flood field above it is the planner's own).
template <class W>
inline void sh_close(W &w, sh_state &s) {
    for (int k = s.n - 1; k >= 0; --k) {
        uint32_t &f = w.cell_flags(s.flat[k]);
        f           = (f & 0xffffff00u) | s.low[k];
    }
    s.n = 0;
}

// ---- mods:M1WL v4: the RE-TRACE (why the heat alone is not enough, and the rule) -----------------------------------------------------------------
//
// WHY. The flood (0x00424ee0) writes d(n) = d(cur) + cost(cur): the distance of a tile EXCLUDES the tile's own cost. The route trace
// (llm_map_region_route_search 0x004236c2) steps to the neighbour with the lowest d, so the cost of the tile it steps ONTO is never compared. A
// priced tile beside a free one has about the same d (usually one or two LESS: it lies on the old shortest line), and the trace takes it: rc9 measured
// 47 of 47 route tiles priced on a heated plan (19 of 45 unheated), 41 of 47 steps taking a priced tile (d 2164, cost 227) over a free one (d 2166, cost 2).
// The heat reached the flood but ATTRACTED the trace.
//
// THE RULE. After the flood and BEFORE the cost bytes are put back, a plan the heat fired for is re-traced from the same field and the same start /
// goal: at each step the candidate is the neighbour (same eight-neighbour order, same corner rule, same "strictly lower d" condition as route_search) with
// the lowest d(m) + cost(m) (cost = the byte value as the flood charges it, the heat included), plus `turn` when the step turns by more than 45 degrees
// from the previous step. The route is written back in the same run-length format. The first step has no previous heading.
//   * d strictly falls on every step, so the trace terminates and never steps back onto a tile (a reversal is impossible by construction);
//   * with every cost 0 and turn 0 the trace IS route_search: the rule first re-traces the plain way and refuses to replace the route when that does not
//     reproduce the planner's route entry by entry (a start / goal / table mismatch), so a corner the rule does not model leaves the plan alone;
//   * a route that would need more than SH_RT_MAX_RUNS runs is not replaced.
// Fires only when the heat fired (the caller passes the open sh_state). Integer only, fixed order, no RNG, nothing persistent.
//
//   World additions: route_step(i, &dx, &dy, &code)   the table llm_map_init_region_route_step_deltas fills (i 0..7: even = orthogonal) with its
//                                                     dir code (1, 4, 7 .. 22);
//                    cell_cost(low)                   the cost the flood adds for a cost byte (the route_cost table when that key is on);
//                    wp_put(player, slot, entry, heading, run).
constexpr int SH_RT_MAX_RUNS  = 290;
constexpr int SH_RT_MAX_STEPS = 2048;

enum sh_retrace_result : int {
    SHR_NONE     = 0, // nothing to do (the rule is off, the heat did not fire, no route)
    SHR_SAME     = 1, // re-traced, the route is the planner's own
    SHR_CHANGED  = 2, // re-traced and replaced
    SHR_BASE     = 3, // refused: the plain re-trace does not reproduce the planner's route
    SHR_NO_ROUTE = 4, // refused: stuck, or too long
};

// What one re-trace measured about the route it looked at (log / display only): tiles, tiles carrying the heat, runs, and turns of more than 45
// degrees, for the planner's route (before) and the re-traced one (after; = before when the route was left alone).
struct sh_retrace_info {
    int len_before = 0, len_after = 0;
    int heat_before = 0, heat_after = 0;
    int runs_before = 0, runs_after = 0;
    int turns_before = 0, turns_after = 0;
};

struct sh_retrace_stats {
    uint32_t plans   = 0; // re-trace attempts (the heat fired and the key is on)
    uint32_t changed = 0; // routes replaced
    uint32_t same    = 0;
    uint32_t refused = 0;
};

inline void sh_retrace_count(sh_retrace_stats &st, int result) {
    if (result == SHR_NONE) return;
    ++st.plans;
    if (result == SHR_CHANGED) ++st.changed;
    else if (result == SHR_SAME) ++st.same;
    else ++st.refused;
}

namespace detail {
struct rt_run {
    uint8_t head[SH_RT_MAX_RUNS + 2];
    uint8_t run[SH_RT_MAX_RUNS + 2];
    int     n = 0;
};

// One trace from (sx, sy) to (gx, gy) over the finished field. use_cost = false is route_search itself. False: stuck, or the route does not fit.
template <class W>
inline bool rt_trace(W &w, uint32_t sx, uint32_t sy, uint32_t gx, uint32_t gy, uint32_t mask_x, uint32_t mask_y, bool use_cost, int turn, rt_run *out) {
    int dxs[8], dys[8], code[8];
    for (int i = 0; i < 8; ++i) w.route_step(i, &dxs[i], &dys[i], &code[i]);
    auto flat_of = [&](uint32_t x, uint32_t y) { return ((x & 0xffu) << 8) | (y & 0xffu); };
    auto field   = [&](uint32_t flat) { return w.cell_flags(flat) >> 8; };
    out->n       = 0;
    uint32_t x = sx & mask_x, y = sy & mask_y;
    int      prev = -1, steps = 0, last_code = -1;
    while (x != (gx & mask_x) || y != (gy & mask_y)) {
        if (++steps > SH_RT_MAX_STEPS) return false;
        const uint32_t curv = field(flat_of(x, y));
        uint32_t       cx[8], cy[8];
        for (int i = 0; i < 8; ++i) {
            cx[i] = (x + static_cast<uint32_t>(dxs[i])) & mask_x;
            cy[i] = (y + static_cast<uint32_t>(dys[i])) & mask_y;
        }
        int      best      = -1;
        uint32_t bestscore = 0;
        for (int pass = 0; pass < 2; ++pass) {
            for (int i = pass; i < 8; i += 2) {
                const uint32_t f = flat_of(cx[i], cy[i]);
                const uint32_t v = field(f);
                if (v == 0 || v >= curv) continue;
                if (pass == 1) { // a diagonal needs one open flank (route_search's second loop)
                    const int a = i - 1, b = (i + 1) & 7;
                    if (field(flat_of(cx[a], cy[a])) == 0 && field(flat_of(cx[b], cy[b])) == 0) continue;
                }
                uint32_t score = v;
                if (use_cost) {
                    score += w.cell_cost(w.cell_flags(f) & 0xffu);
                    if (prev >= 0 && turn > 0) {
                        int d = (i - prev) & 7;
                        if (d > 4) d = 8 - d;
                        if (d >= 2) score += static_cast<uint32_t>(turn);
                    }
                }
                if (best < 0 || score < bestscore) {
                    best      = i;
                    bestscore = score;
                }
            }
        }
        if (best < 0) return false;
        x    = cx[best];
        y    = cy[best];
        prev = best;
        if (out->n > 0 && last_code == code[best] && out->run[out->n - 1] < 255) {
            ++out->run[out->n - 1];
        } else {
            if (out->n >= SH_RT_MAX_RUNS) return false;
            out->head[out->n] = static_cast<uint8_t>(code[best]);
            out->run[out->n]  = 1;
            ++out->n;
        }
        last_code = code[best];
    }
    return true;
}

// tiles / heat-carrying tiles / turns of more than 45 degrees of a run list walked from (sx, sy). Display only.
template <class W>
inline void rt_measure(W &w, uint32_t sx, uint32_t sy, const rt_run &r, uint32_t mask_x, uint32_t mask_y, int *len, int *heat, int *runs, int *turns) {
    uint32_t x = sx & mask_x, y = sy & mask_y;
    *len = *heat = 0;
    *runs        = r.n;
    *turns       = 0;
    for (int k = 0; k < r.n; ++k) {
        int dx = 0, dy = 0;
        w.dir_delta(r.head[k], &dx, &dy);
        if (k > 0) {
            int d = ((r.head[k] - 1) / 3 - (r.head[k - 1] - 1) / 3) & 7;
            if (d > 4) d = 8 - d;
            if (d >= 2) ++*turns;
        }
        for (int j = 0; j < r.run[k]; ++j) {
            x = (x + static_cast<uint32_t>(dx)) & mask_x;
            y = (y + static_cast<uint32_t>(dy)) & mask_y;
            ++*len;
            if ((w.cell_flags(((x & 0xffu) << 8) | (y & 0xffu)) & 0xE2u) != 0) ++*heat; // a tile's own cost byte never carries 0x02 / 0x20 / 0x40 / 0x80
        }
    }
}
} // namespace detail

// Call between sh_open() (fired: s.n > 0) and sh_close(), after the planner returned.
template <class W>
inline int sh_retrace(W &w, const sh_state &s, uint32_t owner, uint32_t mask_x, uint32_t mask_y, const sh_params &p, sh_retrace_info *info = nullptr) {
    if (!p.retrace || s.n <= 0 || owner >= 8 || w.member_count() != 1) return SHR_NONE;
    const int32_t me = static_cast<int32_t>(w.member_handle(0));
    sh_unit       pu;
    if (me < 1 || !w.unit(owner, me, &pu) || pu.path_slot == 0xff) return SHR_NONE;

    // the planner's own route: read the runs, find the goal
    detail::rt_run orig;
    uint32_t       gx = pu.x & mask_x, gy = pu.y & mask_y;
    for (int e = 0; e < SH_RT_MAX_RUNS; ++e) {
        uint8_t h, r;
        w.wp_get(owner, pu.path_slot, static_cast<uint32_t>(e), &h, &r);
        if (h == 0 || r == 0) break;
        if (h > 24) return SHR_NO_ROUTE;
        int dx = 0, dy = 0;
        w.dir_delta(h, &dx, &dy);
        gx                = (gx + static_cast<uint32_t>(dx) * r) & mask_x;
        gy                = (gy + static_cast<uint32_t>(dy) * r) & mask_y;
        orig.head[orig.n] = h;
        orig.run[orig.n]  = r;
        ++orig.n;
    }
    if (orig.n == 0) return SHR_NONE; // already at the goal

    detail::rt_run base;
    if (!detail::rt_trace(w, pu.x, pu.y, gx, gy, mask_x, mask_y, false, 0, &base) || base.n != orig.n) return SHR_BASE;
    for (int k = 0; k < orig.n; ++k)
        if (base.head[k] != orig.head[k] || base.run[k] != orig.run[k]) return SHR_BASE;

    detail::rt_run re;
    if (!detail::rt_trace(w, pu.x, pu.y, gx, gy, mask_x, mask_y, true, p.turn, &re)) return SHR_NO_ROUTE;
    bool same = re.n == orig.n;
    for (int k = 0; same && k < orig.n; ++k)
        if (re.head[k] != orig.head[k] || re.run[k] != orig.run[k]) same = false;
    if (info) {
        detail::rt_measure(w, pu.x, pu.y, orig, mask_x, mask_y, &info->len_before, &info->heat_before, &info->runs_before, &info->turns_before);
        detail::rt_measure(w, pu.x, pu.y, re, mask_x, mask_y, &info->len_after, &info->heat_after, &info->runs_after, &info->turns_after);
    }
    if (same) return SHR_SAME;
    for (int k = 0; k < re.n; ++k) w.wp_put(owner, pu.path_slot, static_cast<uint32_t>(k), re.head[k], re.run[k]);
    w.wp_put(owner, pu.path_slot, static_cast<uint32_t>(re.n), 0, 0);
    return SHR_CHANGED;
}

// ---- mods:M1WL v5: the MARGIN (a heated route may not be much longer than the unheated one) ---------------------------------------------------
//
// WHY. The heat prices tiles, not lengths: a plan through a gap with 7-12 priced tiles at +32 each finds a 25-53 tile loop cheaper than the 11-16 tile
// way through (rc10 Q2: the user's narrow-path routes made 4-5 detours of ~30 tiles under cost 32). The replan detour cap refuses such a plan three
// times and then accepts it.
//
// THE RULE. After the flood, the re-trace and BEFORE the cost bytes are put back, a plan the heat fired for is compared with the plan it would have been
// WITHOUT the heat: the priced tiles get their own cost bytes back, the eligibility field is cleared to "eligible, unreached" (phase 1's result: every
// non-zero field becomes 0xffffff), the exact flood (flood.h; the retail weights of a wave of one, the goal as seed) runs again and the plain trace
// (route_search's rule) walks it from the unit's tile. If the route the unit holds is longer than (100 + margin)% of that route (tiles, compared in
// integers), the unheated route is written instead. Every cell word is then put back to what it was on entry (the heated field), so sh_close() and the
// caller see no difference.
//   * No cheaper way to get the unheated length exists: the heated field cannot be turned back (the flood adds the cost of the tile it leaves, which
//     the heat changed), and the planner is a retail function that runs once. The cost is one more exact flood per heated plan (a few hundred microseconds).
//   * The rule refuses to act (SHM_SKIP) when the goal tile does not hold the flood's seed distance 1 or the plain trace fails or needs more than
//     SH_RT_MAX_RUNS runs: the heated route stands.
//   * The flood is the exact (bucket) one. When the retail FIFO flood lapped its ring (a 59k-cell front on an open map) its field is not the exact
//     shortest-path field, so the unheated length is the TRUE shortest length; the comparison is then only more honest.
//
//   World additions: flood_wrap_mask()   the region coordinate wrap mask the flood uses (0x00708b1c on the retail image).
//   (cell_cost, route_step, wp_put and cell_flags are the re-trace's.)
enum sh_margin_result : int {
    SHM_NONE   = 0, // nothing to do (the rule is off, the heat did not fire, no route)
    SHM_WITHIN = 1, // the heated route is within the margin: left alone
    SHM_KEPT   = 2, // the heated route was longer than the margin: the unheated route was written
    SHM_SKIP   = 3, // not decidable (goal not the seed, or no plain trace): left alone
};

struct sh_margin_info {
    int len_heated = 0; // tiles of the route the unit held
    int len_plain  = 0; // tiles of the unheated route (0 when SHM_SKIP / SHM_NONE)
};

struct sh_margin_stats {
    uint32_t checks  = 0; // plans compared (SHM_WITHIN + SHM_KEPT + SHM_SKIP)
    uint32_t rewrote = 0; // SHM_KEPT: "margin kept"
    uint32_t skipped = 0;
};

inline void sh_margin_count(sh_margin_stats &st, int result) {
    if (result == SHM_NONE) return;
    ++st.checks;
    if (result == SHM_KEPT) ++st.rewrote;
    else if (result == SHM_SKIP) ++st.skipped;
}

namespace detail {
struct margin_scratch {
    uint32_t snap[65536]; // every cell word on entry (working memory: not saved, not hashed)
};
inline margin_scratch &margin_buf() {
    static margin_scratch s;
    return s;
}
inline int rt_tiles(const rt_run &r) {
    int n = 0;
    for (int k = 0; k < r.n; ++k) n += r.run[k];
    return n;
}
template <class W>
struct margin_cells {
    W        &w;
    uint32_t &flags(uint32_t flat) { return w.cell_flags(flat); }
};
} // namespace detail

// Call between sh_open() (fired: s.n > 0) and sh_close(), after the planner (and the re-trace) ran.
template <class W>
inline int sh_margin(W &w, const sh_state &s, uint32_t owner, uint32_t mask_x, uint32_t mask_y, const sh_params &p, sh_margin_info *info = nullptr) {
    if (p.margin <= 0 || s.n <= 0 || owner >= 8 || w.member_count() != 1) return SHM_NONE;
    const int32_t me = static_cast<int32_t>(w.member_handle(0));
    sh_unit       pu;
    if (me < 1 || !w.unit(owner, me, &pu) || pu.path_slot == 0xff) return SHM_NONE;

    // the route the unit holds, and its goal
    detail::rt_run cur;
    uint32_t       gx = pu.x & mask_x, gy = pu.y & mask_y;
    for (int e = 0; e < SH_RT_MAX_RUNS; ++e) {
        uint8_t h, r;
        w.wp_get(owner, pu.path_slot, static_cast<uint32_t>(e), &h, &r);
        if (h == 0 || r == 0) break;
        if (h > 24) return SHM_SKIP;
        int dx = 0, dy = 0;
        w.dir_delta(h, &dx, &dy);
        gx              = (gx + static_cast<uint32_t>(dx) * r) & mask_x;
        gy              = (gy + static_cast<uint32_t>(dy) * r) & mask_y;
        cur.head[cur.n] = h;
        cur.run[cur.n]  = r;
        ++cur.n;
    }
    if (cur.n == 0) return SHM_NONE; // already at the goal
    const int      len_heated = detail::rt_tiles(cur);
    const uint32_t goal       = ((gx & 0xffu) << 8) | (gy & 0xffu);
    if (info) info->len_heated = len_heated;

    detail::margin_scratch &sc = detail::margin_buf();
    for (uint32_t f = 0; f < 65536u; ++f) sc.snap[f] = w.cell_flags(f);
    int result = SHM_SKIP;
    if ((sc.snap[goal] >> 8) == 1u) { // the seed of the flood the planner ran
        // the unheated state: every priced tile's own cost byte back, every eligible cell unreached
        for (int k = 0; k < s.n; ++k) {
            uint32_t &f = w.cell_flags(s.flat[k]);
            f           = (f & 0xffffff00u) | s.low[k];
        }
        for (uint32_t f = 0; f < 65536u; ++f) {
            if ((sc.snap[f] >> 8) != 0) {
                uint32_t &c = w.cell_flags(f);
                c           = 0xffffff00u | (c & 0xffu);
            }
        }
        uint32_t cost[FLOOD_COST_ENTRIES];
        for (uint32_t lo = 0; lo < FLOOD_COST_ENTRIES; ++lo) cost[lo] = w.cell_cost(lo);
        detail::margin_cells<W> cells{w};
        flood_exact(cells, goal, w.flood_wrap_mask(), cost);
        detail::rt_run plain;
        if (detail::rt_trace(w, pu.x, pu.y, gx, gy, mask_x, mask_y, false, 0, &plain)) {
            const int len_plain = detail::rt_tiles(plain);
            if (info) info->len_plain = len_plain;
            result = SHM_WITHIN;
            if (static_cast<int64_t>(len_heated) * 100 > static_cast<int64_t>(len_plain) * (100 + p.margin)) result = SHM_KEPT;
            if (result == SHM_KEPT) {
                // the cell words go back first; the route is written after (it does not touch them, the order is only tidiness)
                for (uint32_t f = 0; f < 65536u; ++f) w.cell_flags(f) = sc.snap[f];
                for (int k = 0; k < plain.n; ++k) w.wp_put(owner, pu.path_slot, static_cast<uint32_t>(k), plain.head[k], plain.run[k]);
                w.wp_put(owner, pu.path_slot, static_cast<uint32_t>(plain.n), 0, 0);
                return SHM_KEPT;
            }
        }
        for (uint32_t f = 0; f < 65536u; ++f) w.cell_flags(f) = sc.snap[f];
    }
    return result;
}

} // namespace mh::sim::move
