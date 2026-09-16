//
// sim/sim_storage_get_approach_tile.h -- llm_strat_storage_get_approach_tile @0x0048b37c (0x22b
// bytes), translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_storage_get_approach_tile_0048b37c.asm), not from the Ghidra .c draft (which
// reads correctly here and was used as the initial map, but see the DECLARED NEED below for one place
// its rendering hides a real gap in the type database).
//
// Resolves the tile a unit should approach/stand on when docking into (or launching from) a storage
// slot, writing the result through two OUT params. For most storage types this is simply the storage
// slot's own exit tile; for a HELI-class docking unit (cfg Unit.move_op_code == 0x11) hovering over an
// ELEVATOR-type building, the exit tile is additionally offset by the building's vertical elevation
// delta stepped along its facing direction, so the helicopter approaches from beside the elevator
// rather than trying to land on top of it.
//
//   resolved_slot = (storage_idx != 0) ? storage_idx : units[player][unit_index].home_storage_slot;
//
//   if (Unit[units[player][unit_index].unit_proto_id].move_op_code == 0x11) {   // heli mover class
//       building_type_id = buildings[player][unit_storage[player][resolved_slot].b_index].building_id;
//       facing           = Building[building_type_id].<facing byte @+0x820>;    // DECLARED NEED, see below
//       diff             = (Unit[unit_proto_id].elevation / 32) - (Unit[unit_proto_id].elevation_2 / 32);
//       *out_fine_x = map_width_mask  & (unit_storage[...].exit_tile_x - diff * dir_step_offsets[facing].dx);
//       *out_fine_y = map_height_mask & (unit_storage[...].exit_tile_y - diff * dir_step_offsets[facing].dy);
//   } else {
//       *out_fine_x = unit_storage[player][resolved_slot].exit_tile_x;   // no wrap mask applied
//       *out_fine_y = unit_storage[player][resolved_slot].exit_tile_y;   // no wrap mask applied
//   }
//
// PURE / NO-WRITE, NO OUTWARD CALL: the body's only CALL is the inert utils_assert_stack_capacity
// prologue (translator brief rule 6 -- omitted). No `_calls` indirection struct needed, same posture
// as sim_bldg_get_coords.h.
//
// ---- THE "elevation / 32" SHIFT FORM: RESOLVED VIA THIS CODEBASE'S OWN fine_to_tile() PRECEDENT ------
// The assembly computes each of Unit.elevation/32 and Unit.elevation_2/32 via
// `SAR EDX,0x1f; SHL EDX,0x5; SBB EAX,EDX; SAR EAX,0x5` (0x0048b464-0x0048b46c and three siblings)
// -- BYTE-FOR-BYTE the same instruction sequence (same shift-by-5, same divisor 32) as the
// "tile<-fine conversion" idiom sim_order_enqueue.cpp's fine_to_tile(), sim_bldg_state_destroyed.cpp/
// .h, sim_prod_shuttle_complete.h, and sim_unit_state_predicates.cpp each independently re-derived and
// verified equals plain C truncating `/ 32` (IDIV-equivalent, NOT floor-division -- they differ on
// negatives, but this exact shift form and `/ 32` provably do not, which is what each of those four
// derivations confirmed against the same raw bytes this function also uses). Per this project's
// established "each TU re-derives its own copy" convention for that idiom, this file's .cpp declares
// its own file-local `fine_to_tile()` rather than reproducing the raw shift-and-bias expression inline
// or depending on another TU's copy.
//
// (Cross-checked separately against the Ghidra .c draft's own rendering, which uses `iVar4 << 4 < 0`
// -- shift by 4, not 5: both are correct, because the carry flag after `SHL reg,5` equals the sign bit
// of `reg << 4`, i.e. the draft's n-1 rendering is a faithful symbolic derivation of the real n=5
// shift's CF, not a transcription slip.)
//
// ---- THE TWO OUTPUT BLOCKS ARE NOT FACTORED TOGETHER -------------------------------------------------
// building_type_id / unit_proto_id are each computed ONCE by the original (0x0048b3fb-0x0048b44e) and
// held in stack locals -- reproduced here as locals computed once, referenced by both blocks. The
// ELEVATION diff, however, IS independently re-derived by each of the X-block (0x0048b3fb-0x0048b4d5)
// and Y-block (0x0048b4d7-0x0048b55e) straight from Unit[unit_proto_id].elevation/elevation_2 in
// memory -- two separate re-reads with no intervening write, so the two diffs are provably equal, but
// per the translator brief's explicit instruction for this function, the .cpp does NOT factor this
// into one shared computation; each block re-derives it exactly as the assembly does.
//
// ---- DECLARED NEED: cfg_final_struct_Building has no named field at +0x820 --------------------------
// The facing/orientation byte the original reads as `Building[building_type_id].field_0x820`
// (0x0048b49e/0x0048b527, `MOVZX EDX, byte ptr [EDX + 0xd9f4a0]`) falls INSIDE
// `mh_cfg_final_struct_Building::_pad_0x78e[160]` (that block spans 0x78e..0x82e per
// addr/mh_structs.gen.h's own offsetof asserts; 0x820 is within it) -- there is no individual field to
// read through the generated header today. Absolute address 0xd9f4a0 resolves against base 0xd9ec80
// (== 0xd9f4a0 - 0x820), the SAME Building[] table base the struct's own `fuel` field comment already
// cites independently, corroborating both the base and this offset. This is exactly the split-a-field-
// out-of-a-pad-block pattern the struct's own history already used for `pip_slot_count` and
// `shuttle_pad_offset_x/y` -- propose an `uint8_t facing;` field at cfg_final_struct_Building+0x820
// (splitting `_pad_0x78e[160]` into `_pad_0x78e[0x92]` + `facing` + `_pad_0x821[13]`), indexed by
// llm_strat_storage_get_approach_tile into the 40-entry `dir_step_offsets` table (sim_view already
// binds this; no range check in the original, so none is added here). Written AS IF that field already
// exists (named `facing` below) per this codebase's established declared-need convention (see
// sim_prod_shuttle_depart.h/sim_bldg_get_coords.h) -- this file will not compile until the conductor
// adds the field and regenerates addr/mh_structs.gen.h.
//
// ---- PARAMETER NAMES ---------------------------------------------------------------------------------
// Matches the caller's already-established naming in sim_order_enqueue.cpp/.h (both call sites already
// pass `out_fine_x`/`out_fine_y`/`storage_idx` through the `gc.storage_get_approach_tile(...)` function
// pointer): `unit_index` for param_2, `out_fine_x`/`out_fine_y` for a2/param_4, `storage_idx` for
// param_5 (0 => use the unit's own home_storage_slot).
//
// No floats anywhere in this function's body.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_storage_get_approach_tile @0x0048b37c. See the header banner above for the full
// derivation; the .cpp carries the per-branch address citations. Out params are raw uint32_t* here,
// matching the committed `uint *a2, uint *param_4` signature -- the public wrapper below carries the
// same committed pointee type (TACT1-P C6, 2026-09-04).
void storage_get_approach_tile(const sim_view &v, uint16_t player, uint16_t unit_index,
                               uint32_t *out_fine_x, uint32_t *out_fine_y, uint32_t storage_idx);

} // namespace detail

// Live wrapper: the logic applied to state().read. Signature matches the committed __watcall shape
// already in addr/mh_calls.gen.h and addr/mh_export.gen.h exactly.
void storage_get_approach_tile(uint16_t player, uint16_t unit_index, uint32_t *out_fine_x,
                               uint32_t *out_fine_y, uint32_t storage_idx);

namespace detail {
} // namespace detail

} // namespace mh::sim
