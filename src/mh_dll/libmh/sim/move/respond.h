//
// sim/move/respond.h -- mods:M1G step 4c: movement RESPONSIVENESS, header-only over a small WORLD accessor so ONE body serves the
// retail splices (mh/seams/move_proto_respond.cpp), the libmh twins (sim_order_dispatch.cpp retain path, sim_unit_state_move_walker.cpp
// turn branch) and the selftest (array fakes). Design: tmp/m1g/4c_plan.md.
//
//   [net] move_reverse_now  a ground unit that is SLIDING from tile O into tile N and is given a move order whose goal lies strictly
//                           nearer to O than to N slides back to O at once instead of finishing the slide, turning 180 degrees and
//                           walking back (only while move_microstep < REVERSE_MS_LIMIT, i.e. less than halfway). Runs only for a record the dispatcher has already decided to DEFER (the retain path), so
//                           nothing else about the order changes: the record is kept, order_queued is set, and the order applies at O
//                           on the next pass exactly as retail would have applied it at N.
//   [net] move_turn_rate    the walker's turn cost (cfg turn_speed * move_step_speed_scale, one heading notch per cost of tick budget)
//                           is scaled by 2^-k, k = 0 retail, 1 = twice as fast, 2 = four times. Exact in x87 and SSE2 (a power of two),
//                           so splice and twin agree bit for bit and k = 0 is byte-identical retail.
//
// The reverse is the walker's OWN step done backwards (sim_unit_state_move_walker.cpp "take the step"): N is vacated (tile_objects
// cleared, passable[N] restored from origin_tile_was_passable, the fog sight removed), O is claimed (tile_objects = this unit, passable[O]
// = 5 "a unit is moving in", origin_tile_was_passable = the old passable[O], x,y = O, fog added), facing_target and facing_current
// turn 180 degrees, move_microstep = 30 - ms (the sprite position screen = tile*32 + 16 + sign[facing] * (31 - ms) is continuous
// across the flip; the unit then slides ms + 1 microsteps back to O). move_step_speed_scale keeps N's value until the next real step.
//
// DETERMINISM: integer only, no clock, no rand; writes only fields the walker already writes. No new persistent state. Stable: after a
// retreat the goal is strictly nearer to the new N (= O) than to the new O (= old N), so the strict test cannot fire a second time.
//
// W (the world) -- same accessors as sim/move/nudge.h plus:
//   unit_t *unit(player, idx); const cfg_t *cfg(const unit_t &); tile_t &tile(x, y); uint8_t &pass(x, y); void dir_delta(h, &dx, &dy);
//   mask_x(); mask_y(); soldier_t *soldier(player, si); fow_remove_sight / fow_update / soldiers_set_heading;
//   note_reverse(player, idx, from_x, from_y, to_x, to_y, ms): the evidence line (a log, never read back).
//
#pragma once

#include <cstdint>

#include "sim/move/nudge.h" // ndg::wrap_delta, ndg::heading_wrap, ndg::soldiers_clear_wander

namespace mh::sim::mvc {

constexpr uint32_t RSP_GROUND_TYPE_MAX = 0xe; // cfg type <= 0xe = ground (the dispatcher's turn-gate skip)
constexpr uint16_t RSP_STATE_WALK      = 0x0f;
constexpr int32_t  RSP_MS_SETTLED      = 0x1f;
// reverse only while LESS THAN HALFWAY through the slide (move_microstep counts UP 0 -> 31): past the middle the slide back costs more than
// finishing it (F12b: a re-order at ms 24 ended 9-12 steps behind retail).
constexpr int32_t REVERSE_MS_LIMIT = 15;

// The three game calls the reverse makes (the libmh_world<Calls> members of the same names). The dispatcher has no such slots in its own
// call table, so the libmh twin binds them here; a selftest installs stubs through g_respond_calls_override.
struct respond_calls {
    void (*fow_remove_sight)(uint32_t player, int32_t x, int32_t y, uint8_t radius);
    void (*map_fow_UpdateFoWPlus)(uint32_t player, uint32_t x, uint32_t y, uint8_t sight);
    void (*unit_soldiers_set_heading)(uint16_t player, int32_t unit_index, uint8_t sprite_frame);
};
inline const respond_calls *g_respond_calls_override = nullptr;

// ---- move_turn_rate -------------------------------------------------------------------------------------------------------------
inline int turn_rate_clamp(int k) { return k < 0 ? 0 : (k > 2 ? 2 : k); }
// The factor the walker's turn cost is multiplied by: exactly 2^-k.
inline double turn_scale(int k) {
    switch (turn_rate_clamp(k)) {
        case 1: return 0.5;
        case 2: return 0.25;
        default: return 1.0;
    }
}

// ---- move_reverse_now -----------------------------------------------------------------------------------------------------------
// How far (squared, torus-wrapped) is tile (ax, ay) from the goal (gx, gy)?
template <class W>
inline int64_t rsp_dist2(W &w, uint32_t ax, uint32_t ay, uint32_t gx, uint32_t gy) {
    const int64_t dx = ndg::wrap_delta(ax, gx, w.mask_x());
    const int64_t dy = ndg::wrap_delta(ay, gy, w.mask_y());
    return dx * dx + dy * dy;
}

// The unit's slide-back tile O = N - delta(facing_target), or false when the unit is not in a state to reverse. Pure read.
template <class W>
inline bool reverse_origin(W &w, uint32_t player, int32_t idx, const ndg::unit_t &u, uint32_t *ox, uint32_t *oy, int *rev_heading) {
    if (u.state != RSP_STATE_WALK || u.move_microstep >= RSP_MS_SETTLED) return false;
    const ndg::cfg_t *c = w.cfg(u);
    if (!c || c->type > RSP_GROUND_TYPE_MAX) return false;
    const int h = u.facing_target;
    if (h < 1 || h > 24) return false;
    int dx, dy;
    w.dir_delta(h, &dx, &dy);
    if (!dx && !dy) return false;
    const uint32_t nx = u.x, ny = u.y;
    *ox          = (nx - static_cast<uint32_t>(dx)) & w.mask_x();
    *oy          = (ny - static_cast<uint32_t>(dy)) & w.mask_y();
    *rev_heading = ndg::heading_wrap(h + 12);
    // The unit really is mid-slide into N: it owns N's tile record and holds the "moving in" mark.
    auto &tn = w.tile(nx, ny);
    if (tn.building != static_cast<uint16_t>(idx) || tn.class_owner != static_cast<uint8_t>(player | 0x80u)) return false;
    return true;
}

// The decision for a deferred move record whose goal is (gx, gy): true = reverse now. Pure read.
template <class W>
inline bool reverse_decide(W &w, uint32_t player, int32_t idx, int32_t gx, int32_t gy, uint16_t order_code, bool boarding) {
    if (boarding || gx < 0 || gy < 0) return false;
    ndg::unit_t *u = w.unit(player, idx);
    if (!u) return false;
    const ndg::cfg_t *c = w.cfg(*u);
    // a move record: a group move is issued as the class's move_op_arg (ground: 0x0a, the marshal), a direct one as move_op_code (0x0f)
    if (!c || (order_code != c->move_op_code && order_code != c->move_op_arg)) return false;
    uint32_t ox, oy;
    int      rev;
    if (u->move_microstep >= REVERSE_MS_LIMIT) return false; // the half-slide guard
    if (!reverse_origin(w, player, idx, *u, &ox, &oy, &rev)) return false;
    // O must be free: passable, not a unit's claim, no building / unit record (stricter than the walker's own test, which misses a
    // moving unit with index 0).
    if (w.pass(ox, oy) == 0 || w.pass(ox, oy) == 5) return false;
    auto &to = w.tile(ox, oy);
    if (to.building != 0 || to.class_owner != 0) return false;
    // strictly nearer: equal = retail
    const uint32_t g_x = static_cast<uint32_t>(gx) & w.mask_x(), g_y = static_cast<uint32_t>(gy) & w.mask_y();
    return rsp_dist2(w, ox, oy, g_x, g_y) < rsp_dist2(w, u->x, u->y, g_x, g_y);
}

// The walker's step done backwards. Call only after reverse_decide() said yes (it re-derives O).
template <class W>
inline void reverse_apply(W &w, uint32_t player, int32_t idx) {
    ndg::unit_t *u = w.unit(player, idx);
    uint32_t     ox, oy;
    int          rev;
    if (!u || !reverse_origin(w, player, idx, *u, &ox, &oy, &rev)) return;
    const ndg::cfg_t *c  = w.cfg(*u);
    const uint32_t    nx = u->x, ny = u->y;
    w.note_reverse(player, idx, nx, ny, ox, oy, static_cast<int>(u->move_microstep));
    // vacate N (the walker's "vacate the OLD tile")
    auto &tn       = w.tile(nx, ny);
    tn.class_owner = 0;
    tn.building    = 0;
    w.pass(nx, ny) = u->origin_tile_was_passable;
    w.fow_remove_sight(player, static_cast<int32_t>(nx), static_cast<int32_t>(ny), c->sight);
    // claim O (the walker's "enter the NEW tile")
    auto &to                    = w.tile(ox, oy);
    to.building                 = static_cast<uint16_t>(idx);
    to.class_owner              = static_cast<uint8_t>(player | 0x80u);
    u->origin_tile_was_passable = w.pass(ox, oy);
    w.pass(ox, oy)              = 5;
    u->x                        = static_cast<uint8_t>(ox);
    u->y                        = static_cast<uint8_t>(oy);
    u->facing_target            = static_cast<uint8_t>(rev);
    u->facing_current           = static_cast<uint8_t>(rev);
    u->move_microstep           = static_cast<decltype(u->move_microstep)>(30 - static_cast<int>(u->move_microstep));
    u->path_blocked_retry_count = 0;
    w.fow_update(player, ox, oy, c->sight);
    ndg::soldiers_clear_wander(w, player, idx, *u, *c, true);
}

// decide + apply: true = the unit turned back.
template <class W>
inline bool reverse_try(W &w, uint32_t player, int32_t idx, int32_t gx, int32_t gy, uint16_t order_code, bool boarding) {
    if (!reverse_decide(w, player, idx, gx, gy, order_code, boarding)) return false;
    reverse_apply(w, player, idx);
    return true;
}

} // namespace mh::sim::mvc
