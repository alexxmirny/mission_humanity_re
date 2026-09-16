//
// tact/tact_dir24_approach.cpp -- see tact_dir24_approach.h. Translated from the DISASSEMBLY, not
// from Ghidra's C.
//
#include "tact/tact_dir24_approach.h"

#include "addr/mh_calls.gen.h"  // llm_tact_calc_dir24 (frontier, stays original per Law 4)
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_calc_dir24.h"
#include "state/mode_planes.h"

namespace mh::tact {
namespace detail {

int32_t calc_approach_dir24_to_tile_stamp(const tact_view &v, int32_t unit_idx, int32_t tile_col,
                                          int32_t tile_row) {
    // @0x00433d76-0x00433d8f: occupant's facing, low 7 bits of the occupancy stamp.
    const uint8_t occ = tile_at(v, tile_col, tile_row).unit[0] & 0x7f;

    // @0x00433d90-0x00433d9d: opposite (12-of-24) sector, wrapped into [0, 24).
    int32_t idx = (int32_t)occ - 1 - 12;
    if (idx < 0) {
        idx += TACT_DIR24_SLOTS;
    }

    // @0x00433da1-0x00433dbc: offset the queried tile by the approach delta.
    const dir24_delta &delta      = v.dir24_approach_delta[idx];
    const int32_t      offset_col = tile_col + delta.dx;
    const int32_t      offset_row = tile_row + delta.dy;

    // @0x00433dbf-0x00433de6: heading from the unit's own position to the offset tile.
    const tact_unit &u = v.units[unit_idx];
    return MH_LIBMH_BIND(llm_tact_calc_dir24)(u.pos_col, u.pos_row, offset_col, offset_row);
}

} // namespace detail

int32_t calc_approach_dir24_to_tile_stamp(int32_t unit_idx, int32_t tile_col, int32_t tile_row) {
    tact_state st = state();
    return detail::calc_approach_dir24_to_tile_stamp(st.read, unit_idx, tile_col, tile_row);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
