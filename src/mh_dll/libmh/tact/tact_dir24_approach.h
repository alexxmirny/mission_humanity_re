//
// tact/tact_dir24_approach.h -- TACT1B: the "approach" tile / heading helper.
//
//   llm_tact_calc_approach_dir24_to_tile_stamp @0x00433d57 (0xa3)
//
// A PURE QUERY: reads the tile at (tile_col, tile_row)'s occupancy stamp, the tile's occupant's
// facing (encoded in the stamp's low 7 bits), offsets the tile by roughly the OPPOSITE of that
// facing via a fixed 24-entry delta table, then returns the dir24 heading from `unit_idx`'s own
// position to that offset tile (llm_tact_calc_dir24, a frontier callee). No global is written --
// `shadow_region_closure.py` reports 0 direct / 0 transitive write cells, so the write-set preflight
// correctly flags it NOT SHADOWABLE by region diffing. It IS shadowable by RETURN VALUE alone
// (gen_dll_shadow.py's `compare_return`, default on for a non-void function): the return is a real
// computed heading, not a value predetermined by an ini flag, so a return-only comparison is
// non-vacuous evidence (contrast the net_lockstep_sync_delay_stub precedent in this manifest, where
// the return was always 0/uninitialised by construction).
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_calc_approach_dir24_to_tile_stamp @0x00433d57.
//
// 1. @0x00433d76-0x00433d8f: read tile_objects[tile_col][tile_row].unit[0] (the occupancy stamp;
//    mode_planes.h's (x<<8)|y indexing, x=tile_col), mask off the top "unit here" bit (0x7f), and
//    subtract 1 -- the low 7 bits are the OCCUPANT's own dir24 facing (1..0x18), 0 = no occupant.
// 2. @0x00433d90-0x00433d9d: idx = occ - 1 - 12, wrapped by +24 if negative -- the OPPOSITE sector
//    (12 of 24 = 180 degrees) of the occupant's facing. Signed int32 arithmetic throughout; when
//    occ==0 (no occupant) idx = -13 -> wraps to 11, a value that is read but not otherwise guarded
//    -- preserved literally, not treated as an error case.
// 3. @0x00433da1-0x00433dbc: offset (tile_col, tile_row) by TACT_DIR24_APPROACH_DELTA[idx].{dx,dy}.
// 4. @0x00433bfc-0x00433de6 [sic 0x00433dbf-0x00433de6]: return
//    llm_tact_calc_dir24(unit.pos_col, unit.pos_row, offset_col, offset_row) -- the heading FROM the
//    calling unit's own position TO the computed approach tile.
int32_t calc_approach_dir24_to_tile_stamp(const tact_view &v, int32_t unit_idx, int32_t tile_col,
                                          int32_t tile_row);

} // namespace detail

int32_t calc_approach_dir24_to_tile_stamp(int32_t unit_idx, int32_t tile_col, int32_t tile_row);


} // namespace mh::tact
