//
// tact/tact_tile_occupancy.h -- TACT1A batch A: rebuilding the tactical occupancy stamp.
//
//   llm_tact_tile_rebuild_occupancy_layer_for_map @0x0043356e (0x32)
//   llm_tact_tile_rebuild_occupancy_layer         @0x00433dfc (0x98)
//
// Writes `tile_objects[][].unit[0]` (mh::state::tile_occupancy, mode_planes.h) over the mission's
// active sub-block: clears it, stamps every active FX pool entry belonging to the reload map, then
// ORs in 0x80 for every live unit's tile. `_for_map` is a thin wrapper: it stamps the reload map id
// into a persistent scratch global (_G_LLM_TACT_OCCUPANCY_REBUILD_MAP_ID) and calls the other one,
// which has no parameter of its own and reads that global back -- a genuine side channel between
// the two, not a translation artifact (single caller, confirmed via find-cross-references).
//
// THE FX-POOL STAMP READ IS NOT WHAT ITS OWN AUTHOR (an earlier pass) DESCRIBED. It reads
// `_G_LLM_TACT_FX_POOL[i].travel_dx`'s FIRST THREE RAW BYTES (0x00433e41-0x00433e4c) as (col, row,
// stamp) for EVERY active entry owned by the reload map -- but `llm_tact_fx_spawn` (0x0042bdce,
// lines 37-40) zeroes travel_dx/travel_dy for every fx_type at spawn, and
// `llm_tact_fx_update_projectile` (0x00431654) only ever advances travel_dx inside its
// `fx_type < 0x20` arm (gated at 0x004316b7). So for a "pure animation effect" (fx_type >= 0x20)
// these three bytes are provably always (0,0,0) for the entry's whole life, and for a moving
// projectile they are the low mantissa bytes of an accumulated displacement double -- not a
// designed col/row/stamp encoding. See the Ghidra `llm_tact_fx::travel_dx` field comment
// for the full derivation. Preserved literally here (Law 2): this header does NOT
// assert the three bytes mean anything, only that they are read and used exactly where the
// original reads and uses them.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_tile_rebuild_occupancy_layer @0x00433dfc.
//
// 1. Clear `.unit[0]` for the full [0,width) x [0,height) sub-block (0x00433e07-0x00433e1f); the
//    original's loop composes the EAX tile index as (col<<8)|row via two 8-bit counters (AH=col,
//    AL=row), matching mode_planes.h's own (x<<8)|y indexing exactly.
// 2. For each of the 1024 FX pool entries (0x00433e26-0x00433e53): skip if fx_type==0 (free slot)
//    or owner != map_id; otherwise stamp tile_objects[col][row].unit[0] = stamp, where
//    (col,row,stamp) are travel_dx's first three raw bytes -- see this header's derivation above.
// 3. For unit slots [0,64) of _G_LLM_TACT_UNITS ONLY -- NOT all 129, NOT starting at
//    TACT_UNIT_FIRST_SLOT (0x00433e5a/0x00433e66): skip an empty slot (type==0), a slot whose type
//    exceeds 0x80, an owner mismatch, or anim_state==0x1f (dying/dead); otherwise OR 0x80 into the
//    unit's own tile (0x00433e80-0x00433e88).
void tile_rebuild_occupancy_layer(const tact_view &v, tact_store &own);

// llm_tact_tile_rebuild_occupancy_layer_for_map @0x0043356e. Stores `map_id` into the scratch
// global and calls the function above (0x00433591).
void tile_rebuild_occupancy_layer_for_map(const tact_view &v, tact_store &own, int32_t map_id);

} // namespace detail

void tile_rebuild_occupancy_layer();
void tile_rebuild_occupancy_layer_for_map(int32_t map_id);


} // namespace mh::tact
