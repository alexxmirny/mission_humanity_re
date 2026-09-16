//
// ai/ai_active_unit_tick.h -- the AI combat/engagement tick for one player (RI-AI batch E).
//
// llm_strat_ai_active_unit_tick @0x004ee30b. Counterpart of llm_strat_unit_passive_engage_tick, but
// AI-only. Per ai_groups[0..ai_group_count) of `player`: refreshes/seeds that group's scan-target
// list (via one of three producer sequences selected by group index / goal), then walks the group's
// unit roster -- idle units become attack candidates, order-pending units are checked for target
// abandonment -- sorts the scan-target list and, per (candidate, target) pair, scores by toroidal
// distance vs weapon range, the turret-threat grid, and AA/ground weapon matching, then commits the
// best-scoring pair as an attack order.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game and no rig. The wrapper below is this applied to state(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_active_unit_tick @0x004ee30b.
//
// ENTRY: player_data[player].ai_player_relation[player] = FOREIGN_BLDG_CHANGE_FLAG ? -1 : 1 -- the
// same four instructions as llm_strat_ai_player_tick's tail (translated independently here, not
// factored into a shared helper -- brief rule 4, and see the batch context's note that the two are
// bit-for-bit identical).
//
// PER-GROUP DISPATCH (the outer loop over player_data[player].ai_groups[0..ai_group_count)):
//   group 0 (the holding pen) always scans (holding_pen_scan_targets / target_list_invalidate_by_id
//   (id 0) / target_list_refresh_mothers / target_list_scan_visible_enemies);
//   groups with goal in {3, 8, 10, 0xb} scan via holding_pen_scan_targets / invalidate /
//   group_seed_resolved_target;
//   groups with goal == 7 scan via holding_pen_scan_targets / invalidate(self) /
//   invalidate(link_target_group);
//   EVERY OTHER GOAL VALUE FALLS THROUGH WITHOUT RUNNING THE SHARED BODY AT ALL -- confirmed at
//   0x004ecc35 (JNZ straight to the per-group loop increment). _G_LLM_STRAT_AI_SCAN_TARGET_COUNT is
//   reset to 0 for every group unconditionally, before this dispatch runs.
//
// SHARED BODY (only for groups that scanned): resets the attack-candidate scratch, then walks the
// group's unit roster as an intrusive singly-linked list (head_unit, then units[].ai_group_next
// until 0). For each member with energy > 0.0 (a DOUBLE compare -- and the player index feeding that
// ONE read is masked `& 0xf`, confirmed real in the assembly at 0x004ecc94, not a decompiler
// artifact; every OTHER read in this function, including the ai_group_next walk itself, uses the
// full unmasked player index): an idle unit (unit_is_order_pending == 0) is appended to the
// attack-candidate scratch; an order-pending unit is checked via the ALREADY-TRANSLATED
// unit_should_abandon_target (ai_abandon_target.h/.cpp, called directly rather than through
// `gc` -- see the .cpp) and, if it should abandon, is handed llm_strat_unit_issue_default_order
// (an ORIGINAL engine function, unrelated to the AI's own default-order helper of the same short
// name used elsewhere in this cluster).
//
// If any scan targets were produced, they are sorted (scan_target_list_sort) and then, while both
// the scan-target and attack-candidate scratches are nonempty:
//   - group 0's holding-pen targets additionally get a one-shot "turret threat window" scan (a
//     disc of the target's own defense-weapon range, walked via the shared spiral table) that OR's
//     class_flags |= 0x100 when no cell in the disc reads a threat-grid value in [1, 5];
//   - every attack candidate is scored against the current target (see the .cpp for the exact
//     UNSIGNED-vs-SIGNED reading of each comparison, taken off the JCC, not guessed);
//   - the best-scoring, weapon-eligible candidate is picked (best_score seeded -0x7fffffff, NOT
//     INT32_MIN; best_index seeded 0xffffffff; score must also be >= 0) and, if one was found,
//     committed via commit_attack_order + group_enter_hold, then swap-removed from the candidate
//     scratch (memcpy(&candidates[best], &candidates[count-1], 0x10); --count -- Ghidra folds the
//     -1 into the base address, see the .cpp);
//   - unless this target was already flagged (class_flags & 0x80), an incoming-damage tail compares
//     the target's (building or unit, selected by target_ref) incoming-damage tally against its own
//     energy truncated toward zero (x87, inlined -- see the .cpp), and on tally >= trunc(energy)
//     decrements priority_score and sets class_flags |= 0x80 so the tail runs at most once per
//     target;
//   - a target with no eligible candidate is dropped (scan_target_count--, advance to the next
//     target); a target that WAS committed against keeps its place and is re-scored on the next
//     iteration against whatever candidates remain.
//
// Loop exit (ai_group_count <= group index): player_data[player].ai_target_list_count = 0. This is
// the ONLY exit path in the function.
void active_unit_tick(const ai_view &v, const ai_store &own, const ai_calls &gc, int32_t player);

} // namespace detail

void active_unit_tick(int32_t player);

} // namespace mh::ai
