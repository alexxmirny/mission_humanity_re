#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The outward calls unit_release_path_and_targets makes, indirected like every sim/ TU so detail::
// stays testable under simtest. Both are frontier originals per the batch context's callee table
// (neither is a sim_resid sibling), so they are reached through mh::call:: in
// live_unit_release_path_and_targets_calls(), never called directly. encode_directions has no
// callees at all (per the batch context's callee table) and needs no calls struct.
struct unit_release_path_and_targets_calls {
    void (*path_free_slot)(uint16_t player, int32_t unit_index); // llm_strat_path_free_slot @0x004969e8
    void (*target_release_ref)(uint32_t player_idx, int32_t unit_idx,
                               uint32_t mode); // llm_strat_target_release_ref @0x004dac44
};

const unit_release_path_and_targets_calls &live_unit_release_path_and_targets_calls();

namespace detail {

// llm_strat_unit_path_encode_directions @0x0044955f. Reads `units[player][unit_idx].path_slot_id`
// through `v` (read-only in this function); walks and rewrites `_G_LLM_STRAT_PATH_BUFFERS` through
// `own.path_buffer_at()`. No callees. void return, matching the original.
void unit_path_encode_directions(const sim_view &v, sim_store &own, uint16_t player, int32_t unit_idx);

// llm_strat_unit_release_path_and_targets @0x004888f4. Reads and writes the unit's own record
// (`path_slot_id`/`target_ref`/`target_index`/`target2_ref`/`target2_index`) through `own.unit_at()`,
// reaches its two callees through `c`, and unconditionally zeroes the caller-supplied
// `out_cleared_pair[0..1]`. void return, matching the original.
void unit_release_path_and_targets(sim_store &own, const unit_release_path_and_targets_calls &c,
                                   uint16_t player, int32_t unit_idx, uint32_t *out_cleared_pair);

} // namespace detail

// Live wrappers: the logic applied to state() (and live_unit_release_path_and_targets_calls() for the
// second one).
void unit_path_encode_directions(uint16_t player, int32_t unit_idx);
void unit_release_path_and_targets(uint16_t player, int32_t unit_idx, uint32_t *out_cleared_pair);

} // namespace mh::sim
