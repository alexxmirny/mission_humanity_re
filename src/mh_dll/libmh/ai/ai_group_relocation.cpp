//
// ai/ai_group_relocation.cpp -- see ai_group_relocation.h for the full per-function narrative and the
// declared-needs banner (this TU does not compile until the conductor closes N1..N5 there).
//
#include "ai/ai_group_relocation.h"


namespace mh::ai {
namespace detail {

namespace {
// The four AI-role classifiers, checked in this exact order by BOTH route_unit_to_home_storage and
// group_reposition_members's phase-2/phase-3 walks -- first match wins, 0 = none of the four.
enum : int32_t { ROLE_NONE    = 0,
                 ROLE_SOLDIER = 1,
                 ROLE_GROUND  = 2,
                 ROLE_PLANE   = 3,
                 ROLE_HELI    = 4 };

int32_t classify_soldier_ground_plane_heli(const ai_calls &gc, uint32_t player, int32_t unit_id) {
    if (gc.unit_is_ai_soldier(player, unit_id)) return ROLE_SOLDIER;
    if (gc.unit_is_ai_ground(player, unit_id)) return ROLE_GROUND;
    if (gc.unit_is_ai_plane(player, unit_id)) return ROLE_PLANE;
    if (gc.unit_is_ai_heli(player, unit_id)) return ROLE_HELI;
    return ROLE_NONE;
}

// group_reposition_members's phase-2/3 walks only ever test soldier-or-ground (see the .h banner);
// kept as a separate tiny helper rather than reusing the four-way classifier so the two call counts
// visibly match the original (2 calls per member here, up to 4 there).
bool is_soldier_or_ground(const ai_calls &gc, uint32_t player, int32_t unit_id) {
    return gc.unit_is_ai_soldier(player, unit_id) != 0 || gc.unit_is_ai_ground(player, unit_id) != 0;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// llm_strat_ai_route_unit_to_home_storage @0x004d590e
// ---------------------------------------------------------------------------------------------
void route_unit_to_home_storage(const ai_view &v, const ai_store & /*own*/, const ai_calls &gc,
                                uint32_t player, int32_t unit_id) {
    // Defensive mask on entry, present in the original (AND EAX,0xf) and reproduced verbatim --
    // nothing here explains why 0xf rather than e.g. MAX_PLAYERS-1=7, so it is transcribed, not
    // "corrected" to the narrower mask.
    const uint32_t player_id = player & 0xfu;

    const int32_t role = classify_soldier_ground_plane_heli(gc, player_id, unit_id);

    // Scan unit_storage[player_id][1..24] (slot 0 never read) for the first occupied
    // (b_index != 0), not-overcrowded (docked_count <= 0x31) slot whose parked building's
    // building_id matches this role's designated home-storage type. See the .h banner's N1 for the
    // new view member this indexes.
    uint32_t           matched_slot = UINT32_MAX;
    const player_data &next_pd      = v.players[player_id + 1]; // deliberate [p+1] aliasing, see field comments
    for (uint32_t slot = 1; slot < 25; ++slot) {
        const auto &us = v.unit_storage[player_id * UNIT_STORAGE_SLOTS_PER_PLAYER + slot];
        if (us.b_index == 0) continue;
        // SIGNED compare in the original (JGE, not JNC/JAE) -- docked_count is presumed int32_t;
        // preserved as signed even though a realistic count is never negative (rule 7/8 of the brief:
        // don't silently widen a comparison's semantics).
        if (us.docked_count >= 0x32) continue;

        const uint32_t bldg_id = building_of(v, player_id, (int32_t)us.b_index).building_id;
        bool           matched = false;
        // Order preserved from the assembly (vehicle, heli, plane, soldier) though it is
        // mutually-exclusive-by-role and so has no behavioural effect on the order itself.
        if (bldg_id == (uint32_t)next_pd.ai_housing_candidate_vehicle && role == ROLE_GROUND)
            matched = true;
        else if (bldg_id == (uint32_t)next_pd.ai_housing_candidate_heli && role == ROLE_HELI)
            matched = true;
        else if (bldg_id == (uint32_t)next_pd.ai_housing_candidate_plane && role == ROLE_PLANE)
            matched = true;
        else if (bldg_id == (uint32_t)next_pd.ai_housing_candidate_soldier && role == ROLE_SOLDIER)
            matched = true;

        if (matched) {
            matched_slot = slot;
            break;
        }
    }

    // UNCONDITIONAL (runs whether or not a slot was found above): move the unit into its role's
    // fixed AI sub-group. See the .h banner for the role1==role2 destination collapse -- read
    // directly off two jump targets landing on the same shared tail, not a translation slip.
    if (role != ROLE_NONE) {
        const uint32_t src_group = unit_of(v, player_id, unit_id).ai_group_index;
        int32_t        dst_group;
        switch (role) {
            case ROLE_SOLDIER: dst_group = 2; break;
            case ROLE_GROUND: dst_group = 2; break;
            case ROLE_PLANE: dst_group = 4; break;
            case ROLE_HELI: dst_group = 3; break;
            default: dst_group = 0; break; // unreachable given the role != ROLE_NONE guard above
        }
        gc.group_member_move(player_id, (int32_t)src_group, dst_group, unit_id);
    }

    if (matched_slot != UINT32_MAX) {
        gc.unit_order_exit_storage_enqueue((uint16_t)player_id, (uint32_t)unit_id,
                                           (int32_t)matched_slot, 0, 0);
    }
}

// ---------------------------------------------------------------------------------------------
// llm_strat_ai_group_reposition_members @0x004d61d3
// ---------------------------------------------------------------------------------------------
void group_reposition_members(const ai_view &v, const ai_store &own, const ai_calls &gc,
                              uint32_t player, int32_t group_idx) {
    const player_data &pd_r = v.players[player];

    // Phase 1: bail-out scan over the WHOLE member list, no role filter.
    {
        uint16_t u = pd_r.ai_groups[group_idx].head_unit;
        while (u != 0) {
            const bool order_pending = gc.unit_is_order_pending(player, u) != 0;
            const bool has_target2   = unit_of(v, player, u).target2_ref != 0;
            if (order_pending || has_target2) {
                own.players[player].ai_reposition_cached_member_count = -1;
                return;
            }
            u = unit_of(v, player, u).ai_group_next;
        }
    }

    // Phase 2: eligibility count (soldier-or-ground only) + memo check.
    uint32_t eligible_count = 0;
    {
        uint16_t u = pd_r.ai_groups[group_idx].head_unit;
        while (u != 0) {
            if (is_soldier_or_ground(gc, player, u)) ++eligible_count;
            u = unit_of(v, player, u).ai_group_next;
        }
    }
    if (eligible_count == 0) return;
    if ((uint32_t)(uint16_t)pd_r.ai_groups[group_idx].member_count ==
        (uint32_t)pd_r.ai_reposition_cached_member_count)
        return;

    // Phase 3: branch on eligible_count vs the AI.SCR threshold (N1 in the .h banner).
    if (eligible_count < (uint32_t)*v.reposition_small_group_max) {
        // ---- SMALL branch ----
        uint32_t anchor_x = 0, anchor_y = 0; // committed llm_strat_ai_pick_owned_tile_or_home out-params
        gc.pick_owned_tile_or_home(player, &anchor_x, &anchor_y);

        *own.group_relocation_scratch_count = 0;
        uint16_t u                          = v.players[player].ai_groups[group_idx].head_unit; // freshly re-read
        while (u != 0) {
            own.group_relocation_scratch_list[*own.group_relocation_scratch_count] = u;
            ++*own.group_relocation_scratch_count;
            u = unit_of(v, player, u).ai_group_next;
        }
        // group_scatter_to_passable_tile is original/untranslated; it reads the scratch list this
        // loop just filled and does whatever it does with it, including (per the .h banner's N4)
        // very likely reaching the ENCLOSURE_SCRATCH grid through its own closure.
        gc.group_scatter_to_passable_tile(player, anchor_x, anchor_y);
    } else {
        // ---- LARGE branch ----
        const int32_t map_w = *v.map_width;
        const int32_t map_h = *v.map_height;

        // Pass 1: count PASSABLE cells over the player's OWN tile_flags_grid (raw byte == 1 -- a
        // third, currently-undocumented use of that grid; see the .h banner).
        int32_t passable_count = 0;
        for (int32_t x = 0; x < map_w; ++x)
            for (int32_t y = 0; y < map_h; ++y)
                if (v.players[player].ai_tile_flags_grid[(x << 8) | y] == 1) ++passable_count;

        const uint32_t step = ((uint32_t)passable_count << 8) / eligible_count; // unsigned divide

        // Pass 2: Bresenham-style even spread over the same grid, filling the new spread-tile
        // scratch table (N5). Accumulator arithmetic transcribed exactly -- see the .h banner.
        *own.spread_tile_count = 0;
        uint32_t acc           = 0;
        for (int32_t x = 0; x < map_w; ++x) {
            for (int32_t y = 0; y < map_h; ++y) {
                if (v.players[player].ai_tile_flags_grid[(x << 8) | y] != 1) continue;
                if (acc < 0x100u) {
                    acc += step;
                    auto &slot = own.spread_tiles[*own.spread_tile_count];
                    slot.x     = (int16_t)x;
                    slot.y     = (int16_t)y;
                    slot.used  = 0;
                    ++*own.spread_tile_count;
                }
                acc -= 0x100u; // unconditional on every passable cell, both branches
            }
        }

        // Pass 3: walk soldier-or-ground members, assign each the nearest unclaimed spread point.
        uint16_t u = v.players[player].ai_groups[group_idx].head_unit; // freshly re-read again
        while (u != 0) {
            if (is_soldier_or_ground(gc, player, u)) {
                int32_t  best_idx  = -1;
                uint32_t best_dist = 0x7fffffffu;
                for (int32_t i = 0; i < *v.spread_tile_count; ++i) {
                    if (v.spread_tiles[i].used != 0) continue;
                    const uint32_t d = gc.toroidal_dist_sq(
                        unit_of(v, player, u).x, unit_of(v, player, u).y,
                        (uint32_t)(uint16_t)v.spread_tiles[i].x, (uint32_t)(uint16_t)v.spread_tiles[i].y);
                    if (d < best_dist) {
                        best_dist = d;
                        best_idx  = i;
                    }
                }
                if (best_idx < 0) break; // breaks the WHOLE walk, not just this member -- see .h banner

                own.spread_tiles[best_idx].used = 1;
                const uint32_t target_x         = (uint16_t)v.spread_tiles[best_idx].x;
                const uint32_t target_y         = (uint16_t)v.spread_tiles[best_idx].y;

                if (!gc.unit_state_is_in_storage_transit(player, u)) {
                    const bool  in_transit = gc.unit_state_is_in_transit(player, u) != 0;
                    const unit &uu         = unit_of(v, player, u);
                    bool        do_move;
                    if (in_transit)
                        do_move = !(uu.goal_x == target_x || uu.goal_y == target_y);
                    else
                        do_move = !(uu.x == target_x || uu.y == target_y);
                    if (do_move) gc.unit_flag_and_move(player, u, target_x, target_y);
                }
            }
            u = unit_of(v, player, u).ai_group_next;
        }
    }

    // Phase 4: common tail, both branches.
    own.players[player].ai_reposition_cached_member_count =
        (int32_t)(uint16_t)v.players[player].ai_groups[group_idx].member_count;
}

// ---------------------------------------------------------------------------------------------
// llm_strat_ai_group_split_excess_members @0x004e69ed
// ---------------------------------------------------------------------------------------------
void group_split_excess_members(const ai_view &v, const ai_store & /*own*/, const ai_calls &gc,
                                uint32_t player, int32_t group_idx) {
    uint32_t centroid_x = 0, centroid_y = 0; // committed llm_strat_ai_group_compute_centroid out-params
    gc.group_compute_centroid(player, group_idx, &centroid_x, &centroid_y);

    const uint32_t excess_pool_count = (uint32_t)(uint16_t)v.players[player].ai_groups[3].member_count;
    gc.group_split_off_create(player, centroid_x, centroid_y, group_idx, excess_pool_count);

    const int16_t task_code = v.players[player].ai_groups[group_idx].task_code;
    if (task_code != 0xc && task_code != 8) {
        gc.group_task_preempt(player, group_idx, /*task_code*/ 8, /*param_4*/ 3, 0, 0, 0, 0, 0);
    }
}

} // namespace detail

void route_unit_to_home_storage(uint32_t player, int32_t unit_id) {
    const ai_state st = state();
    detail::route_unit_to_home_storage(st.read, st.own, live_calls(), player, unit_id);
}
void group_reposition_members(uint32_t player, int32_t group_idx) {
    const ai_state st = state();
    detail::group_reposition_members(st.read, st.own, live_calls(), player, group_idx);
}
void group_split_excess_members(uint32_t player, int32_t group_idx) {
    const ai_state st = state();
    detail::group_split_excess_members(st.read, st.own, live_calls(), player, group_idx);
}


} // namespace mh::ai
