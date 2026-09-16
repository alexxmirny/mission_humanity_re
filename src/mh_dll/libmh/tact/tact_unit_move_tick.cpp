//
// tact/tact_unit_move_tick.cpp -- see tact_unit_move_tick.h. Translated from the DISASSEMBLY, not
// from Ghidra's C.
//
#include "tact/tact_unit_move_tick.h"

#include "addr/mh_calls.gen.h"  // frontier callees (Law 4)
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_facing_to_delta.h"
#include "tact/tact_door.h"
#include "tact/tact_move_step_attempt.h"
#include "tact/tact_unit_cmd_advance.h"
#include "tact/tact_unit_move_advance.h"
#include "tact/tact_unit_rotate_step.h"
#include "tact/tact_unit_stand_tick.h"
#include "state/mode_planes.h"
#include "state/promoted_select.h" // LIB-REF-SPLIT: MH_PROMOTED

namespace mh::tact {
namespace detail {
namespace {

// The ORIGINAL runs this exact sequence at two call sites (0x0042fe45-0x0042fe89 and
// 0x0042ff72-0x0042ffe5): attempt a fresh path via llm_tact_move_step_attempt from the current
// MOVE_CUR_COL/ROW cursor to cmd_queue[cmd_slot]'s (arg0, arg1) destination, and park the unit if
// no real move was found. Factored here since the two sites are IDENTICAL up to one difference,
// carried explicitly as `also_arm_stuck_countdown`: the step-9 site (0x0042fe3c, reached while
// move_stuck_countdown is still counting down) parks with ONLY move_retry_wait=0x10/attempts=0xc
// on failure (0x0042fec1-0x0042fee1); the step-8 site (0x0042ff54, entered directly when
// move_stuck_countdown was already 0) ALSO sets move_stuck_countdown=3 on failure
// (0x0042ffee-0x0043001e). Conflating the two into one unconditional dedup was an earlier draft's
// bug, caught by a rig divergence on this exact field.
//
// Returns true iff a real path slot was assigned (caller falls into step 10's consume); false
// means the unit was parked and the caller should just return.
bool try_acquire_path(const tact_view &v, tact_store &own, int32_t unit_idx, int32_t cmd_slot,
                      bool also_arm_stuck_countdown) {
    tact_unit &u = own.unit_at(unit_idx);

    const int32_t dst_col = u.cmd_queue[cmd_slot].arg0;
    const int32_t dst_row = u.cmd_queue[cmd_slot].arg1;
    (void)MH_LIBMH_BIND(llm_tact_move_step_attempt)(*v.move_cur_col, *v.move_cur_row, dst_col, dst_row);
    own.move_path_cache_valid() = 1;

    int32_t &path_slot_id = own.move_path_slot_id();
    // @0x0042fe8e-0x0042feb8 / 0x0042ffbb-0x0042ffe5: a flood result equal to the current tile
    // means no real move.
    if (*v.move_flood_result_col == *v.move_cur_col && *v.move_flood_result_row == *v.move_cur_row) {
        path_slot_id = -1;
    }
    if (path_slot_id == -1) {
        u.move_retry_wait     = 0x10;
        u.move_retry_attempts = 0xc;
        if (also_arm_stuck_countdown) {
            u.move_stuck_countdown = 3; // @0x00430015, step-8 site only.
        }
        return false;
    }

    // @0x0042fee6-0x0042ff4d / 0x00430023-0x0043008a: assign the slot, mark it in use, and stamp
    // the scratch args.
    u.move_path_slot                                = (int16_t)path_slot_id;
    own.planes().path_slot_flag_at(0, path_slot_id) = 1;
    u.move_path_step                                = 0;
    u.cmd_queue[cmd_slot].arg2                      = *v.move_flood_result_col;
    u.cmd_queue[cmd_slot].arg3                      = *v.move_flood_result_row;
    return true;
}

// Step 10 @0x00430091-0x00430341: consume the current path_waypoint at (move_path_slot,
// move_path_step) against cmd_queue[cmd_slot]'s destination.
void consume_path_step(const tact_view &v, tact_store &own, int32_t unit_idx, int32_t cmd_slot,
                       double dt) {
    tact_unit &u = own.unit_at(unit_idx);

    const mh::state::path_waypoint &wp =
        own.planes().path_waypoint_at(0, u.move_path_slot, u.move_path_step);

    if (wp.run_length != 0) {
        // step 11 @0x00430163-0x004302e0.
        if (u.progress != 0) {
            // step 14 (progress!=0): advance unconditionally, no door/passable recheck.
            int32_t dx = 0, dy = 0;
            MH_LIBMH_BIND(llm_tact_facing_to_delta)(wp.heading, &dx, &dy);
            MH_LIBMH_BIND(llm_tact_unit_move_advance)(unit_idx, *v.move_cur_col, *v.move_cur_row, dx, dy);
            u.move_state_timer += dt;
            return;
        }

        int32_t dx = 0, dy = 0;
        MH_LIBMH_BIND(llm_tact_facing_to_delta)(wp.heading, &dx, &dy);
        const int32_t next_col = *v.move_cur_col + dx;
        const int32_t next_row = *v.move_cur_row + dy;

        if (mh::tact::tile_at(v, next_col, next_row).class_owner != 0) {
            // step 11's door arm @0x004301e1-0x00430219.
            MH_LIBMH_BIND(llm_tact_door_anim_start)(mh::tact::tile_at(v, next_col, next_row).class_owner);
            u.move_state_timer = MH_PROMOTED_ROW(time_GetCurrentTime)(); // OVERWRITE, not an add.
            u.move_state_timer += dt;                                    // the common trailer still runs on this path.
            return;
        }

        // step 12 @0x0043021e-0x00430293.
        if (mh::tact::passable_at(v, next_col, next_row) == 0 ||
            mh::tact::tile_at(v, next_col, next_row).building != 0) {
            u.move_retry_wait      = 0x10;
            u.move_retry_attempts  = 0xc;
            u.move_stuck_countdown = 3;
            u.move_state_timer += dt;
            return;
        }

        // step 13 @0x00430293-0x004302de.
        if (u.facing_dir == wp.heading) {
            MH_LIBMH_BIND(llm_tact_unit_move_advance)(unit_idx, *v.move_cur_col, *v.move_cur_row, dx, dy);
        } else if ((u.status & 8) == 0) {
            MH_LIBMH_BIND(llm_tact_unit_rotate_step)(unit_idx, wp.heading);
        }
        u.move_state_timer += dt;
        return;
    }

    // run_length == 0: this leg is exhausted -- either the queue entry has ARRIVED, or the cached
    // path is stale and must be released so it is re-acquired next call.
    const bool arrived = (u.cmd_queue[cmd_slot].arg0 == u.cmd_queue[cmd_slot].arg2) &&
                         (u.cmd_queue[cmd_slot].arg1 == u.cmd_queue[cmd_slot].arg3);
    if (arrived) {
        MH_LIBMH_BIND(llm_tact_unit_cmd_advance)(unit_idx, cmd_slot);
    } else {
        own.planes().path_slot_flag_at(0, u.move_path_slot) = 0;
        u.move_path_slot                                    = 0;
    }
    u.move_state_timer += dt;
}

} // namespace

void unit_move_tick(const tact_view &v, tact_store &own, int32_t unit_idx, int32_t cmd_slot,
                    double dt) {
    tact_unit &u = own.unit_at(unit_idx);

    // step 1 @0x0042fbc6-0x0042fc19: KNEEL early-return.
    if (u.anim_state == 2 || u.anim_state == 3) {
        MH_LIBMH_BIND(llm_tact_unit_stand_tick)(unit_idx);
        u.move_state_timer += v.character_types[u.type].kneel_time;
        return; // @0x0042fc14: skips the dt trailer entirely.
    }

    // step 2 @0x0042fc19-0x0042fc3b: has-a-live-retry gate.
    if (u.move_retry_wait > 0 || u.move_retry_attempts > 0) {
        // step 3 @0x0042fc3b-0x0042fc96: peek the NEXT queue slot (cmd_index+1, wrapped at 0x80).
        // op==0 skips straight to step 4 with NO progress check at all; only when op!=0 does
        // progress==0 dispatch the advance (@0x0042fc72/0x0042fc82 -- the two JZ's are NOT parallel
        // conditions, the second is nested inside the "op!=0" arm of the first).
        int32_t next_idx = u.cmd_index + 1;
        if (next_idx >= 0x80) next_idx = 0;
        const bool dispatch_now = (u.cmd_queue[next_idx].op != 0) && (u.progress == 0);
        if (dispatch_now) {
            MH_LIBMH_BIND(llm_tact_unit_cmd_advance)(unit_idx, cmd_slot); // uses cmd_slot, NOT next_idx.
            return;                                                       // @0x0042fc91: skips the dt trailer.
        }
        // else fall through to the SAME move_retry_wait check the direct-skip path also reaches.
    }

    // step 4 @0x0042fc96-0x0042fcd0.
    if (u.move_retry_wait > 0) {
        --u.move_retry_wait;
        // QUIRK (see header): this add is followed by the trailer's own add in every caller of this
        // function -- move_state_timer advances by 2*dt on this specific path, preserved literally.
        u.move_state_timer += dt;
        u.move_state_timer += dt;
        return;
    }

    // step 6 @0x0042fcd0-0x0042fdd8 (move_retry_wait == 0 here, unconditionally).
    if (u.move_retry_attempts > 0) {
        u.move_retry_wait = 0x10;
        --u.move_retry_attempts;

        if (u.move_path_slot > 0) {
            // step 6a @0x0042fd18-0x0042fdd3.
            const mh::state::path_waypoint &wp =
                own.planes().path_waypoint_at(0, u.move_path_slot, u.move_path_step);
            int32_t dx = 0, dy = 0;
            MH_LIBMH_BIND(llm_tact_facing_to_delta)(wp.heading, &dx, &dy);
            const int32_t next_col = *v.move_cur_col + dx;
            const int32_t next_row = *v.move_cur_row + dy;
            if (mh::tact::passable_at(v, next_col, next_row) == 0 && u.progress == 0) {
                u.move_retry_wait = 0x10; // @0x0042fd98
            } else {
                u.move_retry_wait      = 0; // @0x0042fdaa-0x0042fdca
                u.move_retry_attempts  = 0;
                u.move_stuck_countdown = 0;
            }
        }
        u.move_state_timer += dt;
        return;
    }

    // step 7 @0x0042fdd8-0x0042fe3c (move_retry_wait == 0 AND move_retry_attempts == 0 here).
    if (u.move_stuck_countdown > 0) {
        --u.move_stuck_countdown;
        if (u.move_stuck_countdown == 0) {
            // just reached 0: ABORT the queued command.
            u.move_aborted_op = (uint8_t)u.cmd_queue[u.cmd_index].op;     // @0x0042fe20-0x0042fe26
            MH_LIBMH_BIND(llm_tact_unit_cmd_advance)(unit_idx, cmd_slot); // @0x0042fe32
            u.move_state_timer += dt;
            return;
        }
        // step 9 @0x0042fe3c-0x0042fe89: still counting down -- check the cache FIRST.
        if (own.move_path_cache_valid() != 0) {
            u.move_state_timer += dt;
            return;
        }
        // step 8/9b: acquire (this site does NOT arm stuck_countdown on park failure -- see
        // try_acquire_path's derivation).
        if (!try_acquire_path(v, own, unit_idx, cmd_slot, /*also_arm_stuck_countdown=*/false)) {
            u.move_state_timer += dt;
            return;
        }
    } else {
        // step 8 entered directly (stuck_countdown was already 0): reuse an existing slot as-is.
        // If there is none, this site is ALSO gated by the cache (@0x0042ff69-0x0042ffb6): a
        // cache already valid this call means do nothing at all -- not even park -- straight to
        // the trailer. Only an invalid cache tries to acquire, and THIS site's failure arm DOES
        // set stuck_countdown=3 (unlike step 9's).
        if (u.move_path_slot == 0) {
            if (own.move_path_cache_valid() != 0) {
                u.move_state_timer += dt;
                return;
            }
            if (!try_acquire_path(v, own, unit_idx, cmd_slot, /*also_arm_stuck_countdown=*/true)) {
                u.move_state_timer += dt;
                return;
            }
        }
    }

    consume_path_step(v, own, unit_idx, cmd_slot, dt);
}

} // namespace detail

void unit_move_tick(int32_t unit_idx, int32_t cmd_slot, double dt) {
    tact_state st = state();
    detail::unit_move_tick(st.read, st.own, unit_idx, cmd_slot, dt);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
