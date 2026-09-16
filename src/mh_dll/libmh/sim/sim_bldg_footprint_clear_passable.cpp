//
// sim/sim_bldg_footprint_clear_passable.cpp -- see sim_bldg_footprint_clear_passable.h. Translated
// from the DISASSEMBLY (tmp/decomp_sim/llm_map_bldg_footprint_clear_passable_0049383b.asm), not from
// a Ghidra .c draft (none supplied for this unit).
//
#include "sim/sim_bldg_footprint_clear_passable.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void bldg_footprint_clear_passable(const sim_view &v, sim_store &own, int32_t origin_x,
                                   int32_t origin_y, int32_t building_idx) {
    // 0x00493861-0x004938cb: nested row-major 10x10 walk over
    // cfg_buildings[building_idx].area[row][col] (row outer, col inner -- re-derived from the two
    // independent loop-top/increment blocks, not assumed from any decompiled `for` nesting).
    const cfg_building &b = v.cfg_buildings[building_idx];
    for (int32_t row = 0; row < 10; ++row) {
        for (int32_t col = 0; col < 10; ++col) {
            // 0x00493888-0x0049389f: `area[row][col] != 0` gates the write below; unoccupied cells
            // are skipped entirely (JZ straight past the passable store).
            if (b.area[row][col] == 0) continue;

            // 0x004938a1-0x004938be: tile_x = (origin_x+row) & general.width_mask,
            // tile_y = (origin_y+col) & general.height_mask -- `general` is sim_view::geom
            // (map_geom), reached through the existing map_width_mask()/map_height_mask() helpers;
            // the SAME `general` struct set_passable's sibling body reads, distinct from the
            // separate width_m/height_m pair sim_state.h also binds.
            int32_t tile_x = (origin_x + row) & static_cast<int32_t>(map_width_mask(v));
            int32_t tile_y = (origin_y + col) & static_cast<int32_t>(map_height_mask(v));

            // 0x004938c0: passable[(tile_x<<8)|tile_y] = 0 -- own.passable_at() implements the same
            // packing. This is the ONLY behavioural difference from set_passable: the stored
            // immediate is 0x0 (clear), not 0x1 (set).
            own.passable_at(tile_x, tile_y) = 0;
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void bldg_footprint_clear_passable(int32_t origin_x, int32_t origin_y, int32_t building_idx) {
    sim_state st = state();
    detail::bldg_footprint_clear_passable(st.read, st.own, origin_x, origin_y, building_idx);
}


} // namespace mh::sim
