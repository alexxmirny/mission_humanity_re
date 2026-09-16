//
// sim/sim_unit_state_move_walker.cpp -- see sim_unit_state_move_walker.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_unit_state_move_walker_0047c903.asm) -- see the header banner for
// the fine_x/fine_y field-offset derivation, the fine->tile shift-form transcription note, and the
// unbacked-enum literal derivation.
//
#include "sim/sim_unit_state_move_walker.h"

#include "addr/mh_calls.gen.h"     // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"           // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/sim_order_enqueue.h" // UNIT_STATE_STOP_TO_DEFAULT, UNIT_STATE_PATROL_SWAP (shared there)
#include "addr/mh_rebind.gen.h"    // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_state_move_walker_calls &live_unit_state_move_walker_calls() {
    static const unit_state_move_walker_calls c = {
        MH_LIBMH_BIND(llm_strat_dir_step_factor),
        MH_LIBMH_BIND(llm_strat_target_class),
        MH_LIBMH_BIND(llm_strat_unit_in_weapon_range),
        MH_LIBMH_BIND(llm_strat_unit_goal_in_weapon_range),
        MH_LIBMH_BIND(llm_strat_unit_fire_at_target_if_aimed),
        MH_LIBMH_BIND(llm_strat_unit_set_state_order),
        MH_LIBMH_BIND(llm_strat_unit_set_state),
        MH_LIBMH_BIND(llm_strat_unit_notify_status),
        MH_LIBMH_BIND(llm_strat_target_release_ref),
        MH_LIBMH_BIND(llm_strat_unit_order_move_auto),
        MH_LIBMH_BIND(llm_strat_unit_get_coords),
        MH_LIBMH_BIND(llm_strat_bldg_get_coords),
        MH_LIBMH_BIND(llm_strat_bldg_footprint_random_offset),
        MH_LIBMH_BIND(llm_strat_facing24_to_delta),
        MH_LIBMH_BIND(llm_strat_path_step_check_and_request_detour),
        MH_LIBMH_BIND(llm_strat_fow_remove_sight),
        MH_LIBMH_BIND(map_fow_UpdateFoWPlus),
        MH_LIBMH_BIND(llm_strat_unit_soldiers_set_heading),
        MH_LIBMH_BIND(llm_strat_unit_walk_step_allowed),
    };
    return c;
}

namespace detail {

// ---- llm_strat_unit_state (order/state) literals -- NOT backed by a real Ghidra enum (see the
// header banner: sim_unit_state_predicates.h already investigated this same field and found the
// decompiler's printed names have no backing Data Type Manager enum). Own local constants
// (MOVE_WALKER_STATE_ prefix), matching every sibling TU's identical convention for this field.
inline constexpr uint16_t MOVE_WALKER_STATE_ATTACK_UNIT        = 0x1a; // 0x0047ca32
inline constexpr uint16_t MOVE_WALKER_STATE_ATTACK_UNIT_RETURN = 0x1b; // 0x0047ca3e
inline constexpr uint16_t MOVE_WALKER_STATE_ATTACK_BUILDING    = 0x1c; // 0x0047cd47
inline constexpr uint16_t MOVE_WALKER_STATE_SQUAD_MERGE        = 0x33; // 0x0047cf92, plate: SQUAD_MERGE
inline constexpr uint16_t MOVE_WALKER_STATE_GROUP_MARSHAL      = 0x0a; // 0x0047cc43/cfa0/d53f

// llm_strat_unit_notify_status's status-code literals this function passes -- also unbacked (no
// Ghidra enum), named locally per the same convention.
inline constexpr uint32_t MOVE_WALKER_NOTIFY_STILL_CHASING = 0x66; // "in progress" (moving/engaging)
inline constexpr uint32_t MOVE_WALKER_NOTIFY_GOAL_REACHED  = 0x65; // arrived at goal tile
inline constexpr uint32_t MOVE_WALKER_NOTIFY_REPLAN        = 1;    // exhausted/blocked -> re-plan bailout

// _DAT_005013c0 / _DAT_005013c8, read-memory-confirmed by the conductor (batch SIM1-G1 context.md).
inline constexpr double MOVE_WALKER_TERRAIN_SCALE_MULT    = 0.5;  // 0x005013c0
inline constexpr double MOVE_WALKER_BLOCKED_ACTIVITY_BUMP = 0.05; // 0x005013c8

// tile_object::unit and unit::unit_above are both `uint8_t[2]` in the generated header (the generator
// renders map::t::unit_full_id as a raw byte pair, not a scalar) -- reassembles the little-endian word
// the original addresses with a single `MOVZX reg, word ptr [...]` at 0x0047d49f. Same idiom as
// sim_combat_kill_credit.cpp's local helper of the same name; not shared across translation units (each
// TU that needs it defines its own, per the "no new shared helpers" rule). Used here as a raw index
// into THIS player's soldier roster (the head-soldier record), not decoded into owner/index halves.
inline uint16_t unit_full_id_word(const uint8_t (&packed)[2]) {
    return static_cast<uint16_t>(packed[0] | (packed[1] << 8));
}

// ---- fine->tile: the SHIFT FORM, transcribed bit-for-bit per the batch context override (see the
// header banner) -- NOT simplified to `fine / 32` despite that being provably the same value per this
// codebase's own established fine_to_tile() precedent. Reproduces 0x0047caac-0x0047cac3 (and its four
// sibling occurrences in this function) exactly: SAR EDX,0x1f; SHL EDX,0x5; SBB EAX,EDX; SAR EAX,0x5.
inline int32_t fine_to_tile(int32_t fine) {
    const int32_t sign_mask = fine >> 31;     // SAR EDX,0x1f -- all-1s if fine<0, else 0
    const int32_t bias      = sign_mask << 5; // SHL EDX,0x5 -- -32 if fine<0, else 0
    // SBB EAX,EDX subtracts the bias AND the borrow-in left by the preceding SHL; the Ghidra draft's
    // own transcription of that borrow is `(uint)((sign_mask << 4) < 0)`, which is 1 iff fine<0 and 0
    // otherwise -- reproduced literally rather than simplified to "if fine<0".
    const int32_t borrow = (sign_mask << 4) < 0 ? 1 : 0;
    return (fine - bias - borrow) >> 5;
}

void unit_state_move_walker(const sim_view &v, sim_store &own, const unit_state_move_walker_calls &c) {
    unit          &u          = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced -- see sim_view::cur_unit's comment.
    const uint16_t player     = *v.cur_player;
    const int32_t  unit_index = static_cast<int32_t>(*v.cur_index);

    // 0x0047c91b-0x0047c94a: a queued order is about to replace this state -- do nothing this tick.
    if (u.order_queued != 0 && u.move_microstep == 0x1f) {
        own.tick_budget() = 0.0;
        return;
    }

    // 0x0047c94f-0x0047c990: microstep cost = cfg step_speed[player] * move_step_speed_scale *
    // dir_step_factor(facing_target). Spent per sub-tile microstep below; ALSO reused as the STEP
    // (not turn) budget gate later in this function (0x0047d249), since it depends only on values
    // that do not change between here and there.
    const double step_cost = v.cfg_units[u.unit_proto_id].step_speed[player] * u.move_step_speed_scale *
                             c.dir_step_factor(static_cast<int32_t>(u.facing_target));

    if (u.move_microstep < 0x1f) {
        // ---- mid-step: spend the microstep budget -------------------------------------------------
        if (own.tick_budget() < step_cost) {
            // 0x0047c9b3-0x0047c9d8: insufficient budget -- push the shortfall into activity_clock and
            // drain tick_budget to 0 (the budget-carryover idiom every tick-budget-gated sim TU uses).
            u.activity_clock -= own.tick_budget();
            own.tick_budget() = 0.0;
            return;
        }
        own.tick_budget() -= step_cost; // 0x0047c9dd-0x0047c9e6
        u.move_microstep += 1;          // 0x0047c9ec-0x0047c9f1

        if (u.move_microstep != 0x1f) {
            return; // still mid sub-tile animation; nothing else to do this tick.
        }

        // 0x0047ca09-0x0047ca26: the sub-tile animation just finished -- settle at the current tile.
        own.passable_at(u.x, u.y) = 0;

        // 0x0047ca32-0x0047cd42: re-check the combat order this microstep-completion may have earned.
        if (u.order == MOVE_WALKER_STATE_ATTACK_UNIT || u.order == MOVE_WALKER_STATE_ATTACK_UNIT_RETURN) {
            const uint32_t target_owner = ref_owner(static_cast<uint32_t>(u.target_ref));
            const uint16_t target_slot  = static_cast<uint16_t>(u.target_index);
            if (v.units[target_owner * v.caps.units + target_slot].energy <= 0.0) {
                // 0x0047cc6a-0x0047cd2e: target is dead -- release, notify, maybe return home, stop.
                if (u.target_ref != 0) {
                    c.target_release_ref(player, unit_index, 1u);
                    u.target_ref   = 0;
                    u.target_index = 0;
                }
                c.unit_notify_status(player, unit_index, MOVE_WALKER_NOTIFY_STILL_CHASING);
                if (u.order == MOVE_WALKER_STATE_ATTACK_UNIT_RETURN &&
                    (u.x != u.home_x || u.y != u.home_y)) {
                    c.unit_order_move_auto(player, unit_index, u.home_x, u.home_y);
                }
                c.unit_set_state_order(UNIT_STATE_STOP_TO_DEFAULT, UNIT_STATE_STOP_TO_DEFAULT);
            } else {
                // 0x0047ca88-0x0047cc65: target still alive -- in weapon range?
                const int32_t target_class =
                    c.target_class(static_cast<uint32_t>(static_cast<uint16_t>(u.target_ref)),
                                   static_cast<int32_t>(target_slot));
                const int32_t in_range =
                    c.unit_in_weapon_range(player, unit_index, fine_to_tile(u.target_fine_x),
                                           fine_to_tile(u.target_fine_y), target_class);
                if (in_range != 0) {
                    c.unit_fire_at_target_if_aimed();
                    c.unit_set_state_order(u.order, UNIT_STATE_STOP_TO_DEFAULT);
                    c.unit_notify_status(player, unit_index, MOVE_WALKER_NOTIFY_STILL_CHASING);
                } else if ((u.path_cursor + 1) % 4 == 0 &&
                           v.path_buffers[player * PATH_WAYPOINTS_PER_PLAYER +
                                          u.path_slot_id * PATH_WAYPOINTS_PER_SLOT + u.path_cursor]
                                   .run_length == 0) {
                    // 0x0047cb8d-0x0047cc65: every 4th spent waypoint, re-fetch the target's live fine
                    // position and re-check with the GOAL-in-range variant; only bail to GROUP_MARSHAL
                    // when even that says out of range.
                    c.unit_get_coords(static_cast<uint16_t>(target_owner), static_cast<int32_t>(target_slot),
                                      &u.target_fine_x, &u.target_fine_y);
                    const int32_t target_class_2 =
                        c.target_class(static_cast<uint32_t>(static_cast<uint16_t>(u.target_ref)),
                                       static_cast<int32_t>(target_slot));
                    const int32_t goal_in_range =
                        c.unit_goal_in_weapon_range(player, unit_index, fine_to_tile(u.target_fine_x),
                                                    fine_to_tile(u.target_fine_y), target_class_2);
                    if (goal_in_range == 0) {
                        c.unit_set_state(MOVE_WALKER_STATE_GROUP_MARSHAL);
                        c.unit_notify_status(player, unit_index, MOVE_WALKER_NOTIFY_STILL_CHASING);
                    }
                }
            }
        } else if (u.order == MOVE_WALKER_STATE_ATTACK_BUILDING) {
            const uint32_t target_owner = ref_owner(static_cast<uint32_t>(u.target_ref));
            const uint16_t target_slot  = static_cast<uint16_t>(u.target_index);
            if (v.buildings[target_owner * v.caps.buildings + target_slot].energy <= 0.0) {
                // 0x0047ce38-0x0047ce9d: target building is destroyed -- release, stop, notify.
                if (u.target_ref != 0) {
                    c.target_release_ref(player, unit_index, 1u);
                    u.target_ref   = 0;
                    u.target_index = 0;
                }
                c.unit_set_state_order(UNIT_STATE_STOP_TO_DEFAULT, UNIT_STATE_STOP_TO_DEFAULT);
                c.unit_notify_status(player, unit_index, MOVE_WALKER_NOTIFY_STILL_CHASING);
            } else {
                // 0x0047cd91-0x0047ce36: target still standing -- fire if in range, else do nothing.
                const int32_t target_class =
                    c.target_class(static_cast<uint32_t>(static_cast<uint16_t>(u.target_ref)),
                                   static_cast<int32_t>(target_slot));
                const int32_t in_range =
                    c.unit_in_weapon_range(player, unit_index, fine_to_tile(u.target_fine_x),
                                           fine_to_tile(u.target_fine_y), target_class);
                if (in_range != 0) {
                    c.unit_fire_at_target_if_aimed();
                    c.unit_set_state_order(u.order, UNIT_STATE_STOP_TO_DEFAULT);
                    c.unit_notify_status(player, unit_index, MOVE_WALKER_NOTIFY_STILL_CHASING);
                }
            }
        }
        // else: order is neither ATTACK_UNIT(_RETURN) nor ATTACK_BUILDING -- nothing further this tick.
        return;
    }

    // ---- 0x0047cea7-...: already settled at a tile boundary (move_microstep >= 0x1f) -----------------
    // Read via the const view (v.path_buffers) throughout except the one true write (the run_length
    // decrement on an actual step, further below, which goes through path_buffer_at()).
    auto path_at = [&](int32_t cursor) -> const path_waypoint & {
        return v.path_buffers[player * PATH_WAYPOINTS_PER_PLAYER + u.path_slot_id * PATH_WAYPOINTS_PER_SLOT +
                              cursor];
    };

    // 0x0047cea7-0x0047ceeb: skip a spent waypoint. Uses the UNADVANCED cursor for the run_length read.
    if (path_at(u.path_cursor).run_length == 0) {
        u.path_cursor += 1;
    }

    if (path_at(u.path_cursor).heading == 0) {
        // ---- 0x0047cf28-0x0047d17d: path exhausted (terminal sentinel) -------------------------------
        const bool at_goal = (u.x == u.goal_x && u.y == u.goal_y);
        const bool should_settle =
            at_goal || u.order == MOVE_WALKER_STATE_ATTACK_BUILDING ||
            u.order == MOVE_WALKER_STATE_ATTACK_UNIT || u.order == MOVE_WALKER_STATE_ATTACK_UNIT_RETURN ||
            u.order == UNIT_STATE_PATROL_SWAP || u.order == MOVE_WALKER_STATE_SQUAD_MERGE;

        if (!should_settle) {
            // 0x0047cfa0-0x0047cfc2: still genuinely mid-journey with an exhausted buffer -- re-plan.
            c.unit_set_state(MOVE_WALKER_STATE_GROUP_MARSHAL);
            c.unit_notify_status(player, unit_index, MOVE_WALKER_NOTIFY_REPLAN);
            return;
        }

        // 0x0047cfc7-0x0047d17d: commit STOP_TO_DEFAULT (order kept as-is), then, for the two ATTACK_*
        // `state` values specifically, a final in-range check (re-rolling the ATTACK_BUILDING footprint
        // offset on a miss) before the goal/still-chasing notify.
        c.unit_set_state_order(u.order, UNIT_STATE_STOP_TO_DEFAULT);

        if (u.state == MOVE_WALKER_STATE_ATTACK_BUILDING || u.state == MOVE_WALKER_STATE_ATTACK_UNIT) {
            int32_t fine_x, fine_y;
            if (u.state == MOVE_WALKER_STATE_ATTACK_BUILDING) {
                c.bldg_get_coords(player, unit_index, &fine_x, &fine_y);
            } else {
                fine_x = u.target_fine_x;
                fine_y = u.target_fine_y;
            }
            const uint16_t target_slot = static_cast<uint16_t>(u.target_index);
            const int32_t  target_class =
                c.target_class(static_cast<uint32_t>(static_cast<uint16_t>(u.target_ref)),
                               static_cast<int32_t>(target_slot));
            const int32_t in_range = c.unit_in_weapon_range(player, unit_index, fine_to_tile(fine_x),
                                                            fine_to_tile(fine_y), target_class);
            if (in_range == 0) {
                c.unit_set_state_order(static_cast<uint16_t>(v.cfg_units[u.unit_proto_id].move_op_arg),
                                       u.state);
            }
        }

        if (u.state == MOVE_WALKER_STATE_ATTACK_BUILDING) {
            const uint32_t target_owner = ref_owner(static_cast<uint32_t>(u.target_ref));
            // u.target_fine_x/y are plain int32_t struct fields ([fine_coord], mh_structs.gen.h),
            // used as int32_t everywhere else; bldg_footprint_random_offset's committed out-params
            // are uint32_t * (TACT1-P C6, 2026-09-04) -- same 32-bit quantity, cast at this site.
            c.bldg_footprint_random_offset(player, static_cast<uint32_t>(unit_index), target_owner,
                                           static_cast<int32_t>(static_cast<uint16_t>(u.target_index)),
                                           reinterpret_cast<uint32_t *>(&u.target_fine_x),
                                           reinterpret_cast<uint32_t *>(&u.target_fine_y));
        }

        if (u.x == u.goal_x && u.y == u.goal_y) {
            c.unit_notify_status(player, unit_index, MOVE_WALKER_NOTIFY_GOAL_REACHED);
        } else {
            c.unit_notify_status(player, unit_index, MOVE_WALKER_NOTIFY_STILL_CHASING);
        }
        return;
    }

    // ---- 0x0047d182-...: a real next step is queued -- compute the destination tile ------------------
    const uint32_t old_x = u.x;
    const uint32_t old_y = u.y;
    int32_t        dx = 0, dy = 0;
    c.facing24_to_delta(path_at(u.path_cursor).heading, &dx, &dy);
    const uint32_t new_x = map_width_mask(v) & (old_x + static_cast<uint32_t>(dx));
    const uint32_t new_y = map_height_mask(v) & (old_y + static_cast<uint32_t>(dy));

    if (path_at(u.path_cursor).heading == u.facing_target) {
        // ---- STEP: facing already points the right way ---------------------------------------------
        if (own.tick_budget() < step_cost) {
            // 0x0047d257-0x0047d27c: insufficient budget -- same carryover idiom as above.
            u.activity_clock -= own.tick_budget();
            own.tick_budget() = 0.0;
            return;
        }

        if (own.passable_at(new_x, new_y) == 0 || own.tile_object_at(new_x, new_y).building != 0) {
            // 0x0047d4fc-0x0047d574: destination is blocked -- request a detour, escalate retries.
            const int32_t detour_requested =
                c.path_step_check_and_request_detour(old_x, old_y, static_cast<int32_t>(new_x),
                                                     static_cast<int32_t>(new_y));
            if (detour_requested != 0) {
                u.path_blocked_retry_count = 0xc9;
            }
            const uint8_t old_retry_count = u.path_blocked_retry_count;
            u.path_blocked_retry_count += 1;
            if (old_retry_count < 0xc9) { // asm: CMP DL,0xc8; JBE (unsigned <=0xc8, i.e. <0xc9)
                u.activity_clock += MOVE_WALKER_BLOCKED_ACTIVITY_BUMP;
            } else {
                u.path_blocked_retry_count = 0;
                c.unit_set_state(MOVE_WALKER_STATE_GROUP_MARSHAL);
                c.unit_notify_status(player, unit_index, MOVE_WALKER_NOTIFY_REPLAN);
            }
            own.tick_budget() = 0.0;
            return;
        }

        // 0x0047d2b0-0x0047d463: destination is clear -- take the step.
        own.tick_budget() -= step_cost;

        // Vacate the OLD tile: tile_objects[old] cleared, passable[old] restored from the unit's own
        // cached snapshot (0x0047d2cd-0x0047d300).
        own.tile_object_at(old_x, old_y).class_owner = 0;
        own.tile_object_at(old_x, old_y).building    = 0;
        own.passable_at(old_x, old_y)                = u.origin_tile_was_passable;

        c.fow_remove_sight(player, static_cast<int32_t>(old_x), static_cast<int32_t>(old_y),
                           v.cfg_units[u.unit_proto_id].sight);

        // The path_buffer_at() write: decrement run_length at the current cursor (0x0047d364-0x0047d35e).
        own.path_buffer_at(player, u.path_slot_id, u.path_cursor).run_length -= 1;

        // Enter the NEW tile: tile_objects[new] claimed by this unit, origin_tile_was_passable snapshot
        // taken BEFORE passable[new] is overwritten to the "occupied by a moving unit" sentinel (5)
        // (0x0047d372-0x0047d400).
        own.tile_object_at(new_x, new_y).building    = static_cast<uint16_t>(unit_index);
        own.tile_object_at(new_x, new_y).class_owner = static_cast<uint8_t>(player | 0x80u);
        u.origin_tile_was_passable                   = own.passable_at(new_x, new_y);

        // Terrain-cost update, EXACT ORDER per the disassembly (0x0047d3d1-0x0047d3f1): the new tile's
        // passable value (widened to double) is ADDED to the current move_step_speed_scale first, THEN
        // the sum is halved -- an averaging filter, not an overwrite.
        u.move_step_speed_scale = static_cast<double>(own.passable_at(new_x, new_y)) + u.move_step_speed_scale;
        u.move_step_speed_scale = u.move_step_speed_scale * MOVE_WALKER_TERRAIN_SCALE_MULT;

        own.passable_at(new_x, new_y) = 5;
        u.x                           = static_cast<uint8_t>(new_x);
        u.y                           = static_cast<uint8_t>(new_y);
        u.move_microstep              = 0;
        u.path_blocked_retry_count    = 0;

        c.map_fow_UpdateFoWPlus(player, new_x, new_y, v.cfg_units[u.unit_proto_id].sight);

        // 0x0047d484-0x0047d4f7: soldier idle-wander clear, gated on BOTH cfg soldier_count>0 AND the
        // head soldier's idle_wander_flag!=0 (the asm re-reads unit_proto_id via fresh roster
        // arithmetic here; same value as u.unit_proto_id already read above).
        if (v.cfg_units[u.unit_proto_id].soldier_count > 0) {
            const uint16_t soldier_idx  = unit_full_id_word(u.unit_above);
            soldier       &head_soldier = own.soldier_at(player, soldier_idx);
            if (head_soldier.idle_wander_flag != 0) {
                c.unit_soldiers_set_heading(player, unit_index, u.facing_target);
                head_soldier.idle_wander_flag = 0;
            }
        }
        return;
    }

    // ---- TURN: facing does not yet point the right way -------------------------------------------
    const double turn_cost = v.cfg_units[u.unit_proto_id].turn_speed[player] * u.move_step_speed_scale;
    if (own.tick_budget() < turn_cost) {
        // 0x0047d5cb-0x0047d5f0: same carryover idiom as the two budget gates above.
        u.activity_clock -= own.tick_budget();
        own.tick_budget() = 0.0;
        return;
    }
    own.tick_budget() -= turn_cost;

    // 0x0047d604-0x0047d69e: shortest-arc rotation direction toward the path's heading. TWO CHAINED
    // range/direction tests in the disassembly (0x0047d656-0x0047d667, then on fallthrough
    // 0x0047d669-0x0047d67a) -- verified boolean-equivalent to the single OR below at every diff
    // boundary (+-12 and adjacent), but this collapses two branches into one expression, so it is
    // flagged in uncertainties as a reassembled-control-flow judgment call per the translator brief.
    const int32_t path_heading   = path_at(u.path_cursor).heading;
    const int32_t facing_target  = u.facing_target;
    const int32_t facing_current = u.facing_current;
    const int32_t diff           = facing_target - path_heading;
    const bool    go_increment   = (diff > 12 && facing_target > path_heading) ||
                              (diff > -12 && facing_target < path_heading);

    int32_t new_facing_target, new_facing_current;
    if (go_increment) {
        // 0x0047d67e-0x0047d69e.
        new_facing_target  = facing_target + 1;
        new_facing_current = facing_current + 1;
        if (new_facing_target > 0x18) new_facing_target -= 0x18;
        if (new_facing_current > 0x18) new_facing_current -= 0x18;
    } else {
        // 0x0047d6a0-0x0047d6c0.
        new_facing_target  = facing_target - 1;
        new_facing_current = facing_current - 1;
        if (new_facing_target < 1) new_facing_target += 0x18;
        if (new_facing_current < 1) new_facing_current += 0x18;
    }

    // 0x0047d6c0-0x0047d737: facing_target always commits; the soldier nudge is gated on cfg
    // soldier_count>0; facing_current only commits if llm_strat_unit_walk_step_allowed() (a BYTE-only
    // test in the asm -- TEST AL,AL -- reproduced as a uint8_t truncation, not a full-int32 compare).
    u.facing_target = static_cast<uint8_t>(new_facing_target);
    if (v.cfg_units[u.unit_proto_id].soldier_count > 0) {
        c.unit_soldiers_set_heading(player, unit_index, static_cast<uint8_t>(new_facing_target));
    }
    if (static_cast<uint8_t>(c.unit_walk_step_allowed(player, unit_index)) != 0) {
        u.facing_current = static_cast<uint8_t>(new_facing_current);
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_state_move_walker() {
    sim_state st = state();
    detail::unit_state_move_walker(st.read, st.own, live_unit_state_move_walker_calls());
}


} // namespace mh::sim
