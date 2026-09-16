//
// ai/ai_turret_plan.cpp -- see ai_turret_plan.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_plan_turret_upgrade_004e4f1f.asm), not from Ghidra's C: the .c invents
// an `extraout_ECX` for pass 2's roster-count reload (it is simply ECX kept live across the whole
// function, computed fresh at 0x004e514e-0x004e5153 as player*0x6aa4 -- the building row stride --
// and reused unmodified through both passes) and calls `llm_strat_tile_midpoint_wrapped()` with NO
// arguments at all, which is a Ghidra artifact of the callee being __cdecl with a committed
// prototype the .c's era did not have; the six real arguments are all still on the stack right
// after that call site in the assembly.
//
#include "ai/ai_turret_plan.h"


namespace mh::ai {
namespace detail {

void plan_turret_upgrade(const ai_view &v, const ai_store &own, const ai_calls &gc,
                         uint32_t player) {
    // GATE (0x004e4f48-0x004e4f50). Nothing else happens on this path -- not even the flood fill.
    if (v.players[player].ai_turret_candidate == -1) return;

    // Cached once: nothing in this function's own callee closure sets a player's turret candidate
    // (it is written by the candidate-selection pass elsewhere in the AI, never called from here),
    // so the value is stable for the whole call despite the original re-loading it from memory at
    // every use site (0x004e50d5, 0x004e5105, 0x004e511e, 0x004e5173, 0x004e51f2) rather than
    // keeping it in a register -- see uncertainties[].
    const int32_t candidate = v.players[player].ai_turret_candidate;

    // ---- Pass 1: place a NEW turret near a disconnected building (0x004e4f5f-0x004e515f) --------
    //
    // exclude_bldg_idx=0 -- nothing excluded, this is the FULL reachability graph from the
    // mother-base/HQ seed through the player's own tiles.
    gc.bldg_connectivity_flood_fill(player, 0, own.bldg_connectivity_base);

    {
        // Standard count-driven roster walk (see ai_turret_threat.cpp): an empty slot advances `i`
        // without consuming `remaining`, and `i` is unbounded.
        int32_t  i         = 1;
        uint32_t remaining = (uint16_t)building_of(v, player, 0).index;
        while (remaining != 0) {
            const building &b = building_of(v, player, i);
            if (b.building_id == 0) {
                ++i;
                continue;
            }
            --remaining;

            // Every one of the four gates below, and every exit out of the spiral scan, falls
            // straight through to `++i` at the bottom of this block with no other effect -- exactly
            // what the original's shared target 0x004e513a does for all of them.
            if (own.bldg_connectivity_base[i] == 0 && // NOT connected to the seed
                gc.bldg_is_alive((int32_t)player, i) != 0 &&
                b.state != 0x6b && b.state != 2 && b.state != 3) {
                // Named `nearest_idx`, not `near` -- `near`/`far` are legacy MSVC keyword-macros and
                // not worth the risk of a transitive collision.
                const int32_t nearest_idx =
                    gc.find_nearest_flagged_building((int32_t)player, b.x, b.y);
                if (nearest_idx != -1) {
                    const building &nb    = building_of(v, player, nearest_idx);
                    int32_t         mid_x = 0, mid_y = 0;
                    // __cdecl, six stack args; order derived from the push sequence, not the
                    // committed parameter names -- see the header.
                    gc.tile_midpoint_wrapped(b.x, b.y, nb.x, nb.y, &mid_x, &mid_y);

                    for (uint32_t k = 0; k < v.spiral_ring_cell_counts[15]; ++k) {
                        const int32_t      x = (int32_t)((uint32_t)(mid_x + v.spiral_offsets[k].dx) &
                                                    *v.map_width_mask);
                        const int32_t      y = (int32_t)((uint32_t)(mid_y + v.spiral_offsets[k].dy) &
                                                    *v.map_height_mask);
                        const tile_object &t = tile_at(v, x, y);
                        if ((int32_t)(t.class_owner & 0xf) == (int32_t)player &&
                            (t.class_owner & 0x40) != 0) {
                            // 0x004e50af jumps to 0x004e513a, the OUTER loop's `++i` -- this
                            // ABANDONS THE WHOLE BUILDING, it does not skip just this spiral cell.
                            break;
                        }
                        // Return polarity is INVERTED from the callee's name: nonzero == fits.
                        uint8_t *passable       = const_cast<uint8_t *>(v.passable);
                        uint8_t *footprint_mask = const_cast<uint8_t *>(&v.cfg_buildings[candidate].area[0][0]);
                        if (gc.footprint_scan_for_blocked_cell(passable, *v.map_width, *v.map_height,
                                                               footprint_mask, 10, 10, x, y) != 0) {
                            gc.bldg_queue_construction((int32_t)player, candidate, (int16_t)x,
                                                       (uint16_t)y);
                            gc.notify_map_changed((int32_t)player, candidate, x, y);
                            break;
                        }
                    }
                }
            }
            ++i;
        }
    }

    // ---- Between the passes (0x004e515f-0x004e5189) -----------------------------------------------
    if (gc.bldg_type_already_queued((int32_t)player, (uint32_t)candidate) != 0) return;

    // ---- Pass 2: restart construction on a halted turret, unless it is load-bearing ---------------
    // (0x004e5189-0x004e52bf). Same roster-walk shape as pass 1, its own "remaining" latch.
    {
        int32_t  i         = 1;
        uint32_t remaining = (uint16_t)building_of(v, player, 0).index;
        while (remaining != 0) {
            const building &b = building_of(v, player, i);
            if (b.building_id == 0) {
                ++i;
                continue;
            }

            if ((int32_t)b.building_id == candidate && b.state != 0x6b && b.state != 2 &&
                b.state != 3) {
                // Re-run the SAME fill with building i itself excluded.
                gc.bldg_connectivity_flood_fill(player, i, own.bldg_connectivity_trial);

                bool blocked = false;
                {
                    // The inner walk's OWN, SEPARATE budget latch -- 0x004e5244 reloads
                    // buildings[player][0].index fresh rather than sharing the outer walk's copy
                    // (EBP-0x24). Do not merge the two counters.
                    int32_t  j            = 1;
                    uint32_t inner_remain = (uint16_t)building_of(v, player, 0).index;
                    while (inner_remain != 0) {
                        if (building_of(v, player, j).building_id != 0) {
                            if (own.bldg_connectivity_trial[j] != own.bldg_connectivity_base[j]) {
                                // A slot where the two planes disagree is a building `i` was the
                                // only connection for -- the cut-vertex test. Break IMMEDIATELY:
                                // 0x004e528a skips straight past the budget decrement, unlike every
                                // other early-out in this file.
                                blocked = true;
                                break;
                            }
                            --inner_remain;
                        }
                        ++j;
                    }
                }

                if (!blocked) {
                    gc.bldg_order_restart_construction_enqueue((uint16_t)player, i);
                    return; // at most one order per call
                }
            }
            --remaining;
            ++i;
        }
    }
    // Falling off the end of the walk (0x004e52bf -> 0x004e792e, the shared Watcom epilogue) is an
    // ordinary return.
}

} // namespace detail

void plan_turret_upgrade(uint32_t player) {
    const ai_state st = state();
    detail::plan_turret_upgrade(st.read, st.own, live_calls(), player);
}


} // namespace mh::ai
