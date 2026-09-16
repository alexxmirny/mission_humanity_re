#include "sim/sim_group_move_order_pathfind.h"
#include "sim/sim_stack_guard.h" // LIFT-TABLE S5: the stack probe is libmh-internal, not a host service

#include "addr/mh_calls.gen.h" // typed callables for the ORIGINAL functions this closure still calls out to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const group_move_order_pathfind_calls &live_group_move_order_pathfind_calls() {
    static const group_move_order_pathfind_calls c{
        MH_LIBMH_BIND(llm_strat_pathfind_find_closer_visible_tile),
        MH_LIBMH_BIND(llm_strat_path_free_slot),
        MH_LIBMH_BIND(llm_strat_path_alloc_slot),
        MH_LIBMH_BIND(llm_map_region_walk_to_valid_tile),
        MH_LIBMH_BIND(llm_strat_tile_dist_wrapped),
        MH_LIBMH_BIND(llm_strat_pathfind_build_steps),
        MH_LIBMH_BIND(llm_strat_group_plan_formation_positions),
        MH_LIBMH_BIND(llm_strat_pathfind_target_hook_stub),
        MH_LIBMH_BIND(llm_map_region_find_route),
        MH_LIBMH_BIND(llm_strat_pathfind_mark_group_member_regions),
        MH_LIBMH_BIND(llm_map_region_flood_reachable),
        MH_LIBMH_BIND(llm_map_region_route_search),
        mh::sim::stack_capacity_guard_noop,
        MH_LIBMH_BIND(llm_map_region_find_nearest_valid_tile),
        MH_LIBMH_BIND(llm_strat_pathfind_plan_group_route),
        MH_LIBMH_BIND(llm_strat_pathfind_trace_route),
        MH_LIBMH_BIND(llm_strat_group_path_step_record),
        MH_LIBMH_BIND(llm_strat_unit_path_queue_count),
    };
    return c;
}

namespace detail {

namespace {

// (tile_x<<8)|tile_y -- the codebase's standard flat passable/tile index (matches
// sim_store::passable_at()'s own indexing convention).
inline uint32_t flat_idx(uint32_t x, uint32_t y) { return (x << 8) | y; }

} // namespace

// The corner-cut "step one cell toward (tx,ty), preferring column then row" primitive (below, as the
// `step_toward` local lambda) is shared verbatim by BOTH corner-cut loops in the per-member walk
// (0x0041d735-0x0041d780 and 0x0041da3d-0x0041da88 -- same instruction shape at both addresses).
// Kept as a function-local lambda rather than a file-scope helper per house rule 4's "no NEW
// [cross-file] helpers": this one never leaves this translation unit, let alone this one function.

void group_move_order_pathfind(const sim_view &v, sim_store &own,
                               const group_move_order_pathfind_calls &c, int32_t mode,
                               uint8_t target_mask) {
    // 0x0041ca5a-0x0041ca67: owner_bit_mask = 1 << (owner_byte & 0x1f) -- SHL's hardware operand mask,
    // reproduced explicitly even though `owner` (a player index 0..7) can never reach 32.
    const int32_t  owner          = *v.group_order_owner;
    const uint32_t owner_bit_mask = 1u << (static_cast<uint8_t>(owner) & 0x1fu);
    const int32_t  member_count   = *v.group_member_count;

    const uint32_t wrap_mask32 = *v.path_wrap_mask;
    const uint8_t  wrap_mask8  = static_cast<uint8_t>(wrap_mask32);

    auto step_toward = [wrap_mask8](uint8_t &x, uint8_t &y, uint8_t tx, uint8_t ty) {
        if (x < tx)
            ++x;
        else if (tx < x)
            --x;
        else if (y < ty)
            ++y;
        else if (ty < y)
            --y;
        x &= wrap_mask8;
        y &= wrap_mask8;
    };

    // ---- step 1 (0x0041ca6a-0x0041cbe9): CENTROID -----------------------------------------------
    int32_t centroid_col;
    int32_t centroid_row;
    if (member_count < 2) {
        centroid_col = v.group_members[0].cur_col;
        centroid_row = v.group_members[0].cur_row;
    } else {
        int32_t       col_accum = 0;
        int32_t       row_accum = 0;
        const int32_t map_w     = *v.map_width;
        const int32_t map_h     = *v.map_height;
        for (int32_t i = 1; i < member_count; ++i) {
            // 0x0041caa4-0x0041cafd: wrapped delta on the column axis. THE HALVING IDIOM: bit-identical
            // to C's `/2` for every int32 input (see sim_pathfind_route_leg_group_and_sort.cpp's
            // identical note) -- written as plain `/2` rather than reproduced as shift/sub.
            int32_t delta_col = static_cast<int32_t>(v.group_members[i].cur_col) -
                                static_cast<int32_t>(v.group_members[0].cur_col);
            if (-map_w / 2 < delta_col) {
                if (map_w / 2 < delta_col) delta_col = 0x80 - delta_col;
            } else {
                delta_col += map_w;
            }
            col_accum += delta_col;

            // 0x0041cb03-0x0041cb5c: same wrapped-delta correction on the row axis against map_height.
            int32_t delta_row = static_cast<int32_t>(v.group_members[i].cur_row) -
                                static_cast<int32_t>(v.group_members[0].cur_row);
            if (-map_h / 2 < delta_row) {
                if (map_h / 2 < delta_row) delta_row = 0x80 - delta_row;
            } else {
                delta_row += map_h;
            }
            row_accum += delta_row;
        }
        // 0x0041cb67-0x0041cbaa: SIGNED divide (IDIV, truncating toward zero) by member_count, THEN
        // add member[0]'s tile, THEN mask -- `&` binds looser than `+`/`/` in C, and that is exactly
        // the asm's instruction order (IDIV, ADD, then AND last).
        centroid_col = static_cast<int32_t>(wrap_mask32) &
                       ((col_accum / member_count) + static_cast<int32_t>(v.group_members[0].cur_col));
        centroid_row = static_cast<int32_t>(wrap_mask32) &
                       ((row_accum / member_count) + static_cast<int32_t>(v.group_members[0].cur_row));
    }
    // 0x0041cbc3-0x0041cbe9: if the computed centroid tile is impassable, fall back to member[0]'s
    // raw tile instead.
    if (v.passable[flat_idx(static_cast<uint32_t>(centroid_col), static_cast<uint32_t>(centroid_row))] == 0) {
        centroid_col = v.group_members[0].cur_col;
        centroid_row = v.group_members[0].cur_row;
    }
    // group_centroid_x/y is READ OUTSIDE this closure by llm_map_minimap_render (the group's minimap
    // blip) -- see sim_state.h's own comment on this member.
    own.group_centroid_x_mut() = centroid_col;
    own.group_centroid_y_mut() = centroid_row;

    // Shared "reset every member's path slot" loop -- used by every early-return branch below.
    auto reset_all_path_slots = [&]() {
        for (int32_t i = 0; i < member_count; ++i) {
            c.path_free_slot(static_cast<uint16_t>(owner), static_cast<int32_t>(v.group_members[i].unit_handle));
            c.path_alloc_slot(owner, static_cast<int32_t>(v.group_members[i].unit_handle));
        }
    };

    // Shared "recompute MEMBER_TILE[*] from the current goal, member[0]-relative" loop -- the SAME
    // expression appears verbatim at 0x0041cf78 (step 5's seed), 0x0041d13e (step 6's
    // plan_group_route-success recompute), and 0x0041d2a8 (step 6's find_nearest_valid_tile-success
    // recompute). NOT used by step 7's mode==0 trim, which uses a DIFFERENT (accum-based) formula.
    auto seed_member_tile_from_goal = [&]() {
        for (int32_t i = 0; i < member_count; ++i) {
            const uint8_t dc = static_cast<uint8_t>(centroid_col) - v.group_members[i].cur_col;
            const uint8_t dr = static_cast<uint8_t>(centroid_row) - v.group_members[i].cur_row;
            own.group_member_tile_byte(i * 2 + 0) =
                wrap_mask8 & static_cast<uint8_t>(static_cast<uint8_t>(own.group_order_goal_x_mut()) - dc);
            own.group_member_tile_byte(i * 2 + 1) =
                wrap_mask8 & static_cast<uint8_t>(static_cast<uint8_t>(own.group_order_goal_y_mut()) - dr);
        }
    };

    // ---- step 2 (0x0041cbf9-0x0041cc72): GOAL VISIBILITY GATE -------------------------------------
    const uint32_t goal_x_wrapped =
        static_cast<uint32_t>(own.group_order_goal_x_mut()) & wrap_mask32;
    const uint32_t goal_y_wrapped =
        static_cast<uint32_t>(own.group_order_goal_y_mut()) & wrap_mask32;
    const bool goal_impassable = v.passable[flat_idx(goal_x_wrapped, goal_y_wrapped)] == 0;
    bool       goal_hidden     = false;
    if (!goal_impassable) {
        const uint8_t fog_byte = own.fog_discovered_at(static_cast<int32_t>(goal_x_wrapped),
                                                       static_cast<int32_t>(goal_y_wrapped));
        goal_hidden            = (owner_bit_mask & fog_byte) == 0;
    }
    if (goal_impassable || goal_hidden) {
        // group_order_goal_{x,y}_mut() is a plain int32_t field (sim_state.h) used as int32_t
        // everywhere else in this file; find_closer_visible_tile's committed out-params are
        // uint32_t* (TACT1-P C6, 2026-09-04) -- same 32-bit quantity, cast at this minority site.
        const int32_t result = c.find_closer_visible_tile(
            static_cast<uint8_t>(own.group_order_goal_x_mut()), static_cast<uint8_t>(own.group_order_goal_y_mut()),
            reinterpret_cast<uint32_t *>(&own.group_order_goal_x_mut()),
            reinterpret_cast<uint32_t *>(&own.group_order_goal_y_mut()), centroid_col, centroid_row);
        if (result != 0) {
            reset_all_path_slots();
            return;
        }
    }

    // ---- step 3 (0x0041ccbf-0x0041cd48): RAW GOAL PASSABILITY (no wrap mask this time) ------------
    if (v.passable[flat_idx(static_cast<uint32_t>(own.group_order_goal_x_mut()),
                            static_cast<uint32_t>(own.group_order_goal_y_mut()))] == 0) {
        // Same int32_t-field/uint32_t*-out-param cast as step 2 above -- committed pointee uint32_t*
        // (TACT1-P C6, 2026-09-04).
        const int32_t result = c.region_walk_to_valid_tile(
            static_cast<uint32_t>(own.group_order_goal_x_mut()), static_cast<uint32_t>(own.group_order_goal_y_mut()),
            static_cast<uint32_t>(centroid_col), static_cast<uint32_t>(centroid_row),
            reinterpret_cast<uint32_t *>(&own.group_order_goal_x_mut()),
            reinterpret_cast<uint32_t *>(&own.group_order_goal_y_mut()));
        if (result != 0) {
            reset_all_path_slots();
            return;
        }
    }

    // ---- step 4 (0x0041cd4d-0x0041cefc): mode==0 WEAPON-RANGE SCAN + no-move shortcut -------------
    // min_range/max_range are read again by step 7's route trim (also mode==0-gated), so they are
    // declared at this outer scope rather than inside the `if (mode == 0)` block.
    int32_t min_range = 0xff;
    int32_t max_range = 0;
    if (mode == 0) {
        const uint16_t cur_player = *v.cur_player;
        for (int32_t i = 0; i < member_count; ++i) {
            const int32_t unit_idx    = v.group_move_scratch[i].unit_idx;
            const unit   &member_unit = v.units[static_cast<int32_t>(cur_player) * v.caps.units + unit_idx];
            for (int32_t w = 0; w < UNIT_WEAPON_SLOTS; ++w) {
                const unit_weapon &wpn = member_unit.weapons[w];
                if (wpn.enabled_2 == 0) continue;
                const cfg_weapon &weapon_cfg = v.cfg_weapons[wpn.weapon_id];
                if ((target_mask & weapon_cfg.target) == 0) continue;
                if (max_range < weapon_cfg.range_max[cur_player]) max_range = weapon_cfg.range_max[cur_player];
                if (weapon_cfg.range_min[cur_player] < min_range) min_range = weapon_cfg.range_min[cur_player];
            }
        }
        const int32_t dist_to_anchor =
            c.tile_dist_wrapped(centroid_col, centroid_row, *v.group_anchor_x, *v.group_anchor_y);

        if (member_count == 1) {
            c.path_free_slot(static_cast<uint16_t>(owner), static_cast<int32_t>(v.group_members[0].unit_handle));
            const int32_t path_slot_id =
                c.path_alloc_slot(owner, static_cast<int32_t>(v.group_members[0].unit_handle));
            const int32_t build_result =
                c.pathfind_build_steps(v.group_move_scratch[0].tile_col, v.group_move_scratch[0].tile_row,
                                       max_range, min_range, path_slot_id);
            if (build_result != 0) return;
        }
        if (dist_to_anchor < max_range) {
            reset_all_path_slots();
            c.group_plan_formation_positions(target_mask);
            return;
        }
    }

    // ---- step 5 (0x0041cf5c-0x0041cfc9): seed MEMBER_TILE[*] from the current goal ----------------
    seed_member_tile_from_goal();

    // ---- step 6 (0x0041cfd2-0x0041d302): REGION ROUTE SEARCH --------------------------------------
    const uint32_t from_packed = flat_idx(static_cast<uint8_t>(centroid_col), static_cast<uint8_t>(centroid_row));
    uint32_t       to_packed   = flat_idx(static_cast<uint8_t>(own.group_order_goal_x_mut()),
                                          static_cast<uint8_t>(own.group_order_goal_y_mut()));

    const bool goal_has_region =
        own.region_cell_at(own.group_order_goal_x_mut(), own.group_order_goal_y_mut()).region != nullptr;
    bool found_route = false;
    if (!goal_has_region) {
        c.pathfind_target_hook_stub(own.group_order_goal_x_mut(), own.group_order_goal_y_mut());
    } else {
        found_route = c.region_find_route(from_packed, to_packed) != 0;
    }

    if (!found_route) {
        // Same int32_t-field/uint32_t*-out-param cast as steps 2/3 above -- committed pointee
        // uint32_t* (TACT1-P C6, 2026-09-04).
        const int32_t plan_result = c.pathfind_plan_group_route(
            static_cast<uint32_t>(own.group_order_goal_x_mut()), static_cast<uint32_t>(own.group_order_goal_y_mut()),
            static_cast<uint32_t>(centroid_col), static_cast<uint32_t>(centroid_row),
            reinterpret_cast<uint32_t *>(&own.group_order_goal_x_mut()),
            reinterpret_cast<uint32_t *>(&own.group_order_goal_y_mut()));
        if (plan_result != 0) {
            reset_all_path_slots();
            return;
        }
        seed_member_tile_from_goal();
    } else {
        // 0x0041d06c: opaque original -- marks the goal region + its neighbors' route_mark so the
        // flood below is scoped to that subgraph (same pre-pass sim_group_plan_formation_positions.cpp
        // performs INLINE for a different member/goal pair; here it is entirely inside the callee).
        c.pathfind_mark_group_member_regions();
        const int32_t flood_result =
            c.region_flood_reachable(static_cast<int32_t>(from_packed), static_cast<int32_t>(to_packed));
        if (flood_result == 0) {
            // 0x0041d09a-0x0041d0d7: two guard calls, a REVERSED-argument-order retry flood, one more
            // guard call, then find_nearest_valid_tile writing a new (col,row) into `to_packed` in
            // place via raw byte pointers (col at byte+1, row at byte+0 -- matches the packed
            // (col<<8)|row layout).
            c.stack_capacity_guard_0x20();
            c.stack_capacity_guard_0x20();
            c.region_flood_reachable(static_cast<int32_t>(to_packed), static_cast<int32_t>(from_packed)); // discarded
            c.stack_capacity_guard_0x20();
            auto         *to_bytes       = reinterpret_cast<uint8_t *>(&to_packed);
            const int32_t nearest_result = c.region_find_nearest_valid_tile(&to_bytes[1], &to_bytes[0]);
            if (nearest_result == 0) {
                // 0x0041d1a4-0x0041d20d: no valid nearby tile at all -- fall back to each member's own
                // raw scratch tile (NOT the goal) and reset every path slot.
                for (int32_t i = 0; i < member_count; ++i) {
                    own.group_member_tile_byte(i * 2 + 0) = static_cast<uint8_t>(v.group_move_scratch[i].tile_col);
                    own.group_member_tile_byte(i * 2 + 1) = static_cast<uint8_t>(v.group_move_scratch[i].tile_row);
                    c.path_free_slot(static_cast<uint16_t>(owner),
                                     static_cast<int32_t>(v.group_members[i].unit_handle));
                    c.path_alloc_slot(owner, static_cast<int32_t>(v.group_members[i].unit_handle));
                }
                return;
            }
            // 0x0041d0ec-0x0041d11d: found -- one more flood (NORMAL order this time, discarded),
            // route_search fills GROUP_ROUTE_STEPS, then goal_x/y are updated from the found tile's
            // packed bytes and MEMBER_TILE is recomputed against the NEW goal.
            c.region_flood_reachable(static_cast<int32_t>(from_packed), static_cast<int32_t>(to_packed)); // discarded
            c.region_route_search(static_cast<uint16_t>(from_packed), static_cast<int16_t>(to_packed),
                                  &own.group_route_step_at(0).dir_code); // discarded return; fills route steps
            own.group_order_goal_x_mut() = static_cast<int32_t>((to_packed >> 8) & 0xffu);
            own.group_order_goal_y_mut() = static_cast<int32_t>(to_packed & 0xffu);
            seed_member_tile_from_goal();
        } else {
            // 0x0041d08a-0x0041d090: flood succeeded on the first try -- route_search fills
            // GROUP_ROUTE_STEPS directly; MEMBER_TILE is left as step 5 (or step 6's earlier
            // seed_member_tile_from_goal()) set it.
            c.region_route_search(static_cast<uint16_t>(from_packed), static_cast<int16_t>(to_packed),
                                  &own.group_route_step_at(0).dir_code);
        }
    }

    // ---- step 7 (0x0041d302-0x0041d4c1): mode==0 ONLY -- trim the route to weapon range -----------
    if (mode == 0) {
        uint32_t accum_dx = 0;
        uint32_t accum_dy = 0;
        for (int32_t idx = 0; v.group_route_steps[idx].run_length != 0; ++idx) {
            const uint8_t  dir     = v.group_route_steps[idx].dir_code;
            const uint32_t run_len = v.group_route_steps[idx].run_length;
            // 0x0041d360/0x0041d38a then 0x0041d395/0x0041d3a0: the running sum is masked EACH
            // iteration (`&` binds looser than `+` in C -- matches the asm's post-add AND).
            accum_dx           = (accum_dx + run_len * static_cast<uint32_t>(v.map_dir_step_deltas[dir * 2 + 0])) & wrap_mask32;
            accum_dy           = (accum_dy + run_len * static_cast<uint32_t>(v.map_dir_step_deltas[dir * 2 + 1])) & wrap_mask32;
            const int32_t dist = c.tile_dist_wrapped(static_cast<int32_t>(accum_dx), static_cast<int32_t>(accum_dy),
                                                     own.group_order_goal_x_mut(), own.group_order_goal_y_mut());
            if (dist < min_range) {
                own.group_route_step_at(idx).run_length = 0;
                own.group_route_step_at(idx).dir_code   = 0;
                break;
            }
        }
        const uint8_t accum_dx_byte =
            wrap_mask8 & static_cast<uint8_t>(static_cast<uint8_t>(accum_dx) + static_cast<uint8_t>(centroid_col));
        const uint8_t accum_dy_byte =
            wrap_mask8 & static_cast<uint8_t>(static_cast<uint8_t>(accum_dy) + static_cast<uint8_t>(centroid_row));
        for (int32_t i = 0; i < member_count; ++i) {
            const uint8_t mc                      = wrap_mask8 & static_cast<uint8_t>(static_cast<uint8_t>(centroid_col) -
                                                                                      v.group_members[i].cur_col + accum_dx_byte);
            const uint8_t mr                      = wrap_mask8 & static_cast<uint8_t>(static_cast<uint8_t>(centroid_row) -
                                                                                      v.group_members[i].cur_row + accum_dy_byte);
            own.group_member_tile_byte(i * 2 + 0) = mc;
            own.group_member_tile_byte(i * 2 + 1) = mr;
            own.group_move_scratch_at(i).tile_col = mc;
            own.group_move_scratch_at(i).tile_row = mr;
        }
    }

    // ---- step 8 (0x0041d4c8-0x0041d4f3): free every member's path slot (alloc happens per-member
    // below, in step 9/10) ---------------------------------------------------------------------------
    for (int32_t i = 0; i < member_count; ++i) {
        c.path_free_slot(static_cast<uint16_t>(owner), static_cast<int32_t>(v.group_members[i].unit_handle));
    }

    // ---- step 9 (0x0041d4f5-0x0041d5eb): member_count==1 SHORTCUT -- verbatim ROUTE_STEPS copy ----
    if (member_count == 1) {
        const int32_t path_slot_id = c.path_alloc_slot(owner, static_cast<int32_t>(v.group_members[0].unit_handle));
        int32_t       j            = 0;
        for (; v.group_route_steps[j].run_length != 0; ++j) {
            path_waypoint &dst = own.path_buffer_at(static_cast<uint32_t>(owner), path_slot_id, j);
            dst.run_length     = v.group_route_steps[j].run_length;
            dst.heading        = v.group_route_steps[j].dir_code;
        }
        path_waypoint &term = own.path_buffer_at(static_cast<uint32_t>(owner), path_slot_id, j);
        term.run_length     = 0;
        term.heading        = 0;
        return; // 0x0041d5eb: JMP straight to the epilogue -- no formation-positions call, any mode.
    }

    // ---- step 10 (0x0041d5f0-0x0041dbf5): member_count>1 -- THE PER-MEMBER WALK ------------------
    //
    // Per-member state machine, transcribed via goto/labels matching the ORIGINAL's real control flow
    // (re-derived from raw instruction addresses -- see this file's top-of-file note on the draft's
    // mislabeled goto target). Every `L_finalize_member` jump below corresponds to a real
    // `JZ`/`JNZ 0x0041db26` in the disassembly.
    for (int32_t i = 0; i < member_count; ++i) {
        const int32_t path_slot_id = c.path_alloc_slot(owner, static_cast<int32_t>(v.group_members[i].unit_handle));
        // delta_col/delta_row: the member's FIXED offset from the centroid, used by every corner-cut
        // target computation in this member's walk (0x0041d631-0x0041d649).
        const int32_t delta_col            = centroid_col - static_cast<int32_t>(v.group_members[i].cur_col);
        const int32_t delta_row            = centroid_row - static_cast<int32_t>(v.group_members[i].cur_row);
        uint8_t       walking_col          = v.group_members[i].cur_col;
        uint8_t       walking_row          = v.group_members[i].cur_row;
        int32_t       remaining_run_length = v.group_route_steps[0].run_length; // 0x0041d666
        uint8_t       dir                  = v.group_route_steps[0].dir_code;   // 0x0041d670
        int32_t       route_idx            = 1;                                 // 0x0041d680
        own.group_path_build_idx_mut()     = 0;                                 // 0x0041d687

    L_walk_top: // 0x0041d691
        if (walking_col == v.group_member_tile[i * 2 + 0] && walking_row == v.group_member_tile[i * 2 + 1])
            goto L_finalize_member;
        if (remaining_run_length != 0) goto L_direct_step; // 0x0041d7d1

        // 0x0041d6bf-0x0041d6ed: reload the next route segment.
        remaining_run_length = v.group_route_steps[route_idx].run_length;
        dir                  = v.group_route_steps[route_idx].dir_code;
        ++route_idx;
        if (remaining_run_length == 0) goto L_finalize_member; // route exhausted

        {
            // 0x0041d6f3-0x0041d7cc: corner-cut check toward the FIXED (walking + delta) target.
            // Continues while BOTH the straight-ahead tile AND at least one orthogonal corner stay
            // passable (0x0041d7c5/0x0041d7c7). Reaching the target jumps into the direct-step logic
            // using the freshly-reloaded `dir`; failing falls through to the deviation search.
            const uint8_t target_col =
                wrap_mask8 & static_cast<uint8_t>(walking_col + static_cast<uint8_t>(delta_col));
            const uint8_t target_row =
                wrap_mask8 & static_cast<uint8_t>(walking_row + static_cast<uint8_t>(delta_row));
            uint8_t probe_x = walking_col;
            uint8_t probe_y = walking_row;
            for (;;) {
                if (probe_x == target_col && probe_y == target_row) goto L_direct_step;
                step_toward(probe_x, probe_y, target_col, target_row);
                const bool ahead_ok = v.passable[flat_idx(probe_x, probe_y)] != 0;
                const bool side_ok  = v.passable[flat_idx(target_col, probe_y)] != 0 ||
                                     v.passable[flat_idx(probe_x, target_row)] != 0;
                if (!(ahead_ok && side_ok)) break;
            }
        }
        goto L_deviation_search; // 0x0041d8bb

    L_direct_step: { // 0x0041d7d1
        const uint8_t  next_col            = wrap_mask8 & (walking_col + v.map_dir_step_deltas[dir * 2 + 0]);
        const uint8_t  next_row            = wrap_mask8 & (walking_row + v.map_dir_step_deltas[dir * 2 + 1]);
        const bool     ahead_passable      = v.passable[flat_idx(next_col, next_row)] != 0;
        const bool     corner_col_passable = v.passable[flat_idx(walking_col, next_row)] != 0;
        const bool     corner_row_passable = v.passable[flat_idx(next_col, walking_row)] != 0;
        const uint32_t terrain_ahead =
            own.region_cell_at(next_col, next_row).terrain_flags >> 8;
        if (!ahead_passable || (!corner_col_passable && !corner_row_passable) || terrain_ahead == 0xffffffu)
            goto L_deviation_search;
        // 0x0041dad2: accept the step -- record it (opaque original: appends to PATH_BUFFERS and
        // advances GROUP_PATH_BUILD_IDX), then advance and continue.
        c.group_path_step_record(path_slot_id, dir, walking_col, walking_row);
        walking_col = next_col;
        walking_row = next_row;
        --remaining_run_length;
        goto L_walk_top;
    }

    L_deviation_search: { // 0x0041d8bb
        const uint32_t best_terrain = own.region_cell_at(walking_col, walking_row).terrain_flags >> 8;
        uint8_t        cur_x        = walking_col;
        uint8_t        cur_y        = walking_row;
        for (;;) {
            cur_x = wrap_mask8 & (cur_x + v.map_dir_step_deltas[dir * 2 + 0]);
            cur_y = wrap_mask8 & (cur_y + v.map_dir_step_deltas[dir * 2 + 1]);
            // 0x0041d91d: SIGNED compare -- remaining_run_length may reach exactly 0 (never negative
            // in a normal run; this guards the same "no more route at all" exhaustion the reload path
            // below also detects) but the asm's own check is signed, reproduced as-is.
            if (remaining_run_length < 0) {
                own.group_member_tile_byte(i * 2 + 0) = cur_x;
                own.group_member_tile_byte(i * 2 + 1) = cur_y;
                goto L_finalize_member;
            }
            --remaining_run_length;
            if (remaining_run_length == 0) {
                remaining_run_length = v.group_route_steps[route_idx].run_length;
                dir                  = v.group_route_steps[route_idx].dir_code;
                ++route_idx;
                if (remaining_run_length == 0) {
                    own.group_member_tile_byte(i * 2 + 0) = walking_col;
                    own.group_member_tile_byte(i * 2 + 1) = walking_row;
                    goto L_finalize_member;
                }
            }
            const uint32_t terrain_here = own.region_cell_at(cur_x, cur_y).terrain_flags >> 8;
            if (v.passable[flat_idx(cur_x, cur_y)] != 0 && terrain_here < best_terrain && terrain_here != 0) {
                // 0x0041d9fb-0x0041da9f: corner-cut toward the ORIGINAL start tile's fixed target
                // (walking + delta) -- SAME target as L_walk_top's corner-cut, but the loop
                // continuation here checks ONLY the straight-ahead tile (no orthogonal-corner OR).
                const uint8_t target_col =
                    wrap_mask8 & static_cast<uint8_t>(cur_x + static_cast<uint8_t>(delta_col));
                const uint8_t target_row =
                    wrap_mask8 & static_cast<uint8_t>(cur_y + static_cast<uint8_t>(delta_row));
                uint8_t probe_x = cur_x;
                uint8_t probe_y = cur_y;
                for (;;) {
                    if (probe_x == target_col && probe_y == target_row) {
                        const int32_t trace_result =
                            c.pathfind_trace_route(walking_col, walking_row, cur_x, cur_y,
                                                   reinterpret_cast<void *>(
                                                       static_cast<uintptr_t>(static_cast<uint32_t>(path_slot_id))));
                        if (trace_result != 0) goto L_finalize_member;
                        walking_col = cur_x;
                        walking_row = cur_y;
                        goto L_walk_top;
                    }
                    step_toward(probe_x, probe_y, target_col, target_row);
                    if (v.passable[flat_idx(probe_x, probe_y)] == 0) break; // falls back into the outer search
                }
            }
            // else: keep searching further in the same compass direction (outer `for(;;)` continues).
        }
    }

    L_finalize_member: // 0x0041db26
    {
        const int32_t  build_idx = *v.group_path_build_idx;
        path_waypoint &term      = own.path_buffer_at(static_cast<uint32_t>(owner), path_slot_id, build_idx);
        term.run_length          = 0;
        term.heading             = 0;
        c.unit_path_queue_count(path_slot_id, build_idx);
    }
        // falls through to the for-loop's own `++i` (0x0041d607's real role: the outer member-loop
        // increment).
    }

    // ---- tail (0x0041db97-0x0041dbf5): mode==0 ONLY -------------------------------------------------
    if (mode == 0) {
        for (int32_t i = 0; i < member_count; ++i) {
            own.group_move_scratch_at(i).tile_col = v.group_member_tile[i * 2 + 0];
            own.group_move_scratch_at(i).tile_row = v.group_member_tile[i * 2 + 1];
        }
        c.group_plan_formation_positions(target_mask);
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void group_move_order_pathfind(int32_t mode, uint8_t target_mask) {
    sim_state st = state();
    detail::group_move_order_pathfind(st.read, st.own, live_group_move_order_pathfind_calls(), mode,
                                      target_mask);
}


} // namespace mh::sim
