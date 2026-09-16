#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The logic over an EXPLICIT view/store, so `net_selftest.exe simtest` can drive it over heap buffers
// with no game and no rig. The wrapper below is this applied to state(); the split costs one inlined
// call. No `_calls` table (see the header banner above -- this function makes no outward calls).
namespace detail {

// llm_strat_unit_unlink_tile @0x00486f41. See the header derivation above for the head-vs-mid-chain
// splice and the unconditional trailing unit_above clear.
void unit_unlink_tile(const sim_view &v, sim_store &own, uint32_t unit_player, uint16_t unit_index);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the original's committed __watcall(EAX,DX)
// shape (sig_llm_strat_unit_unlink_tile).
void unit_unlink_tile(uint32_t unit_player, uint16_t unit_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
