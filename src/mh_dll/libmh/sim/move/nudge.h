//
// sim/move/nudge.h -- mods:M1H lane C: the "units make room for each other" movement prototypes (movement plan 3e-1 .. 3e-4), header-only
// over a small WORLD accessor so ONE body serves the retail splice (mh/seams/move_proto_nudge.cpp: live game memory + mh::call::), the
// libmh twin (sim_unit_state_move_walker.cpp / sim_unit_state_group_marshal.cpp: sim_view / sim_store + the libmh call binders) and the
// selftest (array fakes). The design, the exchange rules and the determinism argument live in the seam's banner (3e-1 nudge, 3e-2 claim,
// 3e-3 swap, 3e-4 marshal no-path sites); this header is that code moved verbatim behind `W`.
//
//   [net] move_nudge_idle   a unit whose next tile holds an IDLE friendly ground unit asks it to step to a free neighbour (state 0x36).
//   [net] move_claim_leader a follower takes the tile of a friendly leader that is TURNING in place.
//   [net] move_swap_headon  two friendly units that meet head-on walk through each other's tile.
//
// WRITES: unit records, tile_objects, passable, the path buffer, the fog (through the game's own fow_remove_sight / UpdateFoWPlus) -- all
// via the same game functions in the same order as the splice, so the state writes are byte-identical. No new persistent state.
// DETERMINISM: integer arithmetic only (the doubles that are compared are compared as raw bits), units in the order the caller meets them,
// fixed neighbour scan order, no clock, no rand.
//
// W (the world; every pointer is null when the index is out of range):
//   unit_t *unit(uint32_t player, int32_t idx);            cfg_t *cfg(const unit_t &);
//   tile_t &tile(uint32_t x, uint32_t y);                  uint8_t &pass(uint32_t x, uint32_t y);       (x, y already wrapped to 0..255)
//   wp_t *path(uint32_t player, uint32_t slot);            the slot's 300 {heading, run_length} entries
//   void dir_delta(int heading, int *dx, int *dy);          heading 1..24 -> the step delta (the game's _G_LLM_MAP_DIR_STEP_DELTAS)
//   uint32_t mask_x(); uint32_t mask_y();                   the map's tile wrap masks (general.width_mask / height_mask)
//   const void *game_clock();                               the 8 bytes of _G_LLM_STRAT_GAME_CLOCK
//   soldier_t *soldier(uint32_t player, uint32_t idx);      the player's idx-th soldier record, null past the roster
//   uint16_t cur_index();                                   _G_LLM_STRAT_CUR_INDEX
//   the game calls: fow_remove_sight(player, x, y, sight), fow_update(player, x, y, sight), soldiers_set_heading(player, idx, h),
//   make_single_step(player, idx) -> int32, free_slot(player, idx), set_order_param(player, idx, v), set_state_of(player, idx, st),
//   notify_status(player, idx, code)
//   void note(const char *what, uint32_t player, int32_t ai, int32_t bi, uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, int n);
//   void hit_nd(); void hit_mk(); void hit_mk2(); void hit_eq();   hit counters (the decision path runs)
//
#pragma once

#include <cstdint>
#include <cstring>

#include "addr/mh_structs.gen.h"
#include "sim/move/enter_queue.h" // mods:M1BE: eq_wait_behind_enterer (move_enter_queue)
#include "sim/move/queue_flank.h" // mp:U75: the flank rule (move_queue_flank_guard)

namespace mh::sim::mvc {

struct nudge_keys {
    bool nudge  = false; // move_nudge_idle
    bool claim  = false; // move_claim_leader
    bool swap   = false; // move_swap_headon
    bool enter  = false; // move_enter_queue (mods:M1BE): wait behind an entering friend of the same storage (enter_queue.h)
    bool qflank = false; // move_queue_flank_guard (mp:U75): pick_nudge_tile takes a diagonal only with an open flank (queue_flank.h)
};

constexpr uint16_t NDG_ST_IDLE = 0x01, NDG_ST_MARSHAL = 0x0a, NDG_ST_HOP = 0x0c, NDG_ST_WALK = 0x0f, NDG_ST_STEP_ADJACENT = 0x36;
constexpr int32_t  NDG_MS_SETTLED   = 0x1f;
constexpr int      NDG_PATH_ENTRIES = 300;

namespace ndg {

using unit_t = mh::game::mh_map_object_unit;
using cfg_t  = mh::game::mh_cfg_final_struct_Unit;

inline int heading_wrap(int h) { return ((h - 1) % 24 + 24) % 24 + 1; }
inline int turn_notches(int a, int b) {
    int d = a - b;
    if (d < 0) d = -d;
    d %= 24;
    return d > 12 ? 24 - d : d;
}
inline uint64_t raw_bits(const double &d) {
    uint64_t v;
    std::memcpy(&v, &d, sizeof(v));
    return v;
}
inline bool unit_alive(const unit_t &u) { return (raw_bits(u.energy) >> 63) == 0 && (raw_bits(u.energy) << 1) != 0; }
inline int  wrap_delta(uint32_t from, uint32_t to, uint32_t mask) {
    int d = static_cast<int>((to - from) & mask);
    if (d > static_cast<int>((mask + 1) / 2)) d -= static_cast<int>(mask + 1);
    return d;
}
inline int sgn(int v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }

template <class W>
inline void heading_delta(W &w, int h, int *dx, int *dy) {
    if (h < 1 || h > 24) {
        *dx = *dy = 0;
        return;
    }
    w.dir_delta(h, dx, dy);
}

// A unit's NEXT step as the walker will take it: the leg at its cursor, after the walker's own "skip a spent leg" rule. Returns the
// heading (0 = the path ends / no path) and the cursor the leg sits at.
template <class W>
inline int next_leg(W &w, const unit_t &u, uint32_t player, int *cursor_out) {
    if (u.path_slot_id >= 100) return 0;
    int cur = u.path_cursor;
    if (cur < 0 || cur >= NDG_PATH_ENTRIES - 1) return 0;
    const auto *e = w.path(player, u.path_slot_id);
    if (e[cur].run_length == 0) ++cur; // spent leg
    if (cur >= NDG_PATH_ENTRIES) return 0;
    *cursor_out = cur;
    return e[cur].heading;
}

// ---- 3e-1: where does the idle blocker go --------------------------------------------------------------------------------
template <class W>
inline bool pick_nudge_tile(W &w, const unit_t &b, int leg, uint32_t sx, uint32_t sy, uint32_t mask_x, uint32_t mask_y, uint32_t *ox, uint32_t *oy,
                            bool *straight = nullptr, bool qflank = false) {
    static const int ks[7] = {2, -2, 1, -1, 3, -3, 0}; // perpendicular, then the diagonals, then (last resort) straight ahead
    int              best  = 1000;
    if (straight) *straight = false;
    for (int i = 0; i < 7; ++i) {
        const int h = heading_wrap(leg + 3 * ks[i]);
        int       dx, dy;
        heading_delta(w, h, &dx, &dy);
        if (!dx && !dy) continue;
        const uint32_t nx = (b.x + static_cast<uint32_t>(dx)) & mask_x, ny = (b.y + static_cast<uint32_t>(dy)) & mask_y;
        if (nx == sx && ny == sy) continue;
        if (w.pass(nx, ny) == 0 || w.tile(nx, ny).building != 0) continue; // step_adjacent's own test: passable and unoccupied
        if (qflank && dx != 0 && dy != 0) {                                // mp:U75: a diagonal between two blocked flanks is a corner cut the planner forbids
            const uint32_t fx = (b.x + static_cast<uint32_t>(dx)) & mask_x, fy = (b.y + static_cast<uint32_t>(dy)) & mask_y;
            if (!mh::sim::move::qf_step_ok(true, dx, dy, mh::sim::move::qf_tile_open(w.pass(fx, b.y), w.tile(fx, b.y).class_owner),
                                           mh::sim::move::qf_tile_open(w.pass(b.x, fy), w.tile(b.x, fy).class_owner)))
                continue;
        }
        const int score = turn_notches(b.facing_target, h) + (ks[i] == 0 ? 30 : 0);
        if (score < best) {
            best = score;
            *ox  = nx;
            *oy  = ny;
            if (straight) *straight = ks[i] == 0;
        }
    }
    return best < 1000;
}

// ---- 3e-2 / 3e-3: the exchange -------------------------------------------------------------------------------------------------
template <class W>
inline void soldiers_clear_wander(W &w, uint32_t player, int32_t unit_index, unit_t &u, const cfg_t &c, bool always_heading) {
    if (c.soldier_count <= 0) return;
    const uint32_t si = static_cast<uint32_t>(u.unit_above[0] | (u.unit_above[1] << 8));
    auto          *s  = w.soldier(player, si);
    if (!s) {
        if (always_heading) w.soldiers_set_heading(static_cast<uint16_t>(player), unit_index, u.facing_target);
        return;
    }
    if (s->idle_wander_flag != 0 || always_heading) {
        w.soldiers_set_heading(static_cast<uint16_t>(player), unit_index, u.facing_target);
        s->idle_wander_flag = 0;
    }
}

// A (acting, at S) and B (at T) trade tiles. claim = false: SWAP, both just take their next step. claim = true: B (the turning
// leader) is moved onto S and gets one extra leg in front of its path (heading hA, run 1) so its path stays valid.
template <class W>
inline void do_exchange(W &w, unit_t &A, int32_t ai, unit_t &B, int32_t bi, uint32_t player, bool claim, int hA, int cursA, int cursB) {
    const uint32_t sx = A.x, sy = A.y, tx = B.x, ty = B.y;
    const cfg_t   *ca = w.cfg(A);
    const cfg_t   *cb = w.cfg(B);

    // tiles: each unit's new tile is claimed at once, each old tile is (re)claimed by the other one in the same tick
    auto &tS         = w.tile(sx, sy);
    auto &tT         = w.tile(tx, ty);
    tS.building      = static_cast<uint16_t>(bi);
    tS.class_owner   = static_cast<uint8_t>(player | 0x80u);
    tT.building      = static_cast<uint16_t>(ai);
    tT.class_owner   = static_cast<uint8_t>(player | 0x80u);
    w.pass(sx, sy)   = 5;
    w.pass(tx, ty)   = 5;
    const uint8_t oa = A.origin_tile_was_passable, ob = B.origin_tile_was_passable;
    A.origin_tile_was_passable = ob; // the underlying value of T is what B stood on
    B.origin_tile_was_passable = oa;

    // paths: A takes its step (the leg shrinks by one, like a normal step)
    auto *pa             = w.path(player, A.path_slot_id);
    pa[cursA].run_length = static_cast<uint8_t>(pa[cursA].run_length - 1);
    if (!claim) {
        auto *pb             = w.path(player, B.path_slot_id);
        pb[cursB].run_length = static_cast<uint8_t>(pb[cursB].run_length - 1);
        B.path_cursor        = cursB;
    } else {
        // room for one more entry: shift cursor..end (end sentinel included) up by one, write the return leg at the cursor
        // (at cursB, the leg B would step next, so a spent leg behind the cursor is still skipped by the walker's own rule)
        auto *pb = w.path(player, B.path_slot_id);
        int   e  = cursB;
        while (e < NDG_PATH_ENTRIES - 1 && pb[e].heading != 0) ++e; // e = index of the end sentinel (caller checked the room)
        for (int i = e; i >= cursB; --i) {
            pb[i + 1].heading    = pb[i].heading;
            pb[i + 1].run_length = pb[i].run_length;
        }
        pb[cursB].heading    = static_cast<uint8_t>(hA);
        pb[cursB].run_length = 1;
        const uint8_t rev    = static_cast<uint8_t>(heading_wrap(hA + 12)); // the direction B slides: from T to S
        B.facing_target      = rev;
        B.facing_current     = rev;
    }

    A.x                        = static_cast<uint8_t>(tx);
    A.y                        = static_cast<uint8_t>(ty);
    B.x                        = static_cast<uint8_t>(sx);
    B.y                        = static_cast<uint8_t>(sy);
    A.move_microstep           = 0;
    B.move_microstep           = 0;
    A.path_blocked_retry_count = 0;
    B.path_blocked_retry_count = 0;

    // fog of war: each unit's sight leaves its old tile and arrives on its new one
    w.fow_remove_sight(player, static_cast<int32_t>(sx), static_cast<int32_t>(sy), ca->sight);
    w.fow_remove_sight(player, static_cast<int32_t>(tx), static_cast<int32_t>(ty), cb->sight);
    w.fow_update(player, tx, ty, ca->sight);
    w.fow_update(player, sx, sy, cb->sight);

    soldiers_clear_wander(w, player, ai, A, *ca, false);
    soldiers_clear_wander(w, player, bi, B, *cb, claim);
}

// give u a one-leg path onto tile (tx,ty) (adjacent) through the game's own path_make_single_step, whose goal is the target tile for
// the call; the unit's own goal is put back when keep_goal.
template <class W>
inline bool one_leg(W &w, uint32_t player, int32_t idx, unit_t &u, uint32_t tx, uint32_t ty, int expect, bool keep_goal) {
    const uint8_t gx = u.goal_x, gy = u.goal_y;
    u.goal_x         = static_cast<uint8_t>(tx);
    u.goal_y         = static_cast<uint8_t>(ty);
    const int32_t ok = w.make_single_step(player, idx);
    if (keep_goal) {
        u.goal_x = gx;
        u.goal_y = gy;
    }
    if (!ok) return false;
    if (u.path_slot_id >= 100 || w.path(player, u.path_slot_id)[0].heading != static_cast<uint8_t>(expect)) {
        w.free_slot(static_cast<uint16_t>(player), idx);
        return false;
    }
    return true;
}

// hand a unit that now holds a one-leg path to the walker the way the retail success path of the marshal does
template <class W>
inline void to_walker(W &w, uint32_t player, int32_t idx, unit_t &u, const cfg_t &c) {
    if (u.order == static_cast<uint16_t>(c.move_op_code)) w.set_order_param(static_cast<int32_t>(player), idx, 1);
    const bool cur = idx == static_cast<int32_t>(w.cur_index());
    w.set_state_of(static_cast<int32_t>(player), idx, static_cast<int16_t>(cur ? c.move_op_code : NDG_ST_HOP));
    u.path_cursor              = 0;
    u.path_blocked_retry_count = 0;
    w.notify_status(player, idx, 100);
}

template <class W>
inline void face(W &w, unit_t &u, const cfg_t &c, uint32_t player, int32_t idx, int h) {
    u.facing_target  = static_cast<uint8_t>(h);
    u.facing_current = static_cast<uint8_t>(h);
    if (c.soldier_count > 0) w.soldiers_set_heading(static_cast<uint16_t>(player), idx, static_cast<uint8_t>(h));
}

// The walker's version of the corridor trade: A (walking, settled, its next leg heading hA at cursor cursA) steps onto B's tile and the idle
// B onto A's. B gets a one-leg path from the game's own path_make_single_step (its goal = A's tile for that call), is turned toward it and
// handed to the walker (state 0xc: it is not the ticking unit); do_exchange claims both tiles crosswise and A's leg shrinks like a normal step.
// B's goal stays the tile it slides to and its order STOP_TO_DEFAULT, so it ends idle there. False = nothing was changed.
template <class W>
inline bool nd_trade(W &w, unit_t &A, int32_t ai, unit_t &B, int32_t bi, uint32_t player, int hA, int cursA) {
    const cfg_t *cb = w.cfg(B);
    if (!cb) return false;
    const uint32_t sx = A.x, sy = A.y;
    const int      hB = heading_wrap(hA + 12);
    if (!one_leg(w, player, bi, B, sx, sy, hB, false)) return false;
    face(w, B, *cb, player, bi, hB);
    do_exchange(w, A, ai, B, bi, player, false, hA, cursA, 0);
    to_walker(w, player, bi, B, *cb);
    B.order = NDG_ST_IDLE;
    return true;
}

// One log line per event (first 200 of each kind per run): the evidence that a key fired, and where.
template <class W>
inline void nd_note(W &w, const char *what, uint32_t player, int32_t ai, int32_t bi, uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy) {
    static int n[3];
    const int  k = what[0] == 'n' ? 0 : (what[0] == 's' ? 1 : 2);
    if (n[k]++ >= 200) return;
    w.note(what, player, ai, bi, sx, sy, dx, dy, n[k]);
}

} // namespace ndg

// ---- the walker decision (3e-1 / 3e-2 / 3e-3) -----------------------------------------------------------------------------------
// Called in place of CALL llm_strat_path_step_check_and_request_detour with the walker's (src x, src y, dst x, dst y).
// Returns -1 = run the retail function unchanged; else its return value (0 = "wait for the occupant").
template <class W>
inline int nd_decide(W &w, const nudge_keys &k, uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy) {
    using namespace ndg;
    // mods:M1BE move_enter_queue: an entering unit behind an entering friend of the same storage WAITS (0 = "wait for the occupant"),
    // before the retail check can splice a sidestep into its path. Anything else falls through to the keys below / retail.
    if (k.enter && eq_wait_behind_enterer(w, sx, sy, dx, dy)) {
        w.hit_eq();
        return 0;
    }
    if (!(k.nudge | k.claim | k.swap)) return -1;
    w.hit_nd(); // the walker's meeting decision runs (a key of this site is on)
    const auto &tS = w.tile(sx, sy);
    const auto &tT = w.tile(dx, dy);
    if ((tS.class_owner & 0x80u) == 0 || (tT.class_owner & 0x80u) == 0) return -1; // a building / terrain blocks: retail
    const uint32_t ap = tS.class_owner & 0xfu, bp = tT.class_owner & 0xfu;
    if (ap != bp) return -1; // enemy / ally: retail
    const int32_t ai = tS.building, bi = tT.building;
    if (ai == bi) return -1;
    unit_t *A = w.unit(ap, ai), *B = w.unit(bp, bi);
    if (!A || !B || A->x != sx || A->y != sy || B->x != dx || B->y != dy) return -1;
    const cfg_t *ca = w.cfg(*A), *cb = w.cfg(*B);
    if (!ca || !cb || ca->move_op_code != 0x0f || cb->move_op_code != 0x0f) return -1;  // ground movers only
    if ((raw_bits(B->energy) >> 63) != 0 || (raw_bits(B->energy) << 1) == 0) return -1; // dead or dying: retail
    if (A->state != NDG_ST_WALK || A->path_slot_id >= 100) return -1;

    int       cursA = 0;
    const int hA    = next_leg(w, *A, ap, &cursA);
    if (hA == 0 || A->facing_target != hA) return -1;
    int adx, ady;
    heading_delta(w, hA, &adx, &ady);
    const uint32_t mask_x = w.mask_x();
    const uint32_t mask_y = w.mask_y();
    if (((sx + static_cast<uint32_t>(adx)) & mask_x) != dx || ((sy + static_cast<uint32_t>(ady)) & mask_y) != dy) return -1; // not the walker's own step

    // ---- 3e-1: an idle friend in the way -------------------------------------------------------------------------------
    if (k.nudge) {
        if (B->state == NDG_ST_STEP_ADJACENT) return 0; // the nudge has been asked for, the unit has not started it yet
        if (B->state == NDG_ST_IDLE && cb->default_op_code == NDG_ST_IDLE && B->order_queued == 0 && B->move_microstep >= NDG_MS_SETTLED) {
            uint32_t nx, ny;
            bool     straight = false;
            if (pick_nudge_tile(w, *B, hA, sx, sy, mask_x, mask_y, &nx, &ny, &straight, k.qflank)) {
                // only straight ahead is free (a one-tile corridor): pushing the friend ahead of us tile after tile is worse than trading
                // tiles with it, as the marshal site does (3e-4); the push stays as the fallback when the trade cannot be set up
                if (straight && A->move_microstep >= NDG_MS_SETTLED && A->order_queued == 0 && nd_trade(w, *A, ai, *B, bi, ap, hA, cursA)) {
                    nd_note(w, "swap-trade", ap, ai, bi, sx, sy, dx, dy);
                    return 0;
                }
                B->goal_x = static_cast<uint8_t>(nx);
                B->goal_y = static_cast<uint8_t>(ny);
                B->state  = NDG_ST_STEP_ADJACENT;
                nd_note(w, "nudge", ap, ai, bi, sx, sy, nx, ny);
                B->order = NDG_ST_IDLE;
                // a sleeping unit (clock ahead of the game clock) must not sit out its sleep before it makes room
                uint64_t gc;
                std::memcpy(&gc, w.game_clock(), 8);
                if (raw_bits(B->activity_clock) > gc) std::memcpy(&B->activity_clock, w.game_clock(), 8);
                return 0;
            }
        }
    }

    // ---- 3e-3 / 3e-2: a walking friend ---------------------------------------------------------------------------------
    if ((k.swap | k.claim) && B->state == NDG_ST_WALK && B->order_queued == 0 && A->order_queued == 0 && A->move_microstep >= NDG_MS_SETTLED &&
        B->path_slot_id < 100) {
        int       cursB = 0;
        const int hB    = next_leg(w, *B, bp, &cursB);
        if (hB != 0) {
            int bdx, bdy;
            heading_delta(w, hB, &bdx, &bdy);
            const bool head_on = ((dx + static_cast<uint32_t>(bdx)) & mask_x) == sx && ((dy + static_cast<uint32_t>(bdy)) & mask_y) == sy;
            if (head_on) {
                if (!k.swap) return -1;
                // still sliding into its tile or still turning toward us: wait for it (its own blocked step would do the swap too)
                if (B->move_microstep < NDG_MS_SETTLED || B->facing_target != hB) return 0;
                nd_note(w, "swap", ap, ai, bi, sx, sy, dx, dy);
                do_exchange(w, *A, ai, *B, bi, ap, false, hA, cursA, cursB);
                return 0;
            }
            if (k.claim && B->move_microstep >= NDG_MS_SETTLED && B->facing_target != hB && B->path_blocked_retry_count == 0) {
                const auto *ea = w.path(ap, A->path_slot_id);
                if (ea[cursA].run_length >= 2) { // F keeps going straight after this step
                    const auto *eb = w.path(bp, B->path_slot_id);
                    int         e  = cursB;
                    while (e < NDG_PATH_ENTRIES - 1 && eb[e].heading != 0) ++e;
                    if (e < NDG_PATH_ENTRIES - 2) { // room for one more entry behind the end sentinel
                        nd_note(w, "claim", ap, ai, bi, sx, sy, dx, dy);
                        do_exchange(w, *A, ai, *B, bi, ap, true, hA, cursA, cursB);
                        return 0;
                    }
                }
            }
        }
    }
    return -1;
}

// ---- the group-marshal no-path sites (3e-4) -------------------------------------------------------------------------------------
// A unit whose route the wave planner cut short at a friend's tile ends its trip in state 0x0a with NO path (slot 0xff): retail adds
// 0.05 s to its clock and tries again until its order times out. mk_decide() looks at the tile the unit wants next (one step toward its
// goal, wrapped) and, for a friendly ground unit standing on it: an IDLE friend with a free neighbour other than straight ahead is
// nudged (the member retries); an IDLE friend with no sidestep, or a friend that is itself marshal-stuck and wants OUR tile, or walking
// with its next step onto our tile (head-on), trades tiles with the member (the walker's own exchange, both handed to the walker).
// Returns true = handled (skip the retail clock bump / retry branch), false = run retail.
template <class W>
inline bool mk_decide(W &w, const nudge_keys &k, uint32_t player, uint32_t idx) {
    using namespace ndg;
    if (!(k.nudge | k.swap)) return false;
    unit_t *A = w.unit(player, static_cast<int32_t>(idx));
    if (!A) return false;
    const cfg_t *ca = w.cfg(*A);
    if (!ca || ca->move_op_code != 0x0f || (A->order != 0x0f && A->order != NDG_ST_IDLE)) return false; // a plain move (order 0x0f, or 1 once replanned)
    if (A->state != NDG_ST_MARSHAL || A->path_slot_id != 0xff || A->order_queued != 0 || A->move_microstep < NDG_MS_SETTLED) return false;
    const uint32_t mask_x = w.mask_x(), mask_y = w.mask_y();
    const int      ddx = wrap_delta(A->x, A->goal_x, mask_x), ddy = wrap_delta(A->y, A->goal_y, mask_y);
    if (!ddx && !ddy) return false;
    // the principal heading (1,4,..,22) whose step delta is exactly (sgn ddx, sgn ddy); 0 if none
    int h0 = 0;
    for (int h = 1; h <= 22; h += 3) {
        int ex, ey;
        heading_delta(w, h, &ex, &ey);
        if (ex == sgn(ddx) && ey == sgn(ddy)) {
            h0 = h;
            break;
        }
    }
    if (!h0) return false;
    const int32_t ai = static_cast<int32_t>(idx);

    // the tile the unit wants next, then (only when that one is a wall) its two flanks
    const int hs[3] = {h0, heading_wrap(h0 - 3), heading_wrap(h0 + 3)};
    for (int kk = 0; kk < 3; ++kk) {
        int dx, dy;
        heading_delta(w, hs[kk], &dx, &dy);
        const uint32_t tx = (A->x + static_cast<uint32_t>(dx)) & mask_x, ty = (A->y + static_cast<uint32_t>(dy)) & mask_y;
        const auto    &tT  = w.tile(tx, ty);
        const bool     occ = (tT.class_owner & 0x80u) != 0 && tT.building != 0;
        if (!occ) {
            if (w.pass(tx, ty) == 0) continue; // a wall: look at the flank
            return false;                      // a free tile: the planner's business
        }
        if ((tT.class_owner & 0xfu) != player) return false;
        const int32_t bi = tT.building;
        if (bi == ai) return false;
        unit_t *B = w.unit(player, bi);
        if (!B || B->x != tx || B->y != ty) return false;
        const cfg_t *cb = w.cfg(*B);
        if (!cb || cb->move_op_code != 0x0f || !unit_alive(*B) || B->move_microstep < NDG_MS_SETTLED) return false;
        const uint32_t sx = A->x, sy = A->y;
        const int      hA = hs[kk];
        const int      hB = heading_wrap(hA + 12);

        // ---- an idle friend ----
        if (B->state == NDG_ST_STEP_ADJACENT) return false; // already nudged, not started: keep waiting
        if (k.nudge && B->state == NDG_ST_IDLE && cb->default_op_code == NDG_ST_IDLE && B->order_queued == 0) {
            uint32_t nx, ny;
            bool     straight = false;
            if (pick_nudge_tile(w, *B, hA, sx, sy, mask_x, mask_y, &nx, &ny, &straight, k.qflank) && !straight) {
                B->goal_x = static_cast<uint8_t>(nx);
                B->goal_y = static_cast<uint8_t>(ny);
                B->state  = NDG_ST_STEP_ADJACENT;
                nd_note(w, "nudge-marshal", player, ai, bi, sx, sy, nx, ny);
                B->order = NDG_ST_IDLE;
                uint64_t gc;
                std::memcpy(&gc, w.game_clock(), 8);
                if (raw_bits(B->activity_clock) > gc) std::memcpy(&B->activity_clock, w.game_clock(), 8);
                return false; // retry soon (retail clock bump)
            }
            // no sidestep: trade tiles
            if (!one_leg(w, player, ai, *A, tx, ty, hA, true)) return false;
            const uint8_t bgx = B->goal_x, bgy = B->goal_y;
            if (!one_leg(w, player, bi, *B, sx, sy, hB, false)) {
                w.free_slot(static_cast<uint16_t>(player), ai);
                B->goal_x = bgx;
                B->goal_y = bgy;
                return false;
            }
            nd_note(w, "swap-marshal", player, ai, bi, sx, sy, tx, ty);
            face(w, *A, *ca, player, ai, hA);
            face(w, *B, *cb, player, bi, hB);
            do_exchange(w, *A, ai, *B, bi, player, false, hA, 0, 0);
            to_walker(w, player, ai, *A, *ca);
            to_walker(w, player, bi, *B, *cb);
            B->order = NDG_ST_IDLE; // an idle friend stays "default" on arrival (goal = the tile it is sliding to)
            return true;
        }

        // ---- a friend that wants our tile (head-on) ----
        if (!k.swap || B->order_queued != 0) return false;
        int  cursB   = 0;
        bool b_stuck = false;
        if (B->state == NDG_ST_MARSHAL && B->path_slot_id == 0xff && (B->order == 0x0f || B->order == NDG_ST_IDLE)) {
            if (B->move_group_id == A->move_group_id && B->goal_x == A->goal_x && B->goal_y == A->goal_y) return false; // one group: the planner's
            const int bdx = wrap_delta(B->x, B->goal_x, mask_x), bdy = wrap_delta(B->y, B->goal_y, mask_y);
            int       bh = 0; // principal_heading(sgn(bdx), sgn(bdy))
            for (int h = 1; h <= 22; h += 3) {
                int ex, ey;
                heading_delta(w, h, &ex, &ey);
                if (ex == sgn(bdx) && ey == sgn(bdy)) {
                    bh = h;
                    break;
                }
            }
            if (bh != hB) return false; // not head-on
            if (!one_leg(w, player, ai, *A, tx, ty, hA, true)) return false;
            if (!one_leg(w, player, bi, *B, sx, sy, hB, true)) {
                w.free_slot(static_cast<uint16_t>(player), ai);
                return false;
            }
            b_stuck = true;
        } else if (B->state == NDG_ST_WALK && B->path_slot_id < 100) {
            const int hb = next_leg(w, *B, player, &cursB);
            if (hb != hB || B->facing_target != hB) return false;
            if (!one_leg(w, player, ai, *A, tx, ty, hA, true)) return false;
        } else {
            return false;
        }
        nd_note(w, "swap-marshal", player, ai, bi, sx, sy, tx, ty);
        face(w, *A, *ca, player, ai, hA);
        if (b_stuck) face(w, *B, *cb, player, bi, hB);
        do_exchange(w, *A, ai, *B, bi, player, false, hA, 0, cursB);
        to_walker(w, player, ai, *A, *ca);
        if (b_stuck) to_walker(w, player, bi, *B, *cb);
        return true;
    }
    return false;
}

} // namespace mh::sim::mvc
