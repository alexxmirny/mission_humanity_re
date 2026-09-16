//
// ai/ai_group_task_predicates.cpp -- see ai_group_task_predicates.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_group_{check_arrival_status_004e943d,no_member_near_centroid_004d66df,
// area_scan_hostile_004d69ce,all_units_settled_004d6b84}.asm), not from Ghidra's .c -- the first three
// decompiles are faithful enough to cross-check against, but the address arithmetic below was derived
// independently from addr/mh_structs.gen.h's offsetof asserts (unit_group::head_unit @0xa,
// ::active_param_a/b/c/d @0x2f/0x33/0x37/0x3b, ::member_count @0x4, ::active_sub_code @0x2d; unit::x/y
// @0x84/0x85, ::ai_group_next @0xd4, ::order_notify_status @0xe8) rather than trusted from the .c's raw
// offsets, and every one of them checks out exactly against the .asm's literal displacements.
//
#include "ai/ai_group_task_predicates.h"


namespace mh::ai {
namespace detail {

int32_t group_check_arrival_status(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                   uint32_t player, int32_t group_index) {
    (void)own;
    const unit_group &grp = v.players[player].ai_groups[group_index];

    int32_t near_hits = 0; // iVar4 -- count of members classed "arrived"
    int32_t readiness = 1; // iVar5 -- default answer for an empty group

    uint32_t unit_id = grp.head_unit; // 0x004e9458/0x004e9478
    while (unit_id != 0) {
        const unit &u = unit_of(v, player, (int32_t)unit_id);

        // "AI plane whose order state is already settled" skips classification entirely and goes
        // straight to the next-unit fetch (0x004e948e-0x004e949d): the settled check only runs when
        // the unit IS a plane, and only a settled plane is skipped.
        const bool skip = gc.unit_is_ai_plane((int32_t)player, (int32_t)unit_id) != 0 &&
                          gc.unit_order_state_is_settled((int32_t)player, (int32_t)unit_id) != 0;

        if (!skip) {
            const uint8_t status = u.order_notify_status; // 0x004e94b1
            if (status <= 6) {                            // CMP AL,0x6 / JA @0x004e94b9 -- else no-op
                switch (status) {
                    case 1:
                    case 2:
                    case 3: {
                        const uint32_t dist =
                            gc.toroidal_dist_sq((int32_t)u.x, (int32_t)u.y, grp.active_param_a,
                                                grp.active_param_b);
                        if (dist < 100) { // JNC @0x004e952c -- UNSIGNED
                            ++near_hits;
                        } else {
                            readiness = 0;
                        }
                        break;
                    }
                    case 4:
                        if (readiness != 0) readiness = 3; // no near-hit tally on this arm
                        break;
                    case 5:
                        if (readiness != 0) readiness = 2;
                        ++near_hits; // FALLS THROUGH into case 6's tally in the original
                        break;
                    case 6:
                        ++near_hits;
                        break;
                    default: // case 0: no-op, matches the table's caseD_0-only entry
                        break;
                }
            }
        }

        unit_id = u.ai_group_next; // caseD_0 tail, 0x004e9549-0x004e955f
    }

    if (readiness == 0) {                                                   // TEST EDI,EDI / JNZ @0x004e9567 -- skip the re-admit check if already !=0
        const uint32_t member_count = (uint32_t)(uint16_t)grp.member_count; // MOVZX, unsigned
        if (member_count * 0x46u <= (uint32_t)(near_hits * 100)) {
            readiness = 1;
        }
    }
    return readiness;
}

int32_t group_no_member_near_centroid(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                      int32_t player, int32_t group_index) {
    (void)own;
    uint32_t centroid_x = 0, centroid_y = 0;                                  // committed llm_strat_ai_group_compute_centroid out-params
    gc.group_compute_centroid(player, group_index, &centroid_x, &centroid_y); // 0x004d66fd

    const unit_group &grp     = v.players[player].ai_groups[group_index];
    uint32_t          unit_id = grp.head_unit; // 0x004d6720
    while (unit_id != 0) {
        const unit    &u    = unit_of(v, (uint32_t)player, (int32_t)unit_id);
        const uint32_t dist = gc.toroidal_dist_sq(centroid_x, centroid_y, (int32_t)u.x, (int32_t)u.y);
        if (dist < 100) { // JNC @0x004d6759 -- UNSIGNED; below 100 -> found a near member, return 0
            return 0;
        }
        unit_id = u.ai_group_next; // 0x004d6762
    }
    return 1; // chain exhausted (or empty group) -- the shared-epilogue exit at 0x004d66d1
}

int32_t group_area_scan_hostile(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                uint32_t player, int32_t group_index, uint8_t owner_mask) {
    (void)own;
    (void)gc; // pure over the view -- no outward calls in the original body
    const unit_group &grp = v.players[player].ai_groups[group_index];

    const uint32_t width_mask  = *v.map_width_mask;
    const uint32_t height_mask = *v.map_height_mask;

    // Shared per-cell test: a BUILDING present, matching owner_mask against the high nibble, whose
    // owning player (low nibble) is hostile to `player`. Returns true on a hit ("stop, return 0").
    auto is_hostile_building_at = [&](uint32_t x, uint32_t y) -> bool {
        const tile_object &t = tile_at(v, (int32_t)x, (int32_t)y);
        if (t.building == 0) return false; // 0x004d6a82/0x004d6ae9
        const uint8_t class_owner = t.class_owner;
        if ((owner_mask & (uint32_t)(class_owner & 0xf0)) == 0) return false; // 0x004d6a92-0x004d6a98
        const int32_t owner = class_owner & 0xf;                              // 0x004d6aa4
        return v.players[player].ai_player_relation[owner] < 0;               // signed; see header on the polarity
    };

    if (grp.active_sub_code == 0) { // CMP word,0 / JZ @0x004d6a0b -- rectangular sweep
        // `!=` LOOPS with wrap-on-increment, NOT `<` loops -- see the header. Types are uint32_t
        // throughout to match the original's plain 32-bit register arithmetic (the AND-with-mask wrap
        // is correct on the bit pattern regardless of the signedness of the coordinate it came from).
        uint32_t       y     = (uint32_t)grp.active_param_b;
        const uint32_t y_end = (uint32_t)grp.active_param_d;
        while (y != y_end) {
            uint32_t       x     = (uint32_t)grp.active_param_a;
            const uint32_t x_end = (uint32_t)grp.active_param_c;
            while (x != x_end) {
                if (is_hostile_building_at(x, y)) return 0;
                x = (x + 1) & width_mask;
            }
            y = (y + 1) & height_mask;
        }
    } else {                                                     // spiral disc scan
        const uint16_t sub_code = (uint16_t)grp.active_sub_code; // read UNSIGNED as the table index
        const uint32_t count    = v.spiral_ring_cell_counts[sub_code];
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t x =
                ((uint32_t)(grp.active_param_a + v.spiral_offsets[i].dx)) & width_mask;
            const uint32_t y =
                ((uint32_t)(grp.active_param_b + v.spiral_offsets[i].dy)) & height_mask;
            if (is_hostile_building_at(x, y)) return 0;
        }
    }
    return 1;
}

int32_t group_all_units_settled(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                int32_t player_id, int32_t group_index) {
    (void)own;
    const unit_group &grp     = v.players[player_id].ai_groups[group_index];
    uint32_t          unit_id = grp.head_unit; // 0x004d6bac
    while (unit_id != 0) {
        if (gc.unit_order_state_is_settled(player_id, (int32_t)unit_id) == 0) { // JZ @0x004d6bc1
            return 0;
        }
        unit_id = unit_of(v, (uint32_t)player_id, (int32_t)unit_id).ai_group_next; // 0x004d6bcf
    }
    return 1;
}

} // namespace detail

int32_t group_check_arrival_status(uint32_t player, int32_t group_index) {
    const ai_state st = state();
    return detail::group_check_arrival_status(st.read, st.own, live_calls(), player, group_index);
}

int32_t group_no_member_near_centroid(int32_t player, int32_t group_index) {
    const ai_state st = state();
    return detail::group_no_member_near_centroid(st.read, st.own, live_calls(), player, group_index);
}

int32_t group_area_scan_hostile(uint32_t player, int32_t group_index, uint8_t owner_mask) {
    const ai_state st = state();
    return detail::group_area_scan_hostile(st.read, st.own, live_calls(), player, group_index,
                                           owner_mask);
}

int32_t group_all_units_settled(int32_t player_id, int32_t group_index) {
    const ai_state st = state();
    return detail::group_all_units_settled(st.read, st.own, live_calls(), player_id, group_index);
}

// ---- the differential-oracle arms ---------------------------------------------------------------
//
// All four are pure predicates (own is unused in every detail:: body above), called from the hot
// per-tick group_task_step poll -- so, like ai_group_task_machine.cpp's arms, these report an
// AGGREGATE histogram on the shared cadence rather than a per-call trace. Each function gets its own
// counters and its own `next_report` threshold (ai_group_task_machine.cpp's header comment on why a
// shared threshold silently starves a low-volume sibling applies here identically -- four sibling arms
// in one TU, four independent thresholds).

} // namespace mh::ai
