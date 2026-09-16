#include "sim/sim_group_plan_formation_positions.h"
#include "sim/sim_stack_guard.h" // LIFT-TABLE S5: the stack probe is libmh-internal, not a host service

#include "addr/mh_calls.gen.h" // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const group_plan_formation_positions_calls &live_group_plan_formation_positions_calls() {
    static const group_plan_formation_positions_calls c = {
        MH_LIBMH_BIND(llm_strat_tile_dist_wrapped),
        MH_LIBMH_BIND(llm_strat_pathfind_build_steps),
        MH_LIBMH_BIND(llm_map_region_flood_reachable),
        MH_LIBMH_BIND(llm_map_region_route_search),
        mh::sim::stack_capacity_guard_noop,
    };
    return c;
}

namespace detail {

// "Not found yet" sentinel for the terrain-min scan -- the MAXIMUM value (terrain_flags>>8) can ever
// take (terrain_flags is a uint32_t; >>8 leaves a 24-bit result), not an arbitrary 0xffffffff. Read off
// the asm literally (`MOV dword ptr [EBP-0x24],0xffffff`), not simplified.
inline constexpr uint32_t FORMATION_TERRAIN_NOT_FOUND = 0xffffffu;

// CONCAT11(hi,lo) semantics from the Ghidra draft: hi occupies bits 8-15, lo occupies bits 0-7. Every
// packed-region-id argument in this function (llm_map_region_flood_reachable / _route_search both take
// a coordinate pair packed this way) uses it; kept file-local per house rule 4 (no new shared helpers).
inline uint16_t pack_xy(int32_t x, int32_t y) {
    return static_cast<uint16_t>((static_cast<uint16_t>(static_cast<uint8_t>(x)) << 8) |
                                 static_cast<uint8_t>(y));
}

void group_plan_formation_positions(const sim_view &v, sim_store &own,
                                    const group_plan_formation_positions_calls &c,
                                    uint8_t                                     target_type_mask) {
    // 0x0041dc20-0x0041e786: one pass per formation slot. The asm re-reads the GLOBAL member count on
    // every iteration (0x0041dc23); nothing in this body writes it, and `*v.group_member_count` is
    // re-evaluated by the loop condition every iteration too, so this is naturally faithful.
    for (int32_t slot = 0; slot < *v.group_member_count; ++slot) {
        // 0x0041dc38-0x0041dc4f: snapshotted ONCE into stack locals by the original and reused for the
        // rest of the iteration (including across every callee call below) -- reproduced the same way,
        // NOT re-read from v.group_move_scratch[slot] after this point (unlike *v.cur_player, which the
        // asm DOES re-read fresh at every use -- see the per-use reads below).
        const group_scratch_member &member = v.group_move_scratch[slot];
        const int32_t               x1     = member.tile_col;
        const int32_t               y1     = member.tile_row;

        // 0x0041dc64: RETURN VALUE DISCARDED -- see the header banner's "THE DISCARDED tile_dist_wrapped
        // CALL". Called for real regardless (house rule 3: callees stay original); whether it has a
        // side effect beyond its return value is unresolved and listed in uncertainties[].
        c.tile_dist_wrapped(x1, y1, *v.group_anchor_x, *v.group_anchor_y);

        // 0x0041dc6c-0x0041dd7d: band selection over the member unit's 4 weapon slots. range_min_band
        // seeds at 0xff (matches the asm's `MOV dword[-0x30],0xff`, NOT 0), range_max_band seeds at 0.
        int32_t range_min_band = 0xff;
        int32_t range_max_band = 0;
        {
            // Fresh read of CUR_PLAYER for the roster index, matching the asm's own re-read at
            // 0x0041dca2 (no callee has run yet in this iteration, but kept as a fresh read for
            // consistency with every other CUR_PLAYER use in this function -- see the header banner).
            const uint16_t cur_player  = *v.cur_player;
            const unit    &member_unit = v.units[static_cast<int32_t>(cur_player) * v.caps.units +
                                              member.unit_idx];
            for (int32_t wslot = 0; wslot < UNIT_WEAPON_SLOTS; ++wslot) {
                const unit_weapon &w = member_unit.weapons[wslot];
                if (w.enabled_2 == 0) continue; // 0x0041dcbe
                const cfg_weapon &weapon_cfg = v.cfg_weapons[w.weapon_id];
                if ((target_type_mask & weapon_cfg.target) == 0) continue; // 0x0041dd02
                if (weapon_cfg.range_min[cur_player] < range_min_band) {   // 0x0041dd1d-0x0041dd3b
                    range_min_band = weapon_cfg.range_min[cur_player];
                }
                if (range_max_band < weapon_cfg.range_max[cur_player]) { // 0x0041dd57-0x0041dd75
                    range_max_band = weapon_cfg.range_max[cur_player];
                }
            }
        }

        if (range_min_band < range_max_band) {
            // ---- 0x0041ddb4-0x0041dde5: a usable band exists -- try the direct path build first. ----
            const uint16_t cur_player_for_build = *v.cur_player; // fresh read, 0x0041ddc2
            const int32_t  path_slot_id_for_build =
                v.units[static_cast<int32_t>(cur_player_for_build) * v.caps.units + member.unit_idx]
                    .path_slot_id;
            // Argument order per the register-order call (EAX,EDX,EBX,ECX,stack) == (x1,y1,range_max_band,
            // range_min_band,path_slot_id) -- range_max_band precedes range_min_band, matching the
            // committed prototype's (start_x,start_y,target_range,unused_reserved,path_slot_index); the
            // "unused_reserved" name is the existing, out-of-scope mh_calls.gen.h label for what is
            // actually range_min_band here (see uncertainties[] -- not renamed, that file is not ours).
            const int32_t build_result = c.pathfind_build_steps(x1, y1, range_max_band, range_min_band,
                                                                path_slot_id_for_build);
            if (build_result == 0) {
                // ---- 0x0041ddf2-0x0041deae: reachability pre-pass -----------------------------------
                // Clear ->route_mark on every region in the active list, then set it on the member
                // tile's own region and every one of that region's neighbors. llm_map_region.route_mark's
                // field comment: flood_reachable gates its flood on (route_mark!=0) -- this pass is what
                // restricts the two flood_reachable calls below (neither of which redoes this pass) to
                // the SAME member-region-plus-neighbors subgraph.
                for (llm_map_region *region = *v.region_list_head; region != nullptr;
                     region                 = region->next) {
                    region->route_mark = 0;
                }
                // Read via the mutable grid accessor -- sim_view has no read-only sibling for
                // _G_LLM_MAP_REGION_GRID (see the header banner's note on that comment's claim becoming
                // stale once this function's pure reads land).
                llm_map_region *member_region = own.region_cell_at(x1, y1).region;
                if (member_region != nullptr) {
                    member_region->route_mark = 1;
                    for (uint32_t nb = 0; nb < member_region->neighbor_count; ++nb) {
                        member_region->neighbors[nb]->route_mark = 1;
                    }
                }

                // 0x0041dee2: RETURN VALUE DISCARDED, called for its side effect on the region graph
                // (route_bfs_dist/route_parent, per llm_map_region's own field comments) that the SECOND
                // flood_reachable call + route_search below are expected to consume.
                c.region_flood_reachable(static_cast<int32_t>(pack_xy(*v.group_anchor_x, *v.group_anchor_y)),
                                         static_cast<int32_t>(pack_xy(x1, y1)));

                // ---- 0x0041deea-0x0041e5d6: two diamond-perimeter scans, radius=range_min_band then
                // radius=range_max_band (the band's two EDGES) -- picks the LOWEST NONZERO
                // (terrain_flags>>8) tile, ties resolved to the LATEST candidate evaluated (`<=`, not
                // `<`). See the header banner's "THE terrain_flags>>8 READ" -- no semantic label
                // asserted for that sub-byte.
                uint32_t       best      = FORMATION_TERRAIN_NOT_FOUND;
                int32_t        found_x   = 0;
                int32_t        found_y   = 0;
                const uint32_t wrap_mask = *v.path_wrap_mask;
                const int32_t  anchor_x  = *v.group_anchor_x;
                const int32_t  anchor_y  = *v.group_anchor_y;

                auto consider = [&](int32_t cand_x, int32_t cand_y) {
                    const uint32_t wx = static_cast<uint32_t>(cand_x) & wrap_mask;
                    const uint32_t wy = static_cast<uint32_t>(cand_y) & wrap_mask;
                    const uint32_t terrain =
                        own.region_cell_at(static_cast<int32_t>(wx), static_cast<int32_t>(wy)).terrain_flags >>
                        8;
                    if (terrain <= best && terrain != 0) {
                        best    = terrain;
                        found_x = static_cast<int32_t>(wx);
                        found_y = static_cast<int32_t>(wy);
                    }
                };
                auto scan_diamond = [&](int32_t radius) {
                    for (int32_t i = 0; i <= radius; ++i) {
                        consider(anchor_x - radius + i, anchor_y - i);
                        consider(anchor_x - radius + i, anchor_y + i);
                        consider(anchor_x + radius - i, anchor_y - i);
                        consider(anchor_x + radius - i, anchor_y + i);
                    }
                };
                scan_diamond(range_min_band);
                scan_diamond(range_max_band);

                if (best == FORMATION_TERRAIN_NOT_FOUND) {
                    // 0x0041e5db-0x0041e5ff: nothing found on either diamond -- see the header banner's
                    // "THE TWO GUARD CALLS": these are REAL, always-executed calls on this path, not a
                    // decompiler artifact. No GROUP_MEMBER_TILE write for this slot; move to the next.
                    c.stack_capacity_guard_0x20();
                    c.stack_capacity_guard_0x20();
                } else {
                    // ---- 0x0041e604-0x0041e781: a candidate WAS found at (found_x, found_y). --------
                    c.region_flood_reachable(static_cast<int32_t>(pack_xy(x1, y1)),
                                             static_cast<int32_t>(pack_xy(found_x, found_y))); // discarded

                    route_step &route0 = own.group_route_step_at(0);
                    c.region_route_search(pack_xy(x1, y1), static_cast<int16_t>(pack_xy(found_x, found_y)),
                                          &route0.dir_code); // discarded; writes route steps as a side effect

                    // Fresh CUR_PLAYER + path_slot_id read, matching the asm's SECOND, independent
                    // computation at 0x0041e649-0x0041e66d (not reused from path_slot_id_for_build above).
                    const uint16_t cur_player_for_append = *v.cur_player;
                    const int32_t  path_slot_id_for_append =
                        v.units[static_cast<int32_t>(cur_player_for_append) * v.caps.units +
                                member.unit_idx]
                            .path_slot_id;

                    // 0x0041e670-0x0041e69e: walk forward to the first zero .run_length -- APPEND, never
                    // overwrite from index 0. Read via the const view (v.path_buffers), matching every
                    // other sim/ TU's "read via view, write via store" split.
                    int32_t cursor = 0;
                    while (v.path_buffers[static_cast<int32_t>(cur_player_for_append) *
                                              PATH_WAYPOINTS_PER_PLAYER +
                                          path_slot_id_for_append * PATH_WAYPOINTS_PER_SLOT + cursor]
                               .run_length != 0) {
                        ++cursor;
                    }

                    // 0x0041e6a5-0x0041e71b: copy the route (written by region_route_search above, read
                    // back through the const view -- the pointer is live memory, not a snapshot, so this
                    // sees the callee's fresh write) into the path buffer until a zero run_length.
                    for (int32_t j = 0; v.group_route_steps[j].run_length != 0; ++j) {
                        path_waypoint &dst = own.path_buffer_at(cur_player_for_append, path_slot_id_for_append,
                                                                cursor);
                        dst.run_length     = v.group_route_steps[j].run_length;
                        dst.heading        = v.group_route_steps[j].dir_code;
                        ++cursor;
                    }
                    // 0x0041e71d-0x0041e75e: re-terminate the buffer.
                    path_waypoint &term = own.path_buffer_at(cur_player_for_append, path_slot_id_for_append,
                                                             cursor);
                    term.run_length     = 0;
                    term.heading        = 0;

                    // 0x0041e765-0x0041e781: GROUP_MEMBER_TILE[slot] = the FOUND tile, not the member's
                    // own. DECLARED NEED -- see the header banner and this file's top-of-file note.
                    own.group_member_tile_byte(slot * 2 + 0) = static_cast<uint8_t>(found_x);
                    own.group_member_tile_byte(slot * 2 + 1) = static_cast<uint8_t>(found_y);
                }
            }
            // else (build_result != 0): nothing further for this member -- matches the asm's JNZ
            // straight to the loop increment (no GROUP_MEMBER_TILE write at all).
        } else {
            // 0x0041dd85-0x0041ddaf: no usable band -- the cheap default: the member's own current tile.
            // DECLARED NEED -- see the header banner and this file's top-of-file note.
            own.group_member_tile_byte(slot * 2 + 0) = static_cast<uint8_t>(x1);
            own.group_member_tile_byte(slot * 2 + 1) = static_cast<uint8_t>(y1);
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void group_plan_formation_positions(uint8_t target_type_mask) {
    sim_state st = state();
    detail::group_plan_formation_positions(st.read, st.own, live_group_plan_formation_positions_calls(),
                                           target_type_mask);
}


} // namespace mh::sim
