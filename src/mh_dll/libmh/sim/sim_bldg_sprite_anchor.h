//
// sim/sim_bldg_sprite_anchor.h -- two sprite-mount-point pixel-coordinate helpers (RI-SIM / SIM1-G4):
// "the bounding-box-centering pixel offset for a building TYPE's default sprite" and
// "the world-pixel coordinate of one of a building INSTANCE's two weapon-mount points".
//
//   llm_strat_bldg_sprite_anchor_offset @0x00450a60 (0xe0 B), `void __mh_watcall_ebx_volatile
//   llm_strat_bldg_sprite_anchor_offset(ushort building_id, int *out_x, int *out_y)` -- committed
//   prototype.
//   llm_strat_bldg_get_sprite_anchor_coord @0x00490eb1 (0x1d5 B), `uint
//   __mh_watcall_ecx_ebx_volatile llm_strat_bldg_get_sprite_anchor_coord(uint player, int b_index,
//   int anchor_kind, byte axis)` -- committed prototype, FIXED this slice (was missing the 4th param
//   `axis`, arriving in CL; see tools/data/ghidra_findings.json 2026-08-21-2327-era entries and the
//   fifth-slice session report for that correction's own history -- this file is the first
//   translation to actually consume the corrected signature).
//
// Translated from the DISASSEMBLY (tmp/decomp_sim/llm_strat_bldg_sprite_anchor_offset_00450a60.asm,
// _get_sprite_anchor_coord_00490eb1.asm), re-exported this slice after the axis-param fix landed (the
// fifth/'s exports were stale). No Ghidra .c draft consulted.
//
// ---- llm_strat_bldg_sprite_anchor_offset: PER-BUILDING-TYPE (not instance), PURE, WRITES ONLY
//      THROUGH THE CALLER'S TWO OUT-POINTERS -------------------------------------------------------
// 0x00450a76-0x00450aa7: `frame = Anim[ Building[building_id].anim[0] + 1 ].sprite_id` (the CFG
// table's own default anim-chain entry for this building TYPE, a plain dword read -- NOT the runtime
// per-instance `buildings[player][index].anim[0]` get_sprite_anchor_coord's first (dead) computation
// below reads). `header = *gfx_bank_pixels + sprite_pix_offsets[frame]`, a `gfx_sprite_hdr*`
// (the sprite-format notes: x_left@4/y_top@6/width@8/height@0xA; the DOC's generic "short" is refined
// here -- this function's own consumption is unambiguously `MOVZX word` at all four offsets, so all
// four are read as UNSIGNED here, unlike get_sprite_anchor_coord's sprite_meta.mount*_x/y reads below,
// which use `MOVSX`). Then, matching the doc's
// already-derived "canvas centering" formula field-for-field:
//   *out_x = ((Building[bid].width  * 32 - header.width ) / 2, ROUND-TOWARD-ZERO) - header.x_left
//   *out_y = ((Building[bid].height * 32 - header.height) / 2, ROUND-TOWARD-ZERO) - header.y_top
// `width`@0xa/`height`@0x9 are ALREADY-NAMED cfg_final_struct_Building fields (0xd9ec8a/0xd9ec89 in
// the disassembly, confirmed against a live get-structure-info dump this slice -- no declared need).
// The round-toward-zero /2 is the x87 idiom `EDX=SAR(val,31); (val-EDX)>>1` --
// `divide_round_toward_zero()` below.
// ZERO writes to any tracked region (writes only through the caller's own stack out-params) and
// returns void -- genuinely NOT SHADOWABLE (no region, no return value to compare); backed by an
// offline oracle only (see the translation report).
//
// ---- llm_strat_bldg_get_sprite_anchor_coord: PER-BUILDING-INSTANCE, PURE QUERY, RETURNS uint -------
// 0x00490ee5-0x00490ef4: computes `Anim[ buildings[player][b_index].anim[0] + 1 ].sprite_id` and
// stores it to a local -- THIS RESULT IS PROVABLY DEAD. The ONLY read of that local
// (`IMUL EAX,[local],0x18` at 0x00490fa1, the sprite_meta-stride multiply) executes strictly after
// 0x00490f6c, which UNCONDITIONALLY overwrites the same local with a SECOND, different computation
// before any branch on anchor_kind/axis. Confirmed by linear address order (0x00490ee5 < 0x00490f6c <
// 0x00490fa1) and re-confirmed there is no jump target landing between the first store and the second
// that could reach 0x00490fa1 skipping the overwrite. A pure read of static cfg/Anim data with no
// side effect, so omitting it changes nothing observable -- see sim_bldg_free_record.cpp's own
// "trailing dead-code block" precedent for the same treatment. NOT reproduced in this translation.
// 0x00490f36-0x00490f6c (the SECOND, live computation): `frame2 = Anim[ buildings[player][b_index]
// .anim[ cfg_buildings[buildings[player][b_index].building_id].sprite_quantity - 1 ] + 1 ].sprite_id`.
// `sprite_quantity` is a per-building-TYPE cfg byte (`cfg_building::sprite_quantity`); indexing
// `anim[sprite_quantity - 1]` reads ONE ELEMENT BEFORE `anim[0]` (into the tail of the preceding
// `anim_dur[12]` array, still within the `building` record) whenever `sprite_quantity == 0` for a
// building type -- a genuine PRESERVE-BUG shape (Law 2), implemented here via raw byte-pointer
// arithmetic on the record (never `.anim[-1]` array indexing) so the same in-struct underflow read
// happens rather than undefined behaviour.
// Then, by (anchor_kind, axis):
//   anchor_kind==1, axis==1: (buildings[..].x*32 + sprite_anchor_offset's out_x + sprite_meta[frame2]
//                             .mount1_x) & geom->bw_mask
//   anchor_kind==1, axis!=1: (buildings[..].y*32 + out_y + sprite_meta[frame2].mount1_y) & bh_mask
//   anchor_kind!=1, axis==1: (buildings[..].x*32 + out_x + sprite_meta[frame2].mount2_x) & bw_mask
//   anchor_kind!=1, axis!=1: (buildings[..].y*32 + out_y + sprite_meta[frame2].mount2_y) & bh_mask
// `buildings[..].x`/`.y` are the building's own tile position (map_object_building::x/y, byte);
// `bw_mask`/`bh_mask` are geom's PIXEL-space wrap masks (offsets 0/4 -- NOT `width_mask`/`height_mask`
// at 8/32, which are the TILE-space masks; confirmed against the live `llm_strat_map_geom` field dump,
// this function ANDs the pixel-space pair). ZERO writes to any tracked region and a non-void (`uint`)
// return -- shadowable by RETURN-VALUE COMPARISON ALONE (compare_return), same posture as
// sim_hangar_energy.cpp's `hangar_any_unit_needs_energy`; declared here as an extra_region-free arm
// that calls sprite_anchor_offset's DETAIL function directly (not through the shadow/mh::call layer --
// sprite_anchor_offset is translated in THIS SAME FILE, an ordinary intra-TU call).
//
// ---- FIELDS: NAMED / BOUND THIS SLICE, NOT BYTE OFFSETS ---------------------------------------------
// gfx_sprite_hdr's 4 fields (x_left/y_top/width/height @4/6/8/0xA) come from the sprite-format notes'
// already-resolved (but not yet Ghidra-DTM-applied) struct -- see the translation report's declared
// finding on that gap; this file reads them via named byte-offset constants, not a Ghidra type, since
// applying a Ghidra structure to a runtime-loaded blob location has no fixed address to apply it AT.
// mh_llm_strat_map_geom.bw_mask@0x0 / .bh_mask@0x4 (double-checked against the live structure dump,
// NOT the width_mask/height_mask pair at 0x8/0x20). gfx_sprite_meta.mount1_x/_y@8/10,
// mount2_x/_y@12/14 (already Ghidra-named). cfg_final_struct_Building.type@8 (register_online's own
// concern, not this file's), .anim[0]@0x14f, .sprite_quantity@0x183 -- all pre-existing Ghidra names,
// confirmed against a live get-structure-info dump this slice, not re-derived from the extent alone.
// map_object_building.x@0xc3/.y@0xc4/.anim[12]@0x93 (cfg_t_frame_index, 4B/entry, represented as
// uint8_t anim[48] in the generated header per this codebase's existing load_u32_le/store_u32_le
// convention -- sim_bldg_liftoff_anim.cpp / sim_map_create_building.cpp precedent).
//
// ---- STATE ALREADY BOUND (sim_state.h) --------------------------------------------------------------
// sim_view::cfg_buildings, ::buildings (via building_of), ::sprite_meta, ::anim_frames, ::geom.
// sim_view::sprite_pix_offsets / ::gfx_bank_pixels -- NEW THIS SLICE (the declared need that reverted
// this pair's translation last slice): _G_LLM_SPRITE_PIX_OFFSETS retyped undefined4 -> uint32_t[22000]
// in Ghidra (was blocking R4b readiness; see ghidra_findings.json 2026-08-22-1157-1), both wired into
// dll_addr_manifest.json / sim_state.h+.cpp / the offline-test fixture.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim::detail {

// llm_strat_bldg_sprite_anchor_offset @0x00450a60. Per-building-TYPE (not instance). See the header
// banner above for the full derivation.
void sprite_anchor_offset(const sim_view &v, uint16_t building_id, int32_t *out_x, int32_t *out_y);

// llm_strat_bldg_get_sprite_anchor_coord @0x00490eb1. Pure query -- see the header banner above.
uint32_t bldg_get_sprite_anchor_coord(const sim_view &v, uint32_t player, int32_t b_index,
                                      int32_t anchor_kind, uint8_t axis);

} // namespace mh::sim::detail

namespace mh::sim {

// Live wrappers: the logic applied to state().read. Match the committed prototypes
// (sig_llm_strat_bldg_sprite_anchor_offset / sig_llm_strat_bldg_get_sprite_anchor_coord) exactly.
void     bldg_sprite_anchor_offset(uint16_t building_id, int32_t *out_x, int32_t *out_y);
uint32_t bldg_get_sprite_anchor_coord(uint32_t player, int32_t b_index, int32_t anchor_kind,
                                      uint8_t axis);

namespace detail {
} // namespace detail

} // namespace mh::sim
