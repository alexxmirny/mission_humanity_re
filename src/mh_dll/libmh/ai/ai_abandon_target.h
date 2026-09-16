//
// ai/ai_abandon_target.h -- should an order-pending unit give up its committed target and let the
// caller assign a fresh one? (RI-AI / AI1A, antichain layer 2).
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game and no rig. The wrapper below is this applied to state(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_unit_should_abandon_target @0x004ec84d.
//
// PURE PREDICATE -- no ai_store parameter because nothing here writes: the site has NO measured
// writable region (mh_shadow.gen.h), so the entire differential verdict rides on the return value.
//
// Returns true iff `unit_index` (owned by `player`) should abandon its currently committed
// PRIMARY target (units[player][unit_index].target_index/target_ref -- ONLY the primary column;
// target2_*, passive_engage_target_*, and ai_contact_* never appear anywhere in this function's
// assembly, verified against every offset used) and let the caller issue a fresh order via
// llm_strat_unit_issue_default_order:
//
//   1. TRUE immediately if the current target is already dead (gc.unit_attack_target_is_dead).
//   2. FALSE immediately if the unit's primary target (target_index, target_ref) matches a
//      still-tracked entry of player_data[player].ai_target_list (entry.aggressor_index ==
//      target_index && entry.aggressor_ref == target_ref) -- already engaging something the AI
//      still wants it on, full stop, before any of the scan below runs.
//   3. Otherwise scans every ai_target_list entry that (a) was recorded for this unit
//      (entry.victim_index == unit_index) and (b) is flagged live (entry.victim_ref & 0xa0), and for
//      each one this unit can actually shoot at (gc.target_ref_has_engageable_weapon), tallies
//      across the WHOLE list (not just the current target):
//        - whether ANY qualifying candidate is within this unit's own max weapon range
//          (in_range_candidate_found, incremented -- only ever tested != 0);
//        - whether any qualifying candidate is a UNIT (candidate_flags & 0x1);
//        - whether any qualifying candidate is a TURRET building (BLDG_TYPE_A_TURRET /
//          BLDG_TYPE_H_TURRET, candidate_flags & 0x4) -- checked UNCONDITIONALLY on the building
//          branch, even when that candidate itself is out of range.
//   4. Then, only if step 3 found at least one qualifying candidate (candidate_flags != 0),
//      decides against the CURRENT target: abandon if it is now out of this unit's own weapon
//      range, OR if the current target is a building and either it is not a turret, or it is a
//      turret but no turret candidate was seen in the scan while a unit candidate was AND
//      something was in range.
//
// THE PACKED-REF ROSTER TEST HERE IS ref_is_building_by_a0 (`(ref & 0xa0) == 0` -> building)
// EVERYWHERE in this function -- both for candidates in the scan and for the current target. The
// OTHER polarity (ref_is_building_by_40) never appears here.
bool unit_should_abandon_target(const ai_view &v, const ai_calls &gc, uint32_t player,
                                int32_t unit_index);

} // namespace detail

bool unit_should_abandon_target(uint32_t player, int32_t unit_index);

} // namespace mh::ai
