//
// ai/ai_worker_rebalance.h -- the per-player worker/builder labor rebalance (RI-AI / AI1B layer 3).
//
// One function: llm_strat_ai_rebalance_building_workers @0x004e3bc2. Splits the player's live
// buildings into "locked" (mid-construction/upgrade/dismantle, consuming builder_count workers from
// cfg) and "worker demand" (staffed producer buildings priority-eligible for workers, consuming
// worker_count workers), derives an idle labor reserve from the population ledger, divides to get a
// labor-utilisation ratio, clamps it into [0, 1] using the ORIGINAL's own x87 branch senses (see the
// .cpp for why that is not the same as the equivalent C++ comparisons), stores it into
// player_data::ai_labor_utilization, and issues activate / assign-workers / unassign-workers orders
// per building on a second roster pass. The return value is a DECISION ("housing is short"), not a
// status code.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrapper below is this applied to state(); the split costs one inlined call.
namespace detail {

// llm_strat_ai_rebalance_building_workers @0x004e3bc2.
int32_t rebalance_building_workers(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                   uint32_t player);

} // namespace detail

int32_t rebalance_building_workers(uint32_t player);

} // namespace mh::ai
