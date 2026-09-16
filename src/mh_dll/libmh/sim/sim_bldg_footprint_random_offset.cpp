//
// sim/sim_bldg_footprint_random_offset.cpp -- see sim_bldg_footprint_random_offset.h. Translated from
// the DISASSEMBLY (tmp/decomp/llm_strat_bldg_footprint_random_offset_00449c61.asm), not from the
// Ghidra .c draft (the draft's arithmetic agrees, but its field access reads `Building[...].width` /
// `(byte)Building[...].height` without stating which asm displacement is which -- the derivation below
// is worked from the raw offsets).
//
#include "sim/sim_bldg_footprint_random_offset.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const footprint_random_offset_calls &live_footprint_random_offset_calls() {
    static const footprint_random_offset_calls c = {
        MH_LIBMH_BIND(llm_rand_below),
    };
    return c;
}

namespace detail {

void bldg_footprint_random_offset(const sim_view &v, const footprint_random_offset_calls &c, uint32_t /*player*/,
                                  uint32_t /*unit_index*/, int32_t target_player, int32_t target_bldg_idx,
                                  uint32_t *out_fine_x, uint32_t *out_fine_y) {
    // player/unit_index (param_1/param_2) are dead register carriers -- see the header's derivation.
    // Not read here.

    const building     &b  = building_of(v, (uint32_t)target_player, target_bldg_idx);
    const cfg_building &cb = v.cfg_buildings[b.building_id];

    // width/height read at their declared BYTE width (MOVZX-from-byte, 0x00449c9f / 0x00449cc9), then
    // SHL 4 (*0x10): a footprint cell is 16 pixels, so this converts the tile-footprint dimension to a
    // pixel span. FIELD-OFFSET DERIVATION (the asm's two displacements are 0xd9ec8a and 0xd9ec89, one
    // byte apart, and cfg_building's own layout has height at offset 0x9 and width at offset 0xa --
    // static_assert'd in addr/mh_structs.gen.h -- so the LARGER displacement (0x...8a, read first,
    // feeding the value that ends up combined with param_5/bw_mask) is WIDTH, and the smaller
    // (0x...89, read second, feeding param_6/bh_mask) is HEIGHT): width -> X axis, height -> Y axis,
    // the same pairing every other footprint helper in this closure uses.
    const uint32_t width_px  = (uint32_t)cb.width << 4;
    const uint32_t height_px = (uint32_t)cb.height << 4;

    const int32_t rand_x = c.rand_below((int32_t)width_px);
    const int32_t rand_y = c.rand_below((int32_t)height_px);

    // half = span/2 via the truncating-signed-divide-by-2 idiom (SAR EDX,0x1f / SUB / SAR EAX,1 --
    // 0x00449ce0-0x00449ceb for X, 0x00449cfc-0x00449d07 for Y), reproduced idiom-for-idiom rather than
    // assumed -- same derivation sim_bldg_placement_preview.cpp's bldg_calc_placement_corner_from_
    // center gives for its own instance of this idiom. width_px/height_px are always non-negative
    // (byte * 16), so the idiom is equivalent to plain truncating division here, but the C++ expresses
    // the same operation the asm performs rather than a simplification of it.
    const int32_t half_w = (int32_t)width_px / 2;
    const int32_t half_h = (int32_t)height_px / 2;

    // IN/OUT: *out_fine_x/*out_fine_y are the CALLER's existing fine position (0x00449d0e-0x00449d36).
    // ADD then AND, in that order -- `(*out + (rand - half)) & mask`, not `*out + ((rand - half) & mask)`
    // -- and the delta is added via plain 32-bit ADD, so a negative (rand - half) wraps exactly like
    // the asm's bit pattern; the (uint32_t) cast reproduces that wrap value-for-value.
    //
    // v.geom->bw_mask / v.geom->bh_mask, NOT map_width_mask(v)/map_height_mask(v): the asm reads raw
    // addresses 0x00e15390/0x00e15394 directly (0x00449d16 comment: "general"; 0x00449d2b comment:
    // "general.bh_mask"), i.e. map_geom's FIRST TWO fields (bw_mask @+0x0, bh_mask @+0x4) -- the
    // PIXEL-space wrap pair, distinct from width_mask/height_mask (the TILE-space pair further into
    // the same struct) that map_width_mask()/map_height_mask() read. See sim_state.h's `map_geom`
    // comment for the full field-family disambiguation.
    *out_fine_x = (*out_fine_x + (uint32_t)(rand_x - half_w)) & v.geom->bw_mask;
    *out_fine_y = (*out_fine_y + (uint32_t)(rand_y - half_h)) & v.geom->bh_mask;
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void bldg_footprint_random_offset(uint32_t param_1, uint32_t param_2, int32_t a2, int32_t param_4,
                                  uint32_t *param_5, uint32_t *param_6) {
    const sim_view v = state().read;
    detail::bldg_footprint_random_offset(v, live_footprint_random_offset_calls(), param_1, param_2, a2, param_4,
                                         param_5, param_6);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
