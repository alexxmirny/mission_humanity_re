#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {
namespace detail {

// llm_strat_unit_path_queue_count @0x0041fea7. See the header banner above for the full derivation of
// both exits' return values -- Exit A (loop exhausted `max_len`) returns `max_len`; Exit B (hit a
// `.run_length == 0` slot at index `i`) returns `2 * (owner*PATH_WAYPOINTS_PER_PLAYER +
// unit_index*PATH_WAYPOINTS_PER_SLOT + i)`, NOT `i` -- preserved exactly, not "fixed".
int32_t unit_path_queue_count(const sim_view &v, int32_t unit_index, int32_t max_len);

} // namespace detail

// Live wrapper: the logic applied to state().read. Matches the committed prototype
// (`int __watcall llm_strat_unit_path_queue_count(int unit_index, int max_len)`) exactly.
int32_t unit_path_queue_count(int32_t unit_index, int32_t max_len);

namespace detail {
} // namespace detail

} // namespace mh::sim
