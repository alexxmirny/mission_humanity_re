//
// sim/move/replan_cap.h -- mods:M1DT fix 4: the block-wait REPLAN must not take a long way round while the friends in the way are
// still walking. Header-only; the retail splices (mh/seams/move_proto_blocked.cpp, move_proto_nudge.cpp) and the libmh twins
// (sim_unit_state_move_walker.cpp, sim_unit_state_group_marshal.cpp) run the same code.
//
// THE MECHANISM (tmp/detour dig, 2026-10-19). A unit blocked by a friend waits move_block_wait_ms, then goes to state 0x0a (group
// marshal) and the planner routes it again with every parked friend a wall: a 34-39 tile loop instead of 12-18. Two rules:
//   (a) move_block_reset_moving: the wait budget restarts when the unit in the way CHANGES to a walking friend (the queue moved). That
//       is blocked.h; this header only holds the one remembered number (the last blocker).
//   (b) move_replan_detour_cap (percent, 0 = off): the block-wait replan is tagged on the unit (rc_tag). When the marshal then attaches
//       the new route, rc_should_refuse() compares its length with the straight distance to the goal; longer than cap% (+ a slack of
//       RC_SLACK_TILES tiles) and the route is DISCARDED: the path slot is freed and the unit stays in state 0xa with its clock
//       advanced 2 s (retail's own no-path retry idiom), so it plans again after the queue has moved. At most RC_MAX_REFUSALS
//       refusals in a row, then the route is taken whatever its length (a truly walled unit still detours, after 3 x 2 s).
//
// STATE: the six pad bytes after move_group_id (_pad_0xca, nothing reads them) hold the tag and the last blocker:
//   [0] tag: 0 = none, else 1 + refusals so far   [1] [2] goal x, y at the time of the tag   [3] order (low byte)
//   [4] [5] index of the last blocker the walker saw (little endian, 0 = none)
// A tag is valid only while goal and order still match, so a fresh order never inherits it. Both are unit-record bytes the lockstep
// hash and a savegame already carry, written identically by the splice and the twin.
//
#pragma once

#include <cstdint>

namespace mh::sim::mvc {

constexpr int RC_MAX_REFUSALS = 3;
constexpr int RC_SLACK_TILES  = 2;
constexpr int RC_PATH_ENTRIES = 300;

// Orders whose path end settles instead of replanning (see blocked.h): never tagged.
inline bool rc_order_eligible(uint16_t order) { return !(order == 0x1a || order == 0x1b || order == 0x1c || order == 0x10 || order == 0x33); }

template <class U>
inline void rc_tag(U &u) {
    if (!rc_order_eligible(u.order)) return;
    u._pad_0xca[0] = 1;
    u._pad_0xca[1] = u.goal_x;
    u._pad_0xca[2] = u.goal_y;
    u._pad_0xca[3] = static_cast<uint8_t>(u.order);
}

template <class U>
inline bool rc_tag_valid(const U &u) {
    return u._pad_0xca[0] != 0 && u._pad_0xca[1] == u.goal_x && u._pad_0xca[2] == u.goal_y && u._pad_0xca[3] == static_cast<uint8_t>(u.order);
}

template <class U>
inline uint16_t rc_last_blocker(const U &u) {
    return static_cast<uint16_t>(u._pad_0xca[4] | (u._pad_0xca[5] << 8));
}
template <class U>
inline void rc_set_last_blocker(U &u, uint16_t idx) {
    u._pad_0xca[4] = static_cast<uint8_t>(idx);
    u._pad_0xca[5] = static_cast<uint8_t>(idx >> 8);
}

// The length in steps of the route in `slot` (run lengths summed up to the first empty entry).
template <class WP>
inline int rc_route_len(uint32_t slot, WP &&wp) {
    int len = 0;
    for (int e = 0; e < RC_PATH_ENTRIES; ++e) {
        const int rl = wp(slot, e).run_length;
        if (rl == 0) break;
        len += rl;
    }
    return len;
}

// Marshal good-path hook. wp(slot, entry) -> waypoint {heading, run_length}. True = discard the route (the caller frees the slot and
// advances the clock); the tag then counts the refusal. False = take it (the tag is cleared once the route is accepted).
template <class U, class WP>
inline bool rc_should_refuse(U &u, int cap_pct, uint32_t width_mask, uint32_t height_mask, WP &&wp) {
    if (cap_pct <= 0 || !rc_tag_valid(u)) return false;
    const uint8_t slot = u.path_slot_id;
    if (slot >= 100) return false;
    const int len  = rc_route_len(slot, wp);
    auto      dist = [](int a, int b, uint32_t mask) {
        const int d    = a > b ? a - b : b - a;
        const int span = static_cast<int>(mask) + 1;
        return d * 2 > span ? span - d : d;
    };
    const int  dx = dist(u.x, u.goal_x, width_mask), dy = dist(u.y, u.goal_y, height_mask);
    const int  direct   = dx > dy ? dx : dy;
    const bool too_long = static_cast<long long>(len) * 100 > static_cast<long long>(cap_pct) * direct + 100LL * RC_SLACK_TILES;
    const int  refusals = u._pad_0xca[0] - 1;
    if (!too_long || refusals >= RC_MAX_REFUSALS) {
        u._pad_0xca[0] = 0;
        return false;
    }
    u._pad_0xca[0] = static_cast<uint8_t>(u._pad_0xca[0] + 1);
    return true;
}

} // namespace mh::sim::mvc
