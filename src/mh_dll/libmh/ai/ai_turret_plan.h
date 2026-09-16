//
// ai/ai_turret_plan.h -- the AI's turret-upgrade planner (RI-AI / AI1B, batch B layer 3).
//
// One of the planners llm_strat_ai_plan_construction runs. Two independent passes over the
// player's own building roster, gated on a cached turret/defense candidate type, with an early
// return between them:
//
//   Pass 1 -- place a NEW turret of that type near whichever of the player's own buildings is not
//             reachable (through the player's own tiles) from the mother-base/HQ seed, by spiral-
//             scanning out to radius 15 from the midpoint of that building and the nearest building
//             that IS reachable.
//   Pass 2 -- if the candidate type is not already queued, find an existing building of that exact
//             type whose removal would NOT change which other buildings are reachable (a cut-vertex
//             test: re-run the reachability fill excluding it and diff the two flag planes), and
//             restart its construction. At most one order per call.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// llm_strat_ai_plan_turret_upgrade @0x004e4f1f.
//
// GATE (0x004e4f48-0x004e4f50): player_data[player].ai_turret_candidate == -1 -> return. Nothing
// else happens -- not even the flood fill.
//
// PASS 1 (0x004e4f5f-0x004e515f). llm_strat_bldg_connectivity_flood_fill(player, 0,
// own.bldg_connectivity_base) first (exclude_bldg_idx=0, i.e. nothing excluded -- the FULL graph),
// then the roster walk (i = 1.., "remaining" = buildings[player][0].index, the standard
// count-driven/empty-slot-does-not-consume-budget shape -- see ai_turret_threat.cpp). Per
// surviving building i, all of the following gate the rest of the body and, on ANY of them
// failing, fall straight through to `++i` (0x004e513a) with no other effect:
//   1. bldg_connectivity_base[i] != 0 -> skip. The pass acts on buildings that are NOT connected
//      to the mother-base/HQ seed through the player's own tiles.
//   2. !bldg_is_alive(player, i) -> skip.
//   3. state == 0x6b (dismantling) || 2 (destroyed) || 3 (dismantle-finish) -> skip.
//   4. near = find_nearest_flagged_building(player, b.x, b.y); near == -1 -> skip. `near` is a
//      roster INDEX into the player's own buildings (the nearest one whose _BASE flag IS set).
//   5. tile_midpoint_wrapped(b.x, b.y, near.x, near.y, &mid_x, &mid_y) -- __cdecl, six stack args.
//      Argument order derived from the push sequence at 0x004e501a-0x004e504f (cdecl pushes
//      right-to-left, so reading the pushes BOTTOM-UP gives the parameter order): x0=b.x, y0=b.y,
//      x1=near.x, y1=near.y, out_x=&mid_x, out_y=&mid_y.
//   6. Spiral scan k = 0 .. spiral_ring_cell_counts[15) (radius 15 -- element 15 of the same
//      uint[128] table every other AI spiral scan reads, read through the view, not a separate
//      region). Per k:
//        x = (mid_x + spiral_offsets[k].dx) & *map_width_mask   (signed byte offset, unsigned AND)
//        y = (mid_y + spiral_offsets[k].dy) & *map_height_mask
//        t = tile_at(x, y).
//        If (t.class_owner & 0xf) == player AND (t.class_owner & 0x40) != 0 (an OWNED, BUILDING
//        tile): the original's JNZ at 0x004e50af targets 0x004e513a, the OUTER loop's `++i` --
//        NOT the next spiral cell. This ABANDONS THE WHOLE BUILDING, it does not `continue` the
//        k-loop. A careless transcription that turns this into "skip this cell, keep scanning" is
//        wrong.
//        Otherwise: footprint_scan_for_blocked_cell(passable, *map_width, *map_height,
//        &cfg_buildings[candidate].area, 10, 10, x, y) -- polarity INVERTED from its name: NONZERO
//        means the footprint FITS. Fits -> bldg_queue_construction(player, candidate, x, y), then
//        notify_map_changed(player, candidate, x, y), then break (falls to `++i`). Does not fit ->
//        next k.
//
// BETWEEN THE PASSES (0x004e515f-0x004e5189): bldg_type_already_queued(player, candidate) != 0 ->
// return.
//
// PASS 2 (0x004e5189-0x004e52bf). Same roster-walk shape, its OWN "remaining" latch. Per surviving
// building i: building_id != candidate -> skip; state == 0x6b || 2 || 3 -> skip. Otherwise:
//   - llm_strat_bldg_connectivity_flood_fill(player, i, own.bldg_connectivity_trial) -- the SAME
//     fill, with building i itself EXCLUDED this time.
//   - Inner walk j = 1.. over the SAME roster with its OWN SEPARATE budget latch (buildings[player]
//     [0].index read AGAIN, into a different variable from the outer walk's "remaining" --
//     0x004e5244 reloads it fresh rather than sharing 0x004e51... 's copy). Per live slot j: if
//     bldg_connectivity_trial[j] != bldg_connectivity_base[j], set blocked=true and BREAK the inner
//     loop IMMEDIATELY (skipping that iteration's budget decrement entirely -- 0x004e528a jumps
//     straight to the post-loop check, not through the DEC). Otherwise the budget IS decremented
//     and the walk continues.
//   - blocked == false -> bldg_order_restart_construction_enqueue((uint16_t)player, i) and RETURN
//     (at most one order per call). blocked == true -> fall through to the outer walk's own
//     decrement/increment and continue with the next i.
//
// Both connectivity planes are indexed by building SLOT, 1..99 -- slot 0 is never written by
// anyone (confirmed from llm_strat_bldg_connectivity_flood_fill's own listing: its fill index
// starts at 1). They are PRIVATE to this function: nothing else in the image references either.
//
// Returns void. The tail `JMP 0x004e792e` (both the gate-fail path and every fall-through) is the
// shared Watcom epilogue, not a call.
void plan_turret_upgrade(const ai_view &v, const ai_store &own, const ai_calls &gc,
                         uint32_t player);

} // namespace detail

void plan_turret_upgrade(uint32_t player);

} // namespace mh::ai
