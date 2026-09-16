#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {
namespace detail {

// llm_strat_bldg_uses_workers @0x004988b0. See the header banner above for the full derivation.
int32_t bldg_uses_workers(const sim_view &v, uint32_t player, int32_t building_index);

} // namespace detail

// Live wrapper: the logic applied to state().read. Matches the original's committed __watcall(EAX,EDX)
// shape (sig_llm_strat_bldg_uses_workers) -- the SAME signature already declared by this function's
// existing callers (sim_bldg_add_workers.h's `add_workers_calls::uses_workers`,
// sim_bldg_finish_order.h's `bldg_finish_order_calls::bldg_uses_workers`), and by the two sibling
// units translated alongside this one in this batch (sim_refresh_building, sim_bldg_remove_workers).
int32_t bldg_uses_workers(uint32_t player, int32_t building_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
