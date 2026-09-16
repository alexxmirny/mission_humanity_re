//
// tact/tact_unit_vision.h -- TACT1D: the tactical FOV/vision-refcount trio.
//
//   llm_tact_fov_probe_far_cell_and_door_state @0x0042e220 (0x165)
//   llm_tact_unit_vision_add                   @0x0042e385 (0x1ba)
//   llm_tact_unit_vision_remove                @0x0042e53f (0x199)
//
// All three share the same setup shape: stamp the calling unit's own position/facing/vision into
// the FOV scratch globals, then raycast (`llm_tact_fov_raycast_stencil`) to fill the 64x64
// `_G_LLM_TACT_FOV_STENCIL` (and, for the probe function, the two nearest-target cell trackers).
// `fov_probe_far_cell_and_door_state` calls the raycaster DIRECTLY (it stamps the five scratch
// globals itself); `unit_vision_add`/`unit_vision_remove` go through `llm_tact_vision_cone_setup`
// instead, which does the identical five stamps and then makes the SAME raycast call -- two call
// sites reaching the same setup, not two different setups.
//
// vision_cone_setup's last four stack args (`dead_outptr0..3`) are addresses of uninitialized
// caller-frame scratch; its own .asm never reads Stack[0x8..0x14], so they are passed as `nullptr`
// here -- confirmed dead, not merely assumed (`tmp/decomp_tact/llm_tact_vision_cone_setup_0042e1c9.asm`
// touches only EAX/EDX/EBX/ECX/Stack[0x4]).
//
// THE FOLDED -31/-31 INDEX BIAS (unit_vision_add/remove's stencil-to-tile write,
// "A FOLDED INDEX BIAS read as a separate global", 2026-08-24): the write target is
// `mh::state::mode_planes::tile_object_at(x, y)`, the SAME tile_objects array tact_unit_teleport.cpp
// already writes through -- NOT a second "tile_fog" region. The stencil loop's two index variables
// are (from the .asm's own storage slots) `j` = [EBP-0x3c] (the loop that resets every outer
// iteration) and `i` = [EBP-0x40] (the outer loop). The write address is built as:
//   EAX = (cur_col + j - 0x1f) << 0xb   |   EDX = (cur_row + i) << 0x3   |   addr = EAX + EDX + K
// i.e. only `j`'s -31 is an explicit `SUB EAX,0x1f` (0x0042e48e / 0x0042e4ce / 0x0042e511, add-region);
// `i`'s -31 is NOT a visible subtraction anywhere -- it is baked into the displacement constant
// itself. Proof: the "clean" (unbiased) displacement for tile_objects[...].visibility (field
// offset +7) would be `tile_objects_base(0xd1ec80) + 7 = 0xd1ec87`; the constant actually used is
// `0xd1eb8f = 0xd1ec87 - 31*8 (0xf8)`. The flags-word constant (`0xd1eb88 = 0xd1ec80 - 31*8`, field
// offset +0) matches the same derivation. So the real coordinates are
// `x = cur_col + j - 31`, `y = cur_row + i - 31` for BOTH variables, and the .asm reads as if only
// the column were biased purely because the compiler chose to bake the row's -31 into the constant
// instead of emitting a second SUB. `tile_object_at(x, y)` is called with these already-biased x/y,
// so the -31/-31 is explicit in THIS translation's index expressions even though only one -31 is an
// explicit instruction in the original.
//
// The stencil index itself is `j*64 + i` (`SHL EAX,0x6` on `j`, `ADD` `i`) -- center cell
// `31*64+31 = 0x7df`, matching tact_store's own fov_stencil_at comment.
//
// FOG BITS ARE BYTE OPS ON THE HIGH BYTE OF A LITTLE-ENDIAN uint16_t: `mh_map_tile_object_data`
// declares `flags` as `uint8_t flags[2]` (Ghidra never applied the word-as-two-bitfields union), so
// `flags[1]` IS the AND/OR target the .asm calls `BH` after `MOV BX, word ptr [...]` -- `flags[0]`
// (`BL`) is loaded and stored back unchanged in every RMW here. 0xbf/0x80/0xc0 on that high byte are
// exactly 0x4000/0x8000/0xc000 of the packed word (the map-render notes: 0x8000=explored,
// 0x4000=fogged). `unit_vision_add` clears FOGGED then sets EXPLORED (two separate RMWs, same net
// effect as `flags[1] = (flags[1] & 0xbf) | 0x80`); `unit_vision_remove` sets BOTH, but ONLY when
// the refcount (`.visibility`, field +7, INC'd by add / DEC'd by remove) reaches exactly 0 --
// `CMP` runs AFTER the `DEC` (0x0042e65b then 0x0042e67a), i.e. a POST-decrement test, reproduced
// here as `--tile.visibility == 0`.
//
// THE owner==1/see_enemy_flag GUARD (@0x0042e3a0-0x0042e3bb / @0x0042e55a-0x0042e575, identical in
// both functions): the .asm's two-branch shape reduces to "return immediately iff (owner==1 AND
// SEE_ENEMY_FLAG==0)" -- i.e. proceed when the unit is NOT owner 1, or when SEE_ENEMY_FLAG is set
// regardless of owner. Read the two JNZ/JZ/JMP targets carefully before "simplifying" this; the
// natural-looking `if (owner != 1 || see_enemy_flag) proceed` is the same condition, stated the
// other way around, and this header states it as the actual early-return guard to match the
// .asm's own control flow.
//
// llm_tact_fov_probe_far_cell_and_door_state's cell UNPACKING (@0x0042e2ba-0x0042e31c): both packed
// cells (`_G_LLM_TACT_FOV_NEAREST_HIBIT_CELL`/`_LOW_CELL`) are loaded via MOVZX (unsigned 16-bit),
// so the compiler's generic signed-divide-by-256 idiom (SAR/SHL/SBB for the quotient, IDIV for the
// remainder) operates on an always-non-negative 32-bit value; the "sign fixup" term is provably 0
// on every possible 16-bit input (see the .cpp for the full derivation) and the sequence reduces
// exactly to `hi = raw >> 8` (-> *out_far_col / *out_cell2_col) and `lo = raw & 0xff` (->
// *out_far_row / *out_cell2_row) -- the SAME (col<<8)|row packing `tile_object_at` uses. Declared in
// uncertainties[] anyway per the brief's own instruction to flag this pattern.
//
// THE "cell2"/DOOR CHECK (@0x0042e337-0x0042e37c) reads `tile_objects[far_col][far_row].building`
// (field +2, NOT bias-adjusted -- far_col/far_row are already absolute tile coordinates, unlike the
// vision loop's stencil-relative deltas above) to get an occupant id, then re-indexes
// `_G_LLM_TACT_UNITS` BY THAT ID (`IMUL ...,0x5f4` -- the tact_unit record stride, confirmed against
// `mh_tact_unit_record`'s own `size 0x5f4`) and reads `.anim_state` (field +5) off of it. So the
// occupant an FOV probe finds "at the far cell" is read back through the SAME unit table this
// function's own `unit_idx` parameter already indexes -- doors are apparently modeled as
// `_G_LLM_TACT_UNITS` entries whose `anim_state` (2/3, "progress anims" per that field's own
// comment) means "this door is mid-animation". Declared in uncertainties[] because it is genuinely
// a `unit_at()` access, not the `tile_object_at()` access a first reading of "door state" might
// suggest.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// Every outward call these three bodies make. `fov_raycast_stencil` is shared by all three;
// `vision_cone_setup` only by add/remove (the probe function stamps the scratch globals itself and
// calls the raycaster directly -- see the header comment above).
struct unit_vision_calls {
    void (*vision_cone_setup)(int32_t col, int32_t row, int32_t angle_base, uint32_t angle_width,
                              int32_t vision_dist, void *dead_outptr0, void *dead_outptr1,
                              void *dead_outptr2, void *dead_outptr3); // llm_tact_vision_cone_setup @0x0042e1c9
    void (*fov_raycast_stencil)();                                     // llm_tact_fov_raycast_stencil @0x00436f30
};

const unit_vision_calls &live_unit_vision_calls();

namespace detail {

// llm_tact_fov_probe_far_cell_and_door_state @0x0042e220.
void fov_probe_far_cell_and_door_state(tact_store &own, mh::state::mode_planes &planes,
                                       const unit_vision_calls &c, int32_t unit_idx,
                                       int32_t *out_far_col, uint32_t *out_far_row,
                                       int32_t *out_cell2_col, uint32_t *out_cell2_row,
                                       int32_t *out_door_animating_flag);

// llm_tact_unit_vision_add @0x0042e385.
void unit_vision_add(tact_store &own, mh::state::mode_planes &planes, const unit_vision_calls &c,
                     int32_t unit_idx);

// llm_tact_unit_vision_remove @0x0042e53f.
void unit_vision_remove(tact_store &own, mh::state::mode_planes &planes, const unit_vision_calls &c,
                        int32_t unit_idx);

} // namespace detail

// PUBLIC signature matches the generated sig_llm_tact_fov_probe_far_cell_and_door_state exactly
// (mh_export.gen.h now carries the committed pointee per param, TACT1-P C6 2026-09-04 -- it used to
// render every pointer param `void *`, a generator artifact, not a real ABI/field-level distinction)
// -- same widths as the detail:: function's params above, so this wrapper is a plain forward now.
// Same shape as ai_group_centroid.cpp's out_x/out_y.
void fov_probe_far_cell_and_door_state(int32_t unit_idx, int32_t *out_far_col,
                                       uint32_t *out_far_row, int32_t *out_cell2_col,
                                       uint32_t *out_cell2_row, int32_t *out_door_animating_flag);
void unit_vision_add(int32_t unit_idx);
void unit_vision_remove(int32_t unit_idx);


} // namespace mh::tact
