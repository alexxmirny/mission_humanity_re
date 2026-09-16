//
// sim/sim_bldg_get_coords.cpp -- see sim_bldg_get_coords.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_get_coords_00449b8a.asm), not from the Ghidra .c draft (which reads
// close to the asm here, modulo the field-width care documented in the header).
//
#include "sim/sim_bldg_get_coords.h"


namespace mh::sim {

namespace detail {

void bldg_get_coords(const sim_view &v, uint16_t player, int32_t building_index, int32_t *out_x, int32_t *out_y) {
    // 0x00449bab-0x00449c1d / 0x00449bd7-0x00449c49: the record is re-read from scratch for each of
    // the four field fetches in the original (player/building_index re-narrowed, the row/slot address
    // re-derived by IMUL every time) -- purely redundant address arithmetic with no intervening write
    // anywhere in this straight-line body, so a single reference here is value-identical.
    const building &b = building_of(v, player, building_index);

    // 0x00449bbe / 0x00449c16: buildings[player][building_index].building_id, MOVZX WORD -- uint16_t,
    // matches mh_map_object_building::building_id exactly (no widening beyond the field's own type).
    const cfg_building &cb = v.cfg_buildings[b.building_id];

    // 0x00449bcb: Building[building_id].width, MOVZX BYTE (uint8_t) -- NOT the wider read the Ghidra
    // .c draft's cast implied; matches mh_cfg_final_struct_Building::width exactly (translator brief
    // rule 7).
    const uint32_t width16 = static_cast<uint32_t>(cb.width) << 4; // 0x00449bcb/0x00449bd2
    // 0x00449c23: Building[building_id].height, MOVZX BYTE -- one byte before width in the record
    // (confirmed against addr/mh_structs.gen.h: height @+0x9, width @+0xa), same zero-extend width.
    const uint32_t height16 = static_cast<uint32_t>(cb.height) << 4; // 0x00449c23/0x00449c2a

    // 0x00449bea/0x00449bf1: buildings[...].x, MOVZX BYTE -- uint8_t, matches the struct field.
    const uint32_t x32 = static_cast<uint32_t>(b.x) << 5; // 0x00449bea/0x00449bf1
    // 0x00449c42/0x00449c49: buildings[...].y, MOVZX BYTE -- uint8_t, matches the struct field.
    const uint32_t y32 = static_cast<uint32_t>(b.y) << 5; // 0x00449c42/0x00449c49

    // 0x00449bf6/0x00449bff: general.bw_mask (offset 0 of llm_strat_map_geom) -- the PIXEL-space wrap
    // mask, NOT the tile-space width_mask (offset 8) map_width_mask() reads. No existing helper for
    // this pair; read v.geom->bw_mask directly per the batch context.
    *out_x = static_cast<int32_t>((x32 + width16) & v.geom->bw_mask);
    // 0x00449c4e/0x00449c56: general.bh_mask (offset 4), the pixel-space Y wrap mask.
    *out_y = static_cast<int32_t>((y32 + height16) & v.geom->bh_mask);
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void bldg_get_coords(uint16_t player, int32_t building_index, int32_t *out_x, int32_t *out_y) {
    const sim_view v = state().read;
    detail::bldg_get_coords(v, player, building_index, out_x, out_y);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
