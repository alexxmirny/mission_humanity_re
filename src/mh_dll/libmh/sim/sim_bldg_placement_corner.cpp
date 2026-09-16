//
// sim/sim_bldg_placement_corner.cpp -- see sim_bldg_placement_corner.h. Translated from the
// DISASSEMBLY
// (tmp/decomp_sim/llm_bldg_calc_placement_corner_from_center_by_type_0048d0ea.asm), not from any
// Ghidra .c draft (none was supplied for this unit).
//
#include "sim/sim_bldg_placement_corner.h"


namespace mh::sim {

namespace detail {

void bldg_calc_placement_corner_from_center_by_type(const sim_view &v, uint16_t building_type,
                                                    int32_t center_x, int32_t center_y,
                                                    uint32_t *out_x, uint32_t *out_y) {
    const cfg_building &cb = v.cfg_buildings[building_type];

    // 0x0048d10b-0x0048d124: half_w = (int32_t)(cb.width - 1) / 2, truncating toward zero -- see the
    // header's "THE HALVING IDIOM" note for why plain C `/2` reproduces the DEC/SAR/SUB/SAR bit
    // pattern value-for-value, including on the width==0 (-1/2 == 0 both ways) edge.
    const int32_t half_w = static_cast<int32_t>(cb.width - 1) / 2;
    // 0x0048d136-0x0048d14f: half_h = (int32_t)(cb.height - 1) / 2, same idiom.
    const int32_t half_h = static_cast<int32_t>(cb.height - 1) / 2;

    // 0x0048d126/0x0048d132/0x0048d134: *out_x = (center_x + half_w) & general.width_mask.
    // ADD, not SUB -- see the header's "ADD, NOT SUB" note: this is a real, confirmed difference from
    // the sibling llm_bldg_calc_placement_corner_from_center, not a transcription slip.
    *out_x = static_cast<uint32_t>(center_x + half_w) & map_width_mask(v);
    // 0x0048d154/0x0048d15e/0x0048d160: *out_y = (center_y + half_h) & general.height_mask.
    *out_y = static_cast<uint32_t>(center_y + half_h) & map_height_mask(v);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void bldg_calc_placement_corner_from_center_by_type(uint16_t building_type, int32_t center_x,
                                                    int32_t center_y, uint32_t *out_x, uint32_t *out_y) {
    const sim_view v = state().read;
    detail::bldg_calc_placement_corner_from_center_by_type(v, building_type, center_x, center_y, out_x,
                                                           out_y);
}


} // namespace mh::sim
