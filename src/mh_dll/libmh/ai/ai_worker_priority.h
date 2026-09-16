//
// ai/ai_worker_priority.h -- is a production building's worker demand exempt from normal worker-
// count aggregation because it is a cached resource-shortage build candidate? (RI-AI / AI1A, batch A
// layer 4).
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game and no rig. The wrapper below is this applied to state(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_is_worker_priority_candidate @0x004e3a77.
//
// PURE PREDICATE -- no ai_store, no ai_calls: nothing here writes and nothing here calls out
// (mh_shadow.gen.h records NO measured writable region for this site), so the entire differential
// verdict rides on the return value.
//
// Reads a BUILDING-TYPE id (buildings[player][site_index].building_id, a ushort -- NOT a
// unit_proto_id; that was an earlier, wrong hypothesis recorded in the plate) and tests it against
// player_data[player].ai_resource_shortage_candidates[0..3]:
//
//   * ON A MATCH of any of the four: look at this building's own production record
//     (production_of(v, player, buildings[player][site_index].sub_id) -- indexed by the building's
//     SLOT, not its roster index). Return true if that production is actively building a unit
//     (.active_unit_type != 0); otherwise scan .queued_count[1 .. cfg_unit_sec->total] INCLUSIVE
//     (an unsigned bound test in the original -- CMP/JBE) for the first nonzero entry and return
//     true on it; return false if the whole range is zero. Slot 0 of queued_count is deliberately
//     never inspected.
//   * ON NO MATCH: the polarity INVERTS. Compare the same building-type id against a FIFTH, separate
//     field, player_data[player].ai_build_candidate_cat_0x30, and return true when they DIFFER,
//     false when they are equal. This is the opposite sense from the four-way match above it --
//     read straight off LAB_004e3bb0 in the disassembly, not inferred from symmetry.
//
// Neither the four-way match, the production/queue scan, nor the sub_id used to index `productions`
// is bounds-checked here or in the original.
bool is_worker_priority_candidate(const ai_view &v, int32_t player, int32_t site_index);

} // namespace detail

bool is_worker_priority_candidate(int32_t player, int32_t site_index);

} // namespace mh::ai
