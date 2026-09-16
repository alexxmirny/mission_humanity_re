#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The logic over an EXPLICIT view/store, so `net_selftest.exe simtest` can drive it over heap
// buffers with no game and no rig. The wrapper below is this applied to state(); the split costs
// one inlined call. No `_calls` table -- this function makes no outward calls (matching
// sim_unit_unlink_tile.h's identical posture and rationale).
namespace detail {

// map_unit_PutOnMap @0x00486c6a. See the header derivation above for the empty/new-base/walk-
// and-splice three-case insertion. `b_id` keeps the committed (misleading) parameter name -- see
// the NAMING TRAP note above.
void unit_put_on_map(const sim_view &v, sim_store &own, uint16_t player, uint16_t b_id, uint8_t x,
                     uint8_t y);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the original's committed
// __watcall(AX,DX,BL,CL) shape (sig_map_unit_PutOnMap).
void unit_put_on_map(uint16_t player, uint16_t b_id, uint8_t x, uint8_t y);

namespace detail {
} // namespace detail

} // namespace mh::sim
