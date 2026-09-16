//
// ai/ai_notify_map_changed.cpp -- see ai_notify_map_changed.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_notify_map_changed_004d82ca.asm and
// tmp/decomp/llm_strat_ai_notify_map_changed_2_004d83c7.asm), not from Ghidra's .c: both decompiles
// are legible and check out instruction-by-instruction, but the EXPORTED PLATE for the SECOND
// function claims it is "byte-for-byte identical" to the first -- it is not (the seed-selection logic
// differs: 7/5/4 for the first, 0/2 for the second, matching notify_object_removed's shape instead).
// That claim was not trusted; both bodies below are read straight off their own bytes.
//
#include "ai/ai_notify_map_changed.h"


namespace mh::ai {
namespace detail {

namespace {

// The six cfg Building types both functions test, in the CMP sequence's own order
// (0x19,0x5,0x16,0x2,0x23,0xf = H_TURRET,A_TURRET,H_MINE,A_MINE,H_RELAY,A_RELAY). Same six as
// is_structural_building_type in ai_notify_removed.cpp; duplicated here per the translator brief's
// "no new shared helpers" rule rather than factored into ai_state.h.
bool is_structural_building_type(uint8_t type) {
    return type == BLDG_TYPE_H_TURRET || type == BLDG_TYPE_A_TURRET || type == BLDG_TYPE_H_MINE ||
           type == BLDG_TYPE_A_MINE || type == BLDG_TYPE_H_RELAY || type == BLDG_TYPE_A_RELAY;
}

} // namespace

void notify_map_changed(const ai_view &v, const ai_store &own, const ai_calls &gc,
                        int32_t builder_player, int32_t building_type, int32_t tile_x,
                        int32_t tile_y) {
    for (uint32_t p = 0; p < (uint32_t)*v.active_player_count; ++p) {
        player_data &pd = own.players[p];
        if (v.players[p].ai_enabled == 0) continue;

        pd.ai_map_changed_pending = 1;

        int32_t seed;
        if ((int32_t)p == builder_player) {
            seed = is_structural_building_type(v.cfg_buildings[building_type].type) ? 5 : 4;
        } else {
            seed = 7;
        }

        uint8_t *stencil = const_cast<uint8_t *>(&v.cfg_buildings[building_type].area[0][0]);
        gc.grid_stamp_seeds(pd.ai_tile_flags_grid, *v.map_width, *v.map_height, stencil,
                            FOOTPRINT_SPAN, FOOTPRINT_SPAN, tile_x, tile_y, seed);
    }
}

void notify_map_changed_2(const ai_view &v, const ai_store &own, const ai_calls &gc,
                          int32_t builder_player, int32_t building_type, int32_t tile_x,
                          int32_t tile_y) {
    for (uint32_t p = 0; p < (uint32_t)*v.active_player_count; ++p) {
        player_data &pd = own.players[p];
        if (v.players[p].ai_enabled == 0) continue;

        pd.ai_map_changed_pending = 1;

        const int32_t seed = ((int32_t)p == builder_player &&
                              !is_structural_building_type(v.cfg_buildings[building_type].type))
                                 ? 2
                                 : 0;

        uint8_t *stencil = const_cast<uint8_t *>(&v.cfg_buildings[building_type].area[0][0]);
        gc.grid_stamp_seeds(pd.ai_tile_flags_grid, *v.map_width, *v.map_height, stencil,
                            FOOTPRINT_SPAN, FOOTPRINT_SPAN, tile_x, tile_y, seed);
    }
}

} // namespace detail

void notify_map_changed(int32_t builder_player, int32_t building_type, int32_t tile_x,
                        int32_t tile_y) {
    const ai_state st = state();
    detail::notify_map_changed(st.read, st.own, live_calls(), builder_player, building_type, tile_x,
                               tile_y);
}

void notify_map_changed_2(int32_t builder_player, int32_t building_type, int32_t tile_x,
                          int32_t tile_y) {
    const ai_state st = state();
    detail::notify_map_changed_2(st.read, st.own, live_calls(), builder_player, building_type, tile_x,
                                 tile_y);
}

// ---- the differential-oracle arms ---------------------------------------------------------------
//
// Both REAL. grid_stamp_seeds is the only callee, and its own closure (the caller-declared grid --
// inside player_data -- plus own.grid_wrap_mask) is exactly what these sites declare; stubbing it
// would guarantee a divergence (the original re-stamps player_data's influence grid, ours would
// leave it untouched) rather than avoid one. Both functions' only direct write is
// ai_map_changed_pending, inside player_data, the same region.

} // namespace mh::ai
