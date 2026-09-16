//
// sim/sim_bldg_get_coords.h -- llm_strat_bldg_get_coords @0x00449b8a (0xd7 bytes), translated from
// the DISASSEMBLY (tmp/decomp/llm_strat_bldg_get_coords_00449b8a.asm).
//
// Converts a building's tile-grid (x,y) + its cfg Building def's (width,height) into a wrapped
// fine-pixel world coordinate, written through the two OUT params:
//   *out_x = (buildings[player][building_index].x * 0x20 + Building[building_id].width  * 0x10) & general.bw_mask
//   *out_y = (buildings[player][building_index].y * 0x20 + Building[building_id].height * 0x10) & general.bh_mask
//
// PURE / NO-WRITE, NO OUTWARD CALL: the body's only CALL is the inert utils_assert_stack_capacity
// prologue (translator brief rule 6 -- omitted). No `_calls` indirection struct is needed, same
// posture as sim_unit_fine_pos.h's four functions.
//
// NAMED `bldg_get_coords`, NOT `get_coords`: sim_unit_fine_pos.h already declares
// `mh::sim::get_coords` / `mh::sim::detail::get_coords` for the DIFFERENT original function
// llm_strat_unit_get_coords, with an identical (uint16_t, int32_t, int32_t*, int32_t*) shape --
// reusing the bare name here would redeclare that overload with a conflicting body. `bldg_` (dropped from
// the "no strat_ prefix" default only for disambiguation, per docs/conventions.md#strategic-vs-tactical)
// keeps this the original's own semantic name instead.
//
// THE MASK PAIR IS v.geom->bw_mask/bh_mask (offsets 0/4 of llm_strat_map_geom), NOT
// v.geom->width_mask/height_mask (offsets 8/32, the tile-space pair map_width_mask()/
// map_height_mask() read) -- see sim_state.h's own comment distinguishing the two pairs on the same
// struct, and this batch's _CONTEXT file. Read v.geom->bw_mask/bh_mask directly; there is no
// existing helper for the pixel-space pair.
//
// FIELD WIDTHS, confirmed against addr/mh_structs.gen.h rather than assumed from the Ghidra .c
// draft's implied widths (translator brief rule 7):
//   mh_map_object_building::x/y            -- uint8_t @ +0xc3/+0xc4 (MOVZX byte, asm 0x00449bea/0x00449c42)
//   mh_map_object_building::building_id    -- uint16_t @ +0x2      (MOVZX word, asm 0x00449bbe/0x00449c16)
//   mh_cfg_final_struct_Building::width    -- uint8_t @ +0xa       (MOVZX byte, asm 0x00449bcb)
//   mh_cfg_final_struct_Building::height   -- uint8_t @ +0x9       (MOVZX byte, asm 0x00449c23)
// All four zero-extend (MOVZX, never MOVSX) before the shift, matching plain unsigned reads through
// the typed struct fields -- no manual masking needed beyond the cast width.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_bldg_get_coords @0x00449b8a. Signature mirrors the committed export/call shape (out
// params are raw int32_t* here -- same "fine_coord" scalar convention sim_unit_fine_pos.h uses --
// the public wrapper below carries the same `int32_t*` pointee (TACT1-P C6, 2026-09-04), matching
// addr/mh_calls.gen.h's `llm_strat_bldg_get_coords(uint16_t, int32_t, int32_t*, int32_t*)`).
void bldg_get_coords(const sim_view &v, uint16_t player, int32_t building_index, int32_t *out_x, int32_t *out_y);

} // namespace detail

// Live wrapper: the logic applied to state().read. Signature matches the committed __watcall shape
// already in addr/mh_calls.gen.h and addr/mh_export.gen.h exactly.
void bldg_get_coords(uint16_t player, int32_t building_index, int32_t *out_x, int32_t *out_y);

namespace detail {
} // namespace detail

} // namespace mh::sim
