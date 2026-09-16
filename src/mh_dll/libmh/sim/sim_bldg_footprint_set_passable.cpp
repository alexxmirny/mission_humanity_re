//
// sim/sim_bldg_footprint_set_passable.cpp -- see sim_bldg_footprint_set_passable.h. Translated from
// the DISASSEMBLY (tmp/decomp/llm_map_bldg_footprint_set_passable_004938d3.asm), not from the
// Ghidra .c draft.
//
#include "sim/sim_bldg_footprint_set_passable.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void bldg_footprint_set_passable(const sim_view &v, sim_store &own, int32_t origin_x,
                                 int32_t origin_y, int32_t building_idx) {
    // 0x004938f2-0x00493961: nested row-major 10x10 walk over
    // cfg_buildings[building_idx].area[row][col] (row outer, col inner -- re-derived from the two
    // independent loop-top/increment blocks, not assumed from the .c's `for` nesting).
    const cfg_building &b = v.cfg_buildings[building_idx];
    for (int32_t row = 0; row < 10; ++row) {
        for (int32_t col = 0; col < 10; ++col) {
            // 0x00493920-0x00493937: `area[row][col] != 0` gates the write below; unoccupied cells
            // are skipped entirely (JZ straight past the passable store).
            if (b.area[row][col] == 0) continue;

            // 0x00493939-0x00493958: tile_x = (origin_x+row) & general.width_mask,
            // tile_y = (origin_y+col) & general.height_mask -- `general` is sim_view::geom
            // (map_geom), reached through the existing map_width_mask()/map_height_mask() helpers;
            // this is the SAME `general` struct the header banner cross-checks by address, and is
            // distinct from the separate width_m/height_m pair sim_state.h also binds.
            int32_t tile_x = (origin_x + row) & static_cast<int32_t>(map_width_mask(v));
            int32_t tile_y = (origin_y + col) & static_cast<int32_t>(map_height_mask(v));

            // 0x00493958: passable[(tile_x<<8)|tile_y] = 1 -- own.passable_at() implements the same
            // packing.
            own.passable_at(tile_x, tile_y) = 1;
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void bldg_footprint_set_passable(int32_t origin_x, int32_t origin_y, int32_t building_idx) {
    sim_state st = state();
    detail::bldg_footprint_set_passable(st.read, st.own, origin_x, origin_y, building_idx);
}


} // namespace mh::sim
