//
// ai/ai_engage_scan.h -- the engage-candidate scan/rank helpers (RI-AI / AI0, AI1A batch A layer 1).
//
// Three more links in the same pipeline ai_engage.{h,cpp} documents: where that file's two
// consumers FILTER and PICK from the shared engage scratch list, these three are what FILLS and
// ORDERS it before a consumer ever runs.
//   unit_scan_engage_candidates_in_range -- PRODUCER: works out a unit's effective engage radius
//                                           (the larger, signed, of its sight and its max weapon
//                                           range) and asks the spiral-ring scanner to append every
//                                           candidate within that radius of the unit's (x, y).
//   engage_sort_candidates_by_dist       -- fills every scratch entry's dist_sq from a source
//                                           entity, then -- CONDITIONALLY, see the definition --
//                                           bubble-sorts the scratch ascending by that distance.
//   engage_partition_turret_candidates   -- stable-ish in-place swap-partition moving every
//                                           turret-building candidate to the front of the scratch.
//
// Same scratch, same "reset-then-fill transient, no persistent state, no cap check" discipline as
// ai_engage.h documents; not repeated here.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game and no rig. The wrappers below are these applied to state(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_unit_scan_engage_candidates_in_range @0x004ed78d.
// radius = max(gc.unit_get_sight(player, unit_id), gc.unit_max_weapon_range(player, unit_id)),
// SIGNED compare (the original's JLE) -- then gc.scan_spiral_ring_for_engage_candidates(player,
// unit.x, unit.y, radius, target_mask). `player` is passed to the two range queries as-is: at this
// call site it is a plain 0..7 player index, not a packed class|owner ref, even though the ai_calls
// parameter is named `unit_ref` for the (rarer) call sites that do pass one.
//
// RETURNS the scan's own result verbatim (`return iVar2;` in the original, 0x004ed7e9) -- the
// committed prototype was corrected void -> int32_t 2026-08-05/06 (three of its four call sites
// consume the value, e.g. llm_strat_ai_group_dispatch_action_flags's `iVar2 = ...(...)`); this
// translation simply never propagated it. Fixed 2026-08-06 while closing an unrelated AI1C build.
int32_t unit_scan_engage_candidates_in_range(const ai_view &v, const ai_calls &gc, int32_t player,
                                             int32_t unit_id, uint32_t target_mask);

// llm_strat_ai_engage_sort_candidates_by_dist @0x004d53ae.
//
// PHASE 1 (always runs): resolve the source entity's (x, y) -- from `source_ref`/`source_index`,
// using the (ref & 0xa0) == 0 -> BUILDING test (ref_is_building_by_a0) -- then fill every scratch
// entry's dist_sq via gc.toroidal_dist_sq(source_x, source_y, target_x, target_y), resolving each
// entry's target the same way over ITS OWN target_ref/target_index.
//
// PHASE 2 (conditionally runs): a bubble sort of the scratch ascending by dist_sq. Gated by
// `inherited_sorted_flag`, which is NOT part of the original's signature -- see the definition for
// why it exists and what each value means. This is a translated ORIGINAL BUG, preserved on purpose,
// not a design choice: do not "fix" it by always sorting.
void engage_sort_candidates_by_dist(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                    uint32_t source_ref, int32_t source_index,
                                    int32_t inherited_sorted_flag);

// llm_strat_ai_engage_partition_turret_candidates @0x004d552b.
// In-place swap-partition of the engage scratch: every candidate whose target is an
// A_TURRET/H_TURRET building (via cfg_buildings[buildings[owner][target_index].building_id].type)
// moves to the front, in place, stable-ish. Uses the (ref & 0x40) != 0 -> BUILDING test
// (ref_is_building_by_40) -- the OTHER live polarity, not the 0xa0 one every other consumer in this
// file uses. Skips the swap entirely when the write cursor has already caught up to the read
// cursor.
void engage_partition_turret_candidates(const ai_view &v, const ai_store &own);

} // namespace detail

int32_t unit_scan_engage_candidates_in_range(int32_t player, int32_t unit_id, uint32_t target_mask);
void    engage_partition_turret_candidates();

// NOTE THE EXTRA PARAMETER, and that it has NO DEFAULT. This wrapper cannot supply the original's
// ambient ESI, and the reviewers (2026-08-01) flagged the earlier version -- which passed 0, i.e.
// "always sort" -- as a latent divergence waiting for its first caller: at any real call site whose
// live ESI is non-zero the original does NOT sort. It is harmless today only because nothing calls
// it (production reaches the ORIGINAL through mh::call::, and there is deliberately no shadow arm).
// Rather than leave a wrong default behind a comment, the decision is forced onto whoever wires this
// up: prove what the call site leaves in ESI, then pass it. See the definition in ai_engage_scan.cpp.
void engage_sort_candidates_by_dist(uint32_t source_ref, int32_t source_index,
                                    int32_t inherited_sorted_flag);

} // namespace mh::ai
