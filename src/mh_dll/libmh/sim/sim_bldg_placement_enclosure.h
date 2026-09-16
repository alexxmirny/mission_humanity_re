//
// sim/sim_bldg_placement_enclosure.h -- two building-placement/connectivity walkers over the
// 31x31 (-15..15 inclusive on both axes) building-zone window (RI-SIM / SIM1B).
//
// Two functions:
//   llm_strat_bldg_connectivity_flood_fill              @0x004d77cc (0x262 bytes)
//   llm_strat_bldg_check_placement_encloses_neighbors    @0x004d8c7d (0x344 bytes)
//
// Grouped in one TU because they share the SAME 31x31-window building-zone idiom (a building's
// footprint centre = cfg_buildings[building_id].{width,height}/2 + building.{x,y}, offset by
// [-15,15] on each axis, masked by the torus-wrap masks) and the same buildings[player][slot]
// "live count in slot 0" occupied-scan idiom -- but they do NOT share state or call each other.
//
// ---- THE WIDTH_M/HEIGHT_M vs GEOM MASKS TRAP (both functions) -----------------------------------
// Both functions read _G_LLM's RID_WIDTH_M / RID_HEIGHT_M pair DIRECTLY (sim_view::width_m/
// height_m, `AND reg, dword ptr [0x00fe5b40]` / `[0x00fe5b44]` in the listings) -- NOT
// v.geom->width_mask/height_mask (a different pair of globals sim_state.h's `map_width_mask()`/
// `map_height_mask()` free functions read). Do not swap them in for the free-function helpers:
// same *shape* of mask, different address, and sim_view carries both bindings on purpose (see
// sim_state.h's own comment on `width_m`/`height_m`). Every wrap in this file goes through
// `*v.width_m` / `*v.height_m` raw, never through `map_width_mask(v)`/`map_height_mask(v)`.
//
#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // ORDER_KIND_BUILDING (0x40), BUILDING_TYPE_A_MOTHER/H_MOTHER
#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_bldg_connectivity_flood_fill @0x004d77cc. View-only (no sim_store, no callees besides
// the inert stack probe) -- `flag_array` is a CALLER-OWNED byte buffer (one entry per building
// slot), written through exactly like a normal C array, not sim state.
//
// SEED PASS (0x004d77ec-0x004d787f). Walks buildings[player][slot] for slot=1.. via the
// "buildings[player][0].index is the live occupied-slot COUNT" idiom (0x004d7809: MOVZX word at
// the array's own +0x0 -- `index`, NOT +0x2 `building_id`; the two fields are adjacent and easy to
// swap). For each OCCUPIED slot (building_id != 0, read at +0x2): flag_array[slot] = 1 iff
// cfg_buildings[building_id].type is BUILDING_TYPE_A_MOTHER (0x6) or _H_MOTHER (0x1a)
// (0x004d7854/0x004d785d), else 0. An EMPTY slot is skipped entirely -- flag_array[slot] is left
// exactly as the caller had it, never zeroed -- and does not count against the occupied total.
//
// FLOOD PASS (0x004d787f-0x004d7a2d), a `do { changed = false; ... } while (changed)` fixpoint --
// reproduce the outer restart loop, not a single sweep. Re-walks the same occupied-slot scan
// (re-reading the live count fresh each outer iteration). Per occupied slot: if
// `slot == exclude_bldg_idx` OR `flag_array[slot] == 0`, skip (still counts toward the occupied
// total). Otherwise scan the building's own 31x31 zone (dy outer, dx inner, both -15..15
// inclusive; centre = cfg_buildings[building_id].{height,width}/2 + building.{y,x}, wrapped by
// `*v.height_m`/`*v.width_m`): for every tile whose tile_objects.class_owner equals
// `player | ORDER_KIND_BUILDING` (0x004d79d1) and whose OWN slot's flag_array entry is still 0
// (read via tile_objects.building, 0x004d79d7-0x004d79e2), set that entry to 1 and mark the pass
// changed (0x004d79e7-0x004d79ea). No early return anywhere in this function (unlike its sibling
// below) -- it always runs to a fixpoint and returns void.
void connectivity_flood_fill(const sim_view &v, uint32_t player, int32_t exclude_bldg_idx,
                             uint8_t *flag_array);

// llm_strat_bldg_check_placement_encloses_neighbors @0x004d8c7d. Reads/writes the function's OWN
// private 256x256 scratch grid (sim_store::bldg_enclosure_scratch_at, RID_STRAT_BLDG_ENCLOSURE_
// SCRATCH) -- the closure's only reader/writer of that region. Every scratch access goes through
// the store accessor, for both reads and writes; there is no read-only view member for it.
//
// FOUR PASSES, IN ORDER:
//
//  (1) 0x004d8c98-0x004d8ce1: seed the WHOLE map extent (`*v.map_width` x `*v.map_height` --
//      the real tile-count extents, NOT `width`/`height` below, which are the proposed
//      footprint's own dimensions) from `v.passable`: scratch[tx,ty] = 0x80 if
//      passable[(tx<<8)|ty] == 0 (impassable), else 0.
//
//  (2) 0x004d8ce1-0x004d8d18: OR bit 0x1 over the PROPOSED footprint -- the function's own
//      `width` x `height` parameters, at (x, y), wrapped by `*v.width_m`/`*v.height_m`.
//
//  (3) 0x004d8d18-0x004d8e9b: for every occupied building in the player's own roster (same
//      "buildings[player][0].index is the live count" scan as the sibling above, same
//      exclude-nothing-here shape but WITHOUT an exclude parameter), OR bit 0x40 over that
//      building's own 31x31 zone (same centre-and-wrap idiom as the sibling: cfg_buildings
//      [building_id].{width,height}/2 + building.{x,y}, dx/dy in [-15,15], wrapped by
//      `*v.width_m`/`*v.height_m`). The INSTANT a stamped tile's byte reads `(byte & 0x41) ==
//      0x41` (blocked-by-pass-1 AND proposed-by-pass-2), RETURN 1 IMMEDIATELY -- before pass 3
//      even finishes iterating the remaining buildings/window cells (0x004d8e6d/0x004d8f72-
//      0x004d8f74, both re-checked below).
//
//  (4) 0x004d8e9b-0x004d8fb6: a SEPARATE fixpoint flood pass over the whole map extent again
//      (`*v.map_width` x `*v.map_height`). A tile with bit 0x40 SET and bit 0x80 CLEAR, whose 4
//      cardinal neighbours (x+1/x-1/y+1/y-1, each wrapped independently by `*v.width_m`/
//      `*v.height_m`) do NOT ALL have bit 0x40 set, gets its OWN 31x31 zone (centred on the tile
//      itself this time, NOT via a building record -- 0x004d8f2c/0x004d8f3f compute the window
//      directly off (tx,ty), unlike pass 3's building-footprint centre) OR'd with 0x40 too,
//      tracking whether any bit actually flipped 0->1 (the pass's own `changed` flag). Same
//      early-return-on-0x41 rule applies inside this window stamp as well (0x004d8f72-
//      0x004d8f74). If a full width x height sweep makes no change at all, return 0.
//
// Needs `sim_store &` (not const) purely for the scratch-grid accessor -- nothing else in this
// function writes sim state.
int32_t check_placement_encloses_neighbors(const sim_view &v, sim_store &own, int32_t player,
                                           int32_t x, int32_t y, uint32_t width, uint32_t height);

} // namespace detail

// ---- the public surface, matching the committed prototypes (addr/mh_calls.gen.h) exactly ------

void    connectivity_flood_fill(uint32_t player, int32_t exclude_bldg_idx, uint8_t *flag_array);
int32_t check_placement_encloses_neighbors(int32_t player, int32_t x, int32_t y, uint32_t width,
                                           uint32_t height);

namespace detail {
} // namespace detail

} // namespace mh::sim
