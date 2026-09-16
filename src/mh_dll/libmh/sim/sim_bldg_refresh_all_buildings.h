//
// sim/sim_bldg_refresh_all_buildings.h -- llm_strat_refresh_all_buildings @0x00470b56 (135 B),
// translated from the DISASSEMBLY (tmp/decomp/llm_strat_refresh_all_buildings_00470b56.asm).
//
// After a power change (called from llm_strat_power_recompute -- see the `ratio` field comment in
// mh_structs.gen.h), walks player `player`'s whole building roster and calls the ORIGINAL
// llm_strat_refresh_building on every occupied slot, so it can re-apply the operational/efficiency
// state each building's power gate implies.
//
// THE OCCUPANCY IDIOM, RE-DERIVED FROM THIS FUNCTION'S OWN ASM (per the brief: do not assume it
// matches sim_bldg_placement_enclosure.cpp's sibling idiom without checking) -- both the live-slot
// COUNT (buildings[player][0].index, read once before the loop) and the per-slot occupancy test
// (buildings[player][slot].index != 0, re-read every iteration) key off `.index`, NOT
// `.building_id`:
//   0x00470b6e-0x00470b82  MOVZX EAX, word ptr [Building_base + player*0x6aa4 + 0xc3d2a0]  -- the
//     dword at offset 0 of buildings[player][0], i.e. `.index` (mh_map_object_building::index,
//     declared int16_t) -- zero-extended (MOVZX, not MOVSX) into the 32-bit live-count local.
//   0x00470bb5-0x00470bbd  CMP word ptr [Building_base + player*0x6aa4 + slot*0x111 + 0xc3d2a0],0
//     -- the SAME field, `buildings[player][slot].index`, compared to zero every iteration.
//
// THE LOOP BOUND IS TWO CONDITIONS ANDed TOGETHER (0x00470b8c-0x00470b96): `slot < 100` (
// BUILDINGS_PER_PLAYER, already in sim_state.h) AND `live_count > 0` (a SIGNED compare, JG) --
// either one ends the walk. live_count is decremented only when a slot is occupied
// (0x00470bbf-0x00470bc2), so it is a countdown of remaining occupied slots, not a slot cursor --
// preserved as plain int32_t arithmetic (no clamp) so an inaccurate `.index` count (more occupied
// slots found than it claimed) drives live_count negative exactly as the original's signed compare
// would observe, rather than being "fixed" into a clamp the original does not have.
//
// The player parameter is stored as a full 32-bit dword on the original's stack frame but every
// subsequent use re-reads it through a 16-bit MOVZX (0x00470b71/0x00470ba2) -- i.e. only the low 16
// bits ever participate in the row-address arithmetic, matching the Ghidra .c draft's own
// `player & 0xffff`. Reproduced here as an explicit narrowing to uint16_t before first use.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one external callee this closure reaches (llm_strat_refresh_building, already committed in
// mh_calls.gen.h -- NOT part of this migration slice, stays original), indirected for offline
// testability -- same reason as sim_bldg_placement_preview.h's `placement_preview_calls`.
struct refresh_all_buildings_calls {
    // llm_strat_refresh_building @0x004705de. EAX=player (uint16_t, per the asm's own MOVZX-into-
    // param read at the call site), EDX=building slot index.
    void (*refresh_building)(uint16_t player, int32_t building_index);
};

const refresh_all_buildings_calls &live_refresh_all_buildings_calls();

namespace detail {

// llm_strat_refresh_all_buildings @0x00470b56. See the header banner above for the derivation.
void refresh_all_buildings(const sim_view &v, const refresh_all_buildings_calls &gc, uint32_t player);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter type matches the committed prototype (addr/mh_export.gen.h's sig_llm_strat_refresh_all_
// buildings, `void(__cdecl *)(uint32_t player)`) and addr/mh_calls.gen.h's own out-call wrapper.

void refresh_all_buildings(uint32_t player);

namespace detail {
} // namespace detail

} // namespace mh::sim
