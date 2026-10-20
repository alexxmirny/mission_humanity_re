//
// sim/move/arrival.h -- mods:M1H lane B: [net] move_distinct_goals (mods:M1X 2a), the pure decision.
//
// THE RETAIL PIPELINE (tmp/m1/pipeline.md sections 2 and 4): llm_strat_unit_state_group_marshal splits a click's units into waves
// and calls llm_strat_group_move_order_commit 0x0041c86c per wave, which calls llm_strat_group_move_order_pathfind 0x0041ca37.
// For a multi-member wave each member's tile is `goal - (centroid - member)` (the selection's shape moved rigidly), its path
// walks the route at that offset and, when the walk gets stuck, the tile becomes "wherever it got to". commit copies those
// tiles into the marshal scratch and the marshal makes them the members' goals (only when move_group_id != 0 and the wave has
// more than one member). A single-member wave keeps the CLICK as its goal, so the waves of one of a spread selection all aim at
// the same tile (replay order 16: u2 waits 604 steps in state 0xa and gives up).
//
// THE KEY: every member of a group move gets its OWN free goal tile. arrival_after_pathfind() runs in commit right after its
// CALL to pathfind (retail splice: 0x0041c9d3; libmh twin: sim_group_move_order_commit.cpp). Only for a plain (non-attack) move
// with move_group_id != 0; only members on a plain ground move: order 0x0f (fresh) or 0x01 (STOP_TO_DEFAULT queued -- the
// marshal good path sets it, so every REPLAN of a move runs with order 1).
//   * A tile T is FREE when: passable[T] != 0 (no terrain/building/settled unit), discovered by the owner (no fog clamp),
//     REACHABLE (the route flood pathfind just ran from the goal reached T: terrain_flags>>8 not 0 and not 0xffffff; if that
//     field is not fresh -- the goal cell is not the flood seed, value 1 -- T must be in the goal's nav region), and NOT TAKEN:
//     not assigned to an earlier member of this wave, and not the goal of another unit of the same player and move_group_id
//     that already has one (state 0xc/0xf, or 0xa with a goal other than the click).
//   * Multi-member wave: pass 1 keeps every member's formation tile F = goal - (centroid - member) that is free (in member
//     order); pass 2 gives each remaining member the nearest free tile around F (radius 8), else around the goal, else keeps
//     the retail tile. The chosen tile is written to _G_LLM_STRAT_GROUP_MEMBER_TILE, which commit copies and the marshal makes
//     the goal.
//   * Single-member wave: if the route's goal G is not free, the nearest free tile T around G becomes the group goal, pathfind
//     is run AGAIN for T (it frees and re-fills the path slot), and the unit's own goal is set to T.
//   Nearest = smallest dx*dx+dy*dy, scanned ring by ring (Chebyshev), ties by scan order (dx then dy ascending).
//
// THE TRAP (plan section 1), FIXED by mods:M1DT fix A' (rc17 dig): the re-run of pathfind used to be the planner called DIRECTLY, outside
// the route-through bracket (move/route_through.h), so settled walkers, 0x0a units and idle friends in a corridor were walls again and the
// re-plan took the long way round (rc17 O4 u1/u11, O8 u12/u15: 7 -> 34 tiles). World::rerun_pathfind now opens and closes the bracket
// around the call: the retail splice via mv_rt_open/mv_rt_close (move_proto_nudge.cpp), the libmh twin via the commit's own bracket
// code. A caller that changes one must change both.
//
// DETERMINISM: integer arithmetic only, units in table order, fixed scan order, no clock, no rand. The only writes are
// _G_LLM_STRAT_GROUP_MEMBER_TILE / GROUP_ORDER_GOAL (the group-move scratch globals, rewritten by every commit), the unit's
// goal_x/goal_y, and what the second pathfind run writes (the same set the first one writes). No new state survives the call.
//
// The "stop retrying" half of the retail key (the marshal's arrival_retry_check, "the unit stands on its goal") belongs to the
// marshal and is NOT here (lane C, near_enough).
//
// Header-only over a World accessor (live memory in the splice, sim_view / sim_store in the twin, a fake in the selftest):
//
//   struct World {
//       uint32_t path_mask();                         // _G_LLM_STRAT_PATH_WRAP_MASK
//       uint32_t owner();                             // group order owner (&7 applied here)
//       int32_t  member_count();
//       int32_t  unit_limit();                        // roster cap per player (100 stock)
//       uint32_t goal_x(); uint32_t goal_y();         // _G_LLM_STRAT_GROUP_ORDER_GOAL_X / _Y
//       void     set_goal(int32_t x, int32_t y);
//       uint32_t anchor_x(); uint32_t anchor_y();     // _G_LLM_STRAT_GROUP_ANCHOR_X / _Y (= the click)
//       uint32_t centroid_x(); uint32_t centroid_y(); // _G_LLM_STRAT_GROUP_CENTROID_X / _Y
//       uint32_t member_handle(int32_t m); uint8_t member_col(int32_t m); uint8_t member_row(int32_t m);
//       uint8_t  tile_byte(int32_t m, int k);         // _G_LLM_STRAT_GROUP_MEMBER_TILE[m][k]
//       void     set_tile_byte(int32_t m, int k, uint8_t v);
//       uint8_t  passable(uint32_t t); uint8_t fog(uint32_t t);
//       uint32_t region_of(uint32_t t); uint32_t field_of(uint32_t t);
//       bool     unit(uint32_t owner, uint32_t i, arr_unit *out);
//       void     set_unit_goal(uint32_t owner, uint32_t i, uint8_t x, uint8_t y);
//       void     rerun_pathfind(uint32_t mode, uint32_t target);
//       void     log_single(uint32_t owner, uint32_t ui, uint32_t gx, uint32_t gy, bool moved, uint32_t tx, uint32_t ty);
//       void     log_wave(uint32_t owner, int32_t n, uint32_t gx, uint32_t gy, bool fresh, int kept, int moved, int failed);
//   };
//
#pragma once

#include <cstdint>

namespace mh::sim::move {

constexpr uint16_t ARR_ORDER_MOVE = 0x0f, ARR_ORDER_STOP_TO_DEFAULT = 0x01;
constexpr uint16_t ARR_ST_MARSHAL = 0x0a, ARR_ST_MOVE_HOP = 0x0c, ARR_ST_MOVE = 0x0f;
constexpr int      ARR_SEARCH_R  = 8;   // ring-search radius around a formation tile / the goal
constexpr int      ARR_MAX_TAKEN = 128; // other units' goals (at most 100 per player)
constexpr uint32_t ARR_NONE      = 0xffffffffu;

struct arr_unit {
    uint16_t order, state;
    uint8_t  goal_x, goal_y;
    int32_t  group; // move_group_id
    // mods:M1G 4b (move/formation.h) also reads these two; arrival.h itself does not
    uint8_t x = 0, y = 0;
    uint8_t path_slot = 0xff; // path_slot_id
};

namespace arrival_detail {

inline bool     is_plain_move(uint16_t order) { return order == ARR_ORDER_MOVE || order == ARR_ORDER_STOP_TO_DEFAULT; }
inline uint32_t tidx(uint32_t x, uint32_t y) { return ((x & 0xffu) << 8) | (y & 0xffu); }

// One commit's view: what makes a tile acceptable as a member's goal.
struct pick_ctx {
    uint32_t owner_bit;
    bool     field_fresh; // the region flood was last seeded at the goal
    uint32_t goal_region; // fallback reachability: same nav region as the goal
    uint32_t taken[ARR_MAX_TAKEN + 256];
    int      n_taken;
    uint32_t mask; // path wrap mask (map width - 1)
};

inline bool is_taken(const pick_ctx &c, uint32_t t) {
    for (int k = 0; k < c.n_taken; ++k)
        if (c.taken[k] == t) return true;
    return false;
}

template <class W>
inline bool tile_free(W &w, const pick_ctx &c, uint32_t t) {
    if (w.passable(t) == 0) return false;
    if ((w.fog(t) & c.owner_bit) == 0) return false;
    if (c.field_fresh) {
        const uint32_t f = w.field_of(t);
        if (f == 0 || f == 0xffffffu) return false;
    } else {
        const uint32_t r = w.region_of(t);
        if (r == 0 || r != c.goal_region) return false;
    }
    return !is_taken(c, t);
}

// Nearest free tile around (cx,cy) within ARR_SEARCH_R (not the centre itself): ring by ring, smallest dx*dx+dy*dy, ties by scan
// order. Returns ARR_NONE when none.
template <class W>
inline uint32_t search_near(W &w, const pick_ctx &c, uint32_t cx, uint32_t cy) {
    uint32_t best = ARR_NONE;
    int      bd2  = 0x7fffffff;
    for (int r = 1; r <= ARR_SEARCH_R; ++r) {
        for (int dx = -r; dx <= r; ++dx) {
            for (int dy = -r; dy <= r; ++dy) {
                if (dx != -r && dx != r && dy != -r && dy != r) continue; // ring r only
                const int d2 = dx * dx + dy * dy;
                if (d2 >= bd2) continue;
                const uint32_t t = tidx((cx + static_cast<uint32_t>(dx)) & c.mask, (cy + static_cast<uint32_t>(dy)) & c.mask);
                if (!tile_free(w, c, t)) continue;
                best = t;
                bd2  = d2;
            }
        }
        if (best != ARR_NONE && bd2 <= (r + 1) * (r + 1)) break; // no tile further out can be nearer
    }
    return best;
}

} // namespace arrival_detail

// Runs right after the planner returned inside commit. `group_id` = commit's move_group_id, `mode` = its is_plain_move_flag (the
// planner's EAX), `target` = the planner's EDX (handed back to rerun_pathfind unchanged).
template <class W>
inline void arrival_after_pathfind(W &w, int32_t group_id, uint32_t mode, uint32_t target) {
    using namespace arrival_detail;
    if (group_id == 0 || mode == 0) return;
    const int32_t n = w.member_count();
    if (n < 1 || n > 256) return;
    const uint32_t owner = w.owner() & 7u;

    pick_ctx c; // this call's scratch, on the stack: nothing survives the call
    c.mask            = w.path_mask() & 0xffu;
    c.owner_bit       = 1u << owner;
    const uint32_t gx = w.goal_x() & c.mask, gy = w.goal_y() & c.mask;
    const uint32_t g     = tidx(gx, gy);
    c.field_fresh        = w.field_of(g) == 1u;
    c.goal_region        = w.region_of(g);
    const uint32_t click = tidx(w.anchor_x() & c.mask, w.anchor_y() & c.mask);

    // Goals other units of this group already hold (not this wave's members).
    c.n_taken              = 0;
    const int32_t unit_cap = w.unit_limit();
    for (int32_t i = 0; i < unit_cap; ++i) {
        bool member = false;
        for (int32_t m = 0; m < n; ++m)
            if (w.member_handle(m) == static_cast<uint32_t>(i)) {
                member = true;
                break;
            }
        if (member) continue;
        arr_unit u;
        if (!w.unit(owner, static_cast<uint32_t>(i), &u)) continue;
        if (u.group != group_id) continue;
        const uint32_t goal = tidx(u.goal_x, u.goal_y);
        if (!(u.state == ARR_ST_MOVE || u.state == ARR_ST_MOVE_HOP || (u.state == ARR_ST_MARSHAL && goal != click))) continue;
        if (c.n_taken < ARR_MAX_TAKEN) c.taken[c.n_taken++] = goal;
    }

    int kept = 0, moved = 0, failed = 0;
    if (n == 1) {
        const uint32_t ui = w.member_handle(0);
        arr_unit       u;
        if (ui >= static_cast<uint32_t>(unit_cap) || !w.unit(owner, ui, &u) || !is_plain_move(u.order)) return;
        if (tile_free(w, c, g)) return; // the route's goal is free: retail
        const uint32_t t = search_near(w, c, gx, gy);
        if (t == ARR_NONE) {
            failed = 1;
        } else {
            w.set_goal(static_cast<int32_t>(t >> 8), static_cast<int32_t>(t & 0xffu));
            w.rerun_pathfind(mode, target); // re-plan the wave of one to T (frees + refills its path slot)
            w.set_unit_goal(owner, ui, static_cast<uint8_t>(t >> 8), static_cast<uint8_t>(t & 0xffu));
            moved = 1;
        }
        w.log_single(owner, ui, gx, gy, moved != 0, moved ? (t >> 8) : gx, moved ? (t & 0xffu) : gy);
        return;
    }

    // Multi-member wave.
    const uint32_t cx = w.centroid_x() & 0xffu, cy = w.centroid_y() & 0xffu;
    uint32_t       form[256];
    uint8_t        state[256]; // 0 = not ours (keep retail tile), 1 = pending, 2 = assigned
    for (int32_t m = 0; m < n; ++m) {
        const uint32_t ui = w.member_handle(m);
        const uint8_t  dc = static_cast<uint8_t>(static_cast<uint8_t>(cx) - w.member_col(m));
        const uint8_t  dr = static_cast<uint8_t>(static_cast<uint8_t>(cy) - w.member_row(m));
        form[m]           = tidx(static_cast<uint8_t>(static_cast<uint8_t>(gx) - dc) & c.mask, static_cast<uint8_t>(static_cast<uint8_t>(gy) - dr) & c.mask);
        arr_unit u;
        state[m] = (ui < static_cast<uint32_t>(unit_cap) && w.unit(owner, ui, &u) && is_plain_move(u.order)) ? 1 : 0;
        if (state[m] == 0) c.taken[c.n_taken++] = tidx(w.tile_byte(m, 0), w.tile_byte(m, 1));
    }
    for (int32_t m = 0; m < n; ++m) { // pass 1: formation tiles that are free
        if (state[m] != 1 || !tile_free(w, c, form[m])) continue;
        state[m]             = 2;
        c.taken[c.n_taken++] = form[m];
        const uint32_t old   = tidx(w.tile_byte(m, 0), w.tile_byte(m, 1));
        if (old != form[m]) ++moved;
        else ++kept;
        w.set_tile_byte(m, 0, static_cast<uint8_t>(form[m] >> 8));
        w.set_tile_byte(m, 1, static_cast<uint8_t>(form[m] & 0xffu));
    }
    for (int32_t m = 0; m < n; ++m) { // pass 2: the rest take the nearest free tile
        if (state[m] != 1) continue;
        uint32_t t = search_near(w, c, form[m] >> 8, form[m] & 0xffu);
        if (t == ARR_NONE) t = tile_free(w, c, g) ? g : search_near(w, c, gx, gy);
        const uint32_t old = tidx(w.tile_byte(m, 0), w.tile_byte(m, 1));
        if (t == ARR_NONE) {
            ++failed;
            t = old; // retail tile
        } else {
            ++moved;
            w.set_tile_byte(m, 0, static_cast<uint8_t>(t >> 8));
            w.set_tile_byte(m, 1, static_cast<uint8_t>(t & 0xffu));
        }
        c.taken[c.n_taken++] = t;
        state[m]             = 2;
    }
    w.log_wave(owner, n, gx, gy, c.field_fresh, kept, moved, failed);
}

} // namespace mh::sim::move
