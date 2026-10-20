//
// sim/sim_move_fixes.h -- mods:M1H lane A: the libmh-side switchboard of the movement / group-movement / combat prototype keys.
//
// WHY THIS EXISTS. mh.dll carries each [net] improved_movement / group_movement key as a byte splice into a RETAIL function
// (seams/net_lockstep.cpp, move_proto_*.cpp). In the libmh configuration those retail functions are never reached -- libmh fills
// the unit-state dispatch table with its own bodies -- so the key does nothing unless the libmh body has a TWIN of the rule.
// Each twin reads its key from here. Lanes B (planner twins) and C (walker / marshal twins) read these fields; this header only
// holds the values, so that the plumbing (reimpl_fixes -> set_fixes -> here) lands once and the twins land independently.
//
// DEFAULTS ARE THE FAITHFUL STOCK: every flag off, every numeric knob at its retail value. A build that never calls set_fixes
// therefore behaves exactly as the original did, which keeps the libmh golden A/B and the retail oracles meaningful.
//
// The five keys that already have twins keep their own holders (sim_path_group_steps.h, sim_unit_state_squad_merge, sim_merge_goal.h,
// sim_attack_range_footprint.h, sim_projectile_facing.h); they are NOT repeated here.
//
#pragma once

namespace mh::sim {

struct move_fixes {
    // improved_movement: the planner
    bool centroid_wrap_fix         = false; // move_centroid_wrap_fix
    bool detour_slot_guard         = false; // move_detour_slot_guard
    bool flood_queue_guard         = false; // move_flood_queue_guard
    int  route_cost_mode           = 0;     // 0 retail raw byte, nonzero = the cost table
    int  route_cost_base           = 1;     // retail 1
    int  route_cost_ring4          = 4;     // retail 4
    int  route_cost_ring8          = 8;     // retail 8
    int  route_cost_ring16         = 16;    // retail 16
    int  route_gap_wave_full       = 0;     // 0 = off
    int  route_corridor_mode       = 0;     // 0 retail / 1 corridor + adjacent / 2 all regions / 3 measure only
    bool route_flood_exact         = false; // route_flood_exact: phase 2 of the region flood as a bucket queue (sim/move/flood.h)
    bool distinct_goals            = false; // move_distinct_goals
    int  near_enough               = 0;     // move_near_enough: R tiles, 0 = off
    bool route_through_idle        = false; // move_route_through_idle (nonzero)
    bool route_through_moving      = false; // move_route_through_moving: a walking friend's tile is open to the planner too
    int  route_through_moving_cost = 0;     // move_route_through_moving_cost: the cost level 0..4 (sim/move/route_through.h)
    bool route_through_replanning  = false; // move_route_through_replanning (mods:M1DT): a re-planning (state 0x0a) friend's tile is open too
    // improved_movement: the walker / marshal
    int  block_wait_ms      = 0;     // move_block_wait_ms: 0 = retail (201 retries)
    bool keep_sidestep      = false; // move_keep_sidestep
    int  repath_cooldown_ms = 0;     // move_repath_cooldown_ms
    bool block_reset_moving = false; // move_block_reset_moving (mods:M1DT): the wait budget restarts when the blocker changes to a walking friend
    int  replan_detour_cap  = 0;     // move_replan_detour_cap (mods:M1DT): percent of the straight distance a block-wait replan may take, 0 = off
    int  formation_click    = 0;     // move_formation_click (mods:M1G 4b): 0 = off, 1..2 = the old keep rule with N - 1 rings of slack, 3 = the formation moves as one (sim/move/formation.h)
    bool enter_queue        = false; // move_enter_queue (mods:M1BE): ORDER 0x24 units wait behind an entering friend and ignore the block keys (sim/move/enter_queue.h)
    bool nudge_idle         = false; // move_nudge_idle
    bool claim_leader       = false; // move_claim_leader
    bool swap_headon        = false; // move_swap_headon
    // improved_movement: storage doors (mods:M1BE)
    bool storage_door_hold = false; // storage_door_hold: an open storage door stays open while an entering friend is within 2 tiles (sim/move/enter_queue.h)
    // improved_movement: responsiveness (mods:M1G 4c)
    bool reverse_now = false; // move_reverse_now: slide back at once when a new goal lies behind
    int  turn_rate   = 0;     // move_turn_rate: walker turns 2^k times faster (0 retail, 0..2)
    // group_movement
    bool group_speed = false; // move_group_speed
};

namespace move_fixes_detail {
inline move_fixes &store() {
    static move_fixes f;
    return f;
}
} // namespace move_fixes_detail

// Set by lockstep::set_fixes (turn_engine.cpp) from the host-synced reimpl_fixes fields of the same names.
inline void              set_move_fixes(const move_fixes &f) { move_fixes_detail::store() = f; }
inline const move_fixes &move_fix() { return move_fixes_detail::store(); }

} // namespace mh::sim
