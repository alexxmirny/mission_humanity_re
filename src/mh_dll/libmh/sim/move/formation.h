//
// sim/move/formation.h -- mods:M1G 4b: [net] move_formation_click, "formation from the click", the pure decision.
//
// WHAT IT FIXES (rc16/rc17 playtests; design: tmp/rc17dig/design_4b.txt, measurements: tmp/rc17dig/tables.md).
//   * In-place overshoot: retail makes a wave's goal tiles `goal - (centroid - member)`, the selection's shape moved rigidly onto the
//     click, so a click on (or next to) the blob shifts every member by click minus the truncated centroid, and a blob split into
//     several waves is re-centred once per wave. Here a member that already stands on a tile of the click's target set STAYS there.
//   * Cause A for plain group moves: every member is planned inside the route-through bracket (move/route_through.h), and 2a's
//     unbracketed single re-run (move/arrival.h) does not run for these commits.
//   * Empty offset walks: a member whose rigid offset walk is empty (retail "tiles = 0") gets its own route to its own tile.
// NOT fixed here: cause C (earlier waves in hop state 0x0c count as walls in the planner); a separate key, see section 9 of the design.
//
// WHERE IT RUNS. Inside the route-through bracket of group_move_order_commit, right after the unchanged retail plan (phase 1) came back
// from llm_strat_group_move_order_pathfind: retail splice = the 0x0041c9ce stub (mh/seams/move_proto_nudge.cpp, fc_after_pathfind);
// libmh twin = sim_group_move_order_commit.cpp. Handled = the caller skips 2a (arrival_after_pathfind) for this commit.
//
// THE RULE (all integer; ring order = Chebyshev ring, inside a ring (dx*dx+dy*dy, dx, dy) ascending, signed dx/dy; no wrap):
//   * Eligible: the wave is every member of the commit, every member's order is plain (0x0f / 0x01), the group id is not 0, mode != 0,
//     1 <= n <= 100. Anything else returns false and the caller runs 2a as before (Ctrl-click, attack and merge stay retail).
//   * Tile T is eligible when: its passable byte BEFORE the route-through bracket opened it is not 0; its occupant is nobody or a member
//     of this wave; the owner has discovered it; it is reachable (the flood just seeded at the goal G reaches it, else the same nav
//     region as G); no same-group non-member unit holds it as a goal (taken rule of arrival.h); and it is not the current tile of a
//     same-group non-member unit in 0x0a whose goal is the click K (a later wave of this order).
//   * Rings are centred on K (the click) when K is itself a passable, discovered, reachable tile, else on G (phase 1's effective goal).
//   * n == 1: the member's own unit.goal if that tile is eligible, else the first eligible tile in ring order.
//   * n > 1: rk = the smallest ring whose cumulative eligible count reaches n. KEEP (unit-index order): a member whose current tile is
//     eligible, within rk + slack and unclaimed keeps it. FILL: S = the first (unkept) unclaimed eligible tiles in ring order. Members
//     <-> S by the Hungarian algorithm minimising the sum of squared distances (rows = unkept members in unit-index order, columns = S
//     in ring order; padded with zero rows/columns when the sides differ; a member that lands on a pad column keeps its retail tile
//     and path).
//   * Plans, inside the open bracket: a kept member's path is made EMPTY (so the marshal's on-goal stop ends it at once); a member whose
//     phase-1 tile is its slot and whose phase-1 path is non-empty keeps that path; every other member is planned alone (count 1) to
//     its tile. The wave scratch (member 0, count, goal, centroid) is restored; MEMBER_TILE and each unit's goal are the final tiles.
//
// KEY 3, "THE FORMATION MOVES AS ONE" (rc18 dig 2026-10-19; spec tmp/rc18dig/spec.md; user decisions Q1-Q4 all "A"). The KEEP step above is replaced for slack >= FC_RULE_MOVE_AS_ONE
// (the callers pass key - 1, the key clamps to 1..3; 1..2 stay the keep rule). Everything else (eligibility, ring order, the Hungarian step, the plans) is shared.
//   * The plan covers the ORDER: this wave plus its queued groupmates (same group, state 0x0a, goal == click). A queued member's target is STAMPED into unit.goal; a later wave finds its
//     members already planned (goal != click) and keeps the stamped tile when it is still eligible. Retail never reads a queued unit's goal before its wave commits (the marshal's peer scan runs once,
//     before the first wave), so the stamp is safe and no per-wave recompute is needed.
//   * A tile whose occupant is a unit of this order (a wave member, a queued one, or an earlier wave: same group id) is eligible without the passable test: that unit walks on. The queued
//     groupmates' tiles are therefore not reserved.
//   * Members still unplanned (F): n == 1 -> own goal if eligible else the first ring tile. Otherwise positions are unwrapped around the first member (the centroid fix's wrap rule),
//     the CORE is every member within R0(n) + 1 (Chebyshev) of the upper-median centre (all members when it holds under half), COMPACT iff 2 M(core) <= 3 I(|core|) (alpha 1.5, int64 moments).
//     SHAPE: d = trunc((m K - S) / m) over the core; in unit-index order a core member whose p + d is eligible and unclaimed is FIXED to it, the rest take the first unclaimed ring tiles
//     (Hungarian on d^2). GATHER: a w x h block (w = ceil(sqrt n)) on the click, its n tiles nearest the click in ring order, ineligible ones dropped and refilled from ring order, Hungarian for all.
//
// PATH-DISTANCE RANKING (key 3 only; 0.2.1 hotfix, rc21 report "formation in tight places leads to detours"). The ring above is GEOMETRIC, so a tile one step away across a
// thin wall (30 steps by path) ranked as near and members were sent round the wall. Key 3 now measures a path distance D(t) from the centre over the FC_SPAN window: a BFS in unit
// steps, 8-connected, whose passable set is the tiles `eligible` would accept ignoring the taken / claimed filters (passable before the bracket or a tile of this order's own
// units, discovered, reachable; a tile held as another unit's goal is NOT a wall), a diagonal step needing one open orthogonal flank (the route planner's own rule).
// Unit steps, not 2/3 weights: in open ground D == the Chebyshev ring, so every open-ground result stays bit-identical to the geometric order; weights would make the bands
// diamonds and reshuffle open ground. The window wraps on the torus. Tiles are ranked by (D, ring, d2, dx, dy); rk = the D band that reaches the order's size; a tile the window
// BFS cannot reach (needs a detour beyond FC_R_MAX) sorts last (D = 255). SHAPE / GATHER accept a tile only when D <= ring + FC_PATH_SLACK, else that member takes the refill.
//
// COST. Up to one single-member planner run per member (n <= 100) on top of the retail plan; measurement and the cap decision:
// tmp/m1g4b/COST.md. The Hungarian step is O(n^3), ~1e6 operations at n = 100.
//
// DETERMINISM: integer arithmetic only, units in index order, fixed scan order, no clock, no rand. Scratch: ~12 KB of stack plus ONE 40 KB
// static cost matrix (the retail exe's stack reserve is 105 KB, so the big array is not on it). The only writes are the group-move scratch
// globals, the members' goals and path slots, and what the single-member planner runs write (the same set the retail plan writes).
//
//   World = arrival.h's World, plus (all take/return plain integers):
//       bool     true_passable(uint32_t t);                   // passable[t] as it was BEFORE the route-through bracket opened it
//       int32_t  occupant(uint32_t owner, uint32_t t);        // 0 = nobody, u >= 1 = own unit u, -1 = anything else (a unit mark, a building)
//       bool     unit(uint32_t owner, uint32_t i, arr_unit *out);       // arr_unit now also carries x, y and the path slot
//       void     set_member_count(int32_t n);
//       void     set_member(int32_t m, uint32_t handle, uint8_t col, uint8_t row);
//       void     set_centroid(uint32_t x, uint32_t y);
//       void     run_pathfind(uint32_t mode, uint32_t target);   // the planner, called directly (the bracket is already open)
//       uint32_t path_first_run(uint32_t owner, uint32_t slot);  // path buffer entry 0's run_length
//       void     path_terminate(uint32_t owner, uint32_t slot);  // path buffer entry 0 := {0, 0}
//       void     log_fc(uint32_t owner, int n, int kept, int filled, int retail, int rk, int replans, int reused);
//       void     log_assign(uint32_t owner, uint32_t unit, uint32_t from, uint32_t to, int how);   // how: 0 retail tile, 1 kept, 2 assigned
//       void     log_adjust(uint32_t owner, uint32_t unit, uint32_t wanted, uint32_t got);   // the planner moved a member's goal tile
//
#pragma once

#include <cstdint>

#include "sim/move/arrival.h"

namespace mh::sim::move {

constexpr int FC_R_MAX            = 16;               // ring search radius around the centre
constexpr int FC_SPAN             = 2 * FC_R_MAX + 1; // the ring window is FC_SPAN x FC_SPAN tiles
constexpr int FC_MAX_MEMBERS      = 100;              // unit_limit of the stock roster (move_proto_arrival.cpp); PT-CAP9 must widen it
constexpr int FC_MAX_TILES        = FC_SPAN * FC_SPAN;
constexpr int FC_MAX_SLACK        = 1; // the OLD keep rule (key 1..2): slack = key - 1
constexpr int FC_RULE_MOVE_AS_ONE = 2; // the `slack` argument of the NEW rule (key 3, "the formation moves as one"); the key clamps to 1..3
constexpr int FC_PATH_SLACK       = 2; // key 3: a SHAPE / GATHER tile is accepted only when its path distance from the centre is <= its Chebyshev distance + this
constexpr int FC_REPLAN_CAP       = 0; // 0 = every member that needs a route gets its own planner run; N > 0 caps the runs per commit

namespace formation_detail {

// e-maxx Hungarian, square N x N (row-major int32 cost), 1-based potentials. assign[row] = column. Strict `<` throughout, so ties resolve
// by (row order, column order) exactly as the offline prototype (scripts/sim4b.py) does.
inline void hungarian(int N, const int32_t *cost, int *assign) {
    constexpr int64_t INF = int64_t(1) << 60;
    int64_t           u[FC_MAX_MEMBERS + 1], v[FC_MAX_MEMBERS + 1], minv[FC_MAX_MEMBERS + 1];
    int               p[FC_MAX_MEMBERS + 1], way[FC_MAX_MEMBERS + 1];
    bool              used[FC_MAX_MEMBERS + 1];
    for (int i = 0; i <= N; ++i) u[i] = v[i] = 0, p[i] = way[i] = 0;
    for (int i = 1; i <= N; ++i) {
        p[0]   = i;
        int j0 = 0;
        for (int j = 0; j <= N; ++j) minv[j] = INF, used[j] = false;
        for (;;) {
            used[j0]        = true;
            const int i0    = p[j0];
            int64_t   delta = INF;
            int       j1    = 0;
            for (int j = 1; j <= N; ++j) {
                if (used[j]) continue;
                const int64_t cur = int64_t(cost[(i0 - 1) * N + (j - 1)]) - u[i0] - v[j];
                if (cur < minv[j]) minv[j] = cur, way[j] = j0;
                if (minv[j] < delta) delta = minv[j], j1 = j;
            }
            for (int j = 0; j <= N; ++j) {
                if (used[j]) u[p[j]] += delta, v[j] -= delta;
                else minv[j] -= delta;
            }
            j0 = j1;
            if (p[j0] == 0) break;
        }
        for (;;) {
            const int j1 = way[j0];
            p[j0]        = p[j1];
            j0           = j1;
            if (j0 == 0) break;
        }
    }
    for (int j = 1; j <= N; ++j) assign[p[j] - 1] = j - 1;
}

struct ring_entry {
    int32_t d2;
    int8_t  dx, dy;
};

// The tiles of ring r around the centre in ring order (d2, dx, dy ascending). Returns the count (<= 8r, 1 for r = 0).
inline int ring_order(int r, ring_entry *out) {
    int n = 0;
    for (int dx = -r; dx <= r; ++dx)
        for (int dy = -r; dy <= r; ++dy) {
            if (dx != -r && dx != r && dy != -r && dy != r) continue;
            out[n++] = ring_entry{dx * dx + dy * dy, static_cast<int8_t>(dx), static_cast<int8_t>(dy)};
        }
    for (int i = 1; i < n; ++i) { // insertion sort: stable, deterministic, n <= 128
        const ring_entry e = out[i];
        int              j = i - 1;
        while (j >= 0 && (out[j].d2 > e.d2 || (out[j].d2 == e.d2 && (out[j].dx > e.dx || (out[j].dx == e.dx && out[j].dy > e.dy))))) {
            out[j + 1] = out[j];
            --j;
        }
        out[j + 1] = e;
    }
    return n;
}


inline int64_t sqr64(int64_t a) { return a * a; }
inline int32_t abs32(int32_t a) { return a < 0 ? -a : a; }

// a / b rounded toward zero, b > 0
inline int32_t trunc_div(int64_t a, int32_t b) {
    const int64_t q = (a < 0 ? -a : a) / b;
    return static_cast<int32_t>(a < 0 ? -q : q);
}

// wrap-aware a - b on a power-of-two torus (the centroid fix's rule): the result lies in [-W/2, W/2)
inline int32_t wrap_delta(int32_t a, int32_t b, uint32_t mask) {
    const int32_t W = static_cast<int32_t>(mask) + 1;
    int32_t       d = static_cast<int32_t>((static_cast<uint32_t>(a - b)) & mask);
    if (d >= W / 2) d -= W;
    return d;
}

// smallest r with (2r+1)^2 >= n
inline int ideal_radius(int n) {
    int r = 0;
    while ((2 * r + 1) * (2 * r + 1) < n) ++r;
    return r;
}

// M(S) = sum over S of (m*x - Sx)^2 + (m*y - Sy)^2, m = |S| (int64)
inline int64_t moment_of(int m, const int32_t *xs, const int32_t *ys) {
    int64_t sx = 0, sy = 0;
    for (int i = 0; i < m; ++i) sx += xs[i], sy += ys[i];
    int64_t M = 0;
    for (int i = 0; i < m; ++i) M += sqr64(m * int64_t(xs[i]) - sx) + sqr64(m * int64_t(ys[i]) - sy);
    return M;
}

// I(m) = the moment of the first m tiles in ring order around (0,0): the most compact blob of m units (the ideal)
inline int64_t ideal_moment(int m) {
    int32_t    xs[FC_MAX_MEMBERS], ys[FC_MAX_MEMBERS];
    ring_entry ring[8 * 16 + 1];
    int        k = 0;
    for (int r = 0; k < m && r <= 16; ++r) {
        const int cnt = ring_order(r, ring);
        for (int e = 0; e < cnt && k < m; ++e) xs[k] = ring[e].dx, ys[k] = ring[e].dy, ++k;
    }
    return moment_of(m, xs, ys);
}

// The SHAPE / GATHER decision of the move-as-one rule over the members' (unwrapped) tiles: fills core[] (1 = in the core) and returns compact.
// med = the upper median centre; *out_m = |core|; `sx`/`sy` = the core's coordinate sums.
inline bool core_compact(int n, const int32_t *x, const int32_t *y, uint8_t *core, int *out_m, int64_t *out_sx, int64_t *out_sy) {
    int32_t xs[FC_MAX_MEMBERS], ys[FC_MAX_MEMBERS];
    for (int i = 0; i < n; ++i) xs[i] = x[i], ys[i] = y[i];
    for (int i = 1; i < n; ++i) { // insertion sort
        const int32_t a = xs[i], b = ys[i];
        int           j = i - 1;
        while (j >= 0 && xs[j] > a) xs[j + 1] = xs[j], --j;
        xs[j + 1] = a;
        j         = i - 1;
        while (j >= 0 && ys[j] > b) ys[j + 1] = ys[j], --j;
        ys[j + 1] = b;
    }
    const int32_t mx = xs[n / 2], my = ys[n / 2];
    const int     rc = ideal_radius(n) + 1;
    int           m  = 0;
    for (int i = 0; i < n; ++i) {
        const int32_t dx = abs32(x[i] - mx), dy = abs32(y[i] - my);
        core[i] = (dx > dy ? dx : dy) <= rc ? 1 : 0;
        m += core[i];
    }
    if (2 * m < n) {
        for (int i = 0; i < n; ++i) core[i] = 1;
        m = n;
    }
    int32_t cx[FC_MAX_MEMBERS], cy[FC_MAX_MEMBERS];
    int     k  = 0;
    int64_t sx = 0, sy = 0;
    for (int i = 0; i < n; ++i)
        if (core[i]) cx[k] = x[i], cy[k] = y[i], sx += x[i], sy += y[i], ++k;
    *out_m  = m;
    *out_sx = sx;
    *out_sy = sy;
    return 2 * moment_of(m, cx, cy) <= 3 * ideal_moment(m); // alpha 1.5
}

} // namespace formation_detail

// Runs right after the retail plan returned inside the open route-through bracket. `slack` = keep slack in rings (the key's N - 1).
// Returns true = handled (the caller skips 2a).
template <class W>
inline bool formation_after_pathfind(W &w, int32_t group_id, uint32_t mode, uint32_t target, int slack) {
    using namespace arrival_detail;
    using namespace formation_detail;
    if (group_id == 0 || mode == 0) return false;
    const int32_t n = w.member_count();
    if (n < 1 || n > FC_MAX_MEMBERS) return false;
    const uint32_t owner    = w.owner() & 7u;
    const int32_t  unit_cap = w.unit_limit();
    // slack >= FC_RULE_MOVE_AS_ONE = the move-as-one rule (key 3); 0..FC_MAX_SLACK = the old keep rule (key 1..2)
    const bool rule3 = slack >= FC_RULE_MOVE_AS_ONE;
    slack            = slack < 0 ? 0 : (slack > FC_MAX_SLACK ? FC_MAX_SLACK : slack);

    // ---- the wave: handles and current tiles; every member must be on a plain move ---------------------------------------------
    uint32_t hdl[FC_MAX_MEMBERS];
    uint8_t  mcol[FC_MAX_MEMBERS], mrow[FC_MAX_MEMBERS];
    uint32_t mgoal[FC_MAX_MEMBERS]; // the member's own unit.goal when the commit started: stamped by an earlier wave of the move-as-one rule, else the click
    for (int32_t m = 0; m < n; ++m) {
        hdl[m]  = w.member_handle(m);
        mcol[m] = w.member_col(m);
        mrow[m] = w.member_row(m);
        arr_unit u;
        if (hdl[m] < 1 || hdl[m] >= static_cast<uint32_t>(unit_cap) || !w.unit(owner, hdl[m], &u) || !is_plain_move(u.order)) return false;
        mgoal[m] = tidx(u.goal_x, u.goal_y);
    }
    auto member_of = [&](int32_t unit_idx) {
        for (int32_t m = 0; m < n; ++m)
            if (hdl[m] == static_cast<uint32_t>(unit_idx)) return true;
        return false;
    };

    // ---- one commit's view ---------------------------------------------------------------------------------------------------
    pick_ctx c;
    c.mask            = w.path_mask() & 0xffu;
    c.owner_bit       = 1u << owner;
    const uint32_t gx = w.goal_x() & c.mask, gy = w.goal_y() & c.mask;
    const uint32_t kx = w.anchor_x() & c.mask, ky = w.anchor_y() & c.mask;
    const uint32_t g = tidx(gx, gy), click = tidx(kx, ky);
    c.field_fresh = w.field_of(g) == 1u;
    c.goal_region = w.region_of(g);
    c.n_taken     = 0;
    uint32_t reserved[ARR_MAX_TAKEN];
    int      n_reserved = 0;
    int      n_q        = 0; // the move-as-one rule plans the queued groupmates (later waves of this order) together with the wave
    uint32_t qh[FC_MAX_MEMBERS];
    uint8_t  qx[FC_MAX_MEMBERS], qy[FC_MAX_MEMBERS];
    for (int32_t i = 1; i < unit_cap; ++i) { // goals other units of this group hold; tiles a later wave of this order still stands on
        if (member_of(i)) continue;
        arr_unit u;
        if (!w.unit(owner, static_cast<uint32_t>(i), &u) || u.group != group_id) continue;
        const uint32_t goal = tidx(u.goal_x, u.goal_y);
        if ((u.state == ARR_ST_MOVE || u.state == ARR_ST_MOVE_HOP || (u.state == ARR_ST_MARSHAL && goal != click)) && c.n_taken < ARR_MAX_TAKEN) c.taken[c.n_taken++] = goal;
        if (u.state == ARR_ST_MARSHAL && goal == click && n_reserved < ARR_MAX_TAKEN) reserved[n_reserved++] = tidx(u.x, u.y);
        if (rule3 && u.state == ARR_ST_MARSHAL && goal == click && n + n_q < FC_MAX_MEMBERS) qh[n_q] = static_cast<uint32_t>(i), qx[n_q] = u.x, qy[n_q] = u.y, ++n_q;
    }
    const int nO = n + n_q; // the order set: the wave, then its queued groupmates
    uint32_t  oh[FC_MAX_MEMBERS];
    uint8_t   ox[FC_MAX_MEMBERS], oy[FC_MAX_MEMBERS];
    for (int m = 0; m < nO; ++m) {
        oh[m] = m < n ? hdl[m] : qh[m - n];
        ox[m] = m < n ? mcol[m] : qx[m - n];
        oy[m] = m < n ? mrow[m] : qy[m - n];
    }
    int ord[FC_MAX_MEMBERS]; // order-set positions in unit-index (handle) order
    for (int32_t m = 0; m < nO; ++m) {
        int j = m - 1;
        while (j >= 0 && oh[ord[j]] > oh[m]) ord[j + 1] = ord[j], --j;
        ord[j + 1] = m;
    }
    auto in_order_set = [&](int32_t unit_idx) {
        for (int m = 0; m < nO; ++m)
            if (oh[m] == static_cast<uint32_t>(unit_idx)) return true;
        return false;
    };
    auto reachable = [&](uint32_t t) {
        if (c.field_fresh) {
            const uint32_t f = w.field_of(t);
            return f != 0 && f != 0xffffffu;
        }
        const uint32_t r = w.region_of(t);
        return r != 0 && r == c.goal_region;
    };
    // ---- the ring window's centre, then (key 3) a one-pass cache of what eligible() and the path BFS both read for every window tile: passable-before-the-bracket, and discovered + reachable
    const bool     k_ok = w.true_passable(click) && (w.fog(click) & c.owner_bit) != 0 && reachable(click);
    const uint32_t cx = k_ok ? kx : gx, cy = k_ok ? ky : gy;
    uint8_t        pc_tp[FC_SPAN][FC_SPAN], pc_fr[FC_SPAN][FC_SPAN]; // key 3 only; stack, ~2 KB
    if (rule3)
        for (int a = 0; a < FC_SPAN; ++a)
            for (int b = 0; b < FC_SPAN; ++b) {
                const uint32_t t = tidx(static_cast<uint32_t>(static_cast<int>(cx) + a - FC_R_MAX) & c.mask, static_cast<uint32_t>(static_cast<int>(cy) + b - FC_R_MAX) & c.mask);
                pc_tp[a][b]      = w.true_passable(t) ? 1 : 0;
                pc_fr[a][b]      = (w.fog(t) & c.owner_bit) != 0 && reachable(t) ? 1 : 0;
            }
    auto eligible = [&](uint32_t t) {
        int wa = -1, wb = -1; // the tile's cell in the cache (key 3, inside the window), else -1
        if (rule3) {
            const int dx = wrap_delta(static_cast<int32_t>(t >> 8), static_cast<int32_t>(cx), c.mask), dy = wrap_delta(static_cast<int32_t>(t & 0xffu), static_cast<int32_t>(cy), c.mask);
            if (dx >= -FC_R_MAX && dx <= FC_R_MAX && dy >= -FC_R_MAX && dy <= FC_R_MAX) wa = dx + FC_R_MAX, wb = dy + FC_R_MAX;
        }
        if (wa >= 0 && !pc_fr[wa][wb]) return false; // not discovered / not reachable: no branch below accepts it
        const int32_t occ  = w.occupant(owner, t);
        bool          mate = false; // move-as-one: a tile one of this order's units stands on is a target (it walks on: an earlier wave or a queued one)
        if (rule3 && occ > 0) {
            arr_unit u;
            mate = in_order_set(occ) || (w.unit(owner, static_cast<uint32_t>(occ), &u) && u.group == group_id);
        }
        if (!mate) {
            if (!(wa >= 0 ? pc_tp[wa][wb] != 0 : w.true_passable(t))) return false;
            if (occ != 0 && !(occ > 0 && member_of(occ))) return false;
        }
        if (!(wa >= 0 ? pc_fr[wa][wb] != 0 : ((w.fog(t) & c.owner_bit) != 0 && reachable(t)))) return false;
        if (is_taken(c, t)) return false;
        if (!rule3)
            for (int k = 0; k < n_reserved; ++k)
                if (reserved[k] == t) return false;
        return true;
    };

    // ---- the ring window: eligible tiles in ring order -------------------------------------------------------------------------
    static_assert(FC_MAX_TILES <= 65535, "tile index fits a uint16");
    uint16_t tiles[FC_MAX_TILES]; // flat tile indices in ring order
    uint8_t  tile_ring[FC_MAX_TILES];
    int8_t   ring_at[FC_SPAN][FC_SPAN]; // -1 = not eligible / outside
    for (int a = 0; a < FC_SPAN; ++a)
        for (int b = 0; b < FC_SPAN; ++b) ring_at[a][b] = -1;
    int        n_tiles = 0, rk = -1;
    ring_entry ring[8 * FC_R_MAX + 1];
    // ---- key 3: the path distance of every window tile from the centre (255 = not reached). On the stack, ~3 KB: small enough (the 40 KB matrices are the static ones).
    uint8_t pdist[FC_SPAN][FC_SPAN];
    for (int a = 0; a < FC_SPAN; ++a)
        for (int b = 0; b < FC_SPAN; ++b) pdist[a][b] = 255;
    if (rule3) {
        uint8_t walk[FC_SPAN][FC_SPAN];
        for (int a = 0; a < FC_SPAN; ++a) // eligible() without the taken filter and without the "a unit stands there" refusal: a unit is not a wall to a route
            for (int b = 0; b < FC_SPAN; ++b) {
                bool ok = pc_fr[a][b] != 0;
                if (ok && !pc_tp[a][b]) { // closed before the bracket: walkable only when a unit of this order stands there (it walks on)
                    const uint32_t t   = tidx(static_cast<uint32_t>(static_cast<int>(cx) + a - FC_R_MAX) & c.mask, static_cast<uint32_t>(static_cast<int>(cy) + b - FC_R_MAX) & c.mask);
                    const int32_t  occ = w.occupant(owner, t);
                    arr_unit       u;
                    ok = occ > 0 && (in_order_set(occ) || (w.unit(owner, static_cast<uint32_t>(occ), &u) && u.group == group_id));
                }
                walk[a][b] = ok ? 1 : 0;
            }
        uint16_t queue[FC_MAX_TILES];
        int      qh = 0, qt = 0;
        pdist[FC_R_MAX][FC_R_MAX] = 0; // the centre is the seed whether or not it is walkable
        queue[qt++]               = static_cast<uint16_t>(FC_R_MAX * FC_SPAN + FC_R_MAX);
        while (qh < qt) {
            const int a = queue[qh] / FC_SPAN, b = queue[qh] % FC_SPAN;
            ++qh;
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy) {
                    if (dx == 0 && dy == 0) continue;
                    const int na = a + dx, nb = b + dy;
                    if (na < 0 || nb < 0 || na >= FC_SPAN || nb >= FC_SPAN || !walk[na][nb] || pdist[na][nb] != 255) continue;
                    if (dx != 0 && dy != 0 && !walk[na][b] && !walk[a][nb]) continue; // a diagonal needs one open orthogonal flank
                    pdist[na][nb] = static_cast<uint8_t>(pdist[a][b] + 1);
                    queue[qt++]   = static_cast<uint16_t>(na * FC_SPAN + nb);
                }
        }
    }
    // path distance of tile t from the centre; 255 = outside the window or not reached
    auto pdist_of = [&](uint32_t t) -> int {
        const int dx = wrap_delta(static_cast<int32_t>(t >> 8), static_cast<int32_t>(cx), c.mask), dy = wrap_delta(static_cast<int32_t>(t & 0xffu), static_cast<int32_t>(cy), c.mask);
        if (dx < -FC_R_MAX || dx > FC_R_MAX || dy < -FC_R_MAX || dy > FC_R_MAX) return 255;
        return pdist[dx + FC_R_MAX][dy + FC_R_MAX];
    };
    // key 3: a tile much farther by path than by the straight ring (the far side of a wall) is no target for SHAPE / GATHER
    auto path_ok = [&](uint32_t t) {
        const int dx = wrap_delta(static_cast<int32_t>(t >> 8), static_cast<int32_t>(cx), c.mask), dy = wrap_delta(static_cast<int32_t>(t & 0xffu), static_cast<int32_t>(cy), c.mask);
        const int p = pdist_of(t), ring_ = abs32(dx) > abs32(dy) ? abs32(dx) : abs32(dy);
        return p != 255 && p <= ring_ + FC_PATH_SLACK;
    };
    for (int r = 0; r <= FC_R_MAX; ++r) {
        const int cnt = ring_order(r, ring);
        for (int e = 0; e < cnt; ++e) {
            const int tx = static_cast<int>(cx) + ring[e].dx, ty = static_cast<int>(cy) + ring[e].dy;
            if (!rule3 && (tx < 0 || ty < 0 || tx > static_cast<int>(c.mask) || ty > static_cast<int>(c.mask))) continue; // key 3 wraps (the map is a torus)
            const uint32_t t = tidx(static_cast<uint32_t>(tx) & c.mask, static_cast<uint32_t>(ty) & c.mask);
            if (!eligible(t)) continue;
            tiles[n_tiles]                                        = static_cast<uint16_t>(t);
            tile_ring[n_tiles++]                                  = static_cast<uint8_t>(rule3 ? pdist[ring[e].dx + FC_R_MAX][ring[e].dy + FC_R_MAX] : r); // key 3: the sort key is the path distance
            ring_at[ring[e].dx + FC_R_MAX][ring[e].dy + FC_R_MAX] = static_cast<int8_t>(r);
            if (rk < 0 && n_tiles >= nO) rk = r;
        }
    }
    if (n_tiles == 0) return false; // nothing to aim at: 2a / retail
    if (rule3) {                    // stable counting sort by path distance: ties keep the ring order (ring, d2, dx, dy) the loop above produced
        int      cnt[257];
        uint16_t tsort[FC_MAX_TILES];
        for (int k = 0; k <= 256; ++k) cnt[k] = 0;
        for (int k = 0; k < n_tiles; ++k) ++cnt[tile_ring[k] + 1];
        for (int k = 0; k < 256; ++k) cnt[k + 1] += cnt[k];
        for (int k = 0; k < n_tiles; ++k) tsort[cnt[tile_ring[k]]++] = tiles[k];
        for (int k = 0; k < n_tiles; ++k) tiles[k] = tsort[k];
        rk = n_tiles >= nO ? pdist_of(tiles[nO - 1]) : -1; // the path-distance band that reaches the order's size
    }
    if (rk < 0) rk = FC_R_MAX;
    auto ring_of = [&](uint32_t t) -> int {
        const int dx = static_cast<int>(t >> 8) - static_cast<int>(cx), dy = static_cast<int>(t & 0xffu) - static_cast<int>(cy);
        if (dx < -FC_R_MAX || dx > FC_R_MAX || dy < -FC_R_MAX || dy > FC_R_MAX) return -1;
        return ring_at[dx + FC_R_MAX][dy + FC_R_MAX];
    };

    // ---- assignment: T[m] per list position; how[m] 0 = retail tile + path, 1 = kept, 2 = assigned ---------------------------------
    uint32_t T[FC_MAX_MEMBERS];
    uint8_t  how[FC_MAX_MEMBERS];
    for (int32_t m = 0; m < nO; ++m) how[m] = 0, T[m] = 0;
    if (nO == 1) {
        arr_unit u;
        w.unit(owner, hdl[0], &u);
        const uint32_t own_goal = tidx(u.goal_x, u.goal_y);
        T[0]                    = eligible(own_goal) && (!rule3 || path_ok(own_goal)) ? own_goal : static_cast<uint32_t>(tiles[0]);
        how[0]                  = 2;
    } else if (!rule3) {
        uint32_t claimed[FC_MAX_MEMBERS];
        int      n_claimed  = 0;
        auto     is_claimed = [&](uint32_t t) {
            for (int k = 0; k < n_claimed; ++k)
                if (claimed[k] == t) return true;
            return false;
        };
        for (int oi = 0; oi < n; ++oi) { // KEEP
            const int      m   = ord[oi];
            const uint32_t cur = tidx(mcol[m], mrow[m]);
            const int      rg  = ring_of(cur);
            if (rg < 0 || rg > rk + slack || is_claimed(cur)) continue;
            T[m]                 = cur;
            how[m]               = 1;
            claimed[n_claimed++] = cur;
        }
        int rows[FC_MAX_MEMBERS], n_rest = 0;
        for (int oi = 0; oi < n; ++oi)
            if (how[ord[oi]] == 0) rows[n_rest++] = ord[oi];
        if (n_rest > 0) { // FILL + Hungarian
            uint32_t S[FC_MAX_MEMBERS];
            int      n_s = 0;
            for (int k = 0; k < n_tiles && n_s < n_rest; ++k)
                if (!is_claimed(tiles[k])) S[n_s++] = tiles[k];
            if (n_s > 0) {
                const int N = n_rest > n_s ? n_rest : n_s;
                // 40 KB: a static scratch, not stack -- the retail exe's stack reserve is only 0x19e10 (105 KB) and this runs deep in the sim.
                // One sim thread, fully overwritten before every use, never read after the call: not state.
                static int32_t cost[FC_MAX_MEMBERS * FC_MAX_MEMBERS];
                for (int i = 0; i < N; ++i)
                    for (int j = 0; j < N; ++j) {
                        int32_t d = 0;
                        if (i < n_rest && j < n_s) {
                            const int dx = static_cast<int>(mcol[rows[i]]) - static_cast<int>(S[j] >> 8);
                            const int dy = static_cast<int>(mrow[rows[i]]) - static_cast<int>(S[j] & 0xffu);
                            d            = dx * dx + dy * dy;
                        }
                        cost[i * N + j] = d;
                    }
                int assign[FC_MAX_MEMBERS];
                hungarian(N, cost, assign);
                for (int i = 0; i < n_rest; ++i)
                    if (assign[i] < n_s) T[rows[i]] = S[assign[i]], how[rows[i]] = 2;
            }
        }
    } else {
        // ===== key 3, "the formation moves as one" (mods:M1G 4b v3; design tmp/rc18dig/spec.md section 2) ===================
        // The plan covers the order set (this wave + its queued groupmates). A member that an earlier wave of this order already planned (its own
        // goal is not the click) keeps that tile when it is still eligible. For the others: a compact core is shifted rigidly by the click minus
        // its truncated centroid (SHAPE), a spread group is gathered into a square block on the click (GATHER).
        uint32_t claimed[FC_MAX_MEMBERS];
        int      n_claimed  = 0;
        auto     is_claimed = [&](uint32_t t) {
            for (int k = 0; k < n_claimed; ++k)
                if (claimed[k] == t) return true;
            return false;
        };
        // Hungarian (squared distance) rows[0..n_rows) <-> S[0..n_s); a row that lands on a pad column stays how == 0
        auto solve = [&](const int *rows, int n_rows, const uint32_t *S, int n_s) {
            if (n_rows <= 0 || n_s <= 0) return;
            const int      N = n_rows > n_s ? n_rows : n_s;
            static int32_t cost3[FC_MAX_MEMBERS * FC_MAX_MEMBERS]; // static scratch, see the old rule's cost matrix
            for (int i = 0; i < N; ++i)
                for (int j = 0; j < N; ++j) {
                    int32_t d = 0;
                    if (i < n_rows && j < n_s) {
                        const int dx = static_cast<int>(ox[rows[i]]) - static_cast<int>(S[j] >> 8);
                        const int dy = static_cast<int>(oy[rows[i]]) - static_cast<int>(S[j] & 0xffu);
                        d            = dx * dx + dy * dy;
                    }
                    cost3[i * N + j] = d;
                }
            int assign[FC_MAX_MEMBERS];
            hungarian(N, cost3, assign);
            for (int i = 0; i < n_rows; ++i)
                if (assign[i] < n_s) T[rows[i]] = S[assign[i]], how[rows[i]] = 2;
        };
        // (a) planned by an earlier wave
        for (int oi = 0; oi < nO; ++oi) {
            const int m = ord[oi];
            if (m >= n || mgoal[m] == click || !eligible(mgoal[m]) || is_claimed(mgoal[m])) continue;
            T[m]                 = mgoal[m];
            how[m]               = 2;
            claimed[n_claimed++] = mgoal[m];
        }
        // (b) the rest of the order set, in unit-index order
        int F[FC_MAX_MEMBERS], nf = 0;
        for (int oi = 0; oi < nO; ++oi)
            if (how[ord[oi]] == 0) F[nf++] = ord[oi];
        if (nf == 1) { // a lone straggler: its own goal if it is eligible, else the first free tile around the click
            const int      m    = F[0];
            const uint32_t g    = m < n ? mgoal[m] : click;
            uint32_t       tt   = 0;
            bool           have = false;
            if (eligible(g) && !is_claimed(g)) tt = g, have = true;
            for (int k = 0; !have && k < n_tiles; ++k)
                if (!is_claimed(tiles[k])) tt = tiles[k], have = true;
            if (have) T[m] = tt, how[m] = 2;
        } else if (nf > 1) {
            // positions on the torus, unwrapped around the first member (the centroid fix's wrap rule)
            int32_t ux[FC_MAX_MEMBERS], uy[FC_MAX_MEMBERS];
            for (int i = 0; i < nf; ++i) {
                ux[i] = static_cast<int32_t>(ox[F[0]]) + wrap_delta(ox[F[i]], ox[F[0]], c.mask);
                uy[i] = static_cast<int32_t>(oy[F[0]]) + wrap_delta(oy[F[i]], oy[F[0]], c.mask);
            }
            const int32_t kux = ux[0] + wrap_delta(static_cast<int32_t>(cx), ux[0], c.mask);
            const int32_t kuy = uy[0] + wrap_delta(static_cast<int32_t>(cy), uy[0], c.mask);
            uint8_t       core[FC_MAX_MEMBERS];
            int           m_core;
            int64_t       sx, sy;
            const bool    compact = core_compact(nf, ux, uy, core, &m_core, &sx, &sy);
            if (compact) { // SHAPE
                const int32_t dx = trunc_div(m_core * static_cast<int64_t>(kux) - sx, m_core), dy = trunc_div(m_core * static_cast<int64_t>(kuy) - sy, m_core);
                for (int i = 0; i < nf; ++i) {
                    if (!core[i]) continue;
                    const uint32_t t = tidx(static_cast<uint32_t>(ux[i] + dx) & c.mask, static_cast<uint32_t>(uy[i] + dy) & c.mask);
                    if (!eligible(t) || is_claimed(t) || !path_ok(t)) continue; // a shifted tile across a wall is refilled from the path order
                    T[F[i]]              = t;
                    how[F[i]]            = 2;
                    claimed[n_claimed++] = t;
                }
                int      rows[FC_MAX_MEMBERS], n_rest = 0;
                uint32_t S[FC_MAX_MEMBERS];
                int      n_s = 0;
                for (int i = 0; i < nf; ++i)
                    if (how[F[i]] == 0) rows[n_rest++] = F[i];
                for (int k = 0; k < n_tiles && n_s < n_rest; ++k)
                    if (!is_claimed(tiles[k])) S[n_s++] = tiles[k];
                solve(rows, n_rest, S, n_s);
            } else { // GATHER
                int bw = 1;
                while (bw * bw < nf) ++bw;
                const int bh = (nf + bw - 1) / bw;
                struct cand {
                    int32_t r, d2, dx, dy;
                    int     tx, ty;
                };
                cand cs[FC_MAX_MEMBERS * 2];
                int  nc = 0;
                for (int i = 0; i < bw; ++i)
                    for (int j = 0; j < bh; ++j) {
                        cand cd;
                        cd.tx = static_cast<int>(cx) - bw / 2 + i;
                        cd.ty = static_cast<int>(cy) - bh / 2 + j;
                        cd.dx = cd.tx - static_cast<int>(cx);
                        cd.dy = cd.ty - static_cast<int>(cy);
                        cd.d2 = cd.dx * cd.dx + cd.dy * cd.dy;
                        cd.r  = abs32(cd.dx) > abs32(cd.dy) ? abs32(cd.dx) : abs32(cd.dy);
                        if (nc < FC_MAX_MEMBERS * 2) cs[nc++] = cd;
                    }
                for (int i = 1; i < nc; ++i) { // ring order around the click: (ring, d2, dx, dy)
                    const cand e = cs[i];
                    int        j = i - 1;
                    while (j >= 0 && (cs[j].r > e.r || (cs[j].r == e.r && (cs[j].d2 > e.d2 || (cs[j].d2 == e.d2 && (cs[j].dx > e.dx || (cs[j].dx == e.dx && cs[j].dy > e.dy))))))) {
                        cs[j + 1] = cs[j];
                        --j;
                    }
                    cs[j + 1] = e;
                }
                uint32_t S[FC_MAX_MEMBERS];
                int      n_s  = 0;
                auto     in_s = [&](uint32_t t) {
                    for (int k = 0; k < n_s; ++k)
                        if (S[k] == t) return true;
                    return false;
                };
                for (int i = 0; i < nc && i < nf; ++i) { // the nf block tiles nearest the click; the ineligible ones are dropped
                    const uint32_t t = tidx(static_cast<uint32_t>(cs[i].tx) & c.mask, static_cast<uint32_t>(cs[i].ty) & c.mask);
                    if (eligible(t) && !is_claimed(t) && !in_s(t) && path_ok(t)) S[n_s++] = t;
                }
                for (int k = 0; k < n_tiles && n_s < nf; ++k) // refill in ring order around the click
                    if (!is_claimed(tiles[k]) && !in_s(tiles[k])) S[n_s++] = tiles[k];
                solve(F, nf, S, n_s);
            }
        }
    }

    // ---- plans, inside the open bracket --------------------------------------------------------------------------------------
    uint32_t saved_tile[FC_MAX_MEMBERS];
    for (int32_t m = 0; m < n; ++m) saved_tile[m] = tidx(w.tile_byte(m, 0), w.tile_byte(m, 1));
    const uint32_t cen_x = w.centroid_x(), cen_y = w.centroid_y();
    const uint32_t goal_x0 = w.goal_x(), goal_y0 = w.goal_y();
    const uint32_t h0 = w.member_handle(0);
    const uint8_t  c0 = w.member_col(0), r0 = w.member_row(0);
    uint32_t       E[FC_MAX_MEMBERS];
    int            kept = 0, filled = 0, retail = 0, replans = 0, reused = 0;
    for (int oi = 0; oi < nO; ++oi) {
        const int m = ord[oi];
        if (m >= n) continue; // queued groupmates are stamped below, not planned now
        arr_unit u;
        w.unit(owner, hdl[m], &u);
        const uint32_t slot = u.path_slot;
        if (how[m] == 0) { // no slot for this member: its retail tile and path stand
            E[m] = saved_tile[m];
            ++retail;
            continue;
        }
        const uint32_t cur = tidx(mcol[m], mrow[m]);
        if (T[m] == cur) { // kept: an EMPTY path, so the marshal's on-goal stop ends it at once
            if (slot != 0xffu) w.path_terminate(owner, slot);
            E[m] = T[m];
            ++kept;
            continue;
        }
        ++filled;
        if (saved_tile[m] == T[m] && slot != 0xffu && w.path_first_run(owner, slot) != 0) { // the retail route already leads there
            E[m] = T[m];
            ++reused;
            continue;
        }
        if (FC_REPLAN_CAP > 0 && replans >= FC_REPLAN_CAP) { // over the cap: keep the retail route; the marshal's retry plans it later
            E[m] = T[m];
            continue;
        }
        w.set_member(0, hdl[m], mcol[m], mrow[m]);
        w.set_member_count(1);
        w.set_goal(static_cast<int32_t>(T[m] >> 8), static_cast<int32_t>(T[m] & 0xffu));
        w.run_pathfind(mode, target);
        E[m] = tidx(w.goal_x() & c.mask, w.goal_y() & c.mask);
        if (E[m] != T[m]) w.log_adjust(owner, hdl[m], T[m], E[m]); // the planner moved an unreachable / blocked goal
        ++replans;
    }
    w.set_member(0, h0, c0, r0);
    w.set_member_count(n);
    w.set_goal(static_cast<int32_t>(goal_x0), static_cast<int32_t>(goal_y0));
    w.set_centroid(cen_x, cen_y);
    for (int32_t m = 0; m < n; ++m) {
        w.set_tile_byte(m, 0, static_cast<uint8_t>(E[m] >> 8));
        w.set_tile_byte(m, 1, static_cast<uint8_t>(E[m] & 0xffu));
        w.set_unit_goal(owner, hdl[m], static_cast<uint8_t>(E[m] >> 8), static_cast<uint8_t>(E[m] & 0xffu));
        w.log_assign(owner, hdl[m], tidx(mcol[m], mrow[m]), E[m], how[m]);
    }
    // Key 3: STAMP the queued groupmates' targets (their wave has not committed yet). Retail's marshal reads a queued unit's goal only in its
    // peer scan, which ran before the first wave; the later waves find their members already planned (the goal differs from the click).
    // `taken` (arrival.h) therefore sees the stamp, and the current tile of a stamped unit is not reserved any more.
    for (int q = n; q < nO; ++q) {
        if (how[q] == 0) continue;
        w.set_unit_goal(owner, oh[q], static_cast<uint8_t>(T[q] >> 8), static_cast<uint8_t>(T[q] & 0xffu));
        w.log_assign(owner, oh[q], tidx(ox[q], oy[q]), T[q], how[q]);
    }
    w.log_fc(owner, n, kept, filled, retail, rk, replans, reused);
    return true;
}

} // namespace mh::sim::move
