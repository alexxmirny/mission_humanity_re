//
// ai/ai_notify_bldg_constructed.h -- the construction-completed AI notification hook (RI-AI batch
// E, 2026-08-07).
//
// Counterpart to llm_strat_ai_notify_object_removed. Called from llm_strat_bldg_state_construction
// (real construction finished) and an unnamed placement helper (llm_bldg_construct_finalize), with
// `mode` selecting which of three unrelated jobs it does -- the original dispatches on it as a
// 0/1/2 switch, not as a bitmask or an enum with a symbolic name anywhere in the image:
//
//   mode 0: "a building just finished appearing" -- flags every OTHER active player's
//           ai_turret_rescan_pending when the new building is an enemy turret, then either
//           dispatches notify_map_changed directly (inactive AI or the player's own mother
//           building) or reconciles the pending ai_bldg_queue entry that predicted this building
//           (matches on {status==0x81, tick_or_unit_id==building_id, build_tile_x==x,
//           resource_reserved[0]==y, building_index==-1} and stamps building_index = order_id),
//           then separately resolves a matching resource-site's status when the building is a mine.
//   mode 1: "confirm a queue entry already linked to this building_index" -- OR's 0x40 into the
//           matching entry's status and, only when the building is the player's cached
//           ai_build_candidate_primary, issues order_population_delta_enqueue(worker_count).
//   mode 2: "the placement failed / is being retired" -- OR's 0x40 into a matching UNRESOLVED
//           (building_index==-1) entry and refunds its four reserved resource costs into
//           resource_spent[0..3], then unconditionally calls notify_map_changed_2.
//   mode >2: no-op (the original's own default arm of the dispatch).
//
// THREE DIFFERENT QUEUE-SCAN PREDICATES are in play across the three modes and they are NOT the
// same test: mode 0 and mode 2 both match building_index==-1 (an unresolved entry), mode 1 matches
// building_index==order_id (already resolved to THIS commit). Do not unify them.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// llm_strat_ai_notify_bldg_constructed @0x004daec2.
//
// player: the OWNING player of the newly-constructed building. x, y: its build tile. building_id:
// a cfg Building[] TYPE index (not a roster slot -- see the mode-0 turret/mine type tests and the
// worker_count/order_population_delta_enqueue call, both indexed by it directly). order_id: the
// value stamped into a matching queue entry's building_index (mode 0) or compared against it
// (mode 1); it is signed and also truncated into a resource_site's int16_t `status` (mode 0's mine
// arm) -- read straight off the assembly's `int param_3`, not the exported plate's `undefined4`.
// mode: the 0/1/2/other dispatch selector described above.
void notify_bldg_constructed(const ai_view &v, const ai_store &own, const ai_calls &gc,
                             uint32_t player, uint32_t x, int32_t order_id, uint32_t building_id,
                             uint32_t y, uint32_t mode);

} // namespace detail

void notify_bldg_constructed(uint32_t player, uint32_t x, uint32_t order_id, uint32_t building_id,
                             uint32_t y, uint32_t mode);

} // namespace mh::ai
