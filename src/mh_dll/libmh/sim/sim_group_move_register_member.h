#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The logic over an EXPLICIT state, so `net_selftest.exe simtest` can drive it over heap buffers with
// no game and no rig. The wrapper below is this applied to state().
namespace detail {

// llm_strat_group_move_register_member @0x0048d16b. See the header derivation above for the full
// shape. `scratch_count` is IN/OUT: read once as the write index, then incremented by one.
void group_move_register_member(const sim_view &v, sim_store &own, int32_t player, int32_t unit_idx,
                                int32_t *scratch_count);

} // namespace detail

// Live wrapper. Signature matches the committed export/call/shadow shape
// (sig_llm_strat_group_move_register_member: void(int32_t player, int32_t unit_idx, int32_t
// *scratch_count), TACT1-P C6, 2026-09-04) -- the original returns nothing and mutates *scratch_count
// in place.
void group_move_register_member(int32_t player, int32_t unit_idx, int32_t *scratch_count);

namespace detail {
} // namespace detail

} // namespace mh::sim
