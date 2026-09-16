//
// ai/ai_group_task_attack.cpp -- see ai_group_task_attack.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_group_task_{attack_nearest_defended_004ea0d2,engage_target_004ea072,
// drain_reserve_attack_004e9c98,attack_random_target_004e9a73}.asm), not from Ghidra's .c: the
// draft renders the one-variable victim latch as two reconciled SSA locals and anchors
// drain_reserve_attack's home-tile reads off ai_tile_flags_grid instead of ai_home_tile_x/y -- both
// are decompiler noise, not real differences. See the header for the full derivation of both.
//
#include "ai/ai_group_task_attack.h"


namespace mh::ai {
namespace detail {

namespace {

// The 21x21-tile (radius <=10 by Chebyshev box, filtered to toroidal dist_sq < 0x65) turret-coverage
// sweep shared verbatim by attack_nearest_defended and drain_reserve_attack, around a building at
// (bx, by) belonging to `victim`. `player_id` is the TICKING player (the relation test is always
// against player_id's own ai_player_relation row, never victim's) and `group_index` is the task's
// own group, which every enqueued engage task is queued onto. Returns the number of engage tasks
// enqueued. NOT a new named helper being introduced for reuse across unrelated functions -- it is
// the one loop body that both functions' assembly independently contains, factored out ONLY so the
// two call sites cannot drift; every field/constant in it is read off both .asm listings identically
// (0x004ea2b7-0x004ea3ff and 0x004e9eab-0x004ea005).
int32_t scan_turret_coverage_and_enqueue_engage(const ai_view &v, const ai_calls &gc, int32_t player_id,
                                                int32_t group_index, int32_t bx, int32_t by) {
    int32_t hits = 0;
    // Outer loop offsets X, inner offsets Y -- read off the register roles at 0x004ea2b7-0x004ea30e
    // (attack_nearest_defended) / 0x004e9eab-0x004e9f09 (drain_reserve_attack): the OUTER counter is
    // added to the building's x and masked by width_m, the INNER counter is added to y and masked by
    // height_m.
    for (int32_t ox = -10; ox < 11; ++ox) {
        for (int32_t oy = -10; oy < 11; ++oy) {
            const uint32_t sx   = *v.map_width_mask & (uint32_t)(bx + ox);
            const uint32_t sy   = *v.map_height_mask & (uint32_t)(by + oy);
            const uint32_t dist = gc.toroidal_dist_sq((int32_t)sx, (int32_t)sy, bx, by);
            if (dist >= 0x65u) continue; // CMP EAX,0x64 / JA -- unsigned, dist > 100 skips
            const tile_object &t = tile_at(v, (int32_t)sx, (int32_t)sy);
            if ((t.class_owner & REF_BLDG_BIT) == 0) continue; // TEST ..0x40 / JZ
            const uint8_t owner = (uint8_t)(t.class_owner & REF_OWNER_MASK);
            // Hostility is always tested against player_id's (the TICKING player's) own relation
            // row, never victim's -- confirmed off the row-offset computation feeding this CMP in
            // both listings.
            if (v.players[player_id].ai_player_relation[owner] > -1) continue; // JG -- not hostile
            const uint16_t bldg_id = building_of(v, owner, t.building).building_id;
            const uint8_t  btype   = v.cfg_buildings[bldg_id].type;
            if (btype != BLDG_TYPE_A_TURRET && btype != BLDG_TYPE_H_TURRET) continue;
            if (gc.bldg_has_aa_weapon(owner, t.building) == 0) continue;
            gc.group_task_enqueue(player_id, group_index, 0x16, 0x183, (uint32_t)(owner | 0x40u),
                                  (uint32_t)t.building, 0, 0, 0);
            ++hits;
        }
    }
    return hits;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// llm_strat_ai_group_task_attack_nearest_defended @0x004ea0d2. Task 0x15.
// ---------------------------------------------------------------------------------------------
group_task_attack_report group_task_attack_nearest_defended(const ai_view &v, const ai_store &own,
                                                            const ai_calls &gc, int32_t player_id,
                                                            int32_t group_index) {
    (void)own;
    group_task_attack_report rep{};
    const uint32_t           active = (uint32_t)*v.active_player_count;

    // Hostile-victim scan: ONE variable throughout (see the header on why the .c's two-local
    // rendering is decompiler noise), latched to the first player that is alive, hostile to
    // player_id, and still has a standing building at buildings[p][0]; kept scanning past that
    // point only while the LATCHED player's four ai_score_* fields are all zero (0x004ea0f6-
    // 0x004ea1ab).
    uint32_t victim = 0xffffffffu;
    for (uint32_t p = 0; p < active; ++p) {
        if ((v.strat_players[p].status_flags & PLAYER_STATUS_ALIVE) == 0) continue;
        if (v.players[player_id].ai_player_relation[p] > -1) continue; // not hostile
        if (building_of(v, p, 0).index == 0) continue;
        if (victim == 0xffffffffu) victim = p;
        if (v.players[victim].ai_score_bldg_type_b != 0 || v.players[victim].ai_score_cat_0x21 != 0 ||
            v.players[victim].ai_score_cat_0x23 != 0 || v.players[victim].ai_score_cat_0x22 != 0) {
            victim = p;
            break;
        }
    }

    int32_t target_building = 0;
    if (victim != active) {
        rep.victim_found     = true;
        rep.victim_player    = (int32_t)victim;
        const int32_t home_x = v.players[player_id].ai_home_tile_x;
        const int32_t home_y = v.players[player_id].ai_home_tile_y;

        // Four fallback type-groups, first non-zero wins (0x004ea205-0x004ea299). The 2nd reads its
        // candidate types from player_data[victim + 1] -- see the header on the deliberate aliasing.
        target_building = gc.group_find_nearest_building_of_types(
            (int32_t)victim, home_x, home_y, (uint32_t)v.players[victim].ai_mine_candidate_tier1,
            (uint32_t)v.players[victim].ai_mine_candidate_tier2, 0xffffffffu, 0xffffffffu);
        if (target_building == 0) {
            target_building = gc.group_find_nearest_building_of_types(
                (int32_t)victim, home_x, home_y,
                (uint32_t)v.players[victim + 1].ai_housing_candidate_soldier,
                (uint32_t)v.players[victim + 1].ai_housing_candidate_vehicle,
                (uint32_t)v.players[victim + 1].ai_housing_candidate_heli,
                (uint32_t)v.players[victim + 1].ai_housing_candidate_plane);
        }
        if (target_building == 0) {
            target_building = gc.group_find_nearest_building_of_types(
                (int32_t)victim, home_x, home_y,
                (uint32_t)v.players[victim].ai_build_candidate_primary,
                (uint32_t)v.players[victim].ai_build_candidate_secondary, 0xffffffffu, 0xffffffffu);
        }
        if (target_building == 0) {
            target_building = gc.group_find_nearest_building_of_types(
                (int32_t)victim, home_x, home_y, v.players[victim].ai_mother_building_type,
                0xffffffffu, 0xffffffffu, 0xffffffffu);
        }
    }

    if (victim != active && target_building != 0) {
        rep.target_building = target_building;
        const building &tb  = building_of(v, victim, target_building);
        rep.turret_hits     = scan_turret_coverage_and_enqueue_engage(v, gc, player_id, group_index,
                                                                      (int32_t)tb.x, (int32_t)tb.y);
        gc.group_task_preempt(player_id, group_index, 0x16, 0x183, (int32_t)(victim | 0x40u),
                              target_building, 0, 0, 0);
        rep.preempted = true;
        return rep;
    }

    gc.group_task_enqueue(player_id, group_index, 8, 0x183, 0, 0, 0, 0, 0);
    gc.group_task_dequeue(player_id, group_index);
    rep.recalled = true;
    return rep;
}

// ---------------------------------------------------------------------------------------------
// llm_strat_ai_group_task_engage_target @0x004ea072. Task 0x16. TWO PARAMETERS ONLY -- see the
// header on the phantom third parameter Ghidra's own committed prototype carried until 2026-08-05.
// ---------------------------------------------------------------------------------------------
void group_task_engage_target(const ai_view &v, const ai_store &own, const ai_calls &gc,
                              int32_t player_id, int32_t group_index) {
    (void)v;
    unit_group &grp = own.players[player_id].ai_groups[group_index];

    // Re-validates the group's already-resolved target (active_param_a/_b) and either confirms it
    // into resolved_target_ref/_index, or clears both to 0 (0x004ea072-0x004ea0d1).
    const int32_t alive =
        gc.target_ref_is_alive((uint32_t)grp.active_param_a, grp.active_param_b);
    if (alive == 0) {
        grp.resolved_target_ref   = 0;
        grp.resolved_target_index = 0;
    } else {
        grp.resolved_target_ref   = grp.active_param_a;
        grp.resolved_target_index = grp.active_param_b;
    }
}

// ---------------------------------------------------------------------------------------------
// llm_strat_ai_group_task_drain_reserve_attack @0x004e9c98. Task 0x17.
// ---------------------------------------------------------------------------------------------
group_task_attack_report group_task_drain_reserve_attack(const ai_view &v, const ai_store &own,
                                                         const ai_calls &gc, uint32_t player_id,
                                                         int32_t group_index) {
    (void)own;
    group_task_attack_report rep{};
    const int32_t            pid = (int32_t)player_id;

    uint32_t centroid_x = 0, centroid_y = 0; // committed llm_strat_ai_group_compute_centroid out-params
    gc.group_compute_centroid(pid, group_index, &centroid_x, &centroid_y);

    // Drain the reserve group (slot 3) into this task's group, launching each drained unit from
    // storage toward the centroid (0x004e9cbc-0x004e9d03). member_count/head_unit are re-read every
    // iteration through the const view rather than cached: group_member_move (the original, called
    // below) is what shrinks member_count and unlinks head_unit, and nothing here may assume it is
    // stable across that call.
    while (v.players[pid].ai_groups[3].member_count != 0) {
        const uint32_t unit_id = v.players[pid].ai_groups[3].head_unit;
        gc.unit_launch_from_storage_enqueue((uint8_t)pid, (int32_t)unit_id, centroid_x,
                                            centroid_y);
        gc.group_member_move(player_id, 3, group_index, (int32_t)unit_id);
        ++rep.drained_units;
    }

    // Hostile-victim scan: NO buildings[p][0] check and NO latch, unlike the two siblings above --
    // a plain single-index scan that keeps advancing while NOT(alive && hostile) OR the four
    // ai_score_* fields are all zero (0x004e9d11-0x004e9d8d). See the header on why this asymmetry
    // is the original's and must not be "fixed" to match its siblings.
    const uint32_t active = (uint32_t)*v.active_player_count;
    uint32_t       victim = active;
    for (uint32_t idx = 0; idx < active; ++idx) {
        if ((v.strat_players[idx].status_flags & PLAYER_STATUS_ALIVE) == 0) continue;
        if (v.players[pid].ai_player_relation[idx] > -1) continue; // not hostile
        if (v.players[idx].ai_score_bldg_type_b == 0 && v.players[idx].ai_score_cat_0x21 == 0 &&
            v.players[idx].ai_score_cat_0x23 == 0 && v.players[idx].ai_score_cat_0x22 == 0) {
            continue;
        }
        victim = idx;
        break;
    }

    int32_t target_building = 0;
    if (victim != active) {
        rep.victim_found  = true;
        rep.victim_player = (int32_t)victim;
        // player_id's (the ticking player's) own home tile, in BOTH cases -- see the header on why
        // the .c's ai_tile_flags_grid-anchored pointer arithmetic for this read is decompiler noise
        // for the same plain field the sibling functions read directly.
        const int32_t home_x = v.players[pid].ai_home_tile_x;
        const int32_t home_y = v.players[pid].ai_home_tile_y;

        target_building = gc.group_find_nearest_building_of_types(
            (int32_t)victim, home_x, home_y, (uint32_t)v.players[victim].ai_mine_candidate_tier1,
            (uint32_t)v.players[victim].ai_mine_candidate_tier2, 0xffffffffu, 0xffffffffu);
        if (target_building == 0) {
            target_building = gc.group_find_nearest_building_of_types(
                (int32_t)victim, home_x, home_y,
                (uint32_t)v.players[victim + 1].ai_housing_candidate_soldier,
                (uint32_t)v.players[victim + 1].ai_housing_candidate_vehicle,
                (uint32_t)v.players[victim + 1].ai_housing_candidate_heli,
                (uint32_t)v.players[victim + 1].ai_housing_candidate_plane);
        }
        if (target_building == 0) {
            target_building = gc.group_find_nearest_building_of_types(
                (int32_t)victim, home_x, home_y,
                (uint32_t)v.players[victim].ai_build_candidate_primary,
                (uint32_t)v.players[victim].ai_build_candidate_secondary, 0xffffffffu, 0xffffffffu);
        }
        if (target_building == 0) {
            target_building = gc.group_find_nearest_building_of_types(
                (int32_t)victim, home_x, home_y, v.players[victim].ai_mother_building_type,
                0xffffffffu, 0xffffffffu, 0xffffffffu);
        }
    }

    if (victim != active && target_building != 0) {
        rep.target_building = target_building;
        const building &tb  = building_of(v, victim, target_building);
        rep.turret_hits     = scan_turret_coverage_and_enqueue_engage(v, gc, pid, group_index, (int32_t)tb.x,
                                                                      (int32_t)tb.y);
        gc.group_task_preempt(pid, group_index, 0x16, 0x183, (int32_t)(victim | 0x40u), target_building,
                              0, 0, 0);
        rep.preempted = true;
        return rep;
    }

    gc.group_task_enqueue(pid, group_index, 8, 0x183, 0, 0, 0, 0, 0);
    gc.group_task_dequeue(pid, group_index);
    rep.recalled = true;
    return rep;
}

// ---------------------------------------------------------------------------------------------
// llm_strat_ai_group_task_attack_random_target @0x004e9a73. Task 0x18.
// ---------------------------------------------------------------------------------------------
group_task_attack_report group_task_attack_random_target(const ai_view &v, const ai_store &own,
                                                         const ai_calls &gc, int32_t player_id,
                                                         int32_t group_index) {
    group_task_attack_report rep{};
    const uint32_t           active = (uint32_t)*v.active_player_count;

    // Same one-variable hostile-victim latch as attack_nearest_defended -- see the header.
    uint32_t victim = 0xffffffffu;
    for (uint32_t p = 0; p < active; ++p) {
        if ((v.strat_players[p].status_flags & PLAYER_STATUS_ALIVE) == 0) continue;
        if (v.players[player_id].ai_player_relation[p] > -1) continue; // not hostile
        if (building_of(v, p, 0).index == 0) continue;
        if (victim == 0xffffffffu) victim = p;
        if (v.players[victim].ai_score_bldg_type_b != 0 || v.players[victim].ai_score_cat_0x21 != 0 ||
            v.players[victim].ai_score_cat_0x23 != 0 || v.players[victim].ai_score_cat_0x22 != 0) {
            victim = p;
            break;
        }
    }

    if (victim != active) {
        rep.victim_found      = true;
        rep.victim_player     = (int32_t)victim;
        const uint32_t home_x = (uint32_t)v.players[player_id].ai_home_tile_x;
        const uint32_t home_y = (uint32_t)v.players[player_id].ai_home_tile_y;

        // Reset-then-fill: this function owns the reset (0x004e9b50), unlike the shared engage/
        // attack/scan-target scratches which several functions share the reset duty over.
        *own.building_candidate_scratch_count = 0;
        gc.group_collect_buildings_of_types((int32_t)victim, home_x, home_y,
                                            (uint32_t)v.players[victim].ai_mine_candidate_tier1,
                                            (uint32_t)v.players[victim].ai_mine_candidate_tier2,
                                            0xffffffffu, 0xffffffffu);
        gc.group_collect_buildings_of_types(
            (int32_t)victim, home_x, home_y,
            (uint32_t)v.players[victim + 1].ai_housing_candidate_soldier,
            (uint32_t)v.players[victim + 1].ai_housing_candidate_vehicle,
            (uint32_t)v.players[victim + 1].ai_housing_candidate_heli,
            (uint32_t)v.players[victim + 1].ai_housing_candidate_plane);
        gc.group_collect_buildings_of_types((int32_t)victim, home_x, home_y,
                                            (uint32_t)v.players[victim].ai_build_candidate_primary,
                                            (uint32_t)v.players[victim].ai_build_candidate_secondary,
                                            0xffffffffu, 0xffffffffu);
        gc.group_collect_buildings_of_types((int32_t)victim, home_x, home_y,
                                            v.players[victim].ai_mother_building_type, 0xffffffffu,
                                            0xffffffffu, 0xffffffffu);

        rep.candidate_count = *v.building_candidate_scratch_count;
        if (rep.candidate_count != 0) {
            const int32_t pick = gc.rand_below_ai((uint32_t)rep.candidate_count);
            gc.group_task_preempt(player_id, group_index, 0x16, 899, (int32_t)(victim | 0x40u),
                                  v.building_candidate_scratch_list[pick], 0, 0, 0);
            rep.preempted = true;
            return rep;
        }
    }

    gc.group_task_enqueue(player_id, group_index, 8, 899, 0, 0, 0, 0, 0);
    gc.group_task_dequeue(player_id, group_index);
    rep.recalled = true;
    return rep;
}

} // namespace detail

void group_task_attack_nearest_defended(int32_t player_id, int32_t group_index) {
    const ai_state st = state();
    (void)detail::group_task_attack_nearest_defended(st.read, st.own, live_calls(), player_id,
                                                     group_index);
}

void group_task_engage_target(int32_t player_id, int32_t group_index) {
    const ai_state st = state();
    detail::group_task_engage_target(st.read, st.own, live_calls(), player_id, group_index);
}

void group_task_drain_reserve_attack(uint32_t player_id, int32_t group_index) {
    const ai_state st = state();
    (void)detail::group_task_drain_reserve_attack(st.read, st.own, live_calls(), player_id,
                                                  group_index);
}

void group_task_attack_random_target(int32_t player_id, int32_t group_index) {
    const ai_state st = state();
    (void)detail::group_task_attack_random_target(st.read, st.own, live_calls(), player_id,
                                                  group_index);
}

// ---- the differential-oracle arms -----------------------------------------------------------

} // namespace mh::ai
