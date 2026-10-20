//
// sim/move/blocked.h -- mods:M1H lane C: the BLOCKED-UNIT protocol (movement plan 3b + 3c + 3d), header-only so the retail splice
// (mh/seams/move_proto_blocked.cpp) and the libmh twin (sim_unit_state_move_walker.cpp) decide from the same code.
//
// The site is the walker's "destination tile is blocked" block, after CALL llm_strat_path_step_check_and_request_detour (0 = wait for
// the occupant, 1 = a detour was requested). Retail (0x0047d50d..0x0047d532): if (detour) retry = 0xc9; old = retry++; if (old <= 0xc8)
// activity_clock += 0.05 (wait) else { retry = 0; state 0xa (marshal / replan); notify 1 }; then the tick budget is zeroed.
//   move_block_wait_ms (3b)       the wait budget: the retry limit becomes clamp(ms / 50, 1, 201) instead of 201. 0 = retail.
//   move_keep_sidestep (3c)       a detour request that spliced a sidestep into THIS unit's own path (the heading at the path cursor no
//                                 longer equals facing_target, the blocked leg's heading) does NOT force the replan: the path is ended
//                                 right after the sidestep, and a keep adds 0x40 to the retry byte (refused once it is >= 0xc0).
//                                 Not applied to attack (0x1a/0x1b/0x1c), patrol (0x10) and squad merge (0x33) orders.
//   move_repath_cooldown_ms (3d)  when the block sends the unit to replan, activity_clock += ms / 1000 s.
//   move_enter_queue (mods:M1BE)  a unit with ORDER 0x24 (enter a storage) is EXEMPT from the three keys above: retail wait limit (201), no
//                                 kept sidestep, no cooldown (enter_queue.h; the walker's nudge site makes it wait behind an entering friend).
// Integer decisions on the unit record and its path buffer only. The one double is the cooldown constant, computed once.
//
#pragma once

#include <cstdint>

#include "addr/mh_structs.gen.h"
#include "sim/move/enter_queue.h" // mods:M1BE: EQ_ORDER_ENTER
#include "sim/move/replan_cap.h"  // mods:M1DT: the block-wait replan tag + the last-blocker memory

namespace mh::sim::mvc {

struct blocked_params {
    int    wait_limit   = 201;   // retry limit; 201 = retail
    bool   keep         = false; // move_keep_sidestep
    bool   cd_on        = false; // move_repath_cooldown_ms != 0
    double cd_s         = 0.0;   // the cooldown in seconds
    bool   reset_moving = false; // move_block_reset_moving (mods:M1DT): the wait budget restarts when the blocker changes to a walking friend
    int    detour_cap   = 0;     // move_replan_detour_cap (mods:M1DT): percent, 0 = off; the block replan is tagged for the marshal check
    bool   enter_queue  = false; // move_enter_queue (mods:M1BE): ORDER 0x24 units ignore wait_limit / keep / cd_on
    // Does the decision path differ from retail at all? (off = the retail block runs)
    bool active() const { return wait_limit != 201 || keep || cd_on || reset_moving || detour_cap > 0; }
};

inline int blocked_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// mods:M1BE: does move_enter_queue exempt this unit from the wait / sidestep / cooldown keys?
template <class U>
inline bool blocked_enter_exempt(const blocked_params &p, const U &u) {
    return p.enter_queue && u.order == EQ_ORDER_ENTER;
}

// The host-synced key values (raw, as written to reimpl_fixes / the [net] ini) -> the gate the decision reads. The same clamps the
// splice setters have always applied: wait 0..10050 ms, cooldown 0..10000 ms.
inline blocked_params make_blocked_params(int block_wait_ms, bool keep_sidestep, int repath_cooldown_ms, bool reset_moving = false, int detour_cap = 0,
                                          bool enter_queue = false) {
    blocked_params p;
    p.enter_queue  = enter_queue;
    const int wait = blocked_clamp(block_wait_ms, 0, 10050);
    if (wait != 0) p.wait_limit = blocked_clamp(wait / 50, 1, 201);
    p.keep         = keep_sidestep;
    p.reset_moving = reset_moving;
    p.detour_cap   = detour_cap < 0 ? 0 : (detour_cap > 1000 ? 1000 : detour_cap);
    const int cool = blocked_clamp(repath_cooldown_ms, 0, 10000);
    if (cool != 0) {
        p.cd_s  = static_cast<double>(cool) / 1000.0;
        p.cd_on = true;
    }
    return p;
}

enum class blocked_next {
    replan, // retry = 0, state 0xa, notify 1 (the retail give-up path); the caller adds the cooldown first when p.cd_on
    wait,   // activity_clock += 0.05 (the retail wait path)
    done,   // nothing more: a kept sidestep (the path was ended after it)
};

constexpr uint16_t BLOCKED_ORDER_ATTACK_UNIT = 0x1a, BLOCKED_ORDER_ATTACK_UNIT_RETURN = 0x1b, BLOCKED_ORDER_ATTACK_BUILDING = 0x1c,
                   BLOCKED_ORDER_PATROL = 0x10, BLOCKED_ORDER_SQUAD_MERGE = 0x33;
constexpr int BLOCKED_PATH_ENTRIES = 300;

// The unit in the way, as the caller reads it off the destination tile (tile_objects[dest]): idx = its index when it is a unit of the
// walker's own side (class_owner == player | 0x80), else 0; walking_friend = that unit is a ground mover in state 0x0f (move_walker).
struct blocker_info {
    uint16_t idx            = 0;
    bool     walking_friend = false;
};

// u = the walking unit; detour = step_check's return value (the splice's EAX); wp(slot, entry) -> mh_llm_strat_path_waypoint & of this
// unit's player. Writes u.path_blocked_retry_count (wait / keep) and, for a kept sidestep, ends the path.
template <class WP>
inline blocked_next blocked_decide_impl(mh::game::mh_map_object_unit &u, bool detour, const blocked_params &p, WP &&wp, const blocker_info &bi) {
    uint8_t retry = u.path_blocked_retry_count;
    if (detour) {
        if (!p.keep || retry >= 0xc0) return blocked_next::replan;
        const uint16_t order = u.order;
        if (order == BLOCKED_ORDER_ATTACK_UNIT || order == BLOCKED_ORDER_ATTACK_UNIT_RETURN || order == BLOCKED_ORDER_ATTACK_BUILDING ||
            order == BLOCKED_ORDER_PATROL || order == BLOCKED_ORDER_SQUAD_MERGE)
            return blocked_next::replan;
        const uint8_t slot = u.path_slot_id;
        if (slot >= 100) return blocked_next::replan;
        const int32_t cur = u.path_cursor;
        if (cur < 0 || cur + 2 >= BLOCKED_PATH_ENTRIES) return blocked_next::replan;
        const uint8_t leg = u.facing_target; // the blocked leg's heading (the walker only steps when they match)
        const uint8_t h   = wp(slot, cur).heading;
        if (h == 0 || h == leg) return blocked_next::replan;                           // no sidestep of OUR path was spliced
        const int k                  = ((static_cast<int>(leg) - 1) % 6 == 0) ? 1 : 2; // cardinal: keep the probe step only
        wp(slot, cur + k).heading    = 0;                                              // heading 0 = path end -> the retail path-end rule replans from there
        wp(slot, cur + k).run_length = 0;
        u.path_blocked_retry_count   = static_cast<uint8_t>(retry + 0x40);
        return blocked_next::done;
    }
    // mods:M1DT (a) move_block_reset_moving: the unit in the way changed to a WALKING friend (the queue ahead moved on): the wait budget
    // restarts. Retail counts retries since THIS unit last stepped, so a follower in a moving queue timed out and replanned round the
    // long way. Only a walking own ground unit resets; a wall, an idle friend or an enemy never does.
    if (p.reset_moving && bi.idx != 0) {
        if (bi.walking_friend && bi.idx != rc_last_blocker(u) && retry != 0) {
            u.path_blocked_retry_count = 0;
            retry                      = 0;
        }
        rc_set_last_blocker(u, bi.idx);
    }
    if (static_cast<int>(retry) < p.wait_limit) {
        u.path_blocked_retry_count = static_cast<uint8_t>(retry + 1);
        return blocked_next::wait;
    }
    return blocked_next::replan;
}

// The block decision: the retail protocol above, then (mods:M1DT (b)) a replan is TAGGED so the marshal can refuse a detour route.
template <class WP>
inline blocked_next blocked_decide(mh::game::mh_map_object_unit &u, bool detour, const blocked_params &p, WP &&wp, const blocker_info &bi = blocker_info{}) {
    if (blocked_enter_exempt(p, u)) { // mods:M1BE: the retail wait budget, no kept sidestep (the cooldown is the caller's, it asks blocked_enter_exempt too)
        blocked_params q     = p;
        q.wait_limit         = 201;
        q.keep               = false;
        q.cd_on              = false;
        const blocked_next r = blocked_decide_impl(u, detour, q, wp, bi);
        if (r == blocked_next::replan && q.detour_cap > 0) rc_tag(u);
        return r;
    }
    const blocked_next r = blocked_decide_impl(u, detour, p, wp, bi);
    if (r == blocked_next::replan && p.detour_cap > 0) rc_tag(u);
    return r;
}

} // namespace mh::sim::mvc
