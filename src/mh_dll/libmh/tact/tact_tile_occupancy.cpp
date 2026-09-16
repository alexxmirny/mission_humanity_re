//
// tact/tact_tile_occupancy.cpp -- see tact_tile_occupancy.h. Translated from the DISASSEMBLY, not
// from Ghidra's C.
//
#include "tact/tact_tile_occupancy.h"

#include "state/mode_planes.h"

namespace mh::tact {
namespace detail {

void tile_rebuild_occupancy_layer(const tact_view &v, tact_store &own) {
    const int32_t width  = *v.map_width;
    const int32_t height = *v.map_height;

    // @0x00433e07-0x00433e1f: clear the whole active sub-block.
    for (int32_t col = 0; col < width; ++col) {
        for (int32_t row = 0; row < height; ++row) {
            mh::state::tile_occupancy(own.planes(), col, row) = 0;
        }
    }

    // @0x00433e26-0x00433e53: stamp every active FX-pool entry owned by the reload map. See the
    // header's derivation of why these three bytes are travel_dx's raw bytes, not a designed field.
    const auto map_id_byte = (uint8_t)own.occupancy_rebuild_map_id();
    for (int32_t i = 0; i < TACT_FX_POOL_SLOTS; ++i) {
        const fx_entry &fx = v.fx_pool[i];
        if (fx.fx_type == 0) {
            continue;
        }
        if (fx.owner != map_id_byte) {
            continue;
        }
        const auto   *raw                                 = reinterpret_cast<const uint8_t *>(&fx.travel_dx);
        const int32_t col                                 = raw[0];
        const int32_t row                                 = raw[1];
        const uint8_t stamp                               = raw[2];
        mh::state::tile_occupancy(own.planes(), col, row) = stamp;
    }

    // @0x00433e5a-0x00433e90: OR in the "unit here" flag for the FIRST 64 unit slots only (not all
    // 129, not 1-based) belonging to the reload map and not dying/dead.
    for (int32_t i = 0; i < TACT_OCCUPANCY_UNIT_SCAN_SLOTS; ++i) {
        const tact_unit &u = v.units[i];
        if (u.type == 0 || u.type > 0x80) {
            continue;
        }
        if (u.owner != map_id_byte) {
            continue;
        }
        if (u.anim_state == 0x1f) {
            continue;
        }
        mh::state::tile_occupancy(own.planes(), u.pos_col, u.pos_row) |= 0x80;
    }
}

void tile_rebuild_occupancy_layer_for_map(const tact_view &v, tact_store &own, int32_t map_id) {
    own.occupancy_rebuild_map_id() = map_id; // @0x0043358c
    tile_rebuild_occupancy_layer(v, own);    // @0x00433591
}

} // namespace detail

void tile_rebuild_occupancy_layer() {
    tact_state st = state();
    detail::tile_rebuild_occupancy_layer(st.read, st.own);
}

void tile_rebuild_occupancy_layer_for_map(int32_t map_id) {
    tact_state st = state();
    detail::tile_rebuild_occupancy_layer_for_map(st.read, st.own, map_id);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
