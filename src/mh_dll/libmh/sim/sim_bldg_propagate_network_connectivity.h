//
// sim/sim_bldg_propagate_network_connectivity.h -- llm_strat_bldg_propagate_network_connectivity
// @0x0049209b (0x293 bytes), translated from the DISASSEMBLY (tmp/decomp/
// llm_strat_bldg_propagate_network_connectivity_0049209b.asm) -- SIM1B building_tick machinery,
//
// The recursive flood-fill core of the power-network connectivity graph. Called from
// llm_strat_bldg_power_network_recompute (also this slice) once per network root. Marks the current
// building connected (llm_bldg_set_connected_flag, an ORIGINAL callee), and -- only if the building is
// online AND not a turret (turrets are power sinks, never conduits: BUILDING_TYPE_H_TURRET/A_TURRET,
// sim/sim_order_enqueue.h) -- scans a 31x31 tile window centered on its footprint for same-owner,
// unbuilt (built_flags bit0 clear), energized (energy>0) buildings and recurses into each hit.
//
// SELF-RECURSION IS A GENUINE LOCAL C++ CALL (detail::propagate_network_connectivity calling
// itself), NOT routed through mh::call:: -- matching sim_unit_create_soldier.h's established
// precedent for its own recursion. CORRECTED 2026-08-13: this translation
// originally bound the recursive call to mh::call::llm_strat_bldg_propagate_network_connectivity
// (the function's own original address), on the theory that "callees stay original" applies to
// every outward call including self-recursion. It measurably does not: an entry-point seam patches
// the function's ENTRY, so a raw-address call to it (from ANYWHERE, including from inside this
// function's own shadow arm) lands on the shadow hook once one is installed -- every recursion
// depth re-triggered a nested snapshot/restore cycle and produced a ~38% false divergence rate on
// `buildings`' connected-flag bit, confirmed as a harness artifact rather than a translation defect
// (two independent reimpl-verify passes had already read the pre-fix body as equivalent against the
// disassembly). "Callees stay original" is about calls to OTHER, un-promoted sibling functions,
// which production genuinely reaches via the original address pre-promotion; a function's OWN
// recursive edge is different in kind, and a direct C++ self-call is also the one that becomes
// correct AFTER promotion (a promoted body should recurse into itself, not back out to the retired
// original).
//
// THE WINDOW-CENTER ARITHMETIC (0x004920e1-0x0049220a in the asm), re-derived directly from the
// disassembly and cross-checked against the committed struct offsets (mh_structs.gen.h), NOT copied
// from the Ghidra .c draft's operator-precedence grouping:
//   half_width  = cfg_buildings[building_id].width  / 2   (uint8_t field @+0xa,  always >=0 so plain
//                                                           SAR-based division-by-2 == a right shift --
//                                                           the asm's sign-correction is dead code here)
//   half_height = cfg_buildings[building_id].height / 2   (uint8_t field @+0x9)
//   x_center = (half_width  + buildings[player][b_index].x) & geom->width_mask   (building.x @+0xc3)
//   y_center = (half_height + buildings[player][b_index].y) & geom->height_mask  (building.y @+0xc4)
//   x_start  = (x_center - 0xf) & geom->width_mask   -- computed ONCE, before the outer (x) loop
//   y_start  = (y_center - 0xf) & geom->height_mask  -- RECOMPUTED fresh at the top of every outer
//                                                        iteration (0x0049223a), not just once
// i.e. width pairs with x/width_mask and height pairs with y/height_mask -- the natural pairing. (A
// prior misreading briefly had these swapped by misidentifying which struct offset was width vs.
// height; static_assert(offsetof(mh_cfg_final_struct_Building, height) == 0x9) /
// static_assert(offsetof(..., width) == 0xa) settle it, and the asm's own field-base literals
// (0xd9ec89 = type_base+1 = height, 0xd9ec8a = type_base+2 = width) confirm the natural pairing holds.)
//
// v.geom->width_mask/height_mask are read DIRECTLY (not through sim_state.h's map_width_mask()/
// map_height_mask() free helpers) -- both those helpers and this function's own literal reads
// (0x00e15398/0x00e153b0 in the asm) resolve to the SAME `general` struct fields, but sim_view also
// carries a second, independently-bound width_m/height_m pair (RID_WIDTH_M/RID_HEIGHT_M, "the AI
// spiral scanners' own wrap mask" per sim_state.h) at a DIFFERENT address -- reading the field off
// `v.geom` directly, by name, removes any chance of reaching for the wrong pair.
//
// THE OUTER/INNER LOOPS are each 31 iterations (0x1f in the asm), tile_x/tile_y cursors incrementing
// by 1 and re-wrapping via `& width_mask`/`& height_mask` after each iteration (post-increment, matches
// the Ghidra .c draft's loop shape) -- covering a 31x31 window centered on (x_center, y_center) with a
// radius-15 extent on each axis (x_start = x_center-15 .. x_center+15, same for y).
//
// THE TILE TEST, per iteration (tile_at(v, tile_x, tile_y), same (tile_x<<8)|tile_y indexing
// sim_state.h's own helper uses -- verified byte-identical to the asm's `(tile_x<<11)+(tile_y<<3)`
// address arithmetic):
//   tile.class_owner == (player | ORDER_KIND_BUILDING)   -- same owner, and a BUILDING-class tile
//     (sim_order_enqueue.h's ORDER_KIND_BUILDING == 0x40, matching tile_object::class_owner's own
//     documented "hi nibble = object class, 0x40=building" encoding -- reused rather than a fresh 0x40u
//     literal)
//   AND buildings[player][tile.building].built_flags & 1 == 0        (not yet connected/unbuilt)
//   AND buildings[player][tile.building].energy > 0.0                (HP-like charge stat, NOT the
//     power resource -- see docs/conventions.md#energy-is-not-power; this function's own name is about power
//     connectivity but this specific gate is an aliveness check, not a power check)
// On a hit, recurses: propagate_network_connectivity(player, tile.building).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one EXTERNAL callee this closure reaches. Self-recursion is a direct C++ call, not a member
// here -- see the header banner.
struct propagate_network_connectivity_calls {
    void (*set_connected_flag)(uint16_t player, int32_t b_index); // llm_bldg_set_connected_flag @0x004965d6
};

const propagate_network_connectivity_calls &live_propagate_network_connectivity_calls();

namespace detail {

// llm_strat_bldg_propagate_network_connectivity @0x0049209b. See the header banner above for the full
// derivation. Read-only over sim_view -- this function writes no sim state itself (the only mutation,
// the connected flag, happens inside the ORIGINAL callee `gc.set_connected_flag`).
void propagate_network_connectivity(const sim_view &v, const propagate_network_connectivity_calls &gc,
                                    uint16_t player, int32_t b_index);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototype (addr/mh_export.gen.h's
// sig_llm_strat_bldg_propagate_network_connectivity, __watcall EAX=player/EDX=b_index) and
// addr/mh_calls.gen.h's own out-call wrapper.

void propagate_network_connectivity(uint16_t player, int32_t b_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
