//
// tact/tact_move_path_preview_walk.h -- TACT1B slice: walk a stored move-path preview, stamping the
// overlay plane along the way.
//
//   llm_tact_move_path_preview_walk @0x0042eaf9 (0x25d)
//
// Reads the RLE-encoded route stored at path_slot_id (owner ALWAYS 0 -- the original hardcodes it,
// preserved literally, see the body), walks it step by step from (start_col, start_row), and stops
// either when the route runs out (heading == 0) or when the tile just stepped onto is not currently
// visible (tile_objects.flags[1] & 0x80 clear -- the same bit sim/sim_map_fog_of_war_recompute.cpp
// sets/clears for "currently visible"; not re-verified for the tactical mode specifically, but
// `mh::state::mode_planes`'s own banner names `unit[]`, not `flags[]`, as the field with two
// mode-dependent readings, so this one is assumed single-meaning). Writes *out_col/*out_row to
// wherever the walk stopped.
//
// EVERY VISITED TILE'S OVERLAY BYTE (tile_object::unit[1], `tile_overlay()`) IS STAMPED, in two
// different ways at two different points of each step -- see the .cpp for the exact bit arithmetic,
// which is preserved instruction-for-instruction rather than rationalised.
//
#pragma once
#include <cstdint>

#include "state/mode_planes.h"
#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_move_path_preview_walk @0x0042eaf9.
//
// 1. @0x0042eb34-0x0042eb42: clear the starting tile's overlay byte (tile_overlay = 0) before
//    tracing anything.
// 2. @0x0042eb49-0x0042eb67: outer loop over RLE entries at path_waypoint_at(0, path_slot_id,
//    entry_idx) -- OWNER IS THE LITERAL 0, not a parameter; the original never varies it
//    (0x0042eb1a inits the local to 0 and nothing in the body writes it). heading == 0 ends the
//    walk: write out (cur_col, cur_row) and return.
// 3. Per entry, inner loop for run_length steps (@0x0042ebb5-0x0042ed33):
//    a. @0x0042ebc5-0x0042ec03: stamp the CURRENT tile's overlay: high nibble := (heading/3)+1
//       (truncating signed divide), low nibble preserved from the existing byte.
//    b. @0x0042ec09-0x0042ecc8: apply the direction cascade to (cur_col, cur_row) -- one of 8
//       headings {1,4,7,0xa,0xd,0x10,0x13,0x16} moves; anything else is a no-op. See the .cpp
//       switch for the exact deltas.
//    c. @0x0042ecc8-0x0042ecdd: if the NEW tile's flags[1] bit 0x80 is CLEAR, write out (cur_col,
//       cur_row) and return immediately -- the walk stops without the step (d) below.
//    d. @0x0042ecf1-0x0042ed2d: otherwise OR (heading/3)+1 (unshifted this time) into the NEW
//       tile's overlay byte, decrement the remaining run length, and loop.
void move_path_preview_walk(mh::state::mode_planes &planes, int32_t start_col, int32_t start_row,
                            int32_t path_slot_id, int32_t *out_col, int32_t *out_row);

} // namespace detail

// Public wrapper. Signature matches the committed prototype in addr/mh_calls.gen.h (the out params
// carry the committed pointee, `int *` -> `int32_t *`, since TACT1-P C6, 2026-09-04).
void move_path_preview_walk(int32_t start_col, int32_t start_row, int32_t path_slot_id,
                            int32_t *out_col, int32_t *out_row);


} // namespace mh::tact
